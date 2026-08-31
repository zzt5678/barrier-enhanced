/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "barrier/FileTransferProtocol.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace barrier {

//! Thread-safe sender transaction state for protocol 1.12 file transfers.
class FileTransferSendState {
public:
    explicit FileTransferSendState(
        UInt32 transferId,
        std::chrono::milliseconds startAckTimeout =
            std::chrono::seconds(15),
        std::chrono::milliseconds commitAckTimeout =
            std::chrono::seconds(90));
    FileTransferSendState(
        UInt32 transferId,
        FileTransferKind kind,
        std::uint64_t clipboardRevision,
        const std::string& clipboardSessionId,
        std::chrono::milliseconds startAckTimeout =
            std::chrono::seconds(15),
        std::chrono::milliseconds commitAckTimeout =
            std::chrono::seconds(90));

    UInt32 transferId() const;
    FileTransferKind kind() const;
    std::uint64_t clipboardRevision() const;
    const std::string& clipboardSessionId() const;

    bool markStartQueued(UInt32 totalSize);
    bool signalStartAck(UInt32 transferId, FileTransferReason reason);
    FileTransferReason waitForStartAck();

    bool markDataQueued(UInt32 offset, UInt32 size);
    bool markEndQueued(UInt32 finalOffset);
    bool signalCommitAck(UInt32 transferId, FileTransferReason reason);
    FileTransferReason waitForCommitAck();

    bool markCancelQueued(FileTransferReason reason);
    bool signalCancelAck(UInt32 transferId, FileTransferReason reason);

    //! Record a local terminal failure that has no peer acknowledgment.
    bool fail(FileTransferReason reason);

    void interrupt();
    void connectionLost();

    bool readyForData() const;
    bool startAcknowledged() const;
    bool stopped() const;
    bool committed() const;
    bool cancelAcknowledged() const;
    UInt32 finalOffset() const;
    FileTransferReason result() const;

private:
    enum class Phase {
        kCreated,
        kWaitingStartAck,
        kStreaming,
        kWaitingCommitAck,
        kCommitted,
        kStopped
    };

    static bool isValidReason(FileTransferReason reason);
    void stopLocked(FileTransferReason reason);

    const UInt32 m_transferId;
    const FileTransferKind m_kind;
    const std::uint64_t m_clipboardRevision;
    const std::string m_clipboardSessionId;
    const std::chrono::milliseconds m_startAckTimeout;
    const std::chrono::milliseconds m_commitAckTimeout;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    Phase m_phase;
    UInt32 m_totalSize;
    UInt32 m_nextOffset;
    UInt32 m_finalOffset;
    FileTransferReason m_result;
    bool m_cancelQueued;
    bool m_cancelAcknowledged;
    bool m_startAcknowledged;
};

} // namespace barrier
