/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#include "server/ClientProxy1_11.h"

#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "base/IEventQueue.h"
#include "base/Log.h"

#include <cstring>

ClientProxy1_11::ClientProxy1_11(const std::string& name,
                                 barrier::IStream* stream,
                                 Server* server,
                                 IEventQueue* events) :
    ClientProxy1_10(name, stream, server, events),
    m_revokeEvents(events)
{
}

ClientProxy1_11::~ClientProxy1_11()
{
}

void
ClientProxy1_11::requestInputLeaseRevoke(UInt32 handoffSeqNum,
                                         UInt32 inputEpoch)
{
    LOG((CLOG_DEBUG1
        "request input lease revoke from \"%s\", handoff=%u epoch=%u",
        getName().c_str(), handoffSeqNum, inputEpoch));
    ProtocolUtil::writef(getStream(), kMsgCRevokeInput,
                         handoffSeqNum, inputEpoch);
}

bool
ClientProxy1_11::parseMessage(const UInt8* code)
{
    if (std::memcmp(code, kMsgDRevokeInputAck, 4) == 0) {
        return recvInputLeaseRevokeAck();
    }
    return ClientProxy1_10::parseMessage(code);
}

bool
ClientProxy1_11::recvInputLeaseRevokeAck()
{
    UInt32 handoffSeqNum = 0;
    UInt8 revoked = 0;
    ProtocolUtil::readf(getStream(), kMsgDRevokeInputAck + 4,
                        &handoffSeqNum, &revoked);

    Event event(m_revokeEvents->forClientProxy().inputHandoffReady(),
                getEventTarget(), NULL, Event::kDeliverImmediately);
    event.setDataObject(new InputHandoffReadyInfo(
        handoffSeqNum, revoked != 0));
    m_revokeEvents->addEvent(event);
    return true;
}
