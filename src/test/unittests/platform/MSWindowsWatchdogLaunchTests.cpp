/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "platform/MSWindowsWatchdog.h"

#include "test/global/gtest.h"

#include <algorithm>

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
