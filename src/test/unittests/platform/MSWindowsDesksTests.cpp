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
}

TEST(MSWindowsDesksTests, cancelledCommandCompletionDoesNotRestoreReadiness)
{
    EXPECT_FALSE(MSWindowsDesks::commandCompletionProvesResponsiveForTest(
        false, 20, 0));
    EXPECT_FALSE(MSWindowsDesks::commandCompletionProvesResponsiveForTest(
        true, 20, 20));
    EXPECT_TRUE(MSWindowsDesks::commandCompletionProvesResponsiveForTest(
        true, 21, 20));
}
