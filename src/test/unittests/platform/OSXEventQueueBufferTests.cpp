#include "platform/OSXEventQueueBuffer.h"
#include "base/Event.h"
#include "base/EventQueue.h"
#include "test/global/gtest.h"
#include <chrono>
#include <thread>
#include <atomic>

TEST(OSXEventQueueBufferTests, internalEventsRemainOrderedOutsideSharedCarbonQueue)
{
    OSXEventQueueBuffer buffer(nullptr);
    buffer.init();
    for (UInt32 id = 1; id <= 1000; ++id) ASSERT_TRUE(buffer.addEvent(id));
    // Cocoa uses the same Carbon queue. Draining it must not steal internal
    // connection, input-ready, or quit events from the worker event loop.
    EventRef native = nullptr;
    while (ReceiveNextEvent(0, nullptr, 0, true, &native) == noErr) {
        ReleaseEvent(native);
    }
    Event event;
    for (UInt32 expected = 1; expected <= 1000; ++expected) {
        UInt32 actual = 0;
        ASSERT_EQ(IEventQueueBuffer::kUser, buffer.getEvent(event, actual));
        EXPECT_EQ(expected, actual);
    }
}

TEST(OSXEventQueueBufferTests, pendingEventWakesWaitAndAllowsShutdown)
{
    OSXEventQueueBuffer buffer(nullptr);
    buffer.init();
    // Flush the platform's initialization events without touching input taps.
    EventRef native = nullptr;
    while (ReceiveNextEvent(0, nullptr, 0, true, &native) == noErr) ReleaseEvent(native);
    std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        buffer.addEvent(42);
    });
    const auto start = std::chrono::steady_clock::now();
    // Bounded failure deadline so a broken wakeup cannot hang the test suite.
    buffer.waitForEvent(1.0);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    producer.join();
    EXPECT_GE(elapsed, 0.025);
    EXPECT_LT(elapsed, 0.8);
    Event event; UInt32 id = 0;
    EXPECT_EQ(IEventQueueBuffer::kUser, buffer.getEvent(event, id));
    EXPECT_EQ(42u, id);
}

TEST(OSXEventQueueBufferTests, emptyWaitHonorsFiniteTimeout)
{
    OSXEventQueueBuffer buffer(nullptr);
    buffer.init();
    EventRef native = nullptr;
    while (ReceiveNextEvent(0, nullptr, 0, true, &native) == noErr) ReleaseEvent(native);
    const auto start = std::chrono::steady_clock::now();
    buffer.waitForEvent(0.03);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    EXPECT_GE(elapsed, 0.015);
    EXPECT_LT(elapsed, 0.5);
}

TEST(OSXEventQueueBufferTests, concurrentProducersPreserveEachSequence)
{
    OSXEventQueueBuffer buffer(nullptr); buffer.init();
    auto post = [&](UInt32 prefix) {
        for (UInt32 i = 1; i <= 1000; ++i) buffer.addEvent(prefix | i);
    };
    std::thread first(post, 0), second(post, 0x10000);
    first.join(); second.join();
    UInt32 counts[2] = {}; Event event;
    for (unsigned i = 0; i < 2000; ++i) {
        UInt32 id = 0;
        ASSERT_EQ(IEventQueueBuffer::kUser, buffer.getEvent(event, id));
        const unsigned producer = id >> 16;
        ASSERT_LT(producer, 2u);
        EXPECT_EQ(++counts[producer], id & 0xffff);
    }
    EXPECT_EQ(1000u, counts[0]); EXPECT_EQ(1000u, counts[1]);
}

TEST(OSXEventQueueBufferTests, nativeCarbonEventIsStillReturnedAsSystemEvent)
{
    EventQueue events;
    OSXEventQueueBuffer buffer(&events); buffer.init();
    EventRef native = nullptr;
    while (ReceiveNextEvent(0, nullptr, 0, true, &native) == noErr) ReleaseEvent(native);
    ASSERT_EQ(noErr, CreateEvent(nullptr, 'WvTs', 1, 0, kEventAttributeNone, &native));
    ASSERT_EQ(noErr, PostEventToQueue(GetCurrentEventQueue(), native, kEventPriorityStandard));
    ReleaseEvent(native);
    EXPECT_FALSE(buffer.isEmpty());
    Event event; UInt32 id = 0;
    ASSERT_EQ(IEventQueueBuffer::kSystem, buffer.getEvent(event, id));
    EXPECT_EQ('WvTs', GetEventClass(*static_cast<EventRef*>(event.getData())));
}
