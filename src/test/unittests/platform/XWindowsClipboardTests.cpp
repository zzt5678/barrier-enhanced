#include "test/global/gtest.h"

#include "platform/XWindowsClipboard.h"

TEST(XWindowsClipboardTests, providerTimeoutCannotBecomeSuccessfulEmptyRead)
{
    EXPECT_FALSE(XWindowsClipboard::isProviderReadValidForTest(
        false, false, false));
    EXPECT_FALSE(XWindowsClipboard::isProviderReadValidForTest(
        true, true, false));
}

TEST(XWindowsClipboardTests, responsiveProviderCanExposeEmptySelection)
{
    EXPECT_TRUE(XWindowsClipboard::isProviderReadValidForTest(
        true, false, false));
}

TEST(XWindowsClipboardTests, successfulFormatReadIsValidWithoutTargets)
{
    // Compatibility probing supports owners that omit usable formats from
    // TARGETS or do not implement TARGETS at all.
    EXPECT_TRUE(XWindowsClipboard::isProviderReadValidForTest(
        false, false, true));
}
