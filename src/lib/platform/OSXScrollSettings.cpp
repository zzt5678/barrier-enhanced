#include "platform/OSXScrollSettings.h"
#include "mt/ThreadShutdown.h"
#include <CoreFoundation/CoreFoundation.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>

struct OSXScrollSettings::State {
    explicit State(Reader read) : reader(std::move(read)) {}
    Reader reader;
    std::atomic<double> multiplier{120.0};
    std::mutex mutex;
    std::condition_variable wake;
    bool requested = true, stopping = false, exited = false;
};

OSXScrollSettings::OSXScrollSettings(Reader reader) : m_state(std::make_shared<State>(std::move(reader)))
{
}

OSXScrollSettings::~OSXScrollSettings()
{
    if (!m_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->stopping = true;
    }
    m_state->wake.notify_one();
    barrier::waitForFinalThreadShutdown("macOS scroll settings",
        barrier::kFinalThreadShutdownDeadlineSeconds, [this](double seconds) {
            std::unique_lock<std::mutex> lock(m_state->mutex);
            return m_state->wake.wait_for(lock, std::chrono::duration<double>(seconds),
                [this] { return m_state->exited; });
        });
    m_thread.join();
}

std::int32_t OSXScrollSettings::convert(float delta) const
{
    if (!std::isfinite(delta)) return 0;
    const double value = delta * m_state->multiplier.load(std::memory_order_relaxed);
    if (value >= std::numeric_limits<std::int32_t>::max()) return std::numeric_limits<std::int32_t>::max();
    if (value <= std::numeric_limits<std::int32_t>::min()) return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(value);
}

void OSXScrollSettings::requestRefresh()
{
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        if (m_state->stopping) return;
        m_state->requested = true;
        if (!m_thread.joinable()) {
            const auto state = m_state;
            m_thread = std::thread([state] { run(state); });
        }
    }
    m_state->wake.notify_one();
}

void OSXScrollSettings::run(const std::shared_ptr<State>& state)
{
    pthread_setname_np("Weave scroll settings");
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->wake.wait(lock, [&] { return state->stopping || state->requested; });
            if (state->stopping) break;
            state->requested = false;
        }
        try {
            double scale = state->reader();
            if (!std::isfinite(scale) || scale < 0) scale = 0;
            double factor = 120.0 * (1.0 + scale) / std::pow(10.0, scale);
            if (!std::isfinite(factor) || factor <= 0) factor = 120.0;
            state->multiplier.store(factor, std::memory_order_relaxed);
        }
        catch (...) {
            // An unavailable preference service must not change input state.
        }
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->exited = true;
    }
    state->wake.notify_all();
}

double OSXScrollSettings::readNativeScaling()
{
    double scale = 0;
    CFPropertyListRef pref = CFPreferencesCopyValue(CFSTR("com.apple.scrollwheel.scaling"),
        kCFPreferencesAnyApplication, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
    if (pref) {
        if (CFGetTypeID(pref) == CFNumberGetTypeID())
            CFNumberGetValue(static_cast<CFNumberRef>(pref), kCFNumberDoubleType, &scale);
        CFRelease(pref);
    }
    return scale;
}
