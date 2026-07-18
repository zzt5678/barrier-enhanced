#define BARRIER_TEST_ENV
#include "server/ClientProxy1_6.h"
#include "server/ClientProxy1_7.h"
#include "server/ClientProxy1_8.h"
#include "server/ClientProxy1_9.h"
#include "server/ClientProxy1_10.h"
#include "server/ClientProxy1_11.h"
#include "server/ClientProxy1_12.h"

#include "barrier/Clipboard.h"
#include "barrier/FileChunk.h"
#include "barrier/FileTransferProtocol.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/RemoteFileClipboard.h"
#include "barrier/StreamChunker.h"
#include "barrier/protocol_types.h"
#include "arch/Arch.h"
#include "base/Stopwatch.h"
#include "mt/Thread.h"
#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/io/MockStream.h"
#include "test/mock/server/MockServer.h"

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <vector>

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::DoubleEq;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {

void setClientProxy16EventDefaults(MockEventQueue& events,
                                   IStreamEvents& streamEvents,
                                   ClipboardEvents& clipboardEvents,
                                   FileEvents& fileEvents,
                                   Event::Type& nextType);

TEST(ClientProxyLifecycleTests, protocol110AdvertisesCommittedHandoffAck)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);

    NiceMock<MockServer> server;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    NiceMock<MockStream>* legacyStream = new NiceMock<MockStream>();
    ON_CALL(*legacyStream, getEventTarget()).WillByDefault(Return(legacyStream));
    ON_CALL(*legacyStream, getBufferedOutputSize()).WillByDefault(Return(0));
    ClientProxy1_9 legacy("legacy", legacyStream, &server, &events);
    EXPECT_FALSE(legacy.supportsInputHandoffCommitAck());

    NiceMock<MockStream>* currentStream = new NiceMock<MockStream>();
    ON_CALL(*currentStream, getEventTarget()).WillByDefault(Return(currentStream));
    ON_CALL(*currentStream, getBufferedOutputSize()).WillByDefault(Return(0));
    ClientProxy1_10 current("current", currentStream, &server, &events);
    EXPECT_TRUE(current.supportsInputHandoffCommitAck());
}

void setClientProxy16EventDefaults(MockEventQueue& events,
                                   IStreamEvents& streamEvents,
                                   ClipboardEvents& clipboardEvents,
                                   FileEvents& fileEvents,
                                   Event::Type& nextType)
{
    streamEvents.setEvents(&events);
    clipboardEvents.setEvents(&events);
    fileEvents.setEvents(&events);

    ON_CALL(events, forIStream()).WillByDefault(ReturnRef(streamEvents));
    ON_CALL(events, forClipboard()).WillByDefault(ReturnRef(clipboardEvents));
    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, getQueuedEventCount()).WillByDefault(Return(0));
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([&nextType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = nextType++;
            }
            return type;
        }));
}

Clipboard makeLargeClipboard()
{
    Clipboard clipboard;
    clipboard.open(40);
    clipboard.empty();
    clipboard.add(IClipboard::kText, std::string(300 * 1024, 'x'));
    clipboard.close();
    return clipboard;
}

Clipboard makeFileClipboard()
{
    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.sessionId = "00000000000000000000000000000031";
    payload.paths.push_back(barrier::fs::u8path("/tmp/small.txt"));

    Clipboard clipboard;
    clipboard.open(40);
    clipboard.empty();
    clipboard.add(IClipboard::kFileList,
                  RemoteFileClipboard::serialize(payload));
    clipboard.close();
    return clipboard;
}

class WorkerExitGate {
public:
    WorkerExitGate() :
        m_gate(new std::atomic<bool>(false))
    {
    }

    ~WorkerExitGate()
    {
        release();
    }

    const std::shared_ptr<std::atomic<bool> >& gate() const
    {
        return m_gate;
    }

    void release()
    {
        m_gate->store(true, std::memory_order_release);
    }

private:
    std::shared_ptr<std::atomic<bool> > m_gate;
};

class FileMessageStream : public barrier::IStream {
public:
    FileMessageStream(UInt8 mark, const String& payload) :
        m_offset(0)
    {
        m_data.push_back(mark);
        const UInt32 size = static_cast<UInt32>(payload.size());
        m_data.push_back(static_cast<UInt8>((size >> 24) & 0xff));
        m_data.push_back(static_cast<UInt8>((size >> 16) & 0xff));
        m_data.push_back(static_cast<UInt8>((size >> 8) & 0xff));
        m_data.push_back(static_cast<UInt8>(size & 0xff));
        m_data.insert(m_data.end(), payload.begin(), payload.end());
    }

    void close() override { }
    UInt32 read(void* buffer, UInt32 count) override
    {
        const UInt32 remaining = static_cast<UInt32>(m_data.size() - m_offset);
        const UInt32 copied = count < remaining ? count : remaining;
        if (copied != 0) {
            std::memcpy(buffer, &m_data[m_offset], copied);
            m_offset += copied;
        }
        return copied;
    }
    void write(const void*, UInt32) override { }
    void writeLowPriority(const void*, UInt32) override { }
    void flush() override { }
    void shutdownInput() override { }
    void shutdownOutput() override { }
    void* getEventTarget() const override { return NULL; }
    bool isReady() const override { return m_offset < m_data.size(); }
    UInt32 getSize() const override
    {
        return static_cast<UInt32>(m_data.size() - m_offset);
    }
    UInt32 getBufferedOutputSize() const override { return 0; }

private:
    std::vector<UInt8> m_data;
    std::size_t m_offset;
};

class RecordingProtocolStream : public barrier::IStream {
public:
    void close() override { }
    UInt32 read(void* buffer, UInt32 count) override
    {
        const UInt32 available = getSize();
        const UInt32 copied = count < available ? count : available;
        if (copied != 0) {
            std::memcpy(buffer, output.data() + readOffset, copied);
            readOffset += copied;
        }
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
        return const_cast<RecordingProtocolStream*>(this);
    }
    bool isReady() const override { return getSize() != 0; }
    UInt32 getSize() const override
    {
        return static_cast<UInt32>(output.size() - readOffset);
    }
    UInt32 getBufferedOutputSize() const override { return 0; }

    void clear()
    {
        output.clear();
        readOffset = 0;
    }
    void consumeCode() { readOffset = 4; }

    std::vector<UInt8> output;
    std::size_t readOffset = 0;
};

TEST(ClientProxyLifecycleTests, protocol112OfferUsesStableControlBinding)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    NiceMock<MockServer> server;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    RecordingProtocolStream* firstStream = new RecordingProtocolStream();
    ClientProxy1_12 first("first", firstStream, &server, &events);
    const std::string firstBinding = first.getConnectionBinding();
    EXPECT_TRUE(isValidConnectionBinding(firstBinding));
    EXPECT_TRUE(first.supportsTransactionalFileTransfer());

    firstStream->clear();
    const std::string token("one-time-token");
    first.offerBulkChannel(token);
    ASSERT_GE(firstStream->output.size(), 4u);
    EXPECT_EQ(0, std::memcmp(firstStream->output.data(),
                             kMsgCBulkOffer1_12, 4));
    firstStream->consumeCode();
    std::string decodedToken;
    std::string decodedBinding;
    ASSERT_TRUE(ProtocolUtil::readf(
        firstStream, kMsgCBulkOffer1_12 + 4,
        &decodedToken, &decodedBinding));
    EXPECT_EQ(token, decodedToken);
    EXPECT_EQ(firstBinding, decodedBinding);
    EXPECT_EQ(firstBinding, first.getConnectionBinding());

    RecordingProtocolStream* secondStream = new RecordingProtocolStream();
    ClientProxy1_12 second("second", secondStream, &server, &events);
    EXPECT_TRUE(isValidConnectionBinding(second.getConnectionBinding()));
    EXPECT_NE(firstBinding, second.getConnectionBinding());
}

TEST(ClientProxyLifecycleTests, protocol112PostAckCancelUsesControlWhileDataUsesBulk)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    NiceMock<MockServer> server;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    RecordingProtocolStream* control = new RecordingProtocolStream();
    ClientProxy1_12 proxy("client", control, &server, &events);
    RecordingProtocolStream* bulk = new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(bulk));
    control->clear();
    bulk->clear();

    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 8);
    ASSERT_TRUE(proxy.sendTransactionalFileFrame(
        barrier::FileTransferFrame::data(
            proxy.getConnectionBinding(), transferId, 0, "x"), true));
    ASSERT_TRUE(proxy.sendTransactionalFileFrame(
        barrier::FileTransferFrame::cancel(
            proxy.getConnectionBinding(), transferId,
            barrier::FileTransferReason::kCancelled), true));
    UInt8 code[4];
    barrier::FileTransferFrame frame;
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kPrimary,
        proxy.getConnectionBinding(), frame));
    EXPECT_EQ(barrier::FileTransferFrameType::kCancel, frame.type);
    EXPECT_EQ(0u, control->getSize());

    ASSERT_EQ(4u, bulk->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, bulk, barrier::FileTransferRole::kPrimary,
        proxy.getConnectionBinding(), frame));
    EXPECT_EQ(barrier::FileTransferFrameType::kData, frame.type);
    EXPECT_EQ(0u, bulk->getSize());

    ASSERT_TRUE(proxy.sendTransactionalFileFrame(
        barrier::FileTransferFrame::cancel(
            proxy.getConnectionBinding(), transferId,
            barrier::FileTransferReason::kTimeout), false));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kPrimary,
        proxy.getConnectionBinding(), frame));
    EXPECT_EQ(barrier::FileTransferFrameType::kCancel, frame.type);
    EXPECT_EQ(0u, control->getSize());
}

TEST(ClientProxyLifecycleTests, protocol112StartIsAcknowledgedOnControl)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    NiceMock<MockServer> server;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    RecordingProtocolStream* control = new RecordingProtocolStream();
    ClientProxy1_12 proxy("client", control, &server, &events);
    RecordingProtocolStream* bulk = new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(bulk));
    control->clear();
    bulk->clear();
    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 1);
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), transferId, 3),
        barrier::FileTransferRole::kSecondary));

    UInt8 code[4];
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    barrier::FileTransferFrame ack;
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    EXPECT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);
    EXPECT_EQ(transferId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    EXPECT_TRUE(proxy.testHasTransactionalReceive());
}

TEST(ClientProxyLifecycleTests,
     protocol112StreamingBulkDisconnectReleasesReceiveForReplacementRoute)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    NiceMock<MockServer> server;
    server.m_events = &events;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    RecordingProtocolStream* control = new RecordingProtocolStream();
    ClientProxy1_12 proxy("client", control, &server, &events);
    RecordingProtocolStream* firstBulk = new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(firstBulk));
    std::shared_ptr<barrier::BulkChannel> firstChannel =
        proxy.acquireBulkChannel();
    ASSERT_TRUE(firstChannel);
    control->clear();
    firstBulk->clear();

    const UInt32 interruptedId =
        barrier::FileTransferProtocol::makeTransferId(
            barrier::FileTransferRole::kSecondary, 21);
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), interruptedId, 3),
        barrier::FileTransferRole::kSecondary));
    UInt8 code[4];
    barrier::FileTransferFrame ack;
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    ASSERT_EQ(barrier::FileTransferReason::kNone, ack.reason);

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        firstBulk,
        barrier::FileTransferFrame::data(
            proxy.getConnectionBinding(), interruptedId, 0, "abc"),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, firstBulk->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.handleBulkMessage(code, firstBulk));
    ASSERT_TRUE(proxy.testHasTransactionalReceive());

    firstBulk->clear();
    for (int i = 0; i < 4; ++i) {
        firstChannel->handleKeepAliveForTest();
    }
    ASSERT_FALSE(firstChannel->isActive());
    EXPECT_FALSE(proxy.testHasTransactionalReceive());

    RecordingProtocolStream* replacementBulk =
        new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(replacementBulk));
    replacementBulk->clear();
    control->clear();
    const UInt32 replacementId =
        barrier::FileTransferProtocol::makeTransferId(
            barrier::FileTransferRole::kSecondary, 22);
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), replacementId, 0),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    EXPECT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);
    EXPECT_EQ(replacementId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    EXPECT_EQ(replacementId, proxy.testTransactionalReceiveId());
}

TEST(ClientProxyLifecycleTests,
     protocol112PausedBulkDisconnectReleasesReceiveForReplacementRoute)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    ON_CALL(events, newTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));
    NiceMock<MockServer> server;
    server.m_events = &events;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    RecordingProtocolStream* control = new RecordingProtocolStream();
    ClientProxy1_12 proxy("client", control, &server, &events);
    RecordingProtocolStream* firstBulk = new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(firstBulk));
    std::shared_ptr<barrier::BulkChannel> firstChannel =
        proxy.acquireBulkChannel();
    ASSERT_TRUE(firstChannel);
    control->clear();
    firstBulk->clear();

    const UInt32 interruptedId =
        barrier::FileTransferProtocol::makeTransferId(
            barrier::FileTransferRole::kSecondary, 23);
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), interruptedId, 1),
        barrier::FileTransferRole::kSecondary));
    UInt8 code[4];
    barrier::FileTransferFrame ack;
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    ASSERT_EQ(barrier::FileTransferReason::kNone, ack.reason);

    const std::uint64_t interruptedGeneration =
        proxy.testTransactionalReceiveGeneration();
    ASSERT_TRUE(firstChannel->pauseInputForBackpressure(
        interruptedGeneration));
    firstChannel->serviceInputPauseForTest(61.0);
    ASSERT_FALSE(firstChannel->isActive());
    EXPECT_FALSE(proxy.testHasTransactionalReceive());

    RecordingProtocolStream* replacementBulk =
        new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(replacementBulk));
    replacementBulk->clear();
    control->clear();
    const UInt32 replacementId =
        barrier::FileTransferProtocol::makeTransferId(
            barrier::FileTransferRole::kSecondary, 24);
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), replacementId, 0),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    EXPECT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);
    EXPECT_EQ(replacementId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    EXPECT_EQ(replacementId, proxy.testTransactionalReceiveId());

    control->clear();
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::cancel(
            proxy.getConnectionBinding(), interruptedId,
            barrier::FileTransferReason::kConnectionLost),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    EXPECT_EQ(barrier::FileTransferFrameType::kCancelAck, ack.type);
    EXPECT_EQ(interruptedId, ack.transferId);
    EXPECT_EQ(replacementId, proxy.testTransactionalReceiveId());

    // A delayed duplicate callback from the retired route must not clear the
    // replacement transfer now owned by the new route.
    proxy.handleBulkDisconnected(firstChannel.get(), interruptedGeneration);
    EXPECT_EQ(replacementId, proxy.testTransactionalReceiveId());
}

TEST(ClientProxyLifecycleTests,
     protocol112ActiveBulkReplacementReleasesReceiveRoute)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    NiceMock<MockServer> server;
    server.m_events = &events;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    RecordingProtocolStream* control = new RecordingProtocolStream();
    ClientProxy1_12 proxy("client", control, &server, &events);
    RecordingProtocolStream* firstBulk = new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(firstBulk));
    std::shared_ptr<barrier::BulkChannel> firstChannel =
        proxy.acquireBulkChannel();
    ASSERT_TRUE(firstChannel);
    control->clear();
    firstBulk->clear();

    const UInt32 interruptedId =
        barrier::FileTransferProtocol::makeTransferId(
            barrier::FileTransferRole::kSecondary, 25);
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), interruptedId, 1),
        barrier::FileTransferRole::kSecondary));
    UInt8 code[4];
    barrier::FileTransferFrame ack;
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    ASSERT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    ASSERT_EQ(interruptedId, proxy.testTransactionalReceiveId());

    RecordingProtocolStream* replacementBulk =
        new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(replacementBulk));
    EXPECT_FALSE(firstChannel->isActive());
    EXPECT_FALSE(proxy.testHasTransactionalReceive());
    replacementBulk->clear();
    control->clear();

    const UInt32 replacementId =
        barrier::FileTransferProtocol::makeTransferId(
            barrier::FileTransferRole::kSecondary, 26);
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), replacementId, 0),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    EXPECT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);
    EXPECT_EQ(replacementId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    EXPECT_EQ(replacementId, proxy.testTransactionalReceiveId());
}

TEST(ClientProxyLifecycleTests,
     protocol112ExplicitBulkDetachReleasesOppositeReceiveRoute)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    NiceMock<MockServer> server;
    server.m_events = &events;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    RecordingProtocolStream* control = new RecordingProtocolStream();
    ClientProxy1_12 proxy("client", control, &server, &events);
    RecordingProtocolStream* firstBulk = new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(firstBulk));
    std::shared_ptr<barrier::BulkChannel> firstChannel =
        proxy.acquireBulkChannel();
    ASSERT_TRUE(firstChannel);
    control->clear();
    firstBulk->clear();

    const UInt32 interruptedId =
        barrier::FileTransferProtocol::makeTransferId(
            barrier::FileTransferRole::kSecondary, 27);
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), interruptedId, 1),
        barrier::FileTransferRole::kSecondary));
    UInt8 code[4];
    barrier::FileTransferFrame ack;
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    ASSERT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    ASSERT_EQ(interruptedId, proxy.testTransactionalReceiveId());

    // A local sender timeout closes and detaches this full-duplex route
    // without delivering BulkChannel::fail() to the opposite receiver.
    firstChannel->close();
    ASSERT_TRUE(proxy.testHasTransactionalReceive());
    proxy.detachBulkChannel();
    EXPECT_FALSE(proxy.testHasTransactionalReceive());

    RecordingProtocolStream* replacementBulk =
        new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(replacementBulk));
    replacementBulk->clear();
    control->clear();
    const UInt32 replacementId =
        barrier::FileTransferProtocol::makeTransferId(
            barrier::FileTransferRole::kSecondary, 28);
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), replacementId, 0),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    EXPECT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);
    EXPECT_EQ(replacementId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    EXPECT_EQ(replacementId, proxy.testTransactionalReceiveId());
}

TEST(ClientProxyLifecycleTests, protocol112RejectsDataOnControl)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    NiceMock<MockServer> server;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    RecordingProtocolStream* control = new RecordingProtocolStream();
    ClientProxy1_12 proxy("client", control, &server, &events);
    control->clear();
    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 2);
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::data(
            proxy.getConnectionBinding(), transferId, 0, "x"),
        barrier::FileTransferRole::kSecondary));

    UInt8 code[4];
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    EXPECT_FALSE(proxy.parseMessage(code));
}

TEST(ClientProxyLifecycleTests, protocol112CommitAckFollowsServerOwnership)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    NiceMock<MockServer> server;
    server.m_events = &events;
    ON_CALL(events, newTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());
    EXPECT_CALL(events, addEvent(_)).Times(AnyNumber());

    RecordingProtocolStream* control = new RecordingProtocolStream();
    ClientProxy1_12 proxy("client", control, &server, &events);
    RecordingProtocolStream* bulk = new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(bulk));
    control->clear();
    bulk->clear();

    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 3);
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), transferId, 3),
        barrier::FileTransferRole::kSecondary));
    UInt8 code[4];
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    barrier::FileTransferFrame ack;
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    ASSERT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        bulk,
        barrier::FileTransferFrame::data(
            proxy.getConnectionBinding(), transferId, 0, "abc"),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, bulk->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.handleBulkMessage(code, bulk));
    EXPECT_EQ(0u, control->getSize());

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        bulk,
        barrier::FileTransferFrame::end(
            proxy.getConnectionBinding(), transferId, 3,
            "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, bulk->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.handleBulkMessage(code, bulk));

    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    EXPECT_EQ(barrier::FileTransferFrameType::kCommitAck, ack.type);
    EXPECT_EQ(transferId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    EXPECT_FALSE(proxy.testHasTransactionalReceive());

    for (int i = 0; i < 500 && server.testHasWriteToDropDirThread(); ++i) {
        ARCH->sleep(0.001);
        server.testCleanupWriteToDropDirThread();
    }
    EXPECT_FALSE(server.testHasWriteToDropDirThread());

    const UInt32 nextTransferId =
        barrier::FileTransferProtocol::makeTransferId(
            barrier::FileTransferRole::kSecondary, 4);
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), nextTransferId, 0),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    EXPECT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);
    EXPECT_EQ(nextTransferId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::cancel(
            proxy.getConnectionBinding(), nextTransferId,
            barrier::FileTransferReason::kCancelled),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    EXPECT_EQ(barrier::FileTransferFrameType::kCancelAck, ack.type);
    EXPECT_EQ(nextTransferId, ack.transferId);
    EXPECT_FALSE(proxy.testHasTransactionalReceive());
}

TEST(ClientProxyLifecycleTests, protocol112CancelDiscardsQueuedBulkBeforeReplacement)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    NiceMock<MockServer> server;
    server.m_events = &events;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    RecordingProtocolStream* control = new RecordingProtocolStream();
    ClientProxy1_12 proxy("client", control, &server, &events);
    RecordingProtocolStream* bulk = new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(bulk));
    control->clear();
    bulk->clear();

    const UInt32 cancelledId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 6);
    const UInt32 replacementId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 7);
    UInt8 code[4];
    barrier::FileTransferFrame ack;

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), cancelledId, 3),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    ASSERT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::cancel(
            proxy.getConnectionBinding(), cancelledId,
            barrier::FileTransferReason::kCancelled),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    ASSERT_EQ(barrier::FileTransferFrameType::kCancelAck, ack.type);
    EXPECT_FALSE(proxy.testHasTransactionalReceive());

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), replacementId, 3),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    ASSERT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);
    ASSERT_EQ(replacementId, proxy.testTransactionalReceiveId());

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        bulk,
        barrier::FileTransferFrame::data(
            proxy.getConnectionBinding(), cancelledId, 0, "abc"),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, bulk->read(code, sizeof(code)));
    EXPECT_TRUE(proxy.handleBulkMessage(code, bulk));
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        bulk,
        barrier::FileTransferFrame::end(
            proxy.getConnectionBinding(), cancelledId, 3,
            "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, bulk->read(code, sizeof(code)));
    EXPECT_TRUE(proxy.handleBulkMessage(code, bulk));
    EXPECT_EQ(replacementId, proxy.testTransactionalReceiveId());

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::cancel(
            proxy.getConnectionBinding(), replacementId,
            barrier::FileTransferReason::kCancelled),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    EXPECT_EQ(barrier::FileTransferFrameType::kCancelAck, ack.type);
    EXPECT_EQ(replacementId, ack.transferId);
    EXPECT_FALSE(proxy.testHasTransactionalReceive());
}

TEST(ClientProxyLifecycleTests, protocol112CancelAckWaitsForSpoolCleanupBeforeReplacement)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    NiceMock<MockServer> server;
    server.m_events = &events;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    RecordingProtocolStream* control = new RecordingProtocolStream();
    ClientProxy1_12 proxy("client", control, &server, &events);
    EventQueueTimer* receiveTimer =
        reinterpret_cast<EventQueueTimer*>(static_cast<uintptr_t>(13));
    EXPECT_CALL(events, newOneShotTimer(_, &proxy))
        .WillRepeatedly(Return(receiveTimer));
    RecordingProtocolStream* bulk = new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(bulk));
    control->clear();
    bulk->clear();

    WorkerExitGate workerExit;
    proxy.testSetTransactionalReceiveWorkerExitGate(workerExit.gate());
    const UInt32 cancelledId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 8);
    const UInt32 replacementId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 9);
    const UInt32 spoolSize = static_cast<UInt32>(
        FileChunk::kMemoryReceiveLimit + 1);
    UInt8 code[4];
    barrier::FileTransferFrame ack;

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), cancelledId, spoolSize),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    ASSERT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);
    ASSERT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    control->clear();

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::cancel(
            proxy.getConnectionBinding(), cancelledId,
            barrier::FileTransferReason::kCancelled),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    EXPECT_TRUE(proxy.testTransactionalCancelAckPending());
    EXPECT_TRUE(proxy.testTransactionalReceiveCleanupPending());
    EXPECT_EQ(0u, control->getSize());

    std::shared_ptr<barrier::BulkChannel> cancelledRoute =
        proxy.acquireBulkChannel();
    ASSERT_TRUE(cancelledRoute);
    cancelledRoute->close();
    proxy.detachBulkChannel();
    EXPECT_TRUE(proxy.testTransactionalCancelAckPending());
    EXPECT_TRUE(proxy.testTransactionalReceiveCleanupPending());

    RecordingProtocolStream* replacementBulk =
        new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(replacementBulk));
    replacementBulk->clear();

    proxy.testPollTransactionalFileReceive();
    EXPECT_TRUE(proxy.testTransactionalCancelAckPending());
    EXPECT_EQ(0u, control->getSize());

    workerExit.release();
    for (int i = 0;
         i < 1000 && proxy.testTransactionalReceiveCleanupPending(); ++i) {
        ARCH->sleep(0.001);
    }
    ASSERT_FALSE(proxy.testTransactionalReceiveCleanupPending());
    proxy.testPollTransactionalFileReceive();

    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    EXPECT_EQ(barrier::FileTransferFrameType::kCancelAck, ack.type);
    EXPECT_EQ(cancelledId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    EXPECT_FALSE(proxy.testTransactionalCancelAckPending());
    EXPECT_EQ(0u, control->getSize());

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), replacementId, 1),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    EXPECT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);
    EXPECT_EQ(replacementId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::cancel(
            proxy.getConnectionBinding(), replacementId,
            barrier::FileTransferReason::kCancelled),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    EXPECT_TRUE(proxy.parseMessage(code));
}

TEST(ClientProxyLifecycleTests, protocol112CancelCleanupDeadlineQuarantinesStuckWorker)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    NiceMock<MockServer> server;
    server.m_events = &events;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    RecordingProtocolStream* control = new RecordingProtocolStream();
    ClientProxy1_12 proxy("client", control, &server, &events);
    std::vector<double> cleanupDelays;
    EventQueueTimer* receiveTimer =
        reinterpret_cast<EventQueueTimer*>(static_cast<uintptr_t>(15));
    EXPECT_CALL(events, newOneShotTimer(_, &proxy))
        .Times(AnyNumber())
        .WillRepeatedly(Invoke(
            [&cleanupDelays, receiveTimer](double delay, void*) {
                cleanupDelays.push_back(delay);
                return receiveTimer;
            }));
    RecordingProtocolStream* bulk = new RecordingProtocolStream();
    ASSERT_TRUE(proxy.attachBulkChannel(bulk));
    control->clear();
    bulk->clear();

    WorkerExitGate workerExit;
    proxy.testSetTransactionalReceiveWorkerExitGate(workerExit.gate());
    const UInt32 cancelledId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 10);
    const UInt32 replacementId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 11);
    const UInt32 spoolSize = static_cast<UInt32>(
        FileChunk::kMemoryReceiveLimit + 1);
    UInt8 code[4];
    barrier::FileTransferFrame ack;

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), cancelledId, spoolSize),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    const std::uint64_t cancelledGeneration =
        proxy.testTransactionalReceiveGeneration();
    control->clear();
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::cancel(
            proxy.getConnectionBinding(), cancelledId,
            barrier::FileTransferReason::kCancelled),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));

    for (int i = 0;
         i < 64 && proxy.testTransactionalCancelAckPending(); ++i) {
        proxy.testFireTransactionalFileReceiveTimer();
    }

    EXPECT_FALSE(proxy.testTransactionalCancelAckPending());
    EXPECT_FALSE(proxy.testTransactionalReceiveCleanupPending());
    EXPECT_TRUE(proxy.testTransactionalRetiredWorkerPending());
    EXPECT_FALSE(proxy.testHasTransactionalFileReceiveTimer());
    ASSERT_GE(cleanupDelays.size(), 2u);
    EXPECT_LE(cleanupDelays.size(), 16u);
    EXPECT_GT(cleanupDelays.back(), cleanupDelays.front());
    const size_t terminalTimerCount = cleanupDelays.size();
    for (int i = 0; i < 64; ++i) {
        proxy.testFireTransactionalFileReceiveTimer();
    }
    EXPECT_EQ(terminalTimerCount, cleanupDelays.size());
    EXPECT_FALSE(proxy.testHasTransactionalFileReceiveTimer());
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, control, barrier::FileTransferRole::kSecondary,
        proxy.getConnectionBinding(), ack));
    EXPECT_EQ(barrier::FileTransferFrameType::kCancelAck, ack.type);
    EXPECT_EQ(cancelledId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    control->clear();

    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::start(
            proxy.getConnectionBinding(), replacementId, spoolSize),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    ASSERT_TRUE(proxy.parseMessage(code));
    const std::uint64_t replacementGeneration =
        proxy.testTransactionalReceiveGeneration();
    EXPECT_EQ(replacementId, proxy.testTransactionalReceiveId());
    EXPECT_NE(cancelledGeneration, replacementGeneration);
    workerExit.release();
    for (int i = 0;
         i < 1000 && proxy.testTransactionalRetiredWorkerPending(); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_FALSE(proxy.testTransactionalRetiredWorkerPending());
    EXPECT_EQ(replacementId, proxy.testTransactionalReceiveId());
    EXPECT_EQ(replacementGeneration,
              proxy.testTransactionalReceiveGeneration());

    control->clear();
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        control,
        barrier::FileTransferFrame::cancel(
            proxy.getConnectionBinding(), replacementId,
            barrier::FileTransferReason::kCancelled),
        barrier::FileTransferRole::kSecondary));
    ASSERT_EQ(4u, control->read(code, sizeof(code)));
    EXPECT_TRUE(proxy.parseMessage(code));
}

TEST(ClientProxyLifecycleTests, protocol112ReceiversAreIsolatedPerConnection)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    NiceMock<MockServer> server;
    server.m_events = &events;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    RecordingProtocolStream* firstControl = new RecordingProtocolStream();
    RecordingProtocolStream* secondControl = new RecordingProtocolStream();
    ClientProxy1_12 first("first", firstControl, &server, &events);
    ClientProxy1_12 second("second", secondControl, &server, &events);
    ASSERT_NE(first.getConnectionBinding(), second.getConnectionBinding());

    RecordingProtocolStream* firstBulk = new RecordingProtocolStream();
    RecordingProtocolStream* secondBulk = new RecordingProtocolStream();
    ASSERT_TRUE(first.attachBulkChannel(firstBulk));
    ASSERT_TRUE(second.attachBulkChannel(secondBulk));
    firstControl->clear();
    secondControl->clear();
    firstBulk->clear();
    secondBulk->clear();

    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 5);
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        firstControl,
        barrier::FileTransferFrame::start(
            first.getConnectionBinding(), transferId, 1),
        barrier::FileTransferRole::kSecondary));
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        secondControl,
        barrier::FileTransferFrame::start(
            second.getConnectionBinding(), transferId, 1),
        barrier::FileTransferRole::kSecondary));

    UInt8 code[4];
    ASSERT_EQ(4u, firstControl->read(code, sizeof(code)));
    ASSERT_TRUE(first.parseMessage(code));
    ASSERT_EQ(4u, secondControl->read(code, sizeof(code)));
    ASSERT_TRUE(second.parseMessage(code));

    EXPECT_TRUE(first.testHasTransactionalReceive());
    EXPECT_TRUE(second.testHasTransactionalReceive());
    EXPECT_EQ(transferId, first.testTransactionalReceiveId());
    EXPECT_EQ(transferId, second.testTransactionalReceiveId());
}

class CountingClipboard : public IClipboard {
public:
    bool empty() override { return true; }
    void add(EFormat, const String&) override { }
    bool open(Time) const override
    {
        ++openCount;
        return true;
    }
    void close() const override { ++closeCount; }
    Time getTime() const override { return 0; }
    bool has(EFormat format) const override
    {
        ++hasCount;
        return format == kText;
    }
    String get(EFormat format) const override
    {
        ++getCount;
        return format == kText ? String(300 * 1024, 'x') : String();
    }

    mutable int openCount = 0;
    mutable int closeCount = 0;
    mutable int hasCount = 0;
    mutable int getCount = 0;
};

class CleanupFailingClientProxy1_6 : public ClientProxy1_6 {
public:
    CleanupFailingClientProxy1_6(const std::string& name,
                                 barrier::IStream* stream,
                                 Server* server,
                                 IEventQueue* events) :
        ClientProxy1_6(name, stream, server, events)
    {
    }

    bool cleanupClipboardSendThread(bool) override
    {
        ++cleanupCalls;
        return false;
    }

    int cleanupCalls = 0;
};

class CleanupClearingClientProxy1_6 : public ClientProxy1_6 {
public:
    CleanupClearingClientProxy1_6(const std::string& name,
                                  barrier::IStream* stream,
                                  Server* server,
                                  IEventQueue* events) :
        ClientProxy1_6(name, stream, server, events)
    {
    }

    bool cleanupClipboardSendThread(bool) override
    {
        ++cleanupCalls;
        setClipboardDirty(kClipboardClipboard, false);
        return true;
    }

    int cleanupCalls = 0;
};

class CleanupEventuallyClientProxy1_9 : public ClientProxy1_9 {
public:
    CleanupEventuallyClientProxy1_9(const std::string& name,
                                    barrier::IStream* stream,
                                    Server* server,
                                    IEventQueue* events) :
        ClientProxy1_9(name, stream, server, events),
        cleanupReady(true)
    {
    }

    bool cleanupClipboardSendThread(bool cancel) override
    {
        if (!cleanupReady) {
            return false;
        }

        return ClientProxy1_9::cleanupClipboardSendThread(cancel);
    }

    bool cleanupReady;
};

class RecordingStream : public barrier::IStream {
public:
    void clear()
    {
        data.clear();
        offset = 0;
    }

    void close() override { closed = true; }
    UInt32 read(void* buffer, UInt32 count) override
    {
        const UInt32 available = getSize();
        const UInt32 copied = count < available ? count : available;
        if (copied != 0) {
            std::memcpy(buffer, data.data() + offset, copied);
            offset += copied;
        }
        return copied;
    }
    void write(const void* buffer, UInt32 count) override
    {
        const UInt8* bytes = static_cast<const UInt8*>(buffer);
        data.insert(data.end(), bytes, bytes + count);
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
        return const_cast<RecordingStream*>(this);
    }
    bool isReady() const override { return getSize() != 0; }
    UInt32 getSize() const override
    {
        return static_cast<UInt32>(data.size() - offset);
    }
    UInt32 getBufferedOutputSize() const override { return 0; }

    bool closed = false;

private:
    std::vector<UInt8> data;
    std::size_t offset = 0;
};

void queueFileMessage(RecordingStream* stream, UInt8 mark,
                      const std::string& content)
{
    std::vector<char> mutableContent(content.begin(), content.end());
    mutableContent.push_back('\0');
    FileChunk::send(stream, mark, mutableContent.data(), content.size());
}

}

TEST(ClientProxyLifecycleTests, protocol111AdvertisesSourceLeaseRevokeAck)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientProxyEvents clientProxyEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    clientProxyEvents.setEvents(&events);
    ON_CALL(events, forClientProxy()).WillByDefault(ReturnRef(clientProxyEvents));

    NiceMock<MockServer> server;
    NiceMock<MockStream>* legacyStream = new NiceMock<MockStream>();
    ON_CALL(*legacyStream, getEventTarget()).WillByDefault(Return(legacyStream));
    ON_CALL(*legacyStream, getBufferedOutputSize()).WillByDefault(Return(0));
    ClientProxy1_10 legacy("legacy", legacyStream, &server, &events);
    EXPECT_FALSE(legacy.supportsInputLeaseRevokeAck());

    RecordingStream* currentStream = new RecordingStream();
    ClientProxy1_11 current("current", currentStream, &server, &events);
    EXPECT_TRUE(current.supportsInputLeaseRevokeAck());

    currentStream->clear();
    current.requestInputLeaseRevoke(52, 41);
    UInt32 handoffSeqNum = 0;
    UInt32 inputEpoch = 0;
    ASSERT_TRUE(ProtocolUtil::readf(currentStream, kMsgCRevokeInput,
                                    &handoffSeqNum, &inputEpoch));
    EXPECT_EQ(52u, handoffSeqNum);
    EXPECT_EQ(41u, inputEpoch);

    currentStream->clear();
    ProtocolUtil::writef(currentStream, kMsgDRevokeInputAck + 4,
                         52, static_cast<UInt8>(1));
    const Event::Type readyType = clientProxyEvents.inputHandoffReady();
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke([&](const Event& event) {
        EXPECT_EQ(readyType, event.getType());
        EXPECT_EQ(current.getEventTarget(), event.getTarget());
        BaseClientProxy::InputHandoffReadyInfo* info =
            static_cast<BaseClientProxy::InputHandoffReadyInfo*>(
                event.getDataObject());
        ASSERT_NE(nullptr, info);
        EXPECT_EQ(52u, info->m_seqNum);
        EXPECT_TRUE(info->m_ready);
        Event::deleteData(event);
    }));
    EXPECT_TRUE(current.parseMessage(
        reinterpret_cast<const UInt8*>(kMsgDRevokeInputAck)));
}

TEST(ClientProxyLifecycleTests, clientProxy16RemovesClipboardSendingHandlerOnDestruction)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents, fileEvents, nextType);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));

    NiceMock<MockServer> server;

    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(clipboardEvents.clipboardSending(), _));

    {
        ClientProxy1_6 proxy("client", stream, &server, &events);
    }
}

TEST(ClientProxyLifecycleTests, clientProxy16RejectsMalformedCompletedClipboard)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);

    NiceMock<MockServer> server;
    RecordingStream* stream = new RecordingStream();
    ClientProxy1_6 proxy("client", stream, &server, &events);
    stream->clear();

    const ClipboardID id = kClipboardClipboard;
    const UInt32 sequence = 17;
    String expectedSize("1");
    String malformed("x");
    String empty;

    ProtocolUtil::writef(stream, kMsgDClipboard + 4, id, sequence,
                         kDataStart, &expectedSize);
    EXPECT_TRUE(proxy.recvClipboard());
    ProtocolUtil::writef(stream, kMsgDClipboard + 4, id, sequence,
                         kDataChunk, &malformed);
    EXPECT_TRUE(proxy.recvClipboard());
    ProtocolUtil::writef(stream, kMsgDClipboard + 4, id, sequence,
                         kDataEnd, &empty);
    EXPECT_FALSE(proxy.recvClipboard());
}

TEST(ClientProxyLifecycleTests, clientProxy17PublishesDecodedHandoffReadiness)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientProxyEvents clientProxyEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    clientProxyEvents.setEvents(&events);
    ON_CALL(events, forClientProxy()).WillByDefault(ReturnRef(clientProxyEvents));

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    const UInt8 payload[] = { 0x00, 0x00, 0x00, 0x2a, 0x01 };
    size_t offset = 0;
    ON_CALL(*stream, read(_, _)).WillByDefault(
        Invoke([&](void* buffer, UInt32 count) -> UInt32 {
            const UInt32 available = static_cast<UInt32>(sizeof(payload) - offset);
            const UInt32 copied = count < available ? count : available;
            std::memcpy(buffer, payload + offset, copied);
            offset += copied;
            return copied;
        }));

    NiceMock<MockServer> server;
    ClientProxy1_7 proxy("client", stream, &server, &events);
    const Event::Type readyType = clientProxyEvents.inputHandoffReady();
    EXPECT_CALL(events, addEvent(_)).WillOnce(
        Invoke([&](const Event& event) {
            EXPECT_EQ(readyType, event.getType());
            EXPECT_EQ(proxy.getEventTarget(), event.getTarget());
            EXPECT_NE(0u, event.getFlags() & Event::kDeliverImmediately);
            BaseClientProxy::InputHandoffReadyInfo* info =
                static_cast<BaseClientProxy::InputHandoffReadyInfo*>(
                    event.getDataObject());
            ASSERT_NE(nullptr, info);
            EXPECT_EQ(42u, info->m_seqNum);
            EXPECT_TRUE(info->m_ready);
            Event::deleteData(event);
        }));

    EXPECT_TRUE(proxy.parseMessage(
        reinterpret_cast<const UInt8*>(kMsgDEnterReady)));
}

TEST(ClientProxyLifecycleTests, clientProxy18FramesEveryInputWithEpochAndSequence)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientProxyEvents clientProxyEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    clientProxyEvents.setEvents(&events);
    ON_CALL(events, forClientProxy()).WillByDefault(ReturnRef(clientProxyEvents));

    RecordingStream* stream = new RecordingStream();
    NiceMock<MockServer> server;
    ClientProxy1_8 proxy("client", stream, &server, &events);

    stream->clear();
    proxy.enter(10, 20, 42, 0, false);
    SInt16 x = 0;
    SInt16 y = 0;
    UInt32 epoch = 0;
    UInt16 mask = 0;
    ASSERT_TRUE(ProtocolUtil::readf(stream, kMsgCEnter,
                                    &x, &y, &epoch, &mask));
    EXPECT_EQ(42u, epoch);

    stream->clear();
    proxy.mouseMove(30, 40);
    UInt32 sequence = 0;
    ASSERT_TRUE(ProtocolUtil::readf(stream, kMsgDMouseMove1_8,
                                    &epoch, &sequence, &x, &y));
    EXPECT_EQ(42u, epoch);
    EXPECT_EQ(1u, sequence);
    EXPECT_EQ(30, x);
    EXPECT_EQ(40, y);

    stream->clear();
    proxy.mouseDown(kButtonLeft);
    UInt8 button = 0;
    ASSERT_TRUE(ProtocolUtil::readf(stream, kMsgDMouseDown1_8,
                                    &epoch, &sequence, &button));
    EXPECT_EQ(42u, epoch);
    EXPECT_EQ(2u, sequence);
    EXPECT_EQ(kButtonLeft, button);

    stream->clear();
    proxy.keyDownBroadcast(7, KeyModifierControl, 9);
    UInt8 flags = 0;
    UInt16 key = 0;
    UInt16 keyMask = 0;
    UInt16 keyButton = 0;
    ASSERT_TRUE(ProtocolUtil::readf(stream, kMsgDKeyDown1_8,
                                    &epoch, &sequence, &flags,
                                    &key, &keyMask, &keyButton));
    EXPECT_EQ(42u, epoch);
    EXPECT_EQ(3u, sequence);
    EXPECT_EQ(kInputMessageBroadcast, flags);
    EXPECT_EQ(7u, key);
    EXPECT_EQ(KeyModifierControl, keyMask);
    EXPECT_EQ(9u, keyButton);

    stream->clear();
    proxy.mouseWheel(-120, 120);
    SInt16 xDelta = 0;
    SInt16 yDelta = 0;
    ASSERT_TRUE(ProtocolUtil::readf(stream, kMsgDMouseWheel1_8,
                                    &epoch, &sequence, &xDelta, &yDelta));
    EXPECT_EQ(42u, epoch);
    EXPECT_EQ(4u, sequence);
    EXPECT_EQ(-120, xDelta);
    EXPECT_EQ(120, yDelta);
}

TEST(ClientProxyLifecycleTests, clientProxy16CleanupRequestsCancelWithoutWaiting)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents, fileEvents, nextType);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    NiceMock<MockServer> server;
    ClientProxy1_6 proxy("client", stream, &server, &events);

    std::atomic<bool> started(false);
    std::atomic<bool> release(false);
    proxy.testSetClipboardSendThread(new Thread([&started, &release]() {
        started.store(true);
        while (!release.load()) {
        }
    }));
    while (!started.load()) {
        ARCH->sleep(0.001);
    }

    Stopwatch elapsed;
    EXPECT_FALSE(proxy.cleanupClipboardSendThread(true));
    EXPECT_LT(elapsed.getTime(), 0.1);

    release.store(true);
    for (int i = 0; i < 100 && !proxy.cleanupClipboardSendThread(false); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(proxy.cleanupClipboardSendThread(false));
}

TEST(ClientProxyLifecycleTests, clientProxy16KeepsClipboardDirtyWhileAsyncSendIsPending)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents, fileEvents, nextType);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));

    NiceMock<MockServer> server;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    ClientProxy1_6 proxy("client", stream, &server, &events);
    Clipboard clipboard = makeLargeClipboard();

    proxy.setClipboardDirty(kClipboardClipboard, true);
    proxy.setClipboard(kClipboardClipboard, &clipboard);

    EXPECT_TRUE(proxy.testClipboardDirty(kClipboardClipboard));
    for (int i = 0; i < 200 && !proxy.cleanupClipboardSendThread(true); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(proxy.cleanupClipboardSendThread(true));
}

TEST(ClientProxyLifecycleTests, clientProxy19RoutesLargeClipboardToBulkStream)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents, fileEvents, nextType);

    NiceMock<MockStream>* controlStream = new NiceMock<MockStream>();
    ON_CALL(*controlStream, getEventTarget()).WillByDefault(Return(controlStream));
    ON_CALL(*controlStream, getBufferedOutputSize()).WillByDefault(Return(0));

    NiceMock<MockStream>* bulkStream = new NiceMock<MockStream>();
    ON_CALL(*bulkStream, getEventTarget()).WillByDefault(Return(bulkStream));
    ON_CALL(*bulkStream, getBufferedOutputSize()).WillByDefault(Return(0));

    NiceMock<MockServer> server;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    ClientProxy1_9 proxy("client", controlStream, &server, &events);
    ASSERT_TRUE(proxy.attachBulkChannel(bulkStream));
    Clipboard clipboard = makeLargeClipboard();

    proxy.setClipboardDirty(kClipboardClipboard, true);
    proxy.setClipboard(kClipboardClipboard, &clipboard);

    EXPECT_TRUE(proxy.testHasClipboardBulkChannel());
    EXPECT_EQ(bulkStream, proxy.testClipboardSendStream());

    for (int i = 0; i < 200 && !proxy.cleanupClipboardSendThread(true); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(proxy.cleanupClipboardSendThread(true));
}

TEST(ClientProxyLifecycleTests, replacingBulkChannelInterruptsSenderPinnedToOldRoute)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);

    NiceMock<MockStream>* controlStream = new NiceMock<MockStream>();
    ON_CALL(*controlStream, getEventTarget()).WillByDefault(Return(controlStream));
    ON_CALL(*controlStream, getBufferedOutputSize()).WillByDefault(Return(0));
    NiceMock<MockStream>* firstBulkStream = new NiceMock<MockStream>();
    ON_CALL(*firstBulkStream, getEventTarget())
        .WillByDefault(Return(firstBulkStream));
    ON_CALL(*firstBulkStream, getBufferedOutputSize()).WillByDefault(Return(0));
    NiceMock<MockStream>* replacementBulkStream = new NiceMock<MockStream>();
    ON_CALL(*replacementBulkStream, getEventTarget())
        .WillByDefault(Return(replacementBulkStream));
    ON_CALL(*replacementBulkStream, getBufferedOutputSize())
        .WillByDefault(Return(0));

    NiceMock<MockServer> server;
    ClientProxy1_9 proxy("client", controlStream, &server, &events);
    ASSERT_TRUE(proxy.attachBulkChannel(firstBulkStream));
    std::shared_ptr<barrier::BulkChannel> firstChannel =
        proxy.acquireBulkChannel();
    ASSERT_TRUE(firstChannel);
    std::shared_ptr<StreamChunker> chunker(new StreamChunker());
    proxy.testSetClipboardBulkSender(chunker, firstChannel);

    ASSERT_TRUE(proxy.attachBulkChannel(replacementBulkStream));

    EXPECT_TRUE(chunker->testShouldInterrupt());
    EXPECT_NE(firstChannel, proxy.acquireBulkChannel());
}

TEST(ClientProxyLifecycleTests, staleBulkRouteCannotFeedFileReceiver)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);

    NiceMock<MockStream>* controlStream = new NiceMock<MockStream>();
    ON_CALL(*controlStream, getEventTarget()).WillByDefault(Return(controlStream));
    ON_CALL(*controlStream, getBufferedOutputSize()).WillByDefault(Return(0));
    NiceMock<MockStream>* staleStream = new NiceMock<MockStream>();
    ON_CALL(*staleStream, getEventTarget()).WillByDefault(Return(staleStream));
    ON_CALL(*staleStream, getBufferedOutputSize()).WillByDefault(Return(0));
    NiceMock<MockStream>* currentStream = new NiceMock<MockStream>();
    ON_CALL(*currentStream, getEventTarget()).WillByDefault(Return(currentStream));
    ON_CALL(*currentStream, getBufferedOutputSize()).WillByDefault(Return(0));

    NiceMock<MockServer> server;
    ClientProxy1_9 proxy("client", controlStream, &server, &events);
    ASSERT_TRUE(proxy.attachBulkChannel(staleStream));
    std::shared_ptr<barrier::BulkChannel> staleChannel =
        proxy.acquireBulkChannel();
    ASSERT_TRUE(staleChannel);
    ASSERT_TRUE(proxy.attachBulkChannel(currentStream));

    EXPECT_CALL(*staleStream, read(_, _)).Times(0);
    EXPECT_FALSE(proxy.handleBulkMessage(
        reinterpret_cast<const UInt8*>(kMsgDFileTransfer), staleStream));
    EXPECT_EQ(FileReceiveSession::kIdle,
              server.getFileReceiveSession().state());
    staleChannel.reset();
}

TEST(ClientProxyLifecycleTests, completedReceiveFencesRouteUntilPayloadIsConsumed)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);

    NiceMock<MockStream>* firstControl = new NiceMock<MockStream>();
    ON_CALL(*firstControl, getEventTarget()).WillByDefault(Return(firstControl));
    ON_CALL(*firstControl, getBufferedOutputSize()).WillByDefault(Return(0));
    NiceMock<MockStream>* secondControl = new NiceMock<MockStream>();
    ON_CALL(*secondControl, getEventTarget()).WillByDefault(Return(secondControl));
    ON_CALL(*secondControl, getBufferedOutputSize()).WillByDefault(Return(0));

    NiceMock<MockServer> server;
    ClientProxy1_5 first("first", firstControl, &server, &events);
    ClientProxy1_5 second("second", secondControl, &server, &events);
    // ClientProxy1_5 receives file data on the control stream and therefore
    // has no BulkChannel route. Source identity still fences competing peers.
    barrier::BulkChannel* firstRoute = NULL;
    barrier::BulkChannel* secondRoute = NULL;

    FileMessageStream start(kDataStart, "0");
    ASSERT_EQ(kStart, first.fileChunkReceived(&start, firstRoute));
    ASSERT_TRUE(server.canReceiveFileChunk(&first, firstRoute));

    FileMessageStream finish(kDataEnd, "");
    ASSERT_EQ(kFinish, first.fileChunkReceived(&finish, firstRoute));

    EXPECT_FALSE(server.canReceiveFileChunk(&first, firstRoute));
    EXPECT_FALSE(server.canReceiveFileChunk(&second, secondRoute));

    ASSERT_TRUE(server.testTakeCompletedFileTransfer());
    EXPECT_TRUE(server.canReceiveFileChunk(&second, secondRoute));
}

TEST(ClientProxyLifecycleTests, legacyControlFileTooLargeForMemoryIsDiscardedWithoutLosingControlRoute)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);

    NiceMock<MockStream>* controlStream = new NiceMock<MockStream>();
    ON_CALL(*controlStream, getEventTarget()).WillByDefault(Return(controlStream));
    ON_CALL(*controlStream, getBufferedOutputSize()).WillByDefault(Return(0));

    NiceMock<MockServer> server;
    ClientProxy1_5 proxy("legacy", controlStream, &server, &events);
    barrier::BulkChannel* controlRoute = NULL;

    FileMessageStream start(
        kDataStart, std::to_string(FileChunk::kMemoryReceiveLimit + 1));
    ASSERT_EQ(kStart, proxy.fileChunkReceived(&start, controlRoute));
    EXPECT_EQ(FileReceiveSession::kDiscarding,
              server.getFileReceiveSession().state());
    EXPECT_TRUE(server.canReceiveFileChunk(&proxy, controlRoute));

    FileMessageStream chunk(kDataChunk, "ignored");
    EXPECT_EQ(kNotFinish, proxy.fileChunkReceived(&chunk, controlRoute));
    EXPECT_TRUE(server.canReceiveFileChunk(&proxy, controlRoute));

    FileMessageStream finish(kDataEnd, "");
    EXPECT_EQ(kCancelled, proxy.fileChunkReceived(&finish, controlRoute));
    EXPECT_EQ(FileReceiveSession::kIdle,
              server.getFileReceiveSession().state());
    EXPECT_TRUE(server.canReceiveFileChunk(&proxy, controlRoute));
}

TEST(ClientProxyLifecycleTests, legacyWireFilePayloadIsConsumedWithoutServerReceiveState)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);

    FileMessageStream* controlStream = new FileMessageStream(kDataStart, "0");
    NiceMock<MockServer> server;
    ClientProxy1_9 proxy("legacy", controlStream, &server, &events);

    EXPECT_TRUE(proxy.parseMessage(
        reinterpret_cast<const UInt8*>(kMsgDFileTransfer)));
    EXPECT_EQ(FileReceiveSession::kIdle,
              server.getFileReceiveSession().state());
}

TEST(ClientProxyLifecycleTests, completionQueueFailureAbortsReceiveWithoutEscapingParser)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);

    NiceMock<MockStream>* controlStream = new NiceMock<MockStream>();
    ON_CALL(*controlStream, getEventTarget()).WillByDefault(Return(controlStream));
    ON_CALL(*controlStream, getBufferedOutputSize()).WillByDefault(Return(0));

    NiceMock<MockServer> server;
    ClientProxy1_5 proxy("legacy", controlStream, &server, &events);
    barrier::BulkChannel* controlRoute = NULL;

    FileMessageStream start(kDataStart, "0");
    ASSERT_EQ(kStart, proxy.fileChunkReceived(&start, controlRoute));
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke([](const Event&) {
        throw std::runtime_error("completion queue unavailable");
    }));

    FileMessageStream finish(kDataEnd, "");
    int result = kFinish;
    EXPECT_NO_THROW(result = proxy.fileChunkReceived(&finish, controlRoute));
    EXPECT_EQ(kError, result);
    EXPECT_EQ(FileReceiveSession::kIdle,
              server.getFileReceiveSession().state());
    EXPECT_TRUE(server.canReceiveFileChunk(&proxy, controlRoute));
}

TEST(ClientProxyLifecycleTests, fileReceiveProtocolErrorRejectsBulkRoute)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);

    NiceMock<MockStream>* controlStream = new NiceMock<MockStream>();
    ON_CALL(*controlStream, getEventTarget()).WillByDefault(Return(controlStream));
    ON_CALL(*controlStream, getBufferedOutputSize()).WillByDefault(Return(0));
    NiceMock<MockStream>* bulkStream = new NiceMock<MockStream>();
    ON_CALL(*bulkStream, getEventTarget()).WillByDefault(Return(bulkStream));
    ON_CALL(*bulkStream, getBufferedOutputSize()).WillByDefault(Return(0));

    NiceMock<MockServer> server;
    ClientProxy1_9 proxy("client", controlStream, &server, &events);
    ASSERT_TRUE(proxy.attachBulkChannel(bulkStream));

    EXPECT_FALSE(proxy.handleBulkMessage(
        reinterpret_cast<const UInt8*>(kMsgDFileTransfer), bulkStream));
    EXPECT_EQ(FileReceiveSession::kIdle,
              server.getFileReceiveSession().state());
}

TEST(ClientProxyLifecycleTests, legacyDuplicateFileStartIsConsumedWithoutServerSession)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);

    NiceMock<MockStream>* controlStream = new NiceMock<MockStream>();
    ON_CALL(*controlStream, getEventTarget()).WillByDefault(Return(controlStream));
    ON_CALL(*controlStream, getBufferedOutputSize()).WillByDefault(Return(0));
    RecordingStream* bulkStream = new RecordingStream();

    NiceMock<MockServer> server;
    ClientProxy1_9 proxy("client", controlStream, &server, &events);
    ASSERT_TRUE(proxy.attachBulkChannel(bulkStream));
    std::shared_ptr<barrier::BulkChannel> channel = proxy.acquireBulkChannel();
    ASSERT_TRUE(channel);
    bulkStream->clear();
    queueFileMessage(bulkStream, kDataStart, "6");
    queueFileMessage(bulkStream, kDataChunk, "abc");
    queueFileMessage(bulkStream, kDataStart, "1");

    channel->handleDataForTest();

    EXPECT_TRUE(channel->isActive());
    EXPECT_FALSE(bulkStream->closed);
    EXPECT_EQ(FileReceiveSession::kIdle,
              server.getFileReceiveSession().state());
    EXPECT_EQ(0u, server.getFileReceiveSession().expectedSize());
    EXPECT_TRUE(server.getFileReceiveSession().data().empty());
    EXPECT_TRUE(server.canReceiveFileChunk(&proxy, NULL));
}

TEST(ClientProxyLifecycleTests, clientProxy19KeepsLargeClipboardDirtyWithoutBulk)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);

    NiceMock<MockStream>* controlStream = new NiceMock<MockStream>();
    ON_CALL(*controlStream, getEventTarget()).WillByDefault(Return(controlStream));
    ON_CALL(*controlStream, getBufferedOutputSize()).WillByDefault(Return(0));
    NiceMock<MockServer> server;
    ClientProxy1_9 proxy("client", controlStream, &server, &events);
    Clipboard clipboard = makeLargeClipboard();

    proxy.setClipboardDirty(kClipboardClipboard, true);
    EXPECT_CALL(*controlStream, writeLowPriority(_, _)).Times(0);
    proxy.setClipboard(kClipboardClipboard, &clipboard);

    EXPECT_TRUE(proxy.testClipboardDirty(kClipboardClipboard));
    EXPECT_FALSE(proxy.testHasClipboardBulkChannel());
}

TEST(ClientProxyLifecycleTests, clientProxy19RejectsFileClipboardWithoutBulk)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);

    NiceMock<MockStream>* controlStream = new NiceMock<MockStream>();
    ON_CALL(*controlStream, getEventTarget()).WillByDefault(Return(controlStream));
    ON_CALL(*controlStream, getBufferedOutputSize()).WillByDefault(Return(0));
    NiceMock<MockServer> server;
    ClientProxy1_9 proxy("client", controlStream, &server, &events);
    Clipboard clipboard = makeFileClipboard();

    proxy.setClipboardDirty(kClipboardClipboard, true);
    EXPECT_CALL(*controlStream, writeLowPriority(_, _)).Times(0);
    proxy.setClipboard(kClipboardClipboard, &clipboard);

    EXPECT_FALSE(proxy.testClipboardDirty(kClipboardClipboard));
    EXPECT_FALSE(proxy.testHasClipboardBulkChannel());
}

TEST(ClientProxyLifecycleTests, clientProxy19RejectsFileClipboardWithBulkStream)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents, fileEvents, nextType);

    NiceMock<MockStream>* controlStream = new NiceMock<MockStream>();
    ON_CALL(*controlStream, getEventTarget()).WillByDefault(Return(controlStream));
    ON_CALL(*controlStream, getBufferedOutputSize()).WillByDefault(Return(0));

    NiceMock<MockStream>* bulkStream = new NiceMock<MockStream>();
    ON_CALL(*bulkStream, getEventTarget()).WillByDefault(Return(bulkStream));
    ON_CALL(*bulkStream, getBufferedOutputSize()).WillByDefault(Return(0));

    NiceMock<MockServer> server;
    ClientProxy1_9 proxy("client", controlStream, &server, &events);
    ASSERT_TRUE(proxy.attachBulkChannel(bulkStream));
    Clipboard clipboard = makeFileClipboard();
    ASSERT_TRUE(RemoteFileClipboard::containsFileList(clipboard));

    proxy.setClipboardDirty(kClipboardClipboard, true);
    EXPECT_CALL(events, addEvent(_)).Times(0);
    EXPECT_CALL(*controlStream, writeLowPriority(_, _)).Times(0);
    EXPECT_CALL(*bulkStream, writeLowPriority(_, _)).Times(0);
    proxy.setClipboard(kClipboardClipboard, &clipboard);
    EXPECT_FALSE(proxy.testClipboardDirty(kClipboardClipboard));
}

TEST(ClientProxyLifecycleTests, queuedFileClipboardRouteLossRedirtiesOnlyMatchingAttempt)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);

    NiceMock<MockStream>* controlStream = new NiceMock<MockStream>();
    ON_CALL(*controlStream, getEventTarget()).WillByDefault(Return(controlStream));
    ON_CALL(*controlStream, getBufferedOutputSize()).WillByDefault(Return(0));
    NiceMock<MockStream>* firstBulkStream = new NiceMock<MockStream>();
    ON_CALL(*firstBulkStream, getEventTarget()).WillByDefault(Return(firstBulkStream));
    ON_CALL(*firstBulkStream, getBufferedOutputSize()).WillByDefault(Return(0));
    NiceMock<MockStream>* replacementBulkStream = new NiceMock<MockStream>();
    ON_CALL(*replacementBulkStream, getEventTarget())
        .WillByDefault(Return(replacementBulkStream));
    ON_CALL(*replacementBulkStream, getBufferedOutputSize())
        .WillByDefault(Return(0));

    NiceMock<MockServer> server;
    ClientProxy1_12 proxy("client", controlStream, &server, &events);
    ASSERT_TRUE(proxy.attachBulkChannel(firstBulkStream));
    std::shared_ptr<barrier::BulkChannel> firstChannel =
        proxy.acquireBulkChannel();
    ASSERT_TRUE(firstChannel);

    std::vector<ClipboardChunk*> queuedChunks;
    EXPECT_CALL(events, addEvent(_)).Times(AnyNumber()).WillRepeatedly(
        Invoke([&clipboardEvents, &queuedChunks](const Event& event) {
            if (event.getType() == clipboardEvents.clipboardSending() &&
                event.getData() != nullptr) {
                queuedChunks.push_back(
                    static_cast<ClipboardChunk*>(event.getData()));
            }
        }));
    EXPECT_CALL(*controlStream, writeLowPriority(_, _)).Times(0);
    EXPECT_CALL(*firstBulkStream, writeLowPriority(_, _)).Times(0);

    Clipboard clipboard = makeFileClipboard();
    proxy.setClipboardDirty(kClipboardClipboard, true);
    proxy.setClipboard(kClipboardClipboard, &clipboard);
    ASSERT_EQ(3u, queuedChunks.size());
    EXPECT_FALSE(proxy.testClipboardDirty(kClipboardClipboard));

    firstChannel->close();
    proxy.testHandleClipboardSendingChunk(queuedChunks[0]);
    EXPECT_TRUE(proxy.testClipboardDirty(kClipboardClipboard));

    ASSERT_TRUE(proxy.attachBulkChannel(replacementBulkStream));
    ASSERT_EQ(6u, queuedChunks.size());
    EXPECT_FALSE(proxy.testClipboardDirty(kClipboardClipboard));

    proxy.testHandleClipboardSendingChunk(queuedChunks[1]);
    proxy.testHandleClipboardSendingChunk(queuedChunks[2]);
    EXPECT_FALSE(proxy.testClipboardDirty(kClipboardClipboard));

    EXPECT_CALL(*replacementBulkStream, writeLowPriority(_, _)).Times(3);
    for (std::size_t i = 3; i < queuedChunks.size(); ++i) {
        proxy.testHandleClipboardSendingChunk(queuedChunks[i]);
    }
    for (ClipboardChunk* chunk : queuedChunks) {
        delete chunk;
    }
}

TEST(ClientProxyLifecycleTests, replacementBulkRetriesAfterOldClipboardSenderStops)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));

    NiceMock<MockStream>* controlStream = new NiceMock<MockStream>();
    ON_CALL(*controlStream, getEventTarget()).WillByDefault(Return(controlStream));
    ON_CALL(*controlStream, getBufferedOutputSize()).WillByDefault(Return(0));
    NiceMock<MockStream>* bulkStream = new NiceMock<MockStream>();
    ON_CALL(*bulkStream, getEventTarget()).WillByDefault(Return(bulkStream));
    ON_CALL(*bulkStream, getBufferedOutputSize()).WillByDefault(Return(0));

    NiceMock<MockServer> server;
    std::vector<ClipboardChunk*> queuedChunks;
    CleanupEventuallyClientProxy1_9 proxy(
        "client", controlStream, &server, &events);
    Clipboard clipboard = makeLargeClipboard();
    proxy.setClipboardDirty(kClipboardClipboard, true);
    proxy.setClipboard(kClipboardClipboard, &clipboard);
    ASSERT_TRUE(proxy.testClipboardDirty(kClipboardClipboard));

    EXPECT_CALL(events, addEvent(_)).Times(AnyNumber()).WillRepeatedly(
        Invoke([&clipboardEvents, &queuedChunks](const Event& event) {
            if (event.getType() == clipboardEvents.clipboardSending() &&
                event.getData() != nullptr) {
                queuedChunks.push_back(
                    static_cast<ClipboardChunk*>(event.getData()));
            }
        }));
    proxy.cleanupReady = false;
    ASSERT_TRUE(proxy.attachBulkChannel(bulkStream));
    EXPECT_TRUE(proxy.testHasClipboardSendRetryTimer());
    EXPECT_TRUE(proxy.testClipboardDirty(kClipboardClipboard));

    proxy.cleanupReady = true;
    proxy.testRunClipboardSendRetry();

    for (int i = 0; i < 200 && proxy.testHasClipboardSendRetryTimer(); ++i) {
        ARCH->sleep(0.001);
        proxy.testRunClipboardSendRetry();
    }

    EXPECT_FALSE(proxy.testHasClipboardSendRetryTimer());
    EXPECT_FALSE(proxy.testClipboardDirty(kClipboardClipboard));
    EXPECT_GT(queuedChunks.size(), 3u);

    for (ClipboardChunk* chunk : queuedChunks) {
        delete chunk;
    }
}

TEST(ClientProxyLifecycleTests, clientProxy16ClearsClipboardDirtyAfterAsyncSendCompletes)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents, fileEvents, nextType);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));

    NiceMock<MockServer> server;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    ClientProxy1_6 proxy("client", stream, &server, &events);
    Clipboard clipboard = makeLargeClipboard();

    proxy.setClipboardDirty(kClipboardClipboard, true);
    proxy.setClipboard(kClipboardClipboard, &clipboard);
    for (int i = 0; i < 200 && !proxy.cleanupClipboardSendThread(false); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(proxy.cleanupClipboardSendThread(false));

    EXPECT_FALSE(proxy.testClipboardDirty(kClipboardClipboard));
}

TEST(ClientProxyLifecycleTests, clientProxy16CleanupFailureSkipsClipboardCopyAndMarshall)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents, fileEvents, nextType);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));

    NiceMock<MockServer> server;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    CleanupFailingClientProxy1_6 proxy("client", stream, &server, &events);
    CountingClipboard clipboard;

    proxy.setClipboardDirty(kClipboardClipboard, true);
    proxy.setClipboard(kClipboardClipboard, &clipboard);

    EXPECT_EQ(1, proxy.cleanupCalls);
    EXPECT_TRUE(proxy.testClipboardDirty(kClipboardClipboard));
    EXPECT_EQ(0, clipboard.openCount);
    EXPECT_EQ(0, clipboard.hasCount);
    EXPECT_EQ(0, clipboard.getCount);
    EXPECT_EQ(0, clipboard.closeCount);
}

TEST(ClientProxyLifecycleTests, clientProxy16CompletedAsyncSendDoesNotReplayClipboard)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents, fileEvents, nextType);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));

    NiceMock<MockServer> server;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    CleanupClearingClientProxy1_6 proxy("client", stream, &server, &events);
    CountingClipboard clipboard;

    proxy.setClipboardDirty(kClipboardClipboard, true);
    proxy.setClipboard(kClipboardClipboard, &clipboard);

    EXPECT_EQ(1, proxy.cleanupCalls);
    EXPECT_FALSE(proxy.testClipboardDirty(kClipboardClipboard));
    EXPECT_EQ(0, clipboard.openCount);
    EXPECT_EQ(0, clipboard.hasCount);
    EXPECT_EQ(0, clipboard.getCount);
    EXPECT_EQ(0, clipboard.closeCount);
}
