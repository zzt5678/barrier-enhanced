#define BARRIER_TEST_ENV

#include "barrier/BulkChannel.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "server/ClientProxyUnknown.h"
#include "io/IStream.h"
#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/server/MockServer.h"

#include <cstring>
#include <string>
#include <vector>

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {

class DuplexMemoryStream : public barrier::IStream {
public:
    void queueInput(const std::vector<UInt8>& bytes)
    {
        input.insert(input.end(), bytes.begin(), bytes.end());
    }

    void close() override { closed = true; }

    UInt32 read(void* buffer, UInt32 count) override
    {
        const UInt32 available = getSize();
        const UInt32 copied = count < available ? count : available;
        if (copied != 0 && buffer != NULL) {
            std::memcpy(buffer, input.data() + inputOffset, copied);
        }
        inputOffset += copied;
        return copied;
    }

    void write(const void* buffer, UInt32 count) override
    {
        const UInt8* bytes = static_cast<const UInt8*>(buffer);
        output.insert(output.end(), bytes, bytes + count);
    }

    void writeLowPriority(const void* buffer, UInt32 count) override
    {
        write(buffer, count);
    }

    void flush() override { }
    void shutdownInput() override { }
    void shutdownOutput() override { }
    void* getEventTarget() const override
    {
        return const_cast<DuplexMemoryStream*>(this);
    }
    bool isReady() const override { return getSize() != 0; }
    UInt32 getSize() const override
    {
        return static_cast<UInt32>(input.size() - inputOffset);
    }
    UInt32 getBufferedOutputSize() const override { return 0; }

    bool closed = false;
    std::vector<UInt8> output;

private:
    std::vector<UInt8> input;
    std::size_t inputOffset = 0;
};

std::vector<UInt8> encodeBulkHello(const std::string& name,
                                   const std::string& token)
{
    DuplexMemoryStream stream;
    ProtocolUtil::writef(&stream, kMsgHelloBulkBack,
                         kProtocolMajorVersion, kProtocolMinorVersion,
                         &name, &token);
    return stream.output;
}

std::vector<UInt8> encodeMessage(const char* message)
{
    return std::vector<UInt8>(message, message + 4);
}

void setEventDefaults(MockEventQueue& events,
                      IStreamEvents& streamEvents,
                      ClientProxyUnknownEvents& unknownEvents,
                      ClientProxyEvents& proxyEvents)
{
    streamEvents.setEvents(&events);
    unknownEvents.setEvents(&events);
    proxyEvents.setEvents(&events);
    ON_CALL(events, forIStream()).WillByDefault(ReturnRef(streamEvents));
    ON_CALL(events, forClientProxyUnknown()).WillByDefault(ReturnRef(unknownEvents));
    ON_CALL(events, forClientProxy()).WillByDefault(ReturnRef(proxyEvents));
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));
    ON_CALL(events, newTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(2)));
}

class RecordingBulkHandler : public barrier::IBulkChannelHandler {
public:
    bool handleBulkMessage(const UInt8* code, barrier::IStream* stream) override
    {
        ++messageCount;
        lastCode.assign(reinterpret_cast<const char*>(code), 4);
        stream->read(NULL, stream->getSize());
        return true;
    }

    void handleBulkDisconnected(barrier::BulkChannel*) override
    {
        ++disconnectCount;
    }

    int messageCount = 0;
    int disconnectCount = 0;
    std::string lastCode;
};

}

TEST(BulkChannelTests, bulkHelloCannotBecomeNormalScreenClient)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    NiceMock<MockServer> server;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    const std::string name("windows");
    const std::string token("0123456789abcdef");
    stream->queueInput(encodeBulkHello(name, token));

    ClientProxyUnknown unknown(stream, 30.0, &server, &events);
    unknown.handleDataForTest();

    EXPECT_EQ(nullptr, unknown.orphanClientProxy());
    std::string actualName;
    std::string actualToken;
    EXPECT_EQ(stream, unknown.orphanBulkStream(actualName, actualToken));
    EXPECT_EQ(name, actualName);
    EXPECT_EQ(token, actualToken);
    delete stream;
}

TEST(BulkChannelTests, acceptsFilePayloadFrames)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    stream->queueInput(encodeMessage(kMsgDFileTransfer));
    barrier::BulkChannel channel(stream, &handler, &events);

    channel.handleDataForTest();

    EXPECT_TRUE(channel.isActive());
    EXPECT_EQ(1, handler.messageCount);
    EXPECT_EQ(std::string(kMsgDFileTransfer, 4), handler.lastCode);
    EXPECT_EQ(0, handler.disconnectCount);
}

TEST(BulkChannelTests, rejectsControlFramesWithoutEscalatingToControlConnection)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    stream->queueInput(encodeMessage(kMsgCKeepAlive));
    barrier::BulkChannel channel(stream, &handler, &events);

    channel.handleDataForTest();

    EXPECT_FALSE(channel.isActive());
    EXPECT_TRUE(stream->closed);
    EXPECT_EQ(0, handler.messageCount);
    EXPECT_EQ(1, handler.disconnectCount);
}

TEST(BulkChannelTests, keepAliveTimerWritesBulkProbe)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    barrier::BulkChannel channel(stream, &handler, &events);

    channel.handleKeepAliveForTest();

    EXPECT_EQ(encodeMessage(kMsgBulkKeepAlive), stream->output);
    EXPECT_TRUE(channel.isActive());
    EXPECT_EQ(0, handler.messageCount);
}

TEST(BulkChannelTests, bulkProbeIsAnsweredWithoutReachingPayloadHandler)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    stream->queueInput(encodeMessage(kMsgBulkKeepAlive));
    barrier::BulkChannel channel(stream, &handler, &events);

    channel.handleDataForTest();

    EXPECT_EQ(encodeMessage(kMsgBulkKeepAliveAck), stream->output);
    EXPECT_TRUE(channel.isActive());
    EXPECT_EQ(0, handler.messageCount);
    EXPECT_EQ(0, handler.disconnectCount);
}

TEST(BulkChannelTests, keepAliveBurstYieldsAtParserFrameBudget)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    const std::vector<UInt8> probe = encodeMessage(kMsgBulkKeepAlive);
    for (int i = 0; i < 65; ++i) {
        stream->queueInput(probe);
    }
    barrier::BulkChannel channel(stream, &handler, &events);

    EXPECT_CALL(events, addEvent(_)).Times(1);
    channel.handleDataForTest();

    EXPECT_EQ(4u, stream->getSize());
    EXPECT_EQ(64u * 4u, stream->output.size());
    EXPECT_TRUE(channel.isActive());
    EXPECT_EQ(0, handler.messageCount);
    EXPECT_EQ(0, handler.disconnectCount);
}

TEST(BulkChannelTests, unansweredBulkProbesCloseOnlyBulkChannel)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    barrier::BulkChannel channel(stream, &handler, &events);

    channel.handleKeepAliveForTest();
    channel.handleKeepAliveForTest();
    channel.handleKeepAliveForTest();
    channel.handleKeepAliveForTest();

    EXPECT_FALSE(channel.isActive());
    EXPECT_TRUE(stream->closed);
    EXPECT_EQ(0, handler.messageCount);
    EXPECT_EQ(1, handler.disconnectCount);
}
