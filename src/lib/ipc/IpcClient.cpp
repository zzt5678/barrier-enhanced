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

#include "ipc/IpcClient.h"
#include "ipc/Ipc.h"
#include "ipc/IpcServerProxy.h"
#include "ipc/IpcMessage.h"
#include "base/TMethodEventJob.h"

#include <memory>

#if SYSAPI_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace {

UInt32
currentProcessId()
{
#if SYSAPI_WIN32
    return static_cast<UInt32>(GetCurrentProcessId());
#else
    return static_cast<UInt32>(getpid());
#endif
}

UInt32
currentSessionId(UInt32 processId)
{
#if SYSAPI_WIN32
    DWORD sessionId = 0;
    return ProcessIdToSessionId(processId, &sessionId) ?
        static_cast<UInt32>(sessionId) : 0;
#else
    (void)processId;
    return 0;
#endif
}

}

//
// IpcClient
//

IpcClient::IpcClient(IEventQueue* events, SocketMultiplexer* socketMultiplexer,
                     EIpcClientType clientType) :
    m_serverAddress(NetworkAddress(IPC_HOST, IPC_PORT)),
    m_socket(events, socketMultiplexer, IArchNetwork::kINET),
    m_server(nullptr),
    m_events(events),
    m_clientType(clientType),
    m_processId(currentProcessId()),
    m_sessionId(currentSessionId(m_processId)),
    m_connectAttempted(false)
{
    init();
}

IpcClient::IpcClient(IEventQueue* events, SocketMultiplexer* socketMultiplexer, int port,
                     EIpcClientType clientType) :
    m_serverAddress(NetworkAddress(IPC_HOST, port)),
    m_socket(events, socketMultiplexer, IArchNetwork::kINET),
    m_server(nullptr),
    m_events(events),
    m_clientType(clientType),
    m_processId(currentProcessId()),
    m_sessionId(currentSessionId(m_processId)),
    m_connectAttempted(false)
{
    init();
}

void
IpcClient::init()
{
    m_serverAddress.resolve();
}

IpcClient::~IpcClient()
{
    try {
        disconnect();
    }
    catch (...) {
        // Destructors must not allow cleanup failures to escape.
    }
}

void
IpcClient::connect()
{
    if (m_connectAttempted) {
        return;
    }
    m_connectAttempted = true;

    try {
        std::unique_ptr<IpcServerProxy> server(new IpcServerProxy(m_socket, m_events));
        std::unique_ptr<IEventJob> messageHandler(
            new TMethodEventJob<IpcClient>(this, &IpcClient::handleMessageReceived));

        m_events->adoptHandler(
            m_events->forIpcServerProxy().messageReceived(), server.get(),
            messageHandler.get());
        messageHandler.release();
        m_server = server.release();

        std::unique_ptr<IEventJob> connectedHandler(
            new TMethodEventJob<IpcClient>(this, &IpcClient::handleConnected));
        m_events->adoptHandler(
            m_events->forIDataSocket().connected(), m_socket.getEventTarget(),
            connectedHandler.get());
        connectedHandler.release();

        m_socket.connect(m_serverAddress);
    }
    catch (...) {
        disconnect();
        throw;
    }
}

void
IpcClient::disconnect()
{
    m_connectAttempted = true;
    m_events->removeHandler(
        m_events->forIDataSocket().connected(), m_socket.getEventTarget());

    IpcServerProxy* server = m_server;
    m_server = nullptr;
    if (server != nullptr) {
        m_events->removeHandler(
            m_events->forIpcServerProxy().messageReceived(), server);
        delete server;
    }

    m_socket.close();
}

void
IpcClient::send(const IpcMessage& message)
{
    assert(m_server != nullptr);
    m_server->send(message);
}

void
IpcClient::handleConnected(const Event&, void*)
{
    m_events->addEvent(Event(
        m_events->forIpcClient().connected(), this, m_server, Event::kDontFreeData));

    IpcHelloMessage message(m_clientType, m_processId);
    send(message);
}

void
IpcClient::handleMessageReceived(const Event& e, void*)
{
    Event event(m_events->forIpcClient().messageReceived(), this);
    event.setDataObject(e.getDataObject());
    m_events->addEvent(event);
}
