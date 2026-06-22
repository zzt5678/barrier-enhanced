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
#include "base/EventTypes.h"
#include "base/Event.h"
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

private:
    std::string m_command;
    UInt8               m_elevateMode;
};
