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

namespace {

const size_t kMaxFramesPerInputBatch = 64;
const size_t kMaxBytesPerInputBatch = 256 * 1024;
const double kMaxSecondsPerInputBatch = 0.002;
const double kBulkKeepAliveSeconds = 2.0;
const UInt32 kMaxUnansweredBulkKeepAlives = 3;

}

namespace barrier {

BulkChannel::BulkChannel(IStream* stream, IBulkChannelHandler* handler,
                         IEventQueue* events) :
    m_stream(stream),
    m_handler(handler),
    m_events(events),
    m_active(true),
    m_handlersInstalled(false),
    m_keepAliveTimer(NULL),
    m_unansweredKeepAlives(0)
{
    assert(m_stream != NULL);
    assert(m_handler != NULL);
    assert(m_events != NULL);
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
    removeHandlers();
    m_active = false;
    m_stream->close();
    IBulkChannelHandler* handler = m_handler;
    if (handler != NULL) {
        handler->handleBulkDisconnected(this);
    }
}

void
BulkChannel::handleData(const Event&, void*)
{
    size_t parsedFrames = 0;
    size_t parsedBytes = 0;
    Stopwatch parseTimer;

    while (m_active) {
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
                m_unansweredKeepAlives = 0;
                ProtocolUtil::writef(m_stream, kMsgBulkKeepAliveAck);
            }
            else if (memcmp(code, kMsgBulkKeepAliveAck, 4) == 0) {
                m_unansweredKeepAlives = 0;
            }
            else {
                if (memcmp(code, kMsgDFileTransfer, 4) != 0 &&
                    memcmp(code, kMsgDClipboard, 4) != 0) {
                    fail("control message received on bulk stream");
                    return;
                }
                if (m_handler == NULL ||
                    !m_handler->handleBulkMessage(code, m_stream)) {
                    fail("invalid bulk payload message");
                    return;
                }
                m_unansweredKeepAlives = 0;
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
