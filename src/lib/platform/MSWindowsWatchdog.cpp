/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2009 Chris Schoeneman
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

#include "platform/MSWindowsWatchdog.h"

#include "ipc/IpcLogOutputter.h"
#include "ipc/IpcServer.h"
#include "ipc/IpcMessage.h"
#include "ipc/ElevationPolicy.h"
#include "ipc/IpcCommandValidator.h"
#include "ipc/Ipc.h"
#include "barrier/App.h"
#include "barrier/ArgParser.h"
#include "barrier/ArgsBase.h"
#include "mt/Thread.h"
#include "arch/win32/ArchDaemonWindows.h"
#include "arch/win32/XArchWindows.h"
#include "arch/Arch.h"
#include "base/log_outputters.h"
#include "base/Log.h"
#include "base/XBase.h"
#include "common/Version.h"
#include "common/win32/encoding_utilities.h"

#include <openssl/rand.h>

#include <sstream>
#include <atomic>
#include <cstdint>
#include <limits>
#include <UserEnv.h>
#include <Shellapi.h>
#include <Sddl.h>
#include <Wtsapi32.h>
#include <vector>

#define MAXIMUM_WAIT_TIME 3
enum {
    kOutputBufferSize = 4096
};
static const int kDesktopNameReadAttempts = 50;
static const double kDesktopNameReadRetrySeconds = 0.1;
static const double kDesktopRelaunchSettleSeconds = 0.25;
static const double kDesktopRelaunchDebounceSeconds = 2.0;
static const double kProcessReadyTimeoutSeconds = 10.0;
static const double kProcessActivationTimeoutSeconds = 10.0;
static const double kProcessReadyPollSeconds = 0.05;
static const double kReadinessProofWaitSeconds = 0.5;
static const double kWatchdogStopBudgetSeconds = 5.0;
static const double kWatchdogStopGraceSeconds = 0.25;
static const double kWatchdogStopPollSeconds = 0.025;
static const char kServiceStandbyOption[] = "--service-standby";

typedef VOID (WINAPI *SendSas)(BOOL asUser);

namespace {

bool
isValidHandle(HANDLE handle)
{
    return handle != NULL && handle != INVALID_HANDLE_VALUE;
}

class ScopedHandle {
public:
    ScopedHandle() :
        m_handle(NULL)
    {
    }

    explicit ScopedHandle(HANDLE handle) :
        m_handle(handle)
    {
    }

    ~ScopedHandle()
    {
        reset();
    }

    HANDLE get() const
    {
        return m_handle;
    }

    HANDLE release()
    {
        HANDLE handle = m_handle;
        m_handle = NULL;
        return handle;
    }

    void reset(HANDLE handle = NULL)
    {
        if (isValidHandle(m_handle)) {
            CloseHandle(m_handle);
        }
        m_handle = handle;
    }

private:
    ScopedHandle(const ScopedHandle&);
    ScopedHandle& operator=(const ScopedHandle&);

	HANDLE m_handle;
};

class ScopedEnvironmentBlock {
public:
	ScopedEnvironmentBlock() :
		m_environment(NULL)
	{
	}

	~ScopedEnvironmentBlock()
	{
		reset();
	}

	LPVOID get() const
	{
		return m_environment;
	}

	LPVOID* out()
	{
		reset();
		return &m_environment;
	}

	void reset(LPVOID environment = NULL)
	{
		if (m_environment != NULL) {
			DestroyEnvironmentBlock(m_environment);
		}
		m_environment = environment;
	}

private:
	ScopedEnvironmentBlock(const ScopedEnvironmentBlock&);
	ScopedEnvironmentBlock& operator=(const ScopedEnvironmentBlock&);

	LPVOID m_environment;
};

class ScopedModule {
public:
	explicit ScopedModule(HINSTANCE module) :
		m_module(module)
	{
	}

	~ScopedModule()
	{
		if (m_module != NULL) {
			FreeLibrary(m_module);
		}
	}

	HINSTANCE get() const
	{
		return m_module;
	}

private:
	ScopedModule(const ScopedModule&);
	ScopedModule& operator=(const ScopedModule&);

	HINSTANCE m_module;
};

void
closeProcessInfo(PROCESS_INFORMATION& info)
{
    if (isValidHandle(info.hThread)) {
        CloseHandle(info.hThread);
    }
    if (isValidHandle(info.hProcess)) {
        CloseHandle(info.hProcess);
    }
    ZeroMemory(&info, sizeof(PROCESS_INFORMATION));
}

bool
isProcessHandleActive(HANDLE process)
{
    if (!isValidHandle(process)) {
        return false;
    }

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process, &exitCode)) {
        LOG((CLOG_WARN "could not query process status, error=%lu", GetLastError()));
        return false;
    }
    return exitCode == STILL_ACTIVE;
}

bool
isProcessHandleStopped(HANDLE process)
{
    if (!isValidHandle(process)) {
        return true;
    }

    const DWORD waitResult = WaitForSingleObject(process, 0);
    if (waitResult == WAIT_OBJECT_0) {
        return true;
    }
    if (waitResult == WAIT_FAILED) {
        LOG((CLOG_WARN "could not verify process exit, error=%lu",
            GetLastError()));
    }
    return false;
}

bool
tokenMatchesUserSid(HANDLE token, const std::string& expectedSid)
{
    if (!isValidHandle(token) || expectedSid.empty()) {
        return false;
    }

    PSID parsedSid = nullptr;
    if (!ConvertStringSidToSidA(expectedSid.c_str(), &parsedSid) ||
        parsedSid == nullptr || !IsValidSid(parsedSid)) {
        if (parsedSid != nullptr) {
            LocalFree(parsedSid);
        }
        return false;
    }

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        LocalFree(parsedSid);
        return false;
    }
    std::vector<unsigned char> storage(size);
    DWORD returned = 0;
    const bool matches = GetTokenInformation(
            token, TokenUser, storage.data(), size, &returned) != FALSE &&
        returned >= sizeof(TOKEN_USER) &&
        IsValidSid(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid) &&
        EqualSid(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid,
                 parsedSid) != FALSE;
    LocalFree(parsedSid);
    return matches;
}

bool
sameLaunchIdentity(const MSWindowsWatchdog::LaunchProfile& left,
                   const MSWindowsWatchdog::LaunchProfile& right)
{
    return left.authenticated == right.authenticated &&
        left.sessionId == right.sessionId &&
        left.userSid == right.userSid &&
        left.profileDirectory == right.profileDirectory &&
        left.generation == right.generation &&
        left.digest == right.digest;
}

bool
sameLaunchOwnerIdentity(const MSWindowsWatchdog::LaunchProfile& left,
                        const MSWindowsWatchdog::LaunchProfile& right)
{
    return left.authenticated && right.authenticated &&
        left.sessionId == right.sessionId &&
        left.userSid == right.userSid;
}

bool
openSslRandomBytes(unsigned char* bytes, std::size_t size)
{
    if (bytes == nullptr ||
        size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return RAND_bytes(bytes, static_cast<int>(size)) == 1;
}

bool
nextReadinessQueryNonce(std::uint64_t& nonce)
{
    return MSWindowsWatchdog::generateReadinessNonce(
        nonce, openSslRandomBytes);
}

bool
containsServiceStandbyOption(const std::string& command)
{
    String parsedCommand = command;
    std::vector<String> arguments;
    ArgParser::splitCommandString(parsedCommand, arguments);
    for (std::vector<String>::const_iterator argument = arguments.begin();
         argument != arguments.end(); ++argument) {
        if (*argument == kServiceStandbyOption) {
            return true;
        }
    }
    return false;
}

}

bool
MSWindowsWatchdog::generateReadinessNonce(
    std::uint64_t& nonce, const RandomBytesProvider& provider)
{
    nonce = 0;
    bool generated = false;
    try {
        generated = provider && provider(
            reinterpret_cast<unsigned char*>(&nonce), sizeof(nonce));
    }
    catch (...) {
        generated = false;
    }

    if (!generated || nonce == 0) {
        nonce = 0;
        return false;
    }
    return true;
}

bool
MSWindowsWatchdog::containsInternalStandbyOption(
    const std::string& command)
{
    return containsServiceStandbyOption(command);
}

bool
MSWindowsWatchdog::isExternalCommandAccepted(
    const std::string& command)
{
    return !containsServiceStandbyOption(command);
}

std::string
MSWindowsWatchdog::makeStandbyLaunchCommand(
    const std::string& command)
{
    std::string launchCommand;
    std::string reason;
    if (!IpcCommandValidator::deriveServiceStandbyCommand(
            command, launchCommand, &reason)) {
        LOG((CLOG_ERR "could not derive service standby command: %s",
            reason.c_str()));
        launchCommand.clear();
    }
    return launchCommand;
}

bool
MSWindowsWatchdog::isSameAuthenticatedLaunchOwner(
    const LaunchProfile& left, const LaunchProfile& right)
{
    return sameLaunchOwnerIdentity(left, right);
}

bool
MSWindowsWatchdog::shouldAbortPendingActivation(
    bool monitoring,
    bool commandEmpty,
    bool ownerMatches,
    bool sessionChanged)
{
    return !monitoring || commandEmpty || !ownerMatches || sessionChanged;
}

bool
MSWindowsWatchdog::shouldDiscardFailedActivation(
    bool monitoring,
    bool commandEmpty,
    bool ownerMatches)
{
    return !monitoring || commandEmpty || !ownerMatches;
}

double
MSWindowsWatchdog::boundedShutdownWaitSeconds(
    double deadlineSeconds,
    double nowSeconds,
    double maximumWaitSeconds)
{
    if (maximumWaitSeconds <= 0.0 || deadlineSeconds <= nowSeconds) {
        return 0.0;
    }

    const double remainingSeconds = deadlineSeconds - nowSeconds;
    return remainingSeconds < maximumWaitSeconds
        ? remainingSeconds : maximumWaitSeconds;
}

bool
MSWindowsWatchdog::stopConfirmationReady(
    bool commandEmpty,
    bool publishedProcessExists,
    bool pendingProcessExists)
{
    return commandEmpty && !publishedProcessExists && !pendingProcessExists;
}

std::string activeDesktopName(bool warnOnFailure = true, DWORD* errorOut = NULL)
{
    std::string name;
    DWORD error = ERROR_SUCCESS;
    HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (desk != NULL) {
        DWORD requiredBytes = 0;
        if (GetUserObjectInformationW(desk, UOI_NAME, NULL, 0, &requiredBytes) == FALSE &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            error = GetLastError();
        }
        else {
            const std::size_t requiredCharacters =
                (static_cast<std::size_t>(requiredBytes) + sizeof(WCHAR) - 1u) /
                sizeof(WCHAR);
            std::vector<WCHAR> buffer(requiredCharacters + 1u, L'\0');
            if (GetUserObjectInformationW(
                    desk, UOI_NAME, buffer.data(),
                    static_cast<DWORD>(buffer.size() * sizeof(WCHAR)),
                    NULL) == TRUE) {
                name = win_wchar_to_utf8(buffer.data());
                if (name.empty()) {
                    error = ERROR_NO_UNICODE_TRANSLATION;
                }
            }
            else {
                error = GetLastError();
            }
        }
        CloseDesktop(desk);
    }
    else {
        error = GetLastError();
    }

    LOG((CLOG_DEBUG "found desktop name: %.64s", name.c_str()));
    if (name.empty() && warnOnFailure) {
        LOG((CLOG_WARN "could not read active input desktop name, error=%lu", error));
    }
    if (errorOut != NULL) {
        *errorOut = error;
    }
    return name;
}

std::string activeDesktopNameWithRetry(const std::atomic<bool>& monitoring)
{
    DWORD lastError = ERROR_SUCCESS;
    std::string desktopName;
    for (int attempt = 0; monitoring.load() && attempt < kDesktopNameReadAttempts; ++attempt) {
        desktopName = activeDesktopName(false, &lastError);
        if (!desktopName.empty()) {
            if (attempt > 0) {
                LOG((CLOG_INFO "active input desktop became available after %d read attempt(s)",
                    attempt + 1));
            }
            return desktopName;
        }

        if (attempt + 1 < kDesktopNameReadAttempts) {
            LOG((CLOG_DEBUG "active input desktop unavailable, retrying read attempt=%d/%d error=%lu",
                attempt + 1, kDesktopNameReadAttempts, lastError));
            ARCH->sleep(kDesktopNameReadRetrySeconds);
        }
    }

    LOG((CLOG_WARN "could not read active input desktop name after %d attempt(s), lastError=%lu",
        kDesktopNameReadAttempts, lastError));
    return std::string();
}

MSWindowsWatchdog::MSWindowsWatchdog(
    bool daemonized,
    bool autoDetectCommand,
    IpcServer& ipcServer,
    IpcLogOutputter& ipcLogOutputter) :
    m_thread(NULL),
    m_autoDetectCommand(autoDetectCommand),
    m_monitoring(true),
	m_commandChanged(false),
    m_commandGeneration(0),
	m_stdOutWrite(NULL),
	m_stdOutRead(NULL),
	m_outputThread(NULL),
    m_ipcServer(ipcServer),
    m_ipcLogOutputter(ipcLogOutputter),
    m_elevateMode(IpcCommandMessage::kElevateAsNeeded),
    m_hasLaunchCandidate(false),
    m_processJob(NULL),
    m_processFailures(0),
    m_processRunning(false),
    m_confirmedStopGeneration(0),
    m_hasConfirmedStopGeneration(false),
    m_fileLogOutputter(NULL),
    m_daemonized(daemonized)
{
    ZeroMemory(&m_processInfo, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&m_pendingProcessInfo, sizeof(PROCESS_INFORMATION));
}

MSWindowsWatchdog::~MSWindowsWatchdog()
{
    // DaemonApp stops explicitly, but the destructor remains the final owner
    // for partial startup and exceptional teardown paths.
    stop();
}

void
MSWindowsWatchdog::startAsync()
{
    m_monitoring.store(true);
    try {
        createManagedProcessJob();
        createOutputPipeHandles();
        m_thread = new Thread([this](){ main_loop(); });
        m_outputThread = new Thread([this](){ output_loop(); });
    }
    catch (...) {
        stop();
        throw;
    }
}

void
MSWindowsWatchdog::stop()
{
	const double stopDeadline = ARCH->time() + kWatchdogStopBudgetSeconds;
	m_monitoring.store(false);
	closeOutputPipeHandles();

	bool mainStopped = m_thread == NULL || m_thread->wait(0.0);
	bool outputStopped = m_outputThread == NULL || m_outputThread->wait(0.0);
	const auto waitForThreadsUntil = [this, &mainStopped, &outputStopped](
			double deadline) {
		while (!mainStopped || !outputStopped) {
			if (!mainStopped) {
				const double waitSeconds = boundedShutdownWaitSeconds(
					deadline, ARCH->time(), kWatchdogStopPollSeconds);
				if (waitSeconds <= 0.0) {
					break;
				}
				mainStopped = m_thread->wait(waitSeconds);
			}
			if (!outputStopped) {
				const double waitSeconds = boundedShutdownWaitSeconds(
					deadline, ARCH->time(), kWatchdogStopPollSeconds);
				if (waitSeconds <= 0.0) {
					break;
				}
				outputStopped = m_outputThread->wait(waitSeconds);
			}
		}
	};

	const double graceStartedAt = ARCH->time();
	const double graceDeadline = graceStartedAt +
		boundedShutdownWaitSeconds(
			stopDeadline, graceStartedAt, kWatchdogStopGraceSeconds);
	waitForThreadsUntil(graceDeadline);

	if (!mainStopped) {
		LOG((CLOG_WARN "watchdog main thread did not stop during grace period; cancelling"));
		m_thread->cancel();
		m_thread->unblockPollSocket();
	}
	if (!outputStopped) {
		LOG((CLOG_WARN "watchdog output thread did not stop during grace period; cancelling"));
		m_outputThread->cancel();
		m_outputThread->unblockPollSocket();
	}

	waitForThreadsUntil(stopDeadline);
	if (!mainStopped || !outputStopped) {
		failFastOwnedProcesses(
			"watchdog threads failed to stop before the shared shutdown deadline");
	}

	delete m_thread;
	m_thread = NULL;
	delete m_outputThread;
	m_outputThread = NULL;

    // Idempotent fallback for partial startup or an exceptional main-loop
    // exit. Closing the job first guarantees contained descendants are being
    // terminated before their individual ownership handles are released.
    closeManagedProcessJob();
    const bool pendingStopped = shutdownOwnedProcessBeforeDeadline(
        m_pendingProcessInfo, stopDeadline);
    const bool managedStopped =
        shutdownOwnedProcessBeforeDeadline(m_processInfo, stopDeadline);
    if (managedStopped) {
        m_processRunning.store(false);
    }

    if (!pendingStopped || !managedStopped ||
        isValidHandle(m_pendingProcessInfo.hProcess) ||
        isValidHandle(m_processInfo.hProcess) ||
        isValidHandle(m_processJob)) {
        LOG((CLOG_CRIT
            "watchdog cannot confirm all owned processes exited; terminating the owner process to preserve the kill-on-close fence"));
        if (isValidHandle(m_processJob)) {
            TerminateJobObject(m_processJob, kExitFailed);
        }
        ExitProcess(kExitFailed);
    }
}

void
MSWindowsWatchdog::createManagedProcessJob()
{
    if (!m_daemonized || isValidHandle(m_processJob)) {
        return;
    }

    ScopedHandle job(CreateJobObjectW(NULL, NULL));
    if (!isValidHandle(job.get())) {
        throw XArch(new XArchEvalWindows());
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    ZeroMemory(&limits, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job.get(), JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) {
        throw XArch(new XArchEvalWindows());
    }

    m_processJob = job.release();
    LOG((CLOG_INFO
        "created kill-on-close job for service-managed input processes"));
}

void
MSWindowsWatchdog::closeManagedProcessJob()
{
    if (!isValidHandle(m_processJob)) {
        return;
    }

    if (!CloseHandle(m_processJob)) {
        LOG((CLOG_ERR "could not close managed process job, error=%lu",
            GetLastError()));
        return;
    }
    m_processJob = NULL;
}

void
MSWindowsWatchdog::failFastOwnedProcesses(const char* reason)
{
    LOG((CLOG_CRIT "%s; terminating the service ownership domain",
        reason == nullptr ? "service process ownership is inconsistent" : reason));
    // A containment failure can leave a just-created suspended process outside
    // the job. Terminate both directly as well as terminating the job so no
    // unpublished input host survives the service restart.
    if (isValidHandle(m_pendingProcessInfo.hProcess)) {
        TerminateProcess(m_pendingProcessInfo.hProcess, kExitFailed);
    }
    if (isValidHandle(m_processInfo.hProcess)) {
        TerminateProcess(m_processInfo.hProcess, kExitFailed);
    }
    if (isValidHandle(m_processJob)) {
        TerminateJobObject(m_processJob, kExitFailed);
    }
    ExitProcess(kExitFailed);
}

bool
MSWindowsWatchdog::assignPendingProcessToJob()
{
    if (!m_daemonized) {
        return true;
    }
    if (!isValidHandle(m_processJob) ||
        !isValidHandle(m_pendingProcessInfo.hProcess)) {
        LOG((CLOG_ERR "cannot contain pending process without valid job and process handles"));
        return false;
    }

    BOOL alreadyAssigned = FALSE;
    if (IsProcessInJob(
            m_pendingProcessInfo.hProcess, m_processJob,
            &alreadyAssigned) && alreadyAssigned) {
        return true;
    }
    if (!AssignProcessToJobObject(
            m_processJob, m_pendingProcessInfo.hProcess)) {
        LOG((CLOG_ERR
            "could not assign suspended process %lu to kill-on-close job, error=%lu",
            m_pendingProcessInfo.dwProcessId, GetLastError()));
        return false;
    }
    return true;
}

bool
MSWindowsWatchdog::resumePendingProcess()
{
    if (!m_daemonized) {
        return true;
    }
    if (!isValidHandle(m_pendingProcessInfo.hThread)) {
        LOG((CLOG_ERR "cannot resume pending process %lu without a thread handle",
            m_pendingProcessInfo.dwProcessId));
        return false;
    }

    if (ResumeThread(m_pendingProcessInfo.hThread) == static_cast<DWORD>(-1)) {
        LOG((CLOG_ERR "could not resume contained process %lu, error=%lu",
            m_pendingProcessInfo.dwProcessId, GetLastError()));
        return false;
    }
    return true;
}

bool
MSWindowsWatchdog::pendingActivationShouldAbort(
    const LaunchProfile& activationOwner,
    std::string& reason)
{
    const CommandState latestState = commandState();
    const bool monitoring = m_monitoring.load();
    const bool commandEmpty = latestState.command.empty();
    const bool ownerMatches = sameLaunchOwnerIdentity(
        latestState.launchProfile, activationOwner);
    const bool sessionChanged = monitoring && m_session.hasChanged() != FALSE;
    if (shouldAbortPendingActivation(
            monitoring, commandEmpty, ownerMatches, sessionChanged)) {
        if (!monitoring) {
            reason = "watchdog stop was requested";
        }
        else if (commandEmpty) {
            reason = "service command was stopped";
        }
        else if (!ownerMatches) {
            reason = "authenticated launch owner was replaced";
        }
        else {
            reason = "active console session changed";
        }
        return true;
    }

    std::string ownerReason;
    if (validateLaunchOwner(activationOwner, ownerReason) !=
        LaunchOwnerState::Ready) {
        reason = ownerReason.empty()
            ? "authenticated launch owner is unavailable"
            : ownerReason;
        return true;
    }

    reason.clear();
    return false;
}

IpcInputReadinessResult
MSWindowsWatchdog::waitForPendingInputReadiness(
    const PROCESS_INFORMATION& processInfo,
    UInt32 expectedSessionId,
    const std::string& expectedDesktopName,
    const char* readinessPhase,
    DesktopSwitchPolicy::ReadinessPhase readinessPolicyPhase,
    const LaunchProfile* activationOwner)
{
    IpcInputReadinessResult result;
    const double readyStart = ARCH->time();
    while (m_monitoring.load() &&
           ARCH->time() - readyStart < kProcessReadyTimeoutSeconds) {
        if (activationOwner != nullptr) {
            std::string abortReason;
            if (pendingActivationShouldAbort(
                    *activationOwner, abortReason)) {
                LOG((CLOG_INFO
                    "aborting process %lu %s readiness wait: %s",
                    processInfo.dwProcessId,
                    readinessPhase == nullptr ? "local" : readinessPhase,
                    abortReason.c_str()));
                return result;
            }
        }
        if (!isProcessHandleActive(processInfo.hProcess)) {
            break;
        }

        if (!m_ipcServer.hasClientProcess(
                kIpcClientNode, processInfo.dwProcessId)) {
            ARCH->sleep(kProcessReadyPollSeconds);
            continue;
        }

        std::uint64_t queryNonce = 0;
        if (!nextReadinessQueryNonce(queryNonce)) {
            LOG((CLOG_ERR
                "could not generate a secure input-readiness nonce for process %lu",
                processInfo.dwProcessId));
            return result;
        }
        IpcInputReadyQueryMessage query(queryNonce);
        bool querySent = false;
        try {
            querySent = m_ipcServer.sendToProcess(
                query, kIpcClientNode, processInfo.dwProcessId);
        }
        catch (const XBase& e) {
            LOG((CLOG_WARN
                "could not query process %lu %s input readiness: %s",
                processInfo.dwProcessId,
                readinessPhase == nullptr ? "local" : readinessPhase,
                e.what()));
        }
        if (!querySent) {
            ARCH->sleep(kProcessReadyPollSeconds);
            continue;
        }

        LOG((CLOG_DEBUG
            "challenging process %lu %s input readiness query=%llu",
            processInfo.dwProcessId,
            readinessPhase == nullptr ? "local" : readinessPhase,
            static_cast<unsigned long long>(queryNonce)));
        const double proofDeadline = ARCH->time() +
            kReadinessProofWaitSeconds;
        do {
            if (activationOwner != nullptr) {
                std::string abortReason;
                if (pendingActivationShouldAbort(
                        *activationOwner, abortReason)) {
                    LOG((CLOG_INFO
                        "aborting process %lu %s readiness proof: %s",
                        processInfo.dwProcessId,
                        readinessPhase == nullptr ? "local" : readinessPhase,
                        abortReason.c_str()));
                    return result;
                }
            }
            result = m_ipcServer.inputReadinessProof(
                kIpcClientNode, processInfo.dwProcessId,
                expectedSessionId, expectedDesktopName, kBuildId,
                queryNonce);
            if (result.match == IpcInputReadinessMatch::Exact ||
                (result.match ==
                     IpcInputReadinessMatch::DesktopMismatch &&
                 DesktopSwitchPolicy::shouldReturnDesktopMismatch(
                     readinessPolicyPhase))) {
                return result;
            }
            if (!m_monitoring.load() ||
                !isProcessHandleActive(processInfo.hProcess)) {
                return IpcInputReadinessResult();
            }
            ARCH->sleep(kProcessReadyPollSeconds);
        } while (ARCH->time() < proofDeadline &&
                 ARCH->time() - readyStart < kProcessReadyTimeoutSeconds);
    }
    return result;
}

IpcInputReadinessResult
MSWindowsWatchdog::activatePendingProcess(
    const PROCESS_INFORMATION& processInfo,
    UInt32 expectedSessionId,
    const std::string& expectedDesktopName,
    const LaunchProfile& activationOwner)
{
    if (!m_daemonized) {
        IpcInputReadinessResult readiness;
        readiness.match = IpcInputReadinessMatch::Exact;
        readiness.desktopName = expectedDesktopName;
        return readiness;
    }

    std::string abortReason;
    if (pendingActivationShouldAbort(activationOwner, abortReason)) {
        LOG((CLOG_INFO
            "not activating standby process %lu: %s",
            processInfo.dwProcessId, abortReason.c_str()));
        return IpcInputReadinessResult();
    }

    std::uint64_t activationNonce = 0;
    if (!nextReadinessQueryNonce(activationNonce)) {
        LOG((CLOG_ERR
            "could not generate a secure activation nonce for process %lu",
            processInfo.dwProcessId));
        return IpcInputReadinessResult();
    }
    bool activationSent = false;
    try {
        activationSent = m_ipcServer.sendActivateToProcess(
            processInfo.dwProcessId, activationNonce);
    }
    catch (const XBase& e) {
        LOG((CLOG_ERR "could not activate standby process %lu: %s",
            processInfo.dwProcessId, e.what()));
    }
    if (!activationSent) {
        LOG((CLOG_ERR
            "could not send activation nonce=%llu to standby process %lu",
            static_cast<unsigned long long>(activationNonce),
            processInfo.dwProcessId));
        return IpcInputReadinessResult();
    }

    LOG((CLOG_INFO
        "activating standby process %lu local data plane nonce=%llu after previous owner fence",
        processInfo.dwProcessId,
        static_cast<unsigned long long>(activationNonce)));
    const double activationStart = ARCH->time();
    while (m_monitoring.load() &&
           ARCH->time() - activationStart <
               kProcessActivationTimeoutSeconds) {
        if (pendingActivationShouldAbort(activationOwner, abortReason)) {
            LOG((CLOG_INFO
                "aborting standby process %lu activation wait: %s",
                processInfo.dwProcessId, abortReason.c_str()));
            return IpcInputReadinessResult();
        }
        if (!isProcessHandleActive(processInfo.hProcess)) {
            LOG((CLOG_ERR
                "standby process %lu exited before activation acknowledgement",
                processInfo.dwProcessId));
            return IpcInputReadinessResult();
        }
        if (m_ipcServer.hasActivatedClientProcess(
                processInfo.dwProcessId, activationNonce)) {
            LOG((CLOG_INFO
                "standby process %lu acknowledged local data-plane start nonce=%llu",
                processInfo.dwProcessId,
                static_cast<unsigned long long>(activationNonce)));
            // IACK proves that the process accepted local connect/listen
            // activation. A fresh readiness challenge below proves the active
            // local input backend; peer connectivity remains independently
            // recoverable and is deliberately not a publication prerequisite.
            const IpcInputReadinessResult readiness =
                waitForPendingInputReadiness(
                    processInfo, expectedSessionId, expectedDesktopName,
                    "active", DesktopSwitchPolicy::ReadinessPhase::Active,
                    &activationOwner);
            if (readiness.match == IpcInputReadinessMatch::Exact) {
                return readiness;
            }
            if (readiness.match ==
                    IpcInputReadinessMatch::DesktopMismatch) {
                LOG((CLOG_WARN
                    "activated process %lu proved unexpected desktop=%s expected=%s",
                    processInfo.dwProcessId,
                    readiness.desktopName.c_str(),
                    expectedDesktopName.c_str()));
            }
            return readiness;
        }
        ARCH->sleep(kProcessReadyPollSeconds);
    }

    LOG((CLOG_ERR
        "standby process %lu did not acknowledge activation nonce=%llu within %.1f seconds",
        processInfo.dwProcessId,
        static_cast<unsigned long long>(activationNonce),
        kProcessActivationTimeoutSeconds));
    return IpcInputReadinessResult();
}

HANDLE
MSWindowsWatchdog::duplicateProcessToken(HANDLE process, LPSECURITY_ATTRIBUTES security)
{
    HANDLE sourceToken = NULL;

    BOOL tokenRet = OpenProcessToken(
        process,
        TOKEN_ASSIGN_PRIMARY | TOKEN_ALL_ACCESS,
        &sourceToken);

    if (!tokenRet) {
        LOG((CLOG_ERR "could not open token, process handle: %p", process));
        throw XArch(new XArchEvalWindows());
    }

    ScopedHandle sourceTokenHandle(sourceToken);
    LOG((CLOG_DEBUG "got token %p, duplicating", sourceTokenHandle.get()));

    HANDLE newToken = NULL;
    BOOL duplicateRet = DuplicateTokenEx(
        sourceTokenHandle.get(), TOKEN_ASSIGN_PRIMARY | TOKEN_ALL_ACCESS, security,
        SecurityImpersonation, TokenPrimary, &newToken);

    if (!duplicateRet) {
        LOG((CLOG_ERR "could not duplicate token %p", sourceTokenHandle.get()));
        throw XArch(new XArchEvalWindows());
    }

    LOG((CLOG_DEBUG "duplicated, new token: %p", newToken));
    return newToken;
}

HANDLE
MSWindowsWatchdog::getUserToken(
    LPSECURITY_ATTRIBUTES security,
    UInt8 elevateMode,
    bool autoElevated)
{
    const bool elevated = shouldElevateProcess(elevateMode);
    if (elevated || autoElevated) {
        LOG((CLOG_DEBUG "getting elevated token, %s",
            (elevated ? "elevation required" : "at login screen")));

        HANDLE process = NULL;
        if (!m_session.isProcessInSession("winlogon.exe", &process)) {
            throw XMSWindowsWatchdogError("cannot get user token without winlogon.exe");
        }

        ScopedHandle processHandle(process);
        if (!isValidHandle(processHandle.get())) {
            throw XMSWindowsWatchdogError("cannot open winlogon.exe process");
        }

        return duplicateProcessToken(processHandle.get(), security);
    } else {
        LOG((CLOG_DEBUG "getting non-elevated token"));
        return m_session.getUserToken(security);
    }
}

bool
MSWindowsWatchdog::shouldElevateProcess(UInt8 elevateMode) const
{
    return ElevationPolicy::shouldElevateProcess(elevateMode);
}

bool
MSWindowsWatchdog::shouldAutoElevate(UInt8 elevateMode, const std::string& desktopName) const
{
    return ElevationPolicy::shouldAutoElevate(elevateMode, desktopName);
}

MSWindowsWatchdog::CommandState
MSWindowsWatchdog::commandState() const
{
    CommandState state;
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        state.command = m_command;
        state.elevateMode = m_elevateMode;
        state.commandChanged = m_commandChanged;
        state.processFailures = m_processFailures;
        state.generation = m_commandGeneration;
        state.lastDesktopName = m_lastDesktopName;
        state.launchProfile = m_launchProfile;
        state.hasLaunchCandidate = m_hasLaunchCandidate;
        state.launchCandidate = m_launchCandidate;
    }

    if (m_autoDetectCommand) {
        state.command = autoDetectedCommand();
    }

    return state;
}

void
MSWindowsWatchdog::incrementProcessFailures()
{
    std::lock_guard<std::mutex> lock(m_commandMutex);
    ++m_processFailures;
}

void
MSWindowsWatchdog::deferLaunchForGeneration(unsigned long long generation)
{
    std::lock_guard<std::mutex> lock(m_commandMutex);
    m_processFailures = 0;
    if (m_commandGeneration == generation) {
        m_commandChanged = true;
    }
}

MSWindowsWatchdog::LaunchOwnerState
MSWindowsWatchdog::validateLaunchOwner(const LaunchProfile& launchProfile,
                                       std::string& reason)
{
    reason.clear();
    if (!launchProfile.authenticated || launchProfile.sessionId == 0 ||
        launchProfile.userSid.empty() || launchProfile.profileDirectory.empty() ||
        launchProfile.generation.empty() || launchProfile.digest.empty()) {
        reason = "service command has no complete authenticated launch profile";
        return LaunchOwnerState::Invalid;
    }

    PSID parsedSid = nullptr;
    if (!ConvertStringSidToSidA(launchProfile.userSid.c_str(), &parsedSid) ||
        parsedSid == nullptr || !IsValidSid(parsedSid)) {
        if (parsedSid != nullptr) {
            LocalFree(parsedSid);
        }
        reason = "service command owner SID is invalid";
        return LaunchOwnerState::Invalid;
    }
    LocalFree(parsedSid);

    m_session.updateActiveSession();
    const UInt32 activeSessionId = m_session.getActiveSessionId();
    if (activeSessionId == 0xffffffffu ||
        launchProfile.sessionId != activeSessionId) {
        reason = "authenticated command owner is not the active console session";
        return LaunchOwnerState::TemporarilyInactive;
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
        reason = "cannot verify the authenticated command owner session state";
        return LaunchOwnerState::TemporarilyInactive;
    }
    const WTS_CONNECTSTATE_CLASS sessionState =
        *reinterpret_cast<const WTS_CONNECTSTATE_CLASS*>(rawState);
    WTSFreeMemory(rawState);
    if (sessionState != WTSActive) {
        reason = "authenticated command owner session is not active";
        return LaunchOwnerState::TemporarilyInactive;
    }

    HANDLE rawSessionToken = nullptr;
    if (!WTSQueryUserToken(activeSessionId, &rawSessionToken)) {
        reason = "cannot verify the authenticated command owner token";
        return LaunchOwnerState::TemporarilyInactive;
    }
    ScopedHandle sessionToken(rawSessionToken);
    if (!tokenMatchesUserSid(sessionToken.get(), launchProfile.userSid)) {
        reason = "authenticated command owner does not match the active user";
        return LaunchOwnerState::TemporarilyInactive;
    }

    return LaunchOwnerState::Ready;
}

void
MSWindowsWatchdog::rememberDesktopName(const std::string& desktopName)
{
    std::lock_guard<std::mutex> lock(m_commandMutex);
    m_lastDesktopName = desktopName;
    m_desktopRelaunchState.pendingDesktopName.clear();
    m_desktopRelaunchState.pendingSince = 0.0;
}

bool
MSWindowsWatchdog::shouldRelaunchForDesktopChange(const std::string& oldDesktop,
                                                  const std::string& newDesktop)
{
    DesktopSwitchPolicy::RelaunchDecision decision =
        DesktopSwitchPolicy::observeDesktop(
            m_desktopRelaunchState, oldDesktop, newDesktop, ARCH->time(),
            kDesktopRelaunchSettleSeconds, kDesktopRelaunchDebounceSeconds);

    if (decision.settling) {
        LOG((CLOG_DEBUG
            "active input desktop changed from %s to %s; waiting for stable desktop",
            oldDesktop.c_str(), newDesktop.c_str()));
        ARCH->sleep(kDesktopRelaunchSettleSeconds);
        if (!m_monitoring.load()) {
            return false;
        }

        std::string confirmedDesktop = activeDesktopName(false);
        if (confirmedDesktop != newDesktop) {
            if (!confirmedDesktop.empty()) {
                DesktopSwitchPolicy::observeDesktop(
                    m_desktopRelaunchState, oldDesktop, confirmedDesktop, ARCH->time(),
                    kDesktopRelaunchSettleSeconds, kDesktopRelaunchDebounceSeconds);
            }
            LOG((CLOG_INFO
                "active input desktop change from %s to %s did not settle; observed %s",
                oldDesktop.c_str(), newDesktop.c_str(),
                confirmedDesktop.empty() ? "<unavailable>" : confirmedDesktop.c_str()));
            return false;
        }

        decision = DesktopSwitchPolicy::observeDesktop(
            m_desktopRelaunchState, oldDesktop, confirmedDesktop, ARCH->time(),
            kDesktopRelaunchSettleSeconds, kDesktopRelaunchDebounceSeconds);
    }

    if (decision.debounced) {
        LOG((CLOG_WARN
            "active input desktop changed from %s to %s but relaunch is debounced",
            oldDesktop.c_str(), newDesktop.c_str()));
        return false;
    }

    return decision.relaunch;
}

void MSWindowsWatchdog::main_loop()
{
	SendSas sendSasFunc = NULL;
	ScopedModule sasLib(LoadLibraryW(L"sas.dll"));
	if (sasLib.get()) {
		LOG((CLOG_DEBUG "found sas.dll"));
		sendSasFunc = (SendSas)GetProcAddress(sasLib.get(), "SendSAS");
	}

    while (m_monitoring.load()) {
        try {
            CommandState state = commandState();

            if (state.command.empty() &&
                (isValidHandle(m_processInfo.hProcess) ||
                 isValidHandle(m_pendingProcessInfo.hProcess))) {
                LOG((CLOG_INFO
                    "managed process exists but command is empty, shutting down"));
                discardPendingProcessOrFailFast(
                    "stopped command left an unpublished process running",
                    3, true);
                if (!shutdownManagedProcess()) {
                    if (m_daemonized) {
                        failFastOwnedProcesses(
                            "stopped command left the managed process running");
                    }
                    throw XMSWindowsWatchdogError(
                        "stopped command process exit remains unconfirmed");
                }
                confirmStoppedGenerationIfReady(state.generation);
                continue;
            }

            if (state.command.empty()) {
                confirmStoppedGenerationIfReady(state.generation);
            }

            if (m_daemonized && !state.command.empty()) {
                std::string ownerReason;
                const LaunchOwnerState ownerState =
                    validateLaunchOwner(state.launchProfile, ownerReason);
                if (ownerState != LaunchOwnerState::Ready) {
                    if (m_processRunning.load()) {
                        LOG((CLOG_WARN
                            "pausing the managed service node because its authenticated owner is unavailable: %s",
                            ownerReason.c_str()));
                    }
                    else {
                        LOG((CLOG_DEBUG "deferring service node launch: %s",
                            ownerReason.c_str()));
                    }
                    discardPendingProcessOrFailFast(
                        "owner-unavailable launch left an unpublished process running",
                        3, true);
                    if (!shutdownManagedProcess(5)) {
                        failFastOwnedProcesses(
                            "owner-unavailable launch left the managed process running");
                    }
                    deferLaunchForGeneration(state.generation);
                    ARCH->sleep(1);
                    continue;
                }
            }

            if (state.processFailures != 0) {
                // increasing backoff period, maximum of 10 seconds.
                int timeout = (state.processFailures * 2) < 10 ? (state.processFailures * 2) : 10;
                LOG((CLOG_INFO "backing off, wait=%ds, failures=%d", timeout, state.processFailures));
                ARCH->sleep(timeout);
            }

            state = commandState();
            bool desktopChanged = false;
            if (!state.command.empty() &&
                ElevationPolicy::shouldRelaunchOnDesktopSwitch(state.elevateMode)) {
                std::string desktopName = activeDesktopName(false);
                if (!desktopName.empty() &&
                    !state.lastDesktopName.empty() &&
                    desktopName != state.lastDesktopName) {
                    desktopChanged = shouldRelaunchForDesktopChange(
                        state.lastDesktopName, desktopName);
                    if (desktopChanged) {
                        LOG((CLOG_INFO "active input desktop changed from %s to %s; relaunching",
                            state.lastDesktopName.c_str(), desktopName.c_str()));
                    }
                }
                if (!desktopName.empty() &&
                    (state.lastDesktopName.empty() || desktopChanged)) {
                    rememberDesktopName(desktopName);
                }
            }

            if (!state.command.empty() &&
                ((state.processFailures != 0) || m_session.hasChanged() ||
                 state.commandChanged || desktopChanged)) {
                startProcess();
            }

            if (m_processRunning.load() &&
                isProcessHandleStopped(m_processInfo.hProcess)) {

                incrementProcessFailures();
                m_processRunning.store(false);

                LOG((CLOG_WARN "detected application not running, pid=%d",
                    m_processInfo.dwProcessId));
                closeProcessInfoHandles();
            }

            if (sendSasFunc != NULL) {

                HANDLE sendSasEvent = CreateEventW(
                    NULL, FALSE, FALSE, L"Global\\SendSAS");
                if (sendSasEvent != NULL) {

                    // use SendSAS event to wait for next session (timeout 1 second).
                    if (WaitForSingleObject(sendSasEvent, 1000) == WAIT_OBJECT_0) {
                        LOG((CLOG_DEBUG "calling SendSAS"));
                        sendSasFunc(FALSE);
                    }

                    CloseHandle(sendSasEvent);
                    continue;
                }
            }

            // if the sas event failed, wait by sleeping.
            ARCH->sleep(1);

        }
        catch (std::exception& e) {
            LOG((CLOG_ERR "failed to launch, error: %s", e.what()));
            incrementProcessFailures();
            if (isProcessHandleStopped(m_processInfo.hProcess)) {
                m_processRunning.store(false);
                closeProcessInfoHandles();
            }
            continue;
        }
        catch (...) {
            LOG((CLOG_ERR "failed to launch, unknown error."));
            incrementProcessFailures();
            if (isProcessHandleStopped(m_processInfo.hProcess)) {
                m_processRunning.store(false);
                closeProcessInfoHandles();
            }
            continue;
        }
    }

	if (isValidHandle(m_pendingProcessInfo.hProcess)) {
        LOG((CLOG_DEBUG "terminating unpublished process on watchdog exit"));
        shutdownPendingProcess(3, true);
    }
	if (isValidHandle(m_processInfo.hProcess)) {
		LOG((CLOG_DEBUG "terminating managed process on watchdog exit"));
		shutdownManagedProcess();
	}

    // Closing this handle is the final service-exit fence. Windows terminates
    // every process assigned to the job even if graceful or targeted shutdown
    // could not be confirmed above.
    closeManagedProcessJob();
    if (isValidHandle(m_pendingProcessInfo.hProcess)) {
        shutdownPendingProcess(5, false);
    }
    if (isValidHandle(m_processInfo.hProcess)) {
        if (shutdownAndCloseProcess(m_processInfo, 5, false)) {
            m_processRunning.store(false);
        }
    }
	closeOutputPipeHandles();

	LOG((CLOG_DEBUG "watchdog main thread finished"));
}

bool
MSWindowsWatchdog::isProcessActive()
{
    // Only the watchdog thread owns and closes m_processInfo. Other threads
    // consume this published state instead of racing GetExitCodeProcess
    // against handle replacement or shutdown.
    return m_processRunning.load();
}

void
MSWindowsWatchdog::setFileLogOutputter(FileLogOutputter* outputter)
{
    std::lock_guard<std::mutex> lock(m_fileLogMutex);
    m_fileLogOutputter = outputter;
}

bool
MSWindowsWatchdog::startProcess()
{
    bool durableCandidateCommitted = false;
    try {
    CommandState state = commandState();
    if (state.command.empty()) {
        throw XMSWindowsWatchdogError("cannot start process, command is empty");
    }

    if (isValidHandle(m_pendingProcessInfo.hProcess)) {
        LOG((CLOG_WARN
            "previous unpublished process %lu is still owned; fencing it before another launch",
            m_pendingProcessInfo.dwProcessId));
        discardPendingProcessOrFailFast(
            "previous unpublished process exit remains unconfirmed", 3, true);
    }
    else {
        closeProcessInfo(m_pendingProcessInfo);
    }

    UInt32 expectedSessionId = 0;
    if (m_daemonized) {
        std::string ownerReason;
        if (validateLaunchOwner(state.launchProfile, ownerReason) !=
            LaunchOwnerState::Ready) {
            LOG((CLOG_INFO "deferring service node launch: %s",
                ownerReason.c_str()));
            if (!shutdownManagedProcess(5)) {
                failFastOwnedProcesses(
                    "invalid launch owner left the managed process running");
            }
            deferLaunchForGeneration(state.generation);
            return false;
        }
        expectedSessionId = state.launchProfile.sessionId;
    }
    else {
        m_session.updateActiveSession();
        expectedSessionId = m_session.getActiveSessionId();
    }

    std::string command;
    std::string desktopName;
    std::string desktopEvidence;
    bool autoElevated = false;
    bool retargetAlreadyAttempted = false;
    PROCESS_INFORMATION& newProcessInfo = m_pendingProcessInfo;
    for (;;) {
        command = state.command;
        autoElevated = false;
        DWORD desktopError = ERROR_SUCCESS;
        const std::string observedDesktopName = m_daemonized
            ? activeDesktopName(false, &desktopError)
            : activeDesktopNameWithRetry(m_monitoring);
        const DesktopSwitchPolicy::LaunchTarget launchTarget =
            DesktopSwitchPolicy::resolveLaunchTarget(
                observedDesktopName, desktopEvidence, m_daemonized);
        desktopName = launchTarget.desktopName;
        if (desktopName.empty()) {
            throw XMSWindowsWatchdogError(
                "active input desktop is unavailable; delaying relaunch");
        }
        if (launchTarget.purpose ==
                DesktopSwitchPolicy::LaunchPurpose::Discovery) {
            LOG((CLOG_WARN
                "active input desktop is unavailable to the service, error=%lu; starting a non-activatable discovery process on %s",
                desktopError, desktopName.c_str()));
        }

        BOOL createRet = FALSE;
        ZeroMemory(&newProcessInfo, sizeof(PROCESS_INFORMATION));
        if (!m_daemonized) {
            createRet = doStartProcessAsSelf(
                command, desktopName, newProcessInfo);
        }
        else {
            autoElevated =
                shouldAutoElevate(state.elevateMode, desktopName) ||
                (launchTarget.purpose ==
                     DesktopSwitchPolicy::LaunchPurpose::Discovery &&
                 ElevationPolicy::shouldElevateDesktopDiscovery(
                     state.elevateMode));
            if (shouldElevateProcess(state.elevateMode) || autoElevated) {
                std::string restrictedCommand;
                std::string restrictionError;
                if (!IpcCommandValidator::restrictElevatedDesktopCommand(
                        command, restrictedCommand, &restrictionError)) {
                    throw XMSWindowsWatchdogError(
                        "refusing unsafe elevated desktop command: " +
                        restrictionError);
                }
                if (restrictedCommand != command) {
                    LOG((CLOG_WARN
                        "disabled file capabilities for elevated desktop launch"));
                    command = restrictedCommand;
                }

                std::string profiledCommand;
                std::string profileError;
                if (!state.launchProfile.authenticated ||
                    !IpcCommandValidator::appendTrustedProfileDirectory(
                        command, state.launchProfile.profileDirectory,
                        profiledCommand, &profileError)) {
                    throw XMSWindowsWatchdogError(
                        "refusing unsafe service launch profile: " +
                        profileError);
                }
                command = profiledCommand;
            }

            SECURITY_ATTRIBUTES sa;
            ZeroMemory(&sa, sizeof(SECURITY_ATTRIBUTES));
            sa.nLength = sizeof(SECURITY_ATTRIBUTES);
            sa.bInheritHandle = TRUE;
            sa.lpSecurityDescriptor = NULL;
            ScopedHandle userToken(
                getUserToken(&sa, state.elevateMode, autoElevated));

            DWORD uiAccess = 1;
            const bool uiAccessEnabled = SetTokenInformation(
                userToken.get(), TokenUIAccess, &uiAccess,
                sizeof(DWORD)) != FALSE;
            const DWORD uiAccessError = uiAccessEnabled
                ? ERROR_SUCCESS : GetLastError();
            const bool secureDesktop = _stricmp(
                desktopName.c_str(), "Default") != 0;
            if (!serviceLaunchInputCapabilityReady(
                    secureDesktop, uiAccessEnabled)) {
                LOG((CLOG_ERR
                    "refusing Windows input helper launch without UIAccess, desktop=%s error=%lu",
                    desktopName.c_str(), uiAccessError));
                throw XMSWindowsWatchdogError(
                    "privileged input capability is unavailable");
            }

            command = makeStandbyLaunchCommand(command);
            if (command.empty()) {
                throw XMSWindowsWatchdogError(
                    "could not derive a safe service standby command");
            }
            createRet = doStartProcessAsUser(
                command, userToken.release(), &sa, desktopName,
                newProcessInfo);
        }

        if (!createRet) {
            LOG((CLOG_ERR "could not launch"));
            closeProcessInfo(newProcessInfo);
            throw XArch(new XArchEvalWindows);
        }

        if (m_daemonized &&
            (!assignPendingProcessToJob() || !resumePendingProcess())) {
            const DWORD processId = newProcessInfo.dwProcessId;
            discardPendingProcessOrFailFast(
                "failed service process remains owned after containment failure",
                0, false);
            LOG((CLOG_ERR
                "discarded service process %lu after containment failure",
                processId));
            throw XMSWindowsWatchdogError(
                "service process containment or resume failed");
        }

        IpcInputReadinessResult readiness;
        if (!m_daemonized) {
            // Foreground relaunches do not use daemon IPC. Preserve the startup
            // crash observation window before adopting the process.
            ARCH->sleep(1);
            if (isProcessHandleActive(newProcessInfo.hProcess)) {
                readiness.match = IpcInputReadinessMatch::Exact;
                readiness.desktopName = desktopName;
            }
        }
        else {
            readiness = waitForPendingInputReadiness(
                newProcessInfo, expectedSessionId, desktopName,
                "standby", DesktopSwitchPolicy::ReadinessPhase::Standby);
        }

        if (readiness.match == IpcInputReadinessMatch::None) {
            if (m_daemonized) {
                std::string ownerReason;
                if (validateLaunchOwner(state.launchProfile, ownerReason) !=
                    LaunchOwnerState::Ready) {
                    LOG((CLOG_INFO
                        "discarding process %lu because its authenticated owner changed while readiness was pending: %s",
                        newProcessInfo.dwProcessId, ownerReason.c_str()));
                    discardPendingProcessOrFailFast(
                        "unready service process could not be discarded after owner change",
                        3, true);
                    deferLaunchForGeneration(state.generation);
                    return false;
                }
            }
            LOG((CLOG_ERR
                "process %lu did not prove local input readiness for session=%lu desktop=%s within %.1f seconds",
                newProcessInfo.dwProcessId,
                static_cast<unsigned long>(expectedSessionId),
                launchTarget.expectedDesktopKnown
                    ? desktopName.c_str() : "<service-unavailable>",
                kProcessReadyTimeoutSeconds));
            discardPendingProcessOrFailFast(
                "process without an input readiness proof could not be discarded",
                3, true);
            throw XMSWindowsWatchdogError("process did not become ready");
        }

        if (!readiness.desktopName.empty()) {
            desktopName = readiness.desktopName;
        }

        const CommandState latestState = commandState();
        const bool latestCandidateMatches =
            latestState.hasLaunchCandidate == state.hasLaunchCandidate &&
            (!state.hasLaunchCandidate || sameServiceLaunchCandidate(
                latestState.launchCandidate, state.launchCandidate));
        if (latestState.generation != state.generation ||
            latestState.command != state.command ||
            latestState.elevateMode != state.elevateMode ||
            !sameLaunchIdentity(latestState.launchProfile,
                                state.launchProfile) ||
            !latestCandidateMatches) {
            LOG((CLOG_INFO
                "discarding ready process %lu because its launch generation was superseded",
                newProcessInfo.dwProcessId));
            discardPendingProcessOrFailFast(
                "superseded ready process could not be discarded", 3, true);
            return false;
        }

        if (m_daemonized) {
            std::string ownerReason;
            if (validateLaunchOwner(state.launchProfile, ownerReason) !=
                LaunchOwnerState::Ready) {
                LOG((CLOG_INFO
                    "discarding ready process %lu because its authenticated owner changed before commit: %s",
                    newProcessInfo.dwProcessId, ownerReason.c_str()));
                discardPendingProcessOrFailFast(
                    "ready process could not be discarded after owner change",
                    3, true);
                deferLaunchForGeneration(state.generation);
                return false;
            }
        }

        const DesktopSwitchPolicy::DesktopRetargetDecision retargetDecision =
            DesktopSwitchPolicy::decideDesktopRetarget(
                launchTarget.purpose,
                readiness.match ==
                    IpcInputReadinessMatch::DesktopMismatch,
                retargetAlreadyAttempted);
        if (retargetDecision ==
                DesktopSwitchPolicy::DesktopRetargetDecision::Keep) {
            break;
        }

        const DWORD probeProcessId = newProcessInfo.dwProcessId;
        const std::string discoveredDesktopName = desktopName;
        LOG((CLOG_INFO
            "desktop probe process %lu proved session=%lu desktop=%s; discarding it before any commit, fence, activation, or publication",
            probeProcessId,
            static_cast<unsigned long>(expectedSessionId),
            discoveredDesktopName.c_str()));
        discardPendingProcessOrFailFast(
            "desktop probe process could not be discarded before exact relaunch",
            3, true);

        if (retargetDecision ==
                DesktopSwitchPolicy::DesktopRetargetDecision::Backoff) {
            LOG((CLOG_WARN
                "desktop changed again during exact readiness; applying launch-failure backoff before a fresh discovery transaction"));
            throw XMSWindowsWatchdogError(
                "desktop remained unstable during exact readiness");
        }

        desktopEvidence = discoveredDesktopName;
        retargetAlreadyAttempted = true;
        LOG((CLOG_INFO
            "desktop probe process %lu exited; launching an exact standby for desktop=%s in the same generation=%llu",
            probeProcessId, desktopEvidence.c_str(),
            state.generation));
    }

        std::string reportedDesktopName = desktopName;
        ServiceLaunchCommitResult launchCommit =
            ServiceLaunchCommitResult::kCommitted;
        if (state.hasLaunchCandidate) {
            LaunchReadyCallback readyCallback;
            {
                std::lock_guard<std::mutex> lock(m_commandMutex);
                readyCallback = m_launchReadyCallback;
            }
            if (!readyCallback) {
                launchCommit = ServiceLaunchCommitResult::kRejected;
            }
            else {
                try {
                    launchCommit = readyCallback(state.launchCandidate);
                }
                catch (const std::exception& e) {
                    LOG((CLOG_ERR
                        "service launch durable commit callback failed: %s",
                        e.what()));
                    launchCommit = ServiceLaunchCommitResult::kRejected;
                }
                catch (...) {
                    LOG((CLOG_ERR
                        "service launch durable commit callback failed"));
                    launchCommit = ServiceLaunchCommitResult::kRejected;
                }
            }
            if (decideServiceLaunchOwnership(launchCommit, false) ==
                ServiceLaunchOwnershipDecision::kKeepPrevious) {
                LOG((CLOG_ERR
                    "discarding ready process %lu because durable launch commit did not complete, result=%d",
                    newProcessInfo.dwProcessId,
                    static_cast<int>(launchCommit)));
                discardPendingProcessOrFailFast(
                    "uncommitted ready process could not be discarded", 3, true);
                return false;
            }
            if (launchCommit == ServiceLaunchCommitResult::kIndeterminate) {
                failFastOwnedProcesses(
                    "service launch durable commit result is indeterminate");
            }
            durableCandidateCommitted = true;
        }

        if (isValidHandle(m_processInfo.hProcess)) {
            LOG((CLOG_INFO
                "fencing previous process %lu before publishing ready replacement %lu",
                m_processInfo.dwProcessId, newProcessInfo.dwProcessId));
            if (!shutdownAndCloseProcess(m_processInfo, 20, true)) {
                m_processRunning.store(true);
                if (state.hasLaunchCandidate &&
                    decideServiceLaunchOwnership(launchCommit, false) ==
                        ServiceLaunchOwnershipDecision::kFailFast) {
                    failFastOwnedProcesses(
                        "durable launch committed but previous process exit is unconfirmed");
                }
                discardPendingProcessOrFailFast(
                    "ready replacement could not be discarded after previous process fence failure",
                    3, true);
                throw XMSWindowsWatchdogError(
                    "previous process did not cross the replacement fence");
            }
            m_processRunning.store(false);
        }
        else {
            closeProcessInfoHandles();
            m_processRunning.store(false);
        }

        if (!isProcessHandleActive(newProcessInfo.hProcess)) {
            if (state.hasLaunchCandidate) {
                failFastOwnedProcesses(
                    "durable launch committed but ready replacement exited before publication");
            }
            discardPendingProcessOrFailFast(
                "exited ready replacement could not be released", 0, false);
            throw XMSWindowsWatchdogError(
                "ready replacement exited before ownership commit");
        }

        if (m_daemonized) {
            std::string ownerReason;
            if (validateLaunchOwner(state.launchProfile, ownerReason) !=
                LaunchOwnerState::Ready) {
                LOG((CLOG_INFO
                    "discarding ready process %lu because its owner changed while fencing the previous process: %s",
                    newProcessInfo.dwProcessId, ownerReason.c_str()));
                if (state.hasLaunchCandidate) {
                    failFastOwnedProcesses(
                        "durable launch committed but authenticated owner changed before publication");
                }
                discardPendingProcessOrFailFast(
                    "ready replacement could not be discarded after owner change",
                    3, true);
                deferLaunchForGeneration(state.generation);
                return false;
            }
        }

        if (m_daemonized) {
            bool activationSuperseded = false;
            {
                std::lock_guard<std::mutex> lock(m_commandMutex);
                activationSuperseded = m_command.empty() ||
                    !sameLaunchOwnerIdentity(
                        m_launchProfile, state.launchProfile);
            }
            if (activationSuperseded) {
                LOG((CLOG_INFO
                    "discarding standby process %lu because a proven stop or owner replacement superseded activation",
                    newProcessInfo.dwProcessId));
                durableCandidateCommitted = false;
                discardPendingProcessOrFailFast(
                    "superseded standby process could not be discarded",
                    3, true);
                return false;
            }

            const IpcInputReadinessResult activationReadiness =
                activatePendingProcess(
                    newProcessInfo, expectedSessionId, desktopName,
                    state.launchProfile);
            if (activationReadiness.match ==
                    IpcInputReadinessMatch::DesktopMismatch) {
                const std::string observedDesktopName =
                    activeDesktopName(false);
                if (DesktopSwitchPolicy::canAdoptRetargetedActiveDesktop(
                        desktopName, activationReadiness.desktopName,
                        observedDesktopName)) {
                    reportedDesktopName = activationReadiness.desktopName;
                    LOG((CLOG_INFO
                        "adopting activated process %lu after desktop retarget expected=%s reported=%s observed=%s",
                        newProcessInfo.dwProcessId,
                        desktopName.c_str(),
                        activationReadiness.desktopName.c_str(),
                        observedDesktopName.c_str()));
                }
                else {
                    LOG((CLOG_WARN
                        "discarding activated process %lu after unconfirmed desktop retarget expected=%s reported=%s observed=%s",
                        newProcessInfo.dwProcessId,
                        desktopName.c_str(),
                        activationReadiness.desktopName.empty()
                            ? "<none>"
                            : activationReadiness.desktopName.c_str(),
                        observedDesktopName.empty()
                            ? "<unavailable>"
                            : observedDesktopName.c_str()));
                    discardPendingProcessOrFailFast(
                        "desktop-retargeted activated process could not be discarded",
                        3, true);
                    durableCandidateCommitted = false;
                    if (state.hasLaunchCandidate) {
                        std::lock_guard<std::mutex> lock(m_commandMutex);
                        if (m_hasLaunchCandidate &&
                            sameServiceLaunchCandidate(
                                m_launchCandidate,
                                state.launchCandidate)) {
                            m_hasLaunchCandidate = false;
                            m_launchCandidate = ServiceLaunchCandidate();
                        }
                    }
                    const CommandState retryState = commandState();
                    if (m_monitoring.load() &&
                        !retryState.command.empty()) {
                        deferLaunchForGeneration(retryState.generation);
                    }
                    return false;
                }
            }
            else if (activationReadiness.match !=
                    IpcInputReadinessMatch::Exact) {
                const CommandState activationFailureState = commandState();
                const bool activationMonitoring = m_monitoring.load();
                const bool stopRequested = !activationMonitoring ||
                    activationFailureState.command.empty();
                const bool ownerReplaced = !stopRequested &&
                    !sameLaunchOwnerIdentity(
                        activationFailureState.launchProfile,
                        state.launchProfile);
                if (shouldDiscardFailedActivation(
                        activationMonitoring,
                        activationFailureState.command.empty(),
                        !ownerReplaced)) {
                    LOG((CLOG_INFO
                        "discarding standby process %lu after activation was superseded: %s",
                        newProcessInfo.dwProcessId,
                        stopRequested
                            ? "watchdog or service command stopped"
                            : "authenticated launch owner replaced"));
                    // Current may already name this candidate, but a proven
                    // stop/owner transition is authoritative and must not turn
                    // a normal ownership handoff into a service crash.
                    durableCandidateCommitted = false;
                    discardPendingProcessOrFailFast(
                        "superseded activated process could not be discarded",
                        3, true);
                    if (m_monitoring.load() &&
                        !activationFailureState.command.empty()) {
                        deferLaunchForGeneration(
                            activationFailureState.generation);
                    }
                    return false;
                }
                // Current already names B and A has crossed the process fence.
                // A normal retry could leave durable state and the live input
                // owner disagreeing, so restart the entire ownership domain.
                failFastOwnedProcesses(
                    "durable standby replacement failed local data-plane activation or active local input readiness");
            }

            std::string activatedOwnerReason;
            if (validateLaunchOwner(
                    state.launchProfile, activatedOwnerReason) !=
                LaunchOwnerState::Ready) {
                const CommandState ownerFailureState = commandState();
                const bool ownerFailureMonitoring = m_monitoring.load();
                const bool ownerFailureMatches = sameLaunchOwnerIdentity(
                    ownerFailureState.launchProfile, state.launchProfile);
                LOG((CLOG_WARN
                    "activated process %lu failed final authenticated owner validation before publication: %s",
                    newProcessInfo.dwProcessId,
                    activatedOwnerReason.c_str()));
                if (shouldDiscardFailedActivation(
                        ownerFailureMonitoring,
                        ownerFailureState.command.empty(),
                        ownerFailureMatches)) {
                    durableCandidateCommitted = false;
                    discardPendingProcessOrFailFast(
                        "superseded activated process could not be discarded after final owner validation",
                        3, true);
                    if (m_monitoring.load() &&
                        !ownerFailureState.command.empty()) {
                        deferLaunchForGeneration(ownerFailureState.generation);
                    }
                    return false;
                }
                failFastOwnedProcesses(
                    "durable activated replacement lost authenticated owner validity before publication");
            }
            if (!reportedDesktopName.empty()) {
                desktopName = reportedDesktopName;
            }
        }

        bool discardSupersededCandidate = false;
        bool failFastBeforePublication = false;
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            const bool commandMatches = m_autoDetectCommand ||
                m_command == state.command;
            const bool candidateMatches =
                m_hasLaunchCandidate == state.hasLaunchCandidate &&
                (!m_hasLaunchCandidate || sameServiceLaunchCandidate(
                    m_launchCandidate, state.launchCandidate));
            const bool stateStillCurrent =
                m_commandGeneration == state.generation &&
                commandMatches && m_elevateMode == state.elevateMode &&
                sameLaunchIdentity(m_launchProfile, state.launchProfile) &&
                candidateMatches;
            const bool ownerSupersededCandidate =
                !sameLaunchOwnerIdentity(m_launchProfile, state.launchProfile);
            const bool sessionSupersededCandidate = m_daemonized &&
                m_session.hasChanged() != FALSE;
            discardSupersededCandidate = shouldDiscardFailedActivation(
                m_monitoring.load(), m_command.empty(),
                !ownerSupersededCandidate);
            failFastBeforePublication = !discardSupersededCandidate &&
                sessionSupersededCandidate;
            if (!discardSupersededCandidate &&
                !failFastBeforePublication) {
                // A newer candidate for the same owner may already be queued.
                // Keep this durably committed B as last-good until C proves
                // ready, but never publish B after a proven stop or owner swap.
                m_processInfo = newProcessInfo;
                ZeroMemory(&m_pendingProcessInfo,
                           sizeof(PROCESS_INFORMATION));
                durableCandidateCommitted = false;
                m_processRunning.store(true);
                m_lastDesktopName = desktopName;
                if (stateStillCurrent) {
                    m_desktopRelaunchState.pendingDesktopName.clear();
                    m_desktopRelaunchState.pendingSince = 0.0;
                    m_processFailures = 0;
                    m_commandChanged = false;
                    if (state.hasLaunchCandidate) {
                        m_hasLaunchCandidate = false;
                        m_launchCandidate = ServiceLaunchCandidate();
                    }
                }
            }
        }

        if (failFastBeforePublication) {
            failFastOwnedProcesses(
                "active console session changed after final owner validation but before publication");
        }

        if (discardSupersededCandidate) {
            LOG((CLOG_INFO
                "discarding committed process %lu because a proven stop or owner replacement superseded publication",
                newProcessInfo.dwProcessId));
            // The newer command transition is authoritative. Do not let the
            // exception guard interpret intentional supersession as an
            // ambiguous failure of the earlier durable commit.
            durableCandidateCommitted = false;
            discardPendingProcessOrFailFast(
                "superseded committed process could not be discarded", 3, true);
            return false;
        }

        LOG((CLOG_INFO "published locally ready process, pid=%lu, session=%i, desktop=%s, "
            "generation=%llu, elevated=%s, command=%s",
            m_processInfo.dwProcessId, expectedSessionId, desktopName.c_str(),
            state.generation,
            (shouldElevateProcess(state.elevateMode) || autoElevated) ? "yes" : "no",
            command.c_str()));
        return true;
    }
    catch (...) {
        if (isValidHandle(m_pendingProcessInfo.hProcess)) {
            if (durableCandidateCommitted) {
                failFastOwnedProcesses(
                    "durable launch committed but replacement failed before publication");
            }
            discardPendingProcessOrFailFast(
                "unpublished process survived an exceptional launch path",
                3, true);
        }
        throw;
    }
}

void
MSWindowsWatchdog::closeProcessInfoHandles()
{
    closeProcessInfo(m_processInfo);
}

bool
MSWindowsWatchdog::shutdownManagedProcess(int timeout)
{
    if (!isValidHandle(m_processInfo.hProcess)) {
        closeProcessInfoHandles();
        m_processRunning.store(false);
        return true;
    }

    if (!shutdownAndCloseProcess(m_processInfo, timeout, true)) {
        // Unknown is treated as owned/running. Publishing false here would let
        // a replacement proceed while the old input node can still be alive.
        m_processRunning.store(true);
        return false;
    }
    m_processRunning.store(false);
    return true;
}

bool
MSWindowsWatchdog::shutdownPendingProcess(int timeout, bool notifyIpc)
{
    return shutdownAndCloseProcess(
        m_pendingProcessInfo, timeout, notifyIpc);
}

void
MSWindowsWatchdog::discardPendingProcessOrFailFast(
    const char* reason, int timeout, bool notifyIpc)
{
    if (shutdownPendingProcess(timeout, notifyIpc)) {
        return;
    }
    if (m_daemonized) {
        failFastOwnedProcesses(reason);
    }
    throw XMSWindowsWatchdogError(
        reason == nullptr
            ? "unpublished process exit remains unconfirmed"
            : reason);
}

void
MSWindowsWatchdog::closeOutputPipeHandles()
{
	std::lock_guard<std::mutex> lock(m_outputPipeMutex);
	if (isValidHandle(m_stdOutWrite)) {
		CloseHandle(m_stdOutWrite);
		m_stdOutWrite = NULL;
	}
	if (isValidHandle(m_stdOutRead)) {
		CloseHandle(m_stdOutRead);
		m_stdOutRead = NULL;
	}
}

void
MSWindowsWatchdog::createOutputPipeHandles()
{
	std::lock_guard<std::mutex> lock(m_outputPipeMutex);
	if (isValidHandle(m_stdOutRead) && isValidHandle(m_stdOutWrite)) {
		return;
	}

	if (isValidHandle(m_stdOutRead) || isValidHandle(m_stdOutWrite)) {
		if (isValidHandle(m_stdOutWrite)) {
			CloseHandle(m_stdOutWrite);
			m_stdOutWrite = NULL;
		}
		if (isValidHandle(m_stdOutRead)) {
			CloseHandle(m_stdOutRead);
			m_stdOutRead = NULL;
		}
	}

	SECURITY_ATTRIBUTES saAttr;
	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;

	if (!CreatePipe(&m_stdOutRead, &m_stdOutWrite, &saAttr, 0)) {
		m_stdOutRead = NULL;
		m_stdOutWrite = NULL;
		throw XArch(new XArchEvalWindows());
	}
	if (!SetHandleInformation(m_stdOutRead, HANDLE_FLAG_INHERIT, 0)) {
		CloseHandle(m_stdOutWrite);
		CloseHandle(m_stdOutRead);
		m_stdOutWrite = NULL;
		m_stdOutRead = NULL;
		throw XArch(new XArchEvalWindows());
	}
}

BOOL MSWindowsWatchdog::doStartProcessAsSelf(const std::string& command,
                                             const std::string& desktop,
                                             PROCESS_INFORMATION& processInfo)
{
    DWORD creationFlags =
        NORMAL_PRIORITY_CLASS |
        CREATE_NO_WINDOW |
        CREATE_UNICODE_ENVIRONMENT;
    if (m_daemonized) {
        creationFlags |= CREATE_SUSPENDED;
    }

    const std::string desktopPath = std::string("winsta0\\") + desktop;
    std::vector<WCHAR> wideDesktopPath = utf8_to_win_char(desktopPath);
    std::vector<WCHAR> commandLine = utf8_to_win_char(command);
    if (wideDesktopPath.size() <= 1u || commandLine.size() <= 1u) {
        SetLastError(ERROR_NO_UNICODE_TRANSLATION);
        LOG((CLOG_ERR
            "refusing process launch with invalid UTF-8 command or desktop"));
        return FALSE;
    }

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(STARTUPINFOW));
    si.cb = sizeof(STARTUPINFOW);
    si.lpDesktop = wideDesktopPath.data();
    si.dwFlags |= STARTF_USESTDHANDLES;

    LOG((CLOG_INFO "starting new process as self on desktop %s", desktopPath.c_str()));
    std::lock_guard<std::mutex> lock(m_outputPipeMutex);
    si.hStdError = m_stdOutWrite;
    si.hStdOutput = m_stdOutWrite;
    return CreateProcessW(
        NULL, commandLine.data(), NULL, NULL, TRUE, creationFlags,
        NULL, NULL, &si, &processInfo);
}

BOOL MSWindowsWatchdog::doStartProcessAsUser(const std::string& command, HANDLE userToken,
                                             LPSECURITY_ATTRIBUTES sa,
                                             const std::string& desktop,
                                             PROCESS_INFORMATION& processInfo)
{
    // clear, as we're reusing process info struct
    ZeroMemory(&processInfo, sizeof(PROCESS_INFORMATION));
    ScopedHandle userTokenHandle(userToken);

    const std::string desktopPath = std::string("winsta0\\") + desktop;
    std::vector<WCHAR> wideDesktopPath = utf8_to_win_char(desktopPath);
    std::vector<WCHAR> commandLine = utf8_to_win_char(command);
    if (wideDesktopPath.size() <= 1u || commandLine.size() <= 1u) {
        SetLastError(ERROR_NO_UNICODE_TRANSLATION);
        LOG((CLOG_ERR
            "refusing privileged process launch with invalid UTF-8 command or desktop"));
        return FALSE;
    }

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(STARTUPINFOW));
    si.cb = sizeof(STARTUPINFOW);
    si.lpDesktop = wideDesktopPath.data();
    si.dwFlags |= STARTF_USESTDHANDLES;

	ScopedEnvironmentBlock environment;
	BOOL blockRet = CreateEnvironmentBlock(environment.out(), userTokenHandle.get(), FALSE);
	if (!blockRet) {
		LOG((CLOG_ERR "could not create environment block"));
		throw XArch(new XArchEvalWindows);
    }

    DWORD creationFlags =
        NORMAL_PRIORITY_CLASS |
        CREATE_NO_WINDOW |
        CREATE_UNICODE_ENVIRONMENT;
    if (m_daemonized) {
        creationFlags |= CREATE_SUSPENDED;
    }

    // re-launch in current active user session
    LOG((CLOG_INFO "starting new process as privileged user on desktop %s", desktopPath.c_str()));
    BOOL createRet = FALSE;
    {
        std::lock_guard<std::mutex> lock(m_outputPipeMutex);
        si.hStdError = m_stdOutWrite;
        si.hStdOutput = m_stdOutWrite;
		createRet = CreateProcessAsUserW(
			userTokenHandle.get(), NULL, commandLine.data(),
			sa, NULL, TRUE, creationFlags,
			environment.get(), NULL, &si, &processInfo);
	}

	return createRet;
}

bool
MSWindowsWatchdog::setCommand(const std::string& command, UInt8 elevateMode)
{
    return setCommandInternal(
        command, elevateMode, LaunchProfile(), nullptr);
}

bool
MSWindowsWatchdog::setCommand(const std::string& command, UInt8 elevateMode,
                              const LaunchProfile& launchProfile)
{
    return setCommandInternal(
        command, elevateMode, launchProfile, nullptr);
}

bool
MSWindowsWatchdog::setCommand(const std::string& command, UInt8 elevateMode,
                              const LaunchProfile& launchProfile,
                              const ServiceLaunchCandidate& candidate)
{
    return setCommandInternal(command, elevateMode, launchProfile, &candidate);
}

bool
MSWindowsWatchdog::requestStop(unsigned long long& commandGeneration)
{
    return setCommandInternal(
        std::string(), IpcCommandMessage::kElevateAsNeeded,
        LaunchProfile(), nullptr, &commandGeneration);
}

bool
MSWindowsWatchdog::setCommandInternal(
    const std::string& command, UInt8 elevateMode,
    const LaunchProfile& launchProfile,
    const ServiceLaunchCandidate* candidate,
    unsigned long long* acceptedGeneration)
{
    std::lock_guard<std::mutex> lock(m_commandMutex);
    const UInt8 normalizedMode = ElevationPolicy::normalizeMode(elevateMode);
    LaunchProfile normalizedProfile = launchProfile;
    if (command.empty()) {
        normalizedProfile = LaunchProfile();
        candidate = nullptr;
    }
    if (!isExternalCommandAccepted(command)) {
        LOG((CLOG_ERR
            "refusing externally supplied internal service standby option"));
        return false;
    }
    if (candidate != nullptr && !serviceLaunchCandidateMatchesCommandProfile(
            *candidate, command, normalizedMode, normalizedProfile.sessionId,
            normalizedProfile.userSid, normalizedProfile.generation,
            normalizedProfile.digest)) {
        LOG((CLOG_ERR
            "refusing service launch candidate that does not match its command profile"));
        return false;
    }
    const bool sameCandidate =
        m_hasLaunchCandidate == (candidate != nullptr) &&
        (candidate == nullptr || sameServiceLaunchCandidate(
            m_launchCandidate, *candidate));
    if (!ElevationPolicy::commandRequiresRelaunch(
            m_command, m_elevateMode, command, normalizedMode) &&
        sameLaunchIdentity(m_launchProfile, normalizedProfile) &&
        sameCandidate) {
        m_launchProfile = normalizedProfile;
        if (acceptedGeneration != nullptr) {
            *acceptedGeneration = m_commandGeneration;
        }
        LOG((CLOG_DEBUG "service command unchanged; keeping process generation=%llu",
            m_commandGeneration));
        return true;
    }

    LOG((CLOG_INFO "service command updated; scheduling process replacement"));
    m_command = command;
    m_elevateMode = normalizedMode;
    m_launchProfile = normalizedProfile;
    m_hasLaunchCandidate = candidate != nullptr;
    m_launchCandidate = candidate == nullptr
        ? ServiceLaunchCandidate() : *candidate;
    m_commandChanged = true;
    m_processFailures = 0;
    ++m_commandGeneration;
    if (acceptedGeneration != nullptr) {
        *acceptedGeneration = m_commandGeneration;
    }
    return true;
}

void
MSWindowsWatchdog::setLaunchReadyCallback(
    const LaunchReadyCallback& callback)
{
    std::lock_guard<std::mutex> lock(m_commandMutex);
    m_launchReadyCallback = callback;
}

void
MSWindowsWatchdog::setStopCompletedCallback(
    const StopCompletedCallback& callback)
{
    std::lock_guard<std::mutex> lock(m_commandMutex);
    m_stopCompletedCallback = callback;
}

bool
MSWindowsWatchdog::isStopConfirmed(
    unsigned long long commandGeneration) const
{
    std::lock_guard<std::mutex> lock(m_commandMutex);
    return m_command.empty() &&
        m_commandGeneration == commandGeneration &&
        m_hasConfirmedStopGeneration.load(std::memory_order_acquire) &&
        m_confirmedStopGeneration.load(std::memory_order_acquire) ==
            commandGeneration;
}

void
MSWindowsWatchdog::confirmStoppedGenerationIfReady(
    unsigned long long commandGeneration)
{
    StopCompletedCallback callback;
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        if (!stopConfirmationReady(
                m_command.empty(),
                isValidHandle(m_processInfo.hProcess),
                isValidHandle(m_pendingProcessInfo.hProcess)) ||
            m_commandGeneration != commandGeneration ||
            (m_hasConfirmedStopGeneration.load(std::memory_order_relaxed) &&
             m_confirmedStopGeneration.load(std::memory_order_relaxed) ==
                commandGeneration)) {
            return;
        }

        m_confirmedStopGeneration.store(
            commandGeneration, std::memory_order_release);
        m_hasConfirmedStopGeneration.store(true, std::memory_order_release);
        callback = m_stopCompletedCallback;
    }

    LOG((CLOG_INFO "confirmed stopped service command generation=%llu",
         commandGeneration));
    if (callback) {
        try {
            callback(commandGeneration);
        }
        catch (const std::exception& e) {
            LOG((CLOG_ERR
                "service stop completion callback failed for generation=%llu: %s",
                commandGeneration, e.what()));
        }
        catch (...) {
            LOG((CLOG_ERR
                "service stop completion callback failed for generation=%llu",
                commandGeneration));
        }
    }
}

std::string
MSWindowsWatchdog::getCommand() const
{
    if (!m_autoDetectCommand) {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        return m_command;
    }

    return autoDetectedCommand();
}

std::string
MSWindowsWatchdog::autoDetectedCommand() const
{
    // seems like a fairly convoluted way to get the process name
    const char* launchName = App::instance().argsBase().m_exename.c_str();
    std::string args = ARCH->commandLine();

    // build up a full command line
    std::stringstream cmdTemp;
    cmdTemp << launchName << args;

    std::string cmd = cmdTemp.str();

    size_t i;
    std::string find = "--relaunch";
    while ((i = cmd.find(find)) != std::string::npos) {
        cmd.replace(i, find.length(), "");
    }

    return cmd;
}

void MSWindowsWatchdog::output_loop()
{
    // +1 char for \0
    CHAR buffer[kOutputBufferSize + 1];

    while (m_monitoring.load()) {

        DWORD bytesAvailable = 0;
        DWORD bytesRead = 0;
        BOOL success = FALSE;
        {
            std::lock_guard<std::mutex> lock(m_outputPipeMutex);
            if (isValidHandle(m_stdOutRead) &&
                PeekNamedPipe(m_stdOutRead, NULL, 0, NULL, &bytesAvailable, NULL) &&
                bytesAvailable > 0) {
                const DWORD bytesToRead =
                    bytesAvailable < kOutputBufferSize ? bytesAvailable : kOutputBufferSize;
                success = ReadFile(m_stdOutRead, buffer, bytesToRead, &bytesRead, NULL);
            }
        }

        // assume the process has gone away? slow down
        // the reads until another one turns up.
        if (!success || bytesRead == 0) {
            ARCH->sleep(0.1);
        }
        else {
            buffer[bytesRead] = '\0';
            m_ipcLogOutputter.write(kINFO, buffer);
            {
                std::lock_guard<std::mutex> lock(m_fileLogMutex);
                if (m_fileLogOutputter != NULL) {
                    m_fileLogOutputter->write(kINFO, buffer);
                }
            }
        }
    }
}

bool
MSWindowsWatchdog::shutdownProcess(HANDLE handle, DWORD pid, int timeout, bool notifyIpc,
                                   UInt32 notifyProcessId)
{
    if (!isValidHandle(handle)) {
        return false;
    }

    DWORD waitResult = WaitForSingleObject(handle, 0);
    if (waitResult == WAIT_OBJECT_0) {
        return true;
    }
    if (waitResult == WAIT_FAILED) {
        LOG((CLOG_WARN "could not query process %lu before shutdown, error=%lu",
            pid, GetLastError()));
        return false;
    }

	if (notifyIpc) {
		IpcShutdownMessage shutdown;
		if (notifyProcessId != 0) {
			if (!m_ipcServer.sendToProcess(shutdown, kIpcClientNode, notifyProcessId)) {
				LOG((CLOG_WARN "no ipc node connection found for process %lu; waiting for exit",
					notifyProcessId));
			}
		}
		else {
			m_ipcServer.send(shutdown, kIpcClientNode);
		}
	}
	else {
			LOG((CLOG_DEBUG "not sending IPC shutdown while replacing process %lu", pid));
    }

    // wait for process to exit gracefully.
    const int normalizedTimeout = timeout > 0 ? timeout : 0;
    double start = ARCH->time();
    while (true) {
        waitResult = WaitForSingleObject(
            handle, normalizedTimeout == 0 ? 0 : 100);
        if (waitResult == WAIT_OBJECT_0) {
            // yay, we got a graceful shutdown. there should be no hook in use errors!
            LOG((CLOG_INFO "process %lu was shutdown gracefully", pid));
            return true;
        }
        if (waitResult == WAIT_FAILED) {
            LOG((CLOG_WARN "could not wait for process %lu shutdown, error=%lu",
                pid, GetLastError()));
            return false;
        }

        const double elapsed = ARCH->time() - start;
        if (normalizedTimeout == 0 || elapsed >= normalizedTimeout) {
            // Forceful termination is a last resort because hook DLLs can stay
            // loaded in other applications. Always wait for the process handle
            // to become signaled before releasing it.
            LOG((CLOG_WARN "shutdown timed out after %d secs, forcefully terminating",
                static_cast<int>(elapsed)));
            if (!TerminateProcess(handle, kExitSuccess)) {
                const DWORD error = GetLastError();
                if (WaitForSingleObject(handle, 0) == WAIT_OBJECT_0) {
                    return true;
                }
                LOG((CLOG_ERR "could not terminate process %lu, error=%lu",
                    pid, error));
                return false;
            }
            waitResult = WaitForSingleObject(handle, 5000);
            if (waitResult != WAIT_OBJECT_0) {
                LOG((CLOG_ERR "terminated process %lu did not signal within 5 seconds",
                    pid));
                return false;
            }
            return true;
        }
    }
}

bool
MSWindowsWatchdog::shutdownAndCloseProcess(PROCESS_INFORMATION& processInfo,
                                           int timeout, bool notifyIpc)
{
    const DWORD processId = processInfo.dwProcessId;
    if (!isValidHandle(processInfo.hProcess)) {
        closeProcessInfo(processInfo);
        return true;
    }

    bool confirmedStopped = false;
    try {
        confirmedStopped = shutdownProcess(
            processInfo.hProcess, processId, timeout, notifyIpc,
            notifyIpc ? processId : 0);
    }
    catch (const XBase& e) {
        LOG((CLOG_WARN
            "could not notify process %lu before shutdown: %s; continuing by PID",
            processId, e.what()));
        confirmedStopped = shutdownProcess(
            processInfo.hProcess, processId, timeout, false, 0);
    }
    catch (const std::exception& e) {
        LOG((CLOG_WARN
            "process %lu shutdown notification failed: %s; continuing by PID",
            processId, e.what()));
        confirmedStopped = shutdownProcess(
            processInfo.hProcess, processId, timeout, false, 0);
    }
    catch (...) {
        LOG((CLOG_WARN
            "process %lu shutdown notification failed; continuing by PID",
            processId));
        confirmedStopped = shutdownProcess(
            processInfo.hProcess, processId, timeout, false, 0);
    }

    if (!confirmedStopped) {
        LOG((CLOG_ERR
            "process %lu exit is unconfirmed; retaining ownership handles",
            processId));
        return false;
    }

    closeProcessInfo(processInfo);
    return true;
}

bool
MSWindowsWatchdog::shutdownOwnedProcessBeforeDeadline(
    PROCESS_INFORMATION& processInfo, double deadlineSeconds)
{
    const DWORD processId = processInfo.dwProcessId;
    if (!isValidHandle(processInfo.hProcess)) {
        closeProcessInfo(processInfo);
        return true;
    }

    DWORD waitResult = WaitForSingleObject(processInfo.hProcess, 0);
    if (waitResult == WAIT_OBJECT_0) {
        closeProcessInfo(processInfo);
        return true;
    }
    if (waitResult == WAIT_FAILED) {
        LOG((CLOG_ERR "could not query owned process %lu during final shutdown, error=%lu",
            processId, GetLastError()));
        return false;
    }

    if (!TerminateProcess(processInfo.hProcess, kExitFailed)) {
        const DWORD terminateError = GetLastError();
        if (WaitForSingleObject(processInfo.hProcess, 0) != WAIT_OBJECT_0) {
            LOG((CLOG_WARN "could not request termination of owned process %lu during final shutdown, error=%lu",
                processId, terminateError));
        }
    }

    const double waitSeconds = boundedShutdownWaitSeconds(
        deadlineSeconds, ARCH->time(), kWatchdogStopBudgetSeconds);
    const DWORD waitMilliseconds = static_cast<DWORD>(waitSeconds * 1000.0);
    waitResult = WaitForSingleObject(processInfo.hProcess, waitMilliseconds);
    if (waitResult == WAIT_FAILED) {
        LOG((CLOG_ERR "could not wait for owned process %lu during final shutdown, error=%lu",
            processId, GetLastError()));
        return false;
    }
    if (waitResult != WAIT_OBJECT_0) {
        LOG((CLOG_ERR "owned process %lu did not stop before the shared shutdown deadline",
            processId));
        return false;
    }

    closeProcessInfo(processInfo);
    return true;
}
