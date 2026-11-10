#include "platform/OSXScrollSettings.h"
#include "test/global/gtest.h"
#include <future>
#include <atomic>
#include <limits>
#include <thread>
#include <vector>

TEST(OSXScrollSettingsTests, slowPreferenceReadCannotBlockInputConversion)
{
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    OSXScrollSettings settings([&] { entered.set_value(); gate.wait(); return 1.0; });
    settings.requestRefresh();
    entered.get_future().wait();
    auto conversion = std::async(std::launch::async, [&] {
        return std::make_pair(settings.convert(1.0f), settings.convert(-0.5f));
    });
    const auto status = conversion.wait_for(std::chrono::milliseconds(200));
    release.set_value();
    const auto values = conversion.get();
    EXPECT_EQ(std::future_status::ready, status);
    // Initial fallback, independent of an unavailable preference service.
    EXPECT_EQ(120, values.first);
    EXPECT_EQ(-60, values.second);
}

TEST(OSXScrollSettingsTests, publishedSettingPreservesScrollFormula)
{
    // scale=1: 120*(1+1)/10=24 units per wheel unit.
    OSXScrollSettings settings([] { return 1.0; });
    settings.requestRefresh();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (settings.convert(1.0f) != 24 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    EXPECT_EQ(24, settings.convert(1.0f));
    EXPECT_EQ(-12, settings.convert(-0.5f));
    EXPECT_EQ(0, settings.convert(0));
}

TEST(OSXScrollSettingsTests, conversionRejectsNonFiniteAndClampsHugeInput)
{
    OSXScrollSettings settings([] { return std::numeric_limits<double>::quiet_NaN(); });
    EXPECT_EQ(0, settings.convert(std::numeric_limits<float>::quiet_NaN()));
    EXPECT_EQ(0, settings.convert(std::numeric_limits<float>::infinity()));
    EXPECT_EQ(2147483647, settings.convert(std::numeric_limits<float>::max()));
    EXPECT_EQ(std::numeric_limits<std::int32_t>::min(), settings.convert(-std::numeric_limits<float>::max()));
}

TEST(OSXScrollSettingsTests, refreshesCoalesceWhileNativeReaderIsBlocked)
{
    std::promise<void> entered, release, refreshed;
    auto gate = release.get_future().share();
    std::atomic<int> calls{0};
    OSXScrollSettings settings([&] {
        const int call = ++calls;
        if (call == 1) { entered.set_value(); gate.wait(); return 0.0; }
        if (call == 2) refreshed.set_value();
        return 1.0;
    });
    settings.requestRefresh();
    entered.get_future().wait();
    for (int i = 0; i < 1000; ++i) settings.requestRefresh();
    release.set_value();
    EXPECT_EQ(std::future_status::ready, refreshed.get_future().wait_for(std::chrono::seconds(2)));
    EXPECT_EQ(2, calls.load());
}
