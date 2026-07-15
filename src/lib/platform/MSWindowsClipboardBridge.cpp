/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "platform/MSWindowsClipboardBridge.h"

#include "barrier/Clipboard.h"
#include "base/Log.h"
#include "common/win32/SessionUserImpersonation.h"
#include "platform/MSWindowsClipboard.h"

#include <Wtsapi32.h>
#include <UserEnv.h>
#include <sddl.h>
#include <wincrypt.h>

#include <iomanip>
#include <sstream>
#include <cstring>
#include <vector>

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

namespace {

const DWORD kConnectTimeoutMs = 1500;
const DWORD kIoTimeoutMs = 1000;
const DWORD kHelperExitTimeoutMs = 1000;
const ULONGLONG kStartRetryDelayMs = 1000;
const char kPipePrefix[] = "\\\\.\\pipe\\WeaveClipboard-";

bool isValidHandle(HANDLE handle)
{
    return handle != NULL && handle != INVALID_HANDLE_VALUE;
}

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = NULL) :
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
        if (m_environment != NULL) {
            DestroyEnvironmentBlock(m_environment);
        }
    }

    LPVOID* out()
    {
        return &m_environment;
    }

    LPVOID get() const
    {
        return m_environment;
    }

private:
    LPVOID m_environment;
};

class ScopedLocalMemory {
public:
    explicit ScopedLocalMemory(HLOCAL memory = NULL) :
        m_memory(memory)
    {
    }

    ~ScopedLocalMemory()
    {
        if (m_memory != NULL) {
            LocalFree(m_memory);
        }
    }

private:
    ScopedLocalMemory(const ScopedLocalMemory&);
    ScopedLocalMemory& operator=(const ScopedLocalMemory&);

    HLOCAL m_memory;
};

std::string windowsError(const char* operation, DWORD error)
{
    std::ostringstream stream;
    stream << operation << " failed with Windows error " << error;
    return stream.str();
}

UInt32 readBigEndianUInt32(const char* data)
{
    const unsigned char* bytes =
        reinterpret_cast<const unsigned char*>(data);
    return (static_cast<UInt32>(bytes[0]) << 24) |
           (static_cast<UInt32>(bytes[1]) << 16) |
           (static_cast<UInt32>(bytes[2]) << 8) |
            static_cast<UInt32>(bytes[3]);
}

bool transferOverlapped(HANDLE pipe, bool write, void* buffer, size_t size,
                        DWORD timeoutMs, std::string* error)
{
    size_t offset = 0;
    while (offset < size) {
        ScopedHandle event(CreateEventA(NULL, TRUE, FALSE, NULL));
        if (!isValidHandle(event.get())) {
            if (error != NULL) {
                *error = windowsError("CreateEvent", GetLastError());
            }
            return false;
        }

        OVERLAPPED operation;
        ZeroMemory(&operation, sizeof(operation));
        operation.hEvent = event.get();

        const DWORD remaining = static_cast<DWORD>(size - offset);
        DWORD transferred = 0;
        BOOL started = write
            ? WriteFile(pipe, static_cast<char*>(buffer) + offset, remaining,
                        &transferred, &operation)
            : ReadFile(pipe, static_cast<char*>(buffer) + offset, remaining,
                       &transferred, &operation);

        if (!started) {
            const DWORD startError = GetLastError();
            if (startError != ERROR_IO_PENDING) {
                if (error != NULL) {
                    *error = windowsError(write ? "WriteFile" : "ReadFile",
                                          startError);
                }
                return false;
            }

            const DWORD wait = WaitForSingleObject(event.get(), timeoutMs);
            if (wait != WAIT_OBJECT_0) {
                const DWORD waitError = wait == WAIT_FAILED
                    ? GetLastError() : ERROR_SUCCESS;
                CancelIoEx(pipe, &operation);
                // OVERLAPPED and its buffer must remain alive until the
                // cancellation completion is delivered.
                GetOverlappedResult(pipe, &operation, &transferred, TRUE);
                if (error != NULL) {
                    *error = wait == WAIT_TIMEOUT
                        ? std::string(write ? "pipe write timed out" : "pipe read timed out")
                        : windowsError("WaitForSingleObject", waitError);
                }
                return false;
            }

            if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE)) {
                if (error != NULL) {
                    *error = windowsError("GetOverlappedResult", GetLastError());
                }
                return false;
            }
        }

        if (transferred == 0) {
            if (error != NULL) {
                *error = "clipboard helper closed the pipe";
            }
            return false;
        }
        offset += transferred;
    }
    return true;
}

bool transferBlocking(HANDLE pipe, bool write, void* buffer, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        DWORD transferred = 0;
        const DWORD remaining = static_cast<DWORD>(size - offset);
        const BOOL success = write
            ? WriteFile(pipe, static_cast<char*>(buffer) + offset, remaining,
                        &transferred, NULL)
            : ReadFile(pipe, static_cast<char*>(buffer) + offset, remaining,
                       &transferred, NULL);
        if (!success || transferred == 0) {
            return false;
        }
        offset += transferred;
    }
    return true;
}

bool connectOverlapped(HANDLE pipe, std::string* error)
{
    ScopedHandle event(CreateEventA(NULL, TRUE, FALSE, NULL));
    if (!isValidHandle(event.get())) {
        if (error != NULL) {
            *error = windowsError("CreateEvent", GetLastError());
        }
        return false;
    }

    OVERLAPPED operation;
    ZeroMemory(&operation, sizeof(operation));
    operation.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &operation)) {
        return true;
    }

    const DWORD connectError = GetLastError();
    if (connectError == ERROR_PIPE_CONNECTED) {
        return true;
    }
    if (connectError != ERROR_IO_PENDING) {
        if (error != NULL) {
            *error = windowsError("ConnectNamedPipe", connectError);
        }
        return false;
    }

    const DWORD wait = WaitForSingleObject(event.get(), kConnectTimeoutMs);
    if (wait != WAIT_OBJECT_0) {
        const DWORD waitError = wait == WAIT_FAILED
            ? GetLastError() : ERROR_SUCCESS;
        CancelIoEx(pipe, &operation);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &operation, &ignored, TRUE);
        if (error != NULL) {
            *error = wait == WAIT_TIMEOUT
                ? "clipboard helper connection timed out"
                : windowsError("WaitForSingleObject", waitError);
        }
        return false;
    }

    DWORD ignored = 0;
    if (!GetOverlappedResult(pipe, &operation, &ignored, FALSE)) {
        if (error != NULL) {
            *error = windowsError("GetOverlappedResult", GetLastError());
        }
        return false;
    }
    return true;
}

std::string randomPipeSuffix()
{
    unsigned char bytes[16];
    ZeroMemory(bytes, sizeof(bytes));
    HCRYPTPROV provider = 0;
    bool generated = false;
    if (CryptAcquireContextA(&provider, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        generated = CryptGenRandom(provider, sizeof(bytes), bytes) != FALSE;
        CryptReleaseContext(provider, 0);
    }
    if (!generated) {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        const ULONGLONG fallback =
            static_cast<ULONGLONG>(counter.QuadPart) ^
            (static_cast<ULONGLONG>(GetCurrentProcessId()) << 32) ^
            GetTickCount64();
        std::memcpy(bytes, &fallback, sizeof(fallback));
    }

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        stream << std::setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return stream.str();
}

bool getUserSidString(HANDLE token, std::string* sid, std::string* error)
{
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    if (bytes == 0 ||
        !GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
        if (error != NULL) {
            *error = windowsError("GetTokenInformation", GetLastError());
        }
        return false;
    }

    LPSTR rawSid = NULL;
    if (!ConvertSidToStringSidA(
            reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,
            &rawSid)) {
        if (error != NULL) {
            *error = windowsError("ConvertSidToStringSid", GetLastError());
        }
        return false;
    }
    ScopedLocalMemory sidMemory(rawSid);
    *sid = rawSid;
    return true;
}

bool isExpectedPipeName(const std::string& pipeName)
{
    return pipeName.size() > sizeof(kPipePrefix) - 1 &&
           pipeName.compare(0, sizeof(kPipePrefix) - 1, kPipePrefix) == 0;
}

} // namespace

namespace MSWindowsClipboardBridgeProtocol {

bool validateResponseHeader(const ResponseHeader& header, std::string* error)
{
    if (header.magic != kMagic) {
        if (error != NULL) {
            *error = "clipboard helper response has an invalid magic value";
        }
        return false;
    }
    if (header.status > kInvalidRequest) {
        if (error != NULL) {
            *error = "clipboard helper response has an invalid status";
        }
        return false;
    }
    if (header.status != kSuccess && header.size != 0) {
        if (error != NULL) {
            *error = "clipboard helper error response contains a payload";
        }
        return false;
    }
    if (header.status == kSuccess &&
        (header.size < 4 || header.size > kMaxSnapshotBytes)) {
        if (error != NULL) {
            *error = "clipboard helper response size is outside the allowed range";
        }
        return false;
    }
    return true;
}

bool validateSnapshot(const std::string& snapshot, std::string* error)
{
    if (snapshot.size() < 4 || snapshot.size() > kMaxSnapshotBytes) {
        if (error != NULL) {
            *error = "marshalled clipboard size is outside the allowed range";
        }
        return false;
    }

    size_t offset = 0;
    const UInt32 formatCount = readBigEndianUInt32(snapshot.data());
    offset += 4;
    if (formatCount > 1024) {
        if (error != NULL) {
            *error = "marshalled clipboard contains too many formats";
        }
        return false;
    }

    for (UInt32 format = 0; format < formatCount; ++format) {
        if (snapshot.size() - offset < 8) {
            if (error != NULL) {
                *error = "marshalled clipboard format header is truncated";
            }
            return false;
        }
        const UInt32 dataSize = readBigEndianUInt32(snapshot.data() + offset + 4);
        offset += 8;
        if (dataSize > snapshot.size() - offset) {
            if (error != NULL) {
                *error = "marshalled clipboard format payload is truncated";
            }
            return false;
        }
        offset += dataSize;
    }

    if (offset != snapshot.size()) {
        if (error != NULL) {
            *error = "marshalled clipboard has trailing data";
        }
        return false;
    }
    return true;
}

} // namespace MSWindowsClipboardBridgeProtocol

MSWindowsClipboardBridge::MSWindowsClipboardBridge() :
    m_pipe(INVALID_HANDLE_VALUE),
    m_process(NULL),
    m_processId(0),
    m_sessionId(0xffffffff),
    m_nextStartAttemptAt(0)
{
    static_assert(sizeof(MSWindowsClipboardBridgeProtocol::RequestHeader) == 8,
                  "clipboard request header layout changed");
    static_assert(sizeof(MSWindowsClipboardBridgeProtocol::ReadyHeader) == 8,
                  "clipboard ready header layout changed");
    static_assert(sizeof(MSWindowsClipboardBridgeProtocol::ResponseHeader) == 12,
                  "clipboard response header layout changed");
}

MSWindowsClipboardBridge::~MSWindowsClipboardBridge()
{
    stop();
}

void MSWindowsClipboardBridge::warmUp()
{
    bool required = false;
    DWORD identityError = ERROR_SUCCESS;
    if (!SessionUserImpersonation::queryRequired(required, identityError)) {
        LOG((CLOG_WARN "could not inspect process identity before starting clipboard helper: error=%lu",
             identityError));
        return;
    }
    if (!required) {
        return;
    }

    std::string error;
    if (!ensureStarted(&error)) {
        LOG((CLOG_WARN "could not prestart the active-session clipboard helper: %s",
             error.c_str()));
    }
}

MSWindowsClipboardBridge::ReadResult
MSWindowsClipboardBridge::readSnapshot(IClipboard* destination,
                                       IClipboard::Time time,
                                       std::string* error)
{
    if (error != NULL) {
        error->clear();
    }
    bool required = false;
    DWORD identityError = ERROR_SUCCESS;
    if (!SessionUserImpersonation::queryRequired(required, identityError)) {
        if (error != NULL) {
            *error = windowsError("process identity inspection", identityError);
        }
        return ReadResult::Failed;
    }
    if (!required) {
        return ReadResult::NotRequired;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensureStarted(error)) {
            return ReadResult::Failed;
        }

        bool connectionFailed = false;
        if (requestSnapshot(destination, time, &connectionFailed, error)) {
            return ReadResult::Succeeded;
        }
        if (!connectionFailed) {
            return ReadResult::Failed;
        }
        stop();
    }
    return ReadResult::Failed;
}

bool MSWindowsClipboardBridge::ensureStarted(std::string* error)
{
    if (error != NULL) {
        error->clear();
    }
    if (isValidHandle(m_pipe) && isValidHandle(m_process)) {
        DWORD exitCode = 0;
        if (GetExitCodeProcess(m_process, &exitCode) && exitCode == STILL_ACTIVE) {
            return true;
        }
        stop();
    }

    const ULONGLONG now = GetTickCount64();
    if (now < m_nextStartAttemptAt) {
        if (error != NULL) {
            *error = "clipboard helper restart is backing off";
        }
        return false;
    }

    m_sessionId = WTSGetActiveConsoleSessionId();
    if (m_sessionId == 0xffffffff) {
        if (error != NULL) {
            *error = "there is no active console session";
        }
        m_nextStartAttemptAt = now + kStartRetryDelayMs;
        return false;
    }

    HANDLE rawSessionToken = NULL;
    if (!WTSQueryUserToken(m_sessionId, &rawSessionToken)) {
        if (error != NULL) {
            *error = windowsError("WTSQueryUserToken", GetLastError());
        }
        m_nextStartAttemptAt = now + kStartRetryDelayMs;
        return false;
    }
    ScopedHandle sessionToken(rawSessionToken);

    HANDLE rawPrimaryToken = NULL;
    if (!DuplicateTokenEx(sessionToken.get(), MAXIMUM_ALLOWED, NULL,
                          SecurityImpersonation, TokenPrimary,
                          &rawPrimaryToken)) {
        if (error != NULL) {
            *error = windowsError("DuplicateTokenEx", GetLastError());
        }
        m_nextStartAttemptAt = now + kStartRetryDelayMs;
        return false;
    }
    ScopedHandle primaryToken(rawPrimaryToken);

    std::string userSid;
    if (!getUserSidString(primaryToken.get(), &userSid, error)) {
        m_nextStartAttemptAt = now + kStartRetryDelayMs;
        return false;
    }

    const std::string sddl =
        std::string("D:P(A;;GA;;;SY)(A;;GA;;;") + userSid + ")";
    PSECURITY_DESCRIPTOR rawDescriptor = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            sddl.c_str(), SDDL_REVISION_1, &rawDescriptor, NULL)) {
        if (error != NULL) {
            *error = windowsError(
                "ConvertStringSecurityDescriptorToSecurityDescriptor",
                GetLastError());
        }
        m_nextStartAttemptAt = now + kStartRetryDelayMs;
        return false;
    }
    ScopedLocalMemory descriptor(rawDescriptor);
    SECURITY_ATTRIBUTES security;
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = rawDescriptor;
    security.bInheritHandle = FALSE;

    std::ostringstream pipeNameBuilder;
    pipeNameBuilder << kPipePrefix << m_sessionId << "-" << GetCurrentProcessId()
                    << "-" << randomPipeSuffix();
    const std::string pipeName = pipeNameBuilder.str();
    ScopedHandle pipe(CreateNamedPipeA(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 64 * 1024, 64 * 1024, 0, &security));
    if (!isValidHandle(pipe.get())) {
        if (error != NULL) {
            *error = windowsError("CreateNamedPipe", GetLastError());
        }
        m_nextStartAttemptAt = now + kStartRetryDelayMs;
        return false;
    }

    std::vector<char> modulePath(32768, '\0');
    const DWORD moduleLength = GetModuleFileNameA(
        NULL, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (moduleLength == 0 || moduleLength >= modulePath.size()) {
        if (error != NULL) {
            *error = windowsError("GetModuleFileName", GetLastError());
        }
        m_nextStartAttemptAt = now + kStartRetryDelayMs;
        return false;
    }

    std::ostringstream commandBuilder;
    commandBuilder << '"' << modulePath.data() << '"'
                   << " --clipboard-helper \"" << pipeName << '"';
    std::string command = commandBuilder.str();
    std::vector<char> commandLine(command.begin(), command.end());
    commandLine.push_back('\0');

    ScopedEnvironmentBlock environment;
    if (!CreateEnvironmentBlock(environment.out(), primaryToken.get(), FALSE)) {
        if (error != NULL) {
            *error = windowsError("CreateEnvironmentBlock", GetLastError());
        }
        m_nextStartAttemptAt = now + kStartRetryDelayMs;
        return false;
    }

    STARTUPINFOA startup;
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    char desktop[] = "winsta0\\Default";
    startup.lpDesktop = desktop;

    PROCESS_INFORMATION processInfo;
    ZeroMemory(&processInfo, sizeof(processInfo));
    const DWORD creationFlags =
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | NORMAL_PRIORITY_CLASS;
    if (!CreateProcessAsUserA(
            primaryToken.get(), NULL, commandLine.data(), NULL, NULL, FALSE,
            creationFlags, environment.get(), NULL, &startup, &processInfo)) {
        if (error != NULL) {
            *error = windowsError("CreateProcessAsUser", GetLastError());
        }
        m_nextStartAttemptAt = now + kStartRetryDelayMs;
        return false;
    }

    ScopedHandle process(processInfo.hProcess);
    ScopedHandle thread(processInfo.hThread);
    if (!connectOverlapped(pipe.get(), error)) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), kHelperExitTimeoutMs);
        m_nextStartAttemptAt = GetTickCount64() + kStartRetryDelayMs;
        return false;
    }

    ULONG clientProcessId = 0;
    if (!GetNamedPipeClientProcessId(pipe.get(), &clientProcessId) ||
        clientProcessId != processInfo.dwProcessId) {
        if (error != NULL) {
            *error = "clipboard helper pipe client identity did not match the launched process";
        }
        DisconnectNamedPipe(pipe.get());
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), kHelperExitTimeoutMs);
        m_nextStartAttemptAt = GetTickCount64() + kStartRetryDelayMs;
        return false;
    }

    MSWindowsClipboardBridgeProtocol::ReadyHeader ready;
    if (!transferOverlapped(pipe.get(), false, &ready, sizeof(ready),
                            kIoTimeoutMs, error) ||
        ready.magic != MSWindowsClipboardBridgeProtocol::kMagic ||
        ready.processId != processInfo.dwProcessId) {
        if (error != NULL && error->empty()) {
            *error = "clipboard helper readiness handshake was invalid";
        }
        DisconnectNamedPipe(pipe.get());
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), kHelperExitTimeoutMs);
        m_nextStartAttemptAt = GetTickCount64() + kStartRetryDelayMs;
        return false;
    }

    m_pipe = pipe.release();
    m_process = process.release();
    m_processId = processInfo.dwProcessId;
    m_nextStartAttemptAt = 0;
    LOG((CLOG_INFO "started active-session clipboard helper: session=%lu pid=%lu",
         m_sessionId, m_processId));
    return true;
}

bool MSWindowsClipboardBridge::requestSnapshot(
    IClipboard* destination, IClipboard::Time time, bool* connectionFailed,
    std::string* error)
{
    if (error != NULL) {
        error->clear();
    }
    *connectionFailed = false;
    MSWindowsClipboardBridgeProtocol::RequestHeader request;
    request.magic = MSWindowsClipboardBridgeProtocol::kMagic;
    request.command = MSWindowsClipboardBridgeProtocol::kSnapshot;
    if (!transferOverlapped(m_pipe, true, &request, sizeof(request),
                            kIoTimeoutMs, error)) {
        *connectionFailed = true;
        return false;
    }

    MSWindowsClipboardBridgeProtocol::ResponseHeader response;
    if (!transferOverlapped(m_pipe, false, &response, sizeof(response),
                            kIoTimeoutMs, error)) {
        *connectionFailed = true;
        return false;
    }
    if (!MSWindowsClipboardBridgeProtocol::validateResponseHeader(response,
                                                                   error)) {
        *connectionFailed = true;
        return false;
    }
    if (response.status != MSWindowsClipboardBridgeProtocol::kSuccess) {
        if (error != NULL) {
            switch (response.status) {
            case MSWindowsClipboardBridgeProtocol::kClipboardUnavailable:
                *error = "the active-session clipboard is temporarily unavailable";
                break;
            case MSWindowsClipboardBridgeProtocol::kSnapshotTooLarge:
                *error = "the active-session clipboard exceeds the snapshot limit";
                break;
            default:
                *error = "the clipboard helper rejected the snapshot request";
                break;
            }
        }
        return false;
    }

    std::string snapshot(response.size, '\0');
    if (!transferOverlapped(m_pipe, false, &snapshot[0], snapshot.size(),
                            kIoTimeoutMs, error)) {
        *connectionFailed = true;
        return false;
    }
    if (!MSWindowsClipboardBridgeProtocol::validateSnapshot(snapshot, error)) {
        *connectionFailed = true;
        return false;
    }

    IClipboard::unmarshall(destination, snapshot, time);
    LOG((CLOG_DEBUG "read %lu clipboard byte(s) from active-session helper: session=%lu pid=%lu",
         static_cast<unsigned long>(snapshot.size()), m_sessionId, m_processId));
    return true;
}

void MSWindowsClipboardBridge::stop()
{
    if (isValidHandle(m_pipe)) {
        MSWindowsClipboardBridgeProtocol::RequestHeader request;
        request.magic = MSWindowsClipboardBridgeProtocol::kMagic;
        request.command = MSWindowsClipboardBridgeProtocol::kShutdown;
        std::string ignored;
        transferOverlapped(m_pipe, true, &request, sizeof(request), 100,
                           &ignored);
        DisconnectNamedPipe(m_pipe);
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }

    if (isValidHandle(m_process)) {
        if (WaitForSingleObject(m_process, kHelperExitTimeoutMs) == WAIT_TIMEOUT) {
            TerminateProcess(m_process, 1);
            WaitForSingleObject(m_process, kHelperExitTimeoutMs);
        }
        CloseHandle(m_process);
        m_process = NULL;
    }
    m_processId = 0;
    m_sessionId = 0xffffffff;
}

int MSWindowsClipboardBridge::runHelper(const std::string& pipeName)
{
    if (!isExpectedPipeName(pipeName)) {
        return 2;
    }
    if (!WaitNamedPipeA(pipeName.c_str(), kConnectTimeoutMs)) {
        return 3;
    }

    ScopedHandle pipe(CreateFileA(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, NULL, OPEN_EXISTING, 0, NULL));
    if (!isValidHandle(pipe.get())) {
        return 4;
    }

    MSWindowsClipboardBridgeProtocol::ReadyHeader ready;
    ready.magic = MSWindowsClipboardBridgeProtocol::kMagic;
    ready.processId = GetCurrentProcessId();
    if (!transferBlocking(pipe.get(), true, &ready, sizeof(ready))) {
        return 5;
    }

    for (;;) {
        MSWindowsClipboardBridgeProtocol::RequestHeader request;
        if (!transferBlocking(pipe.get(), false, &request, sizeof(request))) {
            return 0;
        }
        if (request.magic != MSWindowsClipboardBridgeProtocol::kMagic) {
            return 6;
        }
        if (request.command == MSWindowsClipboardBridgeProtocol::kShutdown) {
            return 0;
        }

        MSWindowsClipboardBridgeProtocol::ResponseHeader response;
        response.magic = MSWindowsClipboardBridgeProtocol::kMagic;
        response.status = MSWindowsClipboardBridgeProtocol::kInvalidRequest;
        response.size = 0;
        std::string snapshot;
        if (request.command == MSWindowsClipboardBridgeProtocol::kSnapshot) {
            Clipboard clipboard;
            MSWindowsClipboard source(NULL);
            bool copied = false;
            for (int attempt = 0; attempt < 8 && !copied; ++attempt) {
                copied = Clipboard::copy(&clipboard, &source);
                if (!copied) {
                    Sleep(25);
                }
            }

            if (!copied) {
                response.status =
                    MSWindowsClipboardBridgeProtocol::kClipboardUnavailable;
            }
            else {
                snapshot = clipboard.marshall();
                if (snapshot.size() >
                    MSWindowsClipboardBridgeProtocol::kMaxSnapshotBytes) {
                    response.status =
                        MSWindowsClipboardBridgeProtocol::kSnapshotTooLarge;
                    snapshot.clear();
                }
                else {
                    response.status = MSWindowsClipboardBridgeProtocol::kSuccess;
                    response.size = static_cast<UInt32>(snapshot.size());
                }
            }
        }

        if (!transferBlocking(pipe.get(), true, &response, sizeof(response))) {
            return 0;
        }
        if (!snapshot.empty() &&
            !transferBlocking(pipe.get(), true, &snapshot[0], snapshot.size())) {
            return 0;
        }
    }
}
