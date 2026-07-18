/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2013-2016 Symless Ltd.
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

#define BARRIER_TEST_ENV

#include "test/mock/server/MockConfig.h"
#include "test/mock/server/MockPrimaryClient.h"
#include "test/mock/barrier/MockScreen.h"
#include "test/mock/server/MockInputFilter.h"
#include "test/global/TestEventQueue.h"
#include "server/Server.h"
#include "server/ClientListener.h"
#include "server/ClientProxy.h"
#include "client/Client.h"
#include "barrier/FileTransferSendState.h"
#include "net/SocketMultiplexer.h"
#include "net/NetworkAddress.h"
#include "net/TCPSocketFactory.h"
#include "mt/Thread.h"
#include "base/TMethodEventJob.h"
#include "base/Log.h"
#include <stdexcept>

#include "test/global/gtest.h"
#include <sstream>
#include <fstream>
#include <iostream>
#include <stdio.h>

#if SYSAPI_WIN32
#include <process.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace std;
using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::Invoke;

#define TEST_HOST "127.0.0.1"

const char* kMockFilename = "NetworkTests.mock";
const size_t kMockFileSize = 1024 * 1024 * 10; // 10MB
const char* kRepeatedMockFilename = "NetworkTests.repeated.mock";
const int kRepeatedMockDataTransfers = 8;
const size_t kRepeatedMockDataSize = 1024 * 1024; // 1MB per connection-reuse cycle

int getFreeTestPort();
void getScreenShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h);
void getCursorPos(SInt32& x, SInt32& y);
UInt8* newMockData(size_t size);
void createFile(fstream& file, const char* filename, size_t size);

class NetworkPrimaryClient : public MockPrimaryClient
{
public:
    void getShape(SInt32& x, SInt32& y,
                  SInt32& width, SInt32& height) const override
    {
        getScreenShape(x, y, width, height);
    }
};

class NetworkTests : public ::testing::Test
{
public:
    struct ClientFileSendContext {
        ClientListener* listener;
        Client* client;
    };

    NetworkTests() :
        m_transferPollTimer(NULL),
        m_transferServerSender(NULL),
        m_transferClientSender(NULL),
        m_transferReceiveCompleted(false),
        m_repeatServerTransfers(false),
        m_transferPollTicks(0),
        m_completedTransfers(0)
    {
        createFile(m_mockFile, kMockFilename, kMockFileSize);
        createFile(m_repeatedMockFile, kRepeatedMockFilename,
                   kRepeatedMockDataSize);
    }

    ~NetworkTests()
    {
        cleanupTransferCompletionPoll();
        remove(kMockFilename);
        remove(kRepeatedMockFilename);
    }

    void                startTransferCompletionPoll(Server* serverSender,
                                                     Client* clientSender,
                                                     bool repeatServerTransfers);
    void                cleanupTransferCompletionPoll();
    void                handleTransferCompletionPoll(const Event&, void*);
    void                markTransferReceiveCompleted(const Event&, void* receiver);

    void                sendToClient_mockData_handleClientConnected(const Event&, void* vlistener);
    void                sendToClient_mockData_fileRecieveCompleted(const Event&, void*);

    void                sendToClient_mockFile_handleClientConnected(const Event&, void* vlistener);
    void                sendToClient_mockFile_fileRecieveCompleted(const Event& event, void*);

    void                sendToServer_mockData_handleClientConnected(const Event&, void* vlistener);
    void                sendToServer_mockData_fileRecieveCompleted(const Event& event, void*);

    void                sendToServer_mockFile_handleClientConnected(const Event&, void* vlistener);
    void                sendToServer_mockFile_fileRecieveCompleted(const Event& event, void*);

    void                repeatedSendToClient_mockData_handleClientConnected(const Event&, void* vlistener);
    void                repeatedSendToClient_mockData_fileRecieveCompleted(const Event& event, void*);

public:
    TestEventQueue        m_events;
    fstream                m_mockFile;
    fstream                m_repeatedMockFile;
    EventQueueTimer*       m_transferPollTimer;
    Server*                m_transferServerSender;
    Client*                m_transferClientSender;
    bool                   m_transferReceiveCompleted;
    bool                   m_repeatServerTransfers;
    int                    m_transferPollTicks;
    int                    m_completedTransfers;
};

TEST_F(NetworkTests, sendToClient_mockData)
{
    // server and client
    NetworkAddress serverAddress(TEST_HOST, getFreeTestPort());

    serverAddress.resolve();

    // server
    SocketMultiplexer serverSocketMultiplexer;
    TCPSocketFactory* serverSocketFactory = new TCPSocketFactory(&m_events, &serverSocketMultiplexer);
    ClientListener listener(serverAddress, serverSocketFactory, &m_events,
                            ConnectionSecurityLevel::PLAINTEXT);
    NiceMock<MockScreen> serverScreen;
    NiceMock<NetworkPrimaryClient> primaryClient;
    NiceMock<MockConfig> serverConfig;
    NiceMock<MockInputFilter> serverInputFilter;

    m_events.adoptHandler(
        m_events.forClientListener().connected(), &listener,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::sendToClient_mockData_handleClientConnected, &listener));

    ON_CALL(serverConfig, isScreen(_)).WillByDefault(Return(true));
    ON_CALL(serverConfig, getInputFilter()).WillByDefault(Return(&serverInputFilter));

    ServerArgs serverArgs;
    serverArgs.m_enableDragDrop = true;
    Server server(serverConfig, &primaryClient, &serverScreen, &m_events, serverArgs);
    listener.setServer(&server);

    // client
    NiceMock<MockScreen> clientScreen;
    SocketMultiplexer clientSocketMultiplexer;
    TCPSocketFactory* clientSocketFactory = new TCPSocketFactory(&m_events, &clientSocketMultiplexer);

    ON_CALL(clientScreen, getShape(_, _, _, _)).WillByDefault(Invoke(getScreenShape));
    ON_CALL(clientScreen, getCursorPos(_, _)).WillByDefault(Invoke(getCursorPos));


    ClientArgs clientArgs;
    clientArgs.m_enableDragDrop = true;
    clientArgs.m_enableCrypto = false;
    Client client(&m_events, "stub", serverAddress, clientSocketFactory, &clientScreen, clientArgs);

    m_events.adoptHandler(
        m_events.forFile().dropDirWriteFinished(), &client,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::sendToClient_mockData_fileRecieveCompleted,
            &client));

    startTransferCompletionPoll(&server, NULL, false);

    client.connect();

    m_events.initQuitTimeout(30);
    m_events.loop();
    cleanupTransferCompletionPoll();
    EXPECT_EQ(1, m_completedTransfers);
    server.setActive(&primaryClient);
    m_events.removeHandler(m_events.forClientListener().connected(), &listener);
    m_events.removeHandler(m_events.forFile().dropDirWriteFinished(), &client);
    m_events.cleanupQuitTimeout();
}

TEST_F(NetworkTests, sendToClient_mockFile)
{
    // server and client
    NetworkAddress serverAddress(TEST_HOST, getFreeTestPort());

    serverAddress.resolve();

    // server
    SocketMultiplexer serverSocketMultiplexer;
    TCPSocketFactory* serverSocketFactory = new TCPSocketFactory(&m_events, &serverSocketMultiplexer);
    ClientListener listener(serverAddress, serverSocketFactory, &m_events,
                            ConnectionSecurityLevel::PLAINTEXT);
    NiceMock<MockScreen> serverScreen;
    NiceMock<NetworkPrimaryClient> primaryClient;
    NiceMock<MockConfig> serverConfig;
    NiceMock<MockInputFilter> serverInputFilter;

    m_events.adoptHandler(
        m_events.forClientListener().connected(), &listener,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::sendToClient_mockFile_handleClientConnected, &listener));

    ON_CALL(serverConfig, isScreen(_)).WillByDefault(Return(true));
    ON_CALL(serverConfig, getInputFilter()).WillByDefault(Return(&serverInputFilter));

    ServerArgs serverArgs;
    serverArgs.m_enableDragDrop = true;
    Server server(serverConfig, &primaryClient, &serverScreen, &m_events, serverArgs);
    listener.setServer(&server);

    // client
    NiceMock<MockScreen> clientScreen;
    SocketMultiplexer clientSocketMultiplexer;
    TCPSocketFactory* clientSocketFactory = new TCPSocketFactory(&m_events, &clientSocketMultiplexer);

    ON_CALL(clientScreen, getShape(_, _, _, _)).WillByDefault(Invoke(getScreenShape));
    ON_CALL(clientScreen, getCursorPos(_, _)).WillByDefault(Invoke(getCursorPos));


    ClientArgs clientArgs;
    clientArgs.m_enableDragDrop = true;
    clientArgs.m_enableCrypto = false;
    Client client(&m_events, "stub", serverAddress, clientSocketFactory, &clientScreen, clientArgs);

    m_events.adoptHandler(
        m_events.forFile().dropDirWriteFinished(), &client,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::sendToClient_mockFile_fileRecieveCompleted,
            &client));

    startTransferCompletionPoll(&server, NULL, false);

    client.connect();

    m_events.initQuitTimeout(30);
    m_events.loop();
    cleanupTransferCompletionPoll();
    EXPECT_EQ(1, m_completedTransfers);
    server.setActive(&primaryClient);
    EXPECT_TRUE(server.testCleanupSendFileThread(false));
    m_events.removeHandler(m_events.forClientListener().connected(), &listener);
    m_events.removeHandler(m_events.forFile().dropDirWriteFinished(), &client);
    m_events.cleanupQuitTimeout();
}

TEST_F(NetworkTests, sendToServer_mockData)
{
    // server and client
    NetworkAddress serverAddress(TEST_HOST, getFreeTestPort());
    serverAddress.resolve();

    // server
    SocketMultiplexer serverSocketMultiplexer;
    TCPSocketFactory* serverSocketFactory = new TCPSocketFactory(&m_events, &serverSocketMultiplexer);
    ClientListener listener(serverAddress, serverSocketFactory, &m_events,
                            ConnectionSecurityLevel::PLAINTEXT);
    NiceMock<MockScreen> serverScreen;
    NiceMock<NetworkPrimaryClient> primaryClient;
    NiceMock<MockConfig> serverConfig;
    NiceMock<MockInputFilter> serverInputFilter;

    ON_CALL(serverConfig, isScreen(_)).WillByDefault(Return(true));
    ON_CALL(serverConfig, getInputFilter()).WillByDefault(Return(&serverInputFilter));

    ServerArgs serverArgs;
    serverArgs.m_enableDragDrop = true;
    Server server(serverConfig, &primaryClient, &serverScreen, &m_events, serverArgs);
    listener.setServer(&server);

    // client
    NiceMock<MockScreen> clientScreen;
    SocketMultiplexer clientSocketMultiplexer;
    TCPSocketFactory* clientSocketFactory = new TCPSocketFactory(&m_events, &clientSocketMultiplexer);

    ON_CALL(clientScreen, getShape(_, _, _, _)).WillByDefault(Invoke(getScreenShape));
    ON_CALL(clientScreen, getCursorPos(_, _)).WillByDefault(Invoke(getCursorPos));

    ClientArgs clientArgs;
    clientArgs.m_enableDragDrop = true;
    clientArgs.m_enableCrypto = false;
    Client client(&m_events, "stub", serverAddress, clientSocketFactory, &clientScreen, clientArgs);
    ClientFileSendContext sendContext = { &listener, &client };

    m_events.adoptHandler(
        m_events.forClientListener().connected(), &listener,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::sendToServer_mockData_handleClientConnected,
            &sendContext));

    m_events.adoptHandler(
        m_events.forFile().dropDirWriteFinished(), &server,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::sendToServer_mockData_fileRecieveCompleted,
            &server));

    startTransferCompletionPoll(NULL, &client, false);

    client.connect();

    m_events.initQuitTimeout(30);
    m_events.loop();
    cleanupTransferCompletionPoll();
    EXPECT_EQ(1, m_completedTransfers);
    server.setActive(&primaryClient);
    m_events.removeHandler(m_events.forClientListener().connected(), &listener);
    m_events.removeHandler(m_events.forFile().dropDirWriteFinished(), &server);
    m_events.cleanupQuitTimeout();
}

TEST_F(NetworkTests, sendToServer_mockFile)
{
    // server and client
    NetworkAddress serverAddress(TEST_HOST, getFreeTestPort());

    serverAddress.resolve();

    // server
    SocketMultiplexer serverSocketMultiplexer;
    TCPSocketFactory* serverSocketFactory = new TCPSocketFactory(&m_events, &serverSocketMultiplexer);
    ClientListener listener(serverAddress, serverSocketFactory, &m_events,
                            ConnectionSecurityLevel::PLAINTEXT);
    NiceMock<MockScreen> serverScreen;
    NiceMock<NetworkPrimaryClient> primaryClient;
    NiceMock<MockConfig> serverConfig;
    NiceMock<MockInputFilter> serverInputFilter;

    ON_CALL(serverConfig, isScreen(_)).WillByDefault(Return(true));
    ON_CALL(serverConfig, getInputFilter()).WillByDefault(Return(&serverInputFilter));

    ServerArgs serverArgs;
    serverArgs.m_enableDragDrop = true;
    Server server(serverConfig, &primaryClient, &serverScreen, &m_events, serverArgs);
    listener.setServer(&server);

    // client
    NiceMock<MockScreen> clientScreen;
    SocketMultiplexer clientSocketMultiplexer;
    TCPSocketFactory* clientSocketFactory = new TCPSocketFactory(&m_events, &clientSocketMultiplexer);

    ON_CALL(clientScreen, getShape(_, _, _, _)).WillByDefault(Invoke(getScreenShape));
    ON_CALL(clientScreen, getCursorPos(_, _)).WillByDefault(Invoke(getCursorPos));

    ClientArgs clientArgs;
    clientArgs.m_enableDragDrop = true;
    clientArgs.m_enableCrypto = false;
    Client client(&m_events, "stub", serverAddress, clientSocketFactory, &clientScreen, clientArgs);
    ClientFileSendContext sendContext = { &listener, &client };

    m_events.adoptHandler(
        m_events.forClientListener().connected(), &listener,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::sendToServer_mockFile_handleClientConnected, &sendContext));

    m_events.adoptHandler(
        m_events.forFile().dropDirWriteFinished(), &server,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::sendToServer_mockFile_fileRecieveCompleted,
            &server));

    startTransferCompletionPoll(NULL, &client, false);

    client.connect();

    m_events.initQuitTimeout(30);
    m_events.loop();
    cleanupTransferCompletionPoll();
    EXPECT_EQ(1, m_completedTransfers);
    server.setActive(&primaryClient);
    EXPECT_TRUE(client.testCleanupSendFileThread(false));
    m_events.removeHandler(m_events.forClientListener().connected(), &listener);
    m_events.removeHandler(m_events.forFile().dropDirWriteFinished(), &server);
    m_events.cleanupQuitTimeout();
}

TEST_F(NetworkTests, repeatedSendToClient_mockDataReusesConnection)
{
    NetworkAddress serverAddress(TEST_HOST, getFreeTestPort());

    serverAddress.resolve();

    SocketMultiplexer serverSocketMultiplexer;
    TCPSocketFactory* serverSocketFactory = new TCPSocketFactory(&m_events, &serverSocketMultiplexer);
    ClientListener listener(serverAddress, serverSocketFactory, &m_events,
                            ConnectionSecurityLevel::PLAINTEXT);
    NiceMock<MockScreen> serverScreen;
    NiceMock<NetworkPrimaryClient> primaryClient;
    NiceMock<MockConfig> serverConfig;
    NiceMock<MockInputFilter> serverInputFilter;

    m_events.adoptHandler(
        m_events.forClientListener().connected(), &listener,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::repeatedSendToClient_mockData_handleClientConnected, &listener));

    ON_CALL(serverConfig, isScreen(_)).WillByDefault(Return(true));
    ON_CALL(serverConfig, getInputFilter()).WillByDefault(Return(&serverInputFilter));

    ServerArgs serverArgs;
    serverArgs.m_enableDragDrop = true;
    Server server(serverConfig, &primaryClient, &serverScreen, &m_events, serverArgs);
    listener.setServer(&server);

    NiceMock<MockScreen> clientScreen;
    SocketMultiplexer clientSocketMultiplexer;
    TCPSocketFactory* clientSocketFactory = new TCPSocketFactory(&m_events, &clientSocketMultiplexer);

    ON_CALL(clientScreen, getShape(_, _, _, _)).WillByDefault(Invoke(getScreenShape));
    ON_CALL(clientScreen, getCursorPos(_, _)).WillByDefault(Invoke(getCursorPos));

    ClientArgs clientArgs;
    clientArgs.m_enableDragDrop = true;
    clientArgs.m_enableCrypto = false;
    Client client(&m_events, "stub", serverAddress, clientSocketFactory, &clientScreen, clientArgs);

    m_events.adoptHandler(
        m_events.forFile().dropDirWriteFinished(), &client,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::repeatedSendToClient_mockData_fileRecieveCompleted,
            &client));

    startTransferCompletionPoll(&server, NULL, true);

    client.connect();

    m_events.initQuitTimeout(30);
    m_events.loop();
    cleanupTransferCompletionPoll();
    EXPECT_EQ(kRepeatedMockDataTransfers, m_completedTransfers);
    m_events.removeHandler(m_events.forClientListener().connected(), &listener);
    m_events.removeHandler(m_events.forFile().dropDirWriteFinished(), &client);
    m_events.cleanupQuitTimeout();
}

void
NetworkTests::startTransferCompletionPoll(Server* serverSender,
                                          Client* clientSender,
                                          bool repeatServerTransfers)
{
    cleanupTransferCompletionPoll();
    m_transferServerSender = serverSender;
    m_transferClientSender = clientSender;
    m_transferReceiveCompleted = false;
    m_repeatServerTransfers = repeatServerTransfers;
    m_transferPollTicks = 0;
    m_completedTransfers = 0;
    m_transferPollTimer = m_events.newTimer(0.01, NULL);
    if (m_transferPollTimer == NULL) {
        ADD_FAILURE() << "could not create transfer completion timer";
        return;
    }
    m_events.adoptHandler(
        Event::kTimer, m_transferPollTimer,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::handleTransferCompletionPoll));
}

void
NetworkTests::cleanupTransferCompletionPoll()
{
    if (m_transferPollTimer != NULL) {
        m_events.removeHandler(Event::kTimer, m_transferPollTimer);
        m_events.deleteTimer(m_transferPollTimer);
        m_transferPollTimer = NULL;
    }
    m_transferServerSender = NULL;
    m_transferClientSender = NULL;
}

void
NetworkTests::markTransferReceiveCompleted(const Event& event, void* receiver)
{
    EXPECT_EQ(receiver, event.getTarget());
    EXPECT_FALSE(m_transferReceiveCompleted);
    m_transferReceiveCompleted = true;
}

void
NetworkTests::handleTransferCompletionPoll(const Event&, void*)
{
    if (!m_transferReceiveCompleted) {
        return;
    }

    std::shared_ptr<barrier::FileTransferSendState> state;
    bool senderRetired = false;
    if (m_transferServerSender != NULL) {
        state = m_transferServerSender->m_sendFileTransactionState;
        senderRetired = state == NULL &&
            m_transferServerSender->m_sendFileThread == NULL &&
            m_transferServerSender->m_sendFileTarget == NULL;
    }
    else if (m_transferClientSender != NULL) {
        state = m_transferClientSender->testSendFileTransactionState();
        senderRetired = state == NULL &&
            !m_transferClientSender->testHasSendFileThread();
    }
    else {
        ADD_FAILURE() << "transfer completion poll has no sender";
        m_events.raiseQuitEvent();
        return;
    }

    if (state && state->stopped() && !state->committed()) {
        ADD_FAILURE() << "transactional sender stopped with reason "
                      << static_cast<int>(state->result());
        m_events.raiseQuitEvent();
        return;
    }
    if (!senderRetired) {
        if (++m_transferPollTicks > 2000) {
            ADD_FAILURE() << "transactional sender was not retired after commit";
            m_events.raiseQuitEvent();
        }
        return;
    }

    ++m_completedTransfers;
    m_transferReceiveCompleted = false;
    m_transferPollTicks = 0;
    if (m_repeatServerTransfers &&
        m_completedTransfers < kRepeatedMockDataTransfers) {
        m_transferServerSender->sendFileToClient(kRepeatedMockFilename);
        return;
    }
    m_events.raiseQuitEvent();
}

void
NetworkTests::sendToClient_mockData_handleClientConnected(const Event&, void* vlistener)
{
    ClientListener* listener = static_cast<ClientListener*>(vlistener);
    Server* server = listener->getServer();

    ClientProxy* client = listener->getNextClient();
    if (client == NULL) {
        throw runtime_error("client is null");
    }

    BaseClientProxy* bcp = client;
    server->adoptClient(bcp);
    server->setActive(bcp);
    server->sendFileToClient(kRepeatedMockFilename);
}

void
NetworkTests::sendToClient_mockData_fileRecieveCompleted(const Event& event, void* receiver)
{
    markTransferReceiveCompleted(event, receiver);
}

void
NetworkTests::repeatedSendToClient_mockData_handleClientConnected(const Event&, void* vlistener)
{
    ClientListener* listener = static_cast<ClientListener*>(vlistener);
    Server* server = listener->getServer();

    ClientProxy* client = listener->getNextClient();
    if (client == NULL) {
        throw runtime_error("client is null");
    }

    BaseClientProxy* bcp = client;
    server->adoptClient(bcp);
    server->setActive(bcp);

    server->sendFileToClient(kRepeatedMockFilename);
}

void
NetworkTests::repeatedSendToClient_mockData_fileRecieveCompleted(
    const Event& event, void* receiver)
{
    markTransferReceiveCompleted(event, receiver);
}

void
NetworkTests::sendToClient_mockFile_handleClientConnected(const Event&, void* vlistener)
{
    ClientListener* listener = static_cast<ClientListener*>(vlistener);
    Server* server = listener->getServer();

    ClientProxy* client = listener->getNextClient();
    if (client == NULL) {
        throw runtime_error("client is null");
    }

    BaseClientProxy* bcp = client;
    server->adoptClient(bcp);
    server->setActive(bcp);

    server->sendFileToClient(kMockFilename);
}

void
NetworkTests::sendToClient_mockFile_fileRecieveCompleted(const Event& event, void* receiver)
{
    markTransferReceiveCompleted(event, receiver);
}

void
NetworkTests::sendToServer_mockData_handleClientConnected(const Event&, void* vclient)
{
    ClientFileSendContext* context = static_cast<ClientFileSendContext*>(vclient);
    ClientProxy* proxy = context->listener->getNextClient();
    if (proxy == NULL) {
        throw runtime_error("client is null");
    }
    context->listener->getServer()->adoptClient(proxy);
    context->client->sendFileToServer(kRepeatedMockFilename);
}

void
NetworkTests::sendToServer_mockData_fileRecieveCompleted(
    const Event& event, void* receiver)
{
    markTransferReceiveCompleted(event, receiver);
}

void
NetworkTests::sendToServer_mockFile_handleClientConnected(const Event&, void* vclient)
{
    ClientFileSendContext* context = static_cast<ClientFileSendContext*>(vclient);
    ClientProxy* proxy = context->listener->getNextClient();
    if (proxy == NULL) {
        throw runtime_error("client is null");
    }
    context->listener->getServer()->adoptClient(proxy);
    context->client->sendFileToServer(kMockFilename);
}

void
NetworkTests::sendToServer_mockFile_fileRecieveCompleted(const Event& event, void* receiver)
{
    markTransferReceiveCompleted(event, receiver);
}

UInt8*
newMockData(size_t size)
{
    UInt8* buffer = new UInt8[size];

    UInt8* data = buffer;
    const UInt8 head[] = "mock head... ";
    size_t headSize = sizeof(head) - 1;
    const UInt8 tail[] = "... mock tail";
    size_t tailSize = sizeof(tail) - 1;
    const UInt8 barrierRocks[] = "barrier\0 rocks! ";
    size_t barrierRocksSize = sizeof(barrierRocks) - 1;

    memcpy(data, head, headSize);
    data += headSize;

    size_t times = (size - headSize - tailSize) / barrierRocksSize;
    for (size_t i = 0; i < times; ++i) {
        memcpy(data, barrierRocks, barrierRocksSize);
        data += barrierRocksSize;
    }

    size_t remainder = (size - headSize - tailSize) % barrierRocksSize;
    if (remainder != 0) {
        memset(data, '.', remainder);
        data += remainder;
    }

    memcpy(data, tail, tailSize);
    return buffer;
}

void
createFile(fstream& file, const char* filename, size_t size)
{
    UInt8* buffer = newMockData(size);

    file.open(filename, ios::out | ios::binary);
    if (!file.is_open()) {
        throw runtime_error("file not open");
    }

    file.write(reinterpret_cast<char*>(buffer), size);
    file.close();

    delete[] buffer;
}

int
getFreeTestPort()
{
#if SYSAPI_WIN32
    static int offset = 0;
    return 24803 + (_getpid() % 20000) + offset++;
#else
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw runtime_error("cannot create port probe socket");
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        throw runtime_error("cannot bind port probe socket");
    }

    socklen_t addrLen = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
        close(fd);
        throw runtime_error("cannot read port probe socket address");
    }

    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
#endif
}

void
getScreenShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h)
{
    x = 0;
    y = 0;
    w = 1;
    h = 1;
}

void
getCursorPos(SInt32& x, SInt32& y)
{
    x = 0;
    y = 0;
}

#endif // WINAPI_CARBON
