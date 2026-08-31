/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "ipc/Ipc.h"

#include <cstdint>
#include <string>

class TCPSocket;

class CommandOrigin {
public:
    CommandOrigin();

    std::uint32_t processId() const { return m_processId; }
    std::uint32_t sessionId() const { return m_sessionId; }
    const std::string& userSid() const { return m_userSid; }
    bool kernelVerified() const { return m_kernelVerified; }

private:
    friend class IpcPeerAuthContext;

    CommandOrigin(std::uint32_t processId, std::uint32_t sessionId,
                  const std::string& userSid);

    std::uint32_t m_processId;
    std::uint32_t m_sessionId;
    std::string m_userSid;
    bool m_kernelVerified;
};

enum class IpcPeerIntegrityLevel : std::uint32_t {
    Unknown = 0,
    Untrusted = 0x0000,
    Low = 0x1000,
    Medium = 0x2000,
    High = 0x3000,
    System = 0x4000,
    ProtectedProcess = 0x5000,
};

class IpcPeerAuthContext {
public:
    IpcPeerAuthContext();

    static IpcPeerAuthContext accepted(std::uint32_t processId,
                                       EIpcClientType clientType,
                                       std::uint32_t sessionId,
                                       IpcPeerIntegrityLevel integrityLevel,
                                       const std::string& userSid);
    static IpcPeerAuthContext rejected(const std::string& reason);

    bool permitsConnection() const;
    const std::string& rejectionReason() const { return m_reason; }
    bool hasKernelIdentity() const;
    std::uint32_t authenticatedSessionId() const { return m_sessionId; }
    IpcPeerIntegrityLevel integrityLevel() const { return m_integrityLevel; }
    CommandOrigin commandOrigin() const;
    bool authorizes(EIpcClientType clientType, std::uint32_t processId,
                    std::string* reason) const;
    bool authorizesSession(std::uint32_t sessionId,
                           std::string* reason) const;

private:
    friend class IpcPeerAuthenticator;

    static IpcPeerAuthContext localTransportWithoutKernelIdentity();

    enum class Policy {
        Rejected,
        KernelVerified,
        LocalTransportFallback,
    };

    Policy m_policy;
    std::uint32_t m_processId;
    EIpcClientType m_clientType;
    std::uint32_t m_sessionId;
    IpcPeerIntegrityLevel m_integrityLevel;
    std::string m_userSid;
    std::string m_reason;
};

class IpcPeerAuthenticator {
public:
    static IpcPeerAuthContext authenticate(const TCPSocket& socket);
};

namespace IpcPeerAuthenticationPolicy {

// A GUI is always the interactive user. A protected node may run as
// LocalSystem when the watchdog follows the Windows secure input desktop.
bool tokenOwnerAllowed(EIpcClientType clientType,
                       bool matchesInteractiveUser,
                       bool isLocalSystem);

} // namespace IpcPeerAuthenticationPolicy
