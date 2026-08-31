/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#include "server/ClientProxy1_12.h"

#include "barrier/BulkChannel.h"
#include "barrier/FileTransferProtocol.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/SecureRandom.h"
#include "barrier/protocol_types.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/TMethodEventJob.h"
#include "io/IStream.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace {

const double kReceivePollInitialSeconds = 0.01;
const double kReceivePollMaxSeconds = 0.5;
const double kReceivePollDeadlineSeconds = 5.0;

std::string generateConnectionBinding()
{
    std::string binding;
    if (!barrier::SecureRandom::generateHex(16, binding)) {
        return std::string();
    }
    return binding;
}

}

ClientProxy1_12::ClientProxy1_12(const std::string& name,
                                 barrier::IStream* stream,
                                 Server* server,
                                 IEventQueue* events) :
    ClientProxy1_11(name, stream, server, events),
    m_connectionBinding(generateConnectionBinding()),
    m_transactionServer(server),
    m_transactionEvents(events),
    m_receiver(m_connectionBinding,
               barrier::FileTransferRole::kSecondary),
    m_receiveContext(),
    m_receiveContextValid(false),
    m_receiveCompletionTimer(NULL),
    m_receiveCompletionTransferId(0),
    m_receiveCancelAckPending(false),
    m_receivePollBudgetTransferId(0),
    m_receivePollElapsed(0.0),
    m_receiveNextPollDelay(kReceivePollInitialSeconds),
    m_receiveRoute(),
    m_receivePausedChannel(),
    m_receivePausedGeneration(0)
{
    if (m_connectionBinding.empty()) {
        LOG((CLOG_ERR
            "protocol 1.12 control connection has no secure binding; "
            "bulk and transactional file transfer are disabled for \"%s\"",
            name.c_str()));
    }
}

ClientProxy1_12::~ClientProxy1_12()
{
    resetReceiveRoute();
}

void
ClientProxy1_12::offerBulkChannel(const std::string& token)
{
    ProtocolUtil::writef(getStream(), kMsgCBulkOffer1_12,
                         &token, &m_connectionBinding);
}

bool
ClientProxy1_12::attachBulkChannel(barrier::IStream* stream)
{
    if (stream == NULL) {
        return false;
    }
    const std::shared_ptr<barrier::BulkChannel> receiveRoute =
        m_receiveRoute.lock();
    const std::shared_ptr<barrier::BulkChannel> activeRoute =
        acquireBulkChannel();
    if (receiveRoute && receiveRoute == activeRoute) {
        releaseReceiveRouteForBulkClosure(receiveRoute.get());
    }
    return ClientProxy1_9::attachBulkChannel(stream);
}

void
ClientProxy1_12::detachBulkChannel()
{
    const std::shared_ptr<barrier::BulkChannel> receiveRoute =
        m_receiveRoute.lock();
    if (receiveRoute) {
        releaseReceiveRouteForBulkClosure(receiveRoute.get());
    }
    ClientProxy1_9::detachBulkChannel();
}

bool
ClientProxy1_12::parseMessage(const UInt8* code)
{
    if (std::memcmp(code, kMsgDFileTransferData1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferEnd1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransfer, 4) == 0) {
        LOG((CLOG_WARN
            "rejecting file payload on protocol 1.12 control connection for \"%s\"",
            getName().c_str()));
        return false;
    }
    if (std::memcmp(code, kMsgDFileTransferStart1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferCancel1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferStartAck1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferCancelAck1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferCommitAck1_12, 4) == 0) {
        return handleControlTransferMessage(code);
    }
    if (std::memcmp(code, kMsgDDragInfo, 4) == 0) {
        UInt32 fileNum = 0;
        std::string content;
        if (!ProtocolUtil::readf(getStream(), kMsgDDragInfo + 4,
                                 &fileNum, &content)) {
            return false;
        }
        m_transactionServer->transactionalDragInfoReceived(
            this, fileNum, content);
        return true;
    }
    return ClientProxy1_11::parseMessage(code);
}

bool
ClientProxy1_12::handleControlTransferMessage(const UInt8* code)
{
    const bool isReceiveCommand =
        std::memcmp(code, kMsgDFileTransferStart1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferCancel1_12, 4) == 0;
    barrier::FileTransferFrame frame;
    if (!barrier::FileTransferProtocol::decode(
            code, getStream(),
            isReceiveCommand ? barrier::FileTransferRole::kSecondary :
                               barrier::FileTransferRole::kPrimary,
            m_connectionBinding, frame)) {
        LOG((CLOG_WARN
            "invalid protocol 1.12 file control frame from \"%s\"",
            getName().c_str()));
        return false;
    }

    if (!isReceiveCommand) {
        return m_transactionServer->handleTransactionalFileAck(this, frame);
    }
    return handleReceiveFrame(
        frame, std::shared_ptr<barrier::BulkChannel>());
}

bool
ClientProxy1_12::handleBulkMessage(const UInt8* code,
                                   barrier::IStream* stream)
{
    const std::shared_ptr<barrier::BulkChannel> channel =
        acquireBulkChannel();
    if (!channel || !channel->isActive() || channel->getStream() != stream) {
        LOG((CLOG_WARN
            "rejecting protocol 1.12 payload from stale bulk route for \"%s\"",
            getName().c_str()));
        return false;
    }

    if (std::memcmp(code, kMsgDFileTransferData1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferEnd1_12, 4) == 0) {
        barrier::FileTransferFrame frame;
        if (!barrier::FileTransferProtocol::decode(
                code, stream, barrier::FileTransferRole::kSecondary,
                m_connectionBinding, frame)) {
            return false;
        }
        return handleReceiveFrame(frame, channel);
    }
    if (std::memcmp(code, kMsgDFileTransferStart1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferCancel1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferStartAck1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferCancelAck1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferCommitAck1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransfer, 4) == 0) {
        LOG((CLOG_WARN
            "rejecting control or legacy file frame on protocol 1.12 bulk route"));
        return false;
    }
    return ClientProxy1_9::handleBulkMessage(code, stream);
}

bool
ClientProxy1_12::handleReceiveFrame(
    const barrier::FileTransferFrame& frame,
    const std::shared_ptr<barrier::BulkChannel>& channel)
{
    const barrier::FileTransferReceiveResult result = m_receiver.handle(frame);
    switch (result.status) {
    case barrier::FileTransferReceiveStatus::kStartAccepted: {
        const std::shared_ptr<barrier::BulkChannel> activeBulk =
            acquireBulkChannel();
        barrier::FileTransferReason reason = barrier::FileTransferReason::kNone;
        if (!activeBulk || !activeBulk->isActive()) {
            reason = barrier::FileTransferReason::kBusy;
        }
        else {
            reason = m_transactionServer->prepareTransactionalFileReceive(
                this, frame, m_receiveContext);
        }
        if (reason != barrier::FileTransferReason::kNone) {
            m_receiver.reset();
            m_receiveContext = Server::TransactionalFileReceiveContext();
            m_receiveContextValid = false;
            m_receiveRoute.reset();
        }
        else {
            m_receiveContextValid = true;
            m_receiveRoute = activeBulk;
        }
        return sendReceiveAck(
            barrier::FileTransferFrameType::kStartAck,
            frame.transferId, reason);
    }

    case barrier::FileTransferReceiveStatus::kStartRejected:
        return sendReceiveAck(
            barrier::FileTransferFrameType::kStartAck,
            frame.transferId, result.reason);

    case barrier::FileTransferReceiveStatus::kDataAccepted:
        return channel && m_receiveContextValid;

    case barrier::FileTransferReceiveStatus::kBackpressure:
        return channel && m_receiveContextValid &&
            pauseReceiveForBackpressure(channel, result.sessionGeneration);

    case barrier::FileTransferReceiveStatus::kReadyToCommit:
    case barrier::FileTransferReceiveStatus::kAwaitingCommit:
        if (!channel || !m_receiveContextValid ||
            !pauseReceiveForCommit(channel, result.sessionGeneration)) {
            resetReceiveRoute();
            return false;
        }
        if (result.status ==
                barrier::FileTransferReceiveStatus::kReadyToCommit) {
            return finishReceive(frame.transferId);
        }
        if (!scheduleReceiveCompletionPoll(frame.transferId)) {
            resetReceiveRoute();
            return false;
        }
        return true;

    case barrier::FileTransferReceiveStatus::kCancelled:
        cleanupReceiveCompletionPoll();
        resetReceiveCompletionPollBudget();
        m_receiveCancelAckPending = true;
        if (m_receiver.workerCleanupPending()) {
            if (!scheduleReceiveCompletionPoll(frame.transferId)) {
                return quarantineReceiveRoute(frame.transferId, true);
            }
            return true;
        }
        m_receiveCancelAckPending = false;
        if (!sendReceiveAck(
                barrier::FileTransferFrameType::kCancelAck,
                frame.transferId, barrier::FileTransferReason::kNone)) {
            resetReceiveRoute();
            return false;
        }
        resetReceiveRoute();
        return true;

    case barrier::FileTransferReceiveStatus::kCancelAlreadyApplied:
        return sendReceiveAck(
            barrier::FileTransferFrameType::kCancelAck,
            frame.transferId, barrier::FileTransferReason::kNone);

    case barrier::FileTransferReceiveStatus::kCancelledPayloadDiscarded:
        LOG((CLOG_DEBUG1
            "discarding queued bulk payload for cancelled transfer %u from \"%s\"",
            frame.transferId, getName().c_str()));
        return channel && channel->isActive();

    case barrier::FileTransferReceiveStatus::kTransferFailed:
    case barrier::FileTransferReceiveStatus::kProtocolError:
        resetReceiveRoute();
        return false;

    }
    return false;
}

bool
ClientProxy1_12::pauseReceiveForBackpressure(
    const std::shared_ptr<barrier::BulkChannel>& channel,
    std::uint64_t generation)
{
    if (!channel->pauseInputForBackpressure(generation) ||
        !m_receiver.installBackpressureBarrier(
            generation,
            channel->makeInputResumeCallback(generation),
            channel->makeInputProgressCallback(generation))) {
        channel->resumeInputAfterBackpressure(generation);
        return false;
    }
    return true;
}

bool
ClientProxy1_12::pauseReceiveForCommit(
    const std::shared_ptr<barrier::BulkChannel>& channel,
    std::uint64_t generation)
{
    if (!channel->pauseInputForCommit(generation) ||
        !m_receiver.installCommitBarrier(
            generation,
            channel->makeInputResumeCallback(generation),
            channel->makeInputProgressCallback(generation))) {
        channel->resumeInputAfterCommit(generation);
        return false;
    }
    m_receivePausedChannel = channel;
    m_receivePausedGeneration = generation;
    return true;
}

bool
ClientProxy1_12::finishReceive(UInt32 transferId)
{
    cleanupReceiveCompletionPoll();
    barrier::CompletedFilePayload payload;
    barrier::FileTransferReason reason = barrier::FileTransferReason::kIoError;
    if (m_receiveContextValid &&
        m_receiver.takeCompleted(transferId, payload)) {
        reason = m_transactionServer->acceptTransactionalFileReceive(
            m_receiveContext, std::move(payload));
    }

    const bool acknowledged = sendReceiveAck(
        barrier::FileTransferFrameType::kCommitAck, transferId, reason);
    m_receiveContext = Server::TransactionalFileReceiveContext();
    m_receiveContextValid = false;
    m_receiveRoute.reset();
    if (m_receivePausedChannel) {
        m_receivePausedChannel->resumeInputAfterCommit(
            m_receivePausedGeneration);
    }
    m_receivePausedChannel.reset();
    m_receivePausedGeneration = 0;
    resetReceiveCompletionPollBudget();
    return acknowledged;
}

bool
ClientProxy1_12::scheduleReceiveCompletionPoll(UInt32 transferId)
{
    if (transferId == 0) {
        return false;
    }
    if (m_receiveCompletionTimer != NULL) {
        return m_receiveCompletionTransferId == transferId;
    }
    if (m_receivePollBudgetTransferId != transferId) {
        resetReceiveCompletionPollBudget();
        m_receivePollBudgetTransferId = transferId;
    }
    const double remaining = kReceivePollDeadlineSeconds -
        m_receivePollElapsed;
    if (remaining <= 0.0) {
        return false;
    }
    const double delay = (std::min)(m_receiveNextPollDelay, remaining);
    m_receiveCompletionTransferId = transferId;
    m_receiveCompletionTimer =
        m_transactionEvents->newOneShotTimer(delay, this);
    if (m_receiveCompletionTimer == NULL) {
        m_receiveCompletionTransferId = 0;
        return false;
    }
    m_receivePollElapsed += delay;
    m_receiveNextPollDelay = (std::min)(
        kReceivePollMaxSeconds, m_receiveNextPollDelay * 2.0);
    m_transactionEvents->adoptHandler(
        Event::kTimer, m_receiveCompletionTimer,
        new TMethodEventJob<ClientProxy1_12>(
            this, &ClientProxy1_12::handleReceiveCompletionPoll));
    return true;
}

void
ClientProxy1_12::handleReceiveCompletionPoll(const Event&, void*)
{
    const UInt32 transferId = m_receiveCompletionTransferId;
    cleanupReceiveCompletionPoll();
    if (m_receiveCancelAckPending) {
        if (m_receiver.workerCleanupPending()) {
            if (!scheduleReceiveCompletionPoll(transferId)) {
                if (!quarantineReceiveRoute(transferId, true)) {
                    LOG((CLOG_WARN
                        "failed to acknowledge quarantined file cancellation, transfer=%u",
                        transferId));
                }
            }
            return;
        }
        m_receiveCancelAckPending = false;
        if (!sendReceiveAck(
                barrier::FileTransferFrameType::kCancelAck,
                transferId, barrier::FileTransferReason::kNone)) {
            resetReceiveRoute();
            return;
        }
        resetReceiveRoute();
        return;
    }
    const barrier::FileTransferReceiveResult result =
        m_receiver.pollCompletion(transferId);
    if (result.status ==
            barrier::FileTransferReceiveStatus::kAwaitingCommit) {
        if (!scheduleReceiveCompletionPoll(transferId)) {
            if (!quarantineReceiveRoute(transferId, false)) {
                LOG((CLOG_WARN
                    "failed to terminate timed-out file receive, transfer=%u",
                    transferId));
            }
        }
        return;
    }
    if (result.status ==
            barrier::FileTransferReceiveStatus::kReadyToCommit) {
        if (!finishReceive(transferId)) {
            resetReceiveRoute();
        }
        return;
    }

    sendReceiveAck(barrier::FileTransferFrameType::kCommitAck,
                   transferId, result.reason);
    resetReceiveRoute();
}

void
ClientProxy1_12::cleanupReceiveCompletionPoll()
{
    if (m_receiveCompletionTimer == NULL) {
        m_receiveCompletionTransferId = 0;
        return;
    }
    EventQueueTimer* timer = m_receiveCompletionTimer;
    m_receiveCompletionTimer = NULL;
    m_receiveCompletionTransferId = 0;
    m_transactionEvents->removeHandler(Event::kTimer, timer);
    m_transactionEvents->deleteTimer(timer);
}

void
ClientProxy1_12::resetReceiveCompletionPollBudget()
{
    m_receivePollBudgetTransferId = 0;
    m_receivePollElapsed = 0.0;
    m_receiveNextPollDelay = kReceivePollInitialSeconds;
}

bool
ClientProxy1_12::quarantineReceiveRoute(
    UInt32 transferId, bool cancelled)
{
    if (transferId == 0) {
        return false;
    }
    LOG((CLOG_WARN
        "quarantining timed-out transactional file receive cleanup, transfer=%u client=%s",
        transferId, getName().c_str()));
    resetReceiveRoute();
    m_receiver.quarantineRetiredWorkerCleanup();
    return sendReceiveAck(
        cancelled ? barrier::FileTransferFrameType::kCancelAck :
                    barrier::FileTransferFrameType::kCommitAck,
        transferId,
        cancelled ? barrier::FileTransferReason::kNone :
                    barrier::FileTransferReason::kTimeout);
}

void
ClientProxy1_12::resetReceiveRoute()
{
    cleanupReceiveCompletionPoll();
    m_receiver.reset();
    m_receiveContext = Server::TransactionalFileReceiveContext();
    m_receiveContextValid = false;
    m_receiveCancelAckPending = false;
    m_receiveRoute.reset();
    if (m_receivePausedChannel) {
        m_receivePausedChannel->resumeInputAfterCommit(
            m_receivePausedGeneration);
    }
    m_receivePausedChannel.reset();
    m_receivePausedGeneration = 0;
    resetReceiveCompletionPollBudget();
}

void
ClientProxy1_12::releaseReceiveRouteForBulkClosure(
    barrier::BulkChannel* channel)
{
    const std::shared_ptr<barrier::BulkChannel> receiveRoute =
        m_receiveRoute.lock();
    if (!receiveRoute || receiveRoute.get() != channel) {
        return;
    }

    // A cancellation can outlive its payload route while the spool worker
    // releases its file.  Keep that cleanup transaction alive so the peer
    // still receives its mandatory CancelAck on the control route.
    m_receiveRoute.reset();
    if (m_receiveCancelAckPending) {
        return;
    }

    if (m_receiver.hasActiveTransfer() || m_receiveContextValid ||
        m_receiver.workerCleanupPending()) {
        m_receiver.abortActiveTransfer();
        resetReceiveRoute();
    }
}

bool
ClientProxy1_12::sendReceiveAck(
    barrier::FileTransferFrameType type,
    UInt32 transferId,
    barrier::FileTransferReason reason)
{
    barrier::FileTransferFrame frame;
    switch (type) {
    case barrier::FileTransferFrameType::kStartAck:
        frame = barrier::FileTransferFrame::startAck(
            m_connectionBinding, transferId, reason);
        break;
    case barrier::FileTransferFrameType::kCancelAck:
        frame = barrier::FileTransferFrame::cancelAck(
            m_connectionBinding, transferId, reason);
        break;
    case barrier::FileTransferFrameType::kCommitAck:
        frame = barrier::FileTransferFrame::commitAck(
            m_connectionBinding, transferId, reason);
        break;
    default:
        return false;
    }
    return barrier::FileTransferProtocol::encode(
        getStream(), frame, barrier::FileTransferRole::kSecondary);
}

bool
ClientProxy1_12::sendTransactionalFileFrame(
    const barrier::FileTransferFrame& frame,
    bool)
{
    if (frame.connectionBinding != m_connectionBinding ||
        !barrier::FileTransferProtocol::validate(
            frame, barrier::FileTransferRole::kPrimary)) {
        return false;
    }

    switch (frame.type) {
    case barrier::FileTransferFrameType::kStart:
    case barrier::FileTransferFrameType::kCancel:
        return barrier::FileTransferProtocol::encode(
            getStream(), frame, barrier::FileTransferRole::kPrimary);

    case barrier::FileTransferFrameType::kData:
    case barrier::FileTransferFrameType::kEnd: {
        const std::shared_ptr<barrier::BulkChannel> channel =
            acquireBulkChannel();
        return channel && channel->isActive() &&
            barrier::FileTransferProtocol::encode(
                channel->getStream(), frame,
                barrier::FileTransferRole::kPrimary);
    }

    case barrier::FileTransferFrameType::kStartAck:
    case barrier::FileTransferFrameType::kCancelAck:
    case barrier::FileTransferFrameType::kCommitAck:
        return false;
    }
    return false;
}

void
ClientProxy1_12::handleBulkDisconnected(
    barrier::BulkChannel* channel, std::uint64_t pausedGeneration)
{
    releaseReceiveRouteForBulkClosure(channel);
    ClientProxy1_9::handleBulkDisconnected(channel, pausedGeneration);
}
