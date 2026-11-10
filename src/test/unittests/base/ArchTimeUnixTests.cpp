#include "test/global/gtest.h"

#if SYSAPI_UNIX
#include "arch/unix/ArchTimeUnix.h"
#include <chrono>

TEST(ArchTimeUnixTests, deadlinesUseSteadyClockRatherThanCivilTime)
{
    ArchTimeUnix clock;
    const double before = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const double sampled = clock.time();
    const double after = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Bracket an independent OS-backed duration-clock sample. Calendar time
    // is in a different domain and can jump when NTP/user settings change.
    EXPECT_GE(sampled, before);
    EXPECT_LE(sampled, after);
}
#endif
