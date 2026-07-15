/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#include "server/ClientProxy1_9.h"

#include "server/Server.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "base/Log.h"
#include "io/IStream.h"

#include <cstring>

ClientProxy1_9::ClientProxy1_9(const std::string& name,
                               barrier::IStream* stream,
                               Server* server,
                               IEventQueue* events) :
    ClientProxy1_8(name, stream, server, events),
    m_bulkServer(server),
    m_bulkEvents(events),
    m_bulkChannel()
{
}

ClientProxy1_9::~ClientProxy1_9()
{
    detachBulkChannel();
}

void
ClientProxy1_9::offerBulkChannel(const std::string& token)
{
    ProtocolUtil::writef(getStream(), kMsgCBulkOffer, &token);
}

bool
ClientProxy1_9::attachBulkChannel(barrier::IStream* stream)
{
    if (stream == NULL) {
        return false;
    }
    detachBulkChannel();
    ProtocolUtil::writef(stream, kMsgDBulkAccepted);
    m_bulkChannel = std::make_shared<barrier::BulkChannel>(stream, this,
                                                           m_bulkEvents);
    LOG((CLOG_NOTE "bulk channel attached for client \"%s\"",
         getName().c_str()));
    if (stream->isReady()) {
        m_bulkEvents->addEvent(Event(m_bulkEvents->forIStream().inputReady(),
                                     stream->getEventTarget()));
    }
    return true;
}

void
ClientProxy1_9::detachBulkChannel()
{
    if (m_bulkChannel) {
        m_bulkChannel->close();
        m_bulkChannel.reset();
    }
}

std::shared_ptr<barrier::BulkChannel>
ClientProxy1_9::acquireBulkChannel() const
{
    if (m_bulkChannel && m_bulkChannel->isActive()) {
        return m_bulkChannel;
    }
    return std::shared_ptr<barrier::BulkChannel>();
}

bool
ClientProxy1_9::handleBulkMessage(const UInt8* code,
                                  barrier::IStream* stream)
{
    if (memcmp(code, kMsgDFileTransfer, 4) == 0) {
        fileChunkReceived(stream);
        return true;
    }
    if (memcmp(code, kMsgDClipboard, 4) == 0) {
        return recvClipboard(stream);
    }
    return false;
}

void
ClientProxy1_9::handleBulkDisconnected(barrier::BulkChannel* channel)
{
    if (!m_bulkChannel || m_bulkChannel.get() != channel) {
        return;
    }
    LOG((CLOG_WARN "bulk channel lost for client \"%s\"; control connection remains active",
         getName().c_str()));
    m_bulkServer->renewBulkChannel(this);
}
