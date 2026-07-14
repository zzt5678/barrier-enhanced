#define BARRIER_TEST_ENV
#include "server/ClientProxy1_0.h"

#include "barrier/protocol_types.h"

#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/io/MockStream.h"

#include <algorithm>
#include <cstring>
#include <vector>

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::Invoke;
using ::testing::Mock;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {

void setClientProxyEventDefaults(MockEventQueue& events,
                                 IStreamEvents& streamEvents,
                                 ClientProxyEvents& clientProxyEvents)
{
    streamEvents.setEvents(&events);
    clientProxyEvents.setEvents(&events);

    ON_CALL(events, forIStream()).WillByDefault(ReturnRef(streamEvents));
    ON_CALL(events, forClientProxy()).WillByDefault(ReturnRef(clientProxyEvents));
    ON_CALL(events, registerTypeOnce(_, _)).WillByDefault(Return(100));
}

void setClientProxyEventDefaults(MockEventQueue& events,
                                 IStreamEvents& streamEvents,
                                 ClientProxyEvents& clientProxyEvents,
                                 IScreenEvents& screenEvents)
{
    setClientProxyEventDefaults(events, streamEvents, clientProxyEvents);
    screenEvents.setEvents(&events);
    ON_CALL(events, forIScreen()).WillByDefault(ReturnRef(screenEvents));
}

std::vector<UInt8> makeDInfoPayload(SInt16 x, SInt16 y, SInt16 w, SInt16 h,
                                    SInt16 dummy, SInt16 mx, SInt16 my)
{
    std::vector<UInt8> payload;
    const SInt16 values[] = { x, y, w, h, dummy, mx, my };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        const UInt16 value = static_cast<UInt16>(values[i]);
        payload.push_back(static_cast<UInt8>((value >> 8) & 0xff));
        payload.push_back(static_cast<UInt8>(value & 0xff));
    }
    return payload;
}

void readPayloadFromStream(MockStream& stream, const std::vector<UInt8>& payload)
{
    size_t offset = 0;
    ON_CALL(stream, read(_, _))
        .WillByDefault(Invoke([&payload, offset](void* buffer, UInt32 n) mutable -> UInt32 {
            if (offset >= payload.size()) {
                return 0;
            }
            const UInt32 count = static_cast<UInt32>(
                std::min<size_t>(n, payload.size() - offset));
            std::memcpy(buffer, payload.data() + offset, count);
            offset += count;
            return count;
        }));
}

}

TEST(ClientProxyDisconnectTests, disconnect_isIdempotent)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyEvents clientProxyEvents;
    setClientProxyEventDefaults(events, streamEvents, clientProxyEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));

    ClientProxy1_0 proxy("client", stream, &events);

    EXPECT_CALL(*stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).Times(1);

    proxy.disconnect();
    proxy.disconnect();
}

TEST(ClientProxyDisconnectTests, inputParserYieldsAndReschedulesAfterBoundedBatch)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyEvents clientProxyEvents;
    setClientProxyEventDefaults(events, streamEvents, clientProxyEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));

    std::vector<UInt8> codes(65 * 4, 0);
    for (size_t i = 0; i < 65; ++i) {
        std::memcpy(&codes[i * 4], kMsgCNoop, 4);
    }
    size_t offset = 0;
    ON_CALL(*stream, read(_, _)).WillByDefault(
        Invoke([&](void* buffer, UInt32 count) -> UInt32 {
            if (offset >= codes.size() || count < 4) {
                return 0;
            }
            std::memcpy(buffer, &codes[offset], 4);
            offset += 4;
            return 4;
        }));
    ON_CALL(*stream, getSize()).WillByDefault(
        Invoke([&]() -> UInt32 { return offset < codes.size() ? 4 : 0; }));

    int rescheduled = 0;
    EXPECT_CALL(events, addEvent(_)).Times(AnyNumber()).WillRepeatedly(
        Invoke([&](const Event& event) {
            if (event.getType() == streamEvents.inputReady()) {
                ++rescheduled;
            }
        }));

    ClientProxy1_0 proxy("client", stream, &events);
    proxy.handleData(Event(), NULL);

    EXPECT_GT(offset, 0u);
    EXPECT_LE(offset, 64u * 4u);
    EXPECT_LT(offset, codes.size());
    EXPECT_EQ(1, rescheduled);

    proxy.handleData(Event(), NULL);
    EXPECT_EQ(codes.size(), offset);
}

TEST(ClientProxyDisconnectTests, closeDoesNotSynchronouslyFlushStream)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyEvents clientProxyEvents;
    setClientProxyEventDefaults(events, streamEvents, clientProxyEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));

    ClientProxy1_0 proxy("client", stream, &events);

    EXPECT_CALL(*stream, write(_, _)).Times(1);
    EXPECT_CALL(*stream, flush()).Times(0);

    proxy.close("EBSY");
}

TEST(ClientProxyDisconnectTests, flatlineDefersWhenStreamHasPendingInput)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyEvents clientProxyEvents;
    setClientProxyEventDefaults(events, streamEvents, clientProxyEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, isReady()).WillByDefault(Return(true));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));

    ClientProxy1_0 proxy("client", stream, &events);
    proxy.m_heartbeatAlarm = 1.0;

    EXPECT_CALL(*stream, close()).Times(0);
    proxy.handleFlatline(Event(), NULL);

    EXPECT_EQ(1u, proxy.m_heartbeatDeferrals);
}

TEST(ClientProxyDisconnectTests, flatlineResetsDeferralBudgetWhenOutputProgresses)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyEvents clientProxyEvents;
    setClientProxyEventDefaults(events, streamEvents, clientProxyEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, isReady()).WillByDefault(Return(false));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(64u));
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));

    ClientProxy1_0 proxy("client", stream, &events);
    proxy.m_heartbeatAlarm = 1.0;
    proxy.m_heartbeatDeferrals = 7;
    proxy.m_lastHeartbeatPendingInput = false;
    proxy.m_lastHeartbeatBufferedOutput = 128;

    EXPECT_CALL(*stream, close()).Times(0);
    proxy.handleFlatline(Event(), NULL);

    EXPECT_EQ(1u, proxy.m_heartbeatDeferrals);
    EXPECT_FALSE(proxy.m_lastHeartbeatPendingInput);
    EXPECT_EQ(64u, proxy.m_lastHeartbeatBufferedOutput);
}

TEST(ClientProxyDisconnectTests, flatlineDoesNotResetDeferralBudgetWhenOutputOnlyGrows)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyEvents clientProxyEvents;
    setClientProxyEventDefaults(events, streamEvents, clientProxyEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, isReady()).WillByDefault(Return(false));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(128u));
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));

    ClientProxy1_0 proxy("client", stream, &events);
    proxy.m_heartbeatAlarm = 1.0;
    proxy.m_heartbeatDeferrals = 6;
    proxy.m_lastHeartbeatPendingInput = false;
    proxy.m_lastHeartbeatBufferedOutput = 64;

    EXPECT_CALL(*stream, close()).Times(0);
    proxy.handleFlatline(Event(), NULL);

    EXPECT_EQ(7u, proxy.m_heartbeatDeferrals);
    EXPECT_FALSE(proxy.m_lastHeartbeatPendingInput);
    EXPECT_EQ(128u, proxy.m_lastHeartbeatBufferedOutput);
}

TEST(ClientProxyDisconnectTests, flatlineResetsDeferralBudgetWhenInputAppears)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyEvents clientProxyEvents;
    setClientProxyEventDefaults(events, streamEvents, clientProxyEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, isReady()).WillByDefault(Return(true));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));

    ClientProxy1_0 proxy("client", stream, &events);
    proxy.m_heartbeatAlarm = 1.0;
    proxy.m_heartbeatDeferrals = 8;
    proxy.m_lastHeartbeatPendingInput = false;

    EXPECT_CALL(*stream, close()).Times(0);
    proxy.handleFlatline(Event(), NULL);

    EXPECT_EQ(1u, proxy.m_heartbeatDeferrals);
    EXPECT_TRUE(proxy.m_lastHeartbeatPendingInput);
}

TEST(ClientProxyDisconnectTests, flatlineResetsDeferralBudgetWhenOutputProgressesAtCap)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyEvents clientProxyEvents;
    setClientProxyEventDefaults(events, streamEvents, clientProxyEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, isReady()).WillByDefault(Return(false));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(4096u));
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));

    ClientProxy1_0 proxy("client", stream, &events);
    proxy.m_heartbeatAlarm = 1.0;
    proxy.m_heartbeatDeferrals = 8;
    proxy.m_lastHeartbeatPendingInput = false;
    proxy.m_lastHeartbeatBufferedOutput = 8192;

    EXPECT_CALL(*stream, close()).Times(0);
    proxy.handleFlatline(Event(), NULL);

    EXPECT_EQ(1u, proxy.m_heartbeatDeferrals);
    EXPECT_EQ(4096u, proxy.m_lastHeartbeatBufferedOutput);
}

TEST(ClientProxyDisconnectTests, flatlineDisconnectsWhenPendingOutputStallsAtDeferralCap)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyEvents clientProxyEvents;
    setClientProxyEventDefaults(events, streamEvents, clientProxyEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, isReady()).WillByDefault(Return(false));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(4096u));
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));

    ClientProxy1_0 proxy("client", stream, &events);
    proxy.m_heartbeatAlarm = 1.0;
    proxy.m_heartbeatDeferrals = 8;
    proxy.m_lastHeartbeatPendingInput = false;
    proxy.m_lastHeartbeatBufferedOutput = 4096;

    EXPECT_CALL(*stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).Times(1);

    proxy.handleFlatline(Event(), NULL);

    EXPECT_EQ(8u, proxy.m_heartbeatDeferrals);
}

TEST(ClientProxyDisconnectTests, flatlineQueriesInfoBeforeDisconnectingIdleClient)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyEvents clientProxyEvents;
    setClientProxyEventDefaults(events, streamEvents, clientProxyEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, isReady()).WillByDefault(Return(false));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));

    ClientProxy1_0 proxy("client", stream, &events);
    proxy.m_heartbeatAlarm = 0.0;
    Mock::VerifyAndClearExpectations(stream);

    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(*stream, write(_, 4))
        .WillOnce(Invoke([](const void* buffer, UInt32) {
            EXPECT_EQ(0, memcmp(buffer, kMsgQInfo, 4));
        }));

    proxy.handleFlatline(Event(), NULL);

    EXPECT_EQ(1u, proxy.m_heartbeatMissedAlarms);
}

TEST(ClientProxyDisconnectTests, flatlineToleratesMultipleIdleHeartbeatMissesBeforeDisconnect)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyEvents clientProxyEvents;
    setClientProxyEventDefaults(events, streamEvents, clientProxyEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, isReady()).WillByDefault(Return(false));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));

    ClientProxy1_0 proxy("client", stream, &events);
    proxy.m_heartbeatAlarm = 0.0;
    proxy.m_heartbeatMissedAlarms = 2;
    Mock::VerifyAndClearExpectations(stream);

    EXPECT_CALL(*stream, close()).Times(0);
    EXPECT_CALL(*stream, write(_, 4)).Times(1);

    proxy.handleFlatline(Event(), NULL);

    EXPECT_EQ(3u, proxy.m_heartbeatMissedAlarms);
}

TEST(ClientProxyDisconnectTests, flatlineDisconnectsIdleClientAfterMissBudget)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyEvents clientProxyEvents;
    setClientProxyEventDefaults(events, streamEvents, clientProxyEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, isReady()).WillByDefault(Return(false));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));

    ClientProxy1_0 proxy("client", stream, &events);
    proxy.m_heartbeatAlarm = 0.0;
    proxy.m_heartbeatMissedAlarms = 3;
    Mock::VerifyAndClearExpectations(stream);

    EXPECT_CALL(*stream, close()).Times(1);
    EXPECT_CALL(events, addEvent(_)).Times(1);

    proxy.handleFlatline(Event(), NULL);
}

TEST(ClientProxyDisconnectTests, dInfoWithUnavailableShapeStillRaisesShapeChanged)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    setClientProxyEventDefaults(events, streamEvents, clientProxyEvents, screenEvents);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    const std::vector<UInt8> payload = makeDInfoPayload(0, 0, 0, 0, 0, 0, 0);
    readPayloadFromStream(*stream, payload);

    ClientProxy1_0 proxy("client", stream, &events);
    Mock::VerifyAndClearExpectations(&events);

    const Event::Type shapeChangedType = screenEvents.shapeChanged();
    EXPECT_CALL(events, addEvent(_))
        .WillOnce(Invoke([&shapeChangedType, &proxy](const Event& event) {
            EXPECT_EQ(shapeChangedType, event.getType());
            EXPECT_EQ(proxy.getEventTarget(), event.getTarget());
            Event::deleteData(event);
        }));

    EXPECT_TRUE(proxy.parseMessage(reinterpret_cast<const UInt8*>(kMsgDInfo)));

    SInt32 x, y, w, h;
    proxy.getShape(x, y, w, h);
    EXPECT_EQ(0, x);
    EXPECT_EQ(0, y);
    EXPECT_EQ(0, w);
    EXPECT_EQ(0, h);
    proxy.getCursorPos(x, y);
    EXPECT_EQ(0, x);
    EXPECT_EQ(0, y);
}
