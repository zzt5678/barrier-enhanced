/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "server/ClientProxy1_6.h"

class Server;
class IEventQueue;

//! Proxy for client implementing protocol version 1.7
class ClientProxy1_7 : public ClientProxy1_6 {
public:
    ClientProxy1_7(const std::string& name, barrier::IStream* adoptedStream,
                   Server* server, IEventQueue* events);
    ~ClientProxy1_7();

    bool supportsInputHandoff() const override { return true; }
    void prepareEnter(SInt32 xAbs, SInt32 yAbs, UInt32 seqNum,
                      KeyModifierMask mask) override;
    void abortEnter(UInt32 seqNum) override;
    bool parseMessage(const UInt8* code) override;

private:
    bool recvInputHandoffReady();

    IEventQueue* m_events;
};
