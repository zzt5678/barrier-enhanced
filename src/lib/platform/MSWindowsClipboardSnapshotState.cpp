/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "platform/MSWindowsClipboardSnapshotState.h"

namespace {

std::atomic<std::uint64_t> s_nextClipboardWorkerNotificationToken(0);

std::uint64_t nextClipboardWorkerNotificationToken()
{
    std::uint64_t token =
        s_nextClipboardWorkerNotificationToken.fetch_add(
            1, std::memory_order_relaxed) + 1;
    if (token == 0) {
        token = s_nextClipboardWorkerNotificationToken.fetch_add(
            1, std::memory_order_relaxed) + 1;
    }
    return token;
}

} // namespace

MSWindowsClipboardWorkerLifetime::MSWindowsClipboardWorkerLifetime() :
    m_notificationToken(nextClipboardWorkerNotificationToken()),
    m_stopRequested(false),
    m_finished(false)
{
}

std::uint64_t
MSWindowsClipboardWorkerLifetime::notificationToken() const
{
    return m_notificationToken;
}

void
MSWindowsClipboardWorkerLifetime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
}

bool
MSWindowsClipboardWorkerLifetime::stopRequested() const
{
    return m_stopRequested.load(std::memory_order_acquire);
}

bool
MSWindowsClipboardWorkerLifetime::acceptsNotification(
    std::uint64_t token) const
{
    return token != 0 && token == m_notificationToken && !stopRequested();
}

void
MSWindowsClipboardWorkerLifetime::markFinished()
{
    {
        std::lock_guard<std::mutex> lock(m_finishedMutex);
        m_finished = true;
    }
    m_finishedChanged.notify_all();
}

bool
MSWindowsClipboardWorkerLifetime::waitForFinished(
    std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(m_finishedMutex);
    return m_finishedChanged.wait_for(lock, timeout, [this]() {
        return m_finished;
    });
}

MSWindowsClipboardSnapshotState::Request::Request() :
    generation(0),
    windowsSequence(0),
    protocolSequence(0),
    attempt(0)
{
}

MSWindowsClipboardSnapshotState::MSWindowsClipboardSnapshotState() :
    m_generation(0),
    m_windowsSequence(0),
    m_status(Status::Idle),
    m_hasPendingRequest(false),
    m_pendingRequest(),
    m_snapshot(),
    m_readyProtocolSequence(0),
    m_notificationPending(false)
{
}

MSWindowsClipboardSnapshotState::Request
MSWindowsClipboardSnapshotState::queue(UInt32 windowsSequence,
                                       UInt32 protocolSequence)
{
    ++m_generation;
    if (m_generation == 0) {
        ++m_generation;
    }

    Request request;
    request.generation = m_generation;
    request.windowsSequence = windowsSequence;
    request.protocolSequence = protocolSequence;
    request.attempt = 0;

    m_windowsSequence = windowsSequence;
    m_status = Status::Pending;
    m_hasPendingRequest = true;
    m_pendingRequest = request;
    m_snapshot.reset();
    m_readyProtocolSequence = 0;
    m_notificationPending = false;
    return request;
}

void
MSWindowsClipboardSnapshotState::invalidate()
{
    ++m_generation;
    if (m_generation == 0) {
        ++m_generation;
    }
    m_windowsSequence = 0;
    m_status = Status::Idle;
    m_hasPendingRequest = false;
    m_pendingRequest = Request();
    m_snapshot.reset();
    m_readyProtocolSequence = 0;
    m_notificationPending = false;
}

bool
MSWindowsClipboardSnapshotState::hasPending() const
{
    return m_hasPendingRequest;
}

bool
MSWindowsClipboardSnapshotState::takeNext(Request* request)
{
    if (request == NULL || !m_hasPendingRequest) {
        return false;
    }

    *request = m_pendingRequest;
    m_hasPendingRequest = false;
    return true;
}

bool
MSWindowsClipboardSnapshotState::matches(const Request& request) const
{
    return request.generation != 0 &&
        request.generation == m_generation &&
        request.windowsSequence != 0 &&
        request.windowsSequence == m_windowsSequence &&
        m_status == Status::Pending;
}

bool
MSWindowsClipboardSnapshotState::complete(
    const Request& request, UInt32 currentWindowsSequence,
    const std::shared_ptr<const String>& snapshot)
{
    if (!snapshot || !matches(request) ||
        currentWindowsSequence != request.windowsSequence) {
        return false;
    }

    m_status = Status::Ready;
    m_snapshot = snapshot;
    m_readyProtocolSequence = request.protocolSequence;
    m_notificationPending = true;
    return true;
}

bool
MSWindowsClipboardSnapshotState::retry(const Request& request,
                                       UInt32 maxAttempts)
{
    if (!matches(request) || request.attempt >= maxAttempts) {
        return false;
    }

    m_pendingRequest = request;
    ++m_pendingRequest.attempt;
    m_hasPendingRequest = true;
    return true;
}

void
MSWindowsClipboardSnapshotState::fail(const Request& request)
{
    if (matches(request)) {
        m_status = Status::Failed;
        m_snapshot.reset();
        m_readyProtocolSequence = 0;
        m_notificationPending = false;
    }
}

bool
MSWindowsClipboardSnapshotState::takeReadyNotification(
    UInt32* protocolSequence)
{
    if (protocolSequence == NULL || !m_notificationPending ||
        m_status != Status::Ready || !m_snapshot) {
        return false;
    }

    *protocolSequence = m_readyProtocolSequence;
    m_notificationPending = false;
    return true;
}

bool
MSWindowsClipboardSnapshotState::copyReady(
    std::shared_ptr<const String>* snapshot, UInt32* windowsSequence) const
{
    if (snapshot == NULL || windowsSequence == NULL ||
        m_status != Status::Ready || !m_snapshot) {
        return false;
    }

    *snapshot = m_snapshot;
    *windowsSequence = m_windowsSequence;
    return true;
}

bool
MSWindowsClipboardSnapshotState::needsSnapshot(UInt32 windowsSequence) const
{
    if (windowsSequence == 0) {
        return false;
    }
    return m_status == Status::Idle || windowsSequence != m_windowsSequence;
}

bool
MSWindowsClipboardSnapshotState::blocksSynchronousRead() const
{
    return m_status == Status::Pending || m_status == Status::Failed;
}

MSWindowsClipboardPublishState::Request::Request() :
    generation(0),
    publicationId(0),
    baseInFlightGeneration(0),
    attempt(0),
    expectedWindowsSequence(0),
    snapshot()
{
}

MSWindowsClipboardPublishState::Completion::Completion() :
    publicationId(0),
    result(CompletionResult::Failed),
    committedWindowsSequence(0)
{
}

MSWindowsClipboardPublishState::MSWindowsClipboardPublishState() :
    m_generation(0),
    m_pending(false),
    m_inFlight(false),
    m_pendingRequest(),
    m_inFlightRequest(),
    m_completions()
{
}

MSWindowsClipboardPublishState::Request
MSWindowsClipboardPublishState::queue(
    const std::shared_ptr<const String>& snapshot,
    UInt32 expectedWindowsSequence,
    std::uint64_t publicationId)
{
    if (m_pending) {
        recordCompletion(
            m_pendingRequest, CompletionResult::Superseded);
    }
    ++m_generation;
    if (m_generation == 0) {
        ++m_generation;
    }

    m_pendingRequest = Request();
    m_pendingRequest.generation = m_generation;
    m_pendingRequest.publicationId = publicationId;
    m_pendingRequest.baseInFlightGeneration = m_inFlight
        ? m_inFlightRequest.generation : 0;
    m_pendingRequest.expectedWindowsSequence = expectedWindowsSequence;
    m_pendingRequest.snapshot = snapshot;
    m_pending = snapshot != NULL;
    return m_pendingRequest;
}

void
MSWindowsClipboardPublishState::invalidate()
{
    if (m_pending) {
        recordCompletion(
            m_pendingRequest, CompletionResult::Superseded);
    }
    ++m_generation;
    if (m_generation == 0) {
        ++m_generation;
    }
    m_pending = false;
    m_pendingRequest = Request();
}

bool
MSWindowsClipboardPublishState::hasPending() const
{
    return m_pending;
}

bool
MSWindowsClipboardPublishState::hasInFlight() const
{
    return m_inFlight;
}

bool
MSWindowsClipboardPublishState::hasCompletion() const
{
    return !m_completions.empty();
}

bool
MSWindowsClipboardPublishState::takeNext(Request* request)
{
    if (request == NULL || m_inFlight || !m_pending ||
        !m_pendingRequest.snapshot) {
        return false;
    }
    *request = m_pendingRequest;
    m_pending = false;
    m_inFlight = true;
    m_inFlightRequest = *request;
    return true;
}

bool
MSWindowsClipboardPublishState::isInFlight(const Request& request) const
{
    return request.generation != 0 && m_inFlight &&
        request.generation == m_inFlightRequest.generation &&
        request.snapshot != NULL;
}

bool
MSWindowsClipboardPublishState::matches(const Request& request) const
{
    return isInFlight(request) && request.generation == m_generation;
}

bool
MSWindowsClipboardPublishState::retry(const Request& request,
                                      UInt32 maxAttempts)
{
    if (!isInFlight(request)) {
        return false;
    }
    m_inFlight = false;
    m_inFlightRequest = Request();
    if (request.generation != m_generation) {
        return false;
    }
    if (request.attempt >= maxAttempts) {
        m_pending = false;
        m_pendingRequest = Request();
        recordCompletion(request, CompletionResult::Failed);
        return false;
    }
    m_pendingRequest = request;
    ++m_pendingRequest.attempt;
    m_pending = true;
    return true;
}

bool
MSWindowsClipboardPublishState::complete(
    const Request& request, UInt32 committedWindowsSequence)
{
    if (!isInFlight(request)) {
        return false;
    }
    m_inFlight = false;
    m_inFlightRequest = Request();

    const bool current = request.generation == m_generation;
    if (!current && m_pending && committedWindowsSequence != 0 &&
        m_pendingRequest.baseInFlightGeneration == request.generation) {
        m_pendingRequest.expectedWindowsSequence =
            committedWindowsSequence;
    }
    if (current) {
        m_pendingRequest = Request();
    }
    recordCompletion(
        request,
        current ? CompletionResult::Succeeded : CompletionResult::Superseded,
        current ? committedWindowsSequence : 0);
    return current;
}

void
MSWindowsClipboardPublishState::fail(
    const Request& request, CompletionResult result)
{
    if (isInFlight(request)) {
        m_inFlight = false;
        m_inFlightRequest = Request();
        const bool current = request.generation == m_generation;
        if (current) {
            m_pending = false;
            m_pendingRequest = Request();
        }
        recordCompletion(
            request,
            current ? result : CompletionResult::Superseded);
    }
}

bool
MSWindowsClipboardPublishState::takeCompletion(Completion* completion)
{
    if (completion == NULL || m_completions.empty()) {
        return false;
    }
    *completion = m_completions.front();
    m_completions.pop_front();
    return true;
}

void
MSWindowsClipboardPublishState::recordCompletion(
    const Request& request, CompletionResult result,
    UInt32 committedWindowsSequence)
{
    if (request.publicationId == 0) {
        return;
    }
    Completion completion;
    completion.publicationId = request.publicationId;
    completion.result = result;
    completion.committedWindowsSequence = committedWindowsSequence;
    m_completions.push_back(completion);
}
