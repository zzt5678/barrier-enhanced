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

#include "ipc/IpcServer.h"

#include "ipc/Ipc.h"
#include "ipc/IpcClientProxy.h"
#include "ipc/IpcMessage.h"
#include "ipc/IpcPeerAuthentication.h"
#include "net/IDataSocket.h"
#include "net/TCPSocket.h"
#include "io/IStream.h"
#include "base/IEventQueue.h"
#include "base/TMethodEventJob.h"
#include "base/Event.h"
#include "base/Log.h"

#include <vector>

//
// IpcServer
//

IpcServer::IpcServer(IEventQueue* events, SocketMultiplexer* socketMultiplexer) :
    m_peerAuthenticator(&IpcPeerAuthenticator::authenticate),
    m_mock(false),
    m_events(events),
    m_socketMultiplexer(socketMultiplexer),
    m_socket(nullptr),
    m_address(NetworkAddress(IPC_HOST, IPC_PORT))
{
    init();
}

IpcServer::IpcServer(IEventQueue* events, SocketMultiplexer* socketMultiplexer, int port) :
    m_peerAuthenticator(&IpcPeerAuthenticator::authenticate),
    m_mock(false),
    m_events(events),
    m_socketMultiplexer(socketMultiplexer),
    m_address(NetworkAddress(IPC_HOST, port))
{
    init();
}

IpcServer::IpcServer(IEventQueue* events,
                     SocketMultiplexer* socketMultiplexer, int port,
                     PeerAuthenticator peerAuthenticator) :
    IpcServer(events, socketMultiplexer, port)
{
    m_peerAuthenticator = peerAuthenticator;
}

IpcServer::PeerAuthenticator
IpcServer::testPeerAuthenticator() const
{
    return m_peerAuthenticator;
}

void
IpcServer::init()
{
    m_socket = new TCPListenSocket(m_events, m_socketMultiplexer, IArchNetwork::kINET);

    m_address.resolve();

    m_events->adoptHandler(
        m_events->forIListenSocket().connecting(), m_socket,
        new TMethodEventJob<IpcServer>(
        this, &IpcServer::handleClientConnecting));
}

IpcServer::~IpcServer()
{
    if (m_mock) {
        return;
    }

    if (m_socket != nullptr) {
        m_events->removeHandler(m_events->forIListenSocket().connecting(), m_socket);
        delete m_socket;
    }

    ClientList clients;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        clients.swap(m_clients);
    }

    ClientList::iterator it;
    for (it = clients.begin(); it != clients.end(); it++) {
        deleteClient(*it);
    }

}

void
IpcServer::listen()
{
    m_socket->bind(m_address);
}

void
IpcServer::handleClientConnecting(const Event&, void*)
{
    barrier::IStream* stream = m_socket->accept();
    if (stream == NULL) {
        return;
    }

    LOG((CLOG_DEBUG "accepted ipc client connection"));

    TCPSocket* tcpSocket = dynamic_cast<TCPSocket*>(stream);
    const IpcPeerAuthContext peerAuth =
        tcpSocket != nullptr && m_peerAuthenticator != nullptr
        ? m_peerAuthenticator(*tcpSocket)
        : IpcPeerAuthContext::rejected(
            tcpSocket == nullptr
                ? "accepted IPC transport is not a TCP socket"
                : "IPC peer authenticator is unavailable");
    if (!peerAuth.permitsConnection()) {
        LOG((CLOG_WARN "rejecting local ipc connection: %s",
             peerAuth.rejectionReason().c_str()));
        delete stream;
        return;
    }

    IpcClientProxy* proxy = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        proxy = new IpcClientProxy(*stream, m_events, peerAuth);
        m_clients.push_back(proxy);
    }

    m_events->adoptHandler(
        m_events->forIpcClientProxy().disconnected(), proxy,
        new TMethodEventJob<IpcServer>(
        this, &IpcServer::handleClientDisconnected));

    m_events->adoptHandler(
        m_events->forIpcClientProxy().messageReceived(), proxy,
        new TMethodEventJob<IpcServer>(
        this, &IpcServer::handleMessageReceived));

    m_events->addEvent(Event(
        m_events->forIpcServer().clientConnected(), this, proxy, Event::kDontFreeData));
}

void
IpcServer::handleClientDisconnected(const Event& e, void*)
{
    IpcClientProxy* proxy = static_cast<IpcClientProxy*>(e.getTarget());

    int connected = 0;
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clients.remove(proxy);
        connected = static_cast<int>(m_clients.size());
    }

    deleteClient(proxy);

    LOG((CLOG_DEBUG "ipc client proxy removed, connected=%d", connected));
}

void
IpcServer::handleMessageReceived(const Event& e, void*)
{
    Event event(m_events->forIpcServer().messageReceived(), this);
    event.setDataObject(e.getDataObject());
    m_events->addEvent(event);
}

void
IpcServer::deleteClient(IpcClientProxy* proxy)
{
    m_events->removeHandler(m_events->forIpcClientProxy().messageReceived(), proxy);
    m_events->removeHandler(m_events->forIpcClientProxy().disconnected(), proxy);
    delete proxy;
}

bool
IpcServer::hasClients(EIpcClientType clientType) const
{
    std::lock_guard<std::mutex> lock(m_clientsMutex);

    if (m_clients.empty()) {
        return false;
    }

    ClientList::const_iterator it;
    for (it = m_clients.begin(); it != m_clients.end(); it++) {
        // at least one client is alive and type matches, there are clients.
        IpcClientProxy* p = *it;
        if (!p->m_disconnecting && p->m_clientType.load() == clientType) {
            return true;
        }
    }

    // all clients must be disconnecting, no active clients.
    return false;
}

bool
IpcServer::hasClientProcess(EIpcClientType clientType, UInt32 processId) const
{
    if (processId == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (ClientList::const_iterator it = m_clients.begin(); it != m_clients.end(); ++it) {
        IpcClientProxy* proxy = *it;
        if (!proxy->m_disconnecting &&
            proxy->m_clientType.load() == clientType &&
            proxy->m_processId.load() == processId) {
            return true;
        }
    }

    return false;
}

bool
IpcServer::hasReadyClientProcess(EIpcClientType clientType, UInt32 processId) const
{
    if (processId == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (ClientList::const_iterator it = m_clients.begin(); it != m_clients.end(); ++it) {
        IpcClientProxy* proxy = *it;
        if (!proxy->m_disconnecting.load() && proxy->m_ready.load() &&
            proxy->m_clientType.load() == clientType &&
            proxy->m_processId.load() == processId) {
            return true;
        }
    }

    return false;
}

bool
IpcServer::hasInputReadyClientProcess(EIpcClientType clientType,
                                      UInt32 processId, UInt32 sessionId,
                                      const std::string& desktopName,
                                      const std::string& buildId,
                                      std::uint64_t queryNonce,
                                      bool requireDesktopMatch,
                                      std::string* reportedDesktopName) const
{
    if (processId == 0 || clientType != kIpcClientNode) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (ClientList::const_iterator it = m_clients.begin(); it != m_clients.end(); ++it) {
        if ((*it)->matchesInputReadiness(
                processId, sessionId, desktopName, buildId, queryNonce,
                requireDesktopMatch, reportedDesktopName)) {
            return true;
        }
    }

    return false;
}

bool
IpcServer::hasActivatedClientProcess(UInt32 processId,
                                     std::uint64_t activationNonce) const
{
    if (processId == 0 || activationNonce == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (ClientList::const_iterator it = m_clients.begin();
         it != m_clients.end(); ++it) {
        if ((*it)->matchesActivation(processId, activationNonce)) {
            return true;
        }
    }

    return false;
}

void
IpcServer::sendWithAcquiredRefs(
    const IpcMessage& message,
    const std::vector<IpcClientProxy*>& recipients)
{
    try {
        for (std::vector<IpcClientProxy*>::const_iterator it = recipients.begin();
             it != recipients.end(); ++it) {
            (*it)->send(message);
        }
    }
    catch (...) {
        for (std::vector<IpcClientProxy*>::const_iterator it = recipients.begin();
             it != recipients.end(); ++it) {
            (*it)->releaseSendRef();
        }
        throw;
    }

    for (std::vector<IpcClientProxy*>::const_iterator it = recipients.begin();
         it != recipients.end(); ++it) {
        (*it)->releaseSendRef();
    }
}

void
IpcServer::send(const IpcMessage& message, EIpcClientType filterType)
{
    std::vector<IpcClientProxy*> recipients;

    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);

        ClientList::iterator it;
        for (it = m_clients.begin(); it != m_clients.end(); it++) {
            IpcClientProxy* proxy = *it;
            if (proxy->m_clientType.load() == filterType &&
                proxy->tryAddSendRef()) {
                recipients.push_back(proxy);
            }
        }
    }

    sendWithAcquiredRefs(message, recipients);
}

bool
IpcServer::sendToProcess(const IpcMessage& message, EIpcClientType filterType,
                         UInt32 processId)
{
    if (processId == 0) {
        return false;
    }

    std::vector<IpcClientProxy*> recipients;

    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);

        ClientList::iterator it;
        for (it = m_clients.begin(); it != m_clients.end(); it++) {
            IpcClientProxy* proxy = *it;
            if (proxy->m_clientType.load() == filterType &&
                proxy->m_processId.load() == processId &&
                proxy->tryAddSendRef()) {
                recipients.push_back(proxy);
            }
        }
    }

    sendWithAcquiredRefs(message, recipients);

    return !recipients.empty();
}

bool
IpcServer::sendActivateToProcess(UInt32 processId,
                                 std::uint64_t activationNonce)
{
    if (processId == 0 || activationNonce == 0) {
        return false;
    }

    IpcActivateNodeMessage activate(activationNonce);
    return sendToProcess(activate, kIpcClientNode, processId);
}
