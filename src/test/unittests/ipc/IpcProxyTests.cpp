#define BARRIER_TEST_ENV
#include "ipc/IpcClientProxy.h"
#include "ipc/IpcMessage.h"
#include "ipc/IpcServer.h"
#include "ipc/IpcServerProxy.h"
#include "barrier/protocol_types.h"

#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/io/MockStream.h"

#include <chrono>
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

void appendUInt64(std::vector<UInt8>& bytes, std::uint64_t value)
{
    appendUInt32(bytes, static_cast<UInt32>(value >> 32));
    appendUInt32(bytes, static_cast<UInt32>(value & 0xffffffffu));
}

void appendString(std::vector<UInt8>& bytes, const std::string& value)
{
    appendUInt32(bytes, static_cast<UInt32>(value.size()));
    appendBytes(bytes, value.data(), value.size());
}

void appendReadyV2Frame(std::vector<UInt8>& bytes,
                        UInt32 readyProcessId,
                        UInt32 sessionId,
                        std::uint64_t inputGeneration,
                        bool inputReady,
                        const std::string& desktopName,
                        const std::string& buildId,
                        std::uint64_t queryNonce = 0)
{
    appendBytes(bytes, "IRV2", 4);
    appendUInt32(bytes, readyProcessId);
    appendUInt32(bytes, sessionId);
    appendUInt64(bytes, inputGeneration);
    bytes.push_back(inputReady ? 1 : 0);
    appendString(bytes, desktopName);
    appendString(bytes, buildId);
    appendUInt64(bytes, queryNonce);
}

std::vector<UInt8> nodeReadyV2Frames(UInt32 helloProcessId,
                                     UInt32 readyProcessId,
                                     UInt32 sessionId,
                                     std::uint64_t inputGeneration,
                                     bool inputReady,
                                     const std::string& desktopName,
                                     const std::string& buildId,
                                     std::uint64_t queryNonce = 0)
{
    std::vector<UInt8> bytes;
    appendBytes(bytes, "IHEL", 4);
    bytes.push_back(static_cast<UInt8>(kIpcClientNode));
    appendUInt32(bytes, helloProcessId);
    appendReadyV2Frame(bytes, readyProcessId, sessionId, inputGeneration,
                       inputReady, desktopName, buildId, queryNonce);
    return bytes;
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

TEST(IpcProxyTests, capabilityReadyMustMatchProcessSessionAndDesktop)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(*stream, &events);
    IpcServer server;
    server.m_clients.push_back(&proxy);
    const std::vector<UInt8> bytes = nodeReadyV2Frames(
        12345, 12345, 7, 42, true, "Default", "test-build");
    size_t offset = 0;
    int messageEvents = 0;
    bool sawCapabilityReady = false;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke([&](const Event& event) {
        ++messageEvents;
        IpcMessage* message = static_cast<IpcMessage*>(event.getDataObject());
        if (message->type() == kIpcReadyV2) {
            IpcNodeReadyV2Message* ready =
                static_cast<IpcNodeReadyV2Message*>(message);
            EXPECT_EQ(12345u, ready->processId());
            EXPECT_EQ(7u, ready->sessionId());
            EXPECT_EQ(42u, ready->inputGeneration());
            EXPECT_TRUE(ready->inputReady());
            EXPECT_EQ("Default", ready->desktopName());
            EXPECT_EQ("test-build", ready->buildId());
            sawCapabilityReady = true;
        }
        delete message;
    }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_EQ(2, messageEvents);
    EXPECT_TRUE(sawCapabilityReady);
    EXPECT_TRUE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Default", "test-build"));
    EXPECT_FALSE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 8, "Default", "test-build"));
    EXPECT_FALSE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Winlogon", "test-build"));
    EXPECT_FALSE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Default", "other-build"));

    std::string reportedDesktop;
    EXPECT_TRUE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Winlogon", "test-build",
        0, false, &reportedDesktop));
    EXPECT_EQ("Default", reportedDesktop);
}

TEST(IpcProxyTests, capabilityProofMustMatchWatchdogQueryNonce)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(*stream, &events);
    IpcServer server;
    server.m_clients.push_back(&proxy);
    const std::vector<UInt8> bytes = nodeReadyV2Frames(
        12345, 12345, 7, 42, true, "Default", "test-build", 77);
    size_t offset = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [](const Event& event) { delete event.getDataObject(); }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_FALSE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Default", "test-build", 76));
    EXPECT_TRUE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Default", "test-build", 77));
}

TEST(IpcProxyTests, periodicInvalidationRetiresWatchdogQueryProof)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(*stream, &events);
    IpcServer server;
    server.m_clients.push_back(&proxy);
    std::vector<UInt8> bytes = nodeReadyV2Frames(
        12345, 12345, 7, 42, true, "Default", "test-build", 77);
    appendReadyV2Frame(
        bytes, 12345, 7, 43, false, "Default", "test-build");
    size_t offset = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [](const Event& event) { delete event.getDataObject(); }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_FALSE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Default", "test-build"));
    EXPECT_FALSE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Default", "test-build", 77));
}

TEST(IpcProxyTests, matchingPeriodicLeasePreservesWatchdogQueryProof)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(*stream, &events);
    IpcServer server;
    server.m_clients.push_back(&proxy);
    std::vector<UInt8> bytes = nodeReadyV2Frames(
        12345, 12345, 7, 42, true, "Default", "test-build", 77);
    appendReadyV2Frame(
        bytes, 12345, 7, 42, true, "Default", "test-build");
    size_t offset = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [](const Event& event) { delete event.getDataObject(); }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_TRUE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Default", "test-build"));
    EXPECT_TRUE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Default", "test-build", 77));
}

TEST(IpcProxyTests, capabilityReadyFalseCannotBeAdopted)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(*stream, &events);
    IpcServer server;
    server.m_clients.push_back(&proxy);
    const std::vector<UInt8> bytes = nodeReadyV2Frames(
        12345, 12345, 7, 42, false, "Default", "test-build");
    size_t offset = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [](const Event& event) { delete event.getDataObject(); }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_TRUE(proxy.m_ready.load());
    EXPECT_FALSE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Default", "test-build"));
}

TEST(IpcProxyTests, capabilityReadinessUsesLatestRecoveryUpdate)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(*stream, &events);
    IpcServer server;
    server.m_clients.push_back(&proxy);
    std::vector<UInt8> bytes = nodeReadyV2Frames(
        12345, 12345, 7, 0, false, "Default", "test-build");
    appendReadyV2Frame(
        bytes, 12345, 7, 43, true, "Default", "test-build");
    size_t offset = 0;
    int readinessUpdates = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke([&](const Event& event) {
        IpcMessage* message = static_cast<IpcMessage*>(event.getDataObject());
        if (message->type() == kIpcReadyV2) {
            ++readinessUpdates;
            EXPECT_EQ(readinessUpdates == 2,
                server.hasInputReadyClientProcess(
                    kIpcClientNode, 12345, 7, "Default", "test-build"));
        }
        delete message;
    }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_EQ(2, readinessUpdates);
    EXPECT_TRUE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Default", "test-build"));
}

TEST(IpcProxyTests, capabilityReadinessUsesLatestInvalidationUpdate)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(*stream, &events);
    IpcServer server;
    server.m_clients.push_back(&proxy);
    std::vector<UInt8> bytes = nodeReadyV2Frames(
        12345, 12345, 7, 42, true, "Default", "test-build");
    appendReadyV2Frame(
        bytes, 12345, 7, 43, false, "Default", "test-build");
    size_t offset = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [](const Event& event) { delete event.getDataObject(); }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_FALSE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Default", "test-build"));
}

TEST(IpcProxyTests, expiredCapabilityReadinessCannotBeAdopted)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(*stream, &events);
    IpcServer server;
    server.m_clients.push_back(&proxy);
    const std::vector<UInt8> bytes = nodeReadyV2Frames(
        12345, 12345, 7, 42, true, "Default", "test-build");
    size_t offset = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [](const Event& event) { delete event.getDataObject(); }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    proxy.m_readyReceivedAt = std::chrono::steady_clock::now() -
        std::chrono::seconds(10);
    EXPECT_FALSE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Default", "test-build"));
}

TEST(IpcProxyTests, expiredCapabilityProofCannotBeAdopted)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(*stream, &events);
    IpcServer server;
    server.m_clients.push_back(&proxy);
    const std::vector<UInt8> bytes = nodeReadyV2Frames(
        12345, 12345, 7, 42, true, "Default", "test-build", 77);
    size_t offset = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [](const Event& event) { delete event.getDataObject(); }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    proxy.m_proofReceivedAt = std::chrono::steady_clock::now() -
        std::chrono::seconds(10);
    EXPECT_FALSE(server.hasInputReadyClientProcess(
        kIpcClientNode, 12345, 7, "Default", "test-build", 77));
}

TEST(IpcProxyTests, oversizedCapabilityStringDisconnectsWithoutThrowing)
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
    std::vector<UInt8> bytes;
    appendBytes(bytes, "IHEL", 4);
    bytes.push_back(static_cast<UInt8>(kIpcClientNode));
    appendUInt32(bytes, 12345);
    appendBytes(bytes, "IRV2", 4);
    appendUInt32(bytes, 12345);
    appendUInt32(bytes, 7);
    appendUInt64(bytes, 42);
    bytes.push_back(1);
    appendUInt32(bytes, PROTOCOL_MAX_STRING_LENGTH + 1);
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

    EXPECT_NO_THROW(proxy.handleData(Event(Event::kUnknown), NULL));
    EXPECT_EQ(1, messageEvents);
    EXPECT_TRUE(proxy.m_disconnecting.load());
}

TEST(IpcProxyTests, capabilityReadyWithDifferentProcessIdIsRejected)
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
    const std::vector<UInt8> bytes = nodeReadyV2Frames(
        12345, 54321, 7, 42, true, "Default", "test-build");
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
    EXPECT_EQ(1, messageEvents);
    EXPECT_FALSE(proxy.m_ready.load());
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

TEST(IpcProxyTests, serverProxySerializesCapabilityReadyAsOneMessage)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcServerProxyEvents ipcEvents;
    setupServerProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Invoke([&stream]() { return &stream; }));

    std::vector<UInt8> expected;
    appendBytes(expected, "IRV2", 4);
    appendUInt32(expected, 12345);
    appendUInt32(expected, 7);
    appendUInt64(expected, 42);
    expected.push_back(1);
    appendString(expected, "Default");
    appendString(expected, "test-build");
    appendUInt64(expected, 0);

    EXPECT_CALL(stream, write(_, static_cast<UInt32>(expected.size())))
        .WillOnce(Invoke([&](const void* data, UInt32 size) {
            const UInt8* begin = static_cast<const UInt8*>(data);
            EXPECT_EQ(expected, std::vector<UInt8>(begin, begin + size));
        }));

    IpcServerProxy proxy(stream, &events);
    IpcNodeReadyV2Message ready(
        12345, 7, 42, true, "Default", "test-build");
    proxy.send(ready);
}

TEST(IpcProxyTests, serverProxyParsesInputReadinessQuery)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcServerProxyEvents ipcEvents;
    setupServerProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Invoke([&stream]() { return &stream; }));
    IpcServerProxy proxy(stream, &events);
    const Event::Type messageType = ipcEvents.messageReceived();
    std::vector<UInt8> bytes;
    appendBytes(bytes, "IRQP", 4);
    appendUInt64(bytes, 91);
    size_t offset = 0;
    bool sawQuery = false;

    EXPECT_CALL(stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke([&](const Event& event) {
        EXPECT_EQ(messageType, event.getType());
        IpcInputReadyQueryMessage* query =
            static_cast<IpcInputReadyQueryMessage*>(event.getDataObject());
        EXPECT_EQ(kIpcReadyQuery, query->type());
        EXPECT_EQ(91u, query->queryNonce());
        sawQuery = true;
        delete query;
    }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_TRUE(sawQuery);
}

TEST(IpcProxyTests, truncatedInputReadinessQueryDisconnectsWithoutEvent)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcServerProxyEvents ipcEvents;
    setupServerProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Invoke([&stream]() { return &stream; }));
    IpcServerProxy proxy(stream, &events);
    std::vector<UInt8> bytes;
    appendBytes(bytes, "IRQP", 4);
    appendUInt32(bytes, 0);
    size_t offset = 0;

    EXPECT_CALL(stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).Times(0);

    proxy.handleData(Event(Event::kUnknown), NULL);
}

TEST(IpcProxyTests, clientProxySerializesInputReadinessQuery)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));
    std::vector<UInt8> expected;
    appendBytes(expected, "IRQP", 4);
    appendUInt64(expected, 91);
    EXPECT_CALL(*stream, write(_, static_cast<UInt32>(expected.size())))
        .WillOnce(Invoke([&](const void* data, UInt32 size) {
            const UInt8* begin = static_cast<const UInt8*>(data);
            EXPECT_EQ(expected, std::vector<UInt8>(begin, begin + size));
        }));

    IpcClientProxy proxy(*stream, &events);
    IpcInputReadyQueryMessage query(91);
    proxy.send(query);
}
