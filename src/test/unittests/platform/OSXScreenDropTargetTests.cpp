#include "platform/OSXScreen.h"
#include "base/EventQueue.h"
#include "test/global/gtest.h"
#include <atomic>
#include <thread>

// Do not enable the screen, install input taps, or access clipboard payloads.
TEST(OSXScreenDropTargetTests, configuredInboxIsAcceptedThroughPlatformInterface)
{
    EventQueue events;
    OSXScreen screen(&events, false, false);
    IPlatformScreen& platform = screen;
    ASSERT_NO_THROW(platform.setDropTarget("/tmp/weave-test-inbox"));
    EXPECT_EQ("/tmp/weave-test-inbox", platform.getDropTarget());
}

TEST(OSXScreenDropTargetTests, replacingAndClearingInboxDoesNotRetainStaleConfiguration)
{
    EventQueue events;
    OSXScreen screen(&events, false, false);
    screen.setDropTarget("/tmp/first-inbox");
    screen.setDropTarget("/tmp/second-inbox");
    EXPECT_EQ("/tmp/second-inbox", screen.getDropTarget());
    screen.setDropTarget("");
    EXPECT_TRUE(screen.getDropTarget().empty());
}

TEST(OSXScreenDropTargetTests, returnedSnapshotSurvivesConcurrentConfigurationChange)
{
    EventQueue events;
    OSXScreen screen(&events, false, false);
    screen.setDropTarget("/tmp/original-inbox");
    const std::string& snapshot = screen.getDropTarget();
    std::thread writer([&screen] { screen.setDropTarget("/tmp/new-inbox"); });
    writer.join();
    EXPECT_EQ("/tmp/original-inbox", snapshot);
    EXPECT_EQ("/tmp/new-inbox", screen.getDropTarget());
}

TEST(OSXScreenDropTargetTests, concurrentReadersOnlyObserveCompleteConfiguredPaths)
{
    EventQueue events;
    OSXScreen screen(&events, false, false);
    const std::string first = "/tmp/" + std::string(1024, 'a');
    const std::string second = "/tmp/" + std::string(2048, 'b');
    screen.setDropTarget(first);
    std::atomic<bool> valid(true);
    std::thread writer([&] {
        for (int i = 0; i < 10000; ++i) screen.setDropTarget(i % 2 ? first : second);
    });
    auto read = [&] {
        for (int i = 0; i < 10000; ++i) {
            const std::string copy = screen.getDropTarget();
            if (copy != first && copy != second) valid = false;
        }
    };
    std::thread reader1(read), reader2(read);
    writer.join();
    reader1.join();
    reader2.join();
    EXPECT_TRUE(valid);
}
