#include "test/global/gtest.h"

#include "platform/XWindowsClipboardSnapshotState.h"

#include <memory>

TEST(XWindowsClipboardSnapshotStateTests, pendingRequestsForOneSelectionCoalesce)
{
    XWindowsClipboardSnapshotState state;
    const XWindowsClipboardSnapshotState::Request first = state.queue(
        kClipboardClipboard, 0x100, 10, 7);
    const XWindowsClipboardSnapshotState::Request latest = state.queue(
        kClipboardClipboard, 0x200, 20, 8);

    XWindowsClipboardSnapshotState::Request taken;
    ASSERT_TRUE(state.takeNext(&taken));
    EXPECT_EQ(latest.generation, taken.generation);
    EXPECT_EQ(latest.owner, taken.owner);
    EXPECT_GT(latest.generation, first.generation);
    EXPECT_FALSE(state.takeNext(&taken));
}

TEST(XWindowsClipboardSnapshotStateTests, staleCompletionCannotReplaceLatestOwner)
{
    XWindowsClipboardSnapshotState state;
    state.queue(kClipboardClipboard, 0x100, 10, 7);
    XWindowsClipboardSnapshotState::Request first;
    ASSERT_TRUE(state.takeNext(&first));

    state.queue(kClipboardClipboard, 0x200, 20, 8);
    XWindowsClipboardSnapshotState::Request latest;
    ASSERT_TRUE(state.takeNext(&latest));

    const std::shared_ptr<const String> stale(new String("stale"));
    EXPECT_FALSE(state.complete(first, 0x100, stale));
    const std::shared_ptr<const String> fresh(new String("fresh"));
    EXPECT_TRUE(state.complete(latest, 0x200, fresh));

    std::shared_ptr<const String> ready;
    Window owner = None;
    Time timestamp = CurrentTime;
    ASSERT_TRUE(state.copyReady(
        kClipboardClipboard, &ready, &owner, &timestamp));
    EXPECT_EQ("fresh", *ready);
    EXPECT_EQ(0x200u, owner);
    EXPECT_EQ(20u, timestamp);
}

TEST(XWindowsClipboardSnapshotStateTests, selfOwnershipInvalidatesWorkerCompletion)
{
    XWindowsClipboardSnapshotState state;
    state.queue(kClipboardClipboard, 0x200, 10, 7);
    XWindowsClipboardSnapshotState::Request external;
    ASSERT_TRUE(state.takeNext(&external));

    state.invalidate(kClipboardClipboard, 0x100);
    const std::shared_ptr<const String> stale(new String("external"));
    EXPECT_FALSE(state.complete(external, 0x200, stale));
    EXPECT_FALSE(state.blocksSynchronousRead(kClipboardClipboard));

    std::shared_ptr<const String> ready;
    Window owner = None;
    Time timestamp = CurrentTime;
    EXPECT_FALSE(state.copyReady(
        kClipboardClipboard, &ready, &owner, &timestamp));
}

TEST(XWindowsClipboardSnapshotStateTests, actualOwnerMismatchRejectsCompletion)
{
    XWindowsClipboardSnapshotState state;
    state.queue(kClipboardSelection, 0x200, 10, 7);
    XWindowsClipboardSnapshotState::Request request;
    ASSERT_TRUE(state.takeNext(&request));

    const std::shared_ptr<const String> snapshot(new String("selection"));
    EXPECT_FALSE(state.complete(request, 0x300, snapshot));
    EXPECT_TRUE(state.blocksSynchronousRead(kClipboardSelection));
}

TEST(XWindowsClipboardSnapshotStateTests, failedAsyncReadKeepsProviderOffMainThread)
{
    XWindowsClipboardSnapshotState state;
    state.queue(kClipboardClipboard, 0x200, 10, 7);
    XWindowsClipboardSnapshotState::Request request;
    ASSERT_TRUE(state.takeNext(&request));

    state.fail(request, 0x200);
    EXPECT_TRUE(state.blocksSynchronousRead(kClipboardClipboard));
}

TEST(XWindowsClipboardSnapshotStateTests, retriesStayOnSameGenerationAndAreBounded)
{
    XWindowsClipboardSnapshotState state;
    state.queue(kClipboardClipboard, 0x200, 10, 7);
    XWindowsClipboardSnapshotState::Request request;
    ASSERT_TRUE(state.takeNext(&request));

    ASSERT_TRUE(state.retry(request, 0x200, 2));
    ASSERT_TRUE(state.takeNext(&request));
    EXPECT_EQ(1u, request.attempt);
    ASSERT_TRUE(state.retry(request, 0x200, 2));
    ASSERT_TRUE(state.takeNext(&request));
    EXPECT_EQ(2u, request.attempt);
    EXPECT_FALSE(state.retry(request, 0x200, 2));
    EXPECT_FALSE(state.hasPending());
}

TEST(XWindowsClipboardSnapshotStateTests,
     unavailableDisplayRetriesAreBoundedWithoutOwnerObservation)
{
    XWindowsClipboardSnapshotState state;
    const Window owner = 0x1234;
    XWindowsClipboardSnapshotState::Request request = state.queue(
        kClipboardClipboard, owner, 44, 7);
    ASSERT_TRUE(state.takeNext(&request));

    EXPECT_TRUE(state.retryWithoutOwnerObservation(request, 1));
    ASSERT_TRUE(state.takeNext(&request));
    EXPECT_EQ(1u, request.attempt);
    EXPECT_FALSE(state.retryWithoutOwnerObservation(request, 1));

    state.failWithoutOwnerObservation(request);
    EXPECT_TRUE(state.blocksSynchronousRead(kClipboardClipboard));
}
