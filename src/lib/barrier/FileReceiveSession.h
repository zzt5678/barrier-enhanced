/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "common/common.h"
#include "io/filesystem.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace barrier {
class TransferDigest;
}

class FileReceiveSession {
public:
    enum State {
        kIdle,
        kReceiving,
        kFinalizing,
        kComplete,
        kFailed,
        kDiscarding
    };

    enum AppendResult {
        kAppendFailed,
        kAppendQueued,
        kAppendBackpressure
    };

    static const size_t kDefaultAsyncQueueLimit = 16 * 1024 * 1024;
    static const size_t kMaxAsyncChunkSize = 4 * 1024 * 1024;

    FileReceiveSession();
    ~FileReceiveSession();

    bool                begin(size_t expectedSize,
                              size_t memoryLimit,
                              size_t reserveLimit,
                              size_t asyncQueueLimit = kDefaultAsyncQueueLimit);
    AppendResult        append(std::string content);
    bool                finish(const std::string& expectedDigest = std::string());
    void                fail();
    void                discardRemaining();
    void                reset();
    void                takeCompleted(std::string& data,
                                      size_t& expectedSize,
                                      barrier::fs::path& spoolPath);
    bool                installCommitBarrier(
                            std::uint64_t generation,
                            const std::function<void()>& commit,
                            const std::function<void()>& progress =
                                std::function<void()>());
    bool                installBackpressureBarrier(
                            std::uint64_t generation,
                            const std::function<void()>& resume,
                            const std::function<void()>& progress =
                                std::function<void()>());

    State               state() const;
    bool                isComplete() const { return state() == kComplete; }
    bool                isFinalizing() const { return state() == kFinalizing; }
    bool                isSpoolOpen() const;
    size_t              spoolOpenCount() const;
    bool                workerCleanupPending() const;
    bool                retiredWorkerPending() const;
    void                quarantineRetiredWorkerCleanup();
    size_t              expectedSize() const { return m_expectedSize; }
    size_t              receivedSize() const { return m_receivedSize; }
    const std::string&  data() const { return m_data; }
    barrier::fs::path   spoolPath() const;
    std::uint64_t       generation() const { return m_generation; }
    bool                matchesGeneration(std::uint64_t generation) const
    {
        return generation != 0 && generation == m_generation;
    }

#if defined(BARRIER_TEST_ENV) || defined(BARRIER_TEST_ACCESS)
    void                testSetWorkerExitGate(
                            const std::shared_ptr<std::atomic<bool> >& gate)
                            { m_workerExitGateForTest = gate; }
#endif

private:
    FileReceiveSession(const FileReceiveSession&);
    FileReceiveSession& operator=(const FileReceiveSession&);

    struct AsyncState;

    static void         runSpoolWorker(
                            const std::shared_ptr<AsyncState>& state) noexcept;
    void                advanceGeneration();
    void                clearPayload();
    void                cancelSpoolWorker();
    bool                releaseCompletedWorkerCleanup();
    void                resolveCommitBarrier() noexcept;

private:
    State               m_state;
    size_t              m_expectedSize;
    size_t              m_receivedSize;
    std::string         m_data;
    std::shared_ptr<AsyncState> m_asyncState;
    std::thread*        m_spoolWorker;
    std::shared_ptr<std::atomic<bool> > m_retiredWorkerReaped;
    bool                m_retiredWorkerBlocksBegin;
    std::uint64_t       m_generation;
    std::unique_ptr<barrier::TransferDigest> m_digest;
    std::function<void()> m_commitBarrier;
    std::shared_ptr<std::atomic<bool> > m_workerExitGateForTest;
};
