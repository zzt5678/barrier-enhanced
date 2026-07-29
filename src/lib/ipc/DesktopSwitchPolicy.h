/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <string>

namespace DesktopSwitchPolicy {

struct RelaunchState {
    RelaunchState();

    std::string pendingDesktopName;
    double pendingSince;
    double lastRelaunchTime;
};

struct RelaunchDecision {
    RelaunchDecision();

    bool relaunch;
    bool rememberDesktop;
    bool settling;
    bool debounced;
};

enum class LaunchPurpose {
    Exact,
    Discovery,
};

enum class DesktopRetargetDecision {
    Keep,
    Retarget,
    Backoff,
};

enum class ReadinessPhase {
    Standby,
    Active,
};

struct LaunchTarget {
    std::string desktopName;
    bool expectedDesktopKnown;
    LaunchPurpose purpose;
};

std::string launchDesktopName(
    const std::string& observedDesktopName,
    bool daemonized);

LaunchTarget resolveLaunchTarget(
    const std::string& observedDesktopName,
    const std::string& readinessDesktopEvidence,
    bool daemonized);

DesktopRetargetDecision decideDesktopRetarget(
    LaunchPurpose launchPurpose,
    bool readinessDesktopMismatch,
    bool retargetAlreadyAttempted);

bool shouldReturnDesktopMismatch(ReadinessPhase phase);

bool canAdoptRetargetedActiveDesktop(
    const std::string& expectedDesktopName,
    const std::string& reportedDesktopName,
    const std::string& observedDesktopName);

RelaunchDecision observeDesktop(
    RelaunchState& state,
    const std::string& lastDesktopName,
    const std::string& observedDesktopName,
    double now,
    double settleSeconds,
    double debounceSeconds);

} // namespace DesktopSwitchPolicy
