/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "base/String.h"
#include "common/basic_types.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

// Shared by the screen and its clipboard worker. A blocked worker may outlive
// the screen, so shutdown revokes its notification token and waits only for a
// fixed deadline. The worker owns this state through its shared context and
// never needs to retain an MSWindowsScreen pointer.
class MSWindowsClipboardWorkerLifetime {
public:
    MSWindowsClipboardWorkerLifetime();

    std::uint64_t notificationToken() const;
    void requestStop();
    bool stopRequested() const;
    bool acceptsNotification(std::uint64_t token) const;
    void markFinished();
    bool waitForFinished(std::chrono::milliseconds timeout);

private:
    MSWindowsClipboardWorkerLifetime(
        const MSWindowsClipboardWorkerLifetime&);
    MSWindowsClipboardWorkerLifetime& operator=(
        const MSWindowsClipboardWorkerLifetime&);

    const std::uint64_t m_notificationToken;
    std::atomic<bool> m_stopRequested;
    std::mutex m_finishedMutex;
    std::condition_variable m_finishedChanged;
    bool m_finished;
};

// Thread safety is supplied by the owner. The state stays independent of
// Win32 so its latest-revision rules can be tested on every build platform.
class MSWindowsClipboardSnapshotState {
public:
    struct Request {
        Request();

        std::uint64_t generation;
        UInt32 windowsSequence;
        UInt32 protocolSequence;
        UInt32 attempt;
    };

    MSWindowsClipboardSnapshotState();

    Request queue(UInt32 windowsSequence, UInt32 protocolSequence);
    void invalidate();

    bool hasPending() const;
    bool takeNext(Request* request);
    bool complete(const Request& request, UInt32 currentWindowsSequence,
                  const std::shared_ptr<const String>& snapshot);
    bool retry(const Request& request, UInt32 maxAttempts);
    void fail(const Request& request);
    bool takeReadyNotification(UInt32* protocolSequence);

    bool copyReady(std::shared_ptr<const String>* snapshot,
                   UInt32* windowsSequence) const;
    bool needsSnapshot(UInt32 windowsSequence) const;
    bool blocksSynchronousRead() const;

private:
    enum class Status {
        Idle,
        Pending,
        Ready,
        Failed
    };

    bool matches(const Request& request) const;

    std::uint64_t m_generation;
    UInt32 m_windowsSequence;
    Status m_status;
    bool m_hasPendingRequest;
    Request m_pendingRequest;
    std::shared_ptr<const String> m_snapshot;
    UInt32 m_readyProtocolSequence;
    bool m_notificationPending;
};

class MSWindowsClipboardPublishState {
public:
    enum class CompletionResult {
        Succeeded,
        Failed,
        Superseded
    };

    struct Request {
        Request();

        std::uint64_t generation;
        std::uint64_t publicationId;
        std::uint64_t baseInFlightGeneration;
        UInt32 attempt;
        UInt32 expectedWindowsSequence;
        std::shared_ptr<const String> snapshot;
    };

    struct Completion {
        Completion();

        std::uint64_t publicationId;
        CompletionResult result;
        UInt32 committedWindowsSequence;
    };

    MSWindowsClipboardPublishState();

    Request queue(const std::shared_ptr<const String>& snapshot,
                  UInt32 expectedWindowsSequence,
                  std::uint64_t publicationId = 0);
    void invalidate();
    bool hasPending() const;
    bool hasInFlight() const;
    bool hasCompletion() const;
    bool takeNext(Request* request);
    bool retry(const Request& request, UInt32 maxAttempts);
    bool complete(const Request& request,
                  UInt32 committedWindowsSequence);
    void fail(const Request& request,
              CompletionResult result = CompletionResult::Failed);
    bool takeCompletion(Completion* completion);

private:
    bool isInFlight(const Request& request) const;
    bool matches(const Request& request) const;
    void recordCompletion(const Request& request,
                          CompletionResult result,
                          UInt32 committedWindowsSequence = 0);

    std::uint64_t m_generation;
    bool m_pending;
    bool m_inFlight;
    Request m_pendingRequest;
    Request m_inFlightRequest;
    std::deque<Completion> m_completions;
};
