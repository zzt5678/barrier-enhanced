#include "barrier/FileTransferSendState.h"
#include "barrier/FileTransferProtocol.h"

#include "test/global/gtest.h"

#include <chrono>
#include <future>
#include <memory>
#include <thread>

namespace {

const UInt32 kTransferId = 37;
const std::string kClipboardSession =
    "00112233445566778899aabbccddeeff";

}

TEST(FileTransferSendStateTests, staleStartAckDoesNotAdvanceTransfer)
{
    barrier::FileTransferSendState state(
        kTransferId, std::chrono::milliseconds(50),
        std::chrono::milliseconds(50));

    ASSERT_TRUE(state.markStartQueued(3));
    EXPECT_FALSE(state.startAcknowledged());
    EXPECT_FALSE(state.signalStartAck(
        kTransferId + 1, barrier::FileTransferReason::kNone));
    EXPECT_FALSE(state.readyForData());

    EXPECT_TRUE(state.signalStartAck(
        kTransferId, barrier::FileTransferReason::kNone));
    EXPECT_TRUE(state.startAcknowledged());
    EXPECT_EQ(barrier::FileTransferReason::kNone, state.waitForStartAck());
    EXPECT_TRUE(state.readyForData());
}

TEST(FileTransferSendStateTests, rejectedStartStopsTransfer)
{
    barrier::FileTransferSendState state(
        kTransferId, std::chrono::milliseconds(50),
        std::chrono::milliseconds(50));

    ASSERT_TRUE(state.markStartQueued(3));
    ASSERT_TRUE(state.signalStartAck(
        kTransferId, barrier::FileTransferReason::kRejected));

    EXPECT_EQ(barrier::FileTransferReason::kRejected,
              state.waitForStartAck());
    EXPECT_TRUE(state.stopped());
    EXPECT_FALSE(state.readyForData());
    EXPECT_FALSE(state.committed());
}

TEST(FileTransferSendStateTests, matchingCommitAckCommitsRecordedOffsets)
{
    barrier::FileTransferSendState state(
        kTransferId, std::chrono::milliseconds(50),
        std::chrono::milliseconds(50));

    ASSERT_TRUE(state.markStartQueued(6));
    ASSERT_TRUE(state.signalStartAck(
        kTransferId, barrier::FileTransferReason::kNone));
    ASSERT_EQ(barrier::FileTransferReason::kNone, state.waitForStartAck());
    ASSERT_TRUE(state.markDataQueued(0, 3));
    ASSERT_TRUE(state.markDataQueued(3, 3));
    EXPECT_FALSE(state.markDataQueued(3, 1));
    ASSERT_TRUE(state.markEndQueued(6));
    EXPECT_EQ(6u, state.finalOffset());
    ASSERT_TRUE(state.signalCommitAck(
        kTransferId, barrier::FileTransferReason::kNone));

    EXPECT_EQ(barrier::FileTransferReason::kNone,
              state.waitForCommitAck());
    EXPECT_TRUE(state.committed());
}

TEST(FileTransferSendStateTests, staleOrFailedCommitAckCannotCommit)
{
    barrier::FileTransferSendState state(
        kTransferId, std::chrono::milliseconds(50),
        std::chrono::milliseconds(50));

    ASSERT_TRUE(state.markStartQueued(0));
    ASSERT_TRUE(state.signalStartAck(
        kTransferId, barrier::FileTransferReason::kNone));
    ASSERT_EQ(barrier::FileTransferReason::kNone, state.waitForStartAck());
    ASSERT_TRUE(state.markEndQueued(0));

    EXPECT_FALSE(state.signalCommitAck(
        kTransferId + 1, barrier::FileTransferReason::kNone));
    EXPECT_FALSE(state.committed());
    EXPECT_TRUE(state.signalCommitAck(
        kTransferId, barrier::FileTransferReason::kDigestMismatch));
    EXPECT_EQ(barrier::FileTransferReason::kDigestMismatch,
              state.waitForCommitAck());
    EXPECT_FALSE(state.committed());
    EXPECT_TRUE(state.stopped());
}

TEST(FileTransferSendStateTests, commitWaitUsesInjectedBoundedTimeout)
{
    barrier::FileTransferSendState state(
        kTransferId, std::chrono::milliseconds(5),
        std::chrono::milliseconds(5));

    ASSERT_TRUE(state.markStartQueued(0));
    ASSERT_TRUE(state.signalStartAck(
        kTransferId, barrier::FileTransferReason::kNone));
    ASSERT_EQ(barrier::FileTransferReason::kNone, state.waitForStartAck());
    ASSERT_TRUE(state.markEndQueued(0));

    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(barrier::FileTransferReason::kTimeout,
              state.waitForCommitAck());
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, std::chrono::milliseconds(100));
    EXPECT_FALSE(state.committed());
    EXPECT_TRUE(state.stopped());
}

TEST(FileTransferSendStateTests, commitWaitHasIndependentLongerTimeout)
{
    auto state = std::make_shared<barrier::FileTransferSendState>(
        kTransferId, std::chrono::milliseconds(2),
        std::chrono::milliseconds(500));

    ASSERT_TRUE(state->markStartQueued(0));
    ASSERT_TRUE(state->signalStartAck(
        kTransferId, barrier::FileTransferReason::kNone));
    ASSERT_EQ(barrier::FileTransferReason::kNone, state->waitForStartAck());
    ASSERT_TRUE(state->markEndQueued(0));

    std::future<bool> acknowledged = std::async(
        std::launch::async, [state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return state->signalCommitAck(
                kTransferId, barrier::FileTransferReason::kNone);
        });

    EXPECT_EQ(barrier::FileTransferReason::kNone,
              state->waitForCommitAck());
    EXPECT_TRUE(acknowledged.get());
    EXPECT_TRUE(state->committed());
}

TEST(FileTransferSendStateTests, interruptWakesStartAckWait)
{
    auto state = std::make_shared<barrier::FileTransferSendState>(
        kTransferId, std::chrono::milliseconds(250),
        std::chrono::milliseconds(250));
    ASSERT_TRUE(state->markStartQueued(3));

    std::future<barrier::FileTransferReason> result = std::async(
        std::launch::async, [state]() { return state->waitForStartAck(); });
    state->interrupt();

    ASSERT_EQ(std::future_status::ready,
              result.wait_for(std::chrono::milliseconds(100)));
    EXPECT_EQ(barrier::FileTransferReason::kCancelled, result.get());
}

TEST(FileTransferSendStateTests, connectionLossWakesCommitAckWait)
{
    auto state = std::make_shared<barrier::FileTransferSendState>(
        kTransferId, std::chrono::milliseconds(250),
        std::chrono::milliseconds(250));
    ASSERT_TRUE(state->markStartQueued(0));
    ASSERT_TRUE(state->signalStartAck(
        kTransferId, barrier::FileTransferReason::kNone));
    ASSERT_EQ(barrier::FileTransferReason::kNone, state->waitForStartAck());
    ASSERT_TRUE(state->markEndQueued(0));

    std::future<barrier::FileTransferReason> result = std::async(
        std::launch::async, [state]() { return state->waitForCommitAck(); });
    state->connectionLost();

    ASSERT_EQ(std::future_status::ready,
              result.wait_for(std::chrono::milliseconds(100)));
    EXPECT_EQ(barrier::FileTransferReason::kConnectionLost, result.get());
}

TEST(FileTransferSendStateTests, cancelAckRequiresMatchingQueuedCancel)
{
    barrier::FileTransferSendState state(
        kTransferId, std::chrono::milliseconds(50),
        std::chrono::milliseconds(50));

    ASSERT_TRUE(state.markStartQueued(3));
    ASSERT_TRUE(state.signalStartAck(
        kTransferId, barrier::FileTransferReason::kNone));
    ASSERT_EQ(barrier::FileTransferReason::kNone, state.waitForStartAck());
    ASSERT_TRUE(state.markCancelQueued(
        barrier::FileTransferReason::kCancelled));
    EXPECT_TRUE(state.startAcknowledged());

    EXPECT_FALSE(state.signalCancelAck(
        kTransferId + 1, barrier::FileTransferReason::kNone));
    EXPECT_FALSE(state.cancelAcknowledged());
    EXPECT_FALSE(state.signalCancelAck(
        kTransferId, barrier::FileTransferReason::kProtocolError));
    EXPECT_FALSE(state.cancelAcknowledged());
    EXPECT_TRUE(state.signalCancelAck(
        kTransferId, barrier::FileTransferReason::kNone));
    EXPECT_TRUE(state.cancelAcknowledged());
    EXPECT_FALSE(state.committed());
}

TEST(FileTransferSendStateTests, localFailureStopsBeforeStartWithStructuredReason)
{
    barrier::FileTransferSendState state(
        kTransferId, std::chrono::milliseconds(50),
        std::chrono::milliseconds(50));

    EXPECT_TRUE(state.fail(barrier::FileTransferReason::kSizeLimit));
    EXPECT_EQ(barrier::FileTransferReason::kSizeLimit, state.result());
    EXPECT_TRUE(state.stopped());
    EXPECT_FALSE(state.markStartQueued(3));
    EXPECT_FALSE(state.fail(barrier::FileTransferReason::kIoError));
    EXPECT_FALSE(state.fail(barrier::FileTransferReason::kNone));
}

TEST(FileTransferSendStateTests, senderIdentityIsImmutableForQueuedStart)
{
    barrier::FileTransferSendState state(
        kTransferId, barrier::FileTransferKind::kClipboard, 73,
        kClipboardSession, std::chrono::milliseconds(50),
        std::chrono::milliseconds(50));

    EXPECT_EQ(barrier::FileTransferKind::kClipboard, state.kind());
    EXPECT_EQ(73u, state.clipboardRevision());
    EXPECT_EQ(kClipboardSession, state.clipboardSessionId());
}
