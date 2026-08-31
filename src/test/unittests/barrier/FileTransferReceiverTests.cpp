/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#include "barrier/FileTransferReceiver.h"

#include "test/global/gtest.h"

#include <chrono>
#include <cstdlib>
#include <thread>

namespace {

const std::string kBinding = "00112233445566778899aabbccddeeff";
const std::string kOtherBinding = "ffeeddccbbaa99887766554433221100";
const std::string kAbcDigest =
    "sha256:ba7816bf8f01cfea414140de5dae2223"
    "b00361a396177a9cb410ff61f20015ad";

UInt32 primaryTransfer(UInt32 sequence)
{
    return barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, sequence);
}

#if !defined(_WIN32)
class ScopedTempDirectoryEnvironment {
public:
    explicit ScopedTempDirectoryEnvironment(const char* value) :
        m_hadValue(std::getenv("TMPDIR") != NULL),
        m_previous(m_hadValue ? std::getenv("TMPDIR") : "")
    {
        setenv("TMPDIR", value, 1);
    }

    ~ScopedTempDirectoryEnvironment()
    {
        if (m_hadValue) {
            setenv("TMPDIR", m_previous.c_str(), 1);
        }
        else {
            unsetenv("TMPDIR");
        }
    }

private:
    bool m_hadValue;
    std::string m_previous;
};
#endif

} // namespace

TEST(FileTransferReceiverTests, commitsOnlyAfterExactOffsetSizeAndDigest)
{
    barrier::FileTransferReceiver receiver(
        kBinding, barrier::FileTransferRole::kPrimary);
    const UInt32 transferId = primaryTransfer(7);

    barrier::FileTransferReceiveResult result = receiver.handle(
        barrier::FileTransferFrame::start(kBinding, transferId, 3));
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              result.status);
    EXPECT_EQ(barrier::FileTransferReason::kNone, result.reason);

    result = receiver.handle(
        barrier::FileTransferFrame::data(kBinding, transferId, 0, "ab"));
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kDataAccepted,
              result.status);
    result = receiver.handle(
        barrier::FileTransferFrame::data(kBinding, transferId, 2, "c"));
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kDataAccepted,
              result.status);

    result = receiver.handle(
        barrier::FileTransferFrame::end(
            kBinding, transferId, 3, kAbcDigest));
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kReadyToCommit,
              result.status);
    EXPECT_TRUE(receiver.hasActiveTransfer());

    barrier::CompletedFilePayload payload;
    ASSERT_TRUE(receiver.takeCompleted(transferId, payload));
    EXPECT_EQ(3u, payload.expectedSize);
    EXPECT_EQ("abc", payload.data);
    EXPECT_TRUE(payload.spoolPath.empty());
    EXPECT_FALSE(receiver.hasActiveTransfer());
}

TEST(FileTransferReceiverTests, offsetMismatchAbortsBeforeCommit)
{
    barrier::FileTransferReceiver receiver(
        kBinding, barrier::FileTransferRole::kPrimary);
    const UInt32 transferId = primaryTransfer(8);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              receiver.handle(barrier::FileTransferFrame::start(
                  kBinding, transferId, 3)).status);

    const barrier::FileTransferReceiveResult result = receiver.handle(
        barrier::FileTransferFrame::data(kBinding, transferId, 1, "abc"));
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kTransferFailed,
              result.status);
    EXPECT_EQ(barrier::FileTransferReason::kOffsetMismatch, result.reason);
    EXPECT_FALSE(receiver.hasActiveTransfer());
}

TEST(FileTransferReceiverTests, finalOffsetAndDigestAreBothCommitGates)
{
    const UInt32 offsetTransfer = primaryTransfer(17);
    barrier::FileTransferReceiver offsetReceiver(
        kBinding, barrier::FileTransferRole::kPrimary);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              offsetReceiver.handle(barrier::FileTransferFrame::start(
                  kBinding, offsetTransfer, 3)).status);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kDataAccepted,
              offsetReceiver.handle(barrier::FileTransferFrame::data(
                  kBinding, offsetTransfer, 0, "abc")).status);
    barrier::FileTransferReceiveResult result = offsetReceiver.handle(
        barrier::FileTransferFrame::end(
            kBinding, offsetTransfer, 2, kAbcDigest));
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kTransferFailed,
              result.status);
    EXPECT_EQ(barrier::FileTransferReason::kOffsetMismatch, result.reason);

    const UInt32 digestTransfer = primaryTransfer(18);
    barrier::FileTransferReceiver digestReceiver(
        kBinding, barrier::FileTransferRole::kPrimary);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              digestReceiver.handle(barrier::FileTransferFrame::start(
                  kBinding, digestTransfer, 3)).status);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kDataAccepted,
              digestReceiver.handle(barrier::FileTransferFrame::data(
                  kBinding, digestTransfer, 0, "abc")).status);
    result = digestReceiver.handle(barrier::FileTransferFrame::end(
        kBinding, digestTransfer, 3,
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kTransferFailed,
              result.status);
    EXPECT_EQ(barrier::FileTransferReason::kDigestMismatch, result.reason);
}

TEST(FileTransferReceiverTests, wrongBindingOrStaleTransferCannotMutateActiveState)
{
    barrier::FileTransferReceiver receiver(
        kBinding, barrier::FileTransferRole::kPrimary);
    const UInt32 transferId = primaryTransfer(9);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              receiver.handle(barrier::FileTransferFrame::start(
                  kBinding, transferId, 3)).status);

    barrier::FileTransferReceiveResult result = receiver.handle(
        barrier::FileTransferFrame::data(
            kOtherBinding, transferId, 0, "abc"));
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kProtocolError,
              result.status);
    EXPECT_TRUE(receiver.hasActiveTransfer());
    EXPECT_EQ(0u, receiver.nextOffset());

    result = receiver.handle(barrier::FileTransferFrame::data(
        kBinding, primaryTransfer(10), 0, "abc"));
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kProtocolError,
              result.status);
    EXPECT_TRUE(receiver.hasActiveTransfer());
    EXPECT_EQ(0u, receiver.nextOffset());
}

TEST(FileTransferReceiverTests, secondStartIsRejectedWithoutReplacingActiveTransfer)
{
    barrier::FileTransferReceiver receiver(
        kBinding, barrier::FileTransferRole::kPrimary);
    const UInt32 first = primaryTransfer(11);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              receiver.handle(barrier::FileTransferFrame::start(
                  kBinding, first, 3)).status);

    const barrier::FileTransferReceiveResult result = receiver.handle(
        barrier::FileTransferFrame::start(
            kBinding, primaryTransfer(12), 2));
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kStartRejected,
              result.status);
    EXPECT_EQ(barrier::FileTransferReason::kBusy, result.reason);
    EXPECT_EQ(first, receiver.activeTransferId());
}

TEST(FileTransferReceiverTests, matchingCancelResetsSessionAndRequiresCancelAck)
{
    barrier::FileTransferReceiver receiver(
        kBinding, barrier::FileTransferRole::kPrimary);
    const UInt32 transferId = primaryTransfer(13);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              receiver.handle(barrier::FileTransferFrame::start(
                  kBinding, transferId, 3)).status);

    const barrier::FileTransferReceiveResult result = receiver.handle(
        barrier::FileTransferFrame::cancel(
            kBinding, transferId,
            barrier::FileTransferReason::kCancelled));
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kCancelled, result.status);
    EXPECT_EQ(barrier::FileTransferReason::kNone, result.reason);
    EXPECT_FALSE(receiver.hasActiveTransfer());
}

TEST(FileTransferReceiverTests, cancelledTransferDiscardsQueuedBulkWithoutAffectingReplacement)
{
    barrier::FileTransferReceiver receiver(
        kBinding, barrier::FileTransferRole::kPrimary);
    const UInt32 cancelledId = primaryTransfer(22);
    const UInt32 replacementId = primaryTransfer(23);

    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              receiver.handle(barrier::FileTransferFrame::start(
                  kBinding, cancelledId, 3)).status);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kCancelled,
              receiver.handle(barrier::FileTransferFrame::cancel(
                  kBinding, cancelledId,
                  barrier::FileTransferReason::kCancelled)).status);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              receiver.handle(barrier::FileTransferFrame::start(
                  kBinding, replacementId, 3)).status);

    EXPECT_EQ(barrier::FileTransferReceiveStatus::kCancelledPayloadDiscarded,
              receiver.handle(barrier::FileTransferFrame::data(
                  kBinding, cancelledId, 0, "abc")).status);
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kCancelledPayloadDiscarded,
              receiver.handle(barrier::FileTransferFrame::end(
                  kBinding, cancelledId, 3, kAbcDigest)).status);
    EXPECT_EQ(replacementId, receiver.activeTransferId());
    EXPECT_EQ(0u, receiver.nextOffset());

    ASSERT_EQ(barrier::FileTransferReceiveStatus::kDataAccepted,
              receiver.handle(barrier::FileTransferFrame::data(
                  kBinding, replacementId, 0, "abc")).status);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kReadyToCommit,
              receiver.handle(barrier::FileTransferFrame::end(
                  kBinding, replacementId, 3, kAbcDigest)).status);
    barrier::CompletedFilePayload payload;
    ASSERT_TRUE(receiver.takeCompleted(replacementId, payload));
    EXPECT_EQ("abc", payload.data);
}

TEST(FileTransferReceiverTests, cancelledTransferTombstonesAreConnectionScopedAndBounded)
{
    barrier::FileTransferReceiver receiver(
        kBinding, barrier::FileTransferRole::kPrimary);

    for (UInt32 sequence = 1; sequence <= 65; ++sequence) {
        const UInt32 transferId = primaryTransfer(100 + sequence);
        ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
                  receiver.handle(barrier::FileTransferFrame::start(
                      kBinding, transferId, 1)).status);
        ASSERT_EQ(barrier::FileTransferReceiveStatus::kCancelled,
                  receiver.handle(barrier::FileTransferFrame::cancel(
                      kBinding, transferId,
                      barrier::FileTransferReason::kCancelled)).status);
    }

    EXPECT_EQ(barrier::FileTransferReceiveStatus::kProtocolError,
              receiver.handle(barrier::FileTransferFrame::data(
                  kBinding, primaryTransfer(101), 0, "x")).status);
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kCancelledPayloadDiscarded,
              receiver.handle(barrier::FileTransferFrame::data(
                  kBinding, primaryTransfer(165), 0, "x")).status);

    barrier::FileTransferReceiver otherConnection(
        kOtherBinding, barrier::FileTransferRole::kPrimary);
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kProtocolError,
              otherConnection.handle(barrier::FileTransferFrame::data(
                  kOtherBinding, primaryTransfer(165), 0, "x")).status);
}

TEST(FileTransferReceiverTests, cancelledTransferDiscardStillEnforcesOffsetsAndSize)
{
    barrier::FileTransferReceiver receiver(
        kBinding, barrier::FileTransferRole::kPrimary);
    const UInt32 transferId = primaryTransfer(166);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              receiver.handle(barrier::FileTransferFrame::start(
                  kBinding, transferId, 3)).status);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kCancelled,
              receiver.handle(barrier::FileTransferFrame::cancel(
                  kBinding, transferId,
                  barrier::FileTransferReason::kCancelled)).status);

    EXPECT_EQ(barrier::FileTransferReceiveStatus::kProtocolError,
              receiver.handle(barrier::FileTransferFrame::data(
                  kBinding, transferId, 1, "a")).status);
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kProtocolError,
              receiver.handle(barrier::FileTransferFrame::data(
                  kBinding, transferId, 0, "abcd")).status);
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kCancelledPayloadDiscarded,
              receiver.handle(barrier::FileTransferFrame::data(
                  kBinding, transferId, 0, "abc")).status);
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kProtocolError,
              receiver.handle(barrier::FileTransferFrame::end(
                  kBinding, transferId, 2, kAbcDigest)).status);
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kCancelledPayloadDiscarded,
              receiver.handle(barrier::FileTransferFrame::end(
                  kBinding, transferId, 3, kAbcDigest)).status);

    EXPECT_EQ(barrier::FileTransferReceiveStatus::kProtocolError,
              receiver.handle(barrier::FileTransferFrame::data(
                  kBinding, transferId, 0, "abc")).status);
}

TEST(FileTransferReceiverTests, cancelledSpoolTransferGatesReplacementUntilCleanup)
{
    barrier::FileTransferReceiveLimits limits;
    limits.memoryLimit = 1;
    limits.reserveLimit = 1;
    limits.asyncQueueLimit = 2 *
        FileReceiveSession::kMaxAsyncChunkSize;
    barrier::FileTransferReceiver receiver(
        kBinding, barrier::FileTransferRole::kPrimary, limits);
    const UInt32 cancelledId = primaryTransfer(19);
    const UInt32 replacementId = primaryTransfer(20);

    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              receiver.handle(barrier::FileTransferFrame::start(
                  kBinding, cancelledId,
                  FileReceiveSession::kMaxAsyncChunkSize)).status);
    ASSERT_NE(barrier::FileTransferReceiveStatus::kTransferFailed,
              receiver.handle(barrier::FileTransferFrame::data(
                  kBinding, cancelledId, 0,
                  std::string(FileReceiveSession::kMaxAsyncChunkSize,
                              'a'))).status);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kCancelled,
              receiver.handle(barrier::FileTransferFrame::cancel(
                  kBinding, cancelledId,
                  barrier::FileTransferReason::kCancelled)).status);

    barrier::FileTransferReceiveResult replacement = receiver.handle(
        barrier::FileTransferFrame::start(kBinding, replacementId, 1));
    if (replacement.status ==
        barrier::FileTransferReceiveStatus::kStartRejected) {
        EXPECT_EQ(barrier::FileTransferReason::kBusy, replacement.reason);
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            replacement = receiver.handle(
                barrier::FileTransferFrame::start(
                    kBinding, replacementId, 1));
        } while (replacement.status ==
                     barrier::FileTransferReceiveStatus::kStartRejected &&
                 std::chrono::steady_clock::now() < deadline);
    }
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              replacement.status);
}

TEST(FileTransferReceiverTests, perConnectionSessionsDoNotCompete)
{
    barrier::FileTransferReceiver first(
        kBinding, barrier::FileTransferRole::kPrimary);
    barrier::FileTransferReceiver second(
        kOtherBinding, barrier::FileTransferRole::kPrimary);

    EXPECT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              first.handle(barrier::FileTransferFrame::start(
                  kBinding, primaryTransfer(14), 3)).status);
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              second.handle(barrier::FileTransferFrame::start(
                  kOtherBinding, primaryTransfer(15), 3)).status);
    EXPECT_EQ(primaryTransfer(14), first.activeTransferId());
    EXPECT_EQ(primaryTransfer(15), second.activeTransferId());
}

TEST(FileTransferReceiverTests,
     lateCancelForBulkAbortedTransferDoesNotReplaceCurrentTransfer)
{
    barrier::FileTransferReceiver receiver(
        kBinding, barrier::FileTransferRole::kPrimary);
    const UInt32 abortedId = primaryTransfer(22);
    const UInt32 replacementId = primaryTransfer(23);
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              receiver.handle(barrier::FileTransferFrame::start(
                  kBinding, abortedId, 3)).status);

    EXPECT_EQ(abortedId, receiver.abortActiveTransfer());
    EXPECT_FALSE(receiver.hasActiveTransfer());
    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              receiver.handle(barrier::FileTransferFrame::start(
                  kBinding, replacementId, 1)).status);

    const barrier::FileTransferReceiveResult lateCancel = receiver.handle(
        barrier::FileTransferFrame::cancel(
            kBinding, abortedId,
            barrier::FileTransferReason::kConnectionLost));
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kCancelAlreadyApplied,
              lateCancel.status);
    EXPECT_EQ(replacementId, receiver.activeTransferId());
}

TEST(FileTransferReceiverTests, asynchronousSpoolMustFinishBeforeOwnershipTransfer)
{
    barrier::FileTransferReceiveLimits limits;
    limits.memoryLimit = 1;
    limits.reserveLimit = 1;
    limits.asyncQueueLimit = 1024;
    barrier::FileTransferReceiver receiver(
        kBinding, barrier::FileTransferRole::kPrimary, limits);
    const UInt32 transferId = primaryTransfer(16);

    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              receiver.handle(barrier::FileTransferFrame::start(
                  kBinding, transferId, 3)).status);
    ASSERT_NE(barrier::FileTransferReceiveStatus::kTransferFailed,
              receiver.handle(barrier::FileTransferFrame::data(
                  kBinding, transferId, 0, "abc")).status);
    barrier::FileTransferReceiveResult result = receiver.handle(
        barrier::FileTransferFrame::end(
            kBinding, transferId, 3, kAbcDigest));
    EXPECT_EQ(barrier::FileTransferReceiveStatus::kAwaitingCommit,
              result.status);

    barrier::CompletedFilePayload payload;
    EXPECT_FALSE(receiver.takeCompleted(transferId, payload));
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    do {
        result = receiver.pollCompletion(transferId);
        if (result.status ==
            barrier::FileTransferReceiveStatus::kReadyToCommit) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);

    ASSERT_EQ(barrier::FileTransferReceiveStatus::kReadyToCommit,
              result.status);
    ASSERT_TRUE(receiver.takeCompleted(transferId, payload));
    EXPECT_TRUE(payload.data.empty());
    EXPECT_EQ(3u, payload.expectedSize);
    EXPECT_FALSE(payload.spoolPath.empty());
    barrier::fs::remove(payload.spoolPath);
}

#if !defined(_WIN32)
TEST(FileTransferReceiverTests, invalidTempDirectoryFailsTransferWithoutTerminating)
{
    ScopedTempDirectoryEnvironment tempDirectory(
        "/definitely/missing/weave-receive-directory");
    barrier::FileTransferReceiveLimits limits;
    limits.memoryLimit = 1;
    limits.reserveLimit = 1;
    limits.asyncQueueLimit = 1024;
    barrier::FileTransferReceiver receiver(
        kBinding, barrier::FileTransferRole::kPrimary, limits);
    const UInt32 transferId = primaryTransfer(21);

    ASSERT_EQ(barrier::FileTransferReceiveStatus::kStartAccepted,
              receiver.handle(barrier::FileTransferFrame::start(
                  kBinding, transferId, 3)).status);

    barrier::FileTransferReceiveResult result;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    do {
        result = receiver.pollCompletion(transferId);
        if (result.status ==
            barrier::FileTransferReceiveStatus::kTransferFailed) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);

    EXPECT_EQ(barrier::FileTransferReceiveStatus::kTransferFailed,
              result.status);
    EXPECT_EQ(barrier::FileTransferReason::kIoError, result.reason);
    EXPECT_FALSE(receiver.hasActiveTransfer());
}
#endif
