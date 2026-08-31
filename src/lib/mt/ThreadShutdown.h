/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#pragma once

#include <functional>

namespace barrier {

using FinalThreadWaiter = std::function<bool(double)>;
using FinalProcessTerminator = std::function<void()>;

const double kFinalThreadShutdownDeadlineSeconds = 2.0;

void terminateProcessForFinalThreadShutdown();

void waitForFinalThreadShutdown(
    const char* context,
    double timeoutSeconds,
    const FinalThreadWaiter& waiter,
    const FinalProcessTerminator& terminator =
        terminateProcessForFinalThreadShutdown);

}
