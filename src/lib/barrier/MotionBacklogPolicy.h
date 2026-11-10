/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace barrier {

// Local queue age only; never compare timestamps originating on another host.
struct QueuedMotion {
    using Clock = std::chrono::steady_clock;
    enum Kind { Absolute, Relative };
    Kind kind;
    std::int32_t x, y;
    Clock::time_point received;
    std::uint64_t segment;
    bool protectedPath;
};

inline bool compactStaleMotion(QueuedMotion& pending, const QueuedMotion& next,
                               QueuedMotion::Clock::time_point now)
{
    // Preserve the recent tail as well as drag/drawing paths. This threshold
    // is a conservative recovery trigger, not a pacing interval.
    const auto staleAge = std::chrono::milliseconds(100);
    if (pending.kind != next.kind || pending.segment != next.segment ||
        pending.protectedPath || next.protectedPath ||
        now - pending.received < staleAge || now - next.received < staleAge) {
        return false;
    }
    if (pending.kind == QueuedMotion::Relative) {
        const std::int64_t x = static_cast<std::int64_t>(pending.x) + next.x;
        const std::int64_t y = static_cast<std::int64_t>(pending.y) + next.y;
        const auto min = (std::numeric_limits<std::int32_t>::min)();
        const auto max = (std::numeric_limits<std::int32_t>::max)();
        // Keep separate commands on overflow; never lose relative distance.
        if (x < min || x > max || y < min || y > max) {
            return false;
        }
        pending.x = static_cast<std::int32_t>(x);
        pending.y = static_cast<std::int32_t>(y);
    }
    else {
        pending.x = next.x;
        pending.y = next.y;
    }
    pending.received = next.received;
    return true;
}

}
