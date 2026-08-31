/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "platform/XWindowsClipboardSnapshotState.h"

#include <limits>

XWindowsClipboardSnapshotState::Request::Request() :
    id(kClipboardEnd),
    generation(0),
    order(0),
    owner(None),
    timestamp(CurrentTime),
    sequenceNumber(0),
    attempt(0)
{
}

XWindowsClipboardSnapshotState::Slot::Slot() :
    generation(0),
    owner(None),
    status(SnapshotStatus::Idle),
    hasPendingRequest(false),
    pendingRequest(),
    snapshot()
{
}

XWindowsClipboardSnapshotState::XWindowsClipboardSnapshotState() :
    m_nextOrder(0)
{
}

XWindowsClipboardSnapshotState::Request
XWindowsClipboardSnapshotState::queue(ClipboardID id, Window owner,
                                      Time timestamp,
                                      UInt32 sequenceNumber)
{
    Slot& slot = m_slots[id];
    ++slot.generation;
    if (slot.generation == 0) {
        ++slot.generation;
    }
    ++m_nextOrder;
    if (m_nextOrder == 0) {
        ++m_nextOrder;
    }

    Request request;
    request.id = id;
    request.generation = slot.generation;
    request.order = m_nextOrder;
    request.owner = owner;
    request.timestamp = timestamp;
    request.sequenceNumber = sequenceNumber;
    request.attempt = 0;

    slot.owner = owner;
    slot.status = SnapshotStatus::Pending;
    slot.hasPendingRequest = true;
    slot.pendingRequest = request;
    slot.snapshot.reset();
    return request;
}

void
XWindowsClipboardSnapshotState::invalidate(ClipboardID id, Window owner)
{
    Slot& slot = m_slots[id];
    ++slot.generation;
    if (slot.generation == 0) {
        ++slot.generation;
    }
    slot.owner = owner;
    slot.status = SnapshotStatus::Idle;
    slot.hasPendingRequest = false;
    slot.pendingRequest = Request();
    slot.snapshot.reset();
}

bool
XWindowsClipboardSnapshotState::hasPending() const
{
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        if (m_slots[id].hasPendingRequest) {
            return true;
        }
    }
    return false;
}

bool
XWindowsClipboardSnapshotState::takeNext(Request* request)
{
    if (request == NULL) {
        return false;
    }

    ClipboardID next = kClipboardEnd;
    std::uint64_t nextOrder = (std::numeric_limits<std::uint64_t>::max)();
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        const Slot& slot = m_slots[id];
        if (slot.hasPendingRequest &&
            slot.pendingRequest.order < nextOrder) {
            next = id;
            nextOrder = slot.pendingRequest.order;
        }
    }
    if (next == kClipboardEnd) {
        return false;
    }

    Slot& slot = m_slots[next];
    *request = slot.pendingRequest;
    slot.hasPendingRequest = false;
    return true;
}

bool
XWindowsClipboardSnapshotState::matches(const Slot& slot,
                                        const Request& request,
                                        Window currentOwner) const
{
    return matchesRequest(slot, request) &&
        request.owner == currentOwner;
}

bool
XWindowsClipboardSnapshotState::matchesRequest(
    const Slot& slot, const Request& request) const
{
    return request.id < kClipboardEnd &&
        request.generation == slot.generation &&
        request.owner != None &&
        request.owner == slot.owner &&
        slot.status == SnapshotStatus::Pending;
}

bool
XWindowsClipboardSnapshotState::complete(
    const Request& request, Window currentOwner,
    const std::shared_ptr<const String>& snapshot)
{
    if (request.id >= kClipboardEnd || !snapshot) {
        return false;
    }

    Slot& slot = m_slots[request.id];
    if (!matches(slot, request, currentOwner)) {
        return false;
    }

    slot.status = SnapshotStatus::Ready;
    slot.pendingRequest = request;
    slot.snapshot = snapshot;
    return true;
}

bool
XWindowsClipboardSnapshotState::retry(const Request& request,
                                      Window currentOwner,
                                      UInt32 maxAttempts)
{
    if (request.id >= kClipboardEnd) {
        return false;
    }

    Slot& slot = m_slots[request.id];
    if (!matches(slot, request, currentOwner)) {
        return false;
    }

    return queueRetry(slot, request, maxAttempts);
}

bool
XWindowsClipboardSnapshotState::retryWithoutOwnerObservation(
    const Request& request, UInt32 maxAttempts)
{
    if (request.id >= kClipboardEnd) {
        return false;
    }

    Slot& slot = m_slots[request.id];
    if (!matchesRequest(slot, request)) {
        return false;
    }

    return queueRetry(slot, request, maxAttempts);
}

bool
XWindowsClipboardSnapshotState::queueRetry(
    Slot& slot, const Request& request, UInt32 maxAttempts)
{
    if (request.attempt >= maxAttempts) {
        return false;
    }

    Request retryRequest = request;
    ++retryRequest.attempt;
    ++m_nextOrder;
    if (m_nextOrder == 0) {
        ++m_nextOrder;
    }
    retryRequest.order = m_nextOrder;
    slot.hasPendingRequest = true;
    slot.pendingRequest = retryRequest;
    return true;
}

void
XWindowsClipboardSnapshotState::fail(const Request& request,
                                     Window currentOwner)
{
    if (request.id >= kClipboardEnd) {
        return;
    }

    Slot& slot = m_slots[request.id];
    if (matches(slot, request, currentOwner)) {
        slot.status = SnapshotStatus::Failed;
        slot.snapshot.reset();
    }
}

void
XWindowsClipboardSnapshotState::failWithoutOwnerObservation(
    const Request& request)
{
    if (request.id >= kClipboardEnd) {
        return;
    }

    Slot& slot = m_slots[request.id];
    if (matchesRequest(slot, request)) {
        slot.status = SnapshotStatus::Failed;
        slot.snapshot.reset();
    }
}

bool
XWindowsClipboardSnapshotState::copyReady(
    ClipboardID id, std::shared_ptr<const String>* snapshot,
    Window* owner, Time* timestamp) const
{
    if (id >= kClipboardEnd || snapshot == NULL || owner == NULL ||
        timestamp == NULL) {
        return false;
    }

    const Slot& slot = m_slots[id];
    if (slot.status != SnapshotStatus::Ready || !slot.snapshot) {
        return false;
    }

    *snapshot = slot.snapshot;
    *owner = slot.owner;
    *timestamp = slot.pendingRequest.timestamp;
    return true;
}

bool
XWindowsClipboardSnapshotState::blocksSynchronousRead(ClipboardID id) const
{
    if (id >= kClipboardEnd) {
        return false;
    }
    const SnapshotStatus status = m_slots[id].status;
    return status == SnapshotStatus::Pending ||
        status == SnapshotStatus::Failed;
}

Window
XWindowsClipboardSnapshotState::currentOwner(ClipboardID id) const
{
    return id < kClipboardEnd ? m_slots[id].owner : None;
}
