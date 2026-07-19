/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ipc/ElevationPolicy.h"
#include "ipc/DesktopSwitchPolicy.h"

#include "ipc/IpcMessage.h"

#include "test/global/gtest.h"

TEST(ElevationPolicyTests, normalizeModeFallsBackToAsNeededForInvalidValues)
{
    EXPECT_EQ(IpcCommandMessage::kElevateAsNeeded,
              ElevationPolicy::normalizeMode(-1));
    EXPECT_EQ(IpcCommandMessage::kElevateAsNeeded,
              ElevationPolicy::normalizeMode(99));
}

TEST(ElevationPolicyTests, explicitProcessElevationOnlyUsesAlways)
{
    EXPECT_FALSE(ElevationPolicy::shouldElevateProcess(
        IpcCommandMessage::kElevateAsNeeded));
    EXPECT_TRUE(ElevationPolicy::shouldElevateProcess(
        IpcCommandMessage::kElevateAlways));
    EXPECT_FALSE(ElevationPolicy::shouldElevateProcess(
        IpcCommandMessage::kElevateNever));
    EXPECT_FALSE(ElevationPolicy::shouldElevateProcess(99));
}

TEST(ElevationPolicyTests, autoElevationFollowsDesktopAndNeverMode)
{
    EXPECT_FALSE(ElevationPolicy::shouldAutoElevate(
        IpcCommandMessage::kElevateAsNeeded, "Default"));
    EXPECT_FALSE(ElevationPolicy::shouldAutoElevate(
        IpcCommandMessage::kElevateAsNeeded, ""));
    EXPECT_TRUE(ElevationPolicy::shouldAutoElevate(
        IpcCommandMessage::kElevateAsNeeded, "Winlogon"));
    EXPECT_TRUE(ElevationPolicy::shouldAutoElevate(
        IpcCommandMessage::kElevateAlways, "Winlogon"));
    EXPECT_FALSE(ElevationPolicy::shouldAutoElevate(
        IpcCommandMessage::kElevateNever, "Winlogon"));
    EXPECT_TRUE(ElevationPolicy::shouldAutoElevate(99, "Winlogon"));
}

TEST(ElevationPolicyTests, onlyAsNeededModeRelaunchesOnDesktopSwitch)
{
    EXPECT_TRUE(ElevationPolicy::shouldRelaunchOnDesktopSwitch(
        IpcCommandMessage::kElevateAsNeeded));
    EXPECT_FALSE(ElevationPolicy::shouldRelaunchOnDesktopSwitch(
        IpcCommandMessage::kElevateAlways));
    EXPECT_FALSE(ElevationPolicy::shouldRelaunchOnDesktopSwitch(
        IpcCommandMessage::kElevateNever));
    EXPECT_TRUE(ElevationPolicy::shouldRelaunchOnDesktopSwitch(99));
}

TEST(ElevationPolicyTests, identicalNormalizedCommandDoesNotRelaunch)
{
    EXPECT_FALSE(ElevationPolicy::commandRequiresRelaunch(
        "weavec --ipc --name windows", IpcCommandMessage::kElevateAlways,
        "weavec --ipc --name windows", IpcCommandMessage::kElevateAlways));
}

TEST(ElevationPolicyTests, commandOrElevationChangeRequiresRelaunch)
{
    EXPECT_TRUE(ElevationPolicy::commandRequiresRelaunch(
        "weavec --ipc --name windows", IpcCommandMessage::kElevateAlways,
        "weavec --ipc --name windows-2", IpcCommandMessage::kElevateAlways));
    EXPECT_TRUE(ElevationPolicy::commandRequiresRelaunch(
        "weavec --ipc --name windows", IpcCommandMessage::kElevateAsNeeded,
        "weavec --ipc --name windows", IpcCommandMessage::kElevateAlways));
    EXPECT_FALSE(ElevationPolicy::commandRequiresRelaunch(
        "weavec --ipc --name windows", 99,
        "weavec --ipc --name windows", IpcCommandMessage::kElevateAsNeeded));
}

TEST(ElevationPolicyTests, modeSettingOverridesLegacyElevateFlag)
{
    EXPECT_EQ(IpcCommandMessage::kElevateNever,
              ElevationPolicy::modeFromSettings("2", "1"));
    EXPECT_EQ(IpcCommandMessage::kElevateNever,
              ElevationPolicy::modeFromSettings(" 2 ", "1"));
    EXPECT_EQ(IpcCommandMessage::kElevateAsNeeded,
              ElevationPolicy::modeFromSettings("99", "1"));
    EXPECT_EQ(IpcCommandMessage::kElevateAsNeeded,
              ElevationPolicy::modeFromSettings("abc", "1"));
    EXPECT_EQ(IpcCommandMessage::kElevateAsNeeded,
              ElevationPolicy::modeFromSettings("1x", "0"));
    EXPECT_EQ(IpcCommandMessage::kElevateAsNeeded,
              ElevationPolicy::modeFromSettings("2x", "1"));
    EXPECT_EQ(IpcCommandMessage::kElevateAsNeeded,
              ElevationPolicy::modeFromSettings("999999999999999999999", "1"));
}

TEST(ElevationPolicyTests, legacyElevateFlagMapsToAlwaysOrAsNeeded)
{
    EXPECT_EQ(IpcCommandMessage::kElevateAlways,
              ElevationPolicy::modeFromSettings("", "1"));
    EXPECT_EQ(IpcCommandMessage::kElevateAsNeeded,
              ElevationPolicy::modeFromSettings("", "0"));
    EXPECT_EQ(IpcCommandMessage::kElevateAsNeeded,
              ElevationPolicy::modeFromSettings("", ""));
}

TEST(DesktopSwitchPolicyTests, firstDesktopObservationIsRememberedWithoutRelaunch)
{
    DesktopSwitchPolicy::RelaunchState state;

    DesktopSwitchPolicy::RelaunchDecision decision =
        DesktopSwitchPolicy::observeDesktop(state, "", "Default", 10.0, 0.25, 2.0);

    EXPECT_FALSE(decision.relaunch);
    EXPECT_TRUE(decision.rememberDesktop);
    EXPECT_FALSE(decision.settling);
    EXPECT_FALSE(decision.debounced);
}

TEST(DesktopSwitchPolicyTests, observedDesktopIsUsedForProcessLaunch)
{
    EXPECT_EQ("Winlogon",
              DesktopSwitchPolicy::launchDesktopName("Winlogon", true));
}

TEST(DesktopSwitchPolicyTests, daemonLaunchFallsBackWhenInputDesktopIsUnavailable)
{
    const DesktopSwitchPolicy::LaunchTarget target =
        DesktopSwitchPolicy::resolveLaunchTarget("", true);

    EXPECT_EQ("Default", target.desktopName);
    EXPECT_FALSE(target.expectedDesktopKnown);
}

TEST(DesktopSwitchPolicyTests, foregroundLaunchDoesNotHideDesktopLookupFailure)
{
    const DesktopSwitchPolicy::LaunchTarget target =
        DesktopSwitchPolicy::resolveLaunchTarget("", false);

    EXPECT_TRUE(target.desktopName.empty());
    EXPECT_FALSE(target.expectedDesktopKnown);
}

TEST(DesktopSwitchPolicyTests, observedDesktopRemainsPartOfReadinessContract)
{
    const DesktopSwitchPolicy::LaunchTarget target =
        DesktopSwitchPolicy::resolveLaunchTarget("Winlogon", true);

    EXPECT_EQ("Winlogon", target.desktopName);
    EXPECT_TRUE(target.expectedDesktopKnown);
}

TEST(DesktopSwitchPolicyTests, transientDesktopChangeMustSettleBeforeRelaunch)
{
    DesktopSwitchPolicy::RelaunchState state;

    DesktopSwitchPolicy::RelaunchDecision decision =
        DesktopSwitchPolicy::observeDesktop(state, "Default", "Winlogon", 10.0, 0.25, 2.0);

    EXPECT_FALSE(decision.relaunch);
    EXPECT_FALSE(decision.rememberDesktop);
    EXPECT_TRUE(decision.settling);

    decision = DesktopSwitchPolicy::observeDesktop(state, "Default", "Default", 10.1, 0.25, 2.0);

    EXPECT_FALSE(decision.relaunch);
    EXPECT_FALSE(decision.settling);
}

TEST(DesktopSwitchPolicyTests, stableDesktopChangeRelaunchesAfterSettleWindow)
{
    DesktopSwitchPolicy::RelaunchState state;

    DesktopSwitchPolicy::RelaunchDecision decision =
        DesktopSwitchPolicy::observeDesktop(state, "Default", "Winlogon", 10.0, 0.25, 2.0);
    EXPECT_TRUE(decision.settling);

    decision = DesktopSwitchPolicy::observeDesktop(state, "Default", "Winlogon", 10.3, 0.25, 2.0);

    EXPECT_TRUE(decision.relaunch);
    EXPECT_TRUE(decision.rememberDesktop);
    EXPECT_FALSE(decision.settling);
    EXPECT_FALSE(decision.debounced);
}

TEST(DesktopSwitchPolicyTests, differentDesktopObservationResetsPendingCandidate)
{
    DesktopSwitchPolicy::RelaunchState state;

    DesktopSwitchPolicy::RelaunchDecision decision =
        DesktopSwitchPolicy::observeDesktop(state, "Default", "Winlogon", 10.0, 0.25, 2.0);
    EXPECT_TRUE(decision.settling);

    decision = DesktopSwitchPolicy::observeDesktop(state, "Default", "ScreenSaver", 10.3, 0.25, 2.0);

    EXPECT_FALSE(decision.relaunch);
    EXPECT_TRUE(decision.settling);

    decision = DesktopSwitchPolicy::observeDesktop(state, "Default", "ScreenSaver", 10.6, 0.25, 2.0);

    EXPECT_TRUE(decision.relaunch);
}

TEST(DesktopSwitchPolicyTests, debounceBlocksRapidSecondRelaunchUntilExpired)
{
    DesktopSwitchPolicy::RelaunchState state;

    DesktopSwitchPolicy::observeDesktop(state, "Default", "Winlogon", 10.0, 0.25, 2.0);
    DesktopSwitchPolicy::RelaunchDecision decision =
        DesktopSwitchPolicy::observeDesktop(state, "Default", "Winlogon", 10.3, 0.25, 2.0);
    EXPECT_TRUE(decision.relaunch);

    decision = DesktopSwitchPolicy::observeDesktop(state, "Winlogon", "Default", 10.4, 0.25, 2.0);
    EXPECT_TRUE(decision.settling);

    decision = DesktopSwitchPolicy::observeDesktop(state, "Winlogon", "Default", 10.7, 0.25, 2.0);
    EXPECT_FALSE(decision.relaunch);
    EXPECT_TRUE(decision.debounced);

    decision = DesktopSwitchPolicy::observeDesktop(state, "Winlogon", "Default", 12.4, 0.25, 2.0);
    EXPECT_TRUE(decision.relaunch);
}
