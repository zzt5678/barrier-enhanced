#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace barrier {

// Transitional polling policy until streams and the event queue expose a
// cancellable low-watermark notification. Times supplied by production callers
// come from steady_clock; explicit time points keep the policy deterministic in tests.
class BackpressureWait {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    BackpressureWait(double timeoutSeconds, TimePoint now,
                     std::uint64_t pending, std::uint64_t written = 0) :
        m_timeout(timeoutSeconds), m_lastProgress(now),
        m_pending(pending), m_written(written)
    {
    }

    void observe(TimePoint now, std::uint64_t pending, std::uint64_t written = 0)
    {
        if (pending < m_pending || written > m_written) {
            m_lastProgress = now;
            m_delay = 0.001;
        }
        else {
            m_delay = std::min(0.010, m_delay * 2);
        }
        m_pending = pending;
        m_written = written;
    }

    bool expired(TimePoint now) const
    {
        return m_timeout > 0 && stalledSeconds(now) >= m_timeout;
    }

    double sleepSeconds(TimePoint now) const
    {
        return m_timeout > 0
            ? std::max(0.0, std::min(m_delay, m_timeout - stalledSeconds(now)))
            : m_delay;
    }

private:
    double stalledSeconds(TimePoint now) const
    {
        return std::chrono::duration<double>(now - m_lastProgress).count();
    }

    double m_timeout;
    TimePoint m_lastProgress;
    std::uint64_t m_pending;
    std::uint64_t m_written;
    double m_delay = 0.001;
};

}
