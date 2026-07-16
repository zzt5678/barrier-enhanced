#define BARRIER_TEST_ENV
#include "client/ServerProxy.h"

#include "client/Client.h"
#include "barrier/Clipboard.h"
#include "barrier/ClipboardChunk.h"
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

#include <chrono>
#include <atomic>
#include <cstring>
#include <thread>
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
    payload.sessionId = "small-file-clipboard";
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

TEST(ServerProxyTests, backloggedMouseMovesCompressInLowLatencyNestedRemoteMode)
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
    proxy.m_lowLatencyMode = true;
    proxy.m_nestedRemoteMode = true;

    EXPECT_TRUE(proxy.shouldCompressMouseMoves());
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

    ServerProxy proxy(&client, &stream, &events);
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
