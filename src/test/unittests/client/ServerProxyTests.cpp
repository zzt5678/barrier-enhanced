#define BARRIER_TEST_ENV
#include "client/ServerProxy.h"

#include "client/Client.h"
#include "barrier/BulkChannel.h"
#include "barrier/Clipboard.h"
#include "barrier/ClipboardChunk.h"
#include "barrier/FileChunk.h"
#include "barrier/FileTransferProtocol.h"
#include "barrier/FileTransferSendState.h"
#include "barrier/TransferDigest.h"
#include "barrier/RemoteFileClipboard.h"
#include "barrier/ClientArgs.h"
#include "barrier/option_types.h"
#include "barrier/protocol_types.h"
#include "barrier/Screen.h"
#include "arch/Arch.h"
#include "base/Stopwatch.h"
#include "mt/Thread.h"
#include "net/ISocketFactory.h"
#include "net/NetworkAddress.h"

#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/io/MockStream.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::AtLeast;
using ::testing::DoubleEq;
using ::testing::Invoke;
using ::testing::Mock;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {

class DummySocketFactory : public ISocketFactory {
public:
    IDataSocket* create(IArchNetwork::EAddressFamily,
                        ConnectionSecurityLevel) const override
    {
        return NULL;
    }

    IListenSocket* createListen(IArchNetwork::EAddressFamily,
                                ConnectionSecurityLevel) const override
    {
        return NULL;
    }
};

class TestScreen : public barrier::Screen {
public:
    void* getEventTarget() const override
    {
        return const_cast<TestScreen*>(this);
    }
};

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
        const UInt32 copied = (std::min)(count, available);
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

    void clearOutput()
    {
        output.clear();
    }

private:
    std::vector<UInt8> input;
    std::size_t inputOffset = 0;
};

const std::string kTransactionalBinding =
    "00112233445566778899aabbccddeeff";

std::vector<UInt8> encodeTransactionalFrame(
    const barrier::FileTransferFrame& frame,
    barrier::FileTransferRole role)
{
    DuplexMemoryStream stream;
    EXPECT_TRUE(barrier::FileTransferProtocol::encode(
        &stream, frame, role));
    return stream.output;
}

ServerProxy::EResult dispatchTransactionalControlFrame(
    ServerProxy& proxy,
    DuplexMemoryStream& control,
    const barrier::FileTransferFrame& frame,
    barrier::FileTransferRole role)
{
    std::vector<UInt8> encoded = encodeTransactionalFrame(frame, role);
    EXPECT_GE(encoded.size(), 4u);
    control.queueInput(std::vector<UInt8>(encoded.begin() + 4, encoded.end()));
    return proxy.parseMessage(encoded.data());
}

bool dispatchTransactionalBulkFrame(
    ServerProxy& proxy,
    DuplexMemoryStream& bulk,
    const barrier::FileTransferFrame& frame,
    barrier::FileTransferRole role)
{
    std::vector<UInt8> encoded = encodeTransactionalFrame(frame, role);
    EXPECT_GE(encoded.size(), 4u);
    bulk.queueInput(std::vector<UInt8>(encoded.begin() + 4, encoded.end()));
    return proxy.handleBulkMessage(encoded.data(), &bulk);
}

barrier::FileTransferFrame decodeTransactionalOutput(
    const std::vector<UInt8>& encoded,
    barrier::FileTransferRole role)
{
    EXPECT_GE(encoded.size(), 4u);
    DuplexMemoryStream stream;
    stream.queueInput(std::vector<UInt8>(encoded.begin() + 4, encoded.end()));
    barrier::FileTransferFrame frame;
    EXPECT_TRUE(barrier::FileTransferProtocol::decode(
        encoded.data(), &stream, role, kTransactionalBinding, frame));
    return frame;
}

std::string digestFor(const std::string& payload)
{
    barrier::TransferDigest digest;
    EXPECT_TRUE(digest.isReady());
    EXPECT_TRUE(digest.update(payload.data(), payload.size()));
    std::string encoded;
    EXPECT_TRUE(digest.finish(encoded));
    return encoded;
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

std::vector<UInt8> encodeFileMessage(UInt8 mark, const std::string& content)
{
    DuplexMemoryStream stream;
    std::vector<char> mutableContent(content.begin(), content.end());
    mutableContent.push_back('\0');
    FileChunk::send(&stream, mark, mutableContent.data(), content.size());
    return stream.output;
}

std::vector<UInt8> encodeFilePayload(UInt8 mark, const std::string& content)
{
    std::vector<UInt8> message = encodeFileMessage(mark, content);
    message.erase(message.begin(), message.begin() + 4);
    return message;
}

class RecordingMouseClient : public Client {
public:
    RecordingMouseClient(IEventQueue* events, barrier::Screen* screen) :
        Client(events, "client", NetworkAddress(), new DummySocketFactory(),
               screen, ClientArgs())
    {
    }

    void mouseMove(SInt32 x, SInt32 y) override
    {
        moves.push_back(std::make_pair(x, y));
    }

    std::vector<std::pair<SInt32, SInt32> > moves;
};

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

Clipboard makeFileClipboard()
{
    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.sessionId = "00000000000000000000000000000021";
    payload.paths.push_back(barrier::fs::u8path("C:/small.txt"));

    Clipboard clipboard;
    clipboard.open(40);
    clipboard.empty();
    clipboard.add(IClipboard::kFileList,
                  RemoteFileClipboard::serialize(payload));
    clipboard.close();
    return clipboard;
}

class CleanupFailingServerProxy : public ServerProxy {
public:
    CleanupFailingServerProxy(Client* client, barrier::IStream* stream, IEventQueue* events) :
        ServerProxy(client, stream, events)
    {
    }

    bool cleanupClipboardSendThread(bool) override
    {
        ++cleanupCalls;
        return false;
    }

    int cleanupCalls = 0;
};

void consumeClientFailureEvent(const Event& event)
{
    delete static_cast<Client::FailInfo*>(event.getData());
}

void setServerProxyEventDefaults(MockEventQueue& events,
                                 IStreamEvents& streamEvents,
                                 ClipboardEvents& clipboardEvents,
                                 FileEvents& fileEvents)
{
    streamEvents.setEvents(&events);
    clipboardEvents.setEvents(&events);
    fileEvents.setEvents(&events);

    ON_CALL(events, forIStream()).WillByDefault(ReturnRef(streamEvents));
    ON_CALL(events, forClipboard()).WillByDefault(ReturnRef(clipboardEvents));
    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, registerTypeOnce(_, _)).WillByDefault(Return(100));
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));
    ON_CALL(events, newTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(2)));
}

void setServerProxyClientEventDefaults(MockEventQueue& events,
                                       IStreamEvents& streamEvents,
                                       ClipboardEvents& clipboardEvents,
                                       FileEvents& fileEvents,
                                       ClientEvents& clientEvents,
                                       IScreenEvents& screenEvents)
{
    setServerProxyEventDefaults(events, streamEvents, clipboardEvents, fileEvents);
    clientEvents.setEvents(&events);
    screenEvents.setEvents(&events);
    ON_CALL(events, forClient()).WillByDefault(ReturnRef(clientEvents));
    ON_CALL(events, forIScreen()).WillByDefault(ReturnRef(screenEvents));
}

}

TEST(ServerProxyTests, hasCompleteOptionPairs_acceptsEmptyAndPairedOptions)
{
    OptionsList empty;
    EXPECT_TRUE(ServerProxy::hasCompleteOptionPairs(empty));

    OptionsList paired;
    paired.push_back(kOptionHeartbeat);
    paired.push_back(5000);
    EXPECT_TRUE(ServerProxy::hasCompleteOptionPairs(paired));
}

TEST(ServerProxyTests, hasCompleteOptionPairs_rejectsOddSizedOptions)
{
    OptionsList malformed;
    malformed.push_back(kOptionHeartbeat);

    EXPECT_FALSE(ServerProxy::hasCompleteOptionPairs(malformed));
}

TEST(ServerProxyTests, inputParserYieldsAndReschedulesAfterBoundedBatch)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    setServerProxyEventDefaults(events, streamEvents, clipboardEvents, fileEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));

    const std::vector<UInt8> messages(65 * 4, 0);
    std::vector<UInt8> codes = messages;
    for (size_t i = 0; i < 65; ++i) {
        std::memcpy(&codes[i * 4], kMsgCNoop, 4);
    }
    size_t offset = 0;
    ON_CALL(stream, read(_, _)).WillByDefault(
        Invoke([&](void* buffer, UInt32 count) -> UInt32 {
            if (offset >= codes.size() || count < 4) {
                return 0;
            }
            std::memcpy(buffer, &codes[offset], 4);
            offset += 4;
            return 4;
        }));
    ON_CALL(stream, getSize()).WillByDefault(
        Invoke([&]() -> UInt32 { return offset < codes.size() ? 4 : 0; }));

    int rescheduled = 0;
    EXPECT_CALL(events, addEvent(_)).Times(AnyNumber()).WillRepeatedly(
        Invoke([&](const Event& event) {
            if (event.getType() == streamEvents.inputReady()) {
                ++rescheduled;
            }
        }));

    ServerProxy proxy(reinterpret_cast<Client*>(1), &stream, &events);
    proxy.handleDataForTest();

    EXPECT_GT(offset, 0u);
    EXPECT_LE(offset, 64u * 4u);
    EXPECT_LT(offset, codes.size());
    EXPECT_EQ(1, rescheduled);

    for (int attempt = 0; attempt < 65 && offset < codes.size(); ++attempt) {
        proxy.handleDataForTest();
    }
    EXPECT_EQ(codes.size(), offset);
    EXPECT_GE(rescheduled, 1);
}

TEST(ServerProxyTests, clipboardCleanupRequestsCancelWithoutWaiting)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    setServerProxyEventDefaults(events, streamEvents, clipboardEvents, fileEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));
    ServerProxy proxy(reinterpret_cast<Client*>(1), &stream, &events);

    std::atomic<bool> started(false);
    std::atomic<bool> release(false);
    proxy.m_clipboardSendThread = new Thread([&started, &release]() {
        started.store(true);
        while (!release.load()) {
        }
    });
    while (!started.load()) {
        ARCH->sleep(0.001);
    }

    Stopwatch elapsed;
    EXPECT_FALSE(proxy.cleanupClipboardSendThread(true));
    EXPECT_LT(elapsed.getTime(), 0.1);

    release.store(true);
    for (int i = 0; i < 100 && proxy.m_clipboardSendThread != NULL; ++i) {
        proxy.cleanupClipboardSendThread(false);
        ARCH->sleep(0.001);
    }
    EXPECT_EQ(NULL, proxy.m_clipboardSendThread);
}

TEST(ServerProxyTests, setKeepAliveRateAllowsTransientSchedulingPause)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    setServerProxyEventDefaults(events, streamEvents, clipboardEvents, fileEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));
    ON_CALL(stream, isReady()).WillByDefault(Return(false));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(0));

    ServerProxy proxy(reinterpret_cast<Client*>(1), &stream, &events);
    Mock::VerifyAndClearExpectations(&events);

    EXPECT_CALL(events, newOneShotTimer(DoubleEq(5.0), _))
        .WillOnce(Return(reinterpret_cast<EventQueueTimer*>(2)));

    proxy.setKeepAliveRate(1.0);
}

TEST(ServerProxyTests, mouseMoveCompressionIsDisabledForImmediateDeliveryModes)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    setServerProxyEventDefaults(events, streamEvents, clipboardEvents, fileEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));
    ON_CALL(stream, isReady()).WillByDefault(Return(true));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(0));

    ServerProxy proxy(reinterpret_cast<Client*>(1), &stream, &events);

    EXPECT_TRUE(proxy.shouldCompressMouseMoves());

    proxy.m_lowLatencyMode = true;
    EXPECT_FALSE(proxy.shouldCompressMouseMoves());

    proxy.m_lowLatencyMode = false;
    proxy.m_nestedRemoteMode = true;
    EXPECT_FALSE(proxy.shouldCompressMouseMoves());

    proxy.m_lowLatencyMode = true;
    EXPECT_FALSE(proxy.shouldCompressMouseMoves());
}

TEST(ServerProxyTests, lowLatencyMouseMovesDeliverEveryCoordinateInBackloggedBatch)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents, clientEvents, screenEvents);

    TestScreen screen;
    RecordingMouseClient client(&events, &screen);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));
    ON_CALL(stream, isReady()).WillByDefault(Return(true));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(0));

    const UInt8 coordinates[] = { 0, 10, 0, 20, 0, 11, 0, 21 };
    size_t offset = 0;
    ON_CALL(stream, read(_, _)).WillByDefault(
        Invoke([&](void* buffer, UInt32 count) -> UInt32 {
            const size_t remaining = sizeof(coordinates) - offset;
            const UInt32 copied = static_cast<UInt32>(
                std::min<size_t>(remaining, count));
            std::memcpy(buffer, coordinates + offset, copied);
            offset += copied;
            return copied;
        }));

    ServerProxy proxy(&client, &stream, &events);
    proxy.m_lowLatencyMode = true;
    proxy.m_nestedRemoteMode = true;
    proxy.m_inputActive = true;
    proxy.m_inputFrameAccepted = true;

    proxy.mouseMove();
    proxy.mouseMove();

    ASSERT_EQ(2u, client.moves.size());
    EXPECT_EQ(10, client.moves[0].first);
    EXPECT_EQ(20, client.moves[0].second);
    EXPECT_EQ(11, client.moves[1].first);
    EXPECT_EQ(21, client.moves[1].second);
}

TEST(ServerProxyTests, protocol19DefersLargeClipboardWithoutControlFallback)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents, clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    NiceMock<MockStream> controlStream;
    ON_CALL(controlStream, getEventTarget()).WillByDefault(Return(&controlStream));
    ON_CALL(controlStream, getBufferedOutputSize()).WillByDefault(Return(0u));
    ServerProxy proxy(&client, &controlStream, &events, 9);

    Clipboard clipboard;
    clipboard.open(40);
    clipboard.empty();
    clipboard.add(IClipboard::kText, std::string(300 * 1024, 'x'));
    clipboard.close();

    EXPECT_CALL(controlStream, write(_, _)).Times(0);
    EXPECT_CALL(controlStream, writeLowPriority(_, _)).Times(0);
    EXPECT_EQ(ServerProxy::kClipboardSendFailed,
              proxy.onClipboardChanged(kClipboardClipboard, &clipboard));
}

TEST(ServerProxyTests, protocol19RejectsFileClipboardEvenWithBulkRoute)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents, clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    NiceMock<MockStream> controlStream;
    ON_CALL(controlStream, getEventTarget()).WillByDefault(Return(&controlStream));
    ON_CALL(controlStream, getBufferedOutputSize()).WillByDefault(Return(0u));
    NiceMock<MockStream>* bulkStream = new NiceMock<MockStream>();
    ON_CALL(*bulkStream, getEventTarget()).WillByDefault(Return(bulkStream));
    ON_CALL(*bulkStream, getBufferedOutputSize()).WillByDefault(Return(0u));
    client.testAttachBulkStream(bulkStream);
    ServerProxy proxy(&client, &controlStream, &events, 9);
    Clipboard clipboard = makeFileClipboard();

    EXPECT_CALL(controlStream, write(_, _)).Times(0);
    EXPECT_CALL(controlStream, writeLowPriority(_, _)).Times(0);
    EXPECT_CALL(*bulkStream, write(_, _)).Times(0);
    EXPECT_CALL(*bulkStream, writeLowPriority(_, _)).Times(0);
    EXPECT_EQ(ServerProxy::kClipboardSendFailed,
              proxy.onClipboardChanged(kClipboardClipboard, &clipboard));
}

TEST(ServerProxyTests, ordinaryMessageResetsKeepAliveAlarm)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    setServerProxyEventDefaults(events, streamEvents, clipboardEvents, fileEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));
    ON_CALL(stream, isReady()).WillByDefault(Return(false));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(0));

    int readCount = 0;
    ON_CALL(stream, read(_, _))
        .WillByDefault(Invoke([&readCount](void* buffer, UInt32 n) -> UInt32 {
            if (readCount++ == 0) {
                memcpy(buffer, kMsgCNoop, 4);
                return 4;
            }
            return 0;
        }));

    ServerProxy proxy(reinterpret_cast<Client*>(1), &stream, &events);
    proxy.m_keepAliveAlarmDeferrals = 2;
    proxy.m_keepAliveMissedAlarms = 2;
    Mock::VerifyAndClearExpectations(&events);

    EXPECT_CALL(events, deleteTimer(_)).Times(0);
    EXPECT_CALL(events, newOneShotTimer(_, _)).Times(0);

    proxy.handleData(Event(), NULL);

    EXPECT_EQ(0u, proxy.m_keepAliveAlarmDeferrals);
    EXPECT_EQ(0u, proxy.m_keepAliveMissedAlarms);
    Mock::VerifyAndClearExpectations(&events);
}

TEST(ServerProxyTests, keepAliveAlarmDefersWhenStreamHasPendingInput)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    setServerProxyEventDefaults(events, streamEvents, clipboardEvents, fileEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));
    ON_CALL(stream, isReady()).WillByDefault(Return(true));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(0));

    ServerProxy proxy(reinterpret_cast<Client*>(1), &stream, &events);

    proxy.handleKeepAliveAlarm(Event(), NULL);

    EXPECT_EQ(1u, proxy.m_keepAliveAlarmDeferrals);
}

TEST(ServerProxyTests, keepAliveAlarmResetsDeferralBudgetWhenOutputProgresses)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    setServerProxyEventDefaults(events, streamEvents, clipboardEvents, fileEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));
    ON_CALL(stream, isReady()).WillByDefault(Return(false));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(64u));

    ServerProxy proxy(reinterpret_cast<Client*>(1), &stream, &events);
    proxy.m_keepAliveAlarmDeferrals = 7;
    proxy.m_lastKeepAlivePendingInput = false;
    proxy.m_lastKeepAliveBufferedOutput = 128;

    proxy.handleKeepAliveAlarm(Event(), NULL);

    EXPECT_EQ(1u, proxy.m_keepAliveAlarmDeferrals);
    EXPECT_FALSE(proxy.m_lastKeepAlivePendingInput);
    EXPECT_EQ(64u, proxy.m_lastKeepAliveBufferedOutput);
}

TEST(ServerProxyTests, keepAliveAlarmDoesNotResetDeferralBudgetWhenOutputOnlyGrows)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    setServerProxyEventDefaults(events, streamEvents, clipboardEvents, fileEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));
    ON_CALL(stream, isReady()).WillByDefault(Return(false));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(128u));

    ServerProxy proxy(reinterpret_cast<Client*>(1), &stream, &events);
    proxy.m_keepAliveAlarmDeferrals = 6;
    proxy.m_lastKeepAlivePendingInput = false;
    proxy.m_lastKeepAliveBufferedOutput = 64;

    proxy.handleKeepAliveAlarm(Event(), NULL);

    EXPECT_EQ(7u, proxy.m_keepAliveAlarmDeferrals);
    EXPECT_FALSE(proxy.m_lastKeepAlivePendingInput);
    EXPECT_EQ(128u, proxy.m_lastKeepAliveBufferedOutput);
}

TEST(ServerProxyTests, keepAliveAlarmResetsDeferralBudgetWhenInputAppears)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    setServerProxyEventDefaults(events, streamEvents, clipboardEvents, fileEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));
    ON_CALL(stream, isReady()).WillByDefault(Return(true));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(0));

    ServerProxy proxy(reinterpret_cast<Client*>(1), &stream, &events);
    proxy.m_keepAliveAlarmDeferrals = 8;
    proxy.m_lastKeepAlivePendingInput = false;

    proxy.handleKeepAliveAlarm(Event(), NULL);

    EXPECT_EQ(1u, proxy.m_keepAliveAlarmDeferrals);
    EXPECT_TRUE(proxy.m_lastKeepAlivePendingInput);
}

TEST(ServerProxyTests, keepAliveAlarmResetsDeferralBudgetWhenOutputProgressesAtCap)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    setServerProxyEventDefaults(events, streamEvents, clipboardEvents, fileEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));
    ON_CALL(stream, isReady()).WillByDefault(Return(false));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(4096u));

    ServerProxy proxy(reinterpret_cast<Client*>(1), &stream, &events);
    proxy.m_keepAliveAlarmDeferrals = 8;
    proxy.m_lastKeepAlivePendingInput = false;
    proxy.m_lastKeepAliveBufferedOutput = 8192;

    proxy.handleKeepAliveAlarm(Event(), NULL);

    EXPECT_EQ(1u, proxy.m_keepAliveAlarmDeferrals);
    EXPECT_EQ(4096u, proxy.m_lastKeepAliveBufferedOutput);
}

TEST(ServerProxyTests, keepAliveAlarmDisconnectsWhenPendingOutputStallsAtDeferralCap)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents, clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));
    ON_CALL(stream, isReady()).WillByDefault(Return(false));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(4096u));

    ServerProxy proxy(&client, &stream, &events);
    proxy.m_keepAliveAlarmDeferrals = 8;
    proxy.m_lastKeepAlivePendingInput = false;
    proxy.m_lastKeepAliveBufferedOutput = 4096;
    Mock::VerifyAndClearExpectations(&events);

    EXPECT_CALL(events, addEvent(_))
        .Times(AtLeast(1))
        .WillRepeatedly(Invoke(consumeClientFailureEvent));

    proxy.handleKeepAliveAlarm(Event(), NULL);

    EXPECT_EQ(8u, proxy.m_keepAliveAlarmDeferrals);
}

TEST(ServerProxyTests, keepAliveAlarmProbesIdleServerBeforeDisconnecting)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents, clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));
    ON_CALL(stream, isReady()).WillByDefault(Return(false));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(0));

    ServerProxy proxy(&client, &stream, &events);
    proxy.m_keepAliveAlarm = 0.0;
    Mock::VerifyAndClearExpectations(&events);

    EXPECT_CALL(events, addEvent(_)).Times(0);
    EXPECT_CALL(stream, write(_, _))
        .WillOnce(Invoke([](const void* buffer, UInt32 size) {
            EXPECT_EQ(4u, size);
            EXPECT_EQ(0, memcmp(buffer, kMsgCKeepAlive, 4));
        }));

    proxy.handleKeepAliveAlarm(Event(), NULL);

    EXPECT_EQ(1u, proxy.m_keepAliveMissedAlarms);
}

TEST(ServerProxyTests, keepAliveAlarmDisconnectsAfterRepeatedIdleMisses)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents, clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));
    ON_CALL(stream, isReady()).WillByDefault(Return(false));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(0));

    ServerProxy proxy(&client, &stream, &events);
    proxy.m_keepAliveAlarm = 0.0;
    proxy.m_keepAliveMissedAlarms = 3;
    Mock::VerifyAndClearExpectations(&events);

    EXPECT_CALL(events, addEvent(_))
        .Times(AtLeast(1))
        .WillRepeatedly(Invoke(consumeClientFailureEvent));
    EXPECT_CALL(stream, write(_, _)).Times(0);

    proxy.handleKeepAliveAlarm(Event(), NULL);
}

TEST(ServerProxyTests, largeClipboardUsesAsyncSenderAndCanBeReaped)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents, clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(0u));
    ON_CALL(events, getQueuedEventCount()).WillByDefault(Return(0u));

    std::vector<UInt8> clipboardMarks;
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&clipboardEvents, &clipboardMarks](const Event& event) {
            if (event.getType() == clipboardEvents.clipboardSending() &&
                event.getData() != NULL) {
                ClipboardChunk* chunk = static_cast<ClipboardChunk*>(event.getData());
                clipboardMarks.push_back(static_cast<UInt8>(chunk->m_chunk[5]));
            }
            Event::deleteData(event);
        }));

    Clipboard clipboard;
    clipboard.open(40);
    clipboard.empty();
    clipboard.add(IClipboard::kText, std::string(300 * 1024, 'x'));
    clipboard.close();

    ServerProxy proxy(&client, &stream, &events, 8);
    EXPECT_EQ(ServerProxy::kClipboardSendPending,
              proxy.onClipboardChanged(kClipboardClipboard, &clipboard));

    bool succeeded = false;
    bool reaped = false;
    for (int i = 0; i < 50; ++i) {
        if (proxy.reapClipboardSendResult(kClipboardClipboard, succeeded)) {
            reaped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(reaped);
    EXPECT_TRUE(succeeded);
    ASSERT_GE(clipboardMarks.size(), 3u);
    EXPECT_EQ(kDataStart, clipboardMarks.front());
    EXPECT_EQ(kDataEnd, clipboardMarks.back());
}

TEST(ServerProxyTests, smallFileClipboardMetadataUsesBulkStream)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents, clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());

    NiceMock<MockStream> controlStream;
    ON_CALL(controlStream, getEventTarget()).WillByDefault(Return(&controlStream));
    ON_CALL(controlStream, getBufferedOutputSize()).WillByDefault(Return(0u));

    NiceMock<MockStream>* bulkStream = new NiceMock<MockStream>();
    ON_CALL(*bulkStream, getEventTarget()).WillByDefault(Return(bulkStream));
    ON_CALL(*bulkStream, getBufferedOutputSize()).WillByDefault(Return(0u));
    client.testAttachBulkStream(bulkStream);

    ServerProxy proxy(&client, &controlStream, &events);
    Clipboard clipboard = makeFileClipboard();
    ASSERT_TRUE(RemoteFileClipboard::containsFileList(clipboard));
    std::vector<ClipboardChunk*> queuedChunks;
    EXPECT_CALL(events, addEvent(_)).Times(AnyNumber()).WillRepeatedly(
        Invoke([&clipboardEvents, &queuedChunks](const Event& event) {
            if (event.getType() == clipboardEvents.clipboardSending() &&
                event.getData() != nullptr) {
                queuedChunks.push_back(static_cast<ClipboardChunk*>(event.getData()));
            }
        }));
    EXPECT_CALL(controlStream, writeLowPriority(_, _)).Times(0);
    EXPECT_CALL(*bulkStream, writeLowPriority(_, _)).Times(AtLeast(1));
    EXPECT_EQ(ServerProxy::kClipboardSendQueued,
              proxy.onClipboardChanged(kClipboardClipboard, &clipboard));
    ASSERT_EQ(3u, queuedChunks.size());
    for (ClipboardChunk* chunk : queuedChunks) {
        EXPECT_EQ(bulkStream, chunk->getSendStream(nullptr));
        ClipboardChunk::send(chunk->getSendStream(nullptr), chunk);
        delete chunk;
    }
}

TEST(ServerProxyTests, queuedFileClipboardRouteLossMarksCurrentRevisionUnsent)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);
    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    NiceMock<MockStream> controlStream;
    ON_CALL(controlStream, getEventTarget()).WillByDefault(Return(&controlStream));
    ON_CALL(controlStream, getBufferedOutputSize()).WillByDefault(Return(0u));
    NiceMock<MockStream>* bulkStream = new NiceMock<MockStream>();
    ON_CALL(*bulkStream, getEventTarget()).WillByDefault(Return(bulkStream));
    ON_CALL(*bulkStream, getBufferedOutputSize()).WillByDefault(Return(0u));
    client.testAttachBulkStream(bulkStream);
    std::shared_ptr<barrier::BulkChannel> channel = client.acquireBulkChannel();
    ASSERT_TRUE(channel);

    ServerProxy proxy(&client, &controlStream, &events);
    Clipboard clipboard = makeFileClipboard();
    std::vector<ClipboardChunk*> queuedChunks;
    EXPECT_CALL(events, addEvent(_)).Times(AnyNumber()).WillRepeatedly(
        Invoke([&clipboardEvents, &queuedChunks](const Event& event) {
            if (event.getType() == clipboardEvents.clipboardSending() &&
                event.getData() != nullptr) {
                queuedChunks.push_back(
                    static_cast<ClipboardChunk*>(event.getData()));
            }
        }));
    EXPECT_CALL(controlStream, writeLowPriority(_, _)).Times(0);
    EXPECT_CALL(*bulkStream, writeLowPriority(_, _)).Times(0);

    EXPECT_EQ(ServerProxy::kClipboardSendQueued,
              proxy.onClipboardChanged(kClipboardClipboard, &clipboard));
    ASSERT_EQ(3u, queuedChunks.size());
    client.testSetClipboardOwnership(kClipboardClipboard, true);
    client.testSetClipboardSent(kClipboardClipboard, true);

    channel->close();
    proxy.testHandleClipboardSendingChunk(queuedChunks[0]);

    EXPECT_FALSE(client.testClipboardSent(kClipboardClipboard));
    EXPECT_TRUE(client.testClipboardRetryPending(kClipboardClipboard));
    for (ClipboardChunk* chunk : queuedChunks) {
        delete chunk;
    }
}

TEST(ServerProxyTests, malformedLegacyFilePayloadIsRejectedWithoutReceiveState)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    NiceMock<MockStream> controlStream;
    ON_CALL(controlStream, getEventTarget()).WillByDefault(Return(&controlStream));
    ON_CALL(controlStream, getBufferedOutputSize()).WillByDefault(Return(0u));
    NiceMock<MockStream>* bulkStream = new NiceMock<MockStream>();
    ON_CALL(*bulkStream, getEventTarget()).WillByDefault(Return(bulkStream));
    ON_CALL(*bulkStream, getBufferedOutputSize()).WillByDefault(Return(0u));
    client.testAttachBulkStream(bulkStream);

    ServerProxy proxy(&client, &controlStream, &events, 11);
    client.testSetServerProxy(&proxy);
    EXPECT_FALSE(client.handleBulkMessage(
        reinterpret_cast<const UInt8*>(kMsgDFileTransfer), bulkStream));
    EXPECT_EQ(FileReceiveSession::kIdle,
              client.getFileReceiveSession().state());
    client.testSetServerProxy(NULL);
}

TEST(ServerProxyTests, legacyControlFilePayloadIsConsumedWithoutCreatingReceiveState)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    DuplexMemoryStream controlStream;
    ServerProxy proxy(&client, &controlStream, &events, 11);
    client.testSetServerProxy(&proxy);

    controlStream.queueInput(encodeFilePayload(
        kDataStart, std::to_string(FileChunk::kMemoryReceiveLimit + 1)));
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>(kMsgDFileTransfer)));
    EXPECT_EQ(FileReceiveSession::kIdle,
              client.getFileReceiveSession().state());

    controlStream.queueInput(encodeFilePayload(kDataChunk, "ignored"));
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>(kMsgDFileTransfer)));
    EXPECT_EQ(FileReceiveSession::kIdle,
              client.getFileReceiveSession().state());

    controlStream.queueInput(encodeFilePayload(kDataEnd, ""));
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>(kMsgDFileTransfer)));
    EXPECT_EQ(FileReceiveSession::kIdle,
              client.getFileReceiveSession().state());

    client.testSetServerProxy(NULL);
}

TEST(ServerProxyTests, completionQueueFailureFailsReceiveWithoutEscapingParser)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    DuplexMemoryStream controlStream;
    ServerProxy proxy(&client, &controlStream, &events);
    client.testSetServerProxy(&proxy);

    controlStream.queueInput(encodeFilePayload(kDataStart, "0"));
    ASSERT_EQ(kStart, proxy.fileChunkReceived());
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke([](const Event&) {
        throw std::runtime_error("completion queue unavailable");
    }));

    controlStream.queueInput(encodeFilePayload(kDataEnd, ""));
    int result = kFinish;
    EXPECT_NO_THROW(result = proxy.fileChunkReceived());
    EXPECT_EQ(kError, result);
    EXPECT_EQ(FileReceiveSession::kFailed,
              client.getFileReceiveSession().state());

    client.testSetServerProxy(NULL);
}

TEST(ServerProxyTests, fileReceiveProtocolErrorThroughBulkClearsClientSession)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    NiceMock<MockStream> controlStream;
    ON_CALL(controlStream, getEventTarget()).WillByDefault(Return(&controlStream));
    ON_CALL(controlStream, getBufferedOutputSize()).WillByDefault(Return(0u));

    DuplexMemoryStream* bulkStream = new DuplexMemoryStream();
    bulkStream->queueInput(std::vector<UInt8>(
        kMsgDFileTransfer, kMsgDFileTransfer + 4));
    client.testAttachBulkStream(bulkStream);
    std::shared_ptr<barrier::BulkChannel> channel = client.acquireBulkChannel();
    ASSERT_TRUE(channel);

    ServerProxy proxy(&client, &controlStream, &events);
    client.testSetServerProxy(&proxy);
    channel->handleDataForTest();

    EXPECT_FALSE(channel->isActive());
    EXPECT_TRUE(bulkStream->closed);
    EXPECT_EQ(FileReceiveSession::kIdle,
              client.getFileReceiveSession().state());
    EXPECT_EQ(0u, client.getFileReceiveSession().expectedSize());
    client.testSetServerProxy(NULL);
}

TEST(ServerProxyTests, legacyDuplicateFileStartIsConsumedWithoutReceiveSession)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    NiceMock<MockStream> controlStream;
    ON_CALL(controlStream, getEventTarget()).WillByDefault(Return(&controlStream));
    ON_CALL(controlStream, getBufferedOutputSize()).WillByDefault(Return(0u));

    DuplexMemoryStream* bulkStream = new DuplexMemoryStream();
    bulkStream->queueInput(encodeFileMessage(kDataStart, "6"));
    bulkStream->queueInput(encodeFileMessage(kDataChunk, "abc"));
    bulkStream->queueInput(encodeFileMessage(kDataStart, "1"));
    client.testAttachBulkStream(bulkStream);
    std::shared_ptr<barrier::BulkChannel> channel = client.acquireBulkChannel();
    ASSERT_TRUE(channel);

    ServerProxy proxy(&client, &controlStream, &events, 11);
    client.testSetServerProxy(&proxy);
    channel->handleDataForTest();

    EXPECT_TRUE(channel->isActive());
    EXPECT_FALSE(bulkStream->closed);
    EXPECT_EQ(FileReceiveSession::kIdle,
              client.getFileReceiveSession().state());
    EXPECT_EQ(0u, client.getFileReceiveSession().expectedSize());
    EXPECT_TRUE(client.getFileReceiveSession().data().empty());
    EXPECT_TRUE(client.getFileReceiveSession().begin(1, 1024, 1024));
    client.getFileReceiveSession().reset();
    client.testSetServerProxy(NULL);
}

TEST(ServerProxyTests, legacyBulkPayloadDoesNotMutateExistingReceiveSession)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    NiceMock<MockStream> controlStream;
    ON_CALL(controlStream, getEventTarget()).WillByDefault(Return(&controlStream));
    ON_CALL(controlStream, getBufferedOutputSize()).WillByDefault(Return(0u));

    DuplexMemoryStream* bulkStream = new DuplexMemoryStream();
    bulkStream->queueInput(encodeFileMessage(kDataEnd, ""));
    bulkStream->queueInput(encodeFileMessage(kDataStart, "1"));
    bulkStream->queueInput(encodeFileMessage(kDataChunk, "b"));
    bulkStream->queueInput(encodeFileMessage(kDataEnd, ""));
    client.testAttachBulkStream(bulkStream);
    std::shared_ptr<barrier::BulkChannel> channel = client.acquireBulkChannel();
    ASSERT_TRUE(channel);

    ServerProxy proxy(&client, &controlStream, &events, 11);
    client.testSetServerProxy(&proxy);
    FileReceiveSession& session = client.getFileReceiveSession();
    ASSERT_TRUE(session.begin(1, FileChunk::kMemoryReceiveLimit,
                              FileChunk::kMemoryReceiveLimit));
    ASSERT_TRUE(session.append("a"));

    std::vector<std::uint64_t> completionGenerations;
    EXPECT_CALL(events, addEvent(_)).Times(AnyNumber()).WillRepeatedly(
        Invoke([&](const Event& event) {
            FileReceiveCompletionInfo* info =
                static_cast<FileReceiveCompletionInfo*>(event.getDataObject());
            if (info != NULL) {
                completionGenerations.push_back(info->m_generation);
                Event::deleteData(event);
            }
        }));

    channel->handleDataForTest();

    EXPECT_TRUE(completionGenerations.empty());
    EXPECT_EQ(0u, bulkStream->getSize());
    EXPECT_TRUE(channel->isActive());
    EXPECT_EQ(FileReceiveSession::kReceiving, session.state());
    EXPECT_EQ(1u, session.expectedSize());
    EXPECT_EQ("a", session.data());

    client.testSetServerProxy(NULL);
}

TEST(ServerProxyTests, cleanupFailureSkipsClipboardMarshall)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    setServerProxyEventDefaults(events, streamEvents, clipboardEvents, fileEvents);

    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));

    CleanupFailingServerProxy proxy(reinterpret_cast<Client*>(1), &stream, &events);
    CountingClipboard clipboard;

    EXPECT_EQ(ServerProxy::kClipboardSendFailed,
              proxy.onClipboardChanged(kClipboardClipboard, &clipboard));

    EXPECT_EQ(1, proxy.cleanupCalls);
    EXPECT_EQ(0, clipboard.openCount);
    EXPECT_EQ(0, clipboard.hasCount);
    EXPECT_EQ(0, clipboard.getCount);
    EXPECT_EQ(0, clipboard.closeCount);
}

TEST(ServerProxyTests, transactionalReceiveCommitsOnlyAfterClientOwnsPayload)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream control;
    DuplexMemoryStream* bulk = new DuplexMemoryStream();
    client.testAttachBulkStream(bulk);

    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 1);
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, transferId, 3),
        barrier::FileTransferRole::kPrimary));
    ASSERT_FALSE(control.output.empty());
    barrier::FileTransferFrame ack = decodeTransactionalOutput(
        control.output, barrier::FileTransferRole::kPrimary);
    EXPECT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    control.clearOutput();

    ASSERT_TRUE(dispatchTransactionalBulkFrame(
        proxy, *bulk,
        barrier::FileTransferFrame::data(
            kTransactionalBinding, transferId, 0, "abc"),
        barrier::FileTransferRole::kPrimary));
    EXPECT_TRUE(control.output.empty());

    ASSERT_TRUE(dispatchTransactionalBulkFrame(
        proxy, *bulk,
        barrier::FileTransferFrame::end(
            kTransactionalBinding, transferId, 3, digestFor("abc")),
        barrier::FileTransferRole::kPrimary));
    proxy.testPollTransactionalFileReceive();

    ASSERT_FALSE(control.output.empty());
    ack = decodeTransactionalOutput(
        control.output, barrier::FileTransferRole::kPrimary);
    EXPECT_EQ(barrier::FileTransferFrameType::kCommitAck, ack.type);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    EXPECT_FALSE(proxy.testHasTransactionalReceive());

    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests, transactionalClipboardStartBindsExplicitIdentityBeforeMetadata)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream control;
    DuplexMemoryStream* bulk = new DuplexMemoryStream();
    client.testAttachBulkStream(bulk);
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 2);
    const std::string session = "0123456789abcdef0123456789abcdef";
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, transferId, 8,
            barrier::FileTransferKind::kClipboard, 42, session),
        barrier::FileTransferRole::kPrimary));

    EXPECT_EQ(barrier::FileTransferKind::kClipboard,
              client.testTransactionalReceiveKind());
    EXPECT_EQ(42u, client.testTransactionalReceiveSenderRevision());
    EXPECT_EQ(session, client.testTransactionalReceiveSession());

    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::cancel(
            kTransactionalBinding, transferId,
            barrier::FileTransferReason::kCancelled),
        barrier::FileTransferRole::kPrimary));
    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests, transactionalClipboardCommitRequiresMatchingCurrentMetadata)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream control;
    DuplexMemoryStream* bulk = new DuplexMemoryStream();
    client.testAttachBulkStream(bulk);
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    const std::string session = "1123456789abcdef0123456789abcdef";
    for (UInt32 sequence = 20; sequence <= 21; ++sequence) {
        const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
            barrier::FileTransferRole::kPrimary, sequence);
        const std::string payload = "BDIRPKG1E";
        control.clearOutput();
        ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
            proxy, control,
            barrier::FileTransferFrame::start(
                kTransactionalBinding, transferId,
                static_cast<UInt32>(payload.size()),
                barrier::FileTransferKind::kClipboard, sequence, session),
            barrier::FileTransferRole::kPrimary));
        control.clearOutput();

        if (sequence == 21) {
            client.testSetFileClipboardSessions(session, "", {});
            client.testSupersedeFileClipboard();
        }

        ASSERT_TRUE(dispatchTransactionalBulkFrame(
            proxy, *bulk,
            barrier::FileTransferFrame::data(
                kTransactionalBinding, transferId, 0, payload),
            barrier::FileTransferRole::kPrimary));
        ASSERT_TRUE(dispatchTransactionalBulkFrame(
            proxy, *bulk,
            barrier::FileTransferFrame::end(
                kTransactionalBinding, transferId,
                static_cast<UInt32>(payload.size()), digestFor(payload)),
            barrier::FileTransferRole::kPrimary));
        proxy.testPollTransactionalFileReceive();

        ASSERT_FALSE(control.output.empty());
        const barrier::FileTransferFrame ack = decodeTransactionalOutput(
            control.output, barrier::FileTransferRole::kPrimary);
        EXPECT_EQ(barrier::FileTransferFrameType::kCommitAck, ack.type);
        EXPECT_EQ(barrier::FileTransferReason::kRejected, ack.reason);
        EXPECT_FALSE(client.testHasWriteToDropDirThread());
        client.acquireBulkChannel()->serviceInputPauseForTest(0.0);
    }

    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests, transactionalClipboardMetadataMayArriveAfterStart)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream control;
    DuplexMemoryStream* bulk = new DuplexMemoryStream();
    client.testAttachBulkStream(bulk);
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 22);
    const std::string session = "2123456789abcdef0123456789abcdef";
    const std::string payload = "BDIRPKG1E";
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, transferId,
            static_cast<UInt32>(payload.size()),
            barrier::FileTransferKind::kClipboard, 99, session),
        barrier::FileTransferRole::kPrimary));
    control.clearOutput();

    client.testSetFileClipboardSessions(session, "", {});
    ASSERT_TRUE(dispatchTransactionalBulkFrame(
        proxy, *bulk,
        barrier::FileTransferFrame::data(
            kTransactionalBinding, transferId, 0, payload),
        barrier::FileTransferRole::kPrimary));
    ASSERT_TRUE(dispatchTransactionalBulkFrame(
        proxy, *bulk,
        barrier::FileTransferFrame::end(
            kTransactionalBinding, transferId,
            static_cast<UInt32>(payload.size()), digestFor(payload)),
        barrier::FileTransferRole::kPrimary));
    proxy.testPollTransactionalFileReceive();

    ASSERT_FALSE(control.output.empty());
    const barrier::FileTransferFrame ack = decodeTransactionalOutput(
        control.output, barrier::FileTransferRole::kPrimary);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    for (int i = 0; i < 200 && client.testHasWriteToDropDirThread(); ++i) {
        client.testCleanupWriteToDropDirThread();
        ARCH->sleep(0.001);
    }
    EXPECT_FALSE(client.testHasWriteToDropDirThread());

    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests,
     transactionalStartWithoutActiveBulkIsRejectedWithoutLosingControl)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream control;
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    const UInt32 rejectedId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 31);
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, rejectedId, 1),
        barrier::FileTransferRole::kPrimary));
    ASSERT_FALSE(control.output.empty());
    barrier::FileTransferFrame ack = decodeTransactionalOutput(
        control.output, barrier::FileTransferRole::kPrimary);
    EXPECT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);
    EXPECT_EQ(rejectedId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kConnectionLost, ack.reason);
    EXPECT_FALSE(proxy.testHasTransactionalReceive());

    DuplexMemoryStream* bulk = new DuplexMemoryStream();
    client.testAttachBulkStream(bulk);
    control.clearOutput();
    const UInt32 acceptedId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 32);
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, acceptedId, 0),
        barrier::FileTransferRole::kPrimary));
    ASSERT_FALSE(control.output.empty());
    ack = decodeTransactionalOutput(
        control.output, barrier::FileTransferRole::kPrimary);
    EXPECT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);
    EXPECT_EQ(acceptedId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    EXPECT_EQ(acceptedId, proxy.testTransactionalReceiveId());

    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests,
     malformedTransactionalControlFrameDisconnectsControlConnection)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);
    dataSocketEvents.setEvents(&events);
    socketEvents.setEvents(&events);
    ON_CALL(events, forIDataSocket()).WillByDefault(ReturnRef(dataSocketEvents));
    ON_CALL(events, forISocket()).WillByDefault(ReturnRef(socketEvents));
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());
    EXPECT_CALL(events, addEvent(_)).Times(AnyNumber()).WillRepeatedly(
        Invoke(consumeClientFailureEvent));

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream* control = new DuplexMemoryStream();
    ServerProxy* proxy = new ServerProxy(&client, control, &events, 12);
    client.testSetStreamOnly(control);
    client.testSetServerProxy(proxy);
    proxy->testBindTransactionalFileTransfer(kTransactionalBinding);
    proxy->testUseMessageParser();

    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 32);
    std::vector<UInt8> encoded = encodeTransactionalFrame(
        barrier::FileTransferFrame::start(
            kTransactionalBinding, transferId, 1),
        barrier::FileTransferRole::kPrimary);
    ASSERT_GT(encoded.size(), 4u);
    encoded.pop_back();
    control->queueInput(encoded);

    proxy->handleDataForTest();

    EXPECT_FALSE(client.isConnected());
}

TEST(ServerProxyTests,
     transactionalBulkFrameOnControlDisconnectsControlConnection)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);
    dataSocketEvents.setEvents(&events);
    socketEvents.setEvents(&events);
    ON_CALL(events, forIDataSocket()).WillByDefault(ReturnRef(dataSocketEvents));
    ON_CALL(events, forISocket()).WillByDefault(ReturnRef(socketEvents));
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());
    EXPECT_CALL(events, addEvent(_)).Times(AnyNumber()).WillRepeatedly(
        Invoke(consumeClientFailureEvent));

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream* control = new DuplexMemoryStream();
    ServerProxy* proxy = new ServerProxy(&client, control, &events, 12);
    client.testSetStreamOnly(control);
    client.testSetServerProxy(proxy);
    proxy->testBindTransactionalFileTransfer(kTransactionalBinding);
    proxy->testUseMessageParser();

    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 34);
    control->queueInput(encodeTransactionalFrame(
        barrier::FileTransferFrame::data(
            kTransactionalBinding, transferId, 0, "x"),
        barrier::FileTransferRole::kPrimary));

    proxy->handleDataForTest();

    EXPECT_FALSE(client.isConnected());
}

TEST(ServerProxyTests,
     transactionalLateCancelAfterBulkLossPreservesReplacementReceive)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream control;
    DuplexMemoryStream* firstBulk = new DuplexMemoryStream();
    client.testAttachBulkStream(firstBulk);
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    const UInt32 interruptedId =
        barrier::FileTransferProtocol::makeTransferId(
            barrier::FileTransferRole::kPrimary, 33);
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, interruptedId, 1),
        barrier::FileTransferRole::kPrimary));
    ASSERT_EQ(interruptedId, proxy.testTransactionalReceiveId());

    std::shared_ptr<barrier::BulkChannel> interruptedRoute =
        client.acquireBulkChannel();
    ASSERT_TRUE(interruptedRoute);
    interruptedRoute->close();
    proxy.handleBulkDisconnected(interruptedRoute.get());
    EXPECT_FALSE(proxy.testHasTransactionalReceive());

    DuplexMemoryStream* replacementBulk = new DuplexMemoryStream();
    client.testAttachBulkStream(replacementBulk);
    control.clearOutput();
    const UInt32 replacementId =
        barrier::FileTransferProtocol::makeTransferId(
            barrier::FileTransferRole::kPrimary, 34);
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, replacementId, 0),
        barrier::FileTransferRole::kPrimary));
    ASSERT_EQ(replacementId, proxy.testTransactionalReceiveId());

    control.clearOutput();
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::cancel(
            kTransactionalBinding, interruptedId,
            barrier::FileTransferReason::kConnectionLost),
        barrier::FileTransferRole::kPrimary));
    ASSERT_FALSE(control.output.empty());
    const barrier::FileTransferFrame ack = decodeTransactionalOutput(
        control.output, barrier::FileTransferRole::kPrimary);
    EXPECT_EQ(barrier::FileTransferFrameType::kCancelAck, ack.type);
    EXPECT_EQ(interruptedId, ack.transferId);
    EXPECT_EQ(replacementId, proxy.testTransactionalReceiveId());

    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests, transactionalManualStartDoesNotInheritAmbientClipboard)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    const std::string ambientSession =
        "3123456789abcdef0123456789abcdef";
    client.testSetFileClipboardSessions(ambientSession, "", {});
    DuplexMemoryStream control;
    DuplexMemoryStream* bulk = new DuplexMemoryStream();
    client.testAttachBulkStream(bulk);
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 23);
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, transferId, 1,
            barrier::FileTransferKind::kManual),
        barrier::FileTransferRole::kPrimary));
    EXPECT_EQ(barrier::FileTransferKind::kManual,
              client.testTransactionalReceiveKind());
    EXPECT_TRUE(client.testTransactionalReceiveSession().empty());

    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::cancel(
            kTransactionalBinding, transferId,
            barrier::FileTransferReason::kCancelled),
        barrier::FileTransferRole::kPrimary));
    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests, transactionalReceiveRejectsStaleBindingAndTransferId)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream control;
    DuplexMemoryStream* bulk = new DuplexMemoryStream();
    client.testAttachBulkStream(bulk);
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 7);
    EXPECT_EQ(ServerProxy::kDisconnect, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            "ffeeddccbbaa99887766554433221100", transferId, 1),
        barrier::FileTransferRole::kPrimary));
    EXPECT_FALSE(proxy.testHasTransactionalReceive());

    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, transferId, 1),
        barrier::FileTransferRole::kPrimary));
    const UInt32 staleId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 6);
    EXPECT_FALSE(dispatchTransactionalBulkFrame(
        proxy, *bulk,
        barrier::FileTransferFrame::data(
            kTransactionalBinding, staleId, 0, "x"),
        barrier::FileTransferRole::kPrimary));

    proxy.handleBulkDisconnected(client.acquireBulkChannel().get());
    EXPECT_FALSE(proxy.testHasTransactionalReceive());
    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests, transactionalReceiveSupportsConsecutiveTransfers)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream control;
    DuplexMemoryStream* bulk = new DuplexMemoryStream();
    client.testAttachBulkStream(bulk);
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    for (UInt32 sequence = 1; sequence <= 2; ++sequence) {
        const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
            barrier::FileTransferRole::kPrimary, sequence);
        const std::string payload(1, static_cast<char>('a' + sequence - 1));
        control.clearOutput();
        ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
            proxy, control,
            barrier::FileTransferFrame::start(
                kTransactionalBinding, transferId, 1),
            barrier::FileTransferRole::kPrimary));
        ASSERT_TRUE(dispatchTransactionalBulkFrame(
            proxy, *bulk,
            barrier::FileTransferFrame::data(
                kTransactionalBinding, transferId, 0, payload),
            barrier::FileTransferRole::kPrimary));
        ASSERT_TRUE(dispatchTransactionalBulkFrame(
            proxy, *bulk,
            barrier::FileTransferFrame::end(
                kTransactionalBinding, transferId, 1,
                digestFor(payload)),
            barrier::FileTransferRole::kPrimary));
        proxy.testPollTransactionalFileReceive();
        ASSERT_FALSE(control.output.empty());
        EXPECT_FALSE(proxy.testHasTransactionalReceive());
        client.acquireBulkChannel()->serviceInputPauseForTest(0.0);
    }

    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests, transactionalCancelDiscardsQueuedBulkBeforeReplacement)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream control;
    DuplexMemoryStream* bulk = new DuplexMemoryStream();
    client.testAttachBulkStream(bulk);
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    const UInt32 cancelledId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 10);
    const UInt32 replacementId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 11);
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, cancelledId, 3),
        barrier::FileTransferRole::kPrimary));
    control.clearOutput();
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::cancel(
            kTransactionalBinding, cancelledId,
            barrier::FileTransferReason::kCancelled),
        barrier::FileTransferRole::kPrimary));
    EXPECT_FALSE(proxy.testHasTransactionalReceive());

    control.clearOutput();
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, replacementId, 3),
        barrier::FileTransferRole::kPrimary));
    EXPECT_TRUE(proxy.testHasTransactionalReceive());
    ASSERT_FALSE(control.output.empty());
    barrier::FileTransferFrame ack = decodeTransactionalOutput(
        control.output, barrier::FileTransferRole::kPrimary);
    ASSERT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);
    ASSERT_EQ(replacementId, ack.transferId);
    ASSERT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    control.clearOutput();

    EXPECT_TRUE(dispatchTransactionalBulkFrame(
        proxy, *bulk,
        barrier::FileTransferFrame::data(
            kTransactionalBinding, cancelledId, 0, "abc"),
        barrier::FileTransferRole::kPrimary));
    EXPECT_TRUE(dispatchTransactionalBulkFrame(
        proxy, *bulk,
        barrier::FileTransferFrame::end(
            kTransactionalBinding, cancelledId, 3, digestFor("abc")),
        barrier::FileTransferRole::kPrimary));
    EXPECT_TRUE(proxy.testHasTransactionalReceive());

    ASSERT_TRUE(dispatchTransactionalBulkFrame(
        proxy, *bulk,
        barrier::FileTransferFrame::data(
            kTransactionalBinding, replacementId, 0, "xyz"),
        barrier::FileTransferRole::kPrimary));
    ASSERT_TRUE(dispatchTransactionalBulkFrame(
        proxy, *bulk,
        barrier::FileTransferFrame::end(
            kTransactionalBinding, replacementId, 3, digestFor("xyz")),
        barrier::FileTransferRole::kPrimary));
    proxy.testPollTransactionalFileReceive();
    ASSERT_FALSE(control.output.empty());
    ack = decodeTransactionalOutput(
        control.output, barrier::FileTransferRole::kPrimary);
    EXPECT_EQ(barrier::FileTransferFrameType::kCommitAck, ack.type);
    EXPECT_EQ(replacementId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    EXPECT_FALSE(proxy.testHasTransactionalReceive());

    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests, transactionalCancelAckWaitsForSpoolCleanupBeforeReplacement)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);
    EventQueueTimer* receiveTimer =
        reinterpret_cast<EventQueueTimer*>(static_cast<uintptr_t>(12));
    EXPECT_CALL(events, newOneShotTimer(_, _)).Times(AnyNumber())
        .WillRepeatedly(Return(reinterpret_cast<EventQueueTimer*>(1)));
    EXPECT_CALL(events, newOneShotTimer(DoubleEq(0.01), nullptr))
        .WillRepeatedly(Return(receiveTimer));

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream control;
    DuplexMemoryStream* bulk = new DuplexMemoryStream();
    client.testAttachBulkStream(bulk);
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    WorkerExitGate workerExit;
    proxy.testSetTransactionalReceiveWorkerExitGate(workerExit.gate());
    const UInt32 cancelledId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 24);
    const UInt32 replacementId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 25);
    const UInt32 spoolSize = static_cast<UInt32>(
        FileChunk::kMemoryReceiveLimit + 1);

    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, cancelledId, spoolSize),
        barrier::FileTransferRole::kPrimary));
    control.clearOutput();

    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::cancel(
            kTransactionalBinding, cancelledId,
            barrier::FileTransferReason::kCancelled),
        barrier::FileTransferRole::kPrimary));
    EXPECT_TRUE(proxy.testTransactionalCancelAckPending());
    EXPECT_TRUE(proxy.testTransactionalReceiveCleanupPending());
    ASSERT_EQ(4u, control.output.size());
    EXPECT_EQ(0, std::memcmp(control.output.data(), kMsgCNoop, 4));
    control.clearOutput();

    proxy.testPollTransactionalFileReceive();
    EXPECT_TRUE(proxy.testTransactionalCancelAckPending());
    EXPECT_TRUE(control.output.empty());

    workerExit.release();
    for (int i = 0;
         i < 1000 && proxy.testTransactionalReceiveCleanupPending(); ++i) {
        ARCH->sleep(0.001);
    }
    ASSERT_FALSE(proxy.testTransactionalReceiveCleanupPending());
    proxy.testPollTransactionalFileReceive();

    ASSERT_FALSE(control.output.empty());
    barrier::FileTransferFrame ack = decodeTransactionalOutput(
        control.output, barrier::FileTransferRole::kPrimary);
    EXPECT_EQ(barrier::FileTransferFrameType::kCancelAck, ack.type);
    EXPECT_EQ(cancelledId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    EXPECT_FALSE(proxy.testTransactionalCancelAckPending());
    control.clearOutput();

    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, replacementId, 1),
        barrier::FileTransferRole::kPrimary));
    ASSERT_FALSE(control.output.empty());
    ack = decodeTransactionalOutput(
        control.output, barrier::FileTransferRole::kPrimary);
    EXPECT_EQ(barrier::FileTransferFrameType::kStartAck, ack.type);
    EXPECT_EQ(replacementId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);

    control.clearOutput();
    EXPECT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::cancel(
            kTransactionalBinding, replacementId,
            barrier::FileTransferReason::kCancelled),
        barrier::FileTransferRole::kPrimary));
    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests, transactionalCancelCleanupDeadlineQuarantinesStuckWorker)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);
    EXPECT_CALL(events, newOneShotTimer(_, _)).Times(AnyNumber())
        .WillRepeatedly(Return(reinterpret_cast<EventQueueTimer*>(1)));

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream control;
    DuplexMemoryStream* bulk = new DuplexMemoryStream();
    client.testAttachBulkStream(bulk);
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    std::vector<double> cleanupDelays;
    EventQueueTimer* receiveTimer =
        reinterpret_cast<EventQueueTimer*>(static_cast<uintptr_t>(14));
    EXPECT_CALL(events, newOneShotTimer(_, nullptr))
        .Times(AnyNumber())
        .WillRepeatedly(Invoke(
            [&cleanupDelays, receiveTimer](double delay, void*) {
                cleanupDelays.push_back(delay);
                return receiveTimer;
            }));

    WorkerExitGate workerExit;
    proxy.testSetTransactionalReceiveWorkerExitGate(workerExit.gate());
    const UInt32 cancelledId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 26);
    const UInt32 replacementId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 27);
    const UInt32 spoolSize = static_cast<UInt32>(
        FileChunk::kMemoryReceiveLimit + 1);

    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, cancelledId, spoolSize),
        barrier::FileTransferRole::kPrimary));
    const std::uint64_t cancelledGeneration =
        proxy.testTransactionalReceiveGeneration();
    control.clearOutput();
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::cancel(
            kTransactionalBinding, cancelledId,
            barrier::FileTransferReason::kCancelled),
        barrier::FileTransferRole::kPrimary));
    control.clearOutput();

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
    ASSERT_FALSE(control.output.empty());
    barrier::FileTransferFrame ack = decodeTransactionalOutput(
        control.output, barrier::FileTransferRole::kPrimary);
    EXPECT_EQ(barrier::FileTransferFrameType::kCancelAck, ack.type);
    EXPECT_EQ(cancelledId, ack.transferId);
    EXPECT_EQ(barrier::FileTransferReason::kNone, ack.reason);
    control.clearOutput();

    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, replacementId, spoolSize),
        barrier::FileTransferRole::kPrimary));
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

    control.clearOutput();
    EXPECT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::cancel(
            kTransactionalBinding, replacementId,
            barrier::FileTransferReason::kCancelled),
        barrier::FileTransferRole::kPrimary));
    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests, transactionalReceiveDoesNotCommitWhenOwnerQueueIsFull)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    std::atomic<bool> releaseWriter(false);
    client.testSetWriteToDropDirThread(new Thread([&releaseWriter]() {
        while (!releaseWriter.load()) {
            ARCH->sleep(0.001);
            Thread::testCancel();
        }
    }));
    for (int i = 0; i < 4; ++i) {
        client.testQueueDropDirTransfer(std::string(1, static_cast<char>('0' + i)));
    }
    ASSERT_EQ(4u, client.testPendingDropDirTransferCount());

    DuplexMemoryStream control;
    DuplexMemoryStream* bulk = new DuplexMemoryStream();
    client.testAttachBulkStream(bulk);
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 9);
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::start(
            kTransactionalBinding, transferId, 1),
        barrier::FileTransferRole::kPrimary));
    control.clearOutput();
    ASSERT_TRUE(dispatchTransactionalBulkFrame(
        proxy, *bulk,
        barrier::FileTransferFrame::data(
            kTransactionalBinding, transferId, 0, "x"),
        barrier::FileTransferRole::kPrimary));
    ASSERT_TRUE(dispatchTransactionalBulkFrame(
        proxy, *bulk,
        barrier::FileTransferFrame::end(
            kTransactionalBinding, transferId, 1, digestFor("x")),
        barrier::FileTransferRole::kPrimary));
    proxy.testPollTransactionalFileReceive();

    ASSERT_FALSE(control.output.empty());
    const barrier::FileTransferFrame ack = decodeTransactionalOutput(
        control.output, barrier::FileTransferRole::kPrimary);
    EXPECT_EQ(barrier::FileTransferFrameType::kCommitAck, ack.type);
    EXPECT_EQ(barrier::FileTransferReason::kBusy, ack.reason);

    releaseWriter.store(true);
    for (int i = 0; i < 200 && client.testHasWriteToDropDirThread(); ++i) {
        client.testCleanupWriteToDropDirThread();
        ARCH->sleep(0.001);
    }
    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests, transactionalSenderAcceptsOnlyCurrentPhaseAcknowledgments)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream control;
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    const UInt32 transferId = client.testAllocateSendFileTransferId();
    std::shared_ptr<barrier::FileTransferSendState> state(
        new barrier::FileTransferSendState(transferId));
    ASSERT_TRUE(state->markStartQueued(3));
    client.testSetSendFileTransactionState(state);

    EXPECT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::startAck(
            kTransactionalBinding, transferId,
            barrier::FileTransferReason::kNone),
        barrier::FileTransferRole::kSecondary));
    EXPECT_TRUE(state->readyForData());

    ASSERT_TRUE(state->markDataQueued(0, 3));
    ASSERT_TRUE(state->markEndQueued(3));
    EXPECT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::commitAck(
            kTransactionalBinding, transferId,
            barrier::FileTransferReason::kNone),
        barrier::FileTransferRole::kSecondary));
    EXPECT_TRUE(state->committed());

    const UInt32 nextTransferId = client.testAllocateSendFileTransferId();
    std::shared_ptr<barrier::FileTransferSendState> nextState(
        new barrier::FileTransferSendState(nextTransferId));
    ASSERT_TRUE(nextState->markStartQueued(1));
    client.testSetSendFileTransactionState(nextState);
    EXPECT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::startAck(
            kTransactionalBinding, transferId,
            barrier::FileTransferReason::kNone),
        barrier::FileTransferRole::kSecondary));
    EXPECT_FALSE(nextState->readyForData());

    EXPECT_EQ(ServerProxy::kDisconnect, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::startAck(
            "ffeeddccbbaa99887766554433221100", nextTransferId,
            barrier::FileTransferReason::kNone),
        barrier::FileTransferRole::kSecondary));

    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests, transactionalSenderRetainsCancelUntilAcknowledged)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);
    EventQueueTimer* cancelAckTimer =
        reinterpret_cast<EventQueueTimer*>(static_cast<uintptr_t>(9));
    EXPECT_CALL(events, deleteTimer(_)).Times(AnyNumber());
    EXPECT_CALL(events, deleteTimer(cancelAckTimer)).Times(1);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream control;
    DuplexMemoryStream* bulk = new DuplexMemoryStream();
    client.testAttachBulkStream(bulk);
    client.testSetSendFileBulkChannel(client.acquireBulkChannel());
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    const UInt32 transferId = client.testAllocateSendFileTransferId();
    std::shared_ptr<barrier::FileTransferSendState> state(
        new barrier::FileTransferSendState(transferId));
    ASSERT_TRUE(state->markStartQueued(3));
    client.testSetSendFileTransactionState(state);
    client.testSetSendFileProtocolState(true, false);
    ASSERT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::startAck(
            kTransactionalBinding, transferId,
            barrier::FileTransferReason::kNone),
        barrier::FileTransferRole::kSecondary));
    control.clearOutput();
    bulk->clearOutput();

    ASSERT_TRUE(state->markDataQueued(0, 1));
    FileChunk* data = FileChunk::data(
        reinterpret_cast<const UInt8*>("x"), 1, 0);
    data->m_transferId = transferId;
    client.testSendFileChunk(data);
    delete data;
    ASSERT_TRUE(state->markCancelQueued(
        barrier::FileTransferReason::kCancelled));
    EXPECT_CALL(events, newOneShotTimer(DoubleEq(15.0), nullptr))
        .WillOnce(Return(cancelAckTimer));

    FileChunk* cancel = FileChunk::cancel(
        barrier::FileTransferReason::kCancelled);
    cancel->m_transferId = transferId;
    client.testSendFileChunk(cancel);
    delete cancel;
    ASSERT_TRUE(client.testSendFileCancelAckPending());
    ASSERT_FALSE(control.output.empty());

    DuplexMemoryStream queuedControl;
    queuedControl.queueInput(control.output);
    UInt8 code[4];
    ASSERT_EQ(4u, queuedControl.read(code, sizeof(code)));
    barrier::FileTransferFrame sent;
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, &queuedControl, barrier::FileTransferRole::kSecondary,
        kTransactionalBinding, sent));
    EXPECT_EQ(barrier::FileTransferFrameType::kCancel, sent.type);
    EXPECT_EQ(0u, queuedControl.getSize());

    DuplexMemoryStream queuedBulk;
    queuedBulk.queueInput(bulk->output);
    ASSERT_EQ(4u, queuedBulk.read(code, sizeof(code)));
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        code, &queuedBulk, barrier::FileTransferRole::kSecondary,
        kTransactionalBinding, sent));
    EXPECT_EQ(barrier::FileTransferFrameType::kData, sent.type);
    EXPECT_EQ(0u, queuedBulk.getSize());

    client.testSetSendFileThread(new Thread([]() { }));
    bool reaped = false;
    for (int i = 0; i < 100 && !reaped; ++i) {
        reaped = client.testReapSendFileThreadIfReady();
        if (!reaped) {
            ARCH->sleep(0.001);
        }
    }
    ASSERT_TRUE(reaped);
    EXPECT_TRUE(client.testHasSendFileTransactionState());

    EXPECT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::cancelAck(
            kTransactionalBinding, transferId,
            barrier::FileTransferReason::kNone),
        barrier::FileTransferRole::kSecondary));
    EXPECT_TRUE(state->cancelAcknowledged());
    EXPECT_FALSE(client.testSendFileCancelAckPending());
    EXPECT_FALSE(client.testHasSendFileTransactionState());

    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}

TEST(ServerProxyTests, terminalAckArmsReaperUntilSenderThreadExits)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setServerProxyClientEventDefaults(
        events, streamEvents, clipboardEvents, fileEvents,
        clientEvents, screenEvents);
    EventQueueTimer* reapTimer = reinterpret_cast<EventQueueTimer*>(10);
    EXPECT_CALL(events, newOneShotTimer(_, _)).Times(AnyNumber())
        .WillRepeatedly(Return(reinterpret_cast<EventQueueTimer*>(1)));
    EXPECT_CALL(events, newOneShotTimer(DoubleEq(0.01), nullptr))
        .WillOnce(Return(reapTimer));
    EXPECT_CALL(events, deleteTimer(_)).Times(AnyNumber());
    EXPECT_CALL(events, deleteTimer(reapTimer)).Times(1);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    DuplexMemoryStream control;
    ServerProxy proxy(&client, &control, &events, 12);
    client.testSetStreamOnly(&control);
    client.testSetServerProxy(&proxy);
    proxy.testBindTransactionalFileTransfer(kTransactionalBinding);

    const UInt32 transferId = client.testAllocateSendFileTransferId();
    std::shared_ptr<barrier::FileTransferSendState> state(
        new barrier::FileTransferSendState(transferId));
    ASSERT_TRUE(state->markStartQueued(1));
    ASSERT_TRUE(state->signalStartAck(
        transferId, barrier::FileTransferReason::kNone));
    ASSERT_TRUE(state->markDataQueued(0, 1));
    ASSERT_TRUE(state->markEndQueued(1));
    client.testSetSendFileTransactionState(state);
    client.testSetSendFileProtocolState(true, true);

    std::atomic<bool> releaseSender(false);
    Thread* sender = new Thread([&releaseSender]() {
        while (!releaseSender.load()) {
            ARCH->sleep(0.001);
        }
    });
    client.testSetSendFileThread(sender);

    EXPECT_EQ(ServerProxy::kOkay, dispatchTransactionalControlFrame(
        proxy, control,
        barrier::FileTransferFrame::commitAck(
            kTransactionalBinding, transferId,
            barrier::FileTransferReason::kNone),
        barrier::FileTransferRole::kSecondary));

    EXPECT_TRUE(state->committed());
    EXPECT_TRUE(client.testHasSendFileThread());
    EXPECT_TRUE(client.testHasSendFileReapTimer());
    EXPECT_TRUE(client.testHasSendFileTransactionState());

    releaseSender.store(true);
    for (int i = 0; i < 200 && !sender->wait(0.0); ++i) {
        ARCH->sleep(0.001);
    }
    ASSERT_TRUE(sender->wait(0.0));
    client.testHandleSendFileReap();

    EXPECT_FALSE(client.testHasSendFileThread());
    EXPECT_FALSE(client.testHasSendFileReapTimer());
    EXPECT_FALSE(client.testHasSendFileTransactionState());

    client.testSetServerProxy(NULL);
    client.testSetStreamOnly(NULL);
}
