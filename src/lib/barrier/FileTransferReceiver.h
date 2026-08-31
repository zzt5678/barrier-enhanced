/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#pragma once

#include "barrier/FileReceiveSession.h"
#include "barrier/FileTransferProtocol.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

namespace barrier {

struct FileTransferReceiveLimits {
    std::size_t memoryLimit = 32u * 1024u * 1024u;
    std::size_t reserveLimit = 64u * 1024u * 1024u;
    std::size_t asyncQueueLimit = FileReceiveSession::kDefaultAsyncQueueLimit;
};

struct CompletedFilePayload {
    std::string data;
    std::size_t expectedSize = 0;
    fs::path spoolPath;
};

enum class FileTransferReceiveStatus {
    kStartAccepted,
    kStartRejected,
    kDataAccepted,
    kBackpressure,
    kAwaitingCommit,
    kReadyToCommit,
    kCancelled,
    kCancelAlreadyApplied,
    kCancelledPayloadDiscarded,
    kTransferFailed,
    kProtocolError
};

struct FileTransferReceiveResult {
    FileTransferReceiveStatus status =
        FileTransferReceiveStatus::kProtocolError;
    FileTransferReason reason = FileTransferReason::kProtocolError;
    UInt32 transferId = 0;
    std::uint64_t sessionGeneration = 0;
};

//! Owns one protocol 1.12 receive transaction for one control connection.
/*!
This class deliberately has no socket or event-queue access. The owning proxy
decodes a frame, passes it here, and turns the returned status into an ACK,
bulk-input pause, or route failure. Payload ownership must be taken before the
proxy sends CommitAck.
*/
class FileTransferReceiver {
public:
    FileTransferReceiver(
        const std::string& connectionBinding,
        FileTransferRole expectedInitiatorRole,
        const FileTransferReceiveLimits& limits = FileTransferReceiveLimits());
    ~FileTransferReceiver();

    FileTransferReceiveResult handle(const FileTransferFrame& frame);
    FileTransferReceiveResult pollCompletion(UInt32 transferId);
    bool takeCompleted(UInt32 transferId, CompletedFilePayload& payload);
    UInt32 abortActiveTransfer();
    void reset();
    bool installCommitBarrier(
        std::uint64_t generation,
        const std::function<void()>& commit,
        const std::function<void()>& progress = std::function<void()>());
    bool installBackpressureBarrier(
        std::uint64_t generation,
        const std::function<void()>& resume,
        const std::function<void()>& progress = std::function<void()>());

    bool hasActiveTransfer() const { return m_activeTransferId != 0; }
    UInt32 activeTransferId() const { return m_activeTransferId; }
    UInt32 nextOffset() const { return m_nextOffset; }
    std::uint64_t sessionGeneration() const { return m_session.generation(); }
    FileReceiveSession::State sessionState() const { return m_session.state(); }
    bool workerCleanupPending() const
    {
        return m_session.workerCleanupPending();
    }
    void quarantineRetiredWorkerCleanup()
    {
        m_session.quarantineRetiredWorkerCleanup();
    }
    const FileReceiveSession& session() const { return m_session; }

private:
    FileTransferReceiveResult result(
        FileTransferReceiveStatus status,
        FileTransferReason reason,
        UInt32 transferId) const;
    FileTransferReceiveResult protocolError(UInt32 transferId) const;
    FileTransferReceiveResult failActive(FileTransferReason reason);
    bool matchesActive(UInt32 transferId) const;
    bool wasCancelled(UInt32 transferId) const;
    void rememberCancelled(UInt32 transferId);
    FileTransferReceiveResult discardCancelledPayload(
        const FileTransferFrame& frame);
    void forgetCancelled(UInt32 transferId);

    enum { kMaxCancelledTransferTombstones = 64 };

    struct CancelledTransferTombstone {
        UInt32 nextOffset;
        UInt32 totalSize;
    };

private:
    std::string m_connectionBinding;
    FileTransferRole m_expectedInitiatorRole;
    FileTransferReceiveLimits m_limits;
    FileReceiveSession m_session;
    UInt32 m_activeTransferId;
    UInt32 m_nextOffset;
    std::deque<UInt32> m_cancelledTransferOrder;
    std::unordered_map<UInt32, CancelledTransferTombstone>
        m_cancelledTransfers;
};

} // namespace barrier
