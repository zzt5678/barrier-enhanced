/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "barrier/clipboard_types.h"
#include "base/String.h"
#include "common/common.h"

#include <X11/Xlib.h>

#include <cstdint>
#include <memory>

// Thread safety is supplied by the owner. Keeping this class independent of
// Xlib I/O makes the generation/owner rules deterministic and unit-testable.
class XWindowsClipboardSnapshotState {
public:
    struct Request {
        Request();

        ClipboardID id;
        std::uint64_t generation;
        std::uint64_t order;
        Window owner;
        Time timestamp;
        UInt32 sequenceNumber;
        UInt32 attempt;
    };

    XWindowsClipboardSnapshotState();

    Request queue(ClipboardID id, Window owner, Time timestamp,
                  UInt32 sequenceNumber);
    void invalidate(ClipboardID id, Window owner);

    bool hasPending() const;
    bool takeNext(Request* request);
    bool complete(const Request& request, Window currentOwner,
                  const std::shared_ptr<const String>& snapshot);
    bool retry(const Request& request, Window currentOwner,
               UInt32 maxAttempts);
    bool retryWithoutOwnerObservation(const Request& request,
                                      UInt32 maxAttempts);
    void fail(const Request& request, Window currentOwner);
    void failWithoutOwnerObservation(const Request& request);

    bool copyReady(ClipboardID id,
                   std::shared_ptr<const String>* snapshot,
                   Window* owner, Time* timestamp) const;
    bool blocksSynchronousRead(ClipboardID id) const;
    Window currentOwner(ClipboardID id) const;

private:
    enum class SnapshotStatus {
        Idle,
        Pending,
        Ready,
        Failed
    };

    struct Slot {
        Slot();

        std::uint64_t generation;
        Window owner;
        SnapshotStatus status;
        bool hasPendingRequest;
        Request pendingRequest;
        std::shared_ptr<const String> snapshot;
    };

    bool matches(const Slot& slot, const Request& request,
                 Window currentOwner) const;
    bool matchesRequest(const Slot& slot, const Request& request) const;
    bool queueRetry(Slot& slot, const Request& request,
                    UInt32 maxAttempts);

    Slot m_slots[kClipboardEnd];
    std::uint64_t m_nextOrder;
};
