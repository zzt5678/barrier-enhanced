/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#include "mt/ThreadShutdown.h"

#include <cstdlib>

namespace barrier {

void
terminateProcessForFinalThreadShutdown()
{
    std::_Exit(EXIT_FAILURE);
}

void
waitForFinalThreadShutdown(
    const char*,
    double timeoutSeconds,
    const FinalThreadWaiter& waiter,
    const FinalProcessTerminator& terminator)
{
    const double boundedTimeout = timeoutSeconds >= 0.0 ? timeoutSeconds : 0.0;
    if (waiter && waiter(boundedTimeout)) {
        return;
    }

    if (terminator) {
        terminator();
    }

    // A production terminator must not return, but keep teardown fail-closed
    // if an injected or platform terminator violates that contract.
    std::_Exit(EXIT_FAILURE);
}

}
