#define BARRIER_TEST_ENV
#include "ipc/IpcClientProxy.h"
#include "ipc/IpcMessage.h"
#include "ipc/IpcServer.h"
#include "ipc/IpcServerProxy.h"

#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/io/MockStream.h"

#include <cstring>
#include <vector>

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::ReturnRef;
using ::testing::Return;

namespace {

Event::Type registerTestEventType(Event::Type& type, const char*)
{
    static Event::Type s_nextType = 100;
    if (type == Event::kUnknown) {
        type = s_nextType++;
    }
    return type;
}

void setupClientProxyEvents(MockEventQueue& events,
                            IStreamEvents& streamEvents,
                            IpcClientProxyEvents& ipcEvents)
{
    streamEvents.setEvents(&events);
    ipcEvents.setEvents(&events);

    ON_CALL(events, forIStream()).WillByDefault(ReturnRef(streamEvents));
    ON_CALL(events, forIpcClientProxy()).WillByDefault(ReturnRef(ipcEvents));
    ON_CALL(events, registerTypeOnce(_, _)).WillByDefault(Invoke(registerTestEventType));
}

void setupServerProxyEvents(MockEventQueue& events,
                            IStreamEvents& streamEvents,
                            IpcServerProxyEvents& ipcEvents)
{
    streamEvents.setEvents(&events);
    ipcEvents.setEvents(&events);

    ON_CALL(events, forIStream()).WillByDefault(ReturnRef(streamEvents));
    ON_CALL(events, forIpcServerProxy()).WillByDefault(ReturnRef(ipcEvents));
    ON_CALL(events, registerTypeOnce(_, _)).WillByDefault(Invoke(registerTestEventType));
}

UInt32 readInvalidHeader(void* buffer, UInt32 size)
{
    EXPECT_EQ(4u, size);
    std::memcpy(buffer, "BAD!", 4);
    return 4;
}

void appendBytes(std::vector<UInt8>& bytes, const char* value, size_t size)
{
    bytes.insert(bytes.end(), value, value + size);
}

void appendUInt32(std::vector<UInt8>& bytes, UInt32 value)
{
    bytes.push_back(static_cast<UInt8>((value >> 24) & 0xff));
    bytes.push_back(static_cast<UInt8>((value >> 16) & 0xff));
    bytes.push_back(static_cast<UInt8>((value >> 8) & 0xff));
    bytes.push_back(static_cast<UInt8>(value & 0xff));
}

std::vector<UInt8> clientCommandFrames(EIpcClientType clientType,
                                       const std::string& command,
                                       UInt8 elevateMode)
{
	std::vector<UInt8> bytes;
	appendBytes(bytes, "IHEL", 4);
	bytes.push_back(static_cast<UInt8>(clientType));
	appendUInt32(bytes, 12345);
	appendBytes(bytes, "ICMD", 4);
	appendUInt32(bytes, static_cast<UInt32>(command.size()));
	appendBytes(bytes, command.data(), command.size());
    bytes.push_back(elevateMode);
    return bytes;
}

UInt32 readFromBuffer(const std::vector<UInt8>& bytes, size_t& offset,
                      void* buffer, UInt32 size)
{
    if (offset >= bytes.size()) {
        return 0;
    }

    EXPECT_LE(offset + size, bytes.size());
    std::memcpy(buffer, bytes.data() + offset, size);
    offset += size;
    return size;
}

}

TEST(IpcProxyTests, clientProxyInvalidHeaderDisconnectsWithoutNullMessageEvent)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(*stream, &events);
    const Event::Type messageType = ipcEvents.messageReceived();
    const Event::Type disconnectedType = ipcEvents.disconnected();

    EXPECT_CALL(*stream, read(_, 4)).WillOnce(Invoke(readInvalidHeader));
    EXPECT_CALL(*stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke([&](const Event& event) {
        EXPECT_EQ(disconnectedType, event.getType());
        EXPECT_NE(messageType, event.getType());
        EXPECT_EQ(&proxy, event.getTarget());
    }));

    proxy.handleData(Event(Event::kUnknown), NULL);
}

TEST(IpcProxyTests, clientProxyDisconnectIsIdempotent)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(*stream, &events);
    const Event::Type disconnectedType = ipcEvents.disconnected();

    EXPECT_CALL(*stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke([&](const Event& event) {
        EXPECT_EQ(disconnectedType, event.getType());
        EXPECT_EQ(&proxy, event.getTarget());
    }));

    proxy.handleDisconnect(Event(Event::kUnknown), NULL);
    proxy.handleWriteError(Event(Event::kUnknown), NULL);
}

TEST(IpcProxyTests, clientProxyRejectsNodeCommand)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(*stream, &events);
    const Event::Type messageType = ipcEvents.messageReceived();
    const Event::Type disconnectedType = ipcEvents.disconnected();
    const std::vector<UInt8> bytes = clientCommandFrames(
        kIpcClientNode, "weaves --example", IpcCommandMessage::kElevateAsNeeded);
    size_t offset = 0;
    int messageEvents = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke([&](const Event& event) {
        if (event.getType() == messageType) {
            ++messageEvents;
            delete event.getDataObject();
        }
        else {
            EXPECT_EQ(disconnectedType, event.getType());
            EXPECT_EQ(&proxy, event.getTarget());
        }
    }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_EQ(1, messageEvents);
}

TEST(IpcProxyTests, clientProxyAllowsGuiCommand)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(*stream, &events);
    const Event::Type messageType = ipcEvents.messageReceived();
    const std::string command = "weaves --example";
    const std::vector<UInt8> bytes = clientCommandFrames(
        kIpcClientGui, command, IpcCommandMessage::kElevateNever);
    size_t offset = 0;
    int messageEvents = 0;
    bool sawCommand = false;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke([&](const Event& event) {
        ASSERT_EQ(messageType, event.getType());
        ++messageEvents;
        IpcMessage* message = static_cast<IpcMessage*>(event.getDataObject());
        if (message->type() == kIpcCommand) {
            IpcCommandMessage* commandMessage = static_cast<IpcCommandMessage*>(message);
            EXPECT_EQ(command, commandMessage->command());
            EXPECT_EQ(IpcCommandMessage::kElevateNever, commandMessage->elevateMode());
            sawCommand = true;
        }
        delete message;
    }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_EQ(2, messageEvents);
    EXPECT_TRUE(sawCommand);
}

TEST(IpcProxyTests, serverSendDoesNotHoldClientListLockWhileWriting)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(*stream, &events);
    proxy.m_clientType = kIpcClientNode;

    IpcServer server;
    server.m_clients.push_back(&proxy);

    EXPECT_CALL(*stream, write(_, _)).WillRepeatedly(Invoke(
        [&](const void*, UInt32) {
            ASSERT_TRUE(server.m_clientsMutex.try_lock());
            server.m_clientsMutex.unlock();
        }));

	IpcLogLineMessage message("test");
	server.send(message, kIpcClientNode);
}

TEST(IpcProxyTests, serverSendToProcessOnlyWritesMatchingNodePid)
{
	NiceMock<MockEventQueue> events;
	IStreamEvents streamEvents;
	IpcClientProxyEvents ipcEvents;
	setupClientProxyEvents(events, streamEvents, ipcEvents);

	NiceMock<MockStream>* oldStream = new NiceMock<MockStream>();
	NiceMock<MockStream>* newStream = new NiceMock<MockStream>();
	ON_CALL(*oldStream, getEventTarget()).WillByDefault(Invoke([oldStream]() { return oldStream; }));
	ON_CALL(*newStream, getEventTarget()).WillByDefault(Invoke([newStream]() { return newStream; }));

	IpcClientProxy oldProxy(*oldStream, &events);
	IpcClientProxy newProxy(*newStream, &events);
	oldProxy.m_clientType = kIpcClientNode;
	oldProxy.m_processId = 1001;
	newProxy.m_clientType = kIpcClientNode;
	newProxy.m_processId = 1002;

	IpcServer server;
	server.m_clients.push_back(&oldProxy);
	server.m_clients.push_back(&newProxy);

	EXPECT_CALL(*oldStream, write(_, _)).Times(1);
	EXPECT_CALL(*newStream, write(_, _)).Times(0);

	IpcShutdownMessage message;
	EXPECT_TRUE(server.sendToProcess(message, kIpcClientNode, 1001));
}

TEST(IpcProxyTests, serverFindsOnlyReadyMatchingProcess)
{
	NiceMock<MockEventQueue> events;
	IStreamEvents streamEvents;
	IpcClientProxyEvents ipcEvents;
	setupClientProxyEvents(events, streamEvents, ipcEvents);

	NiceMock<MockStream>* readyStream = new NiceMock<MockStream>();
	NiceMock<MockStream>* disconnectingStream = new NiceMock<MockStream>();
	ON_CALL(*readyStream, getEventTarget()).WillByDefault(Invoke([readyStream]() { return readyStream; }));
	ON_CALL(*disconnectingStream, getEventTarget()).WillByDefault(
		Invoke([disconnectingStream]() { return disconnectingStream; }));

	IpcClientProxy readyProxy(*readyStream, &events);
	IpcClientProxy disconnectingProxy(*disconnectingStream, &events);
	readyProxy.m_clientType = kIpcClientNode;
	readyProxy.m_processId = 1001;
	disconnectingProxy.m_clientType = kIpcClientNode;
	disconnectingProxy.m_processId = 1002;
	disconnectingProxy.m_disconnecting = true;

	IpcServer server;
	server.m_clients.push_back(&readyProxy);
	server.m_clients.push_back(&disconnectingProxy);

	EXPECT_TRUE(server.hasClientProcess(kIpcClientNode, 1001));
	EXPECT_FALSE(server.hasReadyClientProcess(kIpcClientNode, 1001));
	readyProxy.m_ready = true;
	EXPECT_TRUE(server.hasReadyClientProcess(kIpcClientNode, 1001));
	EXPECT_FALSE(server.hasClientProcess(kIpcClientNode, 1002));
	EXPECT_FALSE(server.hasReadyClientProcess(kIpcClientNode, 1002));
	EXPECT_FALSE(server.hasClientProcess(kIpcClientGui, 1001));
	EXPECT_FALSE(server.hasClientProcess(kIpcClientNode, 0));
}

TEST(IpcProxyTests, clientProxyRequiresNodeReadyAfterHello)
{
	NiceMock<MockEventQueue> events;
	IStreamEvents streamEvents;
	IpcClientProxyEvents ipcEvents;
	setupClientProxyEvents(events, streamEvents, ipcEvents);

	NiceMock<MockStream>* stream = new NiceMock<MockStream>();
	ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

	IpcClientProxy proxy(*stream, &events);
	const Event::Type messageType = ipcEvents.messageReceived();
	std::vector<UInt8> bytes;
	appendBytes(bytes, "IHEL", 4);
	bytes.push_back(static_cast<UInt8>(kIpcClientNode));
	appendUInt32(bytes, 12345);
	appendBytes(bytes, "IRDY", 4);
	size_t offset = 0;
	int messageEvents = 0;
	bool sawReady = false;

	EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
		[&](void* buffer, UInt32 size) {
			return readFromBuffer(bytes, offset, buffer, size);
		}));
	EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke([&](const Event& event) {
		ASSERT_EQ(messageType, event.getType());
		++messageEvents;
		IpcMessage* message = static_cast<IpcMessage*>(event.getDataObject());
		sawReady = sawReady || message->type() == kIpcReady;
		delete message;
	}));

	proxy.handleData(Event(Event::kUnknown), NULL);
	EXPECT_EQ(2, messageEvents);
	EXPECT_TRUE(sawReady);
	EXPECT_TRUE(proxy.m_ready);
}

TEST(IpcProxyTests, clientProxyRejectsReadyBeforeNodeHello)
{
	NiceMock<MockEventQueue> events;
	IStreamEvents streamEvents;
	IpcClientProxyEvents ipcEvents;
	setupClientProxyEvents(events, streamEvents, ipcEvents);

	NiceMock<MockStream>* stream = new NiceMock<MockStream>();
	ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

	IpcClientProxy proxy(*stream, &events);
	const Event::Type messageType = ipcEvents.messageReceived();
	const Event::Type disconnectedType = ipcEvents.disconnected();
	const std::vector<UInt8> bytes = { 'I', 'R', 'D', 'Y' };
	size_t offset = 0;
	int messageEvents = 0;

	EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
		[&](void* buffer, UInt32 size) {
			return readFromBuffer(bytes, offset, buffer, size);
		}));
	EXPECT_CALL(*stream, close()).Times(1);
	EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke([&](const Event& event) {
		if (event.getType() == messageType) {
			++messageEvents;
			delete event.getDataObject();
		}
		else {
			EXPECT_EQ(disconnectedType, event.getType());
		}
	}));

	proxy.handleData(Event(Event::kUnknown), NULL);
	EXPECT_EQ(0, messageEvents);
	EXPECT_FALSE(proxy.m_ready.load());
}

TEST(IpcProxyTests, serverProxyInvalidHeaderDisconnectsWithoutNullMessageEvent)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcServerProxyEvents ipcEvents;
    setupServerProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Invoke([&stream]() { return &stream; }));

    IpcServerProxy proxy(stream, &events);
    const Event::Type messageType = ipcEvents.messageReceived();

    EXPECT_CALL(stream, read(_, 4)).WillOnce(Invoke(readInvalidHeader));
    EXPECT_CALL(stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).Times(0);

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_NE(Event::kUnknown, messageType);
}
