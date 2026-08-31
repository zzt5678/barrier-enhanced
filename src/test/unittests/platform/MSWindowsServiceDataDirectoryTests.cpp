/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/ServiceDataDirectoryPolicy.h"
#include "barrier/win32/MSWindowsServiceDataDirectory.h"

#include "test/global/gtest.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Aclapi.h>
#include <Sddl.h>

#include <string>

namespace {

bool isValidHandle(HANDLE handle)
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) : m_handle(handle) { }
    ~ScopedHandle()
    {
        if (isValidHandle(m_handle)) {
            CloseHandle(m_handle);
        }
    }

    HANDLE get() const { return m_handle; }
    void reset(HANDLE handle = nullptr)
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

class ScopedLocalMemory {
public:
    explicit ScopedLocalMemory(void* memory) : m_memory(memory) { }
    ~ScopedLocalMemory()
    {
        if (m_memory != nullptr) {
            LocalFree(m_memory);
        }
    }

private:
    ScopedLocalMemory(const ScopedLocalMemory&);
    ScopedLocalMemory& operator=(const ScopedLocalMemory&);

    void* m_memory;
};

bool isSystemSid(PSID sid)
{
    if (sid == nullptr) {
        return false;
    }
    alignas(DWORD) BYTE buffer[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(buffer);
    return CreateWellKnownSid(WinLocalSystemSid, nullptr, buffer, &size) &&
        EqualSid(sid, buffer);
}

std::string uniqueTemporaryPath(const char* suffix)
{
    char temporaryDirectory[MAX_PATH + 1] = {0};
    const DWORD directoryLength = GetTempPathA(
        static_cast<DWORD>(sizeof(temporaryDirectory)), temporaryDirectory);
    if (directoryLength == 0 || directoryLength >= sizeof(temporaryDirectory)) {
        return std::string();
    }

    char temporaryFile[MAX_PATH + 1] = {0};
    if (GetTempFileNameA(temporaryDirectory, "wsv", 0, temporaryFile) == 0) {
        return std::string();
    }
    DeleteFileA(temporaryFile);
    return std::string(temporaryFile) + suffix;
}

std::wstring uniqueTemporaryWidePath(const wchar_t* suffix)
{
    wchar_t temporaryDirectory[MAX_PATH + 1] = {0};
    const DWORD directoryLength = GetTempPathW(
        static_cast<DWORD>(sizeof(temporaryDirectory) /
                           sizeof(temporaryDirectory[0])),
        temporaryDirectory);
    if (directoryLength == 0 ||
        directoryLength >= sizeof(temporaryDirectory) /
                               sizeof(temporaryDirectory[0])) {
        return std::wstring();
    }

    wchar_t temporaryFile[MAX_PATH + 1] = {0};
    if (GetTempFileNameW(
            temporaryDirectory, L"wsv", 0, temporaryFile) == 0) {
        return std::wstring();
    }
    DeleteFileW(temporaryFile);
    return std::wstring(temporaryFile) + suffix;
}

std::string utf8FromWide(const std::wstring& value)
{
    if (value.empty()) {
        return std::string();
    }
    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return std::string();
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), &result[0], count,
            nullptr, nullptr) != count) {
        return std::string();
    }
    return result;
}

std::wstring wideFromActiveCodePage(const std::string& value)
{
    if (value.empty()) {
        return std::wstring();
    }
    const int count = MultiByteToWideChar(
        CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(
            CP_ACP, 0, value.data(), static_cast<int>(value.size()),
            &result[0], count) != count) {
        return std::wstring();
    }
    return result;
}

} // namespace

TEST(MSWindowsServiceDataDirectoryTests, descriptorParsesWithSystemOwnerAndGroup)
{
    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorA(
        ServiceDataDirectoryPolicy::protectedDirectorySddl(),
        SDDL_REVISION_1, &rawDescriptor, nullptr));
    ScopedLocalMemory descriptor(rawDescriptor);

    PSID owner = nullptr;
    PSID group = nullptr;
    BOOL ownerDefaulted = FALSE;
    BOOL groupDefaulted = FALSE;
    ASSERT_TRUE(GetSecurityDescriptorOwner(
        rawDescriptor, &owner, &ownerDefaulted));
    ASSERT_TRUE(GetSecurityDescriptorGroup(
        rawDescriptor, &group, &groupDefaulted));
    EXPECT_TRUE(isSystemSid(owner));
    EXPECT_TRUE(isSystemSid(group));

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    ASSERT_TRUE(GetSecurityDescriptorControl(
        rawDescriptor, &control, &revision));
    EXPECT_NE(0, control & SE_DACL_PROTECTED);
}

TEST(MSWindowsServiceDataDirectoryTests, hardensRealDirectoryThroughHandle)
{
    const std::string path = uniqueTemporaryPath("-directory");
    ASSERT_FALSE(path.empty());
    ASSERT_TRUE(CreateDirectoryA(path.c_str(), nullptr));

    const ServiceDataDirectoryResult result =
        ensureProtectedServiceDataDirectory(path);
    if (!result.success() &&
        (result.systemError == ERROR_ACCESS_DENIED ||
         result.systemError == ERROR_PRIVILEGE_NOT_HELD ||
         result.systemError == ERROR_NOT_ALL_ASSIGNED)) {
        RemoveDirectoryA(path.c_str());
        GTEST_SKIP() << "test token cannot assign the LocalSystem owner";
    }
    ASSERT_TRUE(result.success())
        << "error=" << static_cast<int>(result.error)
        << " win32=" << result.systemError;

    ScopedHandle directory(CreateFileA(
        path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    ASSERT_TRUE(isValidHandle(directory.get()));

    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    PSID owner = nullptr;
    PSID group = nullptr;
    PACL dacl = nullptr;
    const DWORD securityResult = GetSecurityInfo(
        directory.get(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
            DACL_SECURITY_INFORMATION,
        &owner, &group, &dacl, nullptr, &rawDescriptor);
    ASSERT_EQ(ERROR_SUCCESS, securityResult);
    ScopedLocalMemory descriptor(rawDescriptor);
    EXPECT_TRUE(isSystemSid(owner));
    EXPECT_TRUE(isSystemSid(group));
    ASSERT_NE(nullptr, dacl);
    EXPECT_EQ(2, dacl->AceCount);

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    ASSERT_TRUE(GetSecurityDescriptorControl(
        rawDescriptor, &control, &revision));
    EXPECT_NE(0, control & SE_DACL_PROTECTED);

    directory.reset();
    EXPECT_TRUE(RemoveDirectoryA(path.c_str()));
}

TEST(MSWindowsServiceDataDirectoryTests, hardensUnicodeUtf8DirectoryThroughWidePath)
{
    const std::wstring widePath = uniqueTemporaryWidePath(
        L"-\x8def\x5f84-\x03a9-directory");
    const std::string utf8Path = utf8FromWide(widePath);
    ASSERT_FALSE(widePath.empty());
    ASSERT_FALSE(utf8Path.empty());
    ASSERT_TRUE(CreateDirectoryW(widePath.c_str(), nullptr));

    const ServiceDataDirectoryResult result =
        ensureProtectedServiceDataDirectory(utf8Path);
    const std::wstring acpPath = wideFromActiveCodePage(utf8Path);
    if (!acpPath.empty() && acpPath != widePath &&
        GetFileAttributesW(acpPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        RemoveDirectoryW(acpPath.c_str());
        RemoveDirectoryW(widePath.c_str());
        FAIL() << "UTF-8 bytes selected an active-code-page sibling path";
    }
    if (!result.success() &&
        result.error == ServiceDataDirectoryError::kPrivilege &&
        (result.systemError == ERROR_ACCESS_DENIED ||
         result.systemError == ERROR_PRIVILEGE_NOT_HELD ||
         result.systemError == ERROR_NOT_ALL_ASSIGNED)) {
        RemoveDirectoryW(widePath.c_str());
        GTEST_SKIP() << "test token cannot assign the LocalSystem owner";
    }
    ASSERT_TRUE(result.success())
        << "error=" << static_cast<int>(result.error)
        << " win32=" << result.systemError;

    ScopedHandle directory(CreateFileW(
        widePath.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    ASSERT_TRUE(isValidHandle(directory.get()));
    directory.reset();
    EXPECT_TRUE(RemoveDirectoryW(widePath.c_str()));
}

TEST(MSWindowsServiceDataDirectoryTests, rejectsInvalidUtf8Path)
{
    const std::string invalidPath("C:\\weave-invalid-\xc3\x28", 19);

    const ServiceDataDirectoryResult result =
        ensureProtectedServiceDataDirectory(invalidPath);

    EXPECT_EQ(ServiceDataDirectoryError::kInvalidPath, result.error);
    EXPECT_EQ(ERROR_NO_UNICODE_TRANSLATION, result.systemError);
}

TEST(MSWindowsServiceDataDirectoryTests, rejectsFinalDirectoryReparsePoint)
{
    const std::string target = uniqueTemporaryPath("-target");
    const std::string link = uniqueTemporaryPath("-link");
    ASSERT_FALSE(target.empty());
    ASSERT_FALSE(link.empty());
    ASSERT_TRUE(CreateDirectoryA(target.c_str(), nullptr));

    typedef BOOLEAN (WINAPI *CreateSymbolicLinkFunction)(
        LPCSTR, LPCSTR, DWORD);
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    CreateSymbolicLinkFunction createSymbolicLink =
        reinterpret_cast<CreateSymbolicLinkFunction>(
            GetProcAddress(kernel, "CreateSymbolicLinkA"));
    const bool linked = createSymbolicLink != nullptr &&
        (createSymbolicLink(link.c_str(), target.c_str(), 0x1 | 0x2) ||
         createSymbolicLink(link.c_str(), target.c_str(), 0x1));
    if (!linked) {
        RemoveDirectoryA(target.c_str());
        GTEST_SKIP() << "directory symbolic links are unavailable";
    }

    const ServiceDataDirectoryResult result =
        ensureProtectedServiceDataDirectory(link);
    EXPECT_EQ(ServiceDataDirectoryError::kUnsafeObject, result.error);

    EXPECT_TRUE(RemoveDirectoryA(link.c_str()));
    EXPECT_TRUE(RemoveDirectoryA(target.c_str()));
}
