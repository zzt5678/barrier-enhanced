/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <Qt>

namespace WindowLifecyclePolicy {

enum class CloseAction {
    Hide,
    Minimize,
    Quit
};

CloseAction closeAction(bool explicitQuitRequested, bool trayAvailable);
bool shouldHideOnMinimize(Qt::WindowStates state, bool minimizeToTray,
                          bool trayAvailable);
bool canCompleteExplicitQuit(bool serviceMode, bool stopAcknowledged);

} // namespace WindowLifecyclePolicy
