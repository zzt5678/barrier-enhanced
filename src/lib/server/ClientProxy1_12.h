/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#pragma once

#include "barrier/FileTransferProtocol.h"
#include "barrier/FileTransferReceiver.h"
#include "server/ClientProxy1_11.h"
#include "server/Server.h"

#include <atomic>
#include <memory>

//! Proxy for clients implementing protocol version 1.12.
class ClientProxy1_12 : public ClientProxy1_11 {
public:
    ClientProxy1_12(const std::string& name, barrier::IStream* adoptedStream,
                    Server* server, IEventQueue* events);
    ~ClientProxy1_12();

    bool supportsTransactionalFileTransfer() const override { return true; }
    std::string getConnectionBinding() const override
        { return m_connectionBinding; }
    void offerBulkChannel(const std::string& token) override;
    bool attachBulkChannel(barrier::IStream* stream) override;
    void detachBulkChannel() override;
    bool parseMessage(const UInt8* code) override;
    bool handleBulkMessage(const UInt8* code,
                           barrier::IStream* stream) override;
    void handleBulkDisconnected(
        barrier::BulkChannel* channel,
        std::uint64_t pausedGeneration = 0) override;

    //! Route one primary-initiated transaction frame to its mandated link.
    bool sendTransactionalFileFrame(
        const barrier::FileTransferFrame& frame,
        bool startAcknowledged);

#ifdef BARRIER_TEST_ENV
    bool testHasTransactionalReceive() const
        { return m_receiver.hasActiveTransfer(); }
    UInt32 testTransactionalReceiveId() const
        { return m_receiver.activeTransferId(); }
    bool testTransactionalReceiveCleanupPending() const
        { return m_receiver.workerCleanupPending(); }
    bool testTransactionalCancelAckPending() const
        { return m_receiveCancelAckPending; }
    bool testTransactionalRetiredWorkerPending() const
        { return m_receiver.session().retiredWorkerPending(); }
    std::uint64_t testTransactionalReceiveGeneration() const
        { return m_receiver.sessionGeneration(); }
    void testSetTransactionalReceiveWorkerExitGate(
        const std::shared_ptr<std::atomic<bool> >& gate)
    {
        const_cast<FileReceiveSession&>(m_receiver.session())
            .testSetWorkerExitGate(gate);
    }
    void testPollTransactionalFileReceive()
        { handleReceiveCompletionPoll(Event(), NULL); }
    void testFireTransactionalFileReceiveTimer()
    {
        if (m_receiveCompletionTimer != NULL) {
            handleReceiveCompletionPoll(Event(), NULL);
        }
    }
    bool testHasTransactionalFileReceiveTimer() const
        { return m_receiveCompletionTimer != NULL; }
#endif

private:
    bool handleControlTransferMessage(const UInt8* code);
    bool handleReceiveFrame(
        const barrier::FileTransferFrame& frame,
        const std::shared_ptr<barrier::BulkChannel>& channel);
    bool pauseReceiveForBackpressure(
        const std::shared_ptr<barrier::BulkChannel>& channel,
        std::uint64_t generation);
    bool pauseReceiveForCommit(
        const std::shared_ptr<barrier::BulkChannel>& channel,
        std::uint64_t generation);
    bool finishReceive(UInt32 transferId);
    bool scheduleReceiveCompletionPoll(UInt32 transferId);
    void handleReceiveCompletionPoll(const Event&, void*);
    void cleanupReceiveCompletionPoll();
    void resetReceiveCompletionPollBudget();
    bool quarantineReceiveRoute(UInt32 transferId, bool cancelled);
    void releaseReceiveRouteForBulkClosure(
        barrier::BulkChannel* channel);
    void resetReceiveRoute();
    bool sendReceiveAck(
        barrier::FileTransferFrameType type,
        UInt32 transferId,
        barrier::FileTransferReason reason);

    std::string m_connectionBinding;
    Server* m_transactionServer;
    IEventQueue* m_transactionEvents;
    barrier::FileTransferReceiver m_receiver;
    Server::TransactionalFileReceiveContext m_receiveContext;
    bool m_receiveContextValid;
    EventQueueTimer* m_receiveCompletionTimer;
    UInt32 m_receiveCompletionTransferId;
    bool m_receiveCancelAckPending;
    UInt32 m_receivePollBudgetTransferId;
    double m_receivePollElapsed;
    double m_receiveNextPollDelay;
    std::weak_ptr<barrier::BulkChannel> m_receiveRoute;
    std::shared_ptr<barrier::BulkChannel> m_receivePausedChannel;
    std::uint64_t m_receivePausedGeneration;
};
