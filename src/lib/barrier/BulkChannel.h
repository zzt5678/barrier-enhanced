/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#pragma once

#include "common/basic_types.h"
#include "base/Event.h"

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
    virtual void handleBulkDisconnected(BulkChannel* channel) = 0;
};

//! Owns a separately framed stream reserved for bulk payload messages.
class BulkChannel {
public:
    BulkChannel(IStream* adoptedStream, IBulkChannelHandler* handler,
                IEventQueue* events);
    ~BulkChannel();

    IStream* getStream() const { return m_stream; }
    bool isActive() const { return m_active; }

    //! Close the channel without notifying its detached owner.
    void close();

#ifdef BARRIER_TEST_ENV
    void handleDataForTest() { handleData(Event(), NULL); }
    void handleKeepAliveForTest() { handleKeepAlive(Event(), NULL); }
#endif

private:
    void addHandlers();
    void removeHandlers();
    void fail(const char* reason);
    void handleData(const Event&, void*);
    void handleDisconnect(const Event&, void*);
    void handleKeepAlive(const Event&, void*);

    IStream* m_stream;
    IBulkChannelHandler* m_handler;
    IEventQueue* m_events;
    bool m_active;
    bool m_handlersInstalled;
    EventQueueTimer* m_keepAliveTimer;
    UInt32 m_unansweredKeepAlives;
};

} // namespace barrier
