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
#include "common/win32/encoding_utilities.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

namespace {

const char kLegacyServiceName[] = "Barrier";
const wchar_t kLegacyServiceNameWide[] = L"Barrier";
const wchar_t kWeaveInstallDirectory[] = L"Weave";
const wchar_t kWeaveDaemonFilename[] = L"weaved.exe";
const std::size_t kMaximumServiceDependencyBytes = 32768u;

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

class ScopedRegistryKey {
public:
    explicit ScopedRegistryKey(HKEY key = nullptr) noexcept : m_key(key) { }
    ~ScopedRegistryKey()
    {
        if (m_key != nullptr) {
            RegCloseKey(m_key);
        }
    }

    HKEY get() const noexcept { return m_key; }

private:
    ScopedRegistryKey(const ScopedRegistryKey&) = delete;
    ScopedRegistryKey& operator=(const ScopedRegistryKey&) = delete;

    HKEY m_key;
};

HKEY
openRegistrySubkeyWide(HKEY parent, const wchar_t* name, bool create)
{
    if (parent == nullptr || name == nullptr || name[0] == L'\0') {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }

    HKEY key = nullptr;
    LONG result = RegOpenKeyExW(
        parent, name, 0, KEY_WRITE | KEY_QUERY_VALUE, &key);
    if (result != ERROR_SUCCESS && create) {
        DWORD disposition = 0;
        result = RegCreateKeyExW(
            parent, name, 0, nullptr, 0,
            KEY_WRITE | KEY_QUERY_VALUE, nullptr, &key, &disposition);
    }
    if (result != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(result));
        return nullptr;
    }
    return key;
}

bool
wideServiceDependencies(
    const char* dependencies,
    std::vector<WCHAR>& wideDependencies)
{
    wideDependencies.clear();
    if (dependencies == nullptr) {
        return false;
    }
    if (dependencies[0] == '\0') {
        return true;
    }

    const char* current = dependencies;
    std::size_t remaining = kMaximumServiceDependencyBytes;
    while (remaining > 1u && current[0] != '\0') {
        const std::size_t length = strnlen_s(current, remaining);
        if (length == 0u || length >= remaining) {
            wideDependencies.clear();
            return false;
        }

        std::wstring wide;
        if (!ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
                std::string(current, length), false, wide)) {
            wideDependencies.clear();
            return false;
        }
        wideDependencies.insert(
            wideDependencies.end(), wide.begin(), wide.end());
        wideDependencies.push_back(L'\0');
        current += length + 1u;
        remaining -= length + 1u;
    }

    if (remaining == 0u || current[0] != '\0') {
        wideDependencies.clear();
        return false;
    }
    wideDependencies.push_back(L'\0');
    return true;
}

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
    std::wstring wideServiceName;
    if (serviceName == nullptr ||
        !ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
            serviceName, false, wideServiceName)) {
        throw XArchDaemonUninstallFailed(
            new XArchEvalWindows(ERROR_NO_UNICODE_TRANSLATION));
    }
    ArchDaemonWindowsPolicy::ScopedServiceHandle manager(
        OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (manager.get() == nullptr) {
        throw XArchDaemonUninstallFailed(new XArchEvalWindows);
    }

    ArchDaemonWindowsPolicy::ScopedServiceHandle service(
        OpenServiceW(
            manager.get(), wideServiceName.c_str(), SERVICE_QUERY_STATUS));
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
utf8ServiceTextToWide(
    const std::string& utf8,
    bool allowEmpty,
    std::wstring& wide)
{
    wide.clear();
    if (utf8.empty()) {
        return allowEmpty;
    }

    const std::vector<WCHAR> converted = utf8_to_win_char(utf8);
    if (converted.size() <= 1u) {
        return false;
    }
    wide.assign(converted.data(), converted.size() - 1u);
    return true;
}

bool
wideServiceArgumentsToUtf8(
    DWORD argc,
    const WCHAR* const* argv,
    std::vector<std::string>& utf8Arguments)
{
    utf8Arguments.clear();
    if (argc == 0u || argv == nullptr ||
        argc > static_cast<DWORD>((std::numeric_limits<int>::max)())) {
        return false;
    }

    utf8Arguments.reserve(argc);
    for (DWORD i = 0; i < argc; ++i) {
        if (argv[i] == nullptr || (i == 0u && argv[i][0] == L'\0')) {
            utf8Arguments.clear();
            return false;
        }
        if (argv[i][0] == L'\0') {
            utf8Arguments.push_back(std::string());
            continue;
        }

        std::string argument = win_wchar_to_utf8(argv[i]);
        if (argument.empty()) {
            utf8Arguments.clear();
            return false;
        }
        utf8Arguments.push_back(std::move(argument));
    }
    return true;
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
    std::wstring quitMessage;
    m_quitMessage = ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
        WEAVE_SERVICE_QUIT_MESSAGE, false, quitMessage)
        ? RegisterWindowMessageW(quitMessage.c_str())
        : 0;
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
    if (name == nullptr || description == nullptr || pathname == nullptr ||
        commandLine == nullptr || dependencies == nullptr) {
        throw XArchDaemonInstallFailed(
            new XArchEvalWindows(ERROR_INVALID_PARAMETER));
    }

    const char* displayName =
        (std::strcmp(name, WEAVE_SERVICE_NAME) == 0) ?
            WEAVE_SERVICE_DISPLAY_NAME : name;
    std::wstring wideName;
    std::wstring wideDisplayName;
    std::wstring widePathname;
    std::vector<WCHAR> wideDependencies;
    if (!ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
            name, false, wideName) ||
        !ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
            displayName, false, wideDisplayName) ||
        !ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
            pathname, false, widePathname) ||
        !wideServiceDependencies(dependencies, wideDependencies)) {
        throw XArchDaemonInstallFailed(
            new XArchEvalWindows(ERROR_NO_UNICODE_TRANSLATION));
    }

    ArchDaemonWindowsPolicy::PostInstallMigrationState migrationState;
    {
        ArchDaemonWindowsPolicy::ScopedServiceHandle mgr(
            OpenSCManagerW(
                NULL, NULL,
                SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
        if (mgr.get() == NULL) {
            throw XArchDaemonInstallFailed(new XArchEvalWindows);
        }

        ArchDaemonWindowsPolicy::ScopedServiceHandle service(CreateServiceW(
            mgr.get(),
            wideName.c_str(),
            wideDisplayName.c_str(),
            0,
            SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            widePathname.c_str(),
            NULL,
            NULL,
            wideDependencies.empty() ? NULL : wideDependencies.data(),
            NULL,
            NULL));

        if (service.get() == NULL) {
            DWORD err = GetLastError();
            if (err != ERROR_SERVICE_EXISTS) {
                throw XArchDaemonInstallFailed(new XArchEvalWindows(err));
            }

            service.reset(OpenServiceW(
                mgr.get(), wideName.c_str(), SERVICE_CHANGE_CONFIG));
            if (service.get() == NULL) {
                err = GetLastError();
                throw XArchDaemonInstallFailed(new XArchEvalWindows(err));
            }
            if (!ChangeServiceConfigW(service.get(),
                                     SERVICE_NO_CHANGE,
                                     SERVICE_AUTO_START,
                                     SERVICE_NO_CHANGE,
                                     widePathname.c_str(),
                                     NULL,
                                     NULL,
                                     NULL,
                                     NULL,
                                     NULL,
                                     wideDisplayName.c_str())) {
                err = GetLastError();
                throw XArchDaemonInstallFailed(new XArchEvalWindows(err));
            }
        }
    }
    migrationState.serviceConfigured = true;

    // open the registry key for this service
    ScopedRegistryKey servicesKey(openNTServicesKey());
    ScopedRegistryKey serviceKey(openRegistrySubkeyWide(
        servicesKey.get(), wideName.c_str(), true));
    if (serviceKey.get() == NULL) {
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
    ArchMiscWindows::setValueUtf8(
        serviceKey.get(), "Description", description);

    // set command line
    ScopedRegistryKey parametersKey(openRegistrySubkeyWide(
        serviceKey.get(), L"Parameters", true));
    if (parametersKey.get() == NULL) {
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
    ArchMiscWindows::setValueUtf8(
        parametersKey.get(), "CommandLine", commandLine);
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
    std::wstring wideName;
    if (name == nullptr ||
        !ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
            name, false, wideName)) {
        throw XArchDaemonUninstallFailed(
            new XArchEvalWindows(ERROR_NO_UNICODE_TRANSLATION));
    }

    // remove parameters for this service.  ignore failures.
    ScopedRegistryKey servicesKey(openNTServicesKey());
    ScopedRegistryKey serviceKey(openRegistrySubkeyWide(
        servicesKey.get(), wideName.c_str(), false));
    if (serviceKey.get() != NULL) {
        RegDeleteKeyW(serviceKey.get(), L"Parameters");
    }

    bool okay = false;
    DWORD err = ERROR_SUCCESS;
    {
        ArchDaemonWindowsPolicy::ScopedServiceHandle mgr(
            OpenSCManagerW(NULL, NULL, GENERIC_WRITE));
        if (mgr.get() == NULL) {
            throw XArchDaemonUninstallFailed(new XArchEvalWindows);
        }

        ArchDaemonWindowsPolicy::ScopedServiceHandle service(
            OpenServiceW(
                mgr.get(), wideName.c_str(), DELETE | SERVICE_STOP));
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
    assert(func != NULL);
    if (name == NULL ||
        !ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
            name, false, m_serviceName)) {
        throw XArchDaemonFailed(
            new XArchEvalWindows(ERROR_NO_UNICODE_TRANSLATION));
    }

    // save daemon function
    m_daemonFunc = func;

    // construct the service entry
    SERVICE_TABLE_ENTRYW entry[2];
    entry[0].lpServiceName = &m_serviceName[0];
    entry[0].lpServiceProc = &ArchDaemonWindows::serviceMainEntry;
    entry[1].lpServiceName = NULL;
    entry[1].lpServiceProc = NULL;

    // hook us up to the service control manager.  this won't return
    // (if successful) until the processes have terminated.
    s_daemon = this;
    if (StartServiceCtrlDispatcherW(entry) == 0) {
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
        OpenSCManagerW(NULL, NULL, GENERIC_WRITE));
    if (mgr.get() == NULL) {
        return false;
    }

    // check if we can open the registry key
    HKEY key = openNTServicesKey();
    if (key != NULL) {
        ArchMiscWindows::closeKey(key);
    }

    return (key != NULL);
}

bool
ArchDaemonWindows::isDaemonInstalled(const char* name)
{
    std::wstring wideName;
    if (name == nullptr ||
        !ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
            name, false, wideName)) {
        return false;
    }
    ArchDaemonWindowsPolicy::ScopedServiceHandle mgr(
        OpenSCManagerW(NULL, NULL, GENERIC_READ));
    if (mgr.get() == NULL) {
        return false;
    }

    ArchDaemonWindowsPolicy::ScopedServiceHandle service(
        OpenServiceW(mgr.get(), wideName.c_str(), GENERIC_READ));
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
    static const wchar_t* s_keyNames[] = {
        L"SYSTEM",
        L"CurrentControlSet",
        L"Services",
        NULL
    };

    HKEY parent = HKEY_LOCAL_MACHINE;
    HKEY opened = nullptr;
    for (std::size_t i = 0; s_keyNames[i] != nullptr; ++i) {
        opened = openRegistrySubkeyWide(parent, s_keyNames[i], true);
        if (parent != HKEY_LOCAL_MACHINE) {
            RegCloseKey(parent);
        }
        if (opened == nullptr) {
            return nullptr;
        }
        parent = opened;
    }
    return parent;
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
ArchDaemonWindows::serviceMain(DWORD argc, LPWSTR* argvIn)
{
    typedef std::vector<std::string> Arguments;

    // create synchronization objects
    m_serviceMutex        = ARCH->newMutex();
    m_serviceCondVar      = ARCH->newCondVar();

    // The configured service name is already validated before the dispatcher
    // starts, so handler registration never depends on unchecked SCM argv.
    m_statusHandle = RegisterServiceCtrlHandlerW(
        m_serviceName.c_str(), &ArchDaemonWindows::serviceHandlerEntry);
    if (m_statusHandle == 0) {
        m_daemonResult = -1;
        ARCH->closeCondVar(m_serviceCondVar);
        ARCH->closeMutex(m_serviceMutex);
        return;
    }

    m_serviceState = SERVICE_START_PENDING;
    setStatus(m_serviceState, 0, 10000);

    Arguments effectiveArguments;
    if (!ArchDaemonWindowsPolicy::wideServiceArgumentsToUtf8(
            argc, argvIn, effectiveArguments)) {
        setStatusError(ERROR_NO_UNICODE_TRANSLATION);
        m_daemonResult = -1;
        ARCH->closeCondVar(m_serviceCondVar);
        ARCH->closeMutex(m_serviceMutex);
        return;
    }

    std::string commandLine;

    // If StartService supplied no arguments, use the persisted UTF-16 REG_SZ
    // command and preserve UTF-8 at the application's DaemonFunc boundary.
    if (argc <= 1u) {
        ScopedRegistryKey servicesKey(openNTServicesKey());
        ScopedRegistryKey serviceKey(openRegistrySubkeyWide(
            servicesKey.get(), m_serviceName.c_str(), false));
        ScopedRegistryKey parametersKey(openRegistrySubkeyWide(
            serviceKey.get(), L"Parameters", false));
        if (parametersKey.get() != NULL) {
            commandLine = ArchMiscWindows::readValueStringUtf8(
                parametersKey.get(), "CommandLine");
        }

        Arguments parsedArguments;
        if (!commandLine.empty()) {
            // Preserve the legacy quoting grammar used by installed configs.
            std::string::size_type i = commandLine.find_first_not_of(" \t");
            while (i != std::string::npos && i != commandLine.size()) {
                std::string::size_type e;
                if (commandLine[i] == '\"') {
                    ++i;
                    e = commandLine.find("\"", i);
                    if (e == std::string::npos ||
                        (e + 1 != commandLine.size() &&
                         commandLine[e + 1] != ' ' &&
                         commandLine[e + 1] != '\t')) {
                        parsedArguments.clear();
                        break;
                    }
                    parsedArguments.push_back(commandLine.substr(i, e - i));
                    i = e + 1;
                }
                else {
                    e = commandLine.find_first_of(" \t", i);
                    if (e == std::string::npos) {
                        e = commandLine.size();
                    }
                    parsedArguments.push_back(commandLine.substr(i, e - i));
                    i = e + 1;
                }
                i = commandLine.find_first_not_of(" \t", i);
            }
        }

        const std::string serviceName = effectiveArguments.front();
        effectiveArguments.clear();
        effectiveArguments.push_back(serviceName);
        effectiveArguments.insert(
            effectiveArguments.end(),
            parsedArguments.begin(), parsedArguments.end());
    }

    std::vector<const char*> argv;
    argv.reserve(effectiveArguments.size());
    for (const std::string& argument : effectiveArguments) {
        argv.push_back(argument.c_str());
    }

    m_commandLine = commandLine;

    try {
        m_daemonResult = m_daemonFunc(
            static_cast<int>(argv.size()), argv.data());
    }
    catch (XArchDaemonRunFailed& e) {
        setStatusError(e.m_result);
        m_daemonResult = -1;
    }
    catch (...) {
        setStatusError(1);
        m_daemonResult = -1;
    }

    ARCH->closeCondVar(m_serviceCondVar);
    ARCH->closeMutex(m_serviceMutex);

    m_serviceState = SERVICE_STOPPED;
    setStatus(m_serviceState, 0, 10000);
}

void WINAPI
ArchDaemonWindows::serviceMainEntry(DWORD argc, LPWSTR* argv)
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
    std::wstring wideName;
    if (name == nullptr ||
        !ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
            name, false, wideName)) {
        throw XArchDaemonFailed(
            new XArchEvalWindows(ERROR_NO_UNICODE_TRANSLATION));
    }
    ArchDaemonWindowsPolicy::ScopedServiceHandle mgr(
        OpenSCManagerW(NULL, NULL, GENERIC_READ));
    if (mgr.get() == NULL) {
        throw XArchDaemonFailed(new XArchEvalWindows());
    }

    ArchDaemonWindowsPolicy::ScopedServiceHandle service(
        OpenServiceW(mgr.get(), wideName.c_str(), SERVICE_START));
    if (service.get() == NULL) {
        throw XArchDaemonFailed(new XArchEvalWindows());
    }

    if (!StartServiceW(service.get(), 0, NULL)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_ALREADY_RUNNING) {
            throw XArchDaemonFailed(new XArchEvalWindows(error));
        }
    }
}

void
ArchDaemonWindows::stop(const char* name)
{
    std::wstring wideName;
    if (name == nullptr ||
        !ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
            name, false, wideName)) {
        throw XArchDaemonFailed(
            new XArchEvalWindows(ERROR_NO_UNICODE_TRANSLATION));
    }
    ArchDaemonWindowsPolicy::ScopedServiceHandle mgr(
        OpenSCManagerW(NULL, NULL, GENERIC_READ));
    if (mgr.get() == NULL) {
        throw XArchDaemonFailed(new XArchEvalWindows());
    }

    ArchDaemonWindowsPolicy::ScopedServiceHandle service(OpenServiceW(
        mgr.get(), wideName.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS));
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
    std::vector<WCHAR> path(32768u, L'\0');
    const DWORD length = GetModuleFileNameW(
        ArchMiscWindows::instanceWin32(), path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0u || length >= path.size()) {
        throw XArchDaemonInstallFailed(new XArchEvalWindows);
    }

    const std::string utf8Path = win_wchar_to_utf8(path.data());
    if (utf8Path.empty()) {
        throw XArchDaemonInstallFailed(
            new XArchEvalWindows(ERROR_NO_UNICODE_TRANSLATION));
    }

    // Refresh the service path as part of every install/upgrade.
    std::stringstream ss;
    ss << '"';
    ss << utf8Path;
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
