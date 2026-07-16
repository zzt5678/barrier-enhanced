/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "server/ClientProxy1_9.h"

//! Proxy for clients implementing protocol version 1.10.
class ClientProxy1_10 : public ClientProxy1_9 {
public:
    ClientProxy1_10(const std::string& name, barrier::IStream* adoptedStream,
                    Server* server, IEventQueue* events);
    ~ClientProxy1_10();

    bool supportsInputHandoffCommitAck() const override { return true; }
};
