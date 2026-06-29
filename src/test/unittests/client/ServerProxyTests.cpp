#define BARRIER_TEST_ENV
#define private public
#include "client/ServerProxy.h"
#undef private

#include "client/Client.h"
#include "barrier/Clipboard.h"
#include "barrier/ClipboardChunk.h"
#include "barrier/ClientArgs.h"
#include "barrier/option_types.h"
#include "barrier/protocol_types.h"
#include "barrier/Screen.h"
#include "net/ISocketFactory.h"
#include "net/NetworkAddress.h"

#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/io/MockStream.h"

#include <chrono>
#include <thread>
#include <vector>

using ::testing::_;
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

TEST(ServerProxyTests, setKeepAliveRateUsesShortIdleDeathWindow)
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

    EXPECT_CALL(events, newOneShotTimer(DoubleEq(2.0), _))
        .WillOnce(Return(reinterpret_cast<EventQueueTimer*>(2)));

    proxy.setKeepAliveRate(1.0);
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
    Mock::VerifyAndClearExpectations(&events);

    EXPECT_CALL(events, deleteTimer(_)).Times(0);
    EXPECT_CALL(events, newOneShotTimer(_, _)).Times(0);

    proxy.handleData(Event(), NULL);

    EXPECT_EQ(0u, proxy.m_keepAliveAlarmDeferrals);
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

    EXPECT_CALL(events, addEvent(_)).Times(AtLeast(1));

    proxy.handleKeepAliveAlarm(Event(), NULL);

    EXPECT_EQ(8u, proxy.m_keepAliveAlarmDeferrals);
}

TEST(ServerProxyTests, keepAliveAlarmDisconnectsIdleServerImmediately)
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

    EXPECT_CALL(events, addEvent(_)).Times(AtLeast(1));
    EXPECT_CALL(stream, write(_, _)).Times(0);

    proxy.handleKeepAliveAlarm(Event(), NULL);
}

TEST(ServerProxyTests, largeClipboardUsesAsyncSenderAndCanBeReaped)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    setServerProxyEventDefaults(events, streamEvents, clipboardEvents, fileEvents);

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

    ServerProxy proxy(reinterpret_cast<Client*>(1), &stream, &events);
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
