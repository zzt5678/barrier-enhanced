/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/win32/MSWindowsServiceLaunchProfile.h"

#include "barrier/ServiceLaunchProfilePolicy.h"
#include "barrier/TransferDigest.h"
#include "barrier/win32/MSWindowsServiceDataDirectory.h"

#include <Aclapi.h>
#include <Sddl.h>
#include <Shlobj.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <vector>

namespace {

const wchar_t kSourceProfileName[] = L"Barrier";
const wchar_t kProductDirectoryName[] = L"Weave";
const wchar_t kLaunchProfilesDirectoryName[] = L"LaunchProfiles";
const char kManifestHeader[] = "WEAVE-LAUNCH-PROFILE 1\n";

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
    HANDLE release()
    {
        HANDLE handle = m_handle;
        m_handle = nullptr;
        return handle;
    }
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
    explicit ScopedLocalMemory(void* memory = nullptr) : m_memory(memory) { }
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

class ScopedCoTaskMemory {
public:
    explicit ScopedCoTaskMemory(wchar_t* memory = nullptr) : m_memory(memory) { }
    ~ScopedCoTaskMemory()
    {
        if (m_memory != nullptr) {
            CoTaskMemFree(m_memory);
        }
    }
private:
    ScopedCoTaskMemory(const ScopedCoTaskMemory&);
    ScopedCoTaskMemory& operator=(const ScopedCoTaskMemory&);
    wchar_t* m_memory;
};

class ScopedImpersonation {
public:
    explicit ScopedImpersonation(HANDLE token) : m_active(false), m_error(0)
    {
        if (ImpersonateLoggedOnUser(token)) {
            m_active = true;
        }
        else {
            m_error = GetLastError();
        }
    }
    ~ScopedImpersonation()
    {
        if (m_active) {
            RevertToSelf();
        }
    }
    bool success() const { return m_active; }
    DWORD error() const { return m_error; }
private:
    ScopedImpersonation(const ScopedImpersonation&);
    ScopedImpersonation& operator=(const ScopedImpersonation&);
    bool m_active;
    DWORD m_error;
};

struct SourceFile {
    const ServiceLaunchFileRule* rule = nullptr;
    std::wstring relativePath;
    std::string contents;
    std::string digest;
};

ServiceLaunchProfileResult failure(ServiceLaunchProfileError error,
                                   DWORD systemError,
                                   const std::string& detail)
{
    ServiceLaunchProfileResult result;
    result.error = error;
    result.systemError = systemError;
    result.detail = detail;
    return result;
}

std::wstring joinPath(const std::wstring& parent, const std::wstring& child)
{
    if (parent.empty()) {
        return child;
    }
    if (parent.back() == L'\\') {
        return parent + child;
    }
    return parent + L"\\" + child;
}

std::wstring relativePathFor(const std::string& path)
{
    std::wstring result;
    result.reserve(path.size());
    for (std::string::const_iterator i = path.begin(); i != path.end(); ++i) {
        result.push_back(*i == '/' ? L'\\' : static_cast<unsigned char>(*i));
    }
    return result;
}

bool knownFolder(const KNOWNFOLDERID& id, HANDLE token, std::wstring& path,
                 DWORD& error)
{
    wchar_t* rawPath = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, 0, token, &rawPath);
    if (FAILED(result) || rawPath == nullptr) {
        error = static_cast<DWORD>(result);
        return false;
    }
    ScopedCoTaskMemory memory(rawPath);
    path.assign(rawPath);
    error = ERROR_SUCCESS;
    return !path.empty();
}

bool tokenSid(HANDLE token, std::string& canonical, PSID& sid,
              std::vector<unsigned char>& storage, DWORD& error)
{
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        error = GetLastError();
        return false;
    }
    storage.resize(size);
    if (!GetTokenInformation(token, TokenUser, storage.data(), size, &size)) {
        error = GetLastError();
        return false;
    }
    TOKEN_USER* user = reinterpret_cast<TOKEN_USER*>(storage.data());
    sid = user->User.Sid;
    char* sidText = nullptr;
    if (!IsValidSid(sid) || !ConvertSidToStringSidA(sid, &sidText)) {
        error = GetLastError();
        return false;
    }
    ScopedLocalMemory text(sidText);
    canonical.assign(sidText);
    error = ERROR_SUCCESS;
    return true;
}

bool suppliedSidMatches(const std::string& supplied, PSID actual)
{
    PSID parsed = nullptr;
    if (!ConvertStringSidToSidA(supplied.c_str(), &parsed)) {
        return false;
    }
    ScopedLocalMemory memory(parsed);
    return IsValidSid(parsed) && EqualSid(parsed, actual);
}

bool canonicalizeSid(const std::string& supplied, std::string& canonical)
{
    PSID parsed = nullptr;
    if (!ConvertStringSidToSidA(supplied.c_str(), &parsed) ||
        !IsValidSid(parsed)) {
        if (parsed != nullptr) {
            LocalFree(parsed);
        }
        return false;
    }
    ScopedLocalMemory parsedMemory(parsed);

    char* rawCanonical = nullptr;
    if (!ConvertSidToStringSidA(parsed, &rawCanonical) ||
        rawCanonical == nullptr) {
        return false;
    }
    ScopedLocalMemory canonicalMemory(rawCanonical);
    canonical.assign(rawCanonical);
    return canonical == supplied;
}

bool isLowerHex(const std::string& value, std::size_t offset,
                std::size_t count)
{
    if (offset > value.size() || count > value.size() - offset) {
        return false;
    }
    for (std::size_t i = offset; i < offset + count; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool isValidGeneration(const std::string& generation)
{
    return generation.size() == 35u &&
        generation.compare(0u, 3u, "v1-") == 0 &&
        isLowerHex(generation, 3u, 32u);
}

bool isValidDigest(const std::string& digest)
{
    return digest.size() == 71u &&
        digest.compare(0u, 7u, "sha256:") == 0 &&
        isLowerHex(digest, 7u, 64u);
}

bool isSafeDirectoryHandle(HANDLE handle, DWORD& error)
{
    BY_HANDLE_FILE_INFORMATION information;
    ZeroMemory(&information, sizeof(information));
    if (!GetFileInformationByHandle(handle, &information)) {
        error = GetLastError();
        return false;
    }
    const DWORD attributes = information.dwFileAttributes;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        error = ERROR_REPARSE_TAG_INVALID;
        return false;
    }
    error = ERROR_SUCCESS;
    return true;
}

bool openSafeDirectory(const std::wstring& path, ScopedHandle& result,
                       DWORD& error)
{
    HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (!isValidHandle(handle)) {
        error = GetLastError();
        return false;
    }
    ScopedHandle candidate(handle);
    if (!isSafeDirectoryHandle(candidate.get(), error)) {
        return false;
    }
    result.reset(candidate.release());
    return true;
}

ServiceLaunchProfileResult readSourceFile(
    const std::wstring& profilePath, const ServiceLaunchFileRule& rule,
    SourceFile& output)
{
    output.rule = &rule;
    output.relativePath = relativePathFor(rule.relativePath);
    const std::wstring path = joinPath(profilePath, output.relativePath);
    ScopedHandle file(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!isValidHandle(file.get())) {
        const DWORD error = GetLastError();
        if (!rule.required &&
            (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
            output.rule = nullptr;
            return ServiceLaunchProfileResult();
        }
        return failure(ServiceLaunchProfileError::kMissingRequiredFile,
                       error, rule.relativePath);
    }

    BY_HANDLE_FILE_INFORMATION information;
    ZeroMemory(&information, sizeof(information));
    if (!GetFileInformationByHandle(file.get(), &information)) {
        return failure(ServiceLaunchProfileError::kRead, GetLastError(),
                       rule.relativePath);
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return failure(ServiceLaunchProfileError::kUnsafeSource,
                       ERROR_REPARSE_TAG_INVALID, rule.relativePath);
    }

    ULARGE_INTEGER size;
    size.HighPart = information.nFileSizeHigh;
    size.LowPart = information.nFileSizeLow;
    if (!isServiceLaunchFileSizeAllowed(rule, size.QuadPart) ||
        size.QuadPart > (std::numeric_limits<std::size_t>::max)()) {
        return failure(ServiceLaunchProfileError::kFileSize,
                       ERROR_FILE_TOO_LARGE, rule.relativePath);
    }

    output.contents.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0u;
    while (offset < output.contents.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            output.contents.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD read = 0;
        if (!ReadFile(file.get(), &output.contents[offset], request,
                      &read, nullptr)) {
            return failure(ServiceLaunchProfileError::kRead,
                           GetLastError(), rule.relativePath);
        }
        if (read == 0) {
            return failure(ServiceLaunchProfileError::kRead,
                           ERROR_HANDLE_EOF, rule.relativePath);
        }
        offset += read;
    }
    return ServiceLaunchProfileResult();
}

bool finishDigest(const std::string& contents, std::string& digest)
{
    barrier::TransferDigest calculator;
    return calculator.isReady() &&
        calculator.update(contents.data(), contents.size()) &&
        calculator.finish(digest);
}

bool validateSourceFiles(ServiceLaunchRole role,
                         std::vector<SourceFile>& files,
                         std::string& detail)
{
    for (std::vector<SourceFile>::iterator file = files.begin();
         file != files.end(); ++file) {
        bool valid = true;
        const std::string relative(file->rule->relativePath);
        if (relative == "SSL/Barrier.pem") {
            valid = validateServiceLaunchPem(file->contents);
        }
        else if (file->rule->fingerprintDatabase) {
            valid = (file->contents.empty() && file->rule->allowEmpty) ||
                validateServiceFingerprintDatabase(file->contents);
        }
        else if (role == ServiceLaunchRole::kServer &&
                 relative == "barrier.sgc") {
            valid = validateServiceLaunchServerConfig(file->contents);
        }
        if (!valid || !finishDigest(file->contents, file->digest)) {
            detail = relative;
            return false;
        }
    }
    return true;
}

bool createGeneration(std::string& generation)
{
    std::array<unsigned char, 16> random;
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
        return false;
    }
    static const char digits[] = "0123456789abcdef";
    generation.assign("v1-");
    generation.reserve(3u + random.size() * 2u);
    for (std::size_t i = 0; i < random.size(); ++i) {
        generation.push_back(digits[random[i] >> 4]);
        generation.push_back(digits[random[i] & 0x0f]);
    }
    return true;
}

std::wstring widenAscii(const std::string& value)
{
    return std::wstring(value.begin(), value.end());
}

ServiceLaunchProfileResult protectDirectory(const std::wstring& path)
{
    const int byteCount = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    if (byteCount <= 1) {
        return failure(ServiceLaunchProfileError::kDestination,
                       ERROR_NO_UNICODE_TRANSLATION,
                       "service data directory path encoding");
    }
    std::string utf8Path(static_cast<std::size_t>(byteCount), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), -1,
            &utf8Path[0], byteCount, nullptr, nullptr) != byteCount) {
        return failure(ServiceLaunchProfileError::kDestination,
                       ERROR_NO_UNICODE_TRANSLATION,
                       "service data directory path encoding");
    }
    utf8Path.resize(utf8Path.size() - 1u);
    const ServiceDataDirectoryResult protectedResult =
        ensureProtectedServiceDataDirectory(utf8Path);
    if (!protectedResult.success()) {
        return failure(ServiceLaunchProfileError::kDestination,
                       protectedResult.systemError, "protected directory");
    }
    return ServiceLaunchProfileResult();
}

ServiceLaunchProfileResult createProtectedDirectory(const std::wstring& path,
                                                     bool requireNew)
{
    if (requireNew) {
        if (!CreateDirectoryW(path.c_str(), nullptr)) {
            return failure(ServiceLaunchProfileError::kDestination,
                           GetLastError(), "generation directory");
        }
    }
    return protectDirectory(path);
}

ServiceLaunchProfileResult writeNewFile(const std::wstring& path,
                                        const std::string& contents)
{
    ScopedHandle file(CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!isValidHandle(file.get())) {
        return failure(ServiceLaunchProfileError::kWrite, GetLastError(),
                       "create staged file");
    }
    std::size_t offset = 0u;
    while (offset < contents.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            contents.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(file.get(), contents.data() + offset, request,
                       &written, nullptr)) {
            return failure(ServiceLaunchProfileError::kWrite, GetLastError(),
                           "write staged file");
        }
        if (written == 0) {
            return failure(ServiceLaunchProfileError::kWrite,
                           ERROR_WRITE_FAULT, "write staged file");
        }
        offset += written;
    }
    if (!FlushFileBuffers(file.get())) {
        return failure(ServiceLaunchProfileError::kWrite, GetLastError(),
                       "flush staged file");
    }
    return ServiceLaunchProfileResult();
}

bool removeProfileTree(const std::wstring& profilePath)
{
    const wchar_t* files[] = {
        L"SSL\\Barrier.pem",
        L"SSL\\Fingerprints\\TrustedServers.txt",
        L"SSL\\Fingerprints\\TrustedClients.txt",
        L"SSL\\Fingerprints\\Local.txt",
        L"barrier.sgc",
        L"profile.manifest"
    };
    for (std::size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        const std::wstring path = joinPath(profilePath, files[i]);
        if (!DeleteFileW(path.c_str())) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                return false;
            }
        }
    }

    const std::wstring fingerprints =
        joinPath(profilePath, L"SSL\\Fingerprints");
    const std::wstring ssl = joinPath(profilePath, L"SSL");
    if (!RemoveDirectoryW(fingerprints.c_str())) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return false;
        }
    }
    if (!RemoveDirectoryW(ssl.c_str())) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return false;
        }
    }
    if (!RemoveDirectoryW(profilePath.c_str())) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            return false;
        }
    }
    return true;
}

class StagedProfileGuard {
public:
    explicit StagedProfileGuard(const std::wstring& path) :
        m_path(path),
        m_active(true)
    {
    }

    ~StagedProfileGuard()
    {
        if (m_active) {
            removeProfileTree(m_path);
        }
    }

    void release() { m_active = false; }

private:
    StagedProfileGuard(const StagedProfileGuard&);
    StagedProfileGuard& operator=(const StagedProfileGuard&);

    std::wstring m_path;
    bool m_active;
};

std::string buildManifest(const std::string& ownerSid,
                          ServiceLaunchRole role,
                          const std::string& generation,
                          const std::string& profileDigest,
                          const std::vector<SourceFile>& files)
{
    std::ostringstream manifest;
    manifest << kManifestHeader
             << "owner-sid=" << ownerSid << "\n"
             << "role=" << serviceLaunchRoleName(role) << "\n"
             << "generation=" << generation << "\n"
             << "profile-digest=" << profileDigest << "\n";
    for (std::vector<SourceFile>::const_iterator file = files.begin();
         file != files.end(); ++file) {
        manifest << "file=" << file->rule->relativePath << "\t"
                 << file->contents.size() << "\t" << file->digest << "\n";
    }
    return manifest.str();
}

bool profileDigest(const std::string& ownerSid, ServiceLaunchRole role,
                   const std::vector<SourceFile>& files, std::string& digest)
{
    barrier::TransferDigest calculator;
    const std::string header = std::string(kManifestHeader) + ownerSid + "\n" +
        serviceLaunchRoleName(role) + "\n";
    if (!calculator.isReady() ||
        !calculator.update(header.data(), header.size())) {
        return false;
    }
    for (std::vector<SourceFile>::const_iterator file = files.begin();
         file != files.end(); ++file) {
        const std::string path(file->rule->relativePath);
        const std::string size = std::to_string(file->contents.size());
        if (!calculator.update(path.data(), path.size()) ||
            !calculator.update("\n", 1u) ||
            !calculator.update(size.data(), size.size()) ||
            !calculator.update("\n", 1u) ||
            !calculator.update(file->contents.data(), file->contents.size()) ||
            !calculator.update("\n", 1u)) {
            return false;
        }
    }
    return calculator.finish(digest);
}

} // namespace

ServiceLaunchProfileResult stageWindowsServiceLaunchProfile(
    HANDLE authenticatedUserToken,
    const std::string& authenticatedUserSid,
    ServiceLaunchRole role,
    const std::string& reusableGeneration,
    const std::string& reusableDigest)
{
    if (!isValidHandle(authenticatedUserToken) || authenticatedUserSid.empty() ||
        (role != ServiceLaunchRole::kClient &&
         role != ServiceLaunchRole::kServer)) {
        return failure(ServiceLaunchProfileError::kInvalidArgument,
                       ERROR_INVALID_PARAMETER, "token, SID, or role");
    }

    std::vector<unsigned char> tokenStorage;
    std::string ownerSid;
    PSID actualSid = nullptr;
    DWORD systemError = ERROR_SUCCESS;
    if (!tokenSid(authenticatedUserToken, ownerSid, actualSid,
                  tokenStorage, systemError)) {
        return failure(ServiceLaunchProfileError::kInvalidArgument,
                       systemError, "read token SID");
    }
    if (!suppliedSidMatches(authenticatedUserSid, actualSid)) {
        return failure(ServiceLaunchProfileError::kIdentityMismatch,
                       ERROR_NONE_MAPPED, "authenticated SID mismatch");
    }

    std::vector<SourceFile> files;
    {
        ScopedImpersonation impersonation(authenticatedUserToken);
        if (!impersonation.success()) {
            return failure(ServiceLaunchProfileError::kImpersonation,
                           impersonation.error(), "impersonate source user");
        }

        std::wstring localAppData;
        if (!knownFolder(FOLDERID_LocalAppData, authenticatedUserToken,
                         localAppData, systemError)) {
            return failure(ServiceLaunchProfileError::kKnownFolder,
                           systemError, "FOLDERID_LocalAppData");
        }
        const std::wstring profilePath =
            joinPath(localAppData, kSourceProfileName);

        // Pin every directory involved in the fixed whitelist. No handle
        // permits delete sharing, so these path components cannot be swapped
        // while their children are opened.
        ScopedHandle profileDirectory;
        ScopedHandle sslDirectory;
        ScopedHandle fingerprintsDirectory;
        if (!openSafeDirectory(profilePath, profileDirectory, systemError) ||
            !openSafeDirectory(joinPath(profilePath, L"SSL"),
                               sslDirectory, systemError)) {
            return failure(ServiceLaunchProfileError::kUnsafeSource,
                           systemError, "profile directory hierarchy");
        }
        const std::wstring fingerprintsPath =
            joinPath(profilePath, L"SSL\\Fingerprints");
        if (!openSafeDirectory(fingerprintsPath,
                               fingerprintsDirectory, systemError) &&
            systemError != ERROR_FILE_NOT_FOUND &&
            systemError != ERROR_PATH_NOT_FOUND) {
            return failure(ServiceLaunchProfileError::kUnsafeSource,
                           systemError, "fingerprint directory");
        }

        std::size_t ruleCount = 0u;
        const ServiceLaunchFileRule* rules =
            serviceLaunchFileRules(role, ruleCount);
        for (std::size_t i = 0; i < ruleCount; ++i) {
            SourceFile source;
            ServiceLaunchProfileResult readResult =
                readSourceFile(profilePath, rules[i], source);
            if (!readResult.success()) {
                return readResult;
            }
            if (source.rule != nullptr) {
                files.push_back(source);
            }
        }
    }

    std::string invalidDetail;
    if (!validateSourceFiles(role, files, invalidDetail)) {
        return failure(ServiceLaunchProfileError::kValidation,
                       ERROR_INVALID_DATA, invalidDetail);
    }

    std::string digest;
    std::string sidDigest;
    if (!profileDigest(ownerSid, role, files, digest) ||
        !finishDigest(ownerSid, sidDigest) || sidDigest.size() <= 7u) {
        return failure(ServiceLaunchProfileError::kDigest,
                       ERROR_INVALID_DATA, "SHA-256");
    }
    const std::string sidHash = sidDigest.substr(7u);

    if (digest == reusableDigest &&
        isValidGeneration(reusableGeneration)) {
        ServiceLaunchProfileResult reusable =
            loadWindowsServiceLaunchProfile(
                ownerSid, reusableGeneration, reusableDigest, role);
        if (reusable.success()) {
            return reusable;
        }
    }

    std::wstring programData;
    if (!knownFolder(FOLDERID_ProgramData, nullptr,
                     programData, systemError)) {
        return failure(ServiceLaunchProfileError::kKnownFolder,
                       systemError, "FOLDERID_ProgramData");
    }

    const std::wstring productRoot =
        joinPath(programData, kProductDirectoryName);
    const std::wstring launchRoot =
        joinPath(productRoot, kLaunchProfilesDirectoryName);
    const std::wstring ownerRoot = joinPath(launchRoot, widenAscii(sidHash));
    const std::wstring roots[] = {productRoot, launchRoot, ownerRoot};
    for (std::size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        ServiceLaunchProfileResult directoryResult =
            createProtectedDirectory(roots[i], false);
        if (!directoryResult.success()) {
            return directoryResult;
        }
    }

    std::string generation;
    if (!createGeneration(generation)) {
        return failure(ServiceLaunchProfileError::kDigest,
                       ERROR_INVALID_DATA, "generation entropy");
    }
    const std::wstring profilePath =
        joinPath(ownerRoot, widenAscii(generation));
    ServiceLaunchProfileResult directoryResult =
        createProtectedDirectory(profilePath, true);
    if (!directoryResult.success()) {
        return directoryResult;
    }
    StagedProfileGuard stagedProfile(profilePath);

    const std::wstring sslPath = joinPath(profilePath, L"SSL");
    const std::wstring fingerprintsPath = joinPath(sslPath, L"Fingerprints");
    const std::wstring stagingDirectories[] = {sslPath, fingerprintsPath};
    for (std::size_t i = 0;
         i < sizeof(stagingDirectories) / sizeof(stagingDirectories[0]); ++i) {
        directoryResult = createProtectedDirectory(stagingDirectories[i], false);
        if (!directoryResult.success()) {
            return directoryResult;
        }
    }

    for (std::vector<SourceFile>::const_iterator file = files.begin();
         file != files.end(); ++file) {
        ServiceLaunchProfileResult writeResult = writeNewFile(
            joinPath(profilePath, file->relativePath), file->contents);
        if (!writeResult.success()) {
            writeResult.detail = file->rule->relativePath;
            return writeResult;
        }
    }

    const std::string manifest =
        buildManifest(ownerSid, role, generation, digest, files);
    ServiceLaunchProfileResult manifestResult = writeNewFile(
        joinPath(profilePath, L"profile.manifest"), manifest);
    if (!manifestResult.success()) {
        return manifestResult;
    }

    ServiceLaunchProfileResult result;
    result.profilePath = profilePath;
    result.generation = generation;
    result.digest = digest;
    result.ownerSid = ownerSid;
    result.newlyStaged = true;
    stagedProfile.release();
    return result;
}

bool discardWindowsServiceLaunchProfile(
    const ServiceLaunchProfileResult& stagedProfile)
{
    std::string canonicalOwnerSid;
    if (!stagedProfile.success() || !stagedProfile.newlyStaged ||
        stagedProfile.profilePath.empty() ||
        !canonicalizeSid(stagedProfile.ownerSid, canonicalOwnerSid) ||
        !isValidGeneration(stagedProfile.generation)) {
        return false;
    }

    std::string sidDigest;
    if (!finishDigest(canonicalOwnerSid, sidDigest) || sidDigest.size() != 71u) {
        return false;
    }

    DWORD systemError = ERROR_SUCCESS;
    std::wstring programData;
    if (!knownFolder(FOLDERID_ProgramData, nullptr, programData, systemError)) {
        return false;
    }
    const std::wstring expected = joinPath(
        joinPath(joinPath(joinPath(programData, kProductDirectoryName),
                          kLaunchProfilesDirectoryName),
                 widenAscii(sidDigest.substr(7u))),
        widenAscii(stagedProfile.generation));
    if (_wcsicmp(expected.c_str(), stagedProfile.profilePath.c_str()) != 0) {
        return false;
    }

    ScopedHandle directory;
    if (!openSafeDirectory(expected, directory, systemError)) {
        return systemError == ERROR_FILE_NOT_FOUND ||
            systemError == ERROR_PATH_NOT_FOUND;
    }
    directory.reset();
    return removeProfileTree(expected);
}

ServiceLaunchProfileResult loadWindowsServiceLaunchProfile(
    const std::string& ownerSid,
    const std::string& generation,
    const std::string& digest,
    ServiceLaunchRole role)
{
    std::string canonicalOwnerSid;
    if ((role != ServiceLaunchRole::kClient &&
         role != ServiceLaunchRole::kServer) ||
        !canonicalizeSid(ownerSid, canonicalOwnerSid) ||
        !isValidGeneration(generation) || !isValidDigest(digest)) {
        return failure(ServiceLaunchProfileError::kInvalidArgument,
                       ERROR_INVALID_PARAMETER,
                       "SID, generation, digest, or role");
    }

    std::string sidDigest;
    if (!finishDigest(canonicalOwnerSid, sidDigest) ||
        sidDigest.size() != 71u) {
        return failure(ServiceLaunchProfileError::kDigest,
                       ERROR_INVALID_DATA, "owner SID digest");
    }
    const std::string sidHash = sidDigest.substr(7u);

    DWORD systemError = ERROR_SUCCESS;
    std::wstring programData;
    if (!knownFolder(FOLDERID_ProgramData, nullptr,
                     programData, systemError)) {
        return failure(ServiceLaunchProfileError::kKnownFolder,
                       systemError, "FOLDERID_ProgramData");
    }

    const std::wstring productRoot =
        joinPath(programData, kProductDirectoryName);
    const std::wstring launchRoot =
        joinPath(productRoot, kLaunchProfilesDirectoryName);
    const std::wstring ownerRoot = joinPath(launchRoot, widenAscii(sidHash));
    const std::wstring profilePath =
        joinPath(ownerRoot, widenAscii(generation));
    const std::wstring sslPath = joinPath(profilePath, L"SSL");
    const std::wstring fingerprintsPath = joinPath(sslPath, L"Fingerprints");

    // Pin every existing path component without delete sharing. A restart
    // never repairs or creates these directories; any missing or unsafe
    // component is a hard failure.
    ScopedHandle productDirectory;
    ScopedHandle launchDirectory;
    ScopedHandle ownerDirectory;
    ScopedHandle profileDirectory;
    ScopedHandle sslDirectory;
    ScopedHandle fingerprintsDirectory;
    if (!openSafeDirectory(productRoot, productDirectory, systemError) ||
        !openSafeDirectory(launchRoot, launchDirectory, systemError) ||
        !openSafeDirectory(ownerRoot, ownerDirectory, systemError) ||
        !openSafeDirectory(profilePath, profileDirectory, systemError) ||
        !openSafeDirectory(sslPath, sslDirectory, systemError)) {
        return failure(ServiceLaunchProfileError::kUnsafeSource,
                       systemError, "committed profile hierarchy");
    }
    if (!openSafeDirectory(fingerprintsPath,
                           fingerprintsDirectory, systemError) &&
        systemError != ERROR_FILE_NOT_FOUND &&
        systemError != ERROR_PATH_NOT_FOUND) {
        return failure(ServiceLaunchProfileError::kUnsafeSource,
                       systemError, "fingerprint directory");
    }

    std::vector<SourceFile> files;
    std::size_t ruleCount = 0u;
    const ServiceLaunchFileRule* rules =
        serviceLaunchFileRules(role, ruleCount);
    for (std::size_t i = 0; i < ruleCount; ++i) {
        SourceFile source;
        ServiceLaunchProfileResult readResult =
            readSourceFile(profilePath, rules[i], source);
        if (!readResult.success()) {
            return readResult;
        }
        if (source.rule != nullptr) {
            files.push_back(source);
        }
    }

    std::string invalidDetail;
    if (!validateSourceFiles(role, files, invalidDetail)) {
        return failure(ServiceLaunchProfileError::kValidation,
                       ERROR_INVALID_DATA, invalidDetail);
    }

    std::string actualDigest;
    if (!profileDigest(canonicalOwnerSid, role, files, actualDigest) ||
        actualDigest != digest) {
        return failure(ServiceLaunchProfileError::kDigest,
                       ERROR_INVALID_DATA, "profile digest mismatch");
    }

    const std::string expectedManifest = buildManifest(
        canonicalOwnerSid, role, generation, actualDigest, files);
    const ServiceLaunchFileRule profileManifestRule = {
        "profile.manifest", 64u * 1024u, true, false, false
    };
    SourceFile profileManifest;
    ServiceLaunchProfileResult manifestResult = readSourceFile(
        profilePath, profileManifestRule, profileManifest);
    if (!manifestResult.success()) {
        return manifestResult;
    }
    if (profileManifest.contents != expectedManifest) {
        return failure(ServiceLaunchProfileError::kValidation,
                       ERROR_INVALID_DATA, "manifest mismatch");
    }

    ServiceLaunchProfileResult result;
    result.profilePath = profilePath;
    result.generation = generation;
    result.digest = actualDigest;
    result.ownerSid = canonicalOwnerSid;
    return result;
}
