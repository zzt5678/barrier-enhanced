/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "arch/Arch.h"
#include "base/Stopwatch.h"
#include "mt/Thread.h"

#include "test/global/gtest.h"

#include <atomic>
#include <cerrno>

#if defined(__linux__)

#include <dirent.h>

#elif SYSAPI_WIN32

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#endif

#if defined(__linux__) || SYSAPI_WIN32

namespace {

size_t
networkResourceCount()
{
    size_t count = 0;
#if defined(__linux__)
    DIR* directory = opendir("/proc/self/fd");
    if (directory != nullptr) {
        while (readdir(directory) != nullptr) {
            ++count;
        }
        closedir(directory);
    }
#else
    DWORD handleCount = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handleCount)) {
        count = handleCount;
    }
#endif

    EXPECT_GT(count, 0u) << "failed to query process network resources";
    return count;
}

ArchSocket
newIdleListeningSocket()
{
    ArchSocket socket = ARCH->newSocket(
        IArchNetwork::kINET, IArchNetwork::kSTREAM);
    ArchNetAddress address = NULL;
    try {
        address = ARCH->newAnyAddr(IArchNetwork::kINET);
        ARCH->setAddrPort(address, 0);
        ARCH->bindSocket(socket, address);
        ARCH->listenOnSocket(socket);
        ARCH->closeAddr(address);
        return socket;
    }
    catch (...) {
        if (address != NULL) {
            ARCH->closeAddr(address);
        }
        ARCH->closeSocket(socket);
        throw;
    }
}

}

#endif

#if defined(__linux__)

TEST(ArchNetworkTests, unblockBeforePollingDoesNotAllocateFileDescriptors)
{
    const size_t descriptorsBefore = networkResourceCount();

    for (int i = 0; i < 16; ++i) {
        Thread thread([]() { });
        thread.unblockPollSocket();
        ASSERT_TRUE(thread.wait(5.0));
    }

    EXPECT_EQ(descriptorsBefore, networkResourceCount());
}

TEST(ArchNetworkTests, denseUnblocksAreFullyDrainedByOnePoll)
{
    std::atomic<bool> pipeReady(false);
    std::atomic<bool> unblocksQueued(false);
    double secondPollDuration = 0.0;

    Thread thread([&]() {
        ArchSocket socket = newIdleListeningSocket();
        try {
            IArchNetwork::PollEntry entry;
            entry.m_socket = socket;
            entry.m_events = IArchNetwork::kPOLLIN;
            entry.m_revents = 0;

            ARCH->pollSocket(&entry, 1, 0.0);
            pipeReady.store(true, std::memory_order_release);
            while (!unblocksQueued.load(std::memory_order_acquire)) {
            }

            errno = EAGAIN;
            ARCH->pollSocket(&entry, 1, 0.1);
            entry.m_revents = 0;
            Stopwatch timer;
            ARCH->pollSocket(&entry, 1, 0.05);
            secondPollDuration = timer.getTime();
            ARCH->closeSocket(socket);
        }
        catch (...) {
            ARCH->closeSocket(socket);
            throw;
        }
    });

    while (!pipeReady.load(std::memory_order_acquire)) {
        ARCH->sleep(0.001);
    }
    for (int i = 0; i < 256; ++i) {
        thread.unblockPollSocket();
    }
    unblocksQueued.store(true, std::memory_order_release);

    ASSERT_TRUE(thread.wait(3.0));
    EXPECT_GE(secondPollDuration, 0.02);
}

#endif

#if defined(__linux__) || SYSAPI_WIN32

TEST(ArchNetworkTests, unblockBeforeFirstPollIsReplayed)
{
    std::atomic<bool> readyForUnblock(false);
    std::atomic<bool> allowPoll(false);
    double pollDuration = 0.0;

    Thread thread([&]() {
        ArchSocket socket = newIdleListeningSocket();
        try {
            IArchNetwork::PollEntry entry;
            entry.m_socket = socket;
            entry.m_events = IArchNetwork::kPOLLIN;
            entry.m_revents = 0;

            readyForUnblock.store(true, std::memory_order_release);
            while (!allowPoll.load(std::memory_order_acquire)) {
                ARCH->sleep(0.001);
            }

            Stopwatch timer;
            ARCH->pollSocket(&entry, 1, 2.0);
            pollDuration = timer.getTime();
            ARCH->closeSocket(socket);
        }
        catch (...) {
            ARCH->closeSocket(socket);
            throw;
        }
    });

    while (!readyForUnblock.load(std::memory_order_acquire)) {
        ARCH->sleep(0.001);
    }

    thread.unblockPollSocket();
    allowPoll.store(true, std::memory_order_release);

    ASSERT_TRUE(thread.wait(3.0));
    EXPECT_LT(pollDuration, 0.5);
}

TEST(ArchNetworkTests, pollUnblockResourceIsReleasedWithThread)
{
    const auto exercisePollUnblock = [](int iterations) {
        for (int iteration = 0; iteration < iterations; ++iteration) {
            std::atomic<bool> pollStarted(false);
            std::atomic<bool> pollReturned(false);
            double pollDuration = 0.0;

            Thread thread([&]() {
                ArchSocket socket = newIdleListeningSocket();
                try {
                    IArchNetwork::PollEntry entry;
                    entry.m_socket = socket;
                    entry.m_events = IArchNetwork::kPOLLIN;
                    entry.m_revents = 0;

                    pollStarted.store(true, std::memory_order_release);
                    Stopwatch timer;
                    ARCH->pollSocket(&entry, 1, 2.0);
                    pollDuration = timer.getTime();
                    ARCH->closeSocket(socket);
                    pollReturned.store(true, std::memory_order_release);
                }
                catch (...) {
                    ARCH->closeSocket(socket);
                    throw;
                }
            });

            while (!pollStarted.load(std::memory_order_acquire)) {
                ARCH->sleep(0.001);
            }

            const double unblockDeadline = ARCH->time() + 1.0;
            while (!pollReturned.load(std::memory_order_acquire) &&
                   ARCH->time() < unblockDeadline) {
                thread.unblockPollSocket();
                ARCH->sleep(0.005);
            }

            const bool finished = thread.wait(3.0);
            if (!finished) {
                thread.cancel();
                thread.unblockPollSocket();
                thread.wait();
            }
            EXPECT_TRUE(finished);
            EXPECT_TRUE(pollReturned.load(std::memory_order_acquire));
            EXPECT_LT(pollDuration, 1.0);
        }
    };

    // Winsock and the C runtime may retain one process-lifetime handle when
    // this path is first exercised. Measure repeated calls after that setup.
    exercisePollUnblock(1);

    const size_t resourcesBefore = networkResourceCount();
    exercisePollUnblock(32);

    const double cleanupDeadline = ARCH->time() + 1.0;
    while (networkResourceCount() != resourcesBefore &&
           ARCH->time() < cleanupDeadline) {
        ARCH->sleep(0.005);
    }
    EXPECT_EQ(resourcesBefore, networkResourceCount());
}

#endif
