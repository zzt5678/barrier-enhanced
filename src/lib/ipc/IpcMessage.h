/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2012 Nick Bolton
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

#pragma once

#include "ipc/Ipc.h"
#include "ipc/IpcPeerAuthentication.h"
#include "base/EventTypes.h"
#include "base/Event.h"
#include <cstdint>
#include <string>

class IpcMessage : public EventData {
public:
    virtual ~IpcMessage();

    //! Gets the message type ID.
    UInt8                type() const { return m_type; }

protected:
    IpcMessage(UInt8 type);

private:
    UInt8                m_type;
};

class IpcHelloMessage : public IpcMessage {
public:
    IpcHelloMessage(EIpcClientType clientType, UInt32 processId = 0);
    virtual ~IpcHelloMessage();

    //! Gets the message type ID.
    EIpcClientType            clientType() const { return m_clientType; }
    UInt32                    processId() const { return m_processId; }

private:
    EIpcClientType            m_clientType;
    UInt32                    m_processId;
};

class IpcShutdownMessage : public IpcMessage {
public:
    IpcShutdownMessage();
    virtual ~IpcShutdownMessage();
};

class IpcStopRequestMessage : public IpcMessage {
public:
    explicit IpcStopRequestMessage(std::uint64_t requestId);
    virtual ~IpcStopRequestMessage();

    std::uint64_t requestId() const { return m_requestId; }
    const CommandOrigin& origin() const { return m_origin; }

private:
    friend class IpcClientProxy;

    void setOrigin(const CommandOrigin& origin) { m_origin = origin; }

    std::uint64_t m_requestId;
    CommandOrigin m_origin;
};

class IpcStopAckMessage : public IpcMessage {
public:
    IpcStopAckMessage(std::uint64_t requestId,
                      std::uint64_t commandGeneration);
    virtual ~IpcStopAckMessage();

    std::uint64_t requestId() const { return m_requestId; }
    std::uint64_t commandGeneration() const { return m_commandGeneration; }

private:
    std::uint64_t m_requestId;
    std::uint64_t m_commandGeneration;
};

class IpcNodeReadyMessage : public IpcMessage {
public:
    IpcNodeReadyMessage();
    virtual ~IpcNodeReadyMessage();
};

class IpcNodeReadyV2Message : public IpcMessage {
public:
    IpcNodeReadyV2Message(UInt32 processId, UInt32 sessionId,
                          std::uint64_t inputGeneration, bool inputReady,
                          const std::string& desktopName,
                          const std::string& buildId,
                          std::uint64_t queryNonce = 0);
    virtual ~IpcNodeReadyV2Message();

    UInt32 processId() const { return m_processId; }
    UInt32 sessionId() const { return m_sessionId; }
    std::uint64_t inputGeneration() const { return m_inputGeneration; }
    bool inputReady() const { return m_inputReady; }
    const std::string& desktopName() const { return m_desktopName; }
    const std::string& buildId() const { return m_buildId; }
    std::uint64_t queryNonce() const { return m_queryNonce; }

private:
    UInt32 m_processId;
    UInt32 m_sessionId;
    std::uint64_t m_inputGeneration;
    bool m_inputReady;
    std::string m_desktopName;
    std::string m_buildId;
    std::uint64_t m_queryNonce;
};

class IpcInputReadyQueryMessage : public IpcMessage {
public:
    explicit IpcInputReadyQueryMessage(std::uint64_t queryNonce);
    virtual ~IpcInputReadyQueryMessage();

    std::uint64_t queryNonce() const { return m_queryNonce; }

private:
    std::uint64_t m_queryNonce;
};

class IpcActivateNodeMessage : public IpcMessage {
public:
    explicit IpcActivateNodeMessage(std::uint64_t activationNonce);
    virtual ~IpcActivateNodeMessage();

    std::uint64_t activationNonce() const { return m_activationNonce; }

private:
    std::uint64_t m_activationNonce;
};

class IpcNodeActivatedMessage : public IpcMessage {
public:
    IpcNodeActivatedMessage(UInt32 processId,
                            std::uint64_t activationNonce);
    virtual ~IpcNodeActivatedMessage();

    UInt32 processId() const { return m_processId; }
    std::uint64_t activationNonce() const { return m_activationNonce; }

private:
    UInt32 m_processId;
    std::uint64_t m_activationNonce;
};


class IpcLogLineMessage : public IpcMessage {
public:
    IpcLogLineMessage(const std::string& logLine);
    virtual ~IpcLogLineMessage();

    //! Gets the log line.
    std::string logLine() const { return m_logLine; }

private:
    std::string m_logLine;
};

class IpcCommandMessage : public IpcMessage {
public:
    enum ElevateMode : UInt8 {
        kElevateAsNeeded = 0,
        kElevateAlways = 1,
        kElevateNever = 2
    };

    IpcCommandMessage(const std::string& command, UInt8 elevateMode);
    virtual ~IpcCommandMessage();

    //! Gets the command.
    std::string command() const { return m_command; }

    //! Gets whether or not the process should be elevated on MS Windows.
    bool                elevate() const { return m_elevateMode == kElevateAlways; }

    //! Gets the requested Windows elevation mode.
    UInt8               elevateMode() const { return m_elevateMode; }

    //! Gets the server-authenticated origin. This metadata is never on wire.
    const CommandOrigin& origin() const { return m_origin; }

private:
    friend class IpcClientProxy;

    void setOrigin(const CommandOrigin& origin) { m_origin = origin; }

    std::string m_command;
    UInt8               m_elevateMode;
    CommandOrigin       m_origin;
};
