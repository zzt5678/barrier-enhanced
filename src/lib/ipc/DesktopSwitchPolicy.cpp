/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ipc/DesktopSwitchPolicy.h"

namespace DesktopSwitchPolicy {

RelaunchState::RelaunchState() :
    pendingSince(0.0),
    lastRelaunchTime(0.0)
{
}

RelaunchDecision::RelaunchDecision() :
    relaunch(false),
    rememberDesktop(false),
    settling(false),
    debounced(false)
{
}

std::string
launchDesktopName(const std::string& observedDesktopName, bool daemonized)
{
    if (!observedDesktopName.empty()) {
        return observedDesktopName;
    }
    return daemonized ? "Default" : std::string();
}

LaunchTarget
resolveLaunchTarget(
    const std::string& observedDesktopName,
    const std::string& readinessDesktopEvidence,
    bool daemonized)
{
    LaunchTarget target;
    target.purpose = LaunchPurpose::Exact;
    if (!observedDesktopName.empty()) {
        target.desktopName = observedDesktopName;
        target.expectedDesktopKnown = true;
    }
    else if (!daemonized) {
        target.desktopName.clear();
        target.expectedDesktopKnown = false;
    }
    else if (!readinessDesktopEvidence.empty()) {
        target.desktopName = readinessDesktopEvidence;
        target.expectedDesktopKnown = true;
    }
    else {
        target.desktopName = launchDesktopName(std::string(), true);
        target.expectedDesktopKnown = false;
        target.purpose = LaunchPurpose::Discovery;
    }
    return target;
}

DesktopRetargetDecision
decideDesktopRetarget(
    LaunchPurpose launchPurpose,
    bool readinessDesktopMismatch,
    bool retargetAlreadyAttempted)
{
    const bool needsRetarget = launchPurpose == LaunchPurpose::Discovery ||
        readinessDesktopMismatch;
    if (!needsRetarget) {
        return DesktopRetargetDecision::Keep;
    }
    return retargetAlreadyAttempted
        ? DesktopRetargetDecision::Backoff
        : DesktopRetargetDecision::Retarget;
}

bool
shouldReturnDesktopMismatch(ReadinessPhase)
{
    return true;
}

bool
canAdoptRetargetedActiveDesktop(
    const std::string& expectedDesktopName,
    const std::string& reportedDesktopName,
    const std::string& observedDesktopName)
{
    return !expectedDesktopName.empty() &&
        !reportedDesktopName.empty() &&
        reportedDesktopName != expectedDesktopName &&
        reportedDesktopName == observedDesktopName;
}

bool
shouldRetryFailedActivationAfterDesktopRetarget(
    const std::string& expectedDesktopName,
    const std::string& observedDesktopName)
{
    return !expectedDesktopName.empty() &&
        !observedDesktopName.empty() &&
        expectedDesktopName != observedDesktopName;
}

RelaunchDecision
observeDesktop(
    RelaunchState& state,
    const std::string& lastDesktopName,
    const std::string& observedDesktopName,
    double now,
    double settleSeconds,
    double debounceSeconds)
{
    RelaunchDecision decision;
    if (observedDesktopName.empty()) {
        return decision;
    }

    if (lastDesktopName.empty()) {
        state.pendingDesktopName.clear();
        state.pendingSince = 0.0;
        decision.rememberDesktop = true;
        return decision;
    }

    if (observedDesktopName == lastDesktopName) {
        state.pendingDesktopName.clear();
        state.pendingSince = 0.0;
        return decision;
    }

    if (state.pendingDesktopName != observedDesktopName) {
        state.pendingDesktopName = observedDesktopName;
        state.pendingSince = now;
        decision.settling = true;
        return decision;
    }

    if (now - state.pendingSince < settleSeconds) {
        decision.settling = true;
        return decision;
    }

    if (state.lastRelaunchTime != 0.0 &&
        now - state.lastRelaunchTime < debounceSeconds) {
        decision.debounced = true;
        return decision;
    }

    state.pendingDesktopName.clear();
    state.pendingSince = 0.0;
    state.lastRelaunchTime = now;
    decision.relaunch = true;
    decision.rememberDesktop = true;
    return decision;
}

} // namespace DesktopSwitchPolicy
