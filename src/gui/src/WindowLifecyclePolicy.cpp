/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "WindowLifecyclePolicy.h"

namespace WindowLifecyclePolicy {

CloseAction closeAction(bool explicitQuitRequested, bool trayAvailable)
{
    if (explicitQuitRequested) {
        return CloseAction::Quit;
    }
    return trayAvailable ? CloseAction::Hide : CloseAction::Minimize;
}

bool shouldHideOnMinimize(Qt::WindowStates state, bool minimizeToTray,
                          bool trayAvailable)
{
    return minimizeToTray && trayAvailable &&
        state.testFlag(Qt::WindowMinimized);
}

bool canCompleteExplicitQuit(bool serviceMode, bool stopAcknowledged)
{
    return !serviceMode || stopAcknowledged;
}

} // namespace WindowLifecyclePolicy
