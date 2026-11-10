#include "platform/OSXInputLoop.h"
#include "test/global/gtest.h"
#include <future>
#include <atomic>
#include <stdexcept>
#include <vector>

TEST(OSXInputLoopTests, nativeCommandsRunOnOneDedicatedOwner)
{
    OSXInputLoop loop;
    const auto caller = std::this_thread::get_id();
    std::thread::id first, second;
    ASSERT_TRUE(loop.invoke([&] { first = std::this_thread::get_id(); }));
    ASSERT_TRUE(loop.invoke([&] {
        second = std::this_thread::get_id();
        EXPECT_TRUE(loop.isOwner());
        EXPECT_TRUE(loop.invoke([] {})); // no self-deadlock
    }));
    EXPECT_NE(caller, first);
    EXPECT_EQ(first, second);
}

TEST(OSXInputLoopTests, expiredQueuedCaptureNeverExecutesAfterOwnerRecovers)
{
    OSXInputLoop loop;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto blocker = std::async(std::launch::async, [&] {
        return loop.invoke([&] { entered.set_value(); gate.wait(); });
    });
    entered.get_future().wait();
    std::atomic<int> captures{0};
    EXPECT_FALSE(loop.invoke([&] { ++captures; }, std::chrono::milliseconds(20)));
    release.set_value();
    EXPECT_TRUE(blocker.get());
    ASSERT_TRUE(loop.invoke([] {}));
    EXPECT_EQ(0, captures.load());
}

TEST(OSXInputLoopTests, throwingCommandDoesNotKillNativeOwner)
{
    OSXInputLoop loop;
    EXPECT_THROW(loop.invoke([] { throw std::runtime_error("command failed"); }), std::runtime_error);
    EXPECT_TRUE(loop.invoke([] {}));
}

TEST(OSXInputLoopTests, queuedReturnAlwaysFollowsRunningPark)
{
    OSXInputLoop loop;
    std::promise<void> parked, release;
    auto gate = release.get_future().share();
    int finalPosition = 0;
    auto callback = std::async(std::launch::async, [&] {
        return loop.invoke([&] { parked.set_value(); gate.wait(); finalPosition = 1280; });
    });
    parked.get_future().wait();
    auto returning = std::async(std::launch::async, [&] {
        return loop.invoke([&] { finalPosition = 2543; });
    });
    release.set_value();
    ASSERT_TRUE(callback.get());
    ASSERT_TRUE(returning.get());
    EXPECT_EQ(2543, finalPosition);
}

TEST(OSXInputLoopTests, destructorWaitsForOwnerCleanup)
{
    std::atomic<int> cleaned{0};
    {
        OSXInputLoop loop;
        EXPECT_TRUE(loop.invoke([&] { ++cleaned; }));
    }
    EXPECT_EQ(1, cleaned.load());
}

TEST(OSXInputLoopTests, stopRemovesNativeResourcesOnTheirOwnerAndRejectsLaterCommands)
{
    OSXInputLoop loop;
    std::thread::id owner, cleanup;
    ASSERT_TRUE(loop.invoke([&] { owner = std::this_thread::get_id(); }));
    loop.stop([&] { cleanup = std::this_thread::get_id(); });
    EXPECT_EQ(owner, cleanup);
    EXPECT_NE(std::this_thread::get_id(), cleanup);
    EXPECT_FALSE(loop.invoke([] { FAIL() << "command ran after stop"; }));
}

TEST(OSXInputLoopTests, stopDrainsTimedOutOwnedReleaseBeforeCleanup)
{
    OSXInputLoop loop;
    std::promise<void> entered, release;
    const auto gate = release.get_future().share();
    auto blocker = std::async(std::launch::async, [&] {
        return loop.invoke([&] { entered.set_value(); gate.wait(); });
    });
    entered.get_future().wait();
    std::vector<int> order;
    auto owned = std::make_shared<int>(17);
    std::weak_ptr<int> lifetime = owned;
    EXPECT_EQ(OSXInputLoop::ReleaseResult::Pending,
        loop.invokeRelease([owned, &order] { order.push_back(*owned); },
                           std::chrono::milliseconds(20)));
    owned.reset();
    EXPECT_FALSE(lifetime.expired());
    EXPECT_FALSE(loop.invoke([&] { order.push_back(33); }, std::chrono::milliseconds(20)));

    auto stopping = std::async(std::launch::async, [&] {
        loop.stop([&] { order.push_back(99); });
    });
    // Rejection proves stop has closed admission before the blocked owner is
    // released, rather than relying on a scheduling delay to exercise drain.
    auto admission = OSXInputLoop::ReleaseResult::Pending;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (admission != OSXInputLoop::ReleaseResult::Rejected &&
           std::chrono::steady_clock::now() < deadline) {
        admission = loop.invokeRelease([] {}, std::chrono::milliseconds(1));
    }
    EXPECT_EQ(OSXInputLoop::ReleaseResult::Rejected, admission);
    release.set_value();
    EXPECT_TRUE(blocker.get());
    stopping.get();
    EXPECT_EQ(std::vector<int>({17, 99}), order);
    EXPECT_TRUE(lifetime.expired());
    EXPECT_EQ(OSXInputLoop::ReleaseResult::Rejected,
        loop.invokeRelease([] { FAIL() << "release ran after cleanup"; }));
}

TEST(OSXInputLoopTests, ownerReleaseRunsInlineWithoutAnotherCallback)
{
    OSXInputLoop loop;
    int releases = 0;
    ASSERT_TRUE(loop.invoke([&] {
        EXPECT_EQ(OSXInputLoop::ReleaseResult::Completed,
            loop.invokeRelease([&] {
                EXPECT_TRUE(loop.isOwner());
                ++releases;
            }, std::chrono::milliseconds(0)));
        EXPECT_EQ(1, releases);
    }));
}

TEST(OSXInputLoopTests, timedOutReleaseKeepsItsPlaceBeforeALaterPress)
{
    OSXInputLoop loop;
    std::promise<void> entered, release;
    const auto gate = release.get_future().share();
    auto blocker = std::async(std::launch::async, [&] {
        return loop.invoke([&] { entered.set_value(); gate.wait(); });
    });
    entered.get_future().wait();
    auto order = std::make_shared<std::vector<bool>>();
    EXPECT_EQ(OSXInputLoop::ReleaseResult::Pending,
        loop.invokeRelease([order] { order->push_back(false); },
                           std::chrono::milliseconds(20)));
    auto pressing = std::async(std::launch::async, [&] {
        return loop.invoke([order] { order->push_back(true); });
    });
    release.set_value();
    EXPECT_TRUE(blocker.get());
    EXPECT_TRUE(pressing.get());
    EXPECT_EQ(std::vector<bool>({false, true}), *order);
}

TEST(OSXInputLoopTests, runningReleaseCanOutliveItsCallerDeadline)
{
    OSXInputLoop loop;
    auto entered = std::make_shared<std::promise<void>>();
    std::promise<void> release;
    const auto gate = release.get_future().share();
    auto releases = std::make_shared<std::atomic<int>>(0);
    auto caller = std::async(std::launch::async, [&] {
        return loop.invokeRelease([entered, gate, releases] {
            entered->set_value();
            gate.wait();
            ++*releases;
        }, std::chrono::milliseconds(20));
    });
    entered->get_future().wait();
    const auto returned = caller.wait_for(std::chrono::seconds(1));
    release.set_value();
    EXPECT_EQ(std::future_status::ready, returned);
    EXPECT_EQ(OSXInputLoop::ReleaseResult::Pending, caller.get());
    ASSERT_TRUE(loop.invoke([] {}));
    EXPECT_EQ(1, releases->load());
}
