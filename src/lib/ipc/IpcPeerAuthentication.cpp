/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ipc/IpcPeerAuthentication.h"

#include "net/TCPSocket.h"

#if SYSAPI_WIN32
#include "arch/win32/ArchNetworkWinsock.h"
#include "base/Log.h"

#include <Iphlpapi.h>
#include <Windows.h>
#include <Sddl.h>
#include <Wtsapi32.h>

#include <algorithm>
#include <cwctype>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Iphlpapi.lib")
#endif

namespace {

#if SYSAPI_WIN32

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) : m_handle(handle) { }
    ~ScopedHandle()
    {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }
    }

    HANDLE get() const { return m_handle; }

private:
    ScopedHandle(const ScopedHandle&);
    ScopedHandle& operator=(const ScopedHandle&);

    HANDLE m_handle;
};

std::wstring lowerPath(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

std::wstring finalPath(const std::wstring& path)
{
    ScopedHandle file(CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        return std::wstring();
    }

    const DWORD required = GetFinalPathNameByHandleW(
        file.get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (required == 0) {
        return std::wstring();
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(
        file.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= buffer.size()) {
        return std::wstring();
    }

    std::wstring result(buffer.data(), length);
    static const std::wstring kExtendedPrefix = L"\\\\?\\";
    if (result.compare(0, kExtendedPrefix.size(), kExtendedPrefix) == 0) {
        result.erase(0, kExtendedPrefix.size());
    }
    return lowerPath(result);
}

bool splitPath(const std::wstring& path, std::wstring& directory,
               std::wstring& basename)
{
    const std::wstring::size_type separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos || separator + 1 >= path.size()) {
        return false;
    }
    directory = path.substr(0, separator);
    basename = path.substr(separator + 1);
    return true;
}

bool querySocketEndpoints(SOCKET socket, sockaddr_in& local,
                          sockaddr_in& peer, std::string& reason)
{
    int localLength = sizeof(local);
    int peerLength = sizeof(peer);
    ZeroMemory(&local, sizeof(local));
    ZeroMemory(&peer, sizeof(peer));
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&local),
                    &localLength) == SOCKET_ERROR ||
        getpeername(socket, reinterpret_cast<sockaddr*>(&peer),
                    &peerLength) == SOCKET_ERROR) {
        reason = "could not inspect the accepted IPC socket endpoints";
        return false;
    }
    if (local.sin_family != AF_INET || peer.sin_family != AF_INET ||
        ntohl(local.sin_addr.s_addr) != INADDR_LOOPBACK ||
        ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK) {
        reason = "IPC connection is not an IPv4 loopback connection";
        return false;
    }
    return true;
}

bool queryPeerProcessId(const sockaddr_in& local, const sockaddr_in& peer,
                        DWORD& processId, std::string& reason)
{
    ULONG size = 0;
    DWORD result = GetExtendedTcpTable(
        nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    if (result != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        reason = "could not size the Windows TCP owner table";
        return false;
    }

    std::vector<unsigned char> buffer(size);
    result = GetExtendedTcpTable(
        buffer.data(), &size, FALSE, AF_INET,
        TCP_TABLE_OWNER_PID_CONNECTIONS, 0);
    if (result != NO_ERROR) {
        reason = "could not read the Windows TCP owner table";
        return false;
    }

    const MIB_TCPTABLE_OWNER_PID* table =
        reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    DWORD matchingProcessId = 0;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_TCPROW_OWNER_PID& row = table->table[i];
        if (row.dwState != MIB_TCP_STATE_ESTAB ||
            row.dwLocalAddr != peer.sin_addr.s_addr ||
            static_cast<u_short>(row.dwLocalPort) != peer.sin_port ||
            row.dwRemoteAddr != local.sin_addr.s_addr ||
            static_cast<u_short>(row.dwRemotePort) != local.sin_port) {
            continue;
        }
        if (matchingProcessId != 0 &&
            matchingProcessId != row.dwOwningPid) {
            reason = "the IPC socket maps to multiple Windows processes";
            return false;
        }
        matchingProcessId = row.dwOwningPid;
    }

    if (matchingProcessId == 0) {
        reason = "the IPC socket has no kernel-owned client process";
        return false;
    }
    processId = matchingProcessId;
    return true;
}

bool queryProcessPath(HANDLE process, std::wstring& path,
                      std::string& reason)
{
    std::vector<wchar_t> buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size) ||
        size == 0) {
        reason = "could not inspect the IPC peer executable";
        return false;
    }
    path.assign(buffer.data(), size);
    return true;
}

bool isActiveInteractiveSession(DWORD sessionId, std::string& reason)
{
    const DWORD activeConsoleSessionId = WTSGetActiveConsoleSessionId();
    if (activeConsoleSessionId == 0xffffffffu ||
        sessionId != activeConsoleSessionId) {
        reason = "IPC peer is not in the active console session";
        return false;
    }

    LPWSTR rawState = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(
            WTS_CURRENT_SERVER_HANDLE, sessionId, WTSConnectState,
            &rawState, &bytes) ||
        rawState == nullptr || bytes < sizeof(WTS_CONNECTSTATE_CLASS)) {
        if (rawState != nullptr) {
            WTSFreeMemory(rawState);
        }
        reason = "could not inspect the IPC peer interactive session state";
        return false;
    }

    const WTS_CONNECTSTATE_CLASS state =
        *reinterpret_cast<const WTS_CONNECTSTATE_CLASS*>(rawState);
    WTSFreeMemory(rawState);
    if (state != WTSActive) {
        reason = "IPC peer is not in an active interactive session";
        return false;
    }
    return true;
}

bool queryIntegrityLevel(HANDLE token, IpcPeerIntegrityLevel& integrityLevel,
                         std::string& reason)
{
    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        reason = "could not size the IPC peer token integrity level";
        return false;
    }

    std::vector<unsigned char> buffer(size);
    DWORD returned = 0;
    if (!GetTokenInformation(token, TokenIntegrityLevel, buffer.data(),
                             size, &returned)) {
        reason = "could not read the IPC peer token integrity level";
        return false;
    }

    const TOKEN_MANDATORY_LABEL* label =
        reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    PSID sid = label->Label.Sid;
    if (!IsValidSid(sid) || *GetSidSubAuthorityCount(sid) == 0) {
        reason = "IPC peer token has an invalid integrity SID";
        return false;
    }

    const DWORD index = *GetSidSubAuthorityCount(sid) - 1;
    const DWORD rid = *GetSidSubAuthority(sid, index);
    if (rid < SECURITY_MANDATORY_MEDIUM_RID) {
        reason = "IPC peer token integrity level is below Medium";
        return false;
    }

    integrityLevel = static_cast<IpcPeerIntegrityLevel>(rid);
    return true;
}

bool validateProcessToken(HANDLE process, DWORD processId,
                          EIpcClientType clientType, DWORD& sessionId,
                          IpcPeerIntegrityLevel& integrityLevel,
                          std::string& userSid,
                          std::string& reason)
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &rawToken)) {
        reason = "could not inspect the IPC peer token";
        return false;
    }
    ScopedHandle token(rawToken);

    DWORD tokenSessionId = 0;
    DWORD returned = 0;
    if (!GetTokenInformation(token.get(), TokenSessionId, &tokenSessionId,
                             sizeof(tokenSessionId), &returned) ||
        tokenSessionId == 0) {
        reason = "IPC peer token is not attached to an interactive session";
        return false;
    }

    DWORD processSessionId = 0;
    if (!ProcessIdToSessionId(processId, &processSessionId) ||
        processSessionId != tokenSessionId) {
        reason = "IPC peer token session does not match its process session";
        return false;
    }
    if (!isActiveInteractiveSession(tokenSessionId, reason)) {
        return false;
    }

    DWORD tokenUserSize = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &tokenUserSize);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || tokenUserSize == 0) {
        reason = "could not size the IPC peer token identity";
        return false;
    }
    std::vector<unsigned char> tokenUserBuffer(tokenUserSize);
    if (!GetTokenInformation(token.get(), TokenUser, tokenUserBuffer.data(),
                             tokenUserSize, &returned)) {
        reason = "could not read the IPC peer token identity";
        return false;
    }
    const TOKEN_USER* tokenUser =
        reinterpret_cast<const TOKEN_USER*>(tokenUserBuffer.data());
    if (!IsValidSid(tokenUser->User.Sid)) {
        reason = "IPC peer token has an invalid user SID";
        return false;
    }
    HANDLE rawSessionToken = nullptr;
    if (!WTSQueryUserToken(tokenSessionId, &rawSessionToken)) {
        reason = "could not inspect the active interactive session user";
        return false;
    }
    ScopedHandle sessionToken(rawSessionToken);

    DWORD sessionUserSize = 0;
    GetTokenInformation(
        sessionToken.get(), TokenUser, nullptr, 0, &sessionUserSize);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        sessionUserSize == 0) {
        reason = "could not size the active interactive session identity";
        return false;
    }
    std::vector<unsigned char> sessionUserBuffer(sessionUserSize);
    if (!GetTokenInformation(
            sessionToken.get(), TokenUser, sessionUserBuffer.data(),
            sessionUserSize, &returned)) {
        reason = "could not read the active interactive session identity";
        return false;
    }
    const TOKEN_USER* sessionUser =
        reinterpret_cast<const TOKEN_USER*>(sessionUserBuffer.data());
    if (!IsValidSid(sessionUser->User.Sid)) {
        reason = "active interactive session has an invalid user SID";
        return false;
    }
    const bool matchesInteractiveUser =
        EqualSid(tokenUser->User.Sid, sessionUser->User.Sid) != FALSE;
    BYTE localSystemSid[SECURITY_MAX_SID_SIZE];
    DWORD localSystemSidSize = sizeof(localSystemSid);
    const bool isLocalSystem =
        CreateWellKnownSid(WinLocalSystemSid, nullptr, localSystemSid,
                           &localSystemSidSize) != FALSE &&
        EqualSid(tokenUser->User.Sid, localSystemSid) != FALSE;
    if (!IpcPeerAuthenticationPolicy::tokenOwnerAllowed(
            clientType, matchesInteractiveUser, isLocalSystem)) {
        reason = clientType == kIpcClientGui
            ? "IPC GUI user does not own the active interactive session"
            : "IPC node is neither the interactive user nor LocalSystem";
        return false;
    }

    LPSTR rawUserSid = nullptr;
    if (!ConvertSidToStringSidA(tokenUser->User.Sid, &rawUserSid) ||
        rawUserSid == nullptr || rawUserSid[0] == '\0') {
        if (rawUserSid != nullptr) {
            LocalFree(rawUserSid);
        }
        reason = "could not preserve the IPC peer user SID";
        return false;
    }
    const std::string verifiedUserSid(rawUserSid);
    LocalFree(rawUserSid);

    DWORD appContainer = 0;
    if (!GetTokenInformation(token.get(), TokenIsAppContainer, &appContainer,
                             sizeof(appContainer), &returned)) {
        reason = "could not inspect the IPC peer token isolation";
        return false;
    }
    if (appContainer != 0) {
        reason = "AppContainer processes cannot control the Weave service";
        return false;
    }
    if (!queryIntegrityLevel(token.get(), integrityLevel, reason)) {
        return false;
    }

    sessionId = tokenSessionId;
    userSid = verifiedUserSid;
    return true;
}

IpcPeerAuthContext authenticateWindowsPeer(ArchSocket socket)
{
    if (socket == nullptr || socket->m_socket == INVALID_SOCKET) {
        return IpcPeerAuthContext::rejected("accepted IPC socket is unavailable");
    }

    sockaddr_in local;
    sockaddr_in peer;
    std::string reason;
    if (!querySocketEndpoints(socket->m_socket, local, peer, reason)) {
        return IpcPeerAuthContext::rejected(reason);
    }

    DWORD processId = 0;
    if (!queryPeerProcessId(local, peer, processId, reason)) {
        return IpcPeerAuthContext::rejected(reason);
    }
    if (processId == GetCurrentProcessId()) {
        return IpcPeerAuthContext::rejected(
            "the SYSTEM service cannot authenticate itself as an IPC client");
    }

    ScopedHandle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
        FALSE, processId));
    if (process.get() == nullptr) {
        return IpcPeerAuthContext::rejected(
            "could not open the kernel-owned IPC peer process");
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process.get(), &exitCode) ||
        exitCode != STILL_ACTIVE) {
        return IpcPeerAuthContext::rejected(
            "kernel-owned IPC peer process is no longer running");
    }

    std::wstring processPath;
    if (!queryProcessPath(process.get(), processPath, reason)) {
        return IpcPeerAuthContext::rejected(reason);
    }

    std::vector<wchar_t> modulePathBuffer(32768, L'\0');
    const DWORD modulePathLength = GetModuleFileNameW(
        nullptr, modulePathBuffer.data(),
        static_cast<DWORD>(modulePathBuffer.size()));
    if (modulePathLength == 0 || modulePathLength >= modulePathBuffer.size()) {
        return IpcPeerAuthContext::rejected(
            "could not resolve the Weave service executable");
    }

    const std::wstring trustedServicePath = finalPath(std::wstring(
        modulePathBuffer.data(), modulePathLength));
    const std::wstring peerPath = finalPath(processPath);
    std::wstring trustedDirectory;
    std::wstring serviceBasename;
    std::wstring peerDirectory;
    std::wstring peerBasename;
    if (trustedServicePath.empty() || peerPath.empty() ||
        !splitPath(trustedServicePath, trustedDirectory, serviceBasename) ||
        !splitPath(peerPath, peerDirectory, peerBasename) ||
        serviceBasename != L"weaved.exe" ||
        trustedDirectory != peerDirectory) {
        return IpcPeerAuthContext::rejected(
            "IPC peer executable is outside the protected service directory");
    }

    EIpcClientType clientType = kIpcClientUnknown;
    if (peerBasename == L"weave.exe") {
        clientType = kIpcClientGui;
    }
    else if (peerBasename == L"weavec.exe" ||
             peerBasename == L"weaves.exe") {
        clientType = kIpcClientNode;
    }
    else {
        return IpcPeerAuthContext::rejected(
            "IPC peer executable is not an allowed Weave component");
    }

    DWORD sessionId = 0;
    IpcPeerIntegrityLevel integrityLevel = IpcPeerIntegrityLevel::Unknown;
    std::string userSid;
    if (!validateProcessToken(process.get(), processId, clientType, sessionId,
                              integrityLevel, userSid, reason)) {
        return IpcPeerAuthContext::rejected(reason);
    }

    LOG((CLOG_INFO
        "authenticated local ipc peer pid=%lu session=%lu integrity=0x%lx type=%s",
        static_cast<unsigned long>(processId),
        static_cast<unsigned long>(sessionId),
        static_cast<unsigned long>(integrityLevel),
        clientType == kIpcClientGui ? "gui" : "node"));
    return IpcPeerAuthContext::accepted(
        processId, clientType, sessionId, integrityLevel, userSid);
}

#endif

}

namespace IpcPeerAuthenticationPolicy {

bool tokenOwnerAllowed(EIpcClientType clientType,
                       bool matchesInteractiveUser,
                       bool isLocalSystem)
{
    if (clientType == kIpcClientGui) {
        return matchesInteractiveUser;
    }
    if (clientType == kIpcClientNode) {
        return matchesInteractiveUser || isLocalSystem;
    }
    return false;
}

} // namespace IpcPeerAuthenticationPolicy

CommandOrigin::CommandOrigin() :
    m_processId(0),
    m_sessionId(0),
    m_kernelVerified(false)
{
}

CommandOrigin::CommandOrigin(std::uint32_t processId,
                             std::uint32_t sessionId,
                             const std::string& userSid) :
    m_processId(processId),
    m_sessionId(sessionId),
    m_userSid(userSid),
    m_kernelVerified(processId != 0 && sessionId != 0 && !userSid.empty())
{
    if (!m_kernelVerified) {
        m_processId = 0;
        m_sessionId = 0;
        m_userSid.clear();
    }
}

IpcPeerAuthContext::IpcPeerAuthContext() :
    m_policy(Policy::Rejected),
    m_processId(0),
    m_clientType(kIpcClientUnknown),
    m_sessionId(0),
    m_integrityLevel(IpcPeerIntegrityLevel::Unknown),
    m_reason("IPC peer authentication context was not initialized")
{
}

IpcPeerAuthContext
IpcPeerAuthContext::accepted(std::uint32_t processId,
                             EIpcClientType clientType,
                             std::uint32_t sessionId,
                             IpcPeerIntegrityLevel integrityLevel,
                             const std::string& userSid)
{
    if (processId == 0 || sessionId == 0 ||
        userSid.empty() ||
        (clientType != kIpcClientGui && clientType != kIpcClientNode) ||
        static_cast<std::uint32_t>(integrityLevel) <
            static_cast<std::uint32_t>(IpcPeerIntegrityLevel::Medium)) {
        return rejected("authenticated IPC peer identity is incomplete or unsafe");
    }

    IpcPeerAuthContext context;
    context.m_policy = Policy::KernelVerified;
    context.m_processId = processId;
    context.m_clientType = clientType;
    context.m_sessionId = sessionId;
    context.m_integrityLevel = integrityLevel;
    context.m_userSid = userSid;
    context.m_reason.clear();
    return context;
}

IpcPeerAuthContext
IpcPeerAuthContext::rejected(const std::string& reason)
{
    IpcPeerAuthContext context;
    context.m_policy = Policy::Rejected;
    context.m_reason = reason.empty()
        ? "peer authentication failed" : reason;
    return context;
}

IpcPeerAuthContext
IpcPeerAuthContext::localTransportWithoutKernelIdentity()
{
    IpcPeerAuthContext context;
    context.m_policy = Policy::LocalTransportFallback;
    context.m_reason.clear();
    return context;
}

bool
IpcPeerAuthContext::permitsConnection() const
{
    return m_policy != Policy::Rejected;
}

bool
IpcPeerAuthContext::hasKernelIdentity() const
{
    return m_policy == Policy::KernelVerified;
}

CommandOrigin
IpcPeerAuthContext::commandOrigin() const
{
    if (m_policy != Policy::KernelVerified ||
        m_clientType != kIpcClientGui) {
        return CommandOrigin();
    }
    return CommandOrigin(m_processId, m_sessionId, m_userSid);
}

bool
IpcPeerAuthContext::authorizes(EIpcClientType clientType,
                               std::uint32_t processId,
                               std::string* reason) const
{
    if (m_policy == Policy::Rejected) {
        if (reason != nullptr) {
            *reason = m_reason.empty() ? "peer authentication failed" : m_reason;
        }
        return false;
    }
    if (processId == 0) {
        if (reason != nullptr) {
            *reason = "claimed IPC process id is invalid";
        }
        return false;
    }
    if (clientType != kIpcClientGui && clientType != kIpcClientNode) {
        if (reason != nullptr) {
            *reason = "claimed IPC role is invalid";
        }
        return false;
    }
    if (m_policy == Policy::KernelVerified) {
        if (processId != m_processId) {
            if (reason != nullptr) {
                *reason = "claimed process id does not own the IPC connection";
            }
            return false;
        }
        if (clientType != m_clientType) {
            if (reason != nullptr) {
                *reason = "claimed IPC role does not match the authenticated image";
            }
            return false;
        }
    }
    return true;
}

bool
IpcPeerAuthContext::authorizesSession(std::uint32_t sessionId,
                                     std::string* reason) const
{
    if (m_policy == Policy::Rejected) {
        if (reason != nullptr) {
            *reason = m_reason.empty() ? "peer authentication failed" : m_reason;
        }
        return false;
    }
    if (sessionId == 0) {
        if (reason != nullptr) {
            *reason = "claimed IPC session id is invalid";
        }
        return false;
    }
    if (m_policy == Policy::KernelVerified && sessionId != m_sessionId) {
        if (reason != nullptr) {
            *reason = "claimed IPC session does not match the authenticated token";
        }
        return false;
    }
    return true;
}

IpcPeerAuthContext
IpcPeerAuthenticator::authenticate(const TCPSocket& socket)
{
#if SYSAPI_WIN32
    return authenticateWindowsPeer(socket.m_socket);
#else
    (void)socket;
    return IpcPeerAuthContext::localTransportWithoutKernelIdentity();
#endif
}
