/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#pragma once

#include "barrier/BulkChannel.h"
#include "server/ClientProxy1_8.h"

#include <memory>

//! Proxy for clients implementing protocol version 1.9.
class ClientProxy1_9 : public ClientProxy1_8,
                       public barrier::IBulkChannelHandler {
public:
    ClientProxy1_9(const std::string& name, barrier::IStream* adoptedStream,
                   Server* server, IEventQueue* events);
    ~ClientProxy1_9();

    bool supportsBulkChannel() const override { return true; }
    void offerBulkChannel(const std::string& token) override;
    bool attachBulkChannel(barrier::IStream* stream) override;
    void detachBulkChannel() override;
    std::shared_ptr<barrier::BulkChannel> acquireBulkChannel() const override;

    bool handleBulkMessage(const UInt8* code,
                           barrier::IStream* stream) override;
    void handleBulkDisconnected(
        barrier::BulkChannel* channel,
        std::uint64_t pausedGeneration = 0) override;

private:
    Server* m_bulkServer;
    IEventQueue* m_bulkEvents;
    std::shared_ptr<barrier::BulkChannel> m_bulkChannel;
};
