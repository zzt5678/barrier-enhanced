/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "server/ClientProxy1_7.h"

#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "base/IEventQueue.h"
#include "base/Log.h"

#include <cstring>

ClientProxy1_7::ClientProxy1_7(const std::string& name,
                               barrier::IStream* stream,
                               Server* server,
                               IEventQueue* events) :
    ClientProxy1_6(name, stream, server, events),
    m_events(events)
{
}

ClientProxy1_7::~ClientProxy1_7()
{
}

void
ClientProxy1_7::prepareEnter(SInt32 xAbs, SInt32 yAbs, UInt32 seqNum,
                            KeyModifierMask mask)
{
    LOG((CLOG_DEBUG1 "prepare enter for \"%s\", %d,%d %u %04x",
        getName().c_str(), xAbs, yAbs, seqNum, mask));
    ProtocolUtil::writef(getStream(), kMsgCPrepareEnter,
                         xAbs, yAbs, seqNum, mask);
}

void
ClientProxy1_7::abortEnter(UInt32 seqNum)
{
    LOG((CLOG_DEBUG1 "abort prepared enter for \"%s\", %u",
        getName().c_str(), seqNum));
    ProtocolUtil::writef(getStream(), kMsgCAbortEnter, seqNum);
}

bool
ClientProxy1_7::parseMessage(const UInt8* code)
{
    if (std::memcmp(code, kMsgDEnterReady, 4) == 0) {
        return recvInputHandoffReady();
    }
    return ClientProxy1_6::parseMessage(code);
}

bool
ClientProxy1_7::recvInputHandoffReady()
{
    UInt32 seqNum = 0;
    UInt8 ready = 0;
    ProtocolUtil::readf(getStream(), kMsgDEnterReady + 4, &seqNum, &ready);

    // Readiness and commit acknowledgments gate pointer ownership. They are
    // parsed on the event-loop thread, so dispatch them before later queued
    // motion can reverse or time out the handoff that they acknowledge.
    Event event(m_events->forClientProxy().inputHandoffReady(),
                getEventTarget(), NULL, Event::kDeliverImmediately);
    event.setDataObject(new InputHandoffReadyInfo(seqNum, ready != 0));
    m_events->addEvent(event);
    return true;
}
