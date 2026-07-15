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
#include "ipc/Ipc.h"
#include "barrier/App.h"
#include "barrier/ArgsBase.h"
#include "mt/Thread.h"
#include "arch/win32/ArchDaemonWindows.h"
#include "arch/win32/XArchWindows.h"
#include "arch/Arch.h"
#include "base/log_outputters.h"
#include "base/Log.h"
#include "common/Version.h"

#include <sstream>
#include <UserEnv.h>
#include <Shellapi.h>
#include <Tlhelp32.h>
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
static const double kProcessReadyPollSeconds = 0.05;

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

}

std::string activeDesktopName(bool warnOnFailure = true, DWORD* errorOut = NULL)
{
    std::string name;
    DWORD error = ERROR_SUCCESS;
    HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (desk != NULL) {
        DWORD requiredBytes = 0;
        if (GetUserObjectInformationA(desk, UOI_NAME, NULL, 0, &requiredBytes) == FALSE &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            error = GetLastError();
        }
        else {
            std::vector<char> buffer(requiredBytes + 1, 0);
            if (GetUserObjectInformationA(
                    desk, UOI_NAME, buffer.data(),
                    static_cast<DWORD>(buffer.size()), NULL) == TRUE) {
                name = buffer.data();
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
    m_processFailures(0),
    m_processRunning(false),
    m_fileLogOutputter(NULL),
    m_daemonized(daemonized)
{
    ZeroMemory(&m_processInfo, sizeof(PROCESS_INFORMATION));
}

void
MSWindowsWatchdog::startAsync()
{
    m_monitoring.store(true);
    createOutputPipeHandles();
    m_thread = new Thread([this](){ main_loop(); });
    m_outputThread = new Thread([this](){ output_loop(); });
}

void
MSWindowsWatchdog::stop()
{
	m_monitoring.store(false);
	closeOutputPipeHandles();

	if (m_thread != NULL) {
		if (!m_thread->wait(5)) {
			LOG((CLOG_WARN "watchdog main thread did not stop; cancelling"));
			m_thread->cancel();
			m_thread->unblockPollSocket();
		}
		if (!m_thread->wait(30)) {
			LOG((CLOG_ERR "watchdog main thread is still running; waiting before destroying watchdog"));
			m_thread->wait();
		}
		delete m_thread;
		m_thread = NULL;
	}

	if (m_outputThread != NULL) {
		if (!m_outputThread->wait(5)) {
			LOG((CLOG_WARN "watchdog output thread did not stop; cancelling"));
			m_outputThread->cancel();
			m_outputThread->unblockPollSocket();
		}
		if (!m_outputThread->wait(30)) {
			LOG((CLOG_ERR "watchdog output thread is still running; waiting before destroying watchdog"));
			m_outputThread->wait();
		}
		delete m_outputThread;
		m_outputThread = NULL;
	}
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
MSWindowsWatchdog::clearLaunchStateForGeneration(unsigned long long generation)
{
    std::lock_guard<std::mutex> lock(m_commandMutex);
    m_processFailures = 0;
    if (m_commandGeneration == generation) {
        m_commandChanged = false;
    }
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
	shutdownExistingProcesses();

	SendSas sendSasFunc = NULL;
	ScopedModule sasLib(LoadLibraryA("sas.dll"));
	if (sasLib.get()) {
		LOG((CLOG_DEBUG "found sas.dll"));
		sendSasFunc = (SendSas)GetProcAddress(sasLib.get(), "SendSAS");
	}

    ZeroMemory(&m_processInfo, sizeof(PROCESS_INFORMATION));

    while (m_monitoring.load()) {
        try {
            CommandState state = commandState();

            if (m_processRunning && state.command.empty()) {
                LOG((CLOG_INFO "process started but command is empty, shutting down"));
                shutdownExistingProcesses();
                m_processRunning = false;
                continue;
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
                std::string desktopName = activeDesktopName();
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

            if (m_processRunning && !isProcessActive()) {

                incrementProcessFailures();
                m_processRunning = false;

                LOG((CLOG_WARN "detected application not running, pid=%d",
                    m_processInfo.dwProcessId));
                closeProcessInfoHandles();
            }

            if (sendSasFunc != NULL) {

                HANDLE sendSasEvent = CreateEventA(NULL, FALSE, FALSE, "Global\\SendSAS");
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
            if (!isProcessActive()) {
                m_processRunning = false;
                closeProcessInfoHandles();
            }
            continue;
        }
        catch (...) {
            LOG((CLOG_ERR "failed to launch, unknown error."));
            incrementProcessFailures();
            if (!isProcessActive()) {
                m_processRunning = false;
                closeProcessInfoHandles();
            }
            continue;
        }
    }

	if (m_processRunning) {
		LOG((CLOG_DEBUG "terminated running process on exit"));
		shutdownProcess(m_processInfo.hProcess, m_processInfo.dwProcessId, 20);
		closeProcessInfoHandles();
		m_processRunning = false;
	}
	closeOutputPipeHandles();

	LOG((CLOG_DEBUG "watchdog main thread finished"));
}

bool
MSWindowsWatchdog::isProcessActive()
{
	if (!m_processRunning || !isValidHandle(m_processInfo.hProcess)) {
		return false;
	}

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(m_processInfo.hProcess, &exitCode)) {
        LOG((CLOG_WARN "could not query process status, error=%lu", GetLastError()));
        return false;
    }
    return exitCode == STILL_ACTIVE;
}

void
MSWindowsWatchdog::setFileLogOutputter(FileLogOutputter* outputter)
{
    std::lock_guard<std::mutex> lock(m_fileLogMutex);
    m_fileLogOutputter = outputter;
}

void
MSWindowsWatchdog::startProcess()
{
    CommandState state = commandState();
    if (state.command.empty()) {
        throw XMSWindowsWatchdogError("cannot start process, command is empty");
    }

    std::string command = state.command;

    m_session.updateActiveSession();

    DWORD desktopError = ERROR_SUCCESS;
    const std::string observedDesktopName = m_daemonized
        ? activeDesktopName(false, &desktopError)
        : activeDesktopNameWithRetry(m_monitoring);
    std::string desktopName = DesktopSwitchPolicy::launchDesktopName(
        observedDesktopName, m_daemonized);
    if (observedDesktopName.empty() && !desktopName.empty()) {
        LOG((CLOG_WARN
            "active input desktop is unavailable to the service, error=%lu; "
            "launching on %s",
            desktopError, desktopName.c_str()));
    }
    if (desktopName.empty()) {
        throw XMSWindowsWatchdogError(
            "active input desktop is unavailable; delaying relaunch");
    }
    rememberDesktopName(desktopName);

    BOOL createRet;
    bool autoElevated = false;
    PROCESS_INFORMATION newProcessInfo;
    ZeroMemory(&newProcessInfo, sizeof(PROCESS_INFORMATION));
    if (!m_daemonized) {
        createRet = doStartProcessAsSelf(command, desktopName, newProcessInfo);
    } else {
        autoElevated = shouldAutoElevate(state.elevateMode, desktopName);

        SECURITY_ATTRIBUTES sa;
        ZeroMemory(&sa, sizeof(SECURITY_ATTRIBUTES));
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = NULL;
        HANDLE userToken = getUserToken(&sa, state.elevateMode, autoElevated);

        // patch by Jack Zhou and Henry Tung
        // set UIAccess to fix Windows 8 GUI interaction
        // http://symless.com/spit/issues/details/3338/#c70
        DWORD uiAccess = 1;
        if (!SetTokenInformation(userToken, TokenUIAccess, &uiAccess, sizeof(DWORD))) {
            DWORD error = GetLastError();
            if (desktopName != "Default") {
                LOG((CLOG_WARN
                    "could not enable UIAccess on secure desktop launch, error=%lu; "
                    "continuing in degraded mode to avoid relaunch loop",
                    error));
            }
            else {
                LOG((CLOG_WARN "could not enable UIAccess on default desktop launch, error=%lu; continuing",
                    error));
            }
        }

        createRet = doStartProcessAsUser(command, userToken, &sa, desktopName, newProcessInfo);
    }

    if (!createRet) {
        LOG((CLOG_ERR "could not launch"));
        closeProcessInfo(newProcessInfo);
        throw XArch(new XArchEvalWindows);
    }
    else {
        bool processReady = false;
        if (!m_daemonized) {
            // Foreground relaunches do not use daemon IPC. Preserve the startup
            // crash observation window before adopting the process.
            ARCH->sleep(1);
            processReady = isProcessHandleActive(newProcessInfo.hProcess);
        }
        else {
            const double readyStart = ARCH->time();
            do {
                if (!isProcessHandleActive(newProcessInfo.hProcess)) {
                    break;
                }

                processReady = m_ipcServer.hasReadyClientProcess(
                    kIpcClientNode, newProcessInfo.dwProcessId);
                if (processReady || !m_monitoring.load()) {
                    break;
                }

                ARCH->sleep(kProcessReadyPollSeconds);
            } while (ARCH->time() - readyStart < kProcessReadyTimeoutSeconds);
        }

        if (!processReady) {
            LOG((CLOG_ERR "process %lu did not complete IPC readiness within %.1f seconds",
                newProcessInfo.dwProcessId, kProcessReadyTimeoutSeconds));
            shutdownProcess(newProcessInfo.hProcess, newProcessInfo.dwProcessId, 3,
                            true, newProcessInfo.dwProcessId);
            closeProcessInfo(newProcessInfo);
            throw XMSWindowsWatchdogError("process did not become ready");
        }

        PROCESS_INFORMATION previousProcessInfo = m_processInfo;
        const bool hadRunningProcess = m_processRunning &&
            isValidHandle(previousProcessInfo.hProcess);

        m_processInfo = newProcessInfo;
        m_processRunning = true;
        clearLaunchStateForGeneration(state.generation);

	    if (hadRunningProcess) {
	        LOG((CLOG_DEBUG "closing previous process %lu after replacement launch succeeded",
	            previousProcessInfo.dwProcessId));
	        shutdownProcess(previousProcessInfo.hProcess, previousProcessInfo.dwProcessId, 20,
	                        true, previousProcessInfo.dwProcessId);
	        closeProcessInfo(previousProcessInfo);
	    }

        LOG((CLOG_INFO "started ready process, pid=%lu, session=%i, desktop=%s, "
            "generation=%llu, elevated=%s, command=%s",
            m_processInfo.dwProcessId, m_session.getActiveSessionId(), desktopName.c_str(),
            state.generation,
            (shouldElevateProcess(state.elevateMode) || autoElevated) ? "yes" : "no",
            command.c_str()));
    }
}

void
MSWindowsWatchdog::closeProcessInfoHandles()
{
    closeProcessInfo(m_processInfo);
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

BOOL MSWindowsWatchdog::doStartProcessAsSelf(std::string& command,
                                             const std::string& desktop,
                                             PROCESS_INFORMATION& processInfo)
{
    DWORD creationFlags =
        NORMAL_PRIORITY_CLASS |
        CREATE_NO_WINDOW |
        CREATE_UNICODE_ENVIRONMENT;

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(STARTUPINFOA));
    si.cb = sizeof(STARTUPINFOA);
    std::string desktopPath = std::string("winsta0\\") + desktop;
    si.lpDesktop = LPSTR(desktopPath.c_str());
    si.dwFlags |= STARTF_USESTDHANDLES;

    LOG((CLOG_INFO "starting new process as self on desktop %s", desktopPath.c_str()));
    std::lock_guard<std::mutex> lock(m_outputPipeMutex);
    si.hStdError = m_stdOutWrite;
    si.hStdOutput = m_stdOutWrite;
    std::vector<char> commandLine(command.begin(), command.end());
    commandLine.push_back('\0');
    return CreateProcessA(NULL, commandLine.data(), NULL, NULL, TRUE, creationFlags, NULL, NULL, &si, &processInfo);
}

BOOL MSWindowsWatchdog::doStartProcessAsUser(std::string& command, HANDLE userToken,
                                             LPSECURITY_ATTRIBUTES sa,
                                             const std::string& desktop,
                                             PROCESS_INFORMATION& processInfo)
{
    // clear, as we're reusing process info struct
    ZeroMemory(&processInfo, sizeof(PROCESS_INFORMATION));
    ScopedHandle userTokenHandle(userToken);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(STARTUPINFOA));
    si.cb = sizeof(STARTUPINFOA);
    std::string desktopPath = std::string("winsta0\\") + desktop;
    si.lpDesktop = LPSTR(desktopPath.c_str());
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

    // re-launch in current active user session
    LOG((CLOG_INFO "starting new process as privileged user on desktop %s", desktopPath.c_str()));
    BOOL createRet = FALSE;
    {
        std::lock_guard<std::mutex> lock(m_outputPipeMutex);
        si.hStdError = m_stdOutWrite;
        si.hStdOutput = m_stdOutWrite;
        std::vector<char> commandLine(command.begin(), command.end());
        commandLine.push_back('\0');
		createRet = CreateProcessAsUserA(
			userTokenHandle.get(), NULL, commandLine.data(),
			sa, NULL, TRUE, creationFlags,
			environment.get(), NULL, &si, &processInfo);
	}

	return createRet;
}

void
MSWindowsWatchdog::setCommand(const std::string& command, UInt8 elevateMode)
{
    std::lock_guard<std::mutex> lock(m_commandMutex);
    const UInt8 normalizedMode = ElevationPolicy::normalizeMode(elevateMode);
    if (!ElevationPolicy::commandRequiresRelaunch(
            m_command, m_elevateMode, command, normalizedMode)) {
        LOG((CLOG_DEBUG "service command unchanged; keeping process generation=%llu",
            m_commandGeneration));
        return;
    }

    LOG((CLOG_INFO "service command updated; scheduling process replacement"));
    m_command = command;
    m_elevateMode = normalizedMode;
    m_commandChanged = true;
    m_processFailures = 0;
    ++m_commandGeneration;
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

void
MSWindowsWatchdog::shutdownProcess(HANDLE handle, DWORD pid, int timeout, bool notifyIpc,
                                   UInt32 notifyProcessId)
{
	DWORD exitCode;
	GetExitCodeProcess(handle, &exitCode);
    if (exitCode != STILL_ACTIVE) {
        return;
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
		LOG((CLOG_DEBUG "not sending IPC shutdown while replacing process %d", pid));
    }

    // wait for process to exit gracefully.
    double start = ARCH->time();
    while (true) {

        GetExitCodeProcess(handle, &exitCode);
        if (exitCode != STILL_ACTIVE) {
            // yay, we got a graceful shutdown. there should be no hook in use errors!
            LOG((CLOG_INFO "process %d was shutdown gracefully", pid));
            break;
        }
        else {

            double elapsed = (ARCH->time() - start);
            if (elapsed > timeout) {
                // if timeout reached, kill forcefully.
                // calling TerminateProcess on barrier is very bad!
                // it causes the hook DLL to stay loaded in some apps,
                // making it impossible to start barrier again.
                LOG((CLOG_WARN "shutdown timed out after %d secs, forcefully terminating", (int)elapsed));
                TerminateProcess(handle, kExitSuccess);
                break;
            }

            ARCH->sleep(1);
        }
    }
}

void
MSWindowsWatchdog::shutdownExistingProcesses()
{
    // first we need to take a snapshot of the running processes
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        LOG((CLOG_ERR "could not get process snapshot"));
        throw XArch(new XArchEvalWindows);
    }
    ScopedHandle snapshotHandle(snapshot);

    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);

    // get the first process, and if we can't do that then it's
    // unlikely we can go any further
    BOOL gotEntry = Process32First(snapshot, &entry);
    if (!gotEntry) {
        LOG((CLOG_ERR "could not get first process entry"));
        throw XArch(new XArchEvalWindows);
    }

    // now just iterate until we can find winlogon.exe pid
    DWORD pid = 0;
    while (gotEntry) {

        // make sure we're not checking the system process
        if (entry.th32ProcessID != 0) {

            if (_stricmp(entry.szExeFile, "barrierc.exe") == 0 ||
                _stricmp(entry.szExeFile, "barriers.exe") == 0 ||
                _stricmp(entry.szExeFile, "weavec.exe") == 0 ||
                _stricmp(entry.szExeFile, "weaves.exe") == 0) {

                HANDLE handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, entry.th32ProcessID);
                ScopedHandle processHandle(handle);
                if (isValidHandle(processHandle.get())) {
                    shutdownProcess(processHandle.get(), entry.th32ProcessID, 10);
                }
            }
        }

        // now move on to the next entry (if we're not at the end)
        gotEntry = Process32Next(snapshot, &entry);
        if (!gotEntry) {

            DWORD err = GetLastError();
            if (err != ERROR_NO_MORE_FILES) {

                // only worry about error if it's not the end of the snapshot
                LOG((CLOG_ERR "could not get subsiquent process entry"));
                throw XArch(new XArchEvalWindows);
            }
        }
    }

    m_processRunning = false;
    closeProcessInfoHandles();
}
