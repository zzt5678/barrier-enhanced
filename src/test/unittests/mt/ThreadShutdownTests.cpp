/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#include "mt/ThreadShutdown.h"

#include "base/ILogOutputter.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>

namespace {

class ExitOnWriteLogOutputter : public ILogOutputter {
public:
    void open(const char*) override
    {
    }

    void close() override
    {
    }

    void show(bool) override
    {
    }

    bool write(ELevel, const char*) override
    {
        std::_Exit(74);
    }
};

}

TEST(ThreadShutdownTests, completedWaitReturnsWithoutTerminating)
{
    double observedTimeout = -1.0;
    int terminateCalls = 0;

    barrier::waitForFinalThreadShutdown(
        "completed worker",
        1.25,
        [&observedTimeout](double timeout) {
            observedTimeout = timeout;
            return true;
        },
        [&terminateCalls]() { ++terminateCalls; });

    EXPECT_DOUBLE_EQ(1.25, observedTimeout);
    EXPECT_EQ(0, terminateCalls);
}

TEST(ThreadShutdownTests, negativeTimeoutNeverReachesWaiterAsInfinite)
{
    double observedTimeout = -1.0;
    int terminateCalls = 0;

    barrier::waitForFinalThreadShutdown(
        "completed worker",
        -5.0,
        [&observedTimeout](double timeout) {
            observedTimeout = timeout;
            return true;
        },
        [&terminateCalls]() { ++terminateCalls; });

    EXPECT_DOUBLE_EQ(0.0, observedTimeout);
    EXPECT_EQ(0, terminateCalls);
}

TEST(ThreadShutdownTests, timeoutInvokesInjectedTerminator)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        barrier::waitForFinalThreadShutdown(
            "stuck worker",
            0.0,
            [](double) { return false; },
            []() { std::_Exit(73); }),
        ::testing::ExitedWithCode(73),
        "");
}

TEST(ThreadShutdownTests, timeoutBypassesGlobalLoggingBeforeTerminator)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            CLOG->insert(new ExitOnWriteLogOutputter, true);
            barrier::waitForFinalThreadShutdown(
                "stuck worker",
                0.0,
                [](double) { return false; },
                []() { std::_Exit(73); });
        },
        ::testing::ExitedWithCode(73),
        "");
}

TEST(ThreadShutdownTests, returningTerminatorStillFailsFast)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        barrier::waitForFinalThreadShutdown(
            "returning terminator",
            0.0,
            [](double) { return false; },
            []() {
                std::fputs("terminator returned unexpectedly\n", stderr);
                std::fflush(stderr);
            }),
        ::testing::ExitedWithCode(EXIT_FAILURE),
        "terminator returned unexpectedly");
}
