/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#include "barrier/BulkChannel.h"

#include "barrier/ProtocolUtil.h"
#include "barrier/XBarrier.h"
#include "barrier/protocol_types.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/Stopwatch.h"
#include "base/TMethodEventJob.h"
#include "io/IStream.h"

#include <cstring>
#include <exception>
#include <new>

namespace {

const size_t kMaxFramesPerInputBatch = 64;
const size_t kMaxBytesPerInputBatch = 256 * 1024;
const double kMaxSecondsPerInputBatch = 0.002;
const double kBulkKeepAliveSeconds = 2.0;
const double kInputPausePollSeconds = 0.01;
const double kInputPauseProgressTimeoutSeconds = 60.0;
const UInt32 kMaxUnansweredBulkKeepAlives = 3;
const UInt32 kMaxStalledBulkOutputIntervals = 15;

bool
isBulkPayloadMessage(const UInt8 code[4])
{
    return std::memcmp(code, kMsgDFileTransfer, 4) == 0 ||
        std::memcmp(code, kMsgDClipboard, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferData1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferEnd1_12, 4) == 0;
}

}

namespace barrier {

struct BulkChannel::InputPauseSignal {
    explicit InputPauseSignal(std::uint64_t value) :
        generation(value),
        resumeGeneration(0),
        progressSequence(0)
    {
    }

    const std::uint64_t generation;
    std::atomic<std::uint64_t> resumeGeneration;
    std::atomic<std::uint64_t> progressSequence;
};

BulkChannel::BulkChannel(IStream* stream, IBulkChannelHandler* handler,
                         IEventQueue* events) :
    m_stream(stream),
    m_handler(handler),
    m_events(events),
    m_active(true),
    m_handlersInstalled(false),
    m_keepAliveTimer(NULL),
    m_unansweredKeepAlives(0),
    m_lastBufferedOutput(0),
    m_stalledOutputIntervals(0),
    m_lastOutputBytesWritten(0),
    m_lastInputBytesReceived(0),
    m_frameActivitySinceKeepAlive(false),
    m_inputPauseGeneration(0),
    m_inputPauseTimer(NULL),
    m_inputPauseStopwatch(),
    m_inputPauseSignal(),
    m_lastInputPauseProgress(0)
{
    assert(m_stream != NULL);
    assert(m_handler != NULL);
    assert(m_events != NULL);
    m_lastBufferedOutput = m_stream->getBufferedOutputSize();
    m_lastOutputBytesWritten = m_stream->getOutputBytesWritten();
    m_lastInputBytesReceived = m_stream->getInputBytesReceived();
    addHandlers();
}

BulkChannel::~BulkChannel()
{
    close();
    delete m_stream;
}

void
BulkChannel::addHandlers()
{
    if (m_handlersInstalled) {
        return;
    }
    void* target = m_stream->getEventTarget();
    m_events->adoptHandler(m_events->forIStream().inputReady(), target,
        new TMethodEventJob<BulkChannel>(this, &BulkChannel::handleData));
    m_events->adoptHandler(m_events->forIStream().outputError(), target,
        new TMethodEventJob<BulkChannel>(this, &BulkChannel::handleDisconnect));
    m_events->adoptHandler(m_events->forIStream().inputShutdown(), target,
        new TMethodEventJob<BulkChannel>(this, &BulkChannel::handleDisconnect));
    m_events->adoptHandler(m_events->forIStream().inputFormatError(), target,
        new TMethodEventJob<BulkChannel>(this, &BulkChannel::handleDisconnect));
    m_events->adoptHandler(m_events->forIStream().outputShutdown(), target,
        new TMethodEventJob<BulkChannel>(this, &BulkChannel::handleDisconnect));
    m_keepAliveTimer = m_events->newTimer(kBulkKeepAliveSeconds, NULL);
    if (m_keepAliveTimer != NULL) {
        m_events->adoptHandler(Event::kTimer, m_keepAliveTimer,
            new TMethodEventJob<BulkChannel>(this, &BulkChannel::handleKeepAlive));
    }
    m_handlersInstalled = true;
}

void
BulkChannel::removeHandlers()
{
    stopInputPauseTimer();
    if (!m_handlersInstalled) {
        return;
    }
    void* target = m_stream->getEventTarget();
    m_events->removeHandler(m_events->forIStream().inputReady(), target);
    m_events->removeHandler(m_events->forIStream().outputError(), target);
    m_events->removeHandler(m_events->forIStream().inputShutdown(), target);
    m_events->removeHandler(m_events->forIStream().inputFormatError(), target);
    m_events->removeHandler(m_events->forIStream().outputShutdown(), target);
    if (m_keepAliveTimer != NULL) {
        m_events->removeHandler(Event::kTimer, m_keepAliveTimer);
        m_events->deleteTimer(m_keepAliveTimer);
        m_keepAliveTimer = NULL;
    }
    m_handlersInstalled = false;
}

void
BulkChannel::close()
{
    clearInputPause();
    removeHandlers();
    m_handler = NULL;
    if (m_active) {
        m_active = false;
        m_stream->close();
    }
}

void
BulkChannel::fail(const char* reason)
{
    if (!m_active) {
        return;
    }
    LOG((CLOG_WARN "bulk channel closed: %s", reason));
    const std::uint64_t pausedGeneration = m_inputPauseGeneration.load();
    clearInputPause();
    removeHandlers();
    m_active = false;
    m_stream->close();
    IBulkChannelHandler* handler = m_handler;
    if (handler != NULL) {
        try {
            handler->handleBulkDisconnected(this, pausedGeneration);
        }
        catch (const std::exception& e) {
            LOG((CLOG_ERR "bulk disconnect handler failed: %s", e.what()));
        }
        catch (...) {
            LOG((CLOG_ERR "bulk disconnect handler failed"));
        }
    }
}

void
BulkChannel::handleData(const Event&, void*)
{
    size_t parsedFrames = 0;
    size_t parsedBytes = 0;
    Stopwatch parseTimer;

    while (m_active && m_inputPauseGeneration.load() == 0) {
        const UInt32 frameSize = m_stream->getSize();
        UInt8 code[4];
        const UInt32 count = m_stream->read(code, 4);
        if (count == 0) {
            break;
        }
        if (count != 4) {
            fail("incomplete message code");
            return;
        }
        try {
            if (memcmp(code, kMsgBulkKeepAlive, 4) == 0) {
                ProtocolUtil::writef(m_stream, kMsgBulkKeepAliveAck);
            }
            else if (memcmp(code, kMsgBulkKeepAliveAck, 4) != 0) {
                if (!isBulkPayloadMessage(code)) {
                    fail("control message received on bulk stream");
                    return;
                }
                if (m_handler == NULL ||
                    !m_handler->handleBulkMessage(code, m_stream)) {
                    fail("invalid bulk payload message");
                    return;
                }
            }
        }
        catch (const XBase& e) {
            LOG((CLOG_WARN "bulk payload protocol error: %s", e.what()));
            fail("payload protocol error");
            return;
        }
        catch (const std::exception& e) {
            LOG((CLOG_WARN "bulk payload failure: %s", e.what()));
            fail("payload handler failure");
            return;
        }
        catch (...) {
            LOG((CLOG_WARN "bulk payload failed with an unknown exception"));
            fail("payload handler failure");
            return;
        }

        m_unansweredKeepAlives = 0;
        m_stalledOutputIntervals = 0;
        m_frameActivitySinceKeepAlive = true;

        ++parsedFrames;
        parsedBytes += frameSize >= 4 ? frameSize : 4;
        if (parsedFrames >= kMaxFramesPerInputBatch ||
            parsedBytes >= kMaxBytesPerInputBatch ||
            parseTimer.getTime() >= kMaxSecondsPerInputBatch) {
            if (m_stream->getSize() != 0) {
                m_events->addEvent(Event(m_events->forIStream().inputReady(),
                                         m_stream->getEventTarget()));
            }
            break;
        }
    }
}

bool
BulkChannel::pauseInputForCommit(std::uint64_t generation)
{
    return pauseInput(generation);
}

void
BulkChannel::resumeInputAfterCommit(std::uint64_t generation)
{
    resumeInput(generation);
}

bool
BulkChannel::pauseInputForBackpressure(std::uint64_t generation)
{
    return pauseInput(generation);
}

void
BulkChannel::resumeInputAfterBackpressure(std::uint64_t generation)
{
    resumeInput(generation);
}

bool
BulkChannel::pauseInput(std::uint64_t generation)
{
    if (!m_active || generation == 0) {
        return false;
    }
    std::uint64_t expected = 0;
    if (!m_inputPauseGeneration.compare_exchange_strong(expected, generation)) {
        return false;
    }

    try {
        m_inputPauseSignal = std::make_shared<InputPauseSignal>(generation);
        m_inputPauseTimer = m_events->newTimer(kInputPausePollSeconds, NULL);
        if (m_inputPauseTimer == NULL) {
            throw std::bad_alloc();
        }
        m_events->adoptHandler(Event::kTimer, m_inputPauseTimer,
            new TMethodEventJob<BulkChannel>(
                this, &BulkChannel::handleInputPauseTimer));
        m_stream->setInputPaused(true);
    }
    catch (...) {
        clearInputPause();
        return false;
    }

    m_lastInputPauseProgress = 0;
    m_inputPauseStopwatch.start();
    m_inputPauseStopwatch.reset();
    return true;
}

void
BulkChannel::resumeInput(std::uint64_t generation)
{
    const std::function<void()> resume = makeInputResumeCallback(generation);
    if (!resume) {
        return;
    }
    resume();
}

std::function<void()>
BulkChannel::makeInputResumeCallback(std::uint64_t generation) const
{
    if (generation == 0 ||
        m_inputPauseGeneration.load() != generation ||
        !m_inputPauseSignal || m_inputPauseSignal->generation != generation) {
        return std::function<void()>();
    }
    const std::shared_ptr<InputPauseSignal> signal = m_inputPauseSignal;
    return [signal, generation]() {
        signal->resumeGeneration.store(generation, std::memory_order_release);
    };
}

std::function<void()>
BulkChannel::makeInputProgressCallback(std::uint64_t generation) const
{
    if (generation == 0 ||
        m_inputPauseGeneration.load() != generation ||
        !m_inputPauseSignal || m_inputPauseSignal->generation != generation) {
        return std::function<void()>();
    }
    const std::shared_ptr<InputPauseSignal> signal = m_inputPauseSignal;
    return [signal]() {
        signal->progressSequence.fetch_add(1, std::memory_order_release);
    };
}

void
BulkChannel::serviceInputPause(double elapsedSeconds)
{
    if (!m_active) {
        return;
    }

    const std::uint64_t generation = m_inputPauseGeneration.load();
    const std::shared_ptr<InputPauseSignal> signal = m_inputPauseSignal;
    if (generation == 0 || !signal || signal->generation != generation) {
        return;
    }

    const std::uint64_t progress =
        signal->progressSequence.load(std::memory_order_acquire);
    if (progress != m_lastInputPauseProgress) {
        m_lastInputPauseProgress = progress;
        m_inputPauseStopwatch.reset();
        elapsedSeconds = 0.0;
    }

    if (signal->resumeGeneration.load(std::memory_order_acquire) == generation) {
        std::uint64_t expected = generation;
        if (!m_inputPauseGeneration.compare_exchange_strong(expected, 0)) {
            return;
        }
        stopInputPauseTimer();
        m_inputPauseSignal.reset();
        try {
            m_stream->setInputPaused(false);
            m_events->addEvent(Event(m_events->forIStream().inputReady(),
                                     m_stream->getEventTarget()));
        }
        catch (const std::exception& e) {
            LOG((CLOG_WARN "failed to queue resumed bulk input: %s", e.what()));
            fail("failed to queue resumed input");
        }
        catch (...) {
            fail("failed to queue resumed input");
        }
        return;
    }

    if (elapsedSeconds >= kInputPauseProgressTimeoutSeconds) {
        fail("input pause made no progress");
    }
}

void
BulkChannel::handleInputPauseTimer(const Event&, void*)
{
    serviceInputPause(m_inputPauseStopwatch.getTime());
}

void
BulkChannel::stopInputPauseTimer()
{
    if (m_inputPauseTimer != NULL) {
        m_events->removeHandler(Event::kTimer, m_inputPauseTimer);
        m_events->deleteTimer(m_inputPauseTimer);
        m_inputPauseTimer = NULL;
    }
}

void
BulkChannel::clearInputPause()
{
    const bool wasPaused = m_inputPauseGeneration.exchange(0) != 0;
    stopInputPauseTimer();
    m_inputPauseSignal.reset();
    m_lastInputPauseProgress = 0;
    if (wasPaused) {
        try {
            m_stream->setInputPaused(false);
        }
        catch (...) {
            // Channel teardown must continue even if an adapter rejects resume.
        }
    }
}

void
BulkChannel::handleDisconnect(const Event&, void*)
{
    fail("transport disconnected");
}

void
BulkChannel::handleKeepAlive(const Event&, void*)
{
    if (!m_active) {
        return;
    }

    if (m_inputPauseGeneration.load() != 0) {
        m_unansweredKeepAlives = 0;
        m_stalledOutputIntervals = 0;
        m_frameActivitySinceKeepAlive = false;
        m_lastBufferedOutput = m_stream->getBufferedOutputSize();
        m_lastOutputBytesWritten = m_stream->getOutputBytesWritten();
        m_lastInputBytesReceived = m_stream->getInputBytesReceived();
        try {
            // Input is paused at a frame boundary, so incoming probes cannot
            // be parsed yet.  Keep the peer alive with an outbound probe while
            // the local spool/commit watchdog retains the failure deadline.
            ProtocolUtil::writef(m_stream, kMsgBulkKeepAlive);
        }
        catch (const XBase& e) {
            LOG((CLOG_WARN "bulk keepalive failed while input paused: %s",
                e.what()));
            fail("keepalive write failed while input paused");
        }
        catch (const std::exception& e) {
            LOG((CLOG_WARN "bulk keepalive failed while input paused: %s",
                e.what()));
            fail("keepalive write failed while input paused");
        }
        catch (...) {
            fail("keepalive write failed while input paused");
        }
        return;
    }

    const UInt32 bufferedOutput = m_stream->getBufferedOutputSize();
    const std::uint64_t outputBytesWritten = m_stream->getOutputBytesWritten();
    const std::uint64_t inputBytesReceived = m_stream->getInputBytesReceived();
    const bool outputProgress = m_lastBufferedOutput > 0 &&
        bufferedOutput < m_lastBufferedOutput;
    const bool observedOutputBacklog = m_lastBufferedOutput > 0 ||
        bufferedOutput > 0;
    const bool writerProgress = observedOutputBacklog &&
        outputBytesWritten > m_lastOutputBytesWritten;
    const bool inputProgress =
        inputBytesReceived > m_lastInputBytesReceived;
    const bool outputBacklogStarted = m_lastBufferedOutput == 0 &&
        bufferedOutput > 0;
    const bool madeProgress = m_frameActivitySinceKeepAlive ||
        inputProgress || outputProgress || writerProgress;
    m_frameActivitySinceKeepAlive = false;
    m_lastBufferedOutput = bufferedOutput;
    m_lastOutputBytesWritten = outputBytesWritten;
    m_lastInputBytesReceived = inputBytesReceived;

    if (madeProgress) {
        m_unansweredKeepAlives = 0;
        m_stalledOutputIntervals = 0;
        return;
    }

    if (bufferedOutput > 0) {
        m_unansweredKeepAlives = 0;
        if (outputBacklogStarted) {
            m_stalledOutputIntervals = 0;
            return;
        }
        if (++m_stalledOutputIntervals >= kMaxStalledBulkOutputIntervals) {
            fail("output backlog stalled");
        }
        return;
    }

    m_stalledOutputIntervals = 0;
    if (m_unansweredKeepAlives >= kMaxUnansweredBulkKeepAlives) {
        fail("keepalive timeout");
        return;
    }

    try {
        ProtocolUtil::writef(m_stream, kMsgBulkKeepAlive);
        ++m_unansweredKeepAlives;
    }
    catch (const XBase& e) {
        LOG((CLOG_WARN "bulk keepalive failed: %s", e.what()));
        fail("keepalive write failed");
    }
    catch (const std::exception& e) {
        LOG((CLOG_WARN "bulk keepalive failed: %s", e.what()));
        fail("keepalive write failed");
    }
    catch (...) {
        fail("keepalive write failed");
    }
}

} // namespace barrier
