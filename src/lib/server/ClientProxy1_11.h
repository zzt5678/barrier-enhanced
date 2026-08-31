/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#pragma once

#include "server/ClientProxy1_10.h"

//! Proxy for clients implementing protocol version 1.11.
class ClientProxy1_11 : public ClientProxy1_10 {
public:
    ClientProxy1_11(const std::string& name, barrier::IStream* adoptedStream,
                    Server* server, IEventQueue* events);
    ~ClientProxy1_11();

    bool supportsInputLeaseRevokeAck() const override { return true; }
    void requestInputLeaseRevoke(UInt32 handoffSeqNum,
                                 UInt32 inputEpoch) override;
    bool parseMessage(const UInt8* code) override;

private:
    bool recvInputLeaseRevokeAck();

    IEventQueue* m_revokeEvents;
};
