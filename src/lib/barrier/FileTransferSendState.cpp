/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/FileTransferSendState.h"

namespace {

bool isLowerHex(char value)
{
    return (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f');
}

bool isValidIdentity(
    barrier::FileTransferKind kind, std::uint64_t revision,
    const std::string& sessionId)
{
    if (kind == barrier::FileTransferKind::kClipboard) {
        if (revision == 0 ||
            sessionId.size() !=
                barrier::FileTransferProtocol::kClipboardSessionHexSize) {
            return false;
        }
        for (char value : sessionId) {
            if (!isLowerHex(value)) {
                return false;
            }
        }
        return true;
    }
    return (kind == barrier::FileTransferKind::kManual ||
            kind == barrier::FileTransferKind::kDrag) &&
        revision == 0 && sessionId.empty();
}

} // namespace

namespace barrier {

FileTransferSendState::FileTransferSendState(
    UInt32 transferId,
    std::chrono::milliseconds startAckTimeout,
    std::chrono::milliseconds commitAckTimeout) :
    FileTransferSendState(
        transferId, FileTransferKind::kManual, 0, std::string(),
        startAckTimeout, commitAckTimeout)
{
}

FileTransferSendState::FileTransferSendState(
    UInt32 transferId, FileTransferKind kind,
    std::uint64_t clipboardRevision,
    const std::string& clipboardSessionId,
    std::chrono::milliseconds startAckTimeout,
    std::chrono::milliseconds commitAckTimeout) :
    m_transferId(transferId),
    m_kind(kind),
    m_clipboardRevision(clipboardRevision),
    m_clipboardSessionId(clipboardSessionId),
    m_startAckTimeout(startAckTimeout.count() > 0 ? startAckTimeout :
                      std::chrono::milliseconds(1)),
    m_commitAckTimeout(commitAckTimeout.count() > 0 ? commitAckTimeout :
                       std::chrono::milliseconds(1)),
    m_phase(transferId == 0 ||
            !isValidIdentity(kind, clipboardRevision, clipboardSessionId) ?
                Phase::kStopped : Phase::kCreated),
    m_totalSize(0),
    m_nextOffset(0),
    m_finalOffset(0),
    m_result(transferId == 0 ||
             !isValidIdentity(kind, clipboardRevision, clipboardSessionId) ?
                 FileTransferReason::kProtocolError :
                 FileTransferReason::kNone),
    m_cancelQueued(false),
    m_cancelAcknowledged(false),
    m_startAcknowledged(false)
{
}

FileTransferKind
FileTransferSendState::kind() const
{
    return m_kind;
}

std::uint64_t
FileTransferSendState::clipboardRevision() const
{
    return m_clipboardRevision;
}

const std::string&
FileTransferSendState::clipboardSessionId() const
{
    return m_clipboardSessionId;
}

UInt32
FileTransferSendState::transferId() const
{
    return m_transferId;
}

bool
FileTransferSendState::isValidReason(FileTransferReason reason)
{
    return static_cast<UInt32>(reason) <=
        static_cast<UInt32>(FileTransferReason::kConnectionLost);
}

void
FileTransferSendState::stopLocked(FileTransferReason reason)
{
    if (m_phase == Phase::kCommitted || m_phase == Phase::kStopped) {
        return;
    }

    m_phase = Phase::kStopped;
    m_result = reason == FileTransferReason::kNone ?
        FileTransferReason::kProtocolError : reason;
}

bool
FileTransferSendState::markStartQueued(UInt32 totalSize)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_phase != Phase::kCreated) {
        return false;
    }
    if (totalSize > FileTransferProtocol::kMaxTransferSize) {
        stopLocked(FileTransferReason::kSizeLimit);
        m_changed.notify_all();
        return false;
    }

    m_totalSize = totalSize;
    m_phase = Phase::kWaitingStartAck;
    return true;
}

bool
FileTransferSendState::signalStartAck(
    UInt32 transferId, FileTransferReason reason)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (transferId != m_transferId ||
        m_phase != Phase::kWaitingStartAck || !isValidReason(reason)) {
        return false;
    }

    if (reason == FileTransferReason::kNone) {
        m_phase = Phase::kStreaming;
        m_startAcknowledged = true;
    }
    else {
        stopLocked(reason);
    }
    m_changed.notify_all();
    return true;
}

FileTransferReason
FileTransferSendState::waitForStartAck()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_phase == Phase::kCreated) {
        stopLocked(FileTransferReason::kProtocolError);
    }
    else if (m_phase == Phase::kWaitingStartAck &&
             !m_changed.wait_for(lock, m_startAckTimeout, [this]() {
                 return m_phase != Phase::kWaitingStartAck;
             })) {
        stopLocked(FileTransferReason::kTimeout);
    }

    return m_phase == Phase::kStreaming ?
        FileTransferReason::kNone : m_result;
}

bool
FileTransferSendState::markDataQueued(UInt32 offset, UInt32 size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_phase != Phase::kStreaming || size == 0 ||
        offset != m_nextOffset || offset > m_totalSize ||
        size > m_totalSize - offset) {
        return false;
    }

    m_nextOffset += size;
    return true;
}

bool
FileTransferSendState::markEndQueued(UInt32 finalOffset)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_phase != Phase::kStreaming || finalOffset != m_nextOffset ||
        finalOffset != m_totalSize) {
        return false;
    }

    m_finalOffset = finalOffset;
    m_phase = Phase::kWaitingCommitAck;
    return true;
}

bool
FileTransferSendState::signalCommitAck(
    UInt32 transferId, FileTransferReason reason)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (transferId != m_transferId ||
        m_phase != Phase::kWaitingCommitAck || !isValidReason(reason)) {
        return false;
    }

    if (reason == FileTransferReason::kNone) {
        m_phase = Phase::kCommitted;
        m_result = FileTransferReason::kNone;
    }
    else {
        stopLocked(reason);
    }
    m_changed.notify_all();
    return true;
}

FileTransferReason
FileTransferSendState::waitForCommitAck()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_phase == Phase::kStreaming || m_phase == Phase::kCreated ||
        m_phase == Phase::kWaitingStartAck) {
        stopLocked(FileTransferReason::kProtocolError);
    }
    else if (m_phase == Phase::kWaitingCommitAck &&
             !m_changed.wait_for(lock, m_commitAckTimeout, [this]() {
                 return m_phase != Phase::kWaitingCommitAck;
             })) {
        stopLocked(FileTransferReason::kTimeout);
    }

    return m_phase == Phase::kCommitted ?
        FileTransferReason::kNone : m_result;
}

bool
FileTransferSendState::markCancelQueued(FileTransferReason reason)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_phase == Phase::kCommitted || m_cancelQueued ||
        reason == FileTransferReason::kNone || !isValidReason(reason)) {
        return false;
    }

    if (m_phase != Phase::kStopped) {
        stopLocked(reason);
    }
    m_cancelQueued = true;
    m_changed.notify_all();
    return true;
}

bool
FileTransferSendState::signalCancelAck(
    UInt32 transferId, FileTransferReason reason)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (transferId != m_transferId || !m_cancelQueued ||
        m_cancelAcknowledged || !isValidReason(reason) ||
        reason != FileTransferReason::kNone) {
        return false;
    }

    m_cancelAcknowledged = true;
    m_changed.notify_all();
    return true;
}

bool
FileTransferSendState::fail(FileTransferReason reason)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_phase == Phase::kCommitted || m_phase == Phase::kStopped ||
        reason == FileTransferReason::kNone || !isValidReason(reason)) {
        return false;
    }

    stopLocked(reason);
    m_changed.notify_all();
    return true;
}

void
FileTransferSendState::interrupt()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    stopLocked(FileTransferReason::kCancelled);
    m_changed.notify_all();
}

void
FileTransferSendState::connectionLost()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    stopLocked(FileTransferReason::kConnectionLost);
    m_changed.notify_all();
}

bool
FileTransferSendState::readyForData() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_phase == Phase::kStreaming;
}

bool
FileTransferSendState::startAcknowledged() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_startAcknowledged;
}

bool
FileTransferSendState::stopped() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_phase == Phase::kStopped;
}

bool
FileTransferSendState::committed() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_phase == Phase::kCommitted;
}

bool
FileTransferSendState::cancelAcknowledged() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cancelAcknowledged;
}

UInt32
FileTransferSendState::finalOffset() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_finalOffset;
}

FileTransferReason
FileTransferSendState::result() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_result;
}

} // namespace barrier
