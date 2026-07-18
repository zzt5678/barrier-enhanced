/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2012 Nick Bolton
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "barrier/win32/DaemonApp.h"

#include "barrier/App.h"
#include "barrier/ArgParser.h"
#include "barrier/ServerArgs.h"
#include "barrier/ClientArgs.h"
#include "barrier/ServiceLaunchState.h"
#include "barrier/win32/MSWindowsServiceDataDirectory.h"
#include "barrier/win32/MSWindowsServiceLaunchProfile.h"
#include "ipc/IpcClientProxy.h"
#include "ipc/IpcCommandValidator.h"
#include "ipc/ElevationPolicy.h"
#include "ipc/IpcMessage.h"
#include "ipc/IpcLogOutputter.h"
#include "net/SocketMultiplexer.h"
#include "arch/XArch.h"
#include "base/Log.h"
#include "base/TMethodEventJob.h"
#include "base/EventQueue.h"
#include "base/log_outputters.h"
#include "base/Log.h"
#include "common/ProductIdentity.h"

#include "arch/win32/ArchMiscWindows.h"
#include "arch/win32/XArchWindows.h"
#include "barrier/Screen.h"
#include "platform/MSWindowsScreen.h"
#include "platform/MSWindowsDebugOutputter.h"
#include "platform/MSWindowsWatchdog.h"
#include "platform/MSWindowsEventQueueBuffer.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Sddl.h>
#include <Wtsapi32.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <stdexcept>
#include <vector>

using namespace std;

DaemonApp* DaemonApp::s_instance = NULL;

namespace {

const char kServiceLaunchStateSetting[] = "ServiceLaunchStateV1";
const char kServiceLaunchPendingSetting[] = "ServiceLaunchPendingV1";
const int kServiceLaunchCommitDispatchSeconds = 5;

enum class ServiceLaunchCommitState {
    kQueued,
    kStarted,
    kDone,
    kCancelled
};

struct ServiceLaunchCommitCompletion {
    std::mutex mutex;
    std::condition_variable condition;
    ServiceLaunchCommitState state = ServiceLaunchCommitState::kQueued;
    ServiceLaunchCommitResult result = ServiceLaunchCommitResult::kRejected;
};

class ServiceLaunchReadyEventData : public EventData {
public:
    explicit ServiceLaunchReadyEventData(
        const ServiceLaunchCandidate& candidate,
        const std::shared_ptr<ServiceLaunchCommitCompletion>& completion) :
        candidate(candidate),
        completion(completion)
    {
    }

    ServiceLaunchCandidate candidate;
    std::shared_ptr<ServiceLaunchCommitCompletion> completion;
};

class ServiceStopConfirmedEventData : public EventData {
public:
    explicit ServiceStopConfirmedEventData(
        unsigned long long commandGeneration) :
        commandGeneration(commandGeneration)
    {
    }

    unsigned long long commandGeneration;
};

class ScopedWindowsHandle {
public:
    explicit ScopedWindowsHandle(HANDLE handle = nullptr) :
        m_handle(handle)
    {
    }

    ~ScopedWindowsHandle()
    {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }
    }

    HANDLE get() const { return m_handle; }

private:
    ScopedWindowsHandle(const ScopedWindowsHandle&);
    ScopedWindowsHandle& operator=(const ScopedWindowsHandle&);

    HANDLE m_handle;
};

bool tokenUserSid(HANDLE token, std::string& sid)
{
    sid.clear();
    if (token == nullptr || token == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return false;
    }
    std::vector<unsigned char> buffer(size);
    DWORD returned = 0;
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &returned) ||
        returned < sizeof(TOKEN_USER)) {
        return false;
    }

    PSID userSid = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
    if (!IsValidSid(userSid)) {
        return false;
    }
    LPSTR rawSid = nullptr;
    if (!ConvertSidToStringSidA(userSid, &rawSid) || rawSid == nullptr) {
        return false;
    }
    sid.assign(rawSid);
    LocalFree(rawSid);
    return true;
}

bool acquireCurrentCommandOriginToken(const CommandOrigin& origin,
                                      HANDLE& token,
                                      std::string& reason)
{
    token = nullptr;
    reason.clear();
    if (!origin.kernelVerified()) {
        reason = "origin was not kernel authenticated";
        return false;
    }

    const DWORD activeSessionId = WTSGetActiveConsoleSessionId();
    if (activeSessionId == 0xffffffffu ||
        origin.sessionId() != activeSessionId) {
        reason = "origin is no longer in the active console session";
        return false;
    }

    LPWSTR rawState = nullptr;
    DWORD stateBytes = 0;
    if (!WTSQuerySessionInformationW(
            WTS_CURRENT_SERVER_HANDLE, activeSessionId, WTSConnectState,
            &rawState, &stateBytes) || rawState == nullptr ||
        stateBytes < sizeof(WTS_CONNECTSTATE_CLASS)) {
        if (rawState != nullptr) {
            WTSFreeMemory(rawState);
        }
        reason = "could not verify the origin session state";
        return false;
    }
    const WTS_CONNECTSTATE_CLASS state =
        *reinterpret_cast<const WTS_CONNECTSTATE_CLASS*>(rawState);
    WTSFreeMemory(rawState);
    if (state != WTSActive) {
        reason = "origin session is no longer active";
        return false;
    }

    HANDLE rawToken = nullptr;
    if (!WTSQueryUserToken(activeSessionId, &rawToken)) {
        reason = "could not open the active origin token";
        return false;
    }
    std::string actualSid;
    if (!tokenUserSid(rawToken, actualSid) || actualSid != origin.userSid()) {
        CloseHandle(rawToken);
        reason = "origin user no longer owns the active console session";
        return false;
    }

    token = rawToken;
    return true;
}

bool narrowProfilePath(const std::wstring& path, std::string& narrow)
{
    if (path.empty()) {
        return false;
    }
    BOOL usedDefault = FALSE;
    const int required = WideCharToMultiByte(
        CP_ACP, WC_NO_BEST_FIT_CHARS, path.c_str(), -1,
        nullptr, 0, nullptr, &usedDefault);
    if (required <= 1 || usedDefault) {
        return false;
    }
    std::vector<char> buffer(static_cast<std::size_t>(required), '\0');
    usedDefault = FALSE;
    if (WideCharToMultiByte(
            CP_ACP, WC_NO_BEST_FIT_CHARS, path.c_str(), -1,
            buffer.data(), required, nullptr, &usedDefault) != required ||
        usedDefault) {
        return false;
    }
    narrow.assign(buffer.data(), static_cast<std::size_t>(required - 1));
    return true;
}

ServiceLaunchRole launchRole(const std::string& command)
{
    return IpcCommandValidator::isServerCommand(command)
        ? ServiceLaunchRole::kServer : ServiceLaunchRole::kClient;
}

bool makeWatchdogLaunchProfile(const ServiceLaunchProfileResult& result,
                               UInt32 sessionId,
                               MSWindowsWatchdog::LaunchProfile& profile,
                               std::string& reason)
{
    std::string profileDirectory;
    if (!result.success() || sessionId == 0 || result.ownerSid.empty() ||
        result.generation.empty() || result.digest.empty() ||
        !narrowProfilePath(result.profilePath, profileDirectory)) {
        reason = result.detail.empty()
            ? "service launch profile metadata is incomplete"
            : result.detail;
        return false;
    }

    profile.authenticated = true;
    profile.sessionId = sessionId;
    profile.userSid = result.ownerSid;
    profile.profileDirectory = profileDirectory;
    profile.generation = result.generation;
    profile.digest = result.digest;
    return true;
}

void discardStagedProfileWithLog(
    const ServiceLaunchProfileResult& stagedProfile,
    const char* context)
{
    if (!stagedProfile.newlyStaged) {
        return;
    }
    if (!discardWindowsServiceLaunchProfile(stagedProfile)) {
        LOG((CLOG_ERR
            "could not discard uncommitted service launch profile generation=%s context=%s",
            stagedProfile.generation.c_str(),
            context == nullptr ? "unspecified" : context));
    }
}

} // namespace

static UInt8
readElevateModeSetting()
{
    return ElevationPolicy::modeFromSettings(
        ARCH->setting("ElevateMode"),
        ARCH->setting("Elevate"));
}

static std::string
normalizedWindowsPath(std::string path)
{
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) {
        if (ch == '/') {
            return '\\';
        }
        return static_cast<char>(std::tolower(ch));
    });
    while (path.size() > 3 && path.back() == '\\') {
        path.pop_back();
    }
    return path;
}

static std::string
environmentPath(const char* name)
{
    const DWORD required = GetEnvironmentVariableA(name, nullptr, 0);
    if (required == 0) {
        return std::string();
    }

    std::vector<char> buffer(required);
    const DWORD written = GetEnvironmentVariableA(name, buffer.data(), required);
    if (written == 0 || written >= required) {
        return std::string();
    }
    return std::string(buffer.data(), written);
}

static bool
isBelowDirectory(const std::string& path, const std::string& directory)
{
    const std::string normalizedPath = normalizedWindowsPath(path);
    const std::string normalizedDirectory = normalizedWindowsPath(directory);
    return !normalizedDirectory.empty() &&
           normalizedPath.size() > normalizedDirectory.size() &&
           normalizedPath.compare(0, normalizedDirectory.size(), normalizedDirectory) == 0 &&
           normalizedPath[normalizedDirectory.size()] == '\\';
}

static void
ensureProtectedServiceDirectory(const std::string& path)
{
    const ServiceDataDirectoryResult result =
        ensureProtectedServiceDataDirectory(path);
    if (!result.success()) {
        std::ostringstream message;
        message << "unable to secure the Weave service data directory"
                << " (stage=" << static_cast<int>(result.error)
                << ", error=" << result.systemError << ")";
        throw std::runtime_error(message.str());
    }
}

static std::string
protectedServiceLogFilename()
{
    const std::string programData = environmentPath("ProgramData");
    if (programData.empty()) {
        throw std::runtime_error("ProgramData is unavailable to the Weave service");
    }

    const std::string serviceRoot = programData + "\\Weave";
    const std::string logDirectory = serviceRoot + "\\Logs";
    ensureProtectedServiceDirectory(serviceRoot);
    ensureProtectedServiceDirectory(logDirectory);

    const std::string logFile = logDirectory + "\\" LOG_FILENAME;
    const DWORD attributes = GetFileAttributesA(logFile.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)) {
        throw std::runtime_error(
            "refusing an unsafe Weave service log file");
    }
    return logFile;
}

static bool
isProtectedProgramFilesPath(const std::string& path)
{
    static const char* kRoots[] = {
        "ProgramFiles",
        "ProgramW6432",
        "ProgramFiles(x86)"
    };
    for (const char* rootName : kRoots) {
        if (isBelowDirectory(path, environmentPath(rootName))) {
            return true;
        }
    }
    return false;
}

int
mainLoopStatic()
{
    DaemonApp::s_instance->mainLoop(true);
    return kExitSuccess;
}

int
mainLoopStatic(int, const char**)
{
    return ArchMiscWindows::runDaemon(mainLoopStatic);
}

DaemonApp::DaemonApp() :
    m_ipcServer(nullptr),
    m_ipcLogOutputter(nullptr),
    m_watchdog(nullptr),
    m_events(nullptr),
    m_fileLogOutputter(nullptr),
    m_daemonized(false),
    m_launchReadyEvent(Event::kUnknown),
    m_stopConfirmedEvent(Event::kUnknown),
    m_launchRevision(0),
    m_acceptLaunchCommits(false),
    m_acceptStopConfirmations(false)
{
    s_instance = this;
}

DaemonApp::~DaemonApp()
{
}

int
DaemonApp::run(int argc, char** argv)
{
    // win32 instance needed for threading, etc.
    ArchMiscWindows::setInstanceWin32(GetModuleHandle(NULL));

    Arch arch;
    arch.init();

    Log log;
    EventQueue events;
    m_events = &events;

    bool uninstall = false;
    try
    {
        // sends debug messages to visual studio console window.
        log.insert(new MSWindowsDebugOutputter());

        // default log level to system setting.
        string logLevel = arch.setting("LogLevel");
        if (logLevel != "")
            log.setFilter(logLevel.c_str());

        bool foreground = false;

        for (int i = 1; i < argc; ++i) {
            string arg(argv[i]);

            if (arg == "/f" || arg == "-f") {
                foreground = true;
            }
            else if (arg == "/install") {
                uninstall = true;
                arch.installDaemon();
                return kExitSuccess;
            }
            else if (arg == "/uninstall") {
                arch.uninstallDaemon();
                return kExitSuccess;
            }
            else {
                stringstream ss;
                ss << "Unrecognized argument: " << arg;
                foregroundError(ss.str().c_str());
                return kExitArgs;
            }
        }

        if (foreground) {
            // add a console to catch Ctrl+C and run process in foreground
            // instead of daemonizing. useful for debugging.
            if (IsDebuggerPresent())
                AllocConsole();
            mainLoop(false);
        }
        else {
            arch.daemonize(WEAVE_SERVICE_NAME, mainLoopStatic);
        }

        return kExitSuccess;
    }
    catch (XArch& e) {
        String message = e.what();
        if (uninstall && (message.find("The service has not been started") != String::npos)) {
            // TODO: if we're keeping this use error code instead (what is it?!).
            // HACK: this message happens intermittently, not sure where from but
            // it's quite misleading for the user. they thing something has gone
            // horribly wrong, but it's just the service manager reporting a false
            // positive (the service has actually shut down in most cases).
        }
        else {
            foregroundError(message.c_str());
        }
        return kExitFailed;
    }
    catch (std::exception& e) {
        foregroundError(e.what());
        return kExitFailed;
    }
    catch (...) {
        foregroundError("Unrecognized error.");
        return kExitFailed;
    }
}

void
DaemonApp::mainLoop(bool daemonized)
{
    try
    {
        m_daemonized = daemonized;
        DAEMON_RUNNING(true);

        if (daemonized) {
            m_fileLogOutputter = new FileLogOutputter(logFilename().c_str());
            CLOG->insert(m_fileLogOutputter);
        }

        if (daemonized) {
            initializeTrustedExecutables();
        }

        // create socket multiplexer.  this must happen after daemonization
        // on unix because threads evaporate across a fork().
        SocketMultiplexer multiplexer;

        // uses event queue, must be created here.
        m_ipcServer = new IpcServer(m_events, &multiplexer);

        // send logging to gui via ipc, log system adopts outputter.
        m_ipcLogOutputter = new IpcLogOutputter(*m_ipcServer, kIpcClientGui, true);
        CLOG->insert(m_ipcLogOutputter);

        m_watchdog = new MSWindowsWatchdog(daemonized, false, *m_ipcServer, *m_ipcLogOutputter);
        m_watchdog->setFileLogOutputter(m_fileLogOutputter);

        m_events->registerTypeOnce(
            m_launchReadyEvent, "DaemonApp::watchdogLaunchReady");
        m_events->adoptHandler(
            m_launchReadyEvent, this,
            new TMethodEventJob<DaemonApp>(
                this, &DaemonApp::handleWatchdogLaunchReady));
        m_watchdog->setLaunchReadyCallback(
            [this](const ServiceLaunchCandidate& candidate) {
                return commitWatchdogLaunchReady(candidate);
            });
        m_events->registerTypeOnce(
            m_stopConfirmedEvent, "DaemonApp::watchdogStopConfirmed");
        m_events->adoptHandler(
            m_stopConfirmedEvent, this,
            new TMethodEventJob<DaemonApp>(
                this, &DaemonApp::handleWatchdogStopConfirmed));
        m_watchdog->setStopCompletedCallback(
            [this](unsigned long long commandGeneration) {
                notifyWatchdogStopConfirmed(commandGeneration);
            });

        m_events->adoptHandler(
            m_events->forIpcServer().messageReceived(), m_ipcServer,
            new TMethodEventJob<DaemonApp>(this, &DaemonApp::handleIpcMessage));

        m_ipcServer->listen();

        // install the platform event queue to handle service stop events.
        m_events->adoptBuffer(new MSWindowsEventQueueBuffer(m_events));

        ServiceLaunchState persistedState;
        String command;
        UInt8 elevateMode = IpcCommandMessage::kElevateAsNeeded;
        if (daemonized) {
            const std::string encodedState =
                ARCH->setting(kServiceLaunchStateSetting);
            const std::string encodedPending =
                ARCH->setting(kServiceLaunchPendingSetting);
            ServiceLaunchCandidate abandonedCandidate;
            if (parseServiceLaunchCandidate(
                    encodedPending, abandonedCandidate)) {
                m_launchRevision = abandonedCandidate.revision;
            }
            if (!encodedState.empty()) {
                std::string stateError;
                ServiceLaunchRecoveryStatus recoveryStatus =
                    ServiceLaunchRecoveryStatus::kCurrentOnly;
                if (recoverServiceLaunchCurrent(
                        encodedState, encodedPending, persistedState,
                        recoveryStatus, &stateError)) {
                    command = persistedState.command;
                    elevateMode = persistedState.elevateMode;
                    if (recoveryStatus ==
                        ServiceLaunchRecoveryStatus::kDiscardedPending) {
                        LOG((CLOG_WARN
                            "discarding unproven pending service launch during recovery"));
                    }
                    else if (recoveryStatus ==
                        ServiceLaunchRecoveryStatus::kDiscardedMalformedPending) {
                        LOG((CLOG_ERR
                            "discarding malformed pending service launch during recovery: %s",
                            stateError.c_str()));
                    }
                }
                else {
                    LOG((CLOG_ERR
                        "ignoring malformed atomic service launch state: %s",
                        stateError.c_str()));
                    ServiceLaunchState stopped;
                    std::string stoppedState;
                    if (serializeServiceLaunchState(stopped, stoppedState)) {
                        try {
                            ARCH->setting(kServiceLaunchStateSetting,
                                          stoppedState);
                        }
                        catch (const XArch& e) {
                            LOG((CLOG_ERR
                                "could not replace malformed service launch state: %s",
                                e.what()));
                        }
                    }
                }
            }
            if (!encodedPending.empty()) {
                try {
                    ARCH->setting(kServiceLaunchPendingSetting, "");
                }
                catch (const XArch& e) {
                    LOG((CLOG_ERR
                        "could not clear abandoned pending service launch: %s",
                        e.what()));
                }
            }
        }
        else {
            command = ARCH->setting("Command");
            elevateMode = readElevateModeSetting();
        }
        if (command != "") {
            std::string rejectReason;
            const String requestedCommand = command;
            const UInt8 requestedElevateMode = elevateMode;
            if (prepareWatchdogCommand(command, requestedElevateMode,
                                       elevateMode, rejectReason)) {
                if (command != requestedCommand) {
                    LOG((CLOG_WARN "sanitized persisted service command: %s",
                         command.c_str()));
                    persistedState.command = command;
                }
                if (elevateMode != requestedElevateMode) {
                    LOG((CLOG_WARN
                        "downgraded persisted Always elevation to AsNeeded"));
                    persistedState.elevateMode = elevateMode;
                }
                if (!daemonized) {
                    LOG((CLOG_INFO "using last known command: %s", command.c_str()));
                    m_watchdog->setCommand(command, elevateMode);
                }
                else {
                    MSWindowsWatchdog::LaunchProfile launchProfile;
                    std::string profileReason;
                    const ServiceLaunchProfileResult profileResult =
                        loadWindowsServiceLaunchProfile(
                            persistedState.ownerSid,
                            persistedState.generation,
                            persistedState.digest,
                            launchRole(command));
                    if (makeWatchdogLaunchProfile(
                            profileResult, persistedState.sessionId,
                            launchProfile, profileReason)) {
                        LOG((CLOG_INFO
                            "using last authenticated command profile generation=%s",
                            launchProfile.generation.c_str()));
                        m_watchdog->setCommand(
                            command, elevateMode, launchProfile);

                    }
                    else {
                        LOG((CLOG_ERR
                            "ignoring persisted service command without a valid launch profile: %s",
                            profileReason.empty()
                                ? "invalid persisted identity metadata"
                                : profileReason.c_str()));
                        ServiceLaunchState stopped;
                        std::string encodedState;
                        if (serializeServiceLaunchState(stopped, encodedState)) {
                            try {
                                ARCH->setting(kServiceLaunchStateSetting,
                                              encodedState);
                            }
                            catch (const XArch& e) {
                                LOG((CLOG_ERR
                                    "could not clear invalid atomic service launch state: %s",
                                    e.what()));
                            }
                        }
                    }
                }
            }
            else {
                LOG((CLOG_ERR "ignoring invalid last known command: %s", rejectReason.c_str()));
                try {
                    if (daemonized) {
                        ServiceLaunchState stopped;
                        std::string encodedState;
                        if (serializeServiceLaunchState(stopped, encodedState)) {
                            ARCH->setting(kServiceLaunchStateSetting,
                                          encodedState);
                        }
                    }
                    else {
                        ARCH->setting("Command", "");
                    }
                }
                catch (const XArch& e) {
                    LOG((CLOG_ERR
                        "could not clear the rejected persisted command: %s",
                        e.what()));
                }
            }
        }

        m_acceptLaunchCommits.store(true);
        m_acceptStopConfirmations.store(true);
        m_watchdog->startAsync();

        m_events->loop();

        m_acceptLaunchCommits.store(false);
        m_acceptStopConfirmations.store(false);
        m_watchdog->setLaunchReadyCallback(
            MSWindowsWatchdog::LaunchReadyCallback());
        m_watchdog->setStopCompletedCallback(
            MSWindowsWatchdog::StopCompletedCallback());
        m_watchdog->stop();
        delete m_watchdog;
        m_watchdog = nullptr;

        m_events->removeHandler(m_launchReadyEvent, this);
        m_events->removeHandler(m_stopConfirmedEvent, this);

        m_events->removeHandler(
            m_events->forIpcServer().messageReceived(), m_ipcServer);

        CLOG->remove(m_ipcLogOutputter);
        delete m_ipcLogOutputter;
        delete m_ipcServer;

        DAEMON_RUNNING(false);
    }
    catch (std::exception& e) {
        LOG((CLOG_CRIT "An error occurred: %s", e.what()));
    }
    catch (...) {
        LOG((CLOG_CRIT "An unknown error occurred.\n"));
    }

    if (m_watchdog != nullptr) {
        // Stop accepting cross-thread commits before the watchdog can outlive
        // the event loop that serializes registry promotion.
        m_acceptLaunchCommits.store(false);
        m_acceptStopConfirmations.store(false);
        m_watchdog->setLaunchReadyCallback(
            MSWindowsWatchdog::LaunchReadyCallback());
        m_watchdog->setStopCompletedCallback(
            MSWindowsWatchdog::StopCompletedCallback());
        delete m_watchdog;
        m_watchdog = nullptr;
    }
    DAEMON_RUNNING(false);
}

void
DaemonApp::initializeTrustedExecutables()
{
    std::vector<char> modulePath(32768);
    const DWORD length = GetModuleFileNameA(nullptr, modulePath.data(),
                                            static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) {
        throw std::runtime_error("unable to resolve the daemon executable path");
    }

    const std::string daemonPath(modulePath.data(), length);
    if (!isProtectedProgramFilesPath(daemonPath)) {
        throw std::runtime_error(
            "refusing to run the SYSTEM service from outside Program Files");
    }

    const std::string::size_type separator = daemonPath.find_last_of("/\\");
    if (separator == std::string::npos) {
        throw std::runtime_error("daemon executable has no parent directory");
    }

    const std::string installDirectory = daemonPath.substr(0, separator + 1);
    m_trustedServerExecutable = installDirectory + "weaves.exe";
    m_trustedClientExecutable = installDirectory + "weavec.exe";
    LOG((CLOG_INFO "protected runtime directory: %s", installDirectory.c_str()));
}

bool
DaemonApp::prepareWatchdogCommand(std::string& command,
                                  UInt8 requestedElevateMode,
                                  UInt8& sanitizedElevateMode,
                                  std::string& reason) const
{
    if (!m_daemonized) {
        sanitizedElevateMode = ElevationPolicy::normalizeMode(
            requestedElevateMode);
        return IpcCommandValidator::isAllowedDaemonCommand(command, &reason);
    }

    IpcCommandValidator::SanitizedDaemonRequest sanitized;
    if (!IpcCommandValidator::sanitizeDaemonRequest(
            command,
            requestedElevateMode,
            m_trustedServerExecutable,
            m_trustedClientExecutable,
            sanitized,
            &reason)) {
        return false;
    }
    command = sanitized.command;
    sanitizedElevateMode = sanitized.elevateMode;
    return true;
}

void
DaemonApp::foregroundError(const char* message)
{
    MessageBox(NULL, message, WEAVE_SERVICE_DISPLAY_NAME, MB_OK | MB_ICONERROR);
}

std::string
DaemonApp::logFilename()
{
    return protectedServiceLogFilename();
}

void
DaemonApp::handleIpcMessage(const Event& e, void*)
{
    IpcMessage* m = static_cast<IpcMessage*>(e.getDataObject());
    if (m == NULL) {
        LOG((CLOG_WARN "ignoring empty ipc message"));
        return;
    }

    switch (m->type()) {
        case kIpcStopRequest: {
            IpcStopRequestMessage* stop =
                static_cast<IpcStopRequestMessage*>(m);
            if (!m_daemonized || stop->requestId() == 0 ||
                !stop->origin().kernelVerified() ||
                stop->origin().processId() == 0) {
                LOG((CLOG_WARN
                    "rejecting invalid service stop request id=%llu",
                    static_cast<unsigned long long>(stop->requestId())));
                break;
            }

            HANDLE rawAuthenticatedUserToken = nullptr;
            std::string originReason;
            if (!acquireCurrentCommandOriginToken(
                    stop->origin(), rawAuthenticatedUserToken,
                    originReason)) {
                LOG((CLOG_ERR
                    "rejecting service stop from an inactive origin: %s",
                    originReason.c_str()));
                break;
            }
            ScopedWindowsHandle authenticatedUserToken(
                rawAuthenticatedUserToken);

            ServiceLaunchState stoppedState;
            std::string encodedStoppedState;
            std::string stateError;
            if (!serializeServiceLaunchState(
                    stoppedState, encodedStoppedState, &stateError)) {
                LOG((CLOG_ERR
                    "could not serialize service stop state: %s",
                    stateError.c_str()));
                break;
            }

            try {
                // Persist the stop before asking the watchdog to execute it.
                // A service restart must never resurrect the generation that
                // this request is fencing.
                ARCH->setting(
                    kServiceLaunchStateSetting, encodedStoppedState);
                try {
                    ARCH->setting(kServiceLaunchPendingSetting, "");
                }
                catch (const XArch& e) {
                    LOG((CLOG_ERR
                        "stopped service but could not clear pending launch: %s",
                        e.what()));
                }
            }
            catch (const XArch& e) {
                LOG((CLOG_ERR
                    "could not durably persist service stop request: %s",
                    e.what()));
                break;
            }

            unsigned long long commandGeneration = 0;
            if (!m_watchdog->requestStop(commandGeneration)) {
                LOG((CLOG_ERR
                    "watchdog rejected service stop request id=%llu",
                    static_cast<unsigned long long>(stop->requestId())));
                break;
            }

            PendingStopAck pending = {
                stop->requestId(),
                stop->origin().processId(),
                commandGeneration,
            };
            m_pendingStopAcks.erase(
                std::remove_if(
                    m_pendingStopAcks.begin(), m_pendingStopAcks.end(),
                    [&pending](const PendingStopAck& existing) {
                        return existing.processId == pending.processId &&
                            existing.requestId == pending.requestId;
                    }),
                m_pendingStopAcks.end());
            const std::size_t kMaxPendingStopAcks = 32;
            if (m_pendingStopAcks.size() >= kMaxPendingStopAcks) {
                LOG((CLOG_WARN
                    "dropping oldest unconfirmed service stop request"));
                m_pendingStopAcks.erase(m_pendingStopAcks.begin());
            }
            m_pendingStopAcks.push_back(pending);

            LOG((CLOG_INFO
                "accepted service stop request id=%llu generation=%llu pid=%u",
                static_cast<unsigned long long>(stop->requestId()),
                commandGeneration, stop->origin().processId()));
            if (m_watchdog->isStopConfirmed(commandGeneration)) {
                acknowledgeConfirmedStops(commandGeneration);
            }
            break;
        }

        case kIpcCommand: {
            IpcCommandMessage* cm = static_cast<IpcCommandMessage*>(m);
            String command = cm->command();
            UInt8 elevateMode = cm->elevateMode();

            HANDLE rawAuthenticatedUserToken = nullptr;
            if (m_daemonized) {
                std::string originReason;
                if (!acquireCurrentCommandOriginToken(
                        cm->origin(), rawAuthenticatedUserToken,
                        originReason)) {
                    LOG((CLOG_ERR
                        "rejecting service command from an inactive origin: %s",
                        originReason.c_str()));
                    break;
                }
            }
            ScopedWindowsHandle authenticatedUserToken(
                rawAuthenticatedUserToken);

            // if empty quotes, clear.
            if (command == "\"\"") {
                command.clear();
            }

            std::string rejectReason;
            const String requestedCommand = command;
            const UInt8 requestedElevateMode = elevateMode;
            if (!prepareWatchdogCommand(command, requestedElevateMode,
                                        elevateMode, rejectReason)) {
                LOG((CLOG_ERR "rejecting ipc command: %s", rejectReason.c_str()));
                break;
            }
            if (command != requestedCommand) {
                LOG((CLOG_WARN "rebuilt ipc command from service allowlist: %s",
                     command.c_str()));
            }
            if (elevateMode != requestedElevateMode) {
                LOG((CLOG_WARN
                    "ignored GUI Always elevation request; using AsNeeded"));
            }

            String logLevel;
            if (!command.empty()) {
                LOG((CLOG_DEBUG "new command, elevateMode=%d command=%s", elevateMode, command.c_str()));

                std::vector<String> argsArray;
                ArgParser::splitCommandString(command, argsArray);
                if (argsArray.empty()) {
                    LOG((CLOG_ERR "rejecting ipc command: command parsed to no arguments"));
                    break;
                }

                ArgParser argParser(NULL);
                const char** argv = argParser.getArgv(argsArray);
                ServerArgs serverArgs;
                ClientArgs clientArgs;
                int argc = static_cast<int>(argsArray.size());
                bool server = IpcCommandValidator::isServerCommand(command);
                ArgsBase* argBase = NULL;

                bool parsed = false;
                if (server) {
                    parsed = argParser.parseServerArgs(serverArgs, argc, argv);
                    argBase = &serverArgs;
                }
                else {
                    parsed = argParser.parseClientArgs(clientArgs, argc, argv);
                    argBase = &clientArgs;
                }

                delete[] argv;
                if (!parsed) {
                    LOG((CLOG_ERR
                        "rejecting internally invalid sanitized ipc command"));
                    break;
                }

                logLevel = argBase->m_logFilter;
            }
            else {
                LOG((CLOG_DEBUG "empty command, elevateMode=%d", elevateMode));
            }

            MSWindowsWatchdog::LaunchProfile launchProfile;
            ServiceLaunchProfileResult stagedProfileResult;
            if (m_daemonized && !command.empty()) {
                ServiceLaunchState committedState;
                const std::string encodedCommittedState =
                    ARCH->setting(kServiceLaunchStateSetting);
                std::string reusableGeneration;
                std::string reusableDigest;
                if (parseServiceLaunchState(
                        encodedCommittedState, committedState) &&
                    committedState.ownerSid == cm->origin().userSid()) {
                    reusableGeneration = committedState.generation;
                    reusableDigest = committedState.digest;
                }
                stagedProfileResult = stageWindowsServiceLaunchProfile(
                    authenticatedUserToken.get(), cm->origin().userSid(),
                    launchRole(command), reusableGeneration, reusableDigest);
                std::string profileReason;
                if (!makeWatchdogLaunchProfile(
                        stagedProfileResult, cm->origin().sessionId(),
                        launchProfile, profileReason)) {
                    discardStagedProfileWithLog(
                        stagedProfileResult,
                        "staged profile metadata was unusable");
                    LOG((CLOG_ERR
                        "rejecting service command: launch profile staging failed stage=%d error=%u detail=%s",
                        static_cast<int>(stagedProfileResult.error),
                        stagedProfileResult.systemError,
                        profileReason.c_str()));
                    break;
                }
            }

            ServiceLaunchState requestedState;
            ServiceLaunchCandidate launchCandidate;
            std::string encodedServiceState;
            if (m_daemonized) {
                HANDLE rawRevalidatedToken = nullptr;
                std::string originReason;
                if (!acquireCurrentCommandOriginToken(
                        cm->origin(), rawRevalidatedToken, originReason)) {
                    discardStagedProfileWithLog(
                        stagedProfileResult, "origin changed after staging");
                    LOG((CLOG_ERR
                        "rejecting service command after staging because its origin changed: %s",
                        originReason.c_str()));
                    break;
                }
                ScopedWindowsHandle revalidatedToken(rawRevalidatedToken);

                requestedState.command = command;
                requestedState.elevateMode = elevateMode;
                requestedState.logLevel = logLevel;
                if (!command.empty()) {
                    requestedState.sessionId = launchProfile.sessionId;
                    requestedState.ownerSid = launchProfile.userSid;
                    requestedState.generation = launchProfile.generation;
                    requestedState.digest = launchProfile.digest;
                }
                std::string stateError;
                bool serialized = false;
                if (command.empty()) {
                    serialized = serializeServiceLaunchState(
                        requestedState, encodedServiceState, &stateError);
                }
                else if (m_launchRevision ==
                         (std::numeric_limits<std::uint64_t>::max)()) {
                    stateError = "service launch candidate revision exhausted";
                }
                else {
                    launchCandidate.revision = ++m_launchRevision;
                    launchCandidate.state = requestedState;
                    serialized = serializeServiceLaunchCandidate(
                        launchCandidate, encodedServiceState, &stateError);
                }
                if (!serialized) {
                    discardStagedProfileWithLog(
                        stagedProfileResult, "candidate serialization failed");
                    LOG((CLOG_ERR
                        "rejecting service command: atomic launch state is invalid: %s",
                        stateError.c_str()));
                    break;
                }
            }

            try {
                if (m_daemonized) {
                    if (command.empty()) {
                        // Stop is itself a proven state transition. Commit it
                        // before discarding any in-flight launch candidate.
                        ARCH->setting(kServiceLaunchStateSetting,
                                      encodedServiceState);
                        try {
                            ARCH->setting(kServiceLaunchPendingSetting, "");
                        }
                        catch (const XArch& e) {
                            // Current already says stopped. A restart still
                            // ignores and retries clearing stale Pending.
                            LOG((CLOG_ERR
                                "stopped service but could not clear pending launch: %s",
                                e.what()));
                        }
                    }
                    else {
                        // Current remains last-good until watchdog readiness.
                        ARCH->setting(kServiceLaunchPendingSetting,
                                      encodedServiceState);
                    }
                }
                else {
                    ARCH->setting("Elevate", String(
                        elevateMode == IpcCommandMessage::kElevateAlways
                            ? "1" : "0"));
                    ARCH->setting("ElevateMode",
                                  String(std::to_string(elevateMode)));
                    if (!logLevel.empty()) {
                        ARCH->setting("LogLevel", logLevel);
                    }
                    ARCH->setting("Command", command);
                }
            }
            catch (XArch& e) {
                discardStagedProfileWithLog(
                    stagedProfileResult, "candidate persistence failed");
                LOG((CLOG_ERR
                    "failed to persist service command candidate; keeping the current process: %s",
                    e.what()));
                break;
            }

            // tell the relauncher about the new command. this causes the
            // relauncher to stop the existing command and start the new
            // command.
            bool watchdogAccepted = true;
            if (m_daemonized) {
                if (command.empty()) {
                    m_watchdog->setCommand(command, elevateMode, launchProfile);
                }
                else {
                    watchdogAccepted = m_watchdog->setCommand(
                        command, elevateMode, launchProfile, launchCandidate);
                }
            }
            else {
                m_watchdog->setCommand(command, elevateMode);
            }

            if (!watchdogAccepted) {
                try {
                    ARCH->setting(kServiceLaunchPendingSetting, "");
                }
                catch (const XArch& e) {
                    LOG((CLOG_ERR
                        "watchdog rejected service launch revision=%llu and Pending could not be cleared: %s",
                        static_cast<unsigned long long>(launchCandidate.revision),
                        e.what()));
                }
                discardStagedProfileWithLog(
                    stagedProfileResult, "watchdog rejected candidate");
                LOG((CLOG_ERR
                    "watchdog rejected persisted service launch revision=%llu; keeping current process",
                    static_cast<unsigned long long>(launchCandidate.revision)));
                break;
            }

            if (!logLevel.empty()) {
                CLOG->setFilter(logLevel.c_str());
            }
            // Service logging stays under the protected ProgramData root.
            // GUI-supplied --log paths were removed by the command policy.
            if (m_fileLogOutputter != nullptr) {
                m_watchdog->setFileLogOutputter(m_fileLogOutputter);
            }

            break;
        }

        case kIpcHello: {
            IpcHelloMessage* hm = static_cast<IpcHelloMessage*>(m);
            String type;
            switch (hm->clientType()) {
                case kIpcClientGui: type = "gui"; break;
                case kIpcClientNode: type = "node"; break;
                default: type = "unknown"; break;
            }

            LOG((CLOG_DEBUG "ipc hello, type=%s", type.c_str()));

            const char * serverstatus = m_watchdog->isProcessActive() ? "active" : "not active";

            // using CLOG_PRINT here allows the GUI to see that the server status
            // regardless of which log level is set
            LOG((CLOG_PRINT "server status: %s", serverstatus));

            m_ipcLogOutputter->notifyBuffer();
            break;
        }

        case kIpcReady:
            LOG((CLOG_WARN
                "ipc node reported legacy readiness without input capabilities; "
                "service and node builds must be deployed together"));
            break;

        case kIpcReadyV2: {
            IpcNodeReadyV2Message* ready =
                static_cast<IpcNodeReadyV2Message*>(m);
            const char* desktop = ready->desktopName().empty() ?
                "<none>" : ready->desktopName().c_str();
            if (ready->inputReady() && ready->queryNonce() != 0) {
                LOG((CLOG_INFO "ipc node input readiness pid=%u session=%u desktop=%s generation=%llu ready=yes build=%s query=%llu",
                    ready->processId(), ready->sessionId(), desktop,
                    static_cast<unsigned long long>(ready->inputGeneration()),
                    ready->buildId().c_str(),
                    static_cast<unsigned long long>(ready->queryNonce())));
            }
            else if (ready->inputReady()) {
                LOG((CLOG_DEBUG2 "ipc node input readiness pid=%u session=%u desktop=%s generation=%llu ready=yes build=%s query=0",
                    ready->processId(), ready->sessionId(), desktop,
                    static_cast<unsigned long long>(ready->inputGeneration()),
                    ready->buildId().c_str()));
            }
            else if (ready->queryNonce() != 0) {
                LOG((CLOG_WARN "ipc node input readiness pid=%u session=%u desktop=%s generation=%llu ready=no build=%s query=%llu",
                    ready->processId(), ready->sessionId(), desktop,
                    static_cast<unsigned long long>(ready->inputGeneration()),
                    ready->buildId().c_str(),
                    static_cast<unsigned long long>(ready->queryNonce())));
            }
            else {
                LOG((CLOG_DEBUG2 "ipc node input readiness pid=%u session=%u desktop=%s generation=%llu ready=no build=%s query=0",
                    ready->processId(), ready->sessionId(), desktop,
                    static_cast<unsigned long long>(ready->inputGeneration()),
                    ready->buildId().c_str()));
            }
            break;
        }
    }
}

void
DaemonApp::notifyWatchdogStopConfirmed(
    unsigned long long commandGeneration)
{
    if (!m_acceptStopConfirmations.load() || m_events == nullptr ||
        m_stopConfirmedEvent == Event::kUnknown) {
        return;
    }

    Event event(m_stopConfirmedEvent, this);
    event.setDataObject(new ServiceStopConfirmedEventData(
        commandGeneration));
    try {
        m_events->addEvent(event);
    }
    catch (const std::exception& e) {
        LOG((CLOG_ERR
            "could not enqueue service stop confirmation generation=%llu: %s",
            commandGeneration, e.what()));
    }
    catch (...) {
        LOG((CLOG_ERR
            "could not enqueue service stop confirmation generation=%llu",
            commandGeneration));
    }
}

void
DaemonApp::handleWatchdogStopConfirmed(const Event& event, void*)
{
    ServiceStopConfirmedEventData* stopped =
        static_cast<ServiceStopConfirmedEventData*>(event.getDataObject());
    if (stopped == nullptr || m_watchdog == nullptr ||
        !m_acceptStopConfirmations.load() ||
        !m_watchdog->isStopConfirmed(stopped->commandGeneration)) {
        return;
    }
    acknowledgeConfirmedStops(stopped->commandGeneration);
}

void
DaemonApp::acknowledgeConfirmedStops(
    unsigned long long commandGeneration)
{
    std::vector<PendingStopAck> remaining;
    remaining.reserve(m_pendingStopAcks.size());
    for (const PendingStopAck& pending : m_pendingStopAcks) {
        if (pending.commandGeneration > commandGeneration) {
            remaining.push_back(pending);
            continue;
        }
        if (pending.commandGeneration < commandGeneration) {
            LOG((CLOG_WARN
                "discarding superseded service stop request id=%llu generation=%llu",
                static_cast<unsigned long long>(pending.requestId),
                pending.commandGeneration));
            continue;
        }

        IpcStopAckMessage ack(pending.requestId, commandGeneration);
        bool acknowledged = false;
        try {
            acknowledged = m_ipcServer->sendToProcess(
                ack, kIpcClientGui, pending.processId);
            if (acknowledged) {
                LOG((CLOG_INFO
                    "acknowledged service stop request id=%llu generation=%llu pid=%u",
                    static_cast<unsigned long long>(pending.requestId),
                    commandGeneration, pending.processId));
            }
            else {
                LOG((CLOG_WARN
                    "requesting GUI disconnected before service stop acknowledgement id=%llu",
                    static_cast<unsigned long long>(pending.requestId)));
            }
        }
        catch (const std::exception& e) {
            LOG((CLOG_ERR
                "could not send service stop acknowledgement id=%llu: %s",
                static_cast<unsigned long long>(pending.requestId),
                e.what()));
        }
        catch (...) {
            LOG((CLOG_ERR
                "could not send service stop acknowledgement id=%llu",
                static_cast<unsigned long long>(pending.requestId)));
        }
        if (!acknowledged) {
            remaining.push_back(pending);
        }
    }
    m_pendingStopAcks.swap(remaining);
}

ServiceLaunchCommitResult
DaemonApp::commitWatchdogLaunchReady(
    const ServiceLaunchCandidate& candidate)
{
    if (!m_acceptLaunchCommits.load() || m_events == nullptr ||
        m_launchReadyEvent == Event::kUnknown) {
        return ServiceLaunchCommitResult::kRejected;
    }

    std::shared_ptr<ServiceLaunchCommitCompletion> completion(
        new ServiceLaunchCommitCompletion());
    Event event(m_launchReadyEvent, this);
    event.setDataObject(new ServiceLaunchReadyEventData(
        candidate, completion));
    try {
        m_events->addEvent(event);
    }
    catch (const std::exception& e) {
        LOG((CLOG_ERR
            "could not enqueue service launch durable commit revision=%llu: %s",
            static_cast<unsigned long long>(candidate.revision), e.what()));
        return ServiceLaunchCommitResult::kRejected;
    }
    catch (...) {
        LOG((CLOG_ERR
            "could not enqueue service launch durable commit revision=%llu",
            static_cast<unsigned long long>(candidate.revision)));
        return ServiceLaunchCommitResult::kRejected;
    }

    std::unique_lock<std::mutex> lock(completion->mutex);
    if (!completion->condition.wait_for(
            lock,
            std::chrono::seconds(kServiceLaunchCommitDispatchSeconds),
            [completion]() {
                return completion->state == ServiceLaunchCommitState::kDone;
            })) {
        if (completion->state == ServiceLaunchCommitState::kQueued) {
            completion->state = ServiceLaunchCommitState::kCancelled;
            LOG((CLOG_ERR
                "timed out before service launch durable commit started revision=%llu",
                static_cast<unsigned long long>(candidate.revision)));
            return ServiceLaunchCommitResult::kTimedOut;
        }
        if (completion->state == ServiceLaunchCommitState::kStarted) {
            LOG((CLOG_CRIT
                "service launch durable commit exceeded its deadline after starting revision=%llu",
                static_cast<unsigned long long>(candidate.revision)));
            return ServiceLaunchCommitResult::kIndeterminate;
        }
    }
    return completion->result;
}

void
DaemonApp::handleWatchdogLaunchReady(const Event& event, void*)
{
    ServiceLaunchReadyEventData* ready =
        static_cast<ServiceLaunchReadyEventData*>(event.getDataObject());
    if (ready == nullptr || !ready->completion) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(ready->completion->mutex);
        if (ready->completion->state != ServiceLaunchCommitState::kQueued) {
            return;
        }
        ready->completion->state = ServiceLaunchCommitState::kStarted;
    }

    ServiceLaunchCommitResult result = ServiceLaunchCommitResult::kRejected;
    try {
        result =
            m_daemonized && m_acceptLaunchCommits.load()
            ? promoteWatchdogLaunchReady(ready->candidate)
            : ServiceLaunchCommitResult::kRejected;
    }
    catch (const std::exception& e) {
        LOG((CLOG_ERR
            "service launch durable commit handler failed revision=%llu: %s",
            static_cast<unsigned long long>(ready->candidate.revision),
            e.what()));
    }
    catch (...) {
        LOG((CLOG_ERR
            "service launch durable commit handler failed revision=%llu",
            static_cast<unsigned long long>(ready->candidate.revision)));
    }

    {
        std::lock_guard<std::mutex> lock(ready->completion->mutex);
        ready->completion->result = result;
        ready->completion->state = ServiceLaunchCommitState::kDone;
    }
    ready->completion->condition.notify_all();
}

ServiceLaunchCommitResult
DaemonApp::promoteWatchdogLaunchReady(
    const ServiceLaunchCandidate& candidate)
{

    std::string encodedPending;
    try {
        encodedPending = ARCH->setting(kServiceLaunchPendingSetting);
    }
    catch (const XArch& e) {
        LOG((CLOG_ERR
            "could not read pending service launch for readiness commit: %s",
            e.what()));
        return ServiceLaunchCommitResult::kRejected;
    }

    ServiceLaunchState matchedState;
    std::string matchError;
    const ServiceLaunchPromotionStatus status = matchServiceLaunchPromotion(
        encodedPending, candidate, matchedState, &matchError);
    if (status != ServiceLaunchPromotionStatus::kMatched) {
        LOG((CLOG_WARN
            "ignoring stale or invalid service launch readiness revision=%llu status=%d detail=%s",
            static_cast<unsigned long long>(candidate.revision),
            static_cast<int>(status), matchError.c_str()));
        return ServiceLaunchCommitResult::kRejected;
    }

    std::string encodedCurrent;
    std::string stateError;
    if (!serializeServiceLaunchState(
            matchedState, encodedCurrent, &stateError)) {
        LOG((CLOG_ERR
            "could not serialize ready service launch revision=%llu: %s",
            static_cast<unsigned long long>(candidate.revision),
            stateError.c_str()));
        return ServiceLaunchCommitResult::kRejected;
    }

    try {
        // The checked Current write is the promotion commit. If it fails,
        // last-good remains untouched and Pending remains available for
        // diagnosis or a later matching readiness event.
        ARCH->setting(kServiceLaunchStateSetting, encodedCurrent);
    }
    catch (const XArch& e) {
        LOG((CLOG_ERR
            "could not promote ready service launch revision=%llu; preserving last-good Current: %s",
            static_cast<unsigned long long>(candidate.revision),
            e.what()));
        return ServiceLaunchCommitResult::kRejected;
    }

    try {
        ARCH->setting(kServiceLaunchPendingSetting, "");
    }
    catch (const XArch& e) {
        // Current has already committed. Startup recovery will select it and
        // retry clearing the now-stale Pending value.
        LOG((CLOG_ERR
            "promoted service launch revision=%llu but could not clear stale Pending: %s",
            static_cast<unsigned long long>(candidate.revision),
            e.what()));
        return ServiceLaunchCommitResult::kCommitted;
    }

    LOG((CLOG_INFO
        "promoted ready service launch revision=%llu generation=%s",
        static_cast<unsigned long long>(candidate.revision),
        matchedState.generation.c_str()));
    return ServiceLaunchCommitResult::kCommitted;
}
