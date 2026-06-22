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

#include "ipc/IpcMessage.h"
#include "ipc/Ipc.h"

IpcMessage::IpcMessage(UInt8 type) :
    m_type(type)
{
}

IpcMessage::~IpcMessage()
{
}

IpcHelloMessage::IpcHelloMessage(EIpcClientType clientType, UInt32 processId) :
    IpcMessage(kIpcHello),
    m_clientType(clientType),
    m_processId(processId)
{
}

IpcHelloMessage::~IpcHelloMessage()
{
}

IpcShutdownMessage::IpcShutdownMessage() :
IpcMessage(kIpcShutdown)
{
}

IpcShutdownMessage::~IpcShutdownMessage()
{
}

IpcLogLineMessage::IpcLogLineMessage(const std::string& logLine) :
    IpcMessage(kIpcLogLine),
    m_logLine(logLine)
{
}

IpcLogLineMessage::~IpcLogLineMessage()
{
}

IpcCommandMessage::IpcCommandMessage(const std::string& command, UInt8 elevateMode) :
    IpcMessage(kIpcCommand),
    m_command(command),
    m_elevateMode(elevateMode)
{
    if (m_elevateMode > kElevateNever) {
        m_elevateMode = kElevateAsNeeded;
    }
}

IpcCommandMessage::~IpcCommandMessage()
{
}
