/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 */

#include "barrier/PacketStreamFilter.h"
#include "barrier/protocol_types.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/io/MockStream.h"

#include "test/global/gtest.h"

#include <algorithm>
#include <string>
#include <vector>

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {

class PartialWritePriorityStream : public barrier::IStream {
public:
    void close() override { }
    UInt32 read(void*, UInt32) override { return 0; }

    void write(const void* buffer, UInt32 size) override
    {
        append(m_primary, buffer, size);
    }

    void writeLowPriority(const void* buffer, UInt32 size) override
    {
        append(m_lowPriority, buffer, size);
    }

    void flush() override { }
    void shutdownInput() override { }
    void shutdownOutput() override { }
    void* getEventTarget() const override { return const_cast<PartialWritePriorityStream*>(this); }
    bool isReady() const override { return false; }
    UInt32 getSize() const override { return 0; }
    UInt32 getBufferedOutputSize() const override
    {
        return static_cast<UInt32>(m_primary.size() + m_lowPriority.size());
    }

    void drainOnce(size_t maxBytes)
    {
        std::vector<UInt8>& selected = m_primary.empty() ? m_lowPriority : m_primary;
        const size_t size = std::min(maxBytes, selected.size());
        m_wire.insert(m_wire.end(), selected.begin(), selected.begin() + size);
        selected.erase(selected.begin(), selected.begin() + size);
    }

    void drainAll(size_t maxBytes)
    {
        while (!m_primary.empty() || !m_lowPriority.empty()) {
            drainOnce(maxBytes);
        }
    }

    const std::vector<UInt8>& wire() const { return m_wire; }

private:
    static void append(std::vector<UInt8>& destination, const void* buffer, UInt32 size)
    {
        const auto* bytes = static_cast<const UInt8*>(buffer);
        destination.insert(destination.end(), bytes, bytes + size);
    }

    std::vector<UInt8> m_primary;
    std::vector<UInt8> m_lowPriority;
    std::vector<UInt8> m_wire;
};

std::vector<UInt8> captureBytes(const void* buffer, UInt32 size)
{
    const auto* start = static_cast<const UInt8*>(buffer);
    return std::vector<UInt8>(start, start + size);
}

void setupStreamEvents(MockEventQueue& events, IStreamEvents& streamEvents)
{
    streamEvents.setEvents(&events);
    ON_CALL(events, forIStream()).WillByDefault(ReturnRef(streamEvents));
    ON_CALL(events, registerTypeOnce(_, _)).WillByDefault(Return(101));
}

bool parsePacketLengths(const std::vector<UInt8>& wire, std::vector<UInt32>& lengths)
{
    size_t offset = 0;
    while (offset < wire.size()) {
        if (wire.size() - offset < 4) {
            return false;
        }

        const UInt32 length =
            (static_cast<UInt32>(wire[offset]) << 24) |
            (static_cast<UInt32>(wire[offset + 1]) << 16) |
            (static_cast<UInt32>(wire[offset + 2]) << 8) |
            static_cast<UInt32>(wire[offset + 3]);
        offset += 4;
        if (length > PROTOCOL_MAX_MESSAGE_LENGTH || wire.size() - offset < length) {
            return false;
        }

        lengths.push_back(length);
        offset += length;
    }
    return true;
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

TEST(PacketStreamFilterTests, write_largePacket_usesSingleHighPriorityWrite)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    const std::string payload(2048, 'p');
    std::vector<UInt8> written;

    ON_CALL(stream, getEventTarget()).WillByDefault(Return(reinterpret_cast<void*>(0x5)));
    EXPECT_CALL(events, removeHandlers(stream.getEventTarget()));
    EXPECT_CALL(events, adoptHandler(Event::kUnknown, stream.getEventTarget(), _));
    EXPECT_CALL(stream, write(_, static_cast<UInt32>(payload.size() + 4)))
        .WillOnce(Invoke([&written](const void* buffer, UInt32 size) {
            written = captureBytes(buffer, size);
        }));
    EXPECT_CALL(stream, writeLowPriority(_, _)).Times(0);
    EXPECT_CALL(events, removeHandler(Event::kUnknown, stream.getEventTarget()));

    {
        PacketStreamFilter filter(&events, &stream, false);
        filter.write(payload.data(), payload.size());
    }

    ASSERT_EQ(payload.size() + 4, written.size());
    EXPECT_EQ(0u, written[0]);
    EXPECT_EQ(0u, written[1]);
    EXPECT_EQ(8u, written[2]);
    EXPECT_EQ(0u, written[3]);
    EXPECT_EQ('p', written[4]);
    EXPECT_EQ('p', written.back());
}

TEST(PacketStreamFilterTests, write_oversizedPacket_emitsOutputErrorWithoutWriting)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    setupStreamEvents(events, streamEvents);
    const Event::Type outputErrorType = streamEvents.outputError();

    NiceMock<MockStream> stream;
    PacketStreamFilter* filterPtr = nullptr;

    ON_CALL(stream, getEventTarget()).WillByDefault(Return(reinterpret_cast<void*>(0x6)));
    EXPECT_CALL(events, removeHandlers(stream.getEventTarget()));
    EXPECT_CALL(events, adoptHandler(Event::kUnknown, stream.getEventTarget(), _));
    EXPECT_CALL(stream, write(_, _)).Times(0);
    EXPECT_CALL(stream, writeLowPriority(_, _)).Times(0);
    EXPECT_CALL(events, addEvent(_))
        .WillOnce(Invoke([&](const Event& event) {
            EXPECT_EQ(outputErrorType, event.getType());
            EXPECT_EQ(filterPtr, event.getTarget());
        }));
    EXPECT_CALL(events, removeHandler(Event::kUnknown, stream.getEventTarget()));

    {
        PacketStreamFilter filter(&events, &stream, false);
        filterPtr = &filter;
        filter.write(nullptr, PROTOCOL_MAX_MESSAGE_LENGTH + 1);
    }
}

TEST(PacketStreamFilterTests, writeLowPriority_smallPacket_usesAtomicPrimaryWrite)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    const char payload[] = "xyz";
    std::vector<UInt8> written;

    ON_CALL(stream, getEventTarget()).WillByDefault(Return(reinterpret_cast<void*>(0x2)));
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

TEST(PacketStreamFilterTests, writeLowPriority_largePacket_usesAtomicPrimaryWrite)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    const std::string payload(2048, 'q');
    std::vector<UInt8> written;

    ON_CALL(stream, getEventTarget()).WillByDefault(Return(reinterpret_cast<void*>(0x4)));
    EXPECT_CALL(events, removeHandlers(stream.getEventTarget()));
    EXPECT_CALL(events, adoptHandler(Event::kUnknown, stream.getEventTarget(), _));
    EXPECT_CALL(stream, write(_, static_cast<UInt32>(payload.size() + 4)))
        .WillOnce(Invoke([&written](const void* buffer, UInt32 size) {
            written = captureBytes(buffer, size);
        }));
    EXPECT_CALL(stream, writeLowPriority(_, _)).Times(0);
    EXPECT_CALL(events, removeHandler(Event::kUnknown, stream.getEventTarget()));

    {
        PacketStreamFilter filter(&events, &stream, false);
        filter.writeLowPriority(payload.data(), payload.size());
    }

    ASSERT_EQ(payload.size() + 4, written.size());
    EXPECT_EQ(0u, written[0]);
    EXPECT_EQ(0u, written[1]);
    EXPECT_EQ(8u, written[2]);
    EXPECT_EQ(0u, written[3]);
    EXPECT_EQ('q', written[4]);
    EXPECT_EQ('q', written.back());
}

TEST(PacketStreamFilterTests, partialBulkWriteWithControlBurstPreservesEveryFrame)
{
    NiceMock<MockEventQueue> events;
    PartialWritePriorityStream stream;
    const std::string bulkPayload(256 * 1024, 'b');

    EXPECT_CALL(events, removeHandlers(stream.getEventTarget()));
    EXPECT_CALL(events, adoptHandler(Event::kUnknown, stream.getEventTarget(), _));
    EXPECT_CALL(events, removeHandler(Event::kUnknown, stream.getEventTarget()));

    {
        PacketStreamFilter filter(&events, &stream, false);
        filter.writeLowPriority(bulkPayload.data(), bulkPayload.size());

        // Model a transport that writes only 4 KiB before control traffic arrives.
        stream.drainOnce(4 * 1024);
        for (size_t i = 0; i < 10000; ++i) {
            const UInt8 control = static_cast<UInt8>(i);
            filter.write(&control, 1);
        }
        stream.drainAll(4 * 1024);
    }

    std::vector<UInt32> lengths;
    ASSERT_TRUE(parsePacketLengths(stream.wire(), lengths));
    ASSERT_EQ(10001u, lengths.size());
    EXPECT_EQ(bulkPayload.size(), lengths.front());
    EXPECT_TRUE(std::all_of(lengths.begin() + 1, lengths.end(),
                            [](UInt32 length) { return length == 1; }));
}

TEST(PacketStreamFilterTests, writeLowPriority_oversizedPacket_emitsOutputErrorWithoutWriting)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    setupStreamEvents(events, streamEvents);
    const Event::Type outputErrorType = streamEvents.outputError();

    NiceMock<MockStream> stream;
    PacketStreamFilter* filterPtr = nullptr;

    ON_CALL(stream, getEventTarget()).WillByDefault(Return(reinterpret_cast<void*>(0x7)));
    EXPECT_CALL(events, removeHandlers(stream.getEventTarget()));
    EXPECT_CALL(events, adoptHandler(Event::kUnknown, stream.getEventTarget(), _));
    EXPECT_CALL(stream, write(_, _)).Times(0);
    EXPECT_CALL(stream, writeLowPriority(_, _)).Times(0);
    EXPECT_CALL(events, addEvent(_))
        .WillOnce(Invoke([&](const Event& event) {
            EXPECT_EQ(outputErrorType, event.getType());
            EXPECT_EQ(filterPtr, event.getTarget());
        }));
    EXPECT_CALL(events, removeHandler(Event::kUnknown, stream.getEventTarget()));

    {
        PacketStreamFilter filter(&events, &stream, false);
        filterPtr = &filter;
        filter.writeLowPriority(nullptr, PROTOCOL_MAX_MESSAGE_LENGTH + 1);
    }
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
