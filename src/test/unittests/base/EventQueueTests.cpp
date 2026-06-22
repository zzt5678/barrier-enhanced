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
#include "mt/Thread.h"

#include "test/global/gtest.h"

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
