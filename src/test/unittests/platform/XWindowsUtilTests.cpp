/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 */

#include "test/global/gtest.h"
#include "platform/XWindowsUtil.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {

int
baselineXErrorHandler(Display*, XErrorEvent*)
{
    return 0;
}

void
markXErrorHandler(Display*, XErrorEvent*, void* data)
{
    static_cast<std::atomic<bool>*>(data)->store(true);
}

int
runOverlappingErrorLocksTest()
{
    alarm(10);
    if (XInitThreads() == 0) {
        return 71;
    }
    XErrorHandler const original = XSetErrorHandler(&baselineXErrorHandler);

    std::mutex mutex;
    std::condition_variable wake;
    bool firstAcquired = false;
    bool secondAttempted = false;
    bool secondAcquired = false;
    bool releaseFirst = false;
    bool releaseSecond = false;
    bool startSecond = false;

    std::thread first([&]() {
        XWindowsUtil::ErrorLock lock(NULL);
        std::unique_lock<std::mutex> stateLock(mutex);
        firstAcquired = true;
        wake.notify_all();
        wake.wait(stateLock, [&]() { return releaseFirst; });
    });

    {
        std::unique_lock<std::mutex> stateLock(mutex);
        wake.wait(stateLock, [&]() { return firstAcquired; });
    }

    std::thread second([&]() {
        {
            std::unique_lock<std::mutex> stateLock(mutex);
            secondAttempted = true;
            wake.notify_all();
            wake.wait(stateLock, [&]() { return startSecond; });
        }
        XWindowsUtil::ErrorLock lock(NULL);
        std::unique_lock<std::mutex> stateLock(mutex);
        secondAcquired = true;
        wake.notify_all();
        wake.wait(stateLock, [&]() { return releaseSecond; });
    });

    bool overlapped = false;
    {
        std::unique_lock<std::mutex> stateLock(mutex);
        wake.wait(stateLock, [&]() { return secondAttempted; });
        startSecond = true;
        wake.notify_all();
        overlapped = wake.wait_for(stateLock, std::chrono::seconds(1),
                                   [&]() { return secondAcquired; });
        releaseFirst = true;
        wake.notify_all();
    }
    first.join();

    {
        std::unique_lock<std::mutex> stateLock(mutex);
        wake.wait(stateLock, [&]() { return secondAcquired; });
        releaseSecond = true;
        wake.notify_all();
    }
    second.join();

    XErrorHandler const restored = XSetErrorHandler(original);
    if (overlapped) {
        return 72;
    }
    return restored == &baselineXErrorHandler ? 0 : 73;
}

int
runCrossThreadHandlerTest()
{
    alarm(10);
    if (XInitThreads() == 0) {
        return 76;
    }
    XErrorHandler const original = XSetErrorHandler(&baselineXErrorHandler);

    std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> foreignContextCalled(false);
    bool lockAcquired = false;
    bool releaseLock = false;

    std::thread owner([&]() {
        XWindowsUtil::ErrorLock lock(
            NULL, &markXErrorHandler, &foreignContextCalled);
        std::unique_lock<std::mutex> stateLock(mutex);
        lockAcquired = true;
        wake.notify_all();
        wake.wait(stateLock, [&]() { return releaseLock; });
    });

    {
        std::unique_lock<std::mutex> stateLock(mutex);
        wake.wait(stateLock, [&]() { return lockAcquired; });
    }

    XErrorHandler const installed =
        XSetErrorHandler(&baselineXErrorHandler);
    XSetErrorHandler(installed);
    XErrorEvent event = {};
    installed(NULL, &event);

    {
        std::lock_guard<std::mutex> stateLock(mutex);
        releaseLock = true;
        wake.notify_all();
    }
    owner.join();

    XErrorHandler const restored = XSetErrorHandler(original);
    if (foreignContextCalled.load()) {
        return 77;
    }
    return restored == &baselineXErrorHandler ? 0 : 78;
}

int
runNestedErrorLocksTest()
{
    alarm(10);
    if (XInitThreads() == 0) {
        return 74;
    }
    XErrorHandler const original = XSetErrorHandler(&baselineXErrorHandler);
    {
        XWindowsUtil::ErrorLock outer(NULL);
        {
            XWindowsUtil::ErrorLock inner(NULL);
        }
    }

    XErrorHandler const restored = XSetErrorHandler(original);
    return restored == &baselineXErrorHandler ? 0 : 75;
}

}

TEST(XWindowsUtilTests, errorLockAllowsNestedUseOnOneThread)
{
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
    EXPECT_EXIT(
        std::_Exit(runNestedErrorLocksTest()),
        ::testing::ExitedWithCode(0),
        "");
}

TEST(XWindowsUtilTests, errorLockSerializesProcessGlobalHandlerAcrossThreads)
{
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
    EXPECT_EXIT(
        std::_Exit(runOverlappingErrorLocksTest()),
        ::testing::ExitedWithCode(0),
        "");
}

TEST(XWindowsUtilTests, errorLockDoesNotRouteForeignThreadErrorsToOwner)
{
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
    EXPECT_EXIT(
        std::_Exit(runCrossThreadHandlerTest()),
        ::testing::ExitedWithCode(0),
        "");
}
