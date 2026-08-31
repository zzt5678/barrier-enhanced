/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "platform/MSWindowsDesks.h"

#include "test/global/gtest.h"

TEST(MSWindowsDesksTests, desktopMustBeAttachedBeforeItCanAcceptInput)
{
    EXPECT_FALSE(MSWindowsDesks::isDesktopReadyForTest(
        false, false, false, true, true));
}

TEST(MSWindowsDesksTests, desktopMustHaveAMessageWindowBeforeItCanAcceptInput)
{
    EXPECT_FALSE(MSWindowsDesks::isDesktopReadyForTest(
        false, false, true, false, true));
}

TEST(MSWindowsDesksTests, secondaryDesktopDoesNotRequireCaptureHooks)
{
    EXPECT_TRUE(MSWindowsDesks::isDesktopReadyForTest(
        false, false, true, true, false));
}

TEST(MSWindowsDesksTests, secondaryDesktopRequiresSuccessfulInjectionProbe)
{
    EXPECT_FALSE(MSWindowsDesks::isDesktopReadyForTest(
        false, false, true, true, false, true, false));
    EXPECT_TRUE(MSWindowsDesks::isDesktopReadyForTest(
        false, false, true, true, false, true, true));
}

TEST(MSWindowsDesksTests, primaryDesktopUsesHookInsteadOfInjectionProbe)
{
    EXPECT_TRUE(MSWindowsDesks::isDesktopReadyForTest(
        true, false, true, true, true, true, false));
}

TEST(MSWindowsDesksTests, primaryDesktopRequiresInstalledHooks)
{
    EXPECT_FALSE(MSWindowsDesks::isDesktopReadyForTest(
        true, false, true, true, false));
    EXPECT_TRUE(MSWindowsDesks::isDesktopReadyForTest(
        true, false, true, true, true));
}

TEST(MSWindowsDesksTests, noHooksModeDoesNotRequireInstalledHooks)
{
    EXPECT_TRUE(MSWindowsDesks::isDesktopReadyForTest(
        true, true, true, true, false));
}

TEST(MSWindowsDesksTests, noHooksModeStillRequiresDesktopCapabilities)
{
    EXPECT_FALSE(MSWindowsDesks::isDesktopReadyForTest(
        true, true, false, true, false));
    EXPECT_FALSE(MSWindowsDesks::isDesktopReadyForTest(
        true, true, true, false, false));
}

TEST(MSWindowsDesksTests, installedHookDoesNotReplaceDesktopCapabilities)
{
    EXPECT_FALSE(MSWindowsDesks::isDesktopReadyForTest(
        true, false, false, true, true));
    EXPECT_FALSE(MSWindowsDesks::isDesktopReadyForTest(
        true, false, true, false, true));
}

TEST(MSWindowsDesksTests, unresponsiveDesktopMessageLoopRejectsInput)
{
    EXPECT_FALSE(MSWindowsDesks::isDesktopReadyForTest(
        false, false, true, true, false, false));
}

TEST(MSWindowsDesksTests, commandCompletionRequiresTargetSequence)
{
    EXPECT_FALSE(MSWindowsDesks::isDeskCommandCompleteForTest(12, 11, true));
    EXPECT_TRUE(MSWindowsDesks::isDeskCommandCompleteForTest(12, 12, true));
    EXPECT_TRUE(MSWindowsDesks::isDeskCommandCompleteForTest(12, 13, true));
    EXPECT_FALSE(MSWindowsDesks::isDeskCommandCompleteForTest(12, 12, false));
}

TEST(MSWindowsDesksTests, cancelledCommandSequenceCannotInjectLateInput)
{
    EXPECT_FALSE(MSWindowsDesks::shouldProcessDeskCommandForTest(20, 20));
    EXPECT_FALSE(MSWindowsDesks::shouldProcessDeskCommandForTest(19, 20));
    EXPECT_TRUE(MSWindowsDesks::shouldProcessDeskCommandForTest(21, 20));
}

TEST(MSWindowsDesksTests, onlyQueuedTimedOutCommandCanBeCancelled)
{
    EXPECT_TRUE(MSWindowsDesks::canCancelTimedOutDeskCommandForTest(20, 0));
    EXPECT_TRUE(MSWindowsDesks::canCancelTimedOutDeskCommandForTest(20, 19));
    EXPECT_FALSE(MSWindowsDesks::canCancelTimedOutDeskCommandForTest(20, 20));
    EXPECT_FALSE(MSWindowsDesks::canCancelTimedOutDeskCommandForTest(
        20, 0, true));
}

TEST(MSWindowsDesksTests, cancelledCommandCompletionDoesNotRestoreReadiness)
{
    EXPECT_FALSE(MSWindowsDesks::commandCompletionProvesResponsiveForTest(
        false, true, 20, 0));
    EXPECT_FALSE(MSWindowsDesks::commandCompletionProvesResponsiveForTest(
        true, true, 20, 20));
    EXPECT_FALSE(MSWindowsDesks::commandCompletionProvesResponsiveForTest(
        true, false, 21, 20));
    EXPECT_TRUE(MSWindowsDesks::commandCompletionProvesResponsiveForTest(
        true, true, 21, 20));
}

TEST(MSWindowsDesksTests, failedInjectionRejectsCompletedDeskCommand)
{
    EXPECT_FALSE(MSWindowsDesks::commandInjectionSucceededForTest(20, 20));
    EXPECT_TRUE(MSWindowsDesks::commandInjectionSucceededForTest(21, 20));
}

TEST(MSWindowsDesksTests, asyncDeskCommandQueueAllowsRoomBeforeLimit)
{
    EXPECT_TRUE(MSWindowsDesks::canQueueDeskCommandForTest(99, 90, 10));
    EXPECT_FALSE(MSWindowsDesks::canQueueDeskCommandForTest(100, 90, 10));
}

TEST(MSWindowsDesksTests, asyncDeskCommandQueueRejectsInvalidSequenceWindow)
{
    EXPECT_FALSE(MSWindowsDesks::canQueueDeskCommandForTest(90, 100, 10));
    EXPECT_FALSE(MSWindowsDesks::canQueueDeskCommandForTest(90, 90, 0));
}

TEST(MSWindowsDesksTests, cachedDeskMustRealignCompletionBeforeAsyncInput)
{
    EXPECT_FALSE(MSWindowsDesks::canQueueDeskCommandForTest(5000, 1, 128));
    EXPECT_TRUE(MSWindowsDesks::canQueueDeskCommandForTest(5000, 5000, 128));
}

TEST(MSWindowsDesksTests, asyncDeskCommandQueueLimitIsLatencyOriented)
{
    EXPECT_LE(MSWindowsDesks::maxPendingDeskCommandsForTest(), 128);
}

TEST(MSWindowsDesksTests, orderedInputCommandsWaitForDesktopCompletion)
{
    EXPECT_TRUE(MSWindowsDesks::deskCommandWaitsForCompletionForTest(
        MSWindowsDesks::kDeskInputKey));
    EXPECT_TRUE(MSWindowsDesks::deskCommandWaitsForCompletionForTest(
        MSWindowsDesks::kDeskInputButton));
    EXPECT_TRUE(MSWindowsDesks::deskCommandWaitsForCompletionForTest(
        MSWindowsDesks::kDeskInputWheel));
}

TEST(MSWindowsDesksTests, steadyMouseMotionDoesNotWaitForDesktopCompletion)
{
    EXPECT_FALSE(MSWindowsDesks::deskCommandWaitsForCompletionForTest(
        MSWindowsDesks::kDeskInputAbsoluteMove));
    EXPECT_FALSE(MSWindowsDesks::deskCommandWaitsForCompletionForTest(
        MSWindowsDesks::kDeskInputRelativeMove));
}

TEST(MSWindowsDesksTests, initialAndWarpPlacementWaitForDesktopCompletion)
{
    EXPECT_TRUE(MSWindowsDesks::deskCommandWaitsForCompletionForTest(
        MSWindowsDesks::kDeskInputAbsoluteMove, true));
}

TEST(MSWindowsDesksTests, normalDeskCommandTimeoutPreservesExistingBudget)
{
    EXPECT_DOUBLE_EQ(0.25,
        MSWindowsDesks::deskCommandAckTimeoutForTest(false, false));
    EXPECT_DOUBLE_EQ(0.75,
        MSWindowsDesks::deskCommandExecutionGraceForTest(false, false));
    EXPECT_DOUBLE_EQ(1.0,
        MSWindowsDesks::deskCommandAckTimeoutForTest(false, false) +
        MSWindowsDesks::deskCommandExecutionGraceForTest(false, false));
}

TEST(MSWindowsDesksTests, lowLatencyDeskCommandTimeoutCapsExecutingCommandBudget)
{
    EXPECT_DOUBLE_EQ(0.05,
        MSWindowsDesks::deskCommandAckTimeoutForTest(true, false));
    EXPECT_DOUBLE_EQ(0.20,
        MSWindowsDesks::deskCommandExecutionGraceForTest(true, false));
    EXPECT_LE(
        MSWindowsDesks::deskCommandAckTimeoutForTest(true, false) +
        MSWindowsDesks::deskCommandExecutionGraceForTest(true, false),
        0.25);
}

TEST(MSWindowsDesksTests, nestedRemoteModeUsesLowLatencyDeskCommandBudget)
{
    EXPECT_DOUBLE_EQ(
        MSWindowsDesks::deskCommandAckTimeoutForTest(true, false),
        MSWindowsDesks::deskCommandAckTimeoutForTest(false, true));
    EXPECT_DOUBLE_EQ(
        MSWindowsDesks::deskCommandExecutionGraceForTest(true, false),
        MSWindowsDesks::deskCommandExecutionGraceForTest(false, true));
}

TEST(MSWindowsDesksTests, negativeCommandWaitTimeoutCannotBecomeInfinite)
{
    EXPECT_DOUBLE_EQ(0.0,
        MSWindowsDesks::boundedDeskCommandWaitTimeoutForTest(-1.0));
    EXPECT_DOUBLE_EQ(0.0,
        MSWindowsDesks::boundedDeskCommandWaitTimeoutForTest(0.0));
    EXPECT_DOUBLE_EQ(0.25,
        MSWindowsDesks::boundedDeskCommandWaitTimeoutForTest(0.25));
}

TEST(MSWindowsDesksTests, desktopActivationWaitsForAllStartupCapabilities)
{
    EXPECT_FALSE(MSWindowsDesks::canActivateDesktopForTest(
        false, true, true, true));
    EXPECT_FALSE(MSWindowsDesks::canActivateDesktopForTest(
        true, false, true, true));
    EXPECT_FALSE(MSWindowsDesks::canActivateDesktopForTest(
        true, true, false, true));
    EXPECT_FALSE(MSWindowsDesks::canActivateDesktopForTest(
        true, true, true, false));
    EXPECT_TRUE(MSWindowsDesks::canActivateDesktopForTest(
        true, true, true, true));
}

TEST(MSWindowsDesksTests, pendingObservedDesktopRejectsNewInputLease)
{
    EXPECT_FALSE(MSWindowsDesks::canAcceptInputForActiveDesktopForTest(
        true, false));
    EXPECT_TRUE(MSWindowsDesks::canAcceptInputForActiveDesktopForTest(
        true, true));
}

TEST(MSWindowsDesksTests, desktopStartupDeadlineIsNonBlockingAndCompletionAware)
{
    EXPECT_FALSE(MSWindowsDesks::hasDesktopStartupTimedOutForTest(
        false, 1999, 2000));
    EXPECT_TRUE(MSWindowsDesks::hasDesktopStartupTimedOutForTest(
        false, 2000, 2000));
    EXPECT_FALSE(MSWindowsDesks::hasDesktopStartupTimedOutForTest(
        true, 3000, 2000));
}

TEST(MSWindowsDesksTests, shutdownNeverPostsQuitToUnstartedDeskThread)
{
    EXPECT_FALSE(MSWindowsDesks::shouldPostDeskQuitForTest(false, 0));
    EXPECT_FALSE(MSWindowsDesks::shouldPostDeskQuitForTest(false, 42));
    EXPECT_FALSE(MSWindowsDesks::shouldPostDeskQuitForTest(true, 0));
    EXPECT_TRUE(MSWindowsDesks::shouldPostDeskQuitForTest(true, 42));
}

TEST(MSWindowsDesksTests, highRateAbsoluteMotionPreservesTrajectorySamples)
{
    SInt32 pendingX = 100;
    SInt32 pendingY = 200;
    int postedCommands = 1;

    // Model a half-second desktop-thread pause at the 240 Hz server rate.
    // Every absolute point must remain a separate queue entry; replacing the
    // pending point here is the visible jump regression this test locks down.
    for (SInt32 sample = 1; sample <= 120; ++sample) {
        if (!MSWindowsDesks::coalesceMouseMotionForTest(
                MSWindowsDesks::kDeskInputAbsoluteMove,
                pendingX, pendingY,
                MSWindowsDesks::kDeskInputAbsoluteMove,
                100 + sample, 200 + sample)) {
            ++postedCommands;
        }
        EXPECT_EQ(100, pendingX);
        EXPECT_EQ(200, pendingY);
    }

    EXPECT_EQ(121, postedCommands);
    EXPECT_LE(postedCommands,
        static_cast<int>(MSWindowsDesks::maxPendingDeskCommandsForTest()));
}

TEST(MSWindowsDesksTests, highRateRelativeMotionPreservesAccumulatedDistance)
{
    SInt32 dx = 0;
    SInt32 dy = 0;
    int postedCommands = 1;
    for (int i = 0; i < 10000; ++i) {
        if (!MSWindowsDesks::coalesceMouseMotionForTest(
                MSWindowsDesks::kDeskInputRelativeMove,
                dx, dy,
                MSWindowsDesks::kDeskInputRelativeMove,
                1, -1)) {
            ++postedCommands;
        }
    }

    EXPECT_EQ(1, postedCommands);
    EXPECT_EQ(10000, dx);
    EXPECT_EQ(-10000, dy);
}

TEST(MSWindowsDesksTests, absoluteRelativeModeChangeSealsMotionBatch)
{
    SInt32 first = 320;
    SInt32 second = 240;
    EXPECT_FALSE(MSWindowsDesks::coalesceMouseMotionForTest(
        MSWindowsDesks::kDeskInputAbsoluteMove,
        first, second,
        MSWindowsDesks::kDeskInputRelativeMove,
        5, -5));
    EXPECT_EQ(320, first);
    EXPECT_EQ(240, second);
}

TEST(MSWindowsDesksTests, orderedAndControlCommandsBoundPendingMotion)
{
    EXPECT_EQ(MSWindowsDesks::kNoPendingMotionBoundary,
        MSWindowsDesks::pendingMotionBoundaryForTest(
            MSWindowsDesks::kDeskInputAbsoluteMove));
    EXPECT_EQ(MSWindowsDesks::kFlushPendingMotion,
        MSWindowsDesks::pendingMotionBoundaryForTest(
            MSWindowsDesks::kDeskInputButton));
    EXPECT_EQ(MSWindowsDesks::kFlushPendingMotion,
        MSWindowsDesks::pendingMotionBoundaryForTest(
            MSWindowsDesks::kDeskInputWheel));
    EXPECT_EQ(MSWindowsDesks::kSupersedePendingMotion,
        MSWindowsDesks::pendingMotionBoundaryForTest(
            MSWindowsDesks::kDeskControlLeave));
    EXPECT_EQ(MSWindowsDesks::kSupersedePendingMotion,
        MSWindowsDesks::pendingMotionBoundaryForTest(
            MSWindowsDesks::kDeskControlSwitch));
    EXPECT_EQ(MSWindowsDesks::kSupersedePendingMotion,
        MSWindowsDesks::pendingMotionBoundaryForTest(
            MSWindowsDesks::kDeskInputAbsoluteMove, true));
}

TEST(MSWindowsDesksTests, desktopChangeDuringLeaseRecoversWithoutStartupDelay)
{
    EXPECT_EQ(MSWindowsDesks::kRecoverInputProcess,
        MSWindowsDesks::desktopTransitionActionForTest(
            false, false, true, false, 100, 2000));
}

TEST(MSWindowsDesksTests, desktopChangeWithoutLeaseCanWaitForHelper)
{
    EXPECT_EQ(MSWindowsDesks::kWaitForObservedDesktop,
        MSWindowsDesks::desktopTransitionActionForTest(
            false, false, false, false, 100, 2000));
    EXPECT_EQ(MSWindowsDesks::kRecoverInputProcess,
        MSWindowsDesks::desktopTransitionActionForTest(
            false, false, false, false, 2000, 2000));
}

TEST(MSWindowsDesksTests, readyObservedDesktopUsesSafeHandoff)
{
    EXPECT_EQ(MSWindowsDesks::kActivateObservedDesktop,
        MSWindowsDesks::desktopTransitionActionForTest(
            false, true, true, true, 100, 2000));
    EXPECT_EQ(MSWindowsDesks::kKeepActiveDesktop,
        MSWindowsDesks::desktopTransitionActionForTest(
            true, false, true, false, 3000, 2000));
}

TEST(MSWindowsDesksTests, inputRecoveryAndHardExitFallbackAreOneShotAndBounded)
{
    bool recoveryRequested = false;
    EXPECT_TRUE(MSWindowsDesks::beginInputRecoveryForTest(
        recoveryRequested));
    EXPECT_TRUE(recoveryRequested);
    EXPECT_FALSE(MSWindowsDesks::beginInputRecoveryForTest(
        recoveryRequested));
    EXPECT_GT(MSWindowsDesks::inputRecoveryHardExitDelayForTest(), 0u);
    EXPECT_LE(MSWindowsDesks::inputRecoveryHardExitDelayForTest(), 5000u);
}
