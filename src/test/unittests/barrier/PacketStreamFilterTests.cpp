/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 */

#include "barrier/PacketStreamFilter.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/io/MockStream.h"

#include "test/global/gtest.h"

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

namespace {

std::vector<UInt8> captureBytes(const void* buffer, UInt32 size)
{
    const auto* start = static_cast<const UInt8*>(buffer);
    return std::vector<UInt8>(start, start + size);
}

}

TEST(PacketStreamFilterTests, write_smallPacket_usesSingleHighPriorityWrite)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    const char payload[] = "abc";
    std::vector<UInt8> written;

    ON_CALL(stream, getEventTarget()).WillByDefault(Return(reinterpret_cast<void*>(0x1)));
    EXPECT_CALL(events, removeHandlers(stream.getEventTarget()));
    EXPECT_CALL(events, adoptHandler(Event::kUnknown, stream.getEventTarget(), _));
    EXPECT_CALL(stream, write(_, 7))
        .WillOnce(Invoke([&written](const void* buffer, UInt32 size) {
            written = captureBytes(buffer, size);
        }));
    EXPECT_CALL(stream, writeLowPriority(_, _)).Times(0);
    EXPECT_CALL(events, removeHandler(Event::kUnknown, stream.getEventTarget()));

    {
        PacketStreamFilter filter(&events, &stream, false);
        filter.write(payload, 3);
    }

    ASSERT_EQ(7u, written.size());
    EXPECT_EQ('a', written[4]);
    EXPECT_EQ('b', written[5]);
    EXPECT_EQ('c', written[6]);
    EXPECT_EQ(0u, written[0]);
    EXPECT_EQ(0u, written[1]);
    EXPECT_EQ(0u, written[2]);
    EXPECT_EQ(3u, written[3]);
}

TEST(PacketStreamFilterTests, writeLowPriority_smallPacket_usesSingleLowPriorityWrite)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    const char payload[] = "xyz";
    std::vector<UInt8> written;

    ON_CALL(stream, getEventTarget()).WillByDefault(Return(reinterpret_cast<void*>(0x2)));
    EXPECT_CALL(events, removeHandlers(stream.getEventTarget()));
    EXPECT_CALL(events, adoptHandler(Event::kUnknown, stream.getEventTarget(), _));
    EXPECT_CALL(stream, write(_, _)).Times(0);
    EXPECT_CALL(stream, writeLowPriority(_, 7))
        .WillOnce(Invoke([&written](const void* buffer, UInt32 size) {
            written = captureBytes(buffer, size);
        }));
    EXPECT_CALL(events, removeHandler(Event::kUnknown, stream.getEventTarget()));

    {
        PacketStreamFilter filter(&events, &stream, false);
        filter.writeLowPriority(payload, 3);
    }

    ASSERT_EQ(7u, written.size());
    EXPECT_EQ('x', written[4]);
    EXPECT_EQ('y', written[5]);
    EXPECT_EQ('z', written[6]);
    EXPECT_EQ(0u, written[0]);
    EXPECT_EQ(0u, written[1]);
    EXPECT_EQ(0u, written[2]);
    EXPECT_EQ(3u, written[3]);
}

TEST(PacketStreamFilterTests, getBufferedOutputSize_forwardsToUnderlyingStream)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;

    ON_CALL(stream, getEventTarget()).WillByDefault(Return(reinterpret_cast<void*>(0x3)));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(42u));
    EXPECT_CALL(events, removeHandlers(stream.getEventTarget()));
    EXPECT_CALL(events, adoptHandler(Event::kUnknown, stream.getEventTarget(), _));
    EXPECT_CALL(events, removeHandler(Event::kUnknown, stream.getEventTarget()));

    PacketStreamFilter filter(&events, &stream, false);
    EXPECT_EQ(42u, filter.getBufferedOutputSize());
}
