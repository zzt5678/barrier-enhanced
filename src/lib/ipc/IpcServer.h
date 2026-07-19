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
#include "net/TCPListenSocket.h"
#include "net/NetworkAddress.h"
#include "arch/Arch.h"
#include "base/EventTypes.h"

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <vector>

class Event;
class IpcClientProxy;
class IpcMessage;
class IEventQueue;
class SocketMultiplexer;
class TCPSocket;
class IpcServerTestAccess;

//! IPC server for communication between daemon and GUI.
/*!
The IPC server listens on localhost. On Windows, localhost is only the
transport boundary: accepted sockets must also prove their kernel owner PID,
trusted component image, role, and token identity before protocol data is
accepted. The IPC client runs in both the client/server process and the GUI.
The IPC server runs on the daemon process.
*/
class IpcServer {
public:
    typedef IpcPeerAuthContext (*PeerAuthenticator)(const TCPSocket&);

    IpcServer(IEventQueue* events, SocketMultiplexer* socketMultiplexer);
    IpcServer(IEventQueue* events, SocketMultiplexer* socketMultiplexer, int port);
    virtual ~IpcServer();

    //! @name manipulators
    //@{

    //! Opens a TCP socket only allowing local connections.
    virtual void        listen();

    //! Send a message to all clients matching the filter type.
    virtual void        send(const IpcMessage& message, EIpcClientType filterType);
    virtual bool        sendToProcess(const IpcMessage& message, EIpcClientType filterType,
                                      UInt32 processId);
    virtual bool        sendActivateToProcess(UInt32 processId,
                                              std::uint64_t activationNonce);

    //@}
    //! @name accessors
    //@{

    //! Returns true when there are clients of the specified type connected.
    virtual bool        hasClients(EIpcClientType clientType) const;
    virtual bool        hasClientProcess(EIpcClientType clientType, UInt32 processId) const;
    virtual bool        hasReadyClientProcess(EIpcClientType clientType, UInt32 processId) const;
    virtual IpcInputReadinessResult inputReadinessProof(
                                                   EIpcClientType clientType,
                                                   UInt32 processId,
                                                   UInt32 sessionId,
                                                   const std::string& desktopName,
                                                   const std::string& buildId,
                                                   std::uint64_t queryNonce) const;
    virtual bool        hasInputReadyClientProcess(EIpcClientType clientType,
                                                   UInt32 processId,
                                                   UInt32 sessionId,
                                                   const std::string& desktopName,
                                                   const std::string& buildId,
                                                   std::uint64_t queryNonce = 0,
                                                   bool requireDesktopMatch = true,
                                                   std::string* reportedDesktopName = nullptr) const;
    virtual bool        hasActivatedClientProcess(UInt32 processId,
                                                  std::uint64_t activationNonce) const;

    //@}

private:
    friend class IpcServerTestAccess;

    IpcServer(IEventQueue* events, SocketMultiplexer* socketMultiplexer,
              int port, PeerAuthenticator peerAuthenticator);
    PeerAuthenticator testPeerAuthenticator() const;
    IpcInputReadinessResult inputReadiness(
                            EIpcClientType clientType,
                            UInt32 processId,
                            UInt32 sessionId,
                            const std::string& desktopName,
                            const std::string& buildId,
                            std::uint64_t queryNonce) const;
    void                sendWithAcquiredRefs(
                            const IpcMessage& message,
                            const std::vector<IpcClientProxy*>& recipients);

#if defined(BARRIER_TEST_ENV) || defined(BARRIER_TEST_ACCESS)
public:
#else
private:
#endif
    void                init();
    void                handleClientConnecting(const Event&, void*);
    void                handleClientDisconnected(const Event&, void*);
    void                handleMessageReceived(const Event&, void*);
    void                deleteClient(IpcClientProxy* proxy);

#if defined(BARRIER_TEST_ENV) || defined(BARRIER_TEST_ACCESS)
public:
#else
private:
#endif
    typedef std::list<IpcClientProxy*> ClientList;

    PeerAuthenticator   m_peerAuthenticator;
    bool                m_mock;
    IEventQueue*        m_events;
    SocketMultiplexer*    m_socketMultiplexer;
    TCPListenSocket*    m_socket;
    NetworkAddress        m_address;
    ClientList            m_clients;
    mutable std::mutex m_clientsMutex;

#ifdef BARRIER_TEST_ENV
public:
    IpcServer() :
        m_peerAuthenticator(nullptr),
        m_mock(true),
        m_events(nullptr),
        m_socketMultiplexer(nullptr),
        m_socket(nullptr) { }
#endif
};
