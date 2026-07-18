/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
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

#include "arch/win32/ArchDaemonWindows.h"
#include "arch/win32/ArchMiscWindows.h"
#include "arch/win32/XArchWindows.h"
#include "arch/Arch.h"
#include "common/stdvector.h"
#include "common/ProductIdentity.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <vector>

namespace {

const char kLegacyServiceName[] = "Barrier";
const wchar_t kLegacyServiceNameWide[] = L"Barrier";
const wchar_t kWeaveInstallDirectory[] = L"Weave";
const wchar_t kWeaveDaemonFilename[] = L"weaved.exe";

std::wstring
normalizeWindowsPathForComparison(const std::wstring& input)
{
    std::wstring path;
    if (input.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
        path = L"\\\\" + input.substr(8);
    }
    else if (input.compare(0, 4, L"\\\\?\\") == 0) {
        path = input.substr(4);
    }
    else {
        path = input;
    }

    for (wchar_t& ch : path) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    while (path.size() > 3 && path.back() == L'\\') {
        path.pop_back();
    }
    return path;
}

bool
windowsPathsEqual(const std::wstring& lhs, const std::wstring& rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    if (lhs.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    return CompareStringOrdinal(
               lhs.c_str(), static_cast<int>(lhs.size()),
               rhs.c_str(), static_cast<int>(rhs.size()), TRUE) == CSTR_EQUAL;
}

bool
parseStrictQuotedServiceImage(
    const std::wstring& imagePath,
    std::wstring& executablePath)
{
    if (imagePath.size() < 3 || imagePath.front() != L'"' ||
        imagePath.back() != L'"') {
        return false;
    }

    executablePath = imagePath.substr(1, imagePath.size() - 2);
    return !executablePath.empty() &&
        executablePath.find(L'"') == std::wstring::npos &&
        executablePath.find(L'\0') == std::wstring::npos;
}

bool
isDriveAbsolutePath(const std::wstring& path)
{
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') {
        return false;
    }
    const wchar_t drive = path[0];
    return (drive >= L'A' && drive <= L'Z') ||
        (drive >= L'a' && drive <= L'z');
}

std::wstring
expectedWeaveDaemonPath(const std::wstring& programFilesPath)
{
    std::wstring root = normalizeWindowsPathForComparison(programFilesPath);
    if (!isDriveAbsolutePath(root)) {
        return std::wstring();
    }
    return root + L"\\" + kWeaveInstallDirectory + L"\\" +
        kWeaveDaemonFilename;
}

class ScopedKernelHandle {
public:
    explicit ScopedKernelHandle(HANDLE handle) noexcept : m_handle(handle) { }
    ~ScopedKernelHandle()
    {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }
    }

    HANDLE get() const noexcept { return m_handle; }

private:
    ScopedKernelHandle(const ScopedKernelHandle&) = delete;
    ScopedKernelHandle& operator=(const ScopedKernelHandle&) = delete;

    HANDLE m_handle;
};

bool
resolveFinalFilePath(const std::wstring& path, std::wstring& finalPath)
{
    ScopedKernelHandle file(CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION information;
    if (!GetFileInformationByHandle(file.get(), &information) ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }

    const DWORD required = GetFinalPathNameByHandleW(
        file.get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0) {
        return false;
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1u);
    const DWORD written = GetFinalPathNameByHandleW(
        file.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written >= buffer.size()) {
        return false;
    }

    finalPath.assign(buffer.data(), written);
    return true;
}

bool
readProgramFilesPath(std::wstring& path)
{
    const DWORD required = GetEnvironmentVariableW(L"ProgramFiles", nullptr, 0);
    if (required == 0) {
        return false;
    }

    std::vector<wchar_t> buffer(required);
    const DWORD written = GetEnvironmentVariableW(
        L"ProgramFiles", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        return false;
    }
    path.assign(buffer.data(), written);
    return true;
}

void
requireServiceRemovalCommitted(const char* serviceName)
{
    ArchDaemonWindowsPolicy::ScopedServiceHandle manager(
        OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (manager.get() == nullptr) {
        throw XArchDaemonUninstallFailed(new XArchEvalWindows);
    }

    ArchDaemonWindowsPolicy::ScopedServiceHandle service(
        OpenService(manager.get(), serviceName, SERVICE_QUERY_STATUS));
    if (service.get() != nullptr) {
        throw XArchDaemonUninstallFailed(
            "legacy Barrier service remains installed");
    }

    const DWORD error = GetLastError();
    if (error != ERROR_SERVICE_DOES_NOT_EXIST &&
        error != ERROR_SERVICE_MARKED_FOR_DELETE) {
        throw XArchDaemonUninstallFailed(new XArchEvalWindows(error));
    }
}

} // namespace

namespace ArchDaemonWindowsPolicy {

ScopedServiceHandle::ScopedServiceHandle(
    SC_HANDLE handle,
    ServiceHandleCloser closer) noexcept :
    m_handle(handle),
    m_closer(closer)
{
}

ScopedServiceHandle::~ScopedServiceHandle()
{
    reset();
}

ScopedServiceHandle::ScopedServiceHandle(ScopedServiceHandle&& other) noexcept :
    m_handle(other.m_handle),
    m_closer(other.m_closer)
{
    other.m_handle = nullptr;
}

ScopedServiceHandle&
ScopedServiceHandle::operator=(ScopedServiceHandle&& other) noexcept
{
    if (this != &other) {
        reset();
        m_handle = other.m_handle;
        m_closer = other.m_closer;
        other.m_handle = nullptr;
    }
    return *this;
}

SC_HANDLE
ScopedServiceHandle::get() const noexcept
{
    return m_handle;
}

void
ScopedServiceHandle::reset(SC_HANDLE handle) noexcept
{
    if (m_handle == handle) {
        return;
    }
    if (m_handle != nullptr && m_closer != nullptr) {
        m_closer(m_handle);
    }
    m_handle = handle;
}

bool
deleteServiceSucceeded(BOOL result) noexcept
{
    return result != FALSE;
}

bool
isSafeLegacyServiceMigrationPath(
    const std::wstring& serviceImagePath,
    const std::wstring& programFilesPath,
    const std::wstring& serviceBinaryFinalPath,
    const std::wstring& expectedBinaryFinalPath)
{
    std::wstring configuredExecutable;
    if (!parseStrictQuotedServiceImage(serviceImagePath, configuredExecutable)) {
        return false;
    }

    const std::wstring expectedPath =
        expectedWeaveDaemonPath(programFilesPath);
    if (expectedPath.empty()) {
        return false;
    }

    const std::wstring configured =
        normalizeWindowsPathForComparison(configuredExecutable);
    const std::wstring serviceFinal =
        normalizeWindowsPathForComparison(serviceBinaryFinalPath);
    const std::wstring expectedFinal =
        normalizeWindowsPathForComparison(expectedBinaryFinalPath);
    return windowsPathsEqual(configured, expectedPath) &&
        windowsPathsEqual(serviceFinal, expectedPath) &&
        windowsPathsEqual(expectedFinal, expectedPath);
}

bool
migrateLegacyServiceAfterInstall(
    const char* installedServiceName,
    const PostInstallMigrationState& state,
    const std::function<bool()>& isEligible,
    const std::function<void()>& removeLegacyService)
{
    if (installedServiceName == nullptr ||
        std::strcmp(installedServiceName, WEAVE_SERVICE_NAME) != 0 ||
        !state.serviceConfigured || !state.parametersConfigured ||
        !isEligible || !removeLegacyService) {
        return false;
    }

    if (!isEligible()) {
        return false;
    }

    removeLegacyService();
    return true;
}

} // namespace ArchDaemonWindowsPolicy

//
// ArchDaemonWindows
//

ArchDaemonWindows*        ArchDaemonWindows::s_daemon = NULL;

ArchDaemonWindows::ArchDaemonWindows() :
m_daemonThreadID(0)
{
    m_quitMessage = RegisterWindowMessage(WEAVE_SERVICE_QUIT_MESSAGE);
}

ArchDaemonWindows::~ArchDaemonWindows()
{
    // do nothing
}

int
ArchDaemonWindows::runDaemon(RunFunc runFunc)
{
    assert(s_daemon != NULL);
    return s_daemon->doRunDaemon(runFunc);
}

void
ArchDaemonWindows::daemonRunning(bool running)
{
    if (s_daemon != NULL) {
        s_daemon->doDaemonRunning(running);
    }
}

UINT
ArchDaemonWindows::getDaemonQuitMessage()
{
    if (s_daemon != NULL) {
        return s_daemon->doGetDaemonQuitMessage();
    }
    else {
        return 0;
    }
}

void
ArchDaemonWindows::daemonFailed(int result)
{
    assert(s_daemon != NULL);
    throw XArchDaemonRunFailed(result);
}

void
ArchDaemonWindows::installDaemon(const char* name,
                const char* description,
                const char* pathname,
                const char* commandLine,
                const char* dependencies)
{
    ArchDaemonWindowsPolicy::PostInstallMigrationState migrationState;
    {
        ArchDaemonWindowsPolicy::ScopedServiceHandle mgr(
            OpenSCManager(NULL, NULL,
                          SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
        if (mgr.get() == NULL) {
            throw XArchDaemonInstallFailed(new XArchEvalWindows);
        }

        const char* displayName =
            (std::strcmp(name, WEAVE_SERVICE_NAME) == 0) ?
                WEAVE_SERVICE_DISPLAY_NAME : name;

        ArchDaemonWindowsPolicy::ScopedServiceHandle service(CreateService(
            mgr.get(),
            name,
            displayName,
            0,
            SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            pathname,
            NULL,
            NULL,
            dependencies,
            NULL,
            NULL));

        if (service.get() == NULL) {
            DWORD err = GetLastError();
            if (err != ERROR_SERVICE_EXISTS) {
                throw XArchDaemonInstallFailed(new XArchEvalWindows(err));
            }

            service.reset(OpenService(
                mgr.get(), name, SERVICE_CHANGE_CONFIG));
            if (service.get() == NULL) {
                err = GetLastError();
                throw XArchDaemonInstallFailed(new XArchEvalWindows(err));
            }
            if (!ChangeServiceConfig(service.get(),
                                     SERVICE_NO_CHANGE,
                                     SERVICE_AUTO_START,
                                     SERVICE_NO_CHANGE,
                                     pathname,
                                     NULL,
                                     NULL,
                                     NULL,
                                     NULL,
                                     NULL,
                                     displayName)) {
                err = GetLastError();
                throw XArchDaemonInstallFailed(new XArchEvalWindows(err));
            }
        }
    }
    migrationState.serviceConfigured = true;

    // open the registry key for this service
    HKEY key = openNTServicesKey();
    key      = ArchMiscWindows::addKey(key, name);
    if (key == NULL) {
        // can't open key
        DWORD err = GetLastError();
        try {
            uninstallDaemon(name);
        }
        catch (...) {
            // ignore
        }
        throw XArchDaemonInstallFailed(new XArchEvalWindows(err));
    }

    // set the description
    ArchMiscWindows::setValue(key, _T("Description"), description);

    // set command line
    key = ArchMiscWindows::addKey(key, _T("Parameters"));
    if (key == NULL) {
        // can't open key
        DWORD err = GetLastError();
        ArchMiscWindows::closeKey(key);
        try {
            uninstallDaemon(name);
        }
        catch (...) {
            // ignore
        }
        throw XArchDaemonInstallFailed(new XArchEvalWindows(err));
    }
    ArchMiscWindows::setValue(key, _T("CommandLine"), commandLine);

    // done with registry
    ArchMiscWindows::closeKey(key);
    migrationState.parametersConfigured = true;

    try {
        ArchDaemonWindowsPolicy::migrateLegacyServiceAfterInstall(
            name,
            migrationState,
            &ArchDaemonWindows::isLegacyWeaveDaemonEligibleForMigration,
            [this]() {
                uninstallDaemon(kLegacyServiceName);
                requireServiceRemovalCommitted(kLegacyServiceName);
            });
    }
    catch (const XArchDaemonUninstallFailed& error) {
        throw XArchDaemonInstallFailed(
            std::string("failed to remove eligible legacy Barrier service: ") +
            error.what());
    }
}

void
ArchDaemonWindows::uninstallDaemon(const char* name)
{
    // remove parameters for this service.  ignore failures.
    HKEY key = openNTServicesKey();
    key      = ArchMiscWindows::openKey(key, name);
    if (key != NULL) {
        ArchMiscWindows::deleteKey(key, _T("Parameters"));
        ArchMiscWindows::closeKey(key);
    }

    bool okay = false;
    DWORD err = ERROR_SUCCESS;
    {
        ArchDaemonWindowsPolicy::ScopedServiceHandle mgr(
            OpenSCManager(NULL, NULL, GENERIC_WRITE));
        if (mgr.get() == NULL) {
            throw XArchDaemonUninstallFailed(new XArchEvalWindows);
        }

        ArchDaemonWindowsPolicy::ScopedServiceHandle service(
            OpenService(mgr.get(), name, DELETE | SERVICE_STOP));
        if (service.get() == NULL) {
            err = GetLastError();
            if (err != ERROR_SERVICE_DOES_NOT_EXIST) {
                throw XArchDaemonUninstallFailed(new XArchEvalWindows(err));
            }
            throw XArchDaemonUninstallNotInstalled(new XArchEvalWindows(err));
        }

        // Stopping remains best effort; DeleteService determines the result.
        SERVICE_STATUS status;
        ControlService(service.get(), SERVICE_CONTROL_STOP, &status);

        const BOOL deleteResult = DeleteService(service.get());
        okay = ArchDaemonWindowsPolicy::deleteServiceSucceeded(deleteResult);
        if (!okay) {
            err = GetLastError();
        }
    }

    // give windows a chance to remove the service before
    // we check if it still exists.
    ARCH->sleep(1);

    // handle failure.  ignore error if service isn't installed anymore.
    if (!okay && isDaemonInstalled(name)) {
        if (err == ERROR_SUCCESS) {
            // this seems to occur even though the uninstall was successful.
            // it could be a timing issue, i.e., isDaemonInstalled is
            // called too soon. i've added a sleep to try and stop this.
            return;
        }
        if (err == ERROR_IO_PENDING) {
            // this seems to be a spurious error
            return;
        }
        if (err != ERROR_SERVICE_MARKED_FOR_DELETE) {
            throw XArchDaemonUninstallFailed(new XArchEvalWindows(err));
        }
        throw XArchDaemonUninstallNotInstalled(new XArchEvalWindows(err));
    }
}

int
ArchDaemonWindows::daemonize(const char* name, DaemonFunc func)
{
    assert(name != NULL);
    assert(func != NULL);

    // save daemon function
    m_daemonFunc = func;

    // construct the service entry
    SERVICE_TABLE_ENTRY entry[2];
    entry[0].lpServiceName = const_cast<char*>(name);
    entry[0].lpServiceProc = &ArchDaemonWindows::serviceMainEntry;
    entry[1].lpServiceName = NULL;
    entry[1].lpServiceProc = NULL;

    // hook us up to the service control manager.  this won't return
    // (if successful) until the processes have terminated.
    s_daemon = this;
    if (StartServiceCtrlDispatcher(entry) == 0) {
        // StartServiceCtrlDispatcher failed
        s_daemon = NULL;
        throw XArchDaemonFailed(new XArchEvalWindows);
    }

    s_daemon = NULL;
    return m_daemonResult;
}

bool
ArchDaemonWindows::canInstallDaemon(const char* /*name*/)
{
    // check if we can open service manager for write
    ArchDaemonWindowsPolicy::ScopedServiceHandle mgr(
        OpenSCManager(NULL, NULL, GENERIC_WRITE));
    if (mgr.get() == NULL) {
        return false;
    }

    // check if we can open the registry key
    HKEY key = openNTServicesKey();
    ArchMiscWindows::closeKey(key);

    return (key != NULL);
}

bool
ArchDaemonWindows::isDaemonInstalled(const char* name)
{
    ArchDaemonWindowsPolicy::ScopedServiceHandle mgr(
        OpenSCManager(NULL, NULL, GENERIC_READ));
    if (mgr.get() == NULL) {
        return false;
    }

    ArchDaemonWindowsPolicy::ScopedServiceHandle service(
        OpenService(mgr.get(), name, GENERIC_READ));
    return service.get() != NULL;
}

bool
ArchDaemonWindows::isLegacyWeaveDaemonEligibleForMigration()
{
    try {
        ArchDaemonWindowsPolicy::ScopedServiceHandle manager(
            OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (manager.get() == nullptr) {
            return false;
        }

        ArchDaemonWindowsPolicy::ScopedServiceHandle service(OpenServiceW(
            manager.get(), kLegacyServiceNameWide, SERVICE_QUERY_CONFIG));
        if (service.get() == nullptr) {
            return false;
        }

        DWORD requiredBytes = 0;
        QueryServiceConfigW(service.get(), nullptr, 0, &requiredBytes);
        if (requiredBytes < sizeof(QUERY_SERVICE_CONFIGW) ||
            GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return false;
        }

        const std::size_t wordCount =
            (requiredBytes + sizeof(std::uintptr_t) - 1u) /
            sizeof(std::uintptr_t);
        std::vector<std::uintptr_t> storage(wordCount);
        const std::size_t storageBytes =
            storage.size() * sizeof(std::uintptr_t);
        if (storageBytes > MAXDWORD) {
            return false;
        }
        QUERY_SERVICE_CONFIGW* config =
            reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage.data());
        if (!QueryServiceConfigW(
                service.get(), config,
                static_cast<DWORD>(storageBytes),
                &requiredBytes) ||
            config->lpBinaryPathName == nullptr) {
            return false;
        }

        const std::wstring imagePath(config->lpBinaryPathName);
        std::wstring executablePath;
        if (!parseStrictQuotedServiceImage(imagePath, executablePath)) {
            return false;
        }

        std::wstring programFilesPath;
        if (!readProgramFilesPath(programFilesPath)) {
            return false;
        }
        const std::wstring expectedPath =
            expectedWeaveDaemonPath(programFilesPath);
        if (expectedPath.empty()) {
            return false;
        }

        std::wstring serviceFinalPath;
        std::wstring expectedFinalPath;
        if (!resolveFinalFilePath(executablePath, serviceFinalPath) ||
            !resolveFinalFilePath(expectedPath, expectedFinalPath)) {
            return false;
        }

        return ArchDaemonWindowsPolicy::isSafeLegacyServiceMigrationPath(
            imagePath, programFilesPath, serviceFinalPath, expectedFinalPath);
    }
    catch (...) {
        return false;
    }
}

HKEY
ArchDaemonWindows::openNTServicesKey()
{
    static const char* s_keyNames[] = {
        _T("SYSTEM"),
        _T("CurrentControlSet"),
        _T("Services"),
        NULL
    };

    return ArchMiscWindows::addKey(HKEY_LOCAL_MACHINE, s_keyNames);
}

bool
ArchDaemonWindows::isRunState(DWORD state)
{
    switch (state) {
    case SERVICE_START_PENDING:
    case SERVICE_CONTINUE_PENDING:
    case SERVICE_RUNNING:
        return true;

    default:
        return false;
    }
}

int
ArchDaemonWindows::doRunDaemon(RunFunc run)
{
    // should only be called from DaemonFunc
    assert(m_serviceMutex != NULL);
    assert(run            != NULL);

    // create message queue for this thread
    MSG dummy;
    PeekMessage(&dummy, NULL, 0, 0, PM_NOREMOVE);

    int result = 0;
    ARCH->lockMutex(m_serviceMutex);
    m_daemonThreadID = GetCurrentThreadId();
    while (m_serviceState != SERVICE_STOPPED) {
        // wait until we're told to start
        while (!isRunState(m_serviceState) &&
                m_serviceState != SERVICE_STOP_PENDING) {
            ARCH->waitCondVar(m_serviceCondVar, m_serviceMutex, -1.0);
        }

        // run unless told to stop
        if (m_serviceState != SERVICE_STOP_PENDING) {
            ARCH->unlockMutex(m_serviceMutex);
            try {
                result = run();
            }
            catch (...) {
                ARCH->lockMutex(m_serviceMutex);
                setStatusError(0);
                m_serviceState = SERVICE_STOPPED;
                setStatus(m_serviceState);
                ARCH->broadcastCondVar(m_serviceCondVar);
                ARCH->unlockMutex(m_serviceMutex);
                throw;
            }
            ARCH->lockMutex(m_serviceMutex);
        }

        // notify of new state
        if (m_serviceState == SERVICE_PAUSE_PENDING) {
            m_serviceState = SERVICE_PAUSED;
        }
        else {
            m_serviceState = SERVICE_STOPPED;
        }
        setStatus(m_serviceState);
        ARCH->broadcastCondVar(m_serviceCondVar);
    }
    ARCH->unlockMutex(m_serviceMutex);
    return result;
}

void
ArchDaemonWindows::doDaemonRunning(bool running)
{
    ARCH->lockMutex(m_serviceMutex);
    if (running) {
        m_serviceState = SERVICE_RUNNING;
        setStatus(m_serviceState);
        ARCH->broadcastCondVar(m_serviceCondVar);
    }
    ARCH->unlockMutex(m_serviceMutex);
}

UINT
ArchDaemonWindows::doGetDaemonQuitMessage()
{
    return m_quitMessage;
}

void
ArchDaemonWindows::setStatus(DWORD state)
{
    setStatus(state, 0, 0);
}

void
ArchDaemonWindows::setStatus(DWORD state, DWORD step, DWORD waitHint)
{
    assert(s_daemon != NULL);

    SERVICE_STATUS status;
    status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS |
                                        SERVICE_INTERACTIVE_PROCESS;
    status.dwCurrentState            = state;
    status.dwControlsAccepted        = SERVICE_ACCEPT_STOP |
                                        SERVICE_ACCEPT_PAUSE_CONTINUE |
                                        SERVICE_ACCEPT_SHUTDOWN;
    status.dwWin32ExitCode           = NO_ERROR;
    status.dwServiceSpecificExitCode = 0;
    status.dwCheckPoint              = step;
    status.dwWaitHint                = waitHint;
    SetServiceStatus(s_daemon->m_statusHandle, &status);
}

void
ArchDaemonWindows::setStatusError(DWORD error)
{
    assert(s_daemon != NULL);

    SERVICE_STATUS status;
    status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS |
                                        SERVICE_INTERACTIVE_PROCESS;
    status.dwCurrentState            = SERVICE_STOPPED;
    status.dwControlsAccepted        = SERVICE_ACCEPT_STOP |
                                        SERVICE_ACCEPT_PAUSE_CONTINUE |
                                        SERVICE_ACCEPT_SHUTDOWN;
    status.dwWin32ExitCode           = ERROR_SERVICE_SPECIFIC_ERROR;
    status.dwServiceSpecificExitCode = error;
    status.dwCheckPoint              = 0;
    status.dwWaitHint                = 0;
    SetServiceStatus(s_daemon->m_statusHandle, &status);
}

void
ArchDaemonWindows::serviceMain(DWORD argc, LPTSTR* argvIn)
{
    typedef std::vector<LPCTSTR> ArgList;
    typedef std::vector<std::string> Arguments;
    const char** argv = const_cast<const char**>(argvIn);

    // create synchronization objects
    m_serviceMutex        = ARCH->newMutex();
    m_serviceCondVar      = ARCH->newCondVar();

    // register our service handler function
    m_statusHandle = RegisterServiceCtrlHandler(argv[0],
                                &ArchDaemonWindows::serviceHandlerEntry);
    if (m_statusHandle == 0) {
        // cannot start as service
        m_daemonResult = -1;
        ARCH->closeCondVar(m_serviceCondVar);
        ARCH->closeMutex(m_serviceMutex);
        return;
    }

    // tell service control manager that we're starting
    m_serviceState = SERVICE_START_PENDING;
    setStatus(m_serviceState, 0, 10000);

    std::string commandLine;

    // if no arguments supplied then try getting them from the registry.
    // the first argument doesn't count because it's the service name.
    Arguments args;
    ArgList myArgv;
    if (argc <= 1) {
        // read command line
        HKEY key = openNTServicesKey();
        key      = ArchMiscWindows::openKey(key, argvIn[0]);
        key      = ArchMiscWindows::openKey(key, _T("Parameters"));
        if (key != NULL) {
            commandLine = ArchMiscWindows::readValueString(key,
                                                _T("CommandLine"));
        }

        // if the command line isn't empty then parse and use it
        if (!commandLine.empty()) {
            // parse, honoring double quoted substrings
            std::string::size_type i = commandLine.find_first_not_of(" \t");
            while (i != std::string::npos && i != commandLine.size()) {
                // find end of string
                std::string::size_type e;
                if (commandLine[i] == '\"') {
                    // quoted.  find closing quote.
                    ++i;
                    e = commandLine.find("\"", i);

                    // whitespace must follow closing quote
                    if (e == std::string::npos ||
                        (e + 1 != commandLine.size() &&
                        commandLine[e + 1] != ' ' &&
                        commandLine[e + 1] != '\t')) {
                        args.clear();
                        break;
                    }

                    // extract
                    args.push_back(commandLine.substr(i, e - i));
                    i = e + 1;
                }
                else {
                    // unquoted.  find next whitespace.
                    e = commandLine.find_first_of(" \t", i);
                    if (e == std::string::npos) {
                        e = commandLine.size();
                    }

                    // extract
                    args.push_back(commandLine.substr(i, e - i));
                    i = e + 1;
                }

                // next argument
                i = commandLine.find_first_not_of(" \t", i);
            }

            // service name goes first
            myArgv.push_back(argv[0]);

            // get pointers
            for (size_t j = 0; j < args.size(); ++j) {
                myArgv.push_back(args[j].c_str());
            }

            // adjust argc/argv
            argc = (DWORD)myArgv.size();
            argv = &myArgv[0];
        }
    }

    m_commandLine = commandLine;

    try {
        // invoke daemon function
        m_daemonResult = m_daemonFunc(static_cast<int>(argc), argv);
    }
    catch (XArchDaemonRunFailed& e) {
        setStatusError(e.m_result);
        m_daemonResult = -1;
    }
    catch (...) {
        setStatusError(1);
        m_daemonResult = -1;
    }

    // clean up
    ARCH->closeCondVar(m_serviceCondVar);
    ARCH->closeMutex(m_serviceMutex);

    // we're going to exit now, so set status to stopped
    m_serviceState = SERVICE_STOPPED;
    setStatus(m_serviceState, 0, 10000);
}

void WINAPI
ArchDaemonWindows::serviceMainEntry(DWORD argc, LPTSTR* argv)
{
    s_daemon->serviceMain(argc, argv);
}

void
ArchDaemonWindows::serviceHandler(DWORD ctrl)
{
    assert(m_serviceMutex   != NULL);
    assert(m_serviceCondVar != NULL);

    ARCH->lockMutex(m_serviceMutex);

    // ignore request if service is already stopped
    if (s_daemon == NULL || m_serviceState == SERVICE_STOPPED) {
        if (s_daemon != NULL) {
            setStatus(m_serviceState);
        }
        ARCH->unlockMutex(m_serviceMutex);
        return;
    }

    switch (ctrl) {
    case SERVICE_CONTROL_PAUSE:
        m_serviceState = SERVICE_PAUSE_PENDING;
        setStatus(m_serviceState, 0, 5000);
        PostThreadMessage(m_daemonThreadID, m_quitMessage, 0, 0);
        break;

    case SERVICE_CONTROL_CONTINUE:
        // FIXME -- maybe should flush quit messages from queue
        m_serviceState = SERVICE_CONTINUE_PENDING;
        setStatus(m_serviceState, 0, 5000);
        ARCH->broadcastCondVar(m_serviceCondVar);
        break;

    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        m_serviceState = SERVICE_STOP_PENDING;
        setStatus(m_serviceState, 0, 5000);
        PostThreadMessage(m_daemonThreadID, m_quitMessage, 0, 0);
        ARCH->broadcastCondVar(m_serviceCondVar);
        break;

    default:
        // unknown service command
        // fall through

    case SERVICE_CONTROL_INTERROGATE:
        setStatus(m_serviceState);
        break;
    }

    ARCH->unlockMutex(m_serviceMutex);
}

void WINAPI
ArchDaemonWindows::serviceHandlerEntry(DWORD ctrl)
{
    s_daemon->serviceHandler(ctrl);
}

void
ArchDaemonWindows::start(const char* name)
{
    ArchDaemonWindowsPolicy::ScopedServiceHandle mgr(
        OpenSCManager(NULL, NULL, GENERIC_READ));
    if (mgr.get() == NULL) {
        throw XArchDaemonFailed(new XArchEvalWindows());
    }

    ArchDaemonWindowsPolicy::ScopedServiceHandle service(
        OpenService(mgr.get(), name, SERVICE_START));
    if (service.get() == NULL) {
        throw XArchDaemonFailed(new XArchEvalWindows());
    }

    if (!StartService(service.get(), 0, NULL)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_ALREADY_RUNNING) {
            throw XArchDaemonFailed(new XArchEvalWindows(error));
        }
    }
}

void
ArchDaemonWindows::stop(const char* name)
{
    ArchDaemonWindowsPolicy::ScopedServiceHandle mgr(
        OpenSCManager(NULL, NULL, GENERIC_READ));
    if (mgr.get() == NULL) {
        throw XArchDaemonFailed(new XArchEvalWindows());
    }

    ArchDaemonWindowsPolicy::ScopedServiceHandle service(OpenService(
        mgr.get(), name, SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (service.get() == NULL) {
        throw XArchDaemonFailed(new XArchEvalWindows());
    }

    // ask the service to stop, asynchronously
    SERVICE_STATUS ss;
    if (!ControlService(service.get(), SERVICE_CONTROL_STOP, &ss)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE) {
            throw XArchDaemonFailed(new XArchEvalWindows(error));
        }
    }
}

void
ArchDaemonWindows::installDaemon()
{
    char path[MAX_PATH];
    GetModuleFileName(ArchMiscWindows::instanceWin32(), path, MAX_PATH);

    // Refresh the service path as part of every install/upgrade.
    std::stringstream ss;
    ss << '"';
    ss << path;
    ss << '"';

    installDaemon(WEAVE_SERVICE_NAME, WEAVE_SERVICE_DESCRIPTION,
                  ss.str().c_str(), "", "");

    start(WEAVE_SERVICE_NAME);
}

void
ArchDaemonWindows::uninstallDaemon()
{
    // remove service if installed.
    if (isDaemonInstalled(WEAVE_SERVICE_NAME)) {
        uninstallDaemon(WEAVE_SERVICE_NAME);
    }
}
