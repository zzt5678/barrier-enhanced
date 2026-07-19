/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "platform/MSWindowsWatchdog.h"
#include "common/win32/encoding_utilities.h"

#include "test/global/gtest.h"

#include <algorithm>

TEST(MSWindowsWatchdogLaunchTests, windowsLaunchEncodingPreservesUnicode)
{
    const std::string command =
        u8"C:\\Users\\\u7528\u6237\\Weave\\weavec.exe --name "
        u8"\u663e\u793a\u5668\U0001f600";

    const std::vector<WCHAR> wide = utf8_to_win_char(command);

    ASSERT_GT(wide.size(), 1u);
    EXPECT_EQ(0, wide.back());
    EXPECT_EQ(command, win_wchar_to_utf8(wide.data()));
}

TEST(MSWindowsWatchdogLaunchTests, windowsLaunchEncodingRejectsMalformedUtf8)
{
    const std::string malformed("bad\xc0\xaf", 5u);

    const std::vector<WCHAR> wide = utf8_to_win_char(malformed);

    ASSERT_EQ(1u, wide.size());
    EXPECT_EQ(0, wide.front());
}

TEST(MSWindowsWatchdogLaunchTests, windowsLaunchEncodingRejectsEmbeddedNull)
{
    const std::string embeddedNull("weavec.exe\0--name hidden", 24u);

    const std::vector<WCHAR> wide = utf8_to_win_char(embeddedNull);

    ASSERT_EQ(1u, wide.size());
    EXPECT_EQ(0, wide.front());
}

TEST(MSWindowsWatchdogLaunchTests, windowsLaunchEncodingRejectsUnpairedSurrogate)
{
    const WCHAR malformed[] = { 0xd800, 0 };

    EXPECT_TRUE(win_wchar_to_utf8(malformed).empty());
}

TEST(MSWindowsWatchdogLaunchTests, readinessNonceFailsClosedWhenRandomSourceFails)
{
    std::uint64_t nonce = 0xfeedfaceu;

    EXPECT_FALSE(MSWindowsWatchdog::generateReadinessNonce(
        nonce,
        [](unsigned char*, std::size_t) {
            return false;
        }));
    EXPECT_EQ(0u, nonce);
}

TEST(MSWindowsWatchdogLaunchTests, readinessNonceUsesInjectedRandomBytes)
{
    std::uint64_t nonce = 0;
    EXPECT_TRUE(MSWindowsWatchdog::generateReadinessNonce(
        nonce,
        [](unsigned char* bytes, std::size_t size) {
            std::fill(bytes, bytes + size, 0xa5);
            return true;
        }));
    EXPECT_EQ(0xa5a5a5a5a5a5a5a5ULL, nonce);
}

TEST(MSWindowsWatchdogLaunchTests, shutdownWaitBudgetNeverExtendsDeadline)
{
    EXPECT_DOUBLE_EQ(0.25, MSWindowsWatchdog::boundedShutdownWaitSeconds(
        100.25, 100.0, 5.0));
    EXPECT_NEAR(0.05, MSWindowsWatchdog::boundedShutdownWaitSeconds(
        100.25, 100.20, 5.0), 1e-12);
    EXPECT_DOUBLE_EQ(0.0, MSWindowsWatchdog::boundedShutdownWaitSeconds(
        100.25, 100.25, 5.0));
    EXPECT_DOUBLE_EQ(0.0, MSWindowsWatchdog::boundedShutdownWaitSeconds(
        100.25, 100.50, 5.0));
    EXPECT_DOUBLE_EQ(0.0, MSWindowsWatchdog::boundedShutdownWaitSeconds(
        100.25, 100.0, -1.0));
}

TEST(MSWindowsWatchdogLaunchTests, sequentialShutdownWaitsShareOneBudget)
{
    const double deadline = 205.0;
    const double mainWait = MSWindowsWatchdog::boundedShutdownWaitSeconds(
        deadline, 200.25, 5.0);
    const double outputWait = MSWindowsWatchdog::boundedShutdownWaitSeconds(
        deadline, 204.75, 5.0);

    EXPECT_DOUBLE_EQ(4.75, mainWait);
    EXPECT_DOUBLE_EQ(0.25, outputWait);
    EXPECT_DOUBLE_EQ(5.0, mainWait + outputWait);
}

TEST(MSWindowsWatchdogLaunchTests, stopAckRequiresNoPublishedOrPendingNode)
{
    EXPECT_TRUE(MSWindowsWatchdog::stopConfirmationReady(
        true, false, false));
    EXPECT_FALSE(MSWindowsWatchdog::stopConfirmationReady(
        false, false, false));
    EXPECT_FALSE(MSWindowsWatchdog::stopConfirmationReady(
        true, true, false));
    EXPECT_FALSE(MSWindowsWatchdog::stopConfirmationReady(
        true, false, true));
    EXPECT_FALSE(MSWindowsWatchdog::stopConfirmationReady(
        true, true, true));
}

TEST(MSWindowsWatchdogLaunchTests, standbyOptionMustBeAnExactArgument)
{
    EXPECT_TRUE(MSWindowsWatchdog::containsInternalStandbyOption(
        "\"C:\\Program Files\\Weave\\weavec.exe\" --service-standby"));
    EXPECT_TRUE(MSWindowsWatchdog::containsInternalStandbyOption(
        "weavec.exe \"--service-standby\""));
    EXPECT_FALSE(MSWindowsWatchdog::containsInternalStandbyOption(
        "weavec.exe --name=--service-standby"));
    EXPECT_FALSE(MSWindowsWatchdog::containsInternalStandbyOption(
        "weavec.exe --service-standby-extra"));
}

TEST(MSWindowsWatchdogLaunchTests, externalCommandsNeverAcceptStandbyOption)
{
    EXPECT_FALSE(MSWindowsWatchdog::isExternalCommandAccepted(
        "\"C:\\Program Files\\Weave\\weavec.exe\" --service-standby"));
    EXPECT_FALSE(MSWindowsWatchdog::isExternalCommandAccepted(
        "weavec.exe \"--service-standby\""));
    EXPECT_TRUE(MSWindowsWatchdog::isExternalCommandAccepted(
        "weavec.exe --name=--service-standby"));
    EXPECT_TRUE(MSWindowsWatchdog::isExternalCommandAccepted(
        "weavec.exe --service-standby-extra"));
}

TEST(MSWindowsWatchdogLaunchTests, standbyCommandIsDerivedWithoutMutation)
{
    const std::string persisted =
        "\"C:\\Program Files\\Weave\\weavec.exe\" --name windows";

    const std::string launch =
        MSWindowsWatchdog::makeStandbyLaunchCommand(persisted);

    EXPECT_EQ("\"C:\\Program Files\\Weave\\weavec.exe\" --name windows",
              persisted);
    EXPECT_EQ(persisted + " --service-standby", launch);
    EXPECT_EQ(launch,
              MSWindowsWatchdog::makeStandbyLaunchCommand(launch));
}

TEST(MSWindowsWatchdogLaunchTests, sameOwnerAllowsANewerProfileGeneration)
{
    MSWindowsWatchdog::LaunchProfile current;
    current.authenticated = true;
    current.sessionId = 7;
    current.userSid = "S-1-5-21-100";
    current.profileDirectory = "C:\\Users\\owner\\old";
    current.generation = "generation-b";
    current.digest = "digest-b";

    MSWindowsWatchdog::LaunchProfile next = current;
    next.profileDirectory = "C:\\Users\\owner\\new";
    next.generation = "generation-c";
    next.digest = "digest-c";

    EXPECT_TRUE(MSWindowsWatchdog::isSameAuthenticatedLaunchOwner(
        current, next));

    next.sessionId = 8;
    EXPECT_FALSE(MSWindowsWatchdog::isSameAuthenticatedLaunchOwner(
        current, next));
    next = current;
    next.userSid = "S-1-5-21-200";
    EXPECT_FALSE(MSWindowsWatchdog::isSameAuthenticatedLaunchOwner(
        current, next));
    next = current;
    next.authenticated = false;
    EXPECT_FALSE(MSWindowsWatchdog::isSameAuthenticatedLaunchOwner(
        current, next));
}

TEST(MSWindowsWatchdogLaunchTests, activationWaitStopsForOwnershipChanges)
{
    EXPECT_FALSE(MSWindowsWatchdog::shouldAbortPendingActivation(
        true, false, true, false));
    EXPECT_TRUE(MSWindowsWatchdog::shouldAbortPendingActivation(
        false, false, true, false));
    EXPECT_TRUE(MSWindowsWatchdog::shouldAbortPendingActivation(
        true, true, true, false));
    EXPECT_TRUE(MSWindowsWatchdog::shouldAbortPendingActivation(
        true, false, false, false));
    EXPECT_TRUE(MSWindowsWatchdog::shouldAbortPendingActivation(
        true, false, true, true));
}

TEST(MSWindowsWatchdogLaunchTests, failedActivationOnlyDiscardsForProvenSupersession)
{
    EXPECT_FALSE(MSWindowsWatchdog::shouldDiscardFailedActivation(
        true, false, true));
    EXPECT_TRUE(MSWindowsWatchdog::shouldDiscardFailedActivation(
        false, false, true));
    EXPECT_TRUE(MSWindowsWatchdog::shouldDiscardFailedActivation(
        true, true, true));
    EXPECT_TRUE(MSWindowsWatchdog::shouldDiscardFailedActivation(
        true, false, false));
}

TEST(MSWindowsWatchdogLaunchTests,
     unknownDesktopRequiresDiscoveryThenAnExactLaunch)
{
    const DesktopSwitchPolicy::LaunchTarget discovery =
        DesktopSwitchPolicy::resolveLaunchTarget("", "", true);

    EXPECT_EQ("Default", discovery.desktopName);
    EXPECT_FALSE(discovery.expectedDesktopKnown);
    EXPECT_EQ(DesktopSwitchPolicy::LaunchPurpose::Discovery,
              discovery.purpose);

    const DesktopSwitchPolicy::LaunchTarget exact =
        DesktopSwitchPolicy::resolveLaunchTarget("", "Winlogon", true);

    EXPECT_EQ("Winlogon", exact.desktopName);
    EXPECT_TRUE(exact.expectedDesktopKnown);
    EXPECT_EQ(DesktopSwitchPolicy::LaunchPurpose::Exact, exact.purpose);
}

TEST(MSWindowsWatchdogLaunchTests,
     observedDesktopSupersedesTransactionLocalDiscoveryEvidence)
{
    const DesktopSwitchPolicy::LaunchTarget exact =
        DesktopSwitchPolicy::resolveLaunchTarget(
            "Default", "Winlogon", true);

    EXPECT_EQ("Default", exact.desktopName);
    EXPECT_TRUE(exact.expectedDesktopKnown);
    EXPECT_EQ(DesktopSwitchPolicy::LaunchPurpose::Exact, exact.purpose);
}
