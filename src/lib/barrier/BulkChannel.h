/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#pragma once

#include "base/Stopwatch.h"
#include "common/basic_types.h"
#include "base/Event.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

class Event;
class IEventQueue;
class EventQueueTimer;

namespace barrier {

class IStream;
class BulkChannel;

class IBulkChannelHandler {
public:
    virtual ~IBulkChannelHandler() { }
    virtual bool handleBulkMessage(const UInt8* code, IStream* stream) = 0;
    virtual void handleBulkDisconnected(
        BulkChannel* channel, std::uint64_t pausedGeneration = 0) = 0;
};

//! Owns a separately framed stream reserved for bulk payload messages.
class BulkChannel {
public:
    BulkChannel(IStream* adoptedStream, IBulkChannelHandler* handler,
                IEventQueue* events);
    ~BulkChannel();

    IStream* getStream() const { return m_stream; }
    bool isActive() const { return m_active; }

    //! Stop parsing buffered frames until the completed transfer is committed.
    bool pauseInputForCommit(std::uint64_t generation);
    void resumeInputAfterCommit(std::uint64_t generation);
    bool pauseInputForBackpressure(std::uint64_t generation);
    void resumeInputAfterBackpressure(std::uint64_t generation);
    std::function<void()> makeInputResumeCallback(
        std::uint64_t generation) const;
    std::function<void()> makeInputProgressCallback(
        std::uint64_t generation) const;

    //! Close the channel without notifying its detached owner.
    void close();

#ifdef BARRIER_TEST_ENV
    void handleDataForTest() { handleData(Event(), NULL); }
    void handleKeepAliveForTest() { handleKeepAlive(Event(), NULL); }
    void serviceInputPauseForTest(double elapsedSeconds)
    {
        serviceInputPause(elapsedSeconds);
    }
#endif

private:
    void addHandlers();
    void removeHandlers();
    void fail(const char* reason);
    void handleData(const Event&, void*);
    void handleDisconnect(const Event&, void*);
    void handleKeepAlive(const Event&, void*);
    void handleInputPauseTimer(const Event&, void*);
    bool pauseInput(std::uint64_t generation);
    void resumeInput(std::uint64_t generation);
    void serviceInputPause(double elapsedSeconds);
    void stopInputPauseTimer();
    void clearInputPause();

    struct InputPauseSignal;

    IStream* m_stream;
    IBulkChannelHandler* m_handler;
    IEventQueue* m_events;
    bool m_active;
    bool m_handlersInstalled;
    EventQueueTimer* m_keepAliveTimer;
    UInt32 m_unansweredKeepAlives;
    UInt32 m_lastBufferedOutput;
    UInt32 m_stalledOutputIntervals;
    std::uint64_t m_lastOutputBytesWritten;
    std::uint64_t m_lastInputBytesReceived;
    bool m_frameActivitySinceKeepAlive;
    std::atomic<std::uint64_t> m_inputPauseGeneration;
    EventQueueTimer* m_inputPauseTimer;
    Stopwatch m_inputPauseStopwatch;
    std::shared_ptr<InputPauseSignal> m_inputPauseSignal;
    std::uint64_t m_lastInputPauseProgress;
};

} // namespace barrier
