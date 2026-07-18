#define BARRIER_TEST_ENV

#include "barrier/BulkChannel.h"
#include "barrier/ClipboardChunk.h"
#include "barrier/FileChunk.h"
#include "barrier/FileReceiveSession.h"
#include "barrier/FileTransferProtocol.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "server/ClientProxyUnknown.h"
#include "io/IStream.h"
#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/server/MockServer.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::Invoke;
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

    void recordInputProgress(UInt32 bytes)
    {
        inputBytesReceived += bytes;
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
    void setInputPaused(bool paused) override
    {
        inputPaused = paused;
        ++inputPauseChanges;
    }
    void* getEventTarget() const override
    {
        return const_cast<DuplexMemoryStream*>(this);
    }
    bool isReady() const override { return getSize() != 0; }
    UInt32 getSize() const override
    {
        return static_cast<UInt32>(input.size() - inputOffset);
    }
    UInt32 getBufferedOutputSize() const override { return bufferedOutput; }
    std::uint64_t getOutputBytesWritten() const override
    {
        return outputBytesWritten;
    }
    std::uint64_t getInputBytesReceived() const override
    {
        return inputBytesReceived;
    }

    bool closed = false;
    bool inputPaused = false;
    int inputPauseChanges = 0;
    UInt32 bufferedOutput = 0;
    std::uint64_t outputBytesWritten = 0;
    std::uint64_t inputBytesReceived = 0;
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
                         kProtocolMajorVersion, 11,
                         &name, &token);
    return stream.output;
}

std::vector<UInt8> encodeBoundBulkHello(
    const std::string& name, const std::string& token,
    const std::string& connectionBinding)
{
    DuplexMemoryStream stream;
    ProtocolUtil::writef(&stream, kMsgHelloBulkBack1_12,
                         kProtocolMajorVersion, 12,
                         &name, &token, &connectionBinding);
    return stream.output;
}

std::vector<UInt8> encodeControlHello(SInt16 minor, const std::string& name)
{
    DuplexMemoryStream stream;
    ProtocolUtil::writef(&stream, kMsgHelloBack,
                         kProtocolMajorVersion, minor, &name);
    return stream.output;
}

std::vector<UInt8> encodeMessage(const char* message)
{
    return std::vector<UInt8>(message, message + 4);
}

std::vector<UInt8> encodeFileMessage(UInt8 mark, const std::string& content)
{
    DuplexMemoryStream stream;
    std::vector<char> mutableContent(content.begin(), content.end());
    mutableContent.push_back('\0');
    FileChunk::send(&stream, mark, mutableContent.data(), content.size());
    return stream.output;
}

std::vector<UInt8> encodeTransactionalFileMessage(
    const barrier::FileTransferFrame& frame)
{
    DuplexMemoryStream stream;
    EXPECT_TRUE(barrier::FileTransferProtocol::encode(
        &stream, frame, barrier::FileTransferRole::kPrimary));
    return stream.output;
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

    void handleBulkDisconnected(barrier::BulkChannel*,
                                std::uint64_t pausedGeneration) override
    {
        callbackOrder.push_back("disconnected");
        ++disconnectCount;
        inputPauseFailureGeneration = pausedGeneration;
    }

    int messageCount = 0;
    int disconnectCount = 0;
    std::uint64_t inputPauseFailureGeneration = 0;
    std::string lastCode;
    std::vector<std::string> callbackOrder;
};

class CommittingFileBulkHandler : public barrier::IBulkChannelHandler {
public:
    explicit CommittingFileBulkHandler(FileReceiveSession& session) :
        session(session),
        channel(),
        disconnectCount(0)
    {
    }

    bool handleBulkMessage(const UInt8* code, barrier::IStream* stream) override
    {
        if (std::memcmp(code, kMsgDFileTransfer, 4) != 0) {
            return false;
        }
        const int result = FileChunk::assemble(stream, session);
        if (result == kFinish) {
            const std::uint64_t generation = session.generation();
            std::shared_ptr<barrier::BulkChannel> current = channel.lock();
            if (!current || !current->pauseInputForCommit(generation) ||
                !session.installCommitBarrier(
                    generation,
                    current->makeInputResumeCallback(generation),
                    current->makeInputProgressCallback(generation))) {
                return false;
            }
            completedGenerations.push_back(generation);
        }
        return result != kError;
    }

    void handleBulkDisconnected(barrier::BulkChannel*, std::uint64_t) override
    {
        ++disconnectCount;
    }

    FileReceiveSession& session;
    std::weak_ptr<barrier::BulkChannel> channel;
    std::vector<std::uint64_t> completedGenerations;
    int disconnectCount;
};

}

TEST(BulkChannelTests, legacyBulkHelloIsRejected)
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
    std::string actualBinding;
    EXPECT_EQ(nullptr, unknown.orphanBulkStream(
        actualName, actualToken, actualBinding));
    EXPECT_TRUE(actualBinding.empty());
}

TEST(BulkChannelTests, legacyControlHelloIsRejected)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    NiceMock<MockServer> server;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    stream->queueInput(encodeControlHello(11, "legacy"));

    ClientProxyUnknown unknown(stream, 30.0, &server, &events);
    const size_t responseOffset = stream->output.size();
    unknown.handleDataForTest();

    EXPECT_EQ(nullptr, unknown.orphanClientProxy());
    ASSERT_GE(stream->output.size(), responseOffset + 4u);
    EXPECT_EQ(0, std::memcmp(stream->output.data() + responseOffset,
                             kMsgEIncompatible, 4));
}

TEST(BulkChannelTests, protocol112BulkHelloCarriesControlConnectionBinding)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    NiceMock<MockServer> server;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    const std::string name("windows");
    const std::string token("one-time-token");
    const std::string binding("00112233445566778899aabbccddeeff");
    stream->queueInput(encodeBoundBulkHello(name, token, binding));

    ClientProxyUnknown unknown(stream, 30.0, &server, &events);
    unknown.handleDataForTest();

    std::string actualName;
    std::string actualToken;
    std::string actualBinding;
    EXPECT_EQ(stream, unknown.orphanBulkStream(
        actualName, actualToken, actualBinding));
    EXPECT_EQ(name, actualName);
    EXPECT_EQ(token, actualToken);
    EXPECT_EQ(binding, actualBinding);
    delete stream;
}

TEST(BulkChannelTests, protocol112RejectsLegacyOrNonCanonicalBulkBinding)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);
    NiceMock<MockServer> server;

    DuplexMemoryStream* legacyStream = new DuplexMemoryStream();
    const std::string name("windows");
    const std::string token("legacy-token");
    DuplexMemoryStream legacyHello;
    ProtocolUtil::writef(&legacyHello, kMsgHelloBulkBack,
                         kProtocolMajorVersion, 12, &name, &token);
    legacyStream->queueInput(legacyHello.output);
    ClientProxyUnknown legacy(legacyStream, 30.0, &server, &events);
    legacy.handleDataForTest();
    std::string actualName;
    std::string actualToken;
    std::string actualBinding;
    EXPECT_EQ(nullptr, legacy.orphanBulkStream(
        actualName, actualToken, actualBinding));

    DuplexMemoryStream* invalidStream = new DuplexMemoryStream();
    const std::string invalidBinding("A0112233445566778899aabbccddeeff");
    invalidStream->queueInput(
        encodeBoundBulkHello(name, token, invalidBinding));
    ClientProxyUnknown invalid(invalidStream, 30.0, &server, &events);
    invalid.handleDataForTest();
    EXPECT_EQ(nullptr, invalid.orphanBulkStream(
        actualName, actualToken, actualBinding));
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

TEST(BulkChannelTests, acceptsProtocol112FileDataFrames)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    const std::string binding("00112233445566778899aabbccddeeff");
    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 1);
    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    stream->queueInput(encodeTransactionalFileMessage(
        barrier::FileTransferFrame::data(
            binding, transferId, 0, "payload")));
    barrier::BulkChannel channel(stream, &handler, &events);

    channel.handleDataForTest();

    EXPECT_TRUE(channel.isActive());
    EXPECT_EQ(1, handler.messageCount);
    EXPECT_EQ(std::string(kMsgDFileTransferData1_12, 4), handler.lastCode);
    EXPECT_EQ(0, handler.disconnectCount);
}

TEST(BulkChannelTests, acceptsProtocol112FileEndFrames)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    const std::string binding("00112233445566778899aabbccddeeff");
    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 1);
    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    stream->queueInput(encodeTransactionalFileMessage(
        barrier::FileTransferFrame::end(
            binding, transferId, 7,
            "sha256:239f59ed55e737c77147cf55ad0c1b030b6d7ee748a7426952f9b852d5a935e5")));
    barrier::BulkChannel channel(stream, &handler, &events);

    channel.handleDataForTest();

    EXPECT_TRUE(channel.isActive());
    EXPECT_EQ(1, handler.messageCount);
    EXPECT_EQ(std::string(kMsgDFileTransferEnd1_12, 4), handler.lastCode);
    EXPECT_EQ(0, handler.disconnectCount);
}

TEST(BulkChannelTests, rejectsProtocol112CancelOnBulkTransport)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    const std::string binding("00112233445566778899aabbccddeeff");
    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 2);
    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    stream->queueInput(encodeTransactionalFileMessage(
        barrier::FileTransferFrame::cancel(
            binding, transferId,
            barrier::FileTransferReason::kCancelled)));
    barrier::BulkChannel channel(stream, &handler, &events);

    channel.handleDataForTest();

    EXPECT_FALSE(channel.isActive());
    EXPECT_EQ(0, handler.messageCount);
    EXPECT_EQ(1, handler.disconnectCount);
}

TEST(BulkChannelTests, consecutiveFilesWaitForPriorCommitWithinOneInputBatch)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    FileReceiveSession session;
    ASSERT_TRUE(session.begin(1, FileChunk::kMemoryReceiveLimit,
                              FileChunk::kMemoryReceiveLimit));
    ASSERT_TRUE(session.append("a"));

    CommittingFileBulkHandler handler(session);
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    stream->queueInput(encodeFileMessage(kDataEnd, ""));
    stream->queueInput(encodeFileMessage(kDataStart, "1"));
    stream->queueInput(encodeFileMessage(kDataChunk, "b"));
    stream->queueInput(encodeFileMessage(kDataEnd, ""));
    std::shared_ptr<barrier::BulkChannel> channel =
        std::make_shared<barrier::BulkChannel>(stream, &handler, &events);
    handler.channel = channel;

    int resumedInputEvents = 0;
    EXPECT_CALL(events, addEvent(_)).Times(AnyNumber()).WillRepeatedly(
        Invoke([&](const Event& event) {
            if (event.getType() == streamEvents.inputReady()) {
                ++resumedInputEvents;
            }
        }));

    channel->handleDataForTest();

    ASSERT_EQ(1u, handler.completedGenerations.size());
    EXPECT_GT(stream->getSize(), 0u);
    EXPECT_TRUE(channel->isActive());
    EXPECT_EQ(0, handler.disconnectCount);

    std::vector<std::string> completedPayloads;
    std::string data;
    size_t expectedSize = 0;
    barrier::fs::path spoolPath;
    session.takeCompleted(data, expectedSize, spoolPath);
    ASSERT_EQ(1u, expectedSize);
    ASSERT_TRUE(spoolPath.empty());
    completedPayloads.push_back(data);
    channel->serviceInputPauseForTest(0.0);
    EXPECT_EQ(1, resumedInputEvents);

    channel->handleDataForTest();

    ASSERT_EQ(2u, handler.completedGenerations.size());
    EXPECT_EQ(0u, stream->getSize());
    EXPECT_TRUE(channel->isActive());
    EXPECT_EQ(0, handler.disconnectCount);

    session.takeCompleted(data, expectedSize, spoolPath);
    ASSERT_EQ(1u, expectedSize);
    ASSERT_TRUE(spoolPath.empty());
    completedPayloads.push_back(data);

    ASSERT_EQ(2u, completedPayloads.size());
    EXPECT_EQ("a", completedPayloads[0]);
    EXPECT_EQ("b", completedPayloads[1]);
}

TEST(BulkChannelTests, consecutiveFilesDoNotResetFinalizingSpoolBeforeCommit)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    const std::string firstPayload(1024 * 1024, 'a');
    FileReceiveSession session;
    ASSERT_TRUE(session.begin(firstPayload.size(), 0, 0,
                              firstPayload.size() * 2));
    for (size_t offset = 0; offset < firstPayload.size();
         offset += FileReceiveSession::kMaxAsyncChunkSize) {
        ASSERT_NE(FileReceiveSession::kAppendFailed,
                  session.append(firstPayload.substr(
                      offset, FileReceiveSession::kMaxAsyncChunkSize)));
    }

    CommittingFileBulkHandler handler(session);
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    stream->queueInput(encodeFileMessage(kDataEnd, ""));
    stream->queueInput(encodeFileMessage(kDataStart, "1"));
    stream->queueInput(encodeFileMessage(kDataChunk, "b"));
    stream->queueInput(encodeFileMessage(kDataEnd, ""));
    std::shared_ptr<barrier::BulkChannel> channel =
        std::make_shared<barrier::BulkChannel>(stream, &handler, &events);
    handler.channel = channel;

    channel->handleDataForTest();

    ASSERT_EQ(1u, handler.completedGenerations.size());
    const UInt32 bufferedSecondTransfer = stream->getSize();
    ASSERT_GT(bufferedSecondTransfer, 0u);
    EXPECT_TRUE(session.isFinalizing() || session.isComplete());

    channel->handleDataForTest();

    EXPECT_EQ(bufferedSecondTransfer, stream->getSize());
    EXPECT_EQ(1u, handler.completedGenerations.size());
    EXPECT_TRUE(channel->isActive());

    for (int attempt = 0; attempt < 100000 && !session.isComplete(); ++attempt) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(session.isComplete());

    std::string data;
    size_t expectedSize = 0;
    barrier::fs::path spoolPath;
    session.takeCompleted(data, expectedSize, spoolPath);
    ASSERT_EQ(firstPayload.size(), expectedSize);
    ASSERT_FALSE(spoolPath.empty());
    ASSERT_TRUE(barrier::fs::exists(spoolPath));
    std::ifstream spool(spoolPath.u8string().c_str(),
                        std::ios::in | std::ios::binary);
    const std::string spooledData((std::istreambuf_iterator<char>(spool)),
                                  std::istreambuf_iterator<char>());
    EXPECT_EQ(firstPayload, spooledData);
    spool.close();
    FileChunk::releaseReceiveBuffer(data, expectedSize, &spoolPath);
    channel->serviceInputPauseForTest(0.0);

    channel->handleDataForTest();

    ASSERT_EQ(2u, handler.completedGenerations.size());
    EXPECT_EQ(0u, stream->getSize());
    EXPECT_TRUE(channel->isActive());
    session.takeCompleted(data, expectedSize, spoolPath);
    EXPECT_EQ(1u, expectedSize);
    EXPECT_EQ("b", data);
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

TEST(BulkChannelTests, decreasingOutputBacklogDoesNotConsumeKeepAliveBudget)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    barrier::BulkChannel channel(stream, &handler, &events);

    for (UInt32 buffered = 7u * 1024u; buffered != 0; buffered -= 1024u) {
        stream->bufferedOutput = buffered;
        channel.handleKeepAliveForTest();
    }

    EXPECT_TRUE(channel.isActive());
    EXPECT_FALSE(stream->closed);
    EXPECT_TRUE(stream->output.empty());
    EXPECT_EQ(0, handler.disconnectCount);
}

TEST(BulkChannelTests, stalledOutputBacklogClosesWithinBoundedIntervals)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    barrier::BulkChannel channel(stream, &handler, &events);
    stream->bufferedOutput = 4096u;

    for (int interval = 0; interval < 15; ++interval) {
        channel.handleKeepAliveForTest();
    }

    EXPECT_TRUE(channel.isActive());
    EXPECT_FALSE(stream->closed);

    channel.handleKeepAliveForTest();

    EXPECT_FALSE(channel.isActive());
    EXPECT_TRUE(stream->closed);
    EXPECT_TRUE(stream->output.empty());
    EXPECT_EQ(1, handler.disconnectCount);
}

TEST(BulkChannelTests, writerProgressKeepsConstantOutputBacklogAlive)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    barrier::BulkChannel channel(stream, &handler, &events);
    stream->bufferedOutput = 4096u;

    for (int interval = 0; interval < 32; ++interval) {
        stream->outputBytesWritten += 1024u;
        channel.handleKeepAliveForTest();
    }

    EXPECT_TRUE(channel.isActive());
    EXPECT_FALSE(stream->closed);
    EXPECT_TRUE(stream->output.empty());
    EXPECT_EQ(0, handler.disconnectCount);
}

TEST(BulkChannelTests, writerProgressKeepsGrowingOutputBacklogAlive)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    barrier::BulkChannel channel(stream, &handler, &events);
    stream->bufferedOutput = 4096u;

    for (int interval = 0; interval < 32; ++interval) {
        stream->bufferedOutput += 256u;
        stream->outputBytesWritten += 1024u;
        channel.handleKeepAliveForTest();
    }

    EXPECT_TRUE(channel.isActive());
    EXPECT_FALSE(stream->closed);
    EXPECT_TRUE(stream->output.empty());
    EXPECT_EQ(0, handler.disconnectCount);
}

TEST(BulkChannelTests, recentFrameActivityDefersIdleProbe)
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
    channel.handleKeepAliveForTest();

    EXPECT_TRUE(channel.isActive());
    EXPECT_TRUE(stream->output.empty());
    EXPECT_EQ(1, handler.messageCount);
    EXPECT_EQ(0, handler.disconnectCount);
}

TEST(BulkChannelTests, partialInboundFrameProgressDoesNotConsumeKeepAliveBudget)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    barrier::BulkChannel channel(stream, &handler, &events);

    for (int interval = 0; interval < 8; ++interval) {
        stream->recordInputProgress(1);
        channel.handleKeepAliveForTest();
    }

    EXPECT_TRUE(channel.isActive());
    EXPECT_FALSE(stream->closed);
    EXPECT_TRUE(stream->output.empty());
    EXPECT_EQ(0, handler.disconnectCount);
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

TEST(BulkChannelTests, queuedClipboardChunkDetectsClosedPinnedRoute)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    std::shared_ptr<barrier::BulkChannel> channel =
        std::make_shared<barrier::BulkChannel>(stream, &handler, &events);
    std::unique_ptr<ClipboardChunk> chunk(
        ClipboardChunk::start(kClipboardClipboard, 1, "0"));
    chunk->setSendRoute(stream, channel);

    EXPECT_TRUE(chunk->isSendRouteActive());

    channel->close();

    EXPECT_FALSE(chunk->isSendRouteActive());
}

TEST(BulkChannelTests, keepAliveBurstYieldsAndReschedulesWithinParserBudget)
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

    int rescheduled = 0;
    EXPECT_CALL(events, addEvent(_)).Times(AnyNumber()).WillRepeatedly(
        Invoke([&](const Event& event) {
            if (event.getType() == streamEvents.inputReady()) {
                ++rescheduled;
            }
        }));
    channel.handleDataForTest();

    const UInt32 totalBytes = 65u * 4u;
    const UInt32 remainingAfterFirstBatch = stream->getSize();
    EXPECT_GT(remainingAfterFirstBatch, 0u);
    EXPECT_LT(remainingAfterFirstBatch, totalBytes);
    EXPECT_EQ(static_cast<size_t>(totalBytes - remainingAfterFirstBatch),
              stream->output.size());
    EXPECT_EQ(1, rescheduled);

    for (int attempt = 0; attempt < 65 && stream->getSize() != 0; ++attempt) {
        channel.handleDataForTest();
    }

    EXPECT_EQ(0u, stream->getSize());
    EXPECT_EQ(static_cast<size_t>(totalBytes), stream->output.size());
    EXPECT_GE(rescheduled, 1);
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

TEST(BulkChannelTests, pausedChannelKeepsUnpausedPeerAlive)
{
    NiceMock<MockEventQueue> pausedEvents;
    IStreamEvents pausedStreamEvents;
    ClientProxyUnknownEvents pausedUnknownEvents;
    ClientProxyEvents pausedProxyEvents;
    setEventDefaults(pausedEvents, pausedStreamEvents,
                     pausedUnknownEvents, pausedProxyEvents);
    NiceMock<MockEventQueue> peerEvents;
    IStreamEvents peerStreamEvents;
    ClientProxyUnknownEvents peerUnknownEvents;
    ClientProxyEvents peerProxyEvents;
    setEventDefaults(peerEvents, peerStreamEvents,
                     peerUnknownEvents, peerProxyEvents);

    RecordingBulkHandler pausedHandler;
    DuplexMemoryStream* pausedStream = new DuplexMemoryStream();
    barrier::BulkChannel paused(pausedStream, &pausedHandler, &pausedEvents);
    RecordingBulkHandler peerHandler;
    DuplexMemoryStream* peerStream = new DuplexMemoryStream();
    barrier::BulkChannel peer(peerStream, &peerHandler, &peerEvents);

    ASSERT_TRUE(paused.pauseInputForBackpressure(17));
    size_t forwardedBytes = 0;
    for (int interval = 0; interval < 8 && peer.isActive(); ++interval) {
        paused.handleKeepAliveForTest();
        const std::vector<UInt8> outbound(
            pausedStream->output.begin() + forwardedBytes,
            pausedStream->output.end());
        forwardedBytes = pausedStream->output.size();
        peerStream->queueInput(outbound);
        peer.handleDataForTest();
        peer.handleKeepAliveForTest();
    }

    EXPECT_TRUE(paused.isActive());
    EXPECT_TRUE(peer.isActive());
    EXPECT_FALSE(peerStream->closed);
    EXPECT_EQ(0, peerHandler.disconnectCount);
}

TEST(BulkChannelTests, commitPauseFreezesIdleProbeBudget)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    barrier::BulkChannel channel(stream, &handler, &events);

    ASSERT_TRUE(channel.pauseInputForCommit(7));
    for (int interval = 0; interval < 8; ++interval) {
        channel.handleKeepAliveForTest();
    }

    EXPECT_TRUE(channel.isActive());
    EXPECT_FALSE(stream->closed);
    EXPECT_EQ(8u * encodeMessage(kMsgBulkKeepAlive).size(),
              stream->output.size());
    EXPECT_EQ(0, handler.disconnectCount);
}

TEST(BulkChannelTests, backpressurePauseBuffersFramesUntilWriterResume)
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

    int resumedInputEvents = 0;
    EXPECT_CALL(events, addEvent(_)).Times(AnyNumber()).WillRepeatedly(
        Invoke([&](const Event& event) {
            if (event.getType() == streamEvents.inputReady()) {
                ++resumedInputEvents;
            }
        }));
    ASSERT_TRUE(channel.pauseInputForBackpressure(9));
    EXPECT_TRUE(stream->inputPaused);
    EXPECT_EQ(1, stream->inputPauseChanges);
    channel.handleDataForTest();
    for (int interval = 0; interval < 8; ++interval) {
        channel.handleKeepAliveForTest();
    }

    EXPECT_EQ(4u, stream->getSize());
    EXPECT_EQ(0, handler.messageCount);
    EXPECT_TRUE(channel.isActive());
    EXPECT_EQ(8u * encodeMessage(kMsgBulkKeepAlive).size(),
              stream->output.size());

    channel.resumeInputAfterBackpressure(9);
    channel.serviceInputPauseForTest(0.0);
    EXPECT_FALSE(stream->inputPaused);
    EXPECT_EQ(2, stream->inputPauseChanges);
    EXPECT_EQ(1, resumedInputEvents);
    channel.handleDataForTest();

    EXPECT_EQ(0u, stream->getSize());
    EXPECT_EQ(1, handler.messageCount);
    EXPECT_TRUE(channel.isActive());
    EXPECT_EQ(0, handler.disconnectCount);
}

TEST(BulkChannelTests, pauseResumeAfterCloseNeverTouchesEventQueue)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    barrier::BulkChannel channel(stream, &handler, &events);

    ASSERT_TRUE(channel.pauseInputForBackpressure(11));
    channel.close();
    EXPECT_CALL(events, addEvent(_)).Times(0);

    channel.resumeInputAfterBackpressure(11);
    channel.serviceInputPauseForTest(0.0);

    EXPECT_FALSE(channel.isActive());
}

TEST(BulkChannelTests, resumeQueueFailureClosesOnlyBulkChannel)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    barrier::BulkChannel channel(stream, &handler, &events);

    ASSERT_TRUE(channel.pauseInputForBackpressure(12));
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke([](const Event&) {
        throw std::runtime_error("event queue unavailable");
    }));

    channel.resumeInputAfterBackpressure(12);
    EXPECT_NO_THROW(channel.serviceInputPauseForTest(0.0));

    EXPECT_FALSE(channel.isActive());
    EXPECT_TRUE(stream->closed);
    EXPECT_EQ(0u, handler.inputPauseFailureGeneration);
    EXPECT_EQ(1, handler.disconnectCount);
    ASSERT_EQ(1u, handler.callbackOrder.size());
    EXPECT_EQ("disconnected", handler.callbackOrder[0]);
}

TEST(BulkChannelTests, stalledInputPauseClosesOnlyBulkChannel)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    barrier::BulkChannel channel(stream, &handler, &events);

    ASSERT_TRUE(channel.pauseInputForCommit(13));
    channel.serviceInputPauseForTest(61.0);

    EXPECT_FALSE(channel.isActive());
    EXPECT_TRUE(stream->closed);
    EXPECT_EQ(13u, handler.inputPauseFailureGeneration);
    EXPECT_EQ(1, handler.disconnectCount);
    ASSERT_EQ(1u, handler.callbackOrder.size());
    EXPECT_EQ("disconnected", handler.callbackOrder[0]);
}

TEST(BulkChannelTests, completedIdleProbeWritesDoNotReplacePeerAcknowledgement)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyUnknownEvents unknownEvents;
    ClientProxyEvents proxyEvents;
    setEventDefaults(events, streamEvents, unknownEvents, proxyEvents);

    RecordingBulkHandler handler;
    DuplexMemoryStream* stream = new DuplexMemoryStream();
    barrier::BulkChannel channel(stream, &handler, &events);

    for (int interval = 0; interval < 8 && channel.isActive(); ++interval) {
        const size_t bytesBefore = stream->output.size();
        channel.handleKeepAliveForTest();
        stream->outputBytesWritten += stream->output.size() - bytesBefore;
    }

    EXPECT_FALSE(channel.isActive());
    EXPECT_TRUE(stream->closed);
    EXPECT_EQ(1, handler.disconnectCount);
}
