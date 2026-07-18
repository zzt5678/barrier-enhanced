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

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
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

void appendActivatedFrame(std::vector<UInt8>& bytes,
                          UInt32 processId,
                          std::uint64_t activationNonce)
{
    appendBytes(bytes, "IACK", 4);
    appendUInt32(bytes, processId);
    appendUInt64(bytes, activationNonce);
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

std::vector<UInt8> clientStopRequestFrames(EIpcClientType clientType,
                                           std::uint64_t requestId)
{
    std::vector<UInt8> bytes;
    appendBytes(bytes, "IHEL", 4);
    bytes.push_back(static_cast<UInt8>(clientType));
    appendUInt32(bytes, 12345);
    appendBytes(bytes, "ISRP", 4);
    appendUInt64(bytes, requestId);
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

UInt32 readOneAvailableByte(const std::vector<UInt8>& bytes, size_t& offset,
                            bool& byteAvailable, void* buffer, UInt32 size)
{
    if (!byteAvailable || offset >= bytes.size()) {
        return 0;
    }

    EXPECT_GT(size, 0u);
    static_cast<UInt8*>(buffer)[0] = bytes[offset++];
    byteAvailable = false;
    return 1;
}

IpcPeerAuthContext authenticatedTestPeer(
    std::uint32_t processId, EIpcClientType clientType,
    std::uint32_t sessionId = 7,
    const std::string& userSid = "S-1-5-21-1000")
{
    return IpcPeerAuthContext::accepted(
        processId, clientType, sessionId, IpcPeerIntegrityLevel::Medium,
        userSid);
}

}

TEST(IpcProxyTests, peerAuthenticationContextFailsClosedByDefault)
{
    const IpcPeerAuthContext context;

    EXPECT_FALSE(context.permitsConnection());
    EXPECT_FALSE(context.hasKernelIdentity());
    EXPECT_EQ(0u, context.authenticatedSessionId());
    EXPECT_EQ(IpcPeerIntegrityLevel::Unknown, context.integrityLevel());
}

TEST(IpcProxyTests, secureDesktopSystemIdentityIsLimitedToNodeRole)
{
    using IpcPeerAuthenticationPolicy::tokenOwnerAllowed;

    EXPECT_TRUE(tokenOwnerAllowed(kIpcClientGui, true, false));
    EXPECT_FALSE(tokenOwnerAllowed(kIpcClientGui, false, true));
    EXPECT_FALSE(tokenOwnerAllowed(kIpcClientGui, false, false));

    EXPECT_TRUE(tokenOwnerAllowed(kIpcClientNode, true, false));
    EXPECT_TRUE(tokenOwnerAllowed(kIpcClientNode, false, true));
    EXPECT_FALSE(tokenOwnerAllowed(kIpcClientNode, false, false));
    EXPECT_FALSE(tokenOwnerAllowed(kIpcClientUnknown, true, true));
}

TEST(IpcProxyTests, peerAuthenticationRejectsIncompleteOrLowIntegrityIdentity)
{
    EXPECT_FALSE(IpcPeerAuthContext::accepted(
        0, kIpcClientNode, 7,
        IpcPeerIntegrityLevel::Medium,
        "S-1-5-21-1000").permitsConnection());
    EXPECT_FALSE(IpcPeerAuthContext::accepted(
        12345, kIpcClientNode, 0,
        IpcPeerIntegrityLevel::Medium,
        "S-1-5-21-1000").permitsConnection());
    EXPECT_FALSE(IpcPeerAuthContext::accepted(
        12345, kIpcClientNode, 7,
        IpcPeerIntegrityLevel::Low,
        "S-1-5-21-1000").permitsConnection());
    EXPECT_FALSE(IpcPeerAuthContext::accepted(
        12345, kIpcClientNode, 7,
        IpcPeerIntegrityLevel::Medium,
        "").permitsConnection());
}

TEST(IpcProxyTests, peerAuthenticationPreservesVerifiedSessionAndIntegrity)
{
    const IpcPeerAuthContext context = authenticatedTestPeer(
        12345, kIpcClientNode, 9);
    std::string reason;

    EXPECT_TRUE(context.permitsConnection());
    EXPECT_TRUE(context.hasKernelIdentity());
    EXPECT_EQ(9u, context.authenticatedSessionId());
    EXPECT_EQ(IpcPeerIntegrityLevel::Medium, context.integrityLevel());
    EXPECT_TRUE(context.authorizesSession(9, &reason));
    EXPECT_FALSE(context.authorizesSession(10, &reason));
    EXPECT_FALSE(reason.empty());
}

TEST(IpcProxyTests, commandConstructedFromWireFieldsHasNoTrustedOrigin)
{
    IpcCommandMessage command(
        "weaves --origin-pid 999 --origin-session 42 --origin-sid S-1-5-18",
        IpcCommandMessage::kElevateAlways);

    EXPECT_FALSE(command.origin().kernelVerified());
    EXPECT_EQ(0u, command.origin().processId());
    EXPECT_EQ(0u, command.origin().sessionId());
    EXPECT_TRUE(command.origin().userSid().empty());
}

TEST(IpcProxyTests, onlyKernelVerifiedGuiPeerProvidesCommandOrigin)
{
    const IpcPeerAuthContext gui = authenticatedTestPeer(
        12345, kIpcClientGui, 9, "S-1-5-21-2000");
    const IpcPeerAuthContext node = authenticatedTestPeer(
        54321, kIpcClientNode, 10, "S-1-5-21-3000");

    EXPECT_TRUE(gui.commandOrigin().kernelVerified());
    EXPECT_EQ(12345u, gui.commandOrigin().processId());
    EXPECT_EQ(9u, gui.commandOrigin().sessionId());
    EXPECT_EQ("S-1-5-21-2000", gui.commandOrigin().userSid());

    EXPECT_FALSE(node.commandOrigin().kernelVerified());
    EXPECT_EQ(0u, node.commandOrigin().processId());
    EXPECT_EQ(0u, node.commandOrigin().sessionId());
    EXPECT_TRUE(node.commandOrigin().userSid().empty());
}

TEST(IpcProxyTests, authenticatedCommandOriginIsNotSerializedOnWire)
{
    NiceMock<MockEventQueue> sourceEvents;
    IStreamEvents sourceStreamEvents;
    IpcClientProxyEvents sourceIpcEvents;
    setupClientProxyEvents(
        sourceEvents, sourceStreamEvents, sourceIpcEvents);

    NiceMock<MockStream>* sourceStream = new NiceMock<MockStream>();
    ON_CALL(*sourceStream, getEventTarget()).WillByDefault(
        Invoke([sourceStream]() { return sourceStream; }));

    const std::string command =
        "weaves --origin-pid 999 --origin-sid S-1-5-18";
    IpcClientProxy sourceProxy(
        *sourceStream, &sourceEvents,
        authenticatedTestPeer(
            12345, kIpcClientGui, 9, "S-1-5-21-2000"));
    sourceProxy.m_processId = 999;
    IpcCommandMessage* parsed = sourceProxy.parseCommand(
        command, IpcCommandMessage::kElevateNever);
    ASSERT_NE(nullptr, parsed);
    ASSERT_TRUE(parsed->origin().kernelVerified());
    EXPECT_EQ(12345u, parsed->origin().processId());

    NiceMock<MockEventQueue> destinationEvents;
    IStreamEvents destinationStreamEvents;
    IpcServerProxyEvents destinationIpcEvents;
    setupServerProxyEvents(
        destinationEvents, destinationStreamEvents, destinationIpcEvents);
    NiceMock<MockStream> destinationStream;
    ON_CALL(destinationStream, getEventTarget()).WillByDefault(
        Invoke([&destinationStream]() { return &destinationStream; }));

    std::vector<UInt8> expected;
    appendBytes(expected, "ICMD", 4);
    appendUInt32(expected, static_cast<UInt32>(command.size()));
    appendBytes(expected, command.data(), command.size());
    expected.push_back(IpcCommandMessage::kElevateNever);
    EXPECT_CALL(destinationStream,
                write(_, static_cast<UInt32>(expected.size())))
        .WillOnce(Invoke([&](const void* data, UInt32 size) {
            const UInt8* begin = static_cast<const UInt8*>(data);
            EXPECT_EQ(expected, std::vector<UInt8>(begin, begin + size));
        }));

    IpcServerProxy destinationProxy(destinationStream, &destinationEvents);
    destinationProxy.send(*parsed);
    delete parsed;
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

TEST(IpcProxyTests, clientProxySendRefWaitTimeoutTerminates)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            NiceMock<MockEventQueue> events;
            IStreamEvents streamEvents;
            IpcClientProxyEvents ipcEvents;
            setupClientProxyEvents(events, streamEvents, ipcEvents);

            NiceMock<MockStream>* stream = new NiceMock<MockStream>();
            ON_CALL(*stream, getEventTarget()).WillByDefault(
                Invoke([stream]() { return stream; }));

            IpcClientProxy* proxy = new IpcClientProxy(*stream, &events);
            if (!proxy->tryAddSendRef()) {
                std::_Exit(72);
            }
            proxy->waitForSendRefs(0.0, []() { std::_Exit(73); });
            std::_Exit(74);
        },
        ::testing::ExitedWithCode(73),
        "");
}

TEST(IpcProxyTests, clientProxyRejectsNodeCommand)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientNode));
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

    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientGui));
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
            EXPECT_TRUE(commandMessage->origin().kernelVerified());
            EXPECT_EQ(12345u, commandMessage->origin().processId());
            EXPECT_EQ(7u, commandMessage->origin().sessionId());
            EXPECT_EQ("S-1-5-21-1000", commandMessage->origin().userSid());
            sawCommand = true;
        }
        delete message;
    }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_EQ(2, messageEvents);
    EXPECT_TRUE(sawCommand);
}

TEST(IpcProxyTests, clientProxyAllowsAuthenticatedGuiStopRequest)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(
        Invoke([stream]() { return stream; }));
    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientGui));
    const std::vector<UInt8> bytes = clientStopRequestFrames(
        kIpcClientGui, 0x1020304050607080ull);
    size_t offset = 0;
    bool sawStopRequest = false;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke([&](const Event& event) {
        IpcMessage* message = static_cast<IpcMessage*>(event.getDataObject());
        if (message->type() == kIpcStopRequest) {
            IpcStopRequestMessage* stop =
                static_cast<IpcStopRequestMessage*>(message);
            EXPECT_EQ(0x1020304050607080ull, stop->requestId());
            EXPECT_TRUE(stop->origin().kernelVerified());
            EXPECT_EQ(12345u, stop->origin().processId());
            sawStopRequest = true;
        }
        delete message;
    }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_TRUE(sawStopRequest);
}

TEST(IpcProxyTests, clientProxyRejectsNodeStopRequest)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(
        Invoke([stream]() { return stream; }));
    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientNode));
    const Event::Type messageType = ipcEvents.messageReceived();
    const Event::Type disconnectedType = ipcEvents.disconnected();
    const std::vector<UInt8> bytes = clientStopRequestFrames(
        kIpcClientNode, 99);
    size_t offset = 0;
    int messageEvents = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [&](const Event& event) {
            if (event.getType() == messageType) {
                ++messageEvents;
                IpcHelloMessage* hello =
                    static_cast<IpcHelloMessage*>(event.getDataObject());
                EXPECT_EQ(kIpcClientNode, hello->clientType());
                EXPECT_EQ(12345u, hello->processId());
                delete hello;
            }
            else {
                EXPECT_EQ(disconnectedType, event.getType());
                EXPECT_EQ(&proxy, event.getTarget());
            }
        }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_EQ(1, messageEvents);
    EXPECT_TRUE(proxy.m_disconnecting.load());
}

TEST(IpcProxyTests, clientProxySerializesStopAckWithRequestAndGeneration)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(
        Invoke([stream]() { return stream; }));
    IpcClientProxy proxy(*stream, &events);
    std::vector<UInt8> expected;
    appendBytes(expected, "ISAK", 4);
    appendUInt64(expected, 0x1020304050607080ull);
    appendUInt64(expected, 42);
    EXPECT_CALL(*stream, write(_, static_cast<UInt32>(expected.size())))
        .WillOnce(Invoke([&](const void* data, UInt32 size) {
            const UInt8* begin = static_cast<const UInt8*>(data);
            EXPECT_EQ(expected, std::vector<UInt8>(begin, begin + size));
        }));

    IpcStopAckMessage ack(0x1020304050607080ull, 42);
    proxy.send(ack);
}

TEST(IpcProxyTests, clientProxyParsesNodeFramesDeliveredOneByteAtATime)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(
        Invoke([stream]() { return stream; }));
    IpcClientProxy proxy(
        *stream, &events,
        authenticatedTestPeer(12345, kIpcClientNode));

    const std::uint64_t activationNonce = 0x1020304050607080ull;
    proxy.m_activationChallengeNonce = activationNonce;
    std::vector<UInt8> bytes = nodeReadyV2Frames(
        12345, 12345, 7, 42, true, "Default", "test-build", 91);
    appendActivatedFrame(bytes, 12345, activationNonce);
    size_t offset = 0;
    bool byteAvailable = false;
    std::vector<UInt8> messageTypes;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readOneAvailableByte(
                bytes, offset, byteAvailable, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [&](const Event& event) {
            ASSERT_EQ(ipcEvents.messageReceived(), event.getType());
            IpcMessage* message =
                static_cast<IpcMessage*>(event.getDataObject());
            messageTypes.push_back(message->type());
            delete message;
        }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    while (offset < bytes.size()) {
        byteAvailable = true;
        proxy.handleData(Event(Event::kUnknown), NULL);
    }

    const std::vector<UInt8> expectedTypes = {
        kIpcHello, kIpcReadyV2, kIpcActivated
    };
    EXPECT_EQ(expectedTypes, messageTypes);
    EXPECT_TRUE(proxy.m_ready.load());
    EXPECT_TRUE(proxy.matchesActivation(12345, activationNonce));
}

TEST(IpcProxyTests, clientProxyParsesGuiCommandDeliveredOneByteAtATime)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(
        Invoke([stream]() { return stream; }));
    IpcClientProxy proxy(
        *stream, &events,
        authenticatedTestPeer(12345, kIpcClientGui));
    const std::string command = "weaves --example";
    const std::vector<UInt8> bytes = clientCommandFrames(
        kIpcClientGui, command, IpcCommandMessage::kElevateAlways);
    size_t offset = 0;
    bool byteAvailable = false;
    std::vector<UInt8> messageTypes;
    std::string parsedCommand;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readOneAvailableByte(
                bytes, offset, byteAvailable, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [&](const Event& event) {
            ASSERT_EQ(ipcEvents.messageReceived(), event.getType());
            IpcMessage* message =
                static_cast<IpcMessage*>(event.getDataObject());
            messageTypes.push_back(message->type());
            if (message->type() == kIpcCommand) {
                parsedCommand =
                    static_cast<IpcCommandMessage*>(message)->command();
            }
            delete message;
        }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    while (offset < bytes.size()) {
        byteAvailable = true;
        proxy.handleData(Event(Event::kUnknown), NULL);
    }

    const std::vector<UInt8> expectedTypes = { kIpcHello, kIpcCommand };
    EXPECT_EQ(expectedTypes, messageTypes);
    EXPECT_EQ(command, parsedCommand);
}

TEST(IpcProxyTests, authenticatedNodeCannotSpoofGuiAndSendCommand)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(
        *stream, &events,
        authenticatedTestPeer(12345, kIpcClientNode));
    const std::vector<UInt8> bytes = clientCommandFrames(
        kIpcClientGui, "weaves --example", IpcCommandMessage::kElevateAlways);
    size_t offset = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke([&](const Event& event) {
        EXPECT_EQ(ipcEvents.disconnected(), event.getType());
        EXPECT_EQ(&proxy, event.getTarget());
    }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_EQ(kIpcClientUnknown, proxy.m_clientType.load());
    EXPECT_EQ(0u, proxy.m_processId.load());
}

TEST(IpcProxyTests, authenticatedGuiCannotSpoofNodeReadiness)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(
        *stream, &events,
        authenticatedTestPeer(12345, kIpcClientGui));
    const std::vector<UInt8> bytes = nodeReadyV2Frames(
        12345, 12345, 7, 42, true, "Default", "test-build");
    size_t offset = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke([&](const Event& event) {
        EXPECT_EQ(ipcEvents.disconnected(), event.getType());
        EXPECT_EQ(&proxy, event.getTarget());
    }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_FALSE(proxy.m_ready.load());
}

TEST(IpcProxyTests, authenticatedPeerCannotClaimAnotherProcessId)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(
        *stream, &events,
        authenticatedTestPeer(54321, kIpcClientGui));
    const std::vector<UInt8> bytes = clientCommandFrames(
        kIpcClientGui, "weaves --example", IpcCommandMessage::kElevateAlways);
    size_t offset = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke([&](const Event& event) {
        EXPECT_EQ(ipcEvents.disconnected(), event.getType());
        EXPECT_EQ(&proxy, event.getTarget());
    }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_EQ(kIpcClientUnknown, proxy.m_clientType.load());
    EXPECT_EQ(0u, proxy.m_processId.load());
}

TEST(IpcProxyTests, failedPeerAuthenticationCannotFallBackToClaimedHello)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    const IpcPeerAuthContext peerAuth = IpcPeerAuthContext::rejected(
        "kernel TCP owner lookup failed");
    EXPECT_FALSE(peerAuth.permitsConnection());
    IpcClientProxy proxy(*stream, &events, peerAuth);
    const std::vector<UInt8> bytes = clientCommandFrames(
        kIpcClientGui, "weaves --example", IpcCommandMessage::kElevateAlways);
    size_t offset = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke([&](const Event& event) {
        EXPECT_EQ(ipcEvents.disconnected(), event.getType());
        EXPECT_EQ(&proxy, event.getTarget());
    }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_EQ(kIpcClientUnknown, proxy.m_clientType.load());
    EXPECT_EQ(0u, proxy.m_processId.load());
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

TEST(IpcProxyTests, serverSendReleasesEveryRecipientWhenFirstWriteThrows)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* firstStream = new NiceMock<MockStream>();
    NiceMock<MockStream>* secondStream = new NiceMock<MockStream>();
    NiceMock<MockStream>* thirdStream = new NiceMock<MockStream>();
    ON_CALL(*firstStream, getEventTarget()).WillByDefault(
        Invoke([firstStream]() { return firstStream; }));
    ON_CALL(*secondStream, getEventTarget()).WillByDefault(
        Invoke([secondStream]() { return secondStream; }));
    ON_CALL(*thirdStream, getEventTarget()).WillByDefault(
        Invoke([thirdStream]() { return thirdStream; }));

    IpcClientProxy first(*firstStream, &events);
    IpcClientProxy second(*secondStream, &events);
    IpcClientProxy third(*thirdStream, &events);
    first.m_clientType = kIpcClientNode;
    second.m_clientType = kIpcClientNode;
    third.m_clientType = kIpcClientNode;

    IpcServer server;
    server.m_clients.push_back(&first);
    server.m_clients.push_back(&second);
    server.m_clients.push_back(&third);

    EXPECT_CALL(*firstStream, write(_, _)).WillOnce(Invoke(
        [](const void*, UInt32) { throw std::runtime_error("write failed"); }));
    EXPECT_CALL(*secondStream, write(_, _)).Times(0);
    EXPECT_CALL(*thirdStream, write(_, _)).Times(0);

    IpcShutdownMessage message;
    EXPECT_THROW(server.send(message, kIpcClientNode), std::runtime_error);

    const UInt32 firstRefs = first.m_sendRefCount;
    const UInt32 secondRefs = second.m_sendRefCount;
    const UInt32 thirdRefs = third.m_sendRefCount;
    while (first.m_sendRefCount != 0) {
        first.releaseSendRef();
    }
    while (second.m_sendRefCount != 0) {
        second.releaseSendRef();
    }
    while (third.m_sendRefCount != 0) {
        third.releaseSendRef();
    }

    EXPECT_EQ(0u, firstRefs);
    EXPECT_EQ(0u, secondRefs);
    EXPECT_EQ(0u, thirdRefs);
}

TEST(IpcProxyTests, serverSendToProcessReleasesEveryRecipientWhenFirstWriteThrows)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* firstStream = new NiceMock<MockStream>();
    NiceMock<MockStream>* secondStream = new NiceMock<MockStream>();
    NiceMock<MockStream>* thirdStream = new NiceMock<MockStream>();
    ON_CALL(*firstStream, getEventTarget()).WillByDefault(
        Invoke([firstStream]() { return firstStream; }));
    ON_CALL(*secondStream, getEventTarget()).WillByDefault(
        Invoke([secondStream]() { return secondStream; }));
    ON_CALL(*thirdStream, getEventTarget()).WillByDefault(
        Invoke([thirdStream]() { return thirdStream; }));

    IpcClientProxy first(*firstStream, &events);
    IpcClientProxy second(*secondStream, &events);
    IpcClientProxy third(*thirdStream, &events);
    first.m_clientType = kIpcClientNode;
    second.m_clientType = kIpcClientNode;
    third.m_clientType = kIpcClientNode;
    first.m_processId = 1001;
    second.m_processId = 1001;
    third.m_processId = 1001;

    IpcServer server;
    server.m_clients.push_back(&first);
    server.m_clients.push_back(&second);
    server.m_clients.push_back(&third);

    EXPECT_CALL(*firstStream, write(_, _)).WillOnce(Invoke(
        [](const void*, UInt32) { throw std::runtime_error("write failed"); }));
    EXPECT_CALL(*secondStream, write(_, _)).Times(0);
    EXPECT_CALL(*thirdStream, write(_, _)).Times(0);

    IpcShutdownMessage message;
    EXPECT_THROW(server.sendToProcess(message, kIpcClientNode, 1001),
                 std::runtime_error);

    const UInt32 firstRefs = first.m_sendRefCount;
    const UInt32 secondRefs = second.m_sendRefCount;
    const UInt32 thirdRefs = third.m_sendRefCount;
    while (first.m_sendRefCount != 0) {
        first.releaseSendRef();
    }
    while (second.m_sendRefCount != 0) {
        second.releaseSendRef();
    }
    while (third.m_sendRefCount != 0) {
        third.releaseSendRef();
    }

    EXPECT_EQ(0u, firstRefs);
    EXPECT_EQ(0u, secondRefs);
    EXPECT_EQ(0u, thirdRefs);
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

TEST(IpcProxyTests, activationAckRequiresExactTargetedChallenge)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(
        Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(
        *stream, &events,
        authenticatedTestPeer(12345, kIpcClientNode));
    proxy.m_clientType = kIpcClientNode;
    proxy.m_processId = 12345;
    IpcServer server;
    server.m_clients.push_back(&proxy);

    const std::uint64_t activationNonce = 0x1122334455667788ull;
    std::vector<UInt8> expectedChallenge;
    appendBytes(expectedChallenge, "IACT", 4);
    appendUInt64(expectedChallenge, activationNonce);
    EXPECT_CALL(*stream, write(_, static_cast<UInt32>(expectedChallenge.size())))
        .WillOnce(Invoke([&](const void* data, UInt32 size) {
            const UInt8* begin = static_cast<const UInt8*>(data);
            EXPECT_EQ(expectedChallenge,
                      std::vector<UInt8>(begin, begin + size));
        }));
    ASSERT_TRUE(server.sendActivateToProcess(12345, activationNonce));

    std::vector<UInt8> frames;
    appendActivatedFrame(frames, 12345, activationNonce);
    size_t offset = 0;
    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(frames, offset, buffer, size);
        }));
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [&](const Event& event) {
            IpcMessage* message =
                static_cast<IpcMessage*>(event.getDataObject());
            delete message;
        }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_TRUE(server.hasActivatedClientProcess(12345, activationNonce));
    EXPECT_FALSE(server.hasActivatedClientProcess(12345,
                                                   activationNonce + 1));
    EXPECT_FALSE(server.hasActivatedClientProcess(54321, activationNonce));
}

TEST(IpcProxyTests, unchallengedActivationAckDisconnects)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(
        Invoke([stream]() { return stream; }));
    IpcClientProxy proxy(
        *stream, &events,
        authenticatedTestPeer(12345, kIpcClientNode));

    std::vector<UInt8> frames;
    appendBytes(frames, "IHEL", 4);
    frames.push_back(static_cast<UInt8>(kIpcClientNode));
    appendUInt32(frames, 12345);
    appendActivatedFrame(frames, 12345, 99);
    size_t offset = 0;
    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(frames, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [&](const Event& event) {
            if (event.getType() == ipcEvents.messageReceived()) {
                delete static_cast<IpcMessage*>(event.getDataObject());
            }
        }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_TRUE(proxy.m_disconnecting.load());
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

	IpcClientProxy proxy(
		*stream, &events, authenticatedTestPeer(12345, kIpcClientNode));
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

    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientNode));
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

TEST(IpcProxyTests, capabilityReadyMustMatchAuthenticatedTokenSession)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(
        Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(
        *stream, &events,
        authenticatedTestPeer(12345, kIpcClientNode, 7));
    const std::vector<UInt8> bytes = nodeReadyV2Frames(
        12345, 12345, 8, 42, true, "Default", "test-build");
    const Event::Type messageType = ipcEvents.messageReceived();
    const Event::Type disconnectedType = ipcEvents.disconnected();
    size_t offset = 0;
    int messageEvents = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [&](const Event& event) {
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
    EXPECT_TRUE(proxy.m_disconnecting.load());
}

TEST(IpcProxyTests, capabilityProofMustMatchWatchdogQueryNonce)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Invoke([stream]() { return stream; }));

    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientNode));
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

    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientNode));
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

    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientNode));
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

    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientNode));
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

    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientNode));
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

    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientNode));
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

    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientNode));
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

    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientNode));
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

    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientNode));
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

TEST(IpcProxyTests, oversizedCommandStringDisconnectsBeforePayloadAllocation)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcClientProxyEvents ipcEvents;
    setupClientProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(
        Invoke([stream]() { return stream; }));
    IpcClientProxy proxy(
        *stream, &events,
        authenticatedTestPeer(12345, kIpcClientGui));
    std::vector<UInt8> bytes;
    appendBytes(bytes, "IHEL", 4);
    bytes.push_back(static_cast<UInt8>(kIpcClientGui));
    appendUInt32(bytes, 12345);
    appendBytes(bytes, "ICMD", 4);
    appendUInt32(bytes, PROTOCOL_MAX_STRING_LENGTH + 1);
    size_t offset = 0;
    int messageEvents = 0;

    EXPECT_CALL(*stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(*stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [&](const Event& event) {
            if (event.getType() == ipcEvents.messageReceived()) {
                ++messageEvents;
                delete event.getDataObject();
            }
        }));

    proxy.handleData(Event(Event::kUnknown), NULL);
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

    IpcClientProxy proxy(
        *stream, &events, authenticatedTestPeer(12345, kIpcClientNode));
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

TEST(IpcProxyTests, oversizedLogStringDisconnectsBeforePayloadAllocation)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcServerProxyEvents ipcEvents;
    setupServerProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(
        Invoke([&stream]() { return &stream; }));
    IpcServerProxy proxy(stream, &events);
    std::vector<UInt8> bytes;
    appendBytes(bytes, "ILOG", 4);
    appendUInt32(bytes, PROTOCOL_MAX_STRING_LENGTH + 1);
    size_t offset = 0;

    EXPECT_CALL(stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).Times(0);

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_TRUE(proxy.m_disconnected);
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

TEST(IpcProxyTests, serverProxyParsesTargetedActivation)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcServerProxyEvents ipcEvents;
    setupServerProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(
        Invoke([&stream]() { return &stream; }));
    IpcServerProxy proxy(stream, &events);
    const Event::Type messageType = ipcEvents.messageReceived();
    std::vector<UInt8> bytes;
    appendBytes(bytes, "IACT", 4);
    appendUInt64(bytes, 0x1020304050607080ull);
    size_t offset = 0;
    EXPECT_CALL(stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readFromBuffer(bytes, offset, buffer, size);
        }));
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke([&](const Event& event) {
        EXPECT_EQ(messageType, event.getType());
        IpcActivateNodeMessage* activate =
            static_cast<IpcActivateNodeMessage*>(event.getDataObject());
        EXPECT_EQ(kIpcActivate, activate->type());
        EXPECT_EQ(0x1020304050607080ull, activate->activationNonce());
        delete activate;
    }));

    proxy.handleData(Event(Event::kUnknown), NULL);
}

TEST(IpcProxyTests, serverProxyParsesFramesDeliveredOneByteAtATime)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcServerProxyEvents ipcEvents;
    setupServerProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(
        Invoke([&stream]() { return &stream; }));
    IpcServerProxy proxy(stream, &events);
    std::vector<UInt8> bytes;
    appendBytes(bytes, "ILOG", 4);
    appendString(bytes, "one-byte log");
    appendBytes(bytes, "ISDN", 4);
    appendBytes(bytes, "IRQP", 4);
    appendUInt64(bytes, 91);
    appendBytes(bytes, "IACT", 4);
    appendUInt64(bytes, 0x1020304050607080ull);
    size_t offset = 0;
    bool byteAvailable = false;
    std::vector<UInt8> messageTypes;

    EXPECT_CALL(stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            return readOneAvailableByte(
                bytes, offset, byteAvailable, buffer, size);
        }));
    EXPECT_CALL(stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillRepeatedly(Invoke(
        [&](const Event& event) {
            ASSERT_EQ(ipcEvents.messageReceived(), event.getType());
            IpcMessage* message =
                static_cast<IpcMessage*>(event.getDataObject());
            messageTypes.push_back(message->type());
            delete message;
        }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    while (offset < bytes.size()) {
        byteAvailable = true;
        proxy.handleData(Event(Event::kUnknown), NULL);
    }

    const std::vector<UInt8> expectedTypes = {
        kIpcLogLine, kIpcShutdown, kIpcReadyQuery, kIpcActivate
    };
    EXPECT_EQ(expectedTypes, messageTypes);
}

TEST(IpcProxyTests, serverProxySerializesActivationAck)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    IpcServerProxyEvents ipcEvents;
    setupServerProxyEvents(events, streamEvents, ipcEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(
        Invoke([&stream]() { return &stream; }));
    std::vector<UInt8> expected;
    appendBytes(expected, "IACK", 4);
    appendUInt32(expected, 12345);
    appendUInt64(expected, 0x1020304050607080ull);
    EXPECT_CALL(stream, write(_, static_cast<UInt32>(expected.size())))
        .WillOnce(Invoke([&](const void* data, UInt32 size) {
            const UInt8* begin = static_cast<const UInt8*>(data);
            EXPECT_EQ(expected, std::vector<UInt8>(begin, begin + size));
        }));

    IpcServerProxy proxy(stream, &events);
    IpcNodeActivatedMessage activated(
        12345, 0x1020304050607080ull);
    proxy.send(activated);
}

TEST(IpcProxyTests, partialInputReadinessQueryWaitsForRemainingBytes)
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
    appendUInt64(bytes, 91);
    size_t offset = 0;
    size_t availableBytes = 8;
    bool sawQuery = false;

    EXPECT_CALL(stream, read(_, _)).WillRepeatedly(Invoke(
        [&](void* buffer, UInt32 size) {
            const size_t available = availableBytes - offset;
            const UInt32 count = static_cast<UInt32>(
                std::min<std::size_t>(size, available));
            if (count != 0) {
                std::memcpy(buffer, bytes.data() + offset, count);
                offset += count;
            }
            return count;
        }));
    EXPECT_CALL(stream, close()).Times(0);
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke([&](const Event& event) {
        IpcInputReadyQueryMessage* query =
            static_cast<IpcInputReadyQueryMessage*>(event.getDataObject());
        EXPECT_EQ(91u, query->queryNonce());
        sawQuery = true;
        delete query;
    }));

    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_FALSE(sawQuery);

    availableBytes = bytes.size();
    proxy.handleData(Event(Event::kUnknown), NULL);
    EXPECT_TRUE(sawQuery);
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
