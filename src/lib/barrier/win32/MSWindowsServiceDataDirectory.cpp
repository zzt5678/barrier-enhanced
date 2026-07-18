/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/win32/MSWindowsServiceDataDirectory.h"

#include "barrier/ServiceDataDirectoryPolicy.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Aclapi.h>
#include <Sddl.h>

#include <limits>

namespace {

bool isValidHandle(HANDLE handle)
{
    return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) : m_handle(handle) { }
    ~ScopedHandle()
    {
        if (isValidHandle(m_handle)) {
            CloseHandle(m_handle);
        }
    }

    HANDLE get() const { return m_handle; }
    void reset(HANDLE handle)
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
    explicit ScopedLocalMemory(void* memory = nullptr) : m_memory(memory) { }
    ~ScopedLocalMemory()
    {
        if (m_memory != nullptr) {
            LocalFree(m_memory);
        }
    }

    void* get() const { return m_memory; }

private:
    ScopedLocalMemory(const ScopedLocalMemory&);
    ScopedLocalMemory& operator=(const ScopedLocalMemory&);

    void* m_memory;
};

ServiceDataDirectoryResult failure(ServiceDataDirectoryError error,
                                   DWORD systemError)
{
    ServiceDataDirectoryResult result;
    result.error = error;
    result.systemError = systemError;
    return result;
}

bool strictUtf8PathToWide(const std::string& path, std::wstring& widePath,
                          DWORD& error)
{
    widePath.clear();
    if (path.empty() || path.size() >
            static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        error = path.empty() ? ERROR_INVALID_NAME : ERROR_FILENAME_EXCED_RANGE;
        return false;
    }

    const int byteCount = static_cast<int>(path.size());
    const int wideCount = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), byteCount,
        nullptr, 0);
    if (wideCount <= 0) {
        error = GetLastError();
        return false;
    }

    widePath.resize(static_cast<std::size_t>(wideCount));
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), byteCount,
            &widePath[0], wideCount) != wideCount) {
        error = GetLastError();
        widePath.clear();
        return false;
    }
    if (widePath.find(L'\0') != std::wstring::npos) {
        error = ERROR_INVALID_NAME;
        widePath.clear();
        return false;
    }

    error = ERROR_SUCCESS;
    return true;
}

class ScopedRequiredPrivileges {
public:
    ScopedRequiredPrivileges() : m_restore(false), m_error(ERROR_SUCCESS)
    {
        HANDLE rawToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(),
                              TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                              &rawToken)) {
            m_error = GetLastError();
            return;
        }
        m_token.reset(rawToken);

        static const char* const names[] = {
            "SeBackupPrivilege",
            "SeRestorePrivilege",
            "SeTakeOwnershipPrivilege"
        };
        const std::size_t privilegeCount = sizeof(names) / sizeof(names[0]);
        PrivilegeSet requested;
        ZeroMemory(&requested, sizeof(requested));
        requested.PrivilegeCount = static_cast<DWORD>(privilegeCount);
        for (std::size_t i = 0; i < privilegeCount; ++i) {
            if (!LookupPrivilegeValueA(nullptr, names[i],
                                       &requested.Privileges[i].Luid)) {
                m_error = GetLastError();
                return;
            }
            requested.Privileges[i].Attributes = SE_PRIVILEGE_ENABLED;
        }

        ZeroMemory(&m_previous, sizeof(m_previous));
        DWORD previousSize = sizeof(m_previous);
        SetLastError(ERROR_SUCCESS);
        if (!AdjustTokenPrivileges(
                m_token.get(), FALSE,
                reinterpret_cast<TOKEN_PRIVILEGES*>(&requested), previousSize,
                reinterpret_cast<TOKEN_PRIVILEGES*>(&m_previous),
                &previousSize)) {
            m_error = GetLastError();
            return;
        }
        m_restore = true;
        const DWORD adjustError = GetLastError();
        if (adjustError == ERROR_NOT_ALL_ASSIGNED) {
            m_error = adjustError;
            return;
        }
    }

    ~ScopedRequiredPrivileges()
    {
        if (m_restore && isValidHandle(m_token.get())) {
            AdjustTokenPrivileges(
                m_token.get(), FALSE,
                reinterpret_cast<TOKEN_PRIVILEGES*>(&m_previous),
                0, nullptr, nullptr);
        }
    }

    bool success() const { return m_error == ERROR_SUCCESS; }
    DWORD error() const { return m_error; }

private:
    struct PrivilegeSet {
        DWORD PrivilegeCount;
        LUID_AND_ATTRIBUTES Privileges[3];
    };

    ScopedRequiredPrivileges(const ScopedRequiredPrivileges&);
    ScopedRequiredPrivileges& operator=(const ScopedRequiredPrivileges&);

    ScopedHandle m_token;
    PrivilegeSet m_previous;
    bool m_restore;
    DWORD m_error;
};

bool descriptorParts(PSECURITY_DESCRIPTOR descriptor,
                     PSID& owner, PSID& group, PACL& dacl)
{
    BOOL defaulted = FALSE;
    BOOL daclPresent = FALSE;
    owner = nullptr;
    group = nullptr;
    dacl = nullptr;
    return GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) &&
        owner != nullptr &&
        GetSecurityDescriptorGroup(descriptor, &group, &defaulted) &&
        group != nullptr &&
        GetSecurityDescriptorDacl(
            descriptor, &daclPresent, &dacl, &defaulted) &&
        daclPresent && dacl != nullptr;
}

bool hasExpectedAccessAce(PACL dacl, DWORD index, PSID expectedSid)
{
    void* rawAce = nullptr;
    if (!GetAce(dacl, index, &rawAce) || rawAce == nullptr) {
        return false;
    }
    const ACCESS_ALLOWED_ACE* ace =
        static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
    const BYTE expectedFlags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    return ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
        ace->Header.AceFlags == expectedFlags &&
        ace->Mask == FILE_ALL_ACCESS &&
        EqualSid(const_cast<DWORD*>(&ace->SidStart), expectedSid);
}

bool securityMatchesPolicy(HANDLE directory, PSID expectedOwner,
                           PSID expectedGroup, PSID expectedSystem,
                           PSID expectedAdministrators, DWORD& error)
{
    PSID owner = nullptr;
    PSID group = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    const DWORD queryResult = GetSecurityInfo(
        directory, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
            DACL_SECURITY_INFORMATION,
        &owner, &group, &dacl, nullptr, &rawDescriptor);
    if (queryResult != ERROR_SUCCESS) {
        error = queryResult;
        return false;
    }
    ScopedLocalMemory descriptor(rawDescriptor);

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    ACL_SIZE_INFORMATION aclInfo;
    ZeroMemory(&aclInfo, sizeof(aclInfo));
    if (owner == nullptr || group == nullptr ||
        !EqualSid(owner, expectedOwner) || !EqualSid(group, expectedGroup) ||
        !GetSecurityDescriptorControl(
            rawDescriptor, &control, &revision) ||
        (control & SE_DACL_PROTECTED) == 0 || dacl == nullptr ||
        !GetAclInformation(
            dacl, &aclInfo, sizeof(aclInfo), AclSizeInformation) ||
        aclInfo.AceCount != 2 ||
        !hasExpectedAccessAce(dacl, 0, expectedSystem) ||
        !hasExpectedAccessAce(dacl, 1, expectedAdministrators)) {
        error = ERROR_INVALID_SECURITY_DESCR;
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

} // namespace

ServiceDataDirectoryResult ensureProtectedServiceDataDirectory(
    const std::string& path)
{
    std::wstring widePath;
    DWORD pathError = ERROR_SUCCESS;
    if (!strictUtf8PathToWide(path, widePath, pathError)) {
        return failure(ServiceDataDirectoryError::kInvalidPath, pathError);
    }

    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            ServiceDataDirectoryPolicy::protectedDirectorySddl(),
            SDDL_REVISION_1, &rawDescriptor, nullptr)) {
        return failure(ServiceDataDirectoryError::kSecurityDescriptor,
                       GetLastError());
    }
    ScopedLocalMemory descriptor(rawDescriptor);

    PSID owner = nullptr;
    PSID group = nullptr;
    PACL dacl = nullptr;
    if (!descriptorParts(rawDescriptor, owner, group, dacl)) {
        return failure(ServiceDataDirectoryError::kSecurityDescriptor,
                       ERROR_INVALID_SECURITY_DESCR);
    }

    alignas(DWORD) BYTE administratorsBuffer[SECURITY_MAX_SID_SIZE];
    DWORD administratorsSize = sizeof(administratorsBuffer);
    if (!CreateWellKnownSid(
            WinBuiltinAdministratorsSid, nullptr,
            administratorsBuffer, &administratorsSize)) {
        return failure(ServiceDataDirectoryError::kSecurityDescriptor,
                       GetLastError());
    }

    SECURITY_ATTRIBUTES security;
    ZeroMemory(&security, sizeof(security));
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = rawDescriptor;
    const BOOL created = CreateDirectoryW(widePath.c_str(), &security);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    if (!created && createError != ERROR_ALREADY_EXISTS) {
        return failure(ServiceDataDirectoryError::kCreate, createError);
    }

    // Hold a no-delete-sharing inspection handle when the current DACL permits
    // one. This lets unprivileged tests reject a final reparse point and pins
    // an existing real directory while the privileged handle is opened.
    ScopedHandle inspection(CreateFileW(
        widePath.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (isValidHandle(inspection.get())) {
        BY_HANDLE_FILE_INFORMATION information;
        ZeroMemory(&information, sizeof(information));
        if (!GetFileInformationByHandle(inspection.get(), &information)) {
            return failure(ServiceDataDirectoryError::kOpen, GetLastError());
        }
        if (!ServiceDataDirectoryPolicy::acceptsDirectoryObject(
                true,
                (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)) {
            return failure(ServiceDataDirectoryError::kUnsafeObject,
                           ERROR_REPARSE_TAG_INVALID);
        }
    }

    ScopedRequiredPrivileges privileges;
    if (!privileges.success()) {
        return failure(ServiceDataDirectoryError::kPrivilege,
                       privileges.error());
    }

    const DWORD access = FILE_READ_ATTRIBUTES | READ_CONTROL |
        WRITE_DAC | WRITE_OWNER;
    ScopedHandle directory(CreateFileW(
        widePath.c_str(), access, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!isValidHandle(directory.get())) {
        return failure(ServiceDataDirectoryError::kOpen, GetLastError());
    }

    BY_HANDLE_FILE_INFORMATION information;
    ZeroMemory(&information, sizeof(information));
    if (!GetFileInformationByHandle(directory.get(), &information)) {
        return failure(ServiceDataDirectoryError::kOpen, GetLastError());
    }
    if (!ServiceDataDirectoryPolicy::acceptsDirectoryObject(
            true,
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)) {
        return failure(ServiceDataDirectoryError::kUnsafeObject,
                       ERROR_REPARSE_TAG_INVALID);
    }

    const DWORD securityResult = SetSecurityInfo(
        directory.get(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        owner, group, dacl, nullptr);
    if (securityResult != ERROR_SUCCESS) {
        return failure(ServiceDataDirectoryError::kApplySecurity,
                       securityResult);
    }

    DWORD verificationError = ERROR_SUCCESS;
    if (!securityMatchesPolicy(
            directory.get(), owner, group, owner,
            administratorsBuffer, verificationError)) {
        return failure(ServiceDataDirectoryError::kVerifySecurity,
                       verificationError);
    }

    return ServiceDataDirectoryResult();
}
