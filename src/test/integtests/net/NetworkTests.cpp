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
#include "barrier/FileChunk.h"
#include "barrier/StreamChunker.h"
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

const size_t kMockDataSize = 1024 * 1024 * 10; // 10MB
const UInt16 kMockDataChunkIncrement = 1024; // 1KB
const char* kMockFilename = "NetworkTests.mock";
const size_t kMockFileSize = 1024 * 1024 * 10; // 10MB
const int kRepeatedMockDataTransfers = 8;
const size_t kRepeatedMockDataSize = 1024 * 1024; // 1MB per connection-reuse cycle

int getFreeTestPort();
void getScreenShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h);
void getCursorPos(SInt32& x, SInt32& y);
UInt8* newMockData(size_t size);
void createFile(fstream& file, const char* filename, size_t size);

class NetworkTests : public ::testing::Test
{
public:
    NetworkTests() :
        m_mockData(NULL),
        m_mockDataSize(0),
        m_mockFileSize(0),
        m_repeatedServer(NULL),
        m_repeatedClient(NULL),
        m_repeatedTransferId(0),
        m_repeatedCompleted(0)
    {
        m_mockData = newMockData(kMockDataSize);
        createFile(m_mockFile, kMockFilename, kMockFileSize);
    }

    ~NetworkTests()
    {
        remove(kMockFilename);
        delete[] m_mockData;
    }

    void                sendMockData(void* eventTarget, UInt32 transferId = 0,
                                     size_t dataSize = kMockDataSize);

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
    UInt8*                m_mockData;
    size_t                m_mockDataSize;
    fstream                m_mockFile;
    size_t                m_mockFileSize;
    Server*                m_repeatedServer;
    BaseClientProxy*       m_repeatedClient;
    UInt32                 m_repeatedTransferId;
    int                    m_repeatedCompleted;
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
    NiceMock<MockPrimaryClient> primaryClient;
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
        m_events.forFile().fileRecieveCompleted(), &client,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::sendToClient_mockData_fileRecieveCompleted));

    client.connect();

    m_events.initQuitTimeout(10);
    m_events.loop();
    server.setActive(&primaryClient);
    m_events.removeHandler(m_events.forClientListener().connected(), &listener);
    m_events.removeHandler(m_events.forFile().fileRecieveCompleted(), &client);
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
    NiceMock<MockPrimaryClient> primaryClient;
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
        m_events.forFile().fileRecieveCompleted(), &client,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::sendToClient_mockFile_fileRecieveCompleted));

    client.connect();

    m_events.initQuitTimeout(30);
    m_events.loop();
    server.setActive(&primaryClient);
    EXPECT_TRUE(server.testCleanupSendFileThread(false));
    m_events.removeHandler(m_events.forClientListener().connected(), &listener);
    m_events.removeHandler(m_events.forFile().fileRecieveCompleted(), &client);
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
    NiceMock<MockPrimaryClient> primaryClient;
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

    m_events.adoptHandler(
        m_events.forClientListener().connected(), &listener,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::sendToServer_mockData_handleClientConnected, &client));

    m_events.adoptHandler(
        m_events.forFile().fileRecieveCompleted(), &server,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::sendToServer_mockData_fileRecieveCompleted));

    client.connect();

    m_events.initQuitTimeout(10);
    m_events.loop();
    server.setActive(&primaryClient);
    m_events.removeHandler(m_events.forClientListener().connected(), &listener);
    m_events.removeHandler(m_events.forFile().fileRecieveCompleted(), &server);
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
    NiceMock<MockPrimaryClient> primaryClient;
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

    m_events.adoptHandler(
        m_events.forClientListener().connected(), &listener,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::sendToServer_mockFile_handleClientConnected, &client));

    m_events.adoptHandler(
        m_events.forFile().fileRecieveCompleted(), &server,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::sendToServer_mockFile_fileRecieveCompleted));

    client.connect();

    m_events.initQuitTimeout(30);
    m_events.loop();
    server.setActive(&primaryClient);
    EXPECT_TRUE(client.testCleanupSendFileThread(false));
    m_events.removeHandler(m_events.forClientListener().connected(), &listener);
    m_events.removeHandler(m_events.forFile().fileRecieveCompleted(), &server);
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
    NiceMock<MockPrimaryClient> primaryClient;
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
        m_events.forFile().fileRecieveCompleted(), &client,
        new TMethodEventJob<NetworkTests>(
            this, &NetworkTests::repeatedSendToClient_mockData_fileRecieveCompleted));

    client.connect();

    m_events.initQuitTimeout(30);
    m_events.loop();
    server.setActive(&primaryClient);
    EXPECT_EQ(kRepeatedMockDataTransfers, m_repeatedCompleted);
    m_events.removeHandler(m_events.forClientListener().connected(), &listener);
    m_events.removeHandler(m_events.forFile().fileRecieveCompleted(), &client);
    m_events.cleanupQuitTimeout();
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
    server->setFileTransferForTest(bcp, 1);

    sendMockData(server, 1);
}

void
NetworkTests::sendToClient_mockData_fileRecieveCompleted(const Event& event, void*)
{
    Client* client = static_cast<Client*>(event.getTarget());
    EXPECT_TRUE(client->isReceivedFileSizeValid());

    m_events.raiseQuitEvent();
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

    m_repeatedServer = server;
    m_repeatedClient = bcp;
    m_repeatedTransferId = 1;
    m_repeatedCompleted = 0;

    server->setFileTransferForTest(bcp, m_repeatedTransferId);
    sendMockData(server, m_repeatedTransferId, kRepeatedMockDataSize);
}

void
NetworkTests::repeatedSendToClient_mockData_fileRecieveCompleted(const Event& event, void*)
{
    Client* client = static_cast<Client*>(event.getTarget());
    EXPECT_TRUE(client->isReceivedFileSizeValid());

    ++m_repeatedCompleted;
    if (m_repeatedCompleted >= kRepeatedMockDataTransfers) {
        m_events.raiseQuitEvent();
        return;
    }

    ASSERT_TRUE(m_repeatedServer != NULL);
    ASSERT_TRUE(m_repeatedClient != NULL);
    ++m_repeatedTransferId;
    m_repeatedServer->setFileTransferForTest(m_repeatedClient, m_repeatedTransferId);
    sendMockData(m_repeatedServer, m_repeatedTransferId, kRepeatedMockDataSize);
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
NetworkTests::sendToClient_mockFile_fileRecieveCompleted(const Event& event, void*)
{
    Client* client = static_cast<Client*>(event.getTarget());
    EXPECT_TRUE(client->isReceivedFileSizeValid());

    m_events.raiseQuitEvent();
}

void
NetworkTests::sendToServer_mockData_handleClientConnected(const Event&, void* vclient)
{
    Client* client = static_cast<Client*>(vclient);
    sendMockData(client);
}

void
NetworkTests::sendToServer_mockData_fileRecieveCompleted(const Event& event, void*)
{
    Server* server = static_cast<Server*>(event.getTarget());
    EXPECT_TRUE(server->isReceivedFileSizeValid());

    m_events.raiseQuitEvent();
}

void
NetworkTests::sendToServer_mockFile_handleClientConnected(const Event&, void* vclient)
{
    Client* client = static_cast<Client*>(vclient);
    client->sendFileToServer(kMockFilename);
}

void
NetworkTests::sendToServer_mockFile_fileRecieveCompleted(const Event& event, void*)
{
    Server* server = static_cast<Server*>(event.getTarget());
    EXPECT_TRUE(server->isReceivedFileSizeValid());

    m_events.raiseQuitEvent();
}

void
NetworkTests::sendMockData(void* eventTarget, UInt32 transferId, size_t mockDataSize)
{
    // send first message (file size)
    String size = barrier::string::sizeTypeToString(mockDataSize);
    FileChunk* sizeMessage = FileChunk::start(size);
    sizeMessage->m_transferId = transferId;

    Event sizeEvent(m_events.forFile().fileChunkSending(), eventTarget, sizeMessage);
    sizeEvent.setDataObject(sizeMessage);
    m_events.addEvent(sizeEvent);

    // send chunk messages with incrementing chunk size
    size_t lastSize = 0;
    size_t sentLength = 0;
    while (true) {
        size_t dataSize = lastSize + kMockDataChunkIncrement;

        // make sure we don't read too much from the mock data.
        if (sentLength + dataSize > mockDataSize) {
            dataSize = mockDataSize - sentLength;
        }

        // first byte is the chunk mark, last is \0
        FileChunk* chunk = FileChunk::data(m_mockData, dataSize);
        chunk->m_transferId = transferId;
        Event chunkEvent(m_events.forFile().fileChunkSending(), eventTarget, chunk);
        chunkEvent.setDataObject(chunk);
        m_events.addEvent(chunkEvent);

        sentLength += dataSize;
        lastSize = dataSize;

        if (sentLength == mockDataSize) {
            break;
        }

    }

    // send last message
    FileChunk* transferFinished = FileChunk::end();
    transferFinished->m_transferId = transferId;
    Event finishEvent(m_events.forFile().fileChunkSending(), eventTarget, transferFinished);
    finishEvent.setDataObject(transferFinished);
    m_events.addEvent(finishEvent);
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
