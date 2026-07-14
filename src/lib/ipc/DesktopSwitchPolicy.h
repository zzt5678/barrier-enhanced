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

std::string launchDesktopName(
    const std::string& observedDesktopName,
    bool daemonized);

RelaunchDecision observeDesktop(
    RelaunchState& state,
    const std::string& lastDesktopName,
    const std::string& observedDesktopName,
    double now,
    double settleSeconds,
    double debounceSeconds);

} // namespace DesktopSwitchPolicy
