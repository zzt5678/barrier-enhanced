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

// TODO: fix, tests failing intermittently on mac.
#ifndef WINAPI_CARBON

#include "test/global/TestEventQueue.h"
#include "ipc/IpcServer.h"
#include "ipc/IpcClient.h"
#include "ipc/IpcServerProxy.h"
#include "ipc/IpcMessage.h"
#include "ipc/IpcClientProxy.h"
#include "ipc/Ipc.h"
#include "ipc/IpcPeerAuthentication.h"
#include "net/SocketMultiplexer.h"
#include "mt/Thread.h"
#include "arch/Arch.h"
#include "base/String.h"
#include "base/Log.h"
#include "base/EventQueue.h"
#include "base/TMethodEventJob.h"

#include "test/global/gtest.h"

#include <atomic>

#if SYSAPI_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <unistd.h>
#endif

#define TEST_IPC_PORT 24802

class IpcServerTestAccess final : public IpcServer
{
public:
    IpcServerTestAccess(IEventQueue* events,
                        SocketMultiplexer* socketMultiplexer, int port,
                        PeerAuthenticator peerAuthenticator) :
        IpcServer(events, socketMultiplexer, port, peerAuthenticator)
    {
    }

    static PeerAuthenticator peerAuthenticator(const IpcServer& server)
    {
        return server.testPeerAuthenticator();
    }
};

namespace {

std::atomic<UInt32> g_rejectingAuthenticatorCalls(0);

UInt32
currentTestProcessId()
{
#if SYSAPI_WIN32
    return static_cast<UInt32>(GetCurrentProcessId());
#else
    return static_cast<UInt32>(getpid());
#endif
}

IpcPeerAuthContext
authenticateCurrentNodeForTest(const TCPSocket&)
{
    return IpcPeerAuthContext::accepted(
        currentTestProcessId(), kIpcClientNode, 1,
        IpcPeerIntegrityLevel::Medium, "test-user-sid");
}

IpcPeerAuthContext
authenticateCurrentGuiForTest(const TCPSocket&)
{
    return IpcPeerAuthContext::accepted(
        currentTestProcessId(), kIpcClientGui, 1,
        IpcPeerIntegrityLevel::Medium, "test-user-sid");
}

IpcPeerAuthContext
rejectCurrentPeerForTest(const TCPSocket&)
{
    g_rejectingAuthenticatorCalls.fetch_add(1, std::memory_order_relaxed);
    return IpcPeerAuthContext::rejected("rejected by the IPC integration test");
}

}

class IpcTests : public ::testing::Test
{
public:
    IpcTests();
    virtual ~IpcTests();

    void                connectToServer_handleMessageReceived(const Event&, void*);
    void                sendMessageToServer_serverHandleMessageReceived(const Event&, void*);
    void                sendMessageToClient_serverHandleClientConnected(const Event&, void*);
    void                sendMessageToClient_clientHandleMessageReceived(const Event&, void*);
    void                runCommandRoundTrip(UInt8 elevateMode);

public:
    SocketMultiplexer    m_multiplexer;
    bool                m_connectToServer_helloMessageReceived;
    bool                m_connectToServer_hasClientNode;
    IpcServer*            m_connectToServer_server;
    String                m_sendMessageToServer_receivedString;
    UInt8                m_sendMessageToServer_elevateModeToSend;
    UInt8                m_sendMessageToServer_receivedElevateMode;
    bool                m_sendMessageToServer_receivedElevate;
    String                m_sendMessageToClient_receivedString;
    IpcClient*            m_sendMessageToServer_client;
    IpcServer*            m_sendMessageToClient_server;
    TestEventQueue        m_events;

};

TEST_F(IpcTests, connectToServer)
{
    SocketMultiplexer socketMultiplexer;
#if SYSAPI_WIN32
    IpcServerTestAccess server(
        &m_events, &socketMultiplexer, TEST_IPC_PORT,
        &authenticateCurrentNodeForTest);
#else
    IpcServer server(&m_events, &socketMultiplexer, TEST_IPC_PORT);
#endif
    server.listen();
    m_connectToServer_server = &server;

    m_events.adoptHandler(
        m_events.forIpcServer().messageReceived(), &server,
        new TMethodEventJob<IpcTests>(
        this, &IpcTests::connectToServer_handleMessageReceived));

    IpcClient client(&m_events, &socketMultiplexer, TEST_IPC_PORT);
    client.connect();

    m_events.initQuitTimeout(5);
    m_events.loop();
    m_events.removeHandler(m_events.forIpcServer().messageReceived(), &server);
    m_events.cleanupQuitTimeout();

    EXPECT_EQ(true, m_connectToServer_helloMessageReceived);
    EXPECT_EQ(true, m_connectToServer_hasClientNode);
}

TEST_F(IpcTests, disconnectWithoutConnectingIsIdempotent)
{
    SocketMultiplexer socketMultiplexer;
    IpcClient client(&m_events, &socketMultiplexer, TEST_IPC_PORT);

    client.disconnect();
    client.disconnect();
}

TEST_F(IpcTests, disconnectAfterConnectingIsIdempotent)
{
    SocketMultiplexer socketMultiplexer;
    IpcServerTestAccess server(
        &m_events, &socketMultiplexer, TEST_IPC_PORT,
        &authenticateCurrentNodeForTest);
    server.listen();
    m_connectToServer_server = &server;

    m_events.adoptHandler(
        m_events.forIpcServer().messageReceived(), &server,
        new TMethodEventJob<IpcTests>(
        this, &IpcTests::connectToServer_handleMessageReceived));

    IpcClient client(&m_events, &socketMultiplexer, TEST_IPC_PORT);
    client.connect();

    m_events.initQuitTimeout(5);
    m_events.loop();
    m_events.removeHandler(m_events.forIpcServer().messageReceived(), &server);
    m_events.cleanupQuitTimeout();

    ASSERT_TRUE(m_connectToServer_helloMessageReceived);
    ASSERT_TRUE(m_connectToServer_hasClientNode);

    client.disconnect();
    client.disconnect();
}

TEST_F(IpcTests, repeatedConnectIsNoOp)
{
    SocketMultiplexer socketMultiplexer;
    IpcServerTestAccess server(
        &m_events, &socketMultiplexer, TEST_IPC_PORT,
        &authenticateCurrentNodeForTest);
    server.listen();
    m_connectToServer_server = &server;

    m_events.adoptHandler(
        m_events.forIpcServer().messageReceived(), &server,
        new TMethodEventJob<IpcTests>(
        this, &IpcTests::connectToServer_handleMessageReceived));

    IpcClient client(&m_events, &socketMultiplexer, TEST_IPC_PORT);
    client.connect();
    client.connect();

    m_events.initQuitTimeout(5);
    m_events.loop();
    m_events.removeHandler(m_events.forIpcServer().messageReceived(), &server);
    m_events.cleanupQuitTimeout();

    EXPECT_TRUE(m_connectToServer_helloMessageReceived);
    EXPECT_TRUE(m_connectToServer_hasClientNode);
}

TEST_F(IpcTests, sendMessageToServer)
{
    runCommandRoundTrip(IpcCommandMessage::kElevateAlways);

    EXPECT_EQ("test", m_sendMessageToServer_receivedString);
    EXPECT_EQ(IpcCommandMessage::kElevateAlways,
              m_sendMessageToServer_receivedElevateMode);
    EXPECT_TRUE(m_sendMessageToServer_receivedElevate);
}

TEST_F(IpcTests, commandMessagePreservesElevateModeAcrossIpcEncoding)
{
    runCommandRoundTrip(IpcCommandMessage::kElevateAsNeeded);
    EXPECT_EQ(IpcCommandMessage::kElevateAsNeeded,
              m_sendMessageToServer_receivedElevateMode);
    EXPECT_FALSE(m_sendMessageToServer_receivedElevate);

    runCommandRoundTrip(IpcCommandMessage::kElevateAlways);
    EXPECT_EQ(IpcCommandMessage::kElevateAlways,
              m_sendMessageToServer_receivedElevateMode);
    EXPECT_TRUE(m_sendMessageToServer_receivedElevate);

    runCommandRoundTrip(IpcCommandMessage::kElevateNever);
    EXPECT_EQ(IpcCommandMessage::kElevateNever,
              m_sendMessageToServer_receivedElevateMode);
    EXPECT_FALSE(m_sendMessageToServer_receivedElevate);

    IpcCommandMessage invalid("test", IpcCommandMessage::kElevateNever + 1);
    EXPECT_EQ(IpcCommandMessage::kElevateAsNeeded, invalid.elevateMode());
    EXPECT_FALSE(invalid.elevate());
}

TEST_F(IpcTests, sendMessageToClient)
{
    SocketMultiplexer socketMultiplexer;
    IpcServerTestAccess server(
        &m_events, &socketMultiplexer, TEST_IPC_PORT,
        &authenticateCurrentNodeForTest);
    server.listen();
    m_sendMessageToClient_server = &server;

    // event handler sends "test" log line to client.
    m_events.adoptHandler(
        m_events.forIpcServer().messageReceived(), &server,
        new TMethodEventJob<IpcTests>(
        this, &IpcTests::sendMessageToClient_serverHandleClientConnected));

    IpcClient client(&m_events, &socketMultiplexer, TEST_IPC_PORT);
    client.connect();

    m_events.adoptHandler(
        m_events.forIpcClient().messageReceived(), &client,
        new TMethodEventJob<IpcTests>(
        this, &IpcTests::sendMessageToClient_clientHandleMessageReceived));

    m_events.initQuitTimeout(5);
    m_events.loop();
    m_events.removeHandler(m_events.forIpcServer().messageReceived(), &server);
    m_events.removeHandler(m_events.forIpcClient().messageReceived(), &client);
    m_events.cleanupQuitTimeout();

    EXPECT_EQ("test", m_sendMessageToClient_receivedString);
}

IpcTests::IpcTests() :
m_connectToServer_helloMessageReceived(false),
m_connectToServer_hasClientNode(false),
m_connectToServer_server(nullptr),
m_sendMessageToServer_elevateModeToSend(IpcCommandMessage::kElevateAlways),
m_sendMessageToServer_receivedElevateMode(IpcCommandMessage::kElevateAsNeeded),
m_sendMessageToServer_receivedElevate(false),
m_sendMessageToClient_server(nullptr),
m_sendMessageToServer_client(nullptr)
{
}

IpcTests::~IpcTests()
{
}

class IpcServerAuthenticationTests : public ::testing::Test
{
public:
    IpcServerAuthenticationTests() :
        m_clientConnected(false),
        m_serverAcceptedClient(false),
        m_observationTimer(nullptr)
    {
    }

    void handleClientConnected(const Event&, void*)
    {
        m_clientConnected = true;
        if (m_observationTimer == nullptr) {
            m_observationTimer = m_events.newOneShotTimer(0.5, nullptr);
            m_events.adoptHandler(
                Event::kTimer, m_observationTimer,
                new TMethodEventJob<IpcServerAuthenticationTests>(
                    this,
                    &IpcServerAuthenticationTests::handleObservationElapsed));
        }
    }

    void handleServerAcceptedClient(const Event&, void*)
    {
        m_serverAcceptedClient = true;
    }

    void handleObservationElapsed(const Event&, void*)
    {
        m_events.raiseQuitEvent();
    }

    void runRejectedConnectionAttempt(IpcServer& server,
                                      SocketMultiplexer& socketMultiplexer,
                                      int port)
    {
        server.listen();
        IpcClient client(&m_events, &socketMultiplexer, port);
        m_events.adoptHandler(
            m_events.forIpcClient().connected(), &client,
            new TMethodEventJob<IpcServerAuthenticationTests>(
                this, &IpcServerAuthenticationTests::handleClientConnected));
        m_events.adoptHandler(
            m_events.forIpcServer().clientConnected(), &server,
            new TMethodEventJob<IpcServerAuthenticationTests>(
                this, &IpcServerAuthenticationTests::handleServerAcceptedClient));

        client.connect();
        m_events.initQuitTimeout(5);
        m_events.loop();

        m_events.removeHandler(m_events.forIpcClient().connected(), &client);
        m_events.removeHandler(m_events.forIpcServer().clientConnected(), &server);
        m_events.cleanupQuitTimeout();
        if (m_observationTimer != nullptr) {
            m_events.removeHandler(Event::kTimer, m_observationTimer);
            m_events.deleteTimer(m_observationTimer);
            m_observationTimer = nullptr;
        }

        EXPECT_TRUE(m_clientConnected);
        EXPECT_FALSE(m_serverAcceptedClient);
        EXPECT_FALSE(server.hasClients(kIpcClientNode));
    }

protected:
    TestEventQueue m_events;
    bool m_clientConnected;
    bool m_serverAcceptedClient;
    EventQueueTimer* m_observationTimer;
};

TEST_F(IpcServerAuthenticationTests, defaultServerUsesKernelPeerAuthenticator)
{
    SocketMultiplexer socketMultiplexer;
    IpcServer server(&m_events, &socketMultiplexer, TEST_IPC_PORT + 1);

    EXPECT_EQ(&IpcPeerAuthenticator::authenticate,
              IpcServerTestAccess::peerAuthenticator(server));
}

TEST_F(IpcServerAuthenticationTests, overrideIsLimitedToOneTestInstance)
{
    SocketMultiplexer socketMultiplexer;
    IpcServerTestAccess overridden(
        &m_events, &socketMultiplexer, TEST_IPC_PORT + 1,
        &authenticateCurrentNodeForTest);
    IpcServer productionDefault(
        &m_events, &socketMultiplexer, TEST_IPC_PORT + 2);

    EXPECT_EQ(&authenticateCurrentNodeForTest,
              IpcServerTestAccess::peerAuthenticator(overridden));
    EXPECT_EQ(&IpcPeerAuthenticator::authenticate,
              IpcServerTestAccess::peerAuthenticator(productionDefault));
}

TEST_F(IpcServerAuthenticationTests, rejectedAuthenticatorFailsClosed)
{
    g_rejectingAuthenticatorCalls.store(0, std::memory_order_relaxed);
    SocketMultiplexer socketMultiplexer;
    IpcServerTestAccess server(
        &m_events, &socketMultiplexer, TEST_IPC_PORT + 3,
        &rejectCurrentPeerForTest);

    runRejectedConnectionAttempt(server, socketMultiplexer, TEST_IPC_PORT + 3);
    EXPECT_EQ(1u, g_rejectingAuthenticatorCalls.load(std::memory_order_relaxed));
}

TEST_F(IpcServerAuthenticationTests, nullAuthenticatorFailsClosed)
{
    SocketMultiplexer socketMultiplexer;
    IpcServerTestAccess server(
        &m_events, &socketMultiplexer, TEST_IPC_PORT + 4, nullptr);

    EXPECT_EQ(nullptr, IpcServerTestAccess::peerAuthenticator(server));
    runRejectedConnectionAttempt(server, socketMultiplexer, TEST_IPC_PORT + 4);
}

#if SYSAPI_WIN32
TEST_F(IpcServerAuthenticationTests, defaultServerRejectsSameProcessClient)
{
    SocketMultiplexer socketMultiplexer;
    IpcServer server(&m_events, &socketMultiplexer, TEST_IPC_PORT + 5);

    runRejectedConnectionAttempt(server, socketMultiplexer, TEST_IPC_PORT + 5);
}
#endif

void
IpcTests::runCommandRoundTrip(UInt8 elevateMode)
{
    m_sendMessageToServer_receivedString.clear();
    m_sendMessageToServer_elevateModeToSend = elevateMode;
    m_sendMessageToServer_receivedElevateMode = IpcCommandMessage::kElevateAsNeeded;
    m_sendMessageToServer_receivedElevate = false;

    SocketMultiplexer socketMultiplexer;
    IpcServerTestAccess server(
        &m_events, &socketMultiplexer, TEST_IPC_PORT,
        &authenticateCurrentGuiForTest);
    server.listen();

    // event handler sends "test" command to server.
    m_events.adoptHandler(
        m_events.forIpcServer().messageReceived(), &server,
        new TMethodEventJob<IpcTests>(
        this, &IpcTests::sendMessageToServer_serverHandleMessageReceived));

    IpcClient client(&m_events, &socketMultiplexer, TEST_IPC_PORT, kIpcClientGui);
    client.connect();
    m_sendMessageToServer_client = &client;

    m_events.initQuitTimeout(5);
    m_events.loop();
    m_events.removeHandler(m_events.forIpcServer().messageReceived(), &server);
    m_events.cleanupQuitTimeout();

    m_sendMessageToServer_client = nullptr;
}

void
IpcTests::connectToServer_handleMessageReceived(const Event& e, void*)
{
    IpcMessage* m = static_cast<IpcMessage*>(e.getDataObject());
    if (m->type() == kIpcHello) {
        m_connectToServer_hasClientNode =
            m_connectToServer_server->hasClients(kIpcClientNode);
        m_connectToServer_helloMessageReceived = true;
        m_events.raiseQuitEvent();
    }
}

void
IpcTests::sendMessageToServer_serverHandleMessageReceived(const Event& e, void*)
{
    IpcMessage* m = static_cast<IpcMessage*>(e.getDataObject());
    if (m->type() == kIpcHello) {
        LOG((CLOG_DEBUG "client said hello, sending test to server"));
        IpcCommandMessage m("test", m_sendMessageToServer_elevateModeToSend);
        m_sendMessageToServer_client->send(m);
    }
    else if (m->type() == kIpcCommand) {
        IpcCommandMessage* cm = static_cast<IpcCommandMessage*>(m);
        LOG((CLOG_DEBUG "got ipc command message, %d", cm->command().c_str()));
        m_sendMessageToServer_receivedString = cm->command();
        m_sendMessageToServer_receivedElevateMode = cm->elevateMode();
        m_sendMessageToServer_receivedElevate = cm->elevate();
        m_events.raiseQuitEvent();
    }
}

void
IpcTests::sendMessageToClient_serverHandleClientConnected(const Event& e, void*)
{
    IpcMessage* m = static_cast<IpcMessage*>(e.getDataObject());
    if (m->type() == kIpcHello) {
        LOG((CLOG_DEBUG "client said hello, sending test to client"));
        IpcLogLineMessage m("test");
        m_sendMessageToClient_server->send(m, kIpcClientNode);
    }
}

void
IpcTests::sendMessageToClient_clientHandleMessageReceived(const Event& e, void*)
{
    IpcMessage* m = static_cast<IpcMessage*>(e.getDataObject());
    if (m->type() == kIpcLogLine) {
        IpcLogLineMessage* llm = static_cast<IpcLogLineMessage*>(m);
        LOG((CLOG_DEBUG "got ipc log message, %d", llm->logLine().c_str()));
        m_sendMessageToClient_receivedString = llm->logLine();
        m_events.raiseQuitEvent();
    }
}

#endif // WINAPI_CARBON
