/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#include "barrier/FileTransferReceiver.h"

#include <utility>

namespace barrier {

FileTransferReceiver::FileTransferReceiver(
    const std::string& connectionBinding,
    FileTransferRole expectedInitiatorRole,
    const FileTransferReceiveLimits& limits) :
    m_connectionBinding(connectionBinding),
    m_expectedInitiatorRole(expectedInitiatorRole),
    m_limits(limits),
    m_session(),
    m_activeTransferId(0),
    m_nextOffset(0),
    m_cancelledTransferOrder(),
    m_cancelledTransfers()
{
}

FileTransferReceiver::~FileTransferReceiver()
{
    reset();
}

FileTransferReceiveResult
FileTransferReceiver::result(
    FileTransferReceiveStatus status,
    FileTransferReason reason,
    UInt32 transferId) const
{
    FileTransferReceiveResult value;
    value.status = status;
    value.reason = reason;
    value.transferId = transferId;
    value.sessionGeneration = m_session.generation();
    return value;
}

FileTransferReceiveResult
FileTransferReceiver::protocolError(UInt32 transferId) const
{
    return result(FileTransferReceiveStatus::kProtocolError,
                  FileTransferReason::kProtocolError, transferId);
}

bool
FileTransferReceiver::matchesActive(UInt32 transferId) const
{
    return transferId != 0 && transferId == m_activeTransferId;
}

bool
FileTransferReceiver::wasCancelled(UInt32 transferId) const
{
    return transferId != 0 &&
        m_cancelledTransfers.find(transferId) != m_cancelledTransfers.end();
}

void
FileTransferReceiver::rememberCancelled(UInt32 transferId)
{
    if (transferId == 0 || wasCancelled(transferId)) {
        return;
    }

    while (m_cancelledTransferOrder.size() >=
           kMaxCancelledTransferTombstones) {
        m_cancelledTransfers.erase(m_cancelledTransferOrder.front());
        m_cancelledTransferOrder.pop_front();
    }
    CancelledTransferTombstone tombstone;
    tombstone.nextOffset = m_nextOffset;
    tombstone.totalSize = static_cast<UInt32>(m_session.expectedSize());
    m_cancelledTransfers.insert(std::make_pair(transferId, tombstone));
    m_cancelledTransferOrder.push_back(transferId);
}

FileTransferReceiveResult
FileTransferReceiver::discardCancelledPayload(
    const FileTransferFrame& frame)
{
    std::unordered_map<UInt32, CancelledTransferTombstone>::iterator found =
        m_cancelledTransfers.find(frame.transferId);
    if (found == m_cancelledTransfers.end()) {
        return protocolError(frame.transferId);
    }

    CancelledTransferTombstone& tombstone = found->second;
    if (frame.type == FileTransferFrameType::kData) {
        const std::size_t payloadSize = frame.payload.size();
        if (frame.offset != tombstone.nextOffset ||
            tombstone.nextOffset > tombstone.totalSize ||
            payloadSize > tombstone.totalSize - tombstone.nextOffset) {
            return protocolError(frame.transferId);
        }
        tombstone.nextOffset += static_cast<UInt32>(payloadSize);
    }
    else if (frame.type == FileTransferFrameType::kEnd) {
        if (frame.offset != tombstone.nextOffset ||
            tombstone.nextOffset != tombstone.totalSize) {
            return protocolError(frame.transferId);
        }
        forgetCancelled(frame.transferId);
    }
    else {
        return protocolError(frame.transferId);
    }

    return result(FileTransferReceiveStatus::kCancelledPayloadDiscarded,
                  FileTransferReason::kCancelled, frame.transferId);
}

void
FileTransferReceiver::forgetCancelled(UInt32 transferId)
{
    m_cancelledTransfers.erase(transferId);
    for (std::deque<UInt32>::iterator id = m_cancelledTransferOrder.begin();
         id != m_cancelledTransferOrder.end();) {
        if (*id == transferId) {
            id = m_cancelledTransferOrder.erase(id);
        }
        else {
            ++id;
        }
    }
}

FileTransferReceiveResult
FileTransferReceiver::failActive(FileTransferReason reason)
{
    const UInt32 transferId = m_activeTransferId;
    m_session.fail();
    m_activeTransferId = 0;
    m_nextOffset = 0;
    return result(FileTransferReceiveStatus::kTransferFailed,
                  reason, transferId);
}

FileTransferReceiveResult
FileTransferReceiver::handle(const FileTransferFrame& frame)
{
    FileTransferValidationError validationError =
        FileTransferValidationError::kNone;
    if (frame.connectionBinding != m_connectionBinding ||
        !FileTransferProtocol::validate(
            frame, m_expectedInitiatorRole, &validationError)) {
        return protocolError(frame.transferId);
    }

    if ((frame.type == FileTransferFrameType::kData ||
         frame.type == FileTransferFrameType::kEnd) &&
        wasCancelled(frame.transferId)) {
        return discardCancelledPayload(frame);
    }

    switch (frame.type) {
    case FileTransferFrameType::kStart:
        if (wasCancelled(frame.transferId)) {
            return protocolError(frame.transferId);
        }
        if (hasActiveTransfer() || m_session.workerCleanupPending()) {
            return result(FileTransferReceiveStatus::kStartRejected,
                          FileTransferReason::kBusy, frame.transferId);
        }
        if (!m_session.begin(frame.totalSize,
                             m_limits.memoryLimit,
                             m_limits.reserveLimit,
                             m_limits.asyncQueueLimit)) {
            const FileTransferReason reason =
                m_session.workerCleanupPending() ?
                    FileTransferReason::kBusy :
                    FileTransferReason::kIoError;
            if (reason != FileTransferReason::kBusy) {
                m_session.reset();
            }
            return result(FileTransferReceiveStatus::kStartRejected,
                          reason, frame.transferId);
        }
        m_activeTransferId = frame.transferId;
        m_nextOffset = 0;
        return result(FileTransferReceiveStatus::kStartAccepted,
                      FileTransferReason::kNone, frame.transferId);

    case FileTransferFrameType::kData: {
        if (!matchesActive(frame.transferId)) {
            return protocolError(frame.transferId);
        }
        const std::size_t payloadSize = frame.payload.size();
        if (frame.offset != m_nextOffset ||
            m_nextOffset > m_session.expectedSize() ||
            payloadSize > m_session.expectedSize() - m_nextOffset) {
            return failActive(FileTransferReason::kOffsetMismatch);
        }
        const FileReceiveSession::AppendResult append =
            m_session.append(frame.payload);
        if (append == FileReceiveSession::kAppendFailed) {
            return failActive(FileTransferReason::kIoError);
        }
        m_nextOffset += static_cast<UInt32>(payloadSize);
        return result(
            append == FileReceiveSession::kAppendBackpressure ?
                FileTransferReceiveStatus::kBackpressure :
                FileTransferReceiveStatus::kDataAccepted,
            FileTransferReason::kNone, frame.transferId);
    }

    case FileTransferFrameType::kEnd:
        if (!matchesActive(frame.transferId)) {
            return protocolError(frame.transferId);
        }
        if (frame.offset != m_nextOffset ||
            m_nextOffset != m_session.expectedSize()) {
            return failActive(FileTransferReason::kOffsetMismatch);
        }
        if (!m_session.finish(frame.payload)) {
            return failActive(
                m_session.state() == FileReceiveSession::kFailed ?
                    FileTransferReason::kIoError :
                    FileTransferReason::kDigestMismatch);
        }
        return result(
            m_session.isComplete() ?
                FileTransferReceiveStatus::kReadyToCommit :
                FileTransferReceiveStatus::kAwaitingCommit,
            FileTransferReason::kNone, frame.transferId);

    case FileTransferFrameType::kCancel:
        if (wasCancelled(frame.transferId)) {
            return result(FileTransferReceiveStatus::kCancelAlreadyApplied,
                          FileTransferReason::kNone, frame.transferId);
        }
        if (!matchesActive(frame.transferId)) {
            return protocolError(frame.transferId);
        }
        rememberCancelled(frame.transferId);
        m_session.reset();
        m_activeTransferId = 0;
        m_nextOffset = 0;
        return result(FileTransferReceiveStatus::kCancelled,
                      FileTransferReason::kNone, frame.transferId);

    case FileTransferFrameType::kStartAck:
    case FileTransferFrameType::kCancelAck:
    case FileTransferFrameType::kCommitAck:
        return protocolError(frame.transferId);
    }

    return protocolError(frame.transferId);
}

FileTransferReceiveResult
FileTransferReceiver::pollCompletion(UInt32 transferId)
{
    if (!matchesActive(transferId)) {
        return protocolError(transferId);
    }
    switch (m_session.state()) {
    case FileReceiveSession::kFinalizing:
        return result(FileTransferReceiveStatus::kAwaitingCommit,
                      FileTransferReason::kNone, transferId);
    case FileReceiveSession::kComplete:
        return result(FileTransferReceiveStatus::kReadyToCommit,
                      FileTransferReason::kNone, transferId);
    case FileReceiveSession::kFailed:
        return failActive(FileTransferReason::kIoError);
    default:
        return protocolError(transferId);
    }
}

bool
FileTransferReceiver::takeCompleted(
    UInt32 transferId, CompletedFilePayload& payload)
{
    if (!matchesActive(transferId) || !m_session.isComplete()) {
        return false;
    }

    CompletedFilePayload completed;
    m_session.takeCompleted(completed.data, completed.expectedSize,
                            completed.spoolPath);
    if (completed.expectedSize != m_nextOffset ||
        (completed.spoolPath.empty() &&
         completed.data.size() != completed.expectedSize)) {
        if (!completed.spoolPath.empty()) {
            fs::remove(completed.spoolPath);
        }
        m_activeTransferId = 0;
        m_nextOffset = 0;
        return false;
    }

    payload = std::move(completed);
    m_activeTransferId = 0;
    m_nextOffset = 0;
    return true;
}

UInt32
FileTransferReceiver::abortActiveTransfer()
{
    const UInt32 transferId = m_activeTransferId;
    if (transferId == 0) {
        return 0;
    }
    rememberCancelled(transferId);
    m_session.reset();
    m_activeTransferId = 0;
    m_nextOffset = 0;
    return transferId;
}

bool
FileTransferReceiver::installCommitBarrier(
    std::uint64_t generation, const std::function<void()>& commit,
    const std::function<void()>& progress)
{
    return m_session.installCommitBarrier(generation, commit, progress);
}

bool
FileTransferReceiver::installBackpressureBarrier(
    std::uint64_t generation, const std::function<void()>& resume,
    const std::function<void()>& progress)
{
    return m_session.installBackpressureBarrier(generation, resume, progress);
}

void
FileTransferReceiver::reset()
{
    m_session.reset();
    m_activeTransferId = 0;
    m_nextOffset = 0;
}

} // namespace barrier
