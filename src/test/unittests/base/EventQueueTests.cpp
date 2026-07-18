/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "base/EventQueue.h"

#include "base/IEventJob.h"
#include "base/EventTypes.h"
#include "base/SimpleEventQueueBuffer.h"
#include "arch/Arch.h"
#include "mt/Thread.h"

#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"

#include <cstring>
#include <new>
#include <vector>

namespace {

class CountingEventData : public EventData {
public:
    explicit CountingEventData(int* deletedCount) :
        m_deletedCount(deletedCount)
    {
    }

    ~CountingEventData()
    {
        ++(*m_deletedCount);
    }

private:
    int* m_deletedCount;
};

class QuitAfterDispatchJob : public IEventJob {
public:
    QuitAfterDispatchJob(EventQueue* events, int* dispatchCount) :
        m_events(events),
        m_dispatchCount(dispatchCount)
    {
    }

    void run(const Event&) override
    {
        ++(*m_dispatchCount);
        m_events->addEvent(Event(Event::kQuit));
    }

private:
    EventQueue* m_events;
    int* m_dispatchCount;
};

class CountingEventJob : public IEventJob {
public:
    explicit CountingEventJob(int* deletedCount) :
        m_deletedCount(deletedCount)
    {
    }

    ~CountingEventJob()
    {
        ++(*m_deletedCount);
    }

    void run(const Event&) override
    {
    }

private:
    int* m_deletedCount;
};

class CountingTimerBuffer : public SimpleEventQueueBuffer {
public:
    explicit CountingTimerBuffer(int* deletedCount) :
        m_deletedCount(deletedCount)
    {
    }

    EventQueueTimer* newTimer(double, bool) const override
    {
        return reinterpret_cast<EventQueueTimer*>(new char);
    }

    void deleteTimer(EventQueueTimer* timer) const override
    {
        ++(*m_deletedCount);
        delete reinterpret_cast<char*>(timer);
    }

private:
    int* m_deletedCount;
};

class AlwaysReadySystemBuffer : public SimpleEventQueueBuffer {
public:
    AlwaysReadySystemBuffer() :
        m_getEventCalls(0)
    {
    }

    bool isEmpty() const override
    {
        return false;
    }

    Type getEvent(Event& event, UInt32&) override
    {
        ++m_getEventCalls;
        event = Event(Event::kSystem);
        return kSystem;
    }

    int getEventCalls() const
    {
        return m_getEventCalls;
    }

private:
    int m_getEventCalls;
};

class RecordOrderJob : public IEventJob {
public:
    RecordOrderJob(EventQueue* events, std::vector<int>* order, int value,
                   bool quitAfterDispatch) :
        m_events(events),
        m_order(order),
        m_value(value),
        m_quitAfterDispatch(quitAfterDispatch)
    {
    }

    void run(const Event&) override
    {
        m_order->push_back(m_value);
        if (m_quitAfterDispatch) {
            m_events->addEvent(Event(Event::kQuit));
        }
    }

private:
    EventQueue* m_events;
    std::vector<int>* m_order;
    int m_value;
    bool m_quitAfterDispatch;
};

class ExpireTimerAndAddImmediateEventJob : public IEventJob {
public:
    ExpireTimerAndAddImmediateEventJob(EventQueue* events,
                                       std::vector<int>* order,
                                       Event::Type immediateType,
                                       void* immediateTarget,
                                       void* timerTarget,
                                       EventQueueTimer** timer) :
        m_events(events),
        m_order(order),
        m_immediateType(immediateType),
        m_immediateTarget(immediateTarget),
        m_timerTarget(timerTarget),
        m_timer(timer)
    {
    }

    void run(const Event&) override
    {
        m_order->push_back(1);
        *m_timer = m_events->newOneShotTimer(0.001, m_timerTarget);
        ARCH->sleep(0.01);
        m_events->addEvent(Event(m_immediateType, m_immediateTarget, NULL,
                                 Event::kDeliverImmediately));
    }

private:
    EventQueue* m_events;
    std::vector<int>* m_order;
    Event::Type m_immediateType;
    void* m_immediateTarget;
    void* m_timerTarget;
    EventQueueTimer** m_timer;
};

}

TEST(EventQueueTests, destructorDeletesPendingEventData)
{
    int deletedCount = 0;

    {
        EventQueue events;
        Event::Type pendingType = Event::kUnknown;
        events.registerTypeOnce(pendingType, "testPendingDestructorEvent");

        Event event(pendingType, &events);
        event.setDataObject(new CountingEventData(&deletedCount));
        events.addEvent(event);
    }

    EXPECT_EQ(1, deletedCount);
}

TEST(EventTypesTests, streamInputFormatErrorRegistersAUniqueNamedType)
{
    alignas(IStreamEvents) unsigned char storage[sizeof(IStreamEvents)];
    std::memset(storage, 0xa5, sizeof(storage));
    IStreamEvents* streamEvents = new (storage) IStreamEvents;
    EventQueue events;
    streamEvents->setEvents(&events);

    const Event::Type types[] = {
        streamEvents->inputReady(),
        streamEvents->outputFlushed(),
        streamEvents->outputError(),
        streamEvents->inputShutdown(),
        streamEvents->outputShutdown(),
        streamEvents->inputFormatError()
    };

    EXPECT_STREQ("IStreamEvents::inputFormatError",
                 events.getTypeName(types[5]));
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        EXPECT_GE(types[i], static_cast<Event::Type>(Event::kLast));
        for (size_t j = i + 1; j < sizeof(types) / sizeof(types[0]); ++j) {
            EXPECT_NE(types[i], types[j]);
        }
    }

    streamEvents->~IStreamEvents();
}

TEST(EventTypesTests, sameAccessorNameKeepsItsEventDomain)
{
    EventQueue events;
    ClientEvents clientEvents;
    IDataSocketEvents socketEvents;
    clientEvents.setEvents(&events);
    socketEvents.setEvents(&events);

    const Event::Type clientConnected = clientEvents.connected();
    const Event::Type socketConnected = socketEvents.connected();

    EXPECT_NE(clientConnected, socketConnected);
    EXPECT_STREQ("ClientEvents::connected",
                 events.getTypeName(clientConnected));
    EXPECT_STREQ("IDataSocketEvents::connected",
                 events.getTypeName(socketConnected));
    EXPECT_EQ(clientConnected,
              events.getRegisteredType("ClientEvents::connected"));
    EXPECT_EQ(socketConnected,
              events.getRegisteredType("IDataSocketEvents::connected"));
}

TEST(EventQueueTests, destructorDeletesAdoptedHandlers)
{
    int deletedCount = 0;

    {
        EventQueue events;
        Event::Type type = Event::kUnknown;
        int firstTarget = 0;
        int secondTarget = 0;
        events.registerTypeOnce(type, "testDestructorHandler");
        events.adoptHandler(type, &firstTarget, new CountingEventJob(&deletedCount));
        events.adoptHandler(type, &secondTarget, new CountingEventJob(&deletedCount));
    }

    EXPECT_EQ(2, deletedCount);
}

TEST(MockEventQueueTests, defaultActionsReleaseTransferredOwnership)
{
    int deletedDataCount = 0;
    int deletedHandlerCount = 0;

    {
        ::testing::NiceMock<MockEventQueue> events;
        int target = 0;
        events.adoptHandler(Event::kLast, &target,
                            new CountingEventJob(&deletedHandlerCount));

        Event event(Event::kLast, &target);
        event.setDataObject(new CountingEventData(&deletedDataCount));
        events.addEvent(event);
    }

    EXPECT_EQ(1, deletedDataCount);
    EXPECT_EQ(1, deletedHandlerCount);
}

TEST(MockEventQueueTests, defaultRemoveHandlerReleasesAdoptedJobImmediately)
{
    int deletedHandlerCount = 0;
    ::testing::NiceMock<MockEventQueue> events;
    int target = 0;
    events.adoptHandler(Event::kLast, &target,
                        new CountingEventJob(&deletedHandlerCount));

    events.removeHandler(Event::kLast, &target);

    EXPECT_EQ(1, deletedHandlerCount);
}

TEST(MockEventQueueTests, defaultRegisterTypeOnceReturnsStableUsableType)
{
    ::testing::NiceMock<MockEventQueue> events;
    Event::Type type = Event::kUnknown;

    const Event::Type first = events.registerTypeOnce(type, "mockEventType");
    const Event::Type second = events.registerTypeOnce(type, "mockEventType");

    EXPECT_GE(first, static_cast<Event::Type>(Event::kLast));
    EXPECT_EQ(first, type);
    EXPECT_EQ(first, second);
}

TEST(EventQueueTests, destructorDeletesOutstandingTimersThroughTheirBuffer)
{
    int deletedCount = 0;

    {
        EventQueue events;
        events.adoptBuffer(new CountingTimerBuffer(&deletedCount));
        events.newTimer(1.0, NULL);
        events.newOneShotTimer(1.0, NULL);
    }

    EXPECT_EQ(2, deletedCount);
}

TEST(EventQueueTests, expiredTimerPreemptsContinuouslyReadySystemBuffer)
{
    EventQueue events;
    AlwaysReadySystemBuffer* buffer = new AlwaysReadySystemBuffer;
    events.adoptBuffer(buffer);

    int timerTarget = 0;
    EventQueueTimer* timer = events.newOneShotTimer(0.001, &timerTarget);
    ARCH->sleep(0.01);

    Event event;
    ASSERT_TRUE(events.getEvent(event, 0.0));
    EXPECT_EQ(Event::kTimer, event.getType());
    EXPECT_EQ(&timerTarget, event.getTarget());
    EXPECT_EQ(0, buffer->getEventCalls());

    events.deleteTimer(timer);
}

TEST(EventQueueTests, immediateEventDispatchesBeforeExpiredTimerAndQueuedMotion)
{
    EventQueue events;
    Event::Type inputType = Event::kUnknown;
    Event::Type immediateType = Event::kUnknown;
    Event::Type motionType = Event::kUnknown;
    events.registerTypeOnce(inputType, "testImmediateInputEvent");
    events.registerTypeOnce(immediateType, "testImmediateAckEvent");
    events.registerTypeOnce(motionType, "testQueuedMotionEvent");

    std::vector<int> dispatchOrder;
    int eventTarget = 0;
    int timerTarget = 0;
    EventQueueTimer* timer = NULL;
    events.adoptHandler(
        inputType, &eventTarget,
        new ExpireTimerAndAddImmediateEventJob(
            &events, &dispatchOrder, immediateType, &eventTarget,
            &timerTarget, &timer));
    events.adoptHandler(
        immediateType, &eventTarget,
        new RecordOrderJob(&events, &dispatchOrder, 2, false));
    events.adoptHandler(
        Event::kTimer, &timerTarget,
        new RecordOrderJob(&events, &dispatchOrder, 3, false));
    events.adoptHandler(
        motionType, &eventTarget,
        new RecordOrderJob(&events, &dispatchOrder, 4, false));

    events.addEvent(Event(inputType, &eventTarget));
    events.addEvent(Event(motionType, &eventTarget));
    events.addEvent(Event(Event::kQuit));
    events.loop();

    ASSERT_NE(static_cast<EventQueueTimer*>(NULL), timer);
    ASSERT_EQ(4u, dispatchOrder.size());
    EXPECT_EQ(1, dispatchOrder[0]);
    EXPECT_EQ(2, dispatchOrder[1]);
    EXPECT_EQ(3, dispatchOrder[2]);
    EXPECT_EQ(4, dispatchOrder[3]);

    events.deleteTimer(timer);
}

TEST(EventQueueTests, loopDispatchesEventsQueuedBeforeReady)
{
    EventQueue events;
    Event::Type pendingType = Event::kUnknown;
    events.registerTypeOnce(pendingType, "testPendingDispatchEvent");

    int dispatchCount = 0;
    int target = 0;
    events.adoptHandler(pendingType, &target,
                        new QuitAfterDispatchJob(&events, &dispatchCount));

    events.addEvent(Event(pendingType, &target));
    events.loop();

    EXPECT_EQ(1, dispatchCount);
}

TEST(EventQueueTests, loopDispatchesEventAddedFromAnotherThreadAfterReady)
{
    EventQueue events;
    Event::Type concurrentType = Event::kUnknown;
    events.registerTypeOnce(concurrentType, "testConcurrentDispatchEvent");

    int dispatchCount = 0;
    int target = 0;
    events.adoptHandler(concurrentType, &target,
                        new QuitAfterDispatchJob(&events, &dispatchCount));

    Thread loopThread([&events]() {
        events.loop();
    });
    events.waitForReady();

    events.addEvent(Event(concurrentType, &target));

    if (!loopThread.wait(2.0)) {
        events.addEvent(Event(Event::kQuit));
        ASSERT_TRUE(loopThread.wait(2.0));
    }

    EXPECT_EQ(1, dispatchCount);
}

TEST(EventQueueTests, loopPreservesStartupPendingBeforePostReadyEvents)
{
    EventQueue events;
    Event::Type pendingType = Event::kUnknown;
    Event::Type readyType = Event::kUnknown;
    events.registerTypeOnce(pendingType, "testStartupPendingOrderEvent");
    events.registerTypeOnce(readyType, "testPostReadyOrderEvent");

    std::vector<int> dispatchOrder;
    int target = 0;
    events.adoptHandler(pendingType, &target,
                        new RecordOrderJob(&events, &dispatchOrder, 1, false));
    events.adoptHandler(readyType, &target,
                        new RecordOrderJob(&events, &dispatchOrder, 2, true));

    events.addEvent(Event(pendingType, &target));

    Thread loopThread([&events]() {
        events.loop();
    });
    events.waitForReady();

    events.addEvent(Event(readyType, &target));

    if (!loopThread.wait(2.0)) {
        events.addEvent(Event(Event::kQuit));
        ASSERT_TRUE(loopThread.wait(2.0));
    }

    ASSERT_EQ(2u, dispatchOrder.size());
    EXPECT_EQ(1, dispatchOrder[0]);
    EXPECT_EQ(2, dispatchOrder[1]);
}

TEST(EventQueueTests, loopCoalescesDuplicateFileKeepAliveEventsForSameTarget)
{
    EventQueue events;
    const Event::Type keepAliveType = events.forFile().keepAlive();

    std::vector<int> dispatchOrder;
    int target = 0;
    events.adoptHandler(keepAliveType, &target,
                        new RecordOrderJob(&events, &dispatchOrder, 1, false));

    events.addEvent(Event(keepAliveType, &target));
    events.addEvent(Event(keepAliveType, &target));
    events.addEvent(Event(keepAliveType, &target));
    events.addEvent(Event(Event::kQuit));

    events.loop();

    ASSERT_EQ(1u, dispatchOrder.size());
    EXPECT_EQ(1, dispatchOrder[0]);
}

TEST(EventQueueTests, loopPreservesFileKeepAliveEventsForDifferentTargets)
{
    EventQueue events;
    const Event::Type keepAliveType = events.forFile().keepAlive();

    std::vector<int> dispatchOrder;
    int target1 = 0;
    int target2 = 0;
    events.adoptHandler(keepAliveType, &target1,
                        new RecordOrderJob(&events, &dispatchOrder, 1, false));
    events.adoptHandler(keepAliveType, &target2,
                        new RecordOrderJob(&events, &dispatchOrder, 2, false));

    events.addEvent(Event(keepAliveType, &target1));
    events.addEvent(Event(keepAliveType, &target2));
    events.addEvent(Event(keepAliveType, &target1));
    events.addEvent(Event(Event::kQuit));

    events.loop();

    ASSERT_EQ(2u, dispatchOrder.size());
    EXPECT_EQ(1, dispatchOrder[0]);
    EXPECT_EQ(2, dispatchOrder[1]);
}
