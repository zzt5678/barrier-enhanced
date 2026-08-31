#include "test/global/gtest.h"

#include "platform/MSWindowsClipboardSnapshotState.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

TEST(MSWindowsClipboardSnapshotStateTests, latestRevisionCoalescesPendingRequests)
{
    MSWindowsClipboardSnapshotState state;
    const MSWindowsClipboardSnapshotState::Request first = state.queue(10, 1);
    const MSWindowsClipboardSnapshotState::Request latest = state.queue(11, 2);

    MSWindowsClipboardSnapshotState::Request taken;
    ASSERT_TRUE(state.takeNext(&taken));
    EXPECT_EQ(latest.generation, taken.generation);
    EXPECT_EQ(11u, taken.windowsSequence);
    EXPECT_EQ(2u, taken.protocolSequence);
    EXPECT_GT(latest.generation, first.generation);
    EXPECT_FALSE(state.takeNext(&taken));
}

TEST(MSWindowsClipboardSnapshotStateTests, staleGenerationCannotCommit)
{
    MSWindowsClipboardSnapshotState state;
    state.queue(10, 1);
    MSWindowsClipboardSnapshotState::Request stale;
    ASSERT_TRUE(state.takeNext(&stale));

    state.queue(11, 2);
    const std::shared_ptr<const String> snapshot(new String("stale"));
    EXPECT_FALSE(state.complete(stale, 10, snapshot));
}

TEST(MSWindowsClipboardSnapshotStateTests, changedWindowsSequenceCannotCommit)
{
    MSWindowsClipboardSnapshotState state;
    state.queue(10, 1);
    MSWindowsClipboardSnapshotState::Request request;
    ASSERT_TRUE(state.takeNext(&request));

    const std::shared_ptr<const String> snapshot(new String("stale"));
    EXPECT_FALSE(state.complete(request, 11, snapshot));
    EXPECT_TRUE(state.blocksSynchronousRead());
}

TEST(MSWindowsClipboardSnapshotStateTests, remoteClipboardSetInvalidatesInFlightRead)
{
    MSWindowsClipboardSnapshotState state;
    state.queue(10, 1);
    MSWindowsClipboardSnapshotState::Request request;
    ASSERT_TRUE(state.takeNext(&request));

    state.invalidate();
    const std::shared_ptr<const String> snapshot(new String("external"));
    EXPECT_FALSE(state.complete(request, 10, snapshot));
    EXPECT_FALSE(state.blocksSynchronousRead());
}

TEST(MSWindowsClipboardSnapshotStateTests, disableInvalidatesReadySnapshotAndRequeuesSameSequence)
{
    MSWindowsClipboardSnapshotState state;
    state.queue(10, 1);
    MSWindowsClipboardSnapshotState::Request request;
    ASSERT_TRUE(state.takeNext(&request));

    const std::shared_ptr<const String> snapshot(new String("ready"));
    ASSERT_TRUE(state.complete(request, 10, snapshot));
    EXPECT_FALSE(state.needsSnapshot(10));

    // MSWindowsScreen::disable() must revoke committed and in-flight state so
    // reconnecting to the same Windows clipboard sequence takes a fresh read.
    state.invalidate();

    EXPECT_TRUE(state.needsSnapshot(10));
    std::shared_ptr<const String> stale;
    UInt32 sequence = 0;
    EXPECT_FALSE(state.copyReady(&stale, &sequence));
}

TEST(MSWindowsClipboardSnapshotStateTests, failedReadDoesNotReopenSameRevision)
{
    MSWindowsClipboardSnapshotState state;
    state.queue(10, 1);
    MSWindowsClipboardSnapshotState::Request request;
    ASSERT_TRUE(state.takeNext(&request));

    state.fail(request);
    EXPECT_TRUE(state.blocksSynchronousRead());
    EXPECT_FALSE(state.needsSnapshot(10));
    EXPECT_TRUE(state.needsSnapshot(11));
}

TEST(MSWindowsClipboardSnapshotStateTests, retriesRemainBoundedOnOneGeneration)
{
    MSWindowsClipboardSnapshotState state;
    state.queue(10, 1);
    MSWindowsClipboardSnapshotState::Request request;
    ASSERT_TRUE(state.takeNext(&request));

    ASSERT_TRUE(state.retry(request, 2));
    ASSERT_TRUE(state.takeNext(&request));
    EXPECT_EQ(1u, request.attempt);
    ASSERT_TRUE(state.retry(request, 2));
    ASSERT_TRUE(state.takeNext(&request));
    EXPECT_EQ(2u, request.attempt);
    EXPECT_FALSE(state.retry(request, 2));
}

TEST(MSWindowsClipboardSnapshotStateTests, readySnapshotRequiresMatchingSequence)
{
    MSWindowsClipboardSnapshotState state;
    state.queue(10, 1);
    MSWindowsClipboardSnapshotState::Request request;
    ASSERT_TRUE(state.takeNext(&request));

    const std::shared_ptr<const String> snapshot(new String("ready"));
    ASSERT_TRUE(state.complete(request, 10, snapshot));

    std::shared_ptr<const String> ready;
    UInt32 sequence = 0;
    ASSERT_TRUE(state.copyReady(&ready, &sequence));
    EXPECT_EQ("ready", *ready);
    EXPECT_EQ(10u, sequence);
    EXPECT_FALSE(state.needsSnapshot(10));
    EXPECT_TRUE(state.needsSnapshot(11));
}

TEST(MSWindowsClipboardSnapshotStateTests, readyNotificationIsConsumedOnce)
{
    MSWindowsClipboardSnapshotState state;
    state.queue(10, 1);
    MSWindowsClipboardSnapshotState::Request request;
    ASSERT_TRUE(state.takeNext(&request));

    const std::shared_ptr<const String> snapshot(new String("ready"));
    ASSERT_TRUE(state.complete(request, 10, snapshot));
    UInt32 protocolSequence = 0;
    ASSERT_TRUE(state.takeReadyNotification(&protocolSequence));
    EXPECT_EQ(1u, protocolSequence);
    EXPECT_FALSE(state.takeReadyNotification(&protocolSequence));
}

TEST(MSWindowsClipboardSnapshotStateTests, zeroSequenceFailsClosed)
{
    MSWindowsClipboardSnapshotState state;
    state.queue(0, 1);
    MSWindowsClipboardSnapshotState::Request request;
    ASSERT_TRUE(state.takeNext(&request));

    const std::shared_ptr<const String> snapshot(new String("unverifiable"));
    EXPECT_FALSE(state.complete(request, 0, snapshot));
    EXPECT_FALSE(state.needsSnapshot(0));
}

TEST(MSWindowsClipboardPublishStateTests, latestRemoteClipboardWins)
{
    MSWindowsClipboardPublishState state;
    const std::shared_ptr<const String> first(new String("first"));
    const std::shared_ptr<const String> latest(new String("latest"));
    const MSWindowsClipboardPublishState::Request stale = state.queue(first, 10);
    const MSWindowsClipboardPublishState::Request expected = state.queue(latest, 10);

    MSWindowsClipboardPublishState::Request actual;
    ASSERT_TRUE(state.takeNext(&actual));
    EXPECT_EQ(expected.generation, actual.generation);
    EXPECT_EQ(latest.get(), actual.snapshot.get());
    EXPECT_FALSE(state.complete(stale, 0));
    EXPECT_TRUE(state.complete(actual, 11));
}

TEST(MSWindowsClipboardPublishStateTests, remotePublishRetryIsBounded)
{
    MSWindowsClipboardPublishState state;
    state.queue(std::shared_ptr<const String>(new String("snapshot")), 10);
    MSWindowsClipboardPublishState::Request request;
    ASSERT_TRUE(state.takeNext(&request));

    ASSERT_TRUE(state.retry(request, 1));
    ASSERT_TRUE(state.takeNext(&request));
    EXPECT_EQ(1u, request.attempt);
    EXPECT_FALSE(state.retry(request, 1));
    EXPECT_FALSE(state.hasPending());
    EXPECT_FALSE(state.hasInFlight());
}

TEST(MSWindowsClipboardPublishStateTests, queuedLatestSurvivesOlderInFlight)
{
    MSWindowsClipboardPublishState state;
    const std::shared_ptr<const String> first(new String("first"));
    const std::shared_ptr<const String> latest(new String("latest"));
    state.queue(first, 10);

    MSWindowsClipboardPublishState::Request inFlight;
    ASSERT_TRUE(state.takeNext(&inFlight));
    ASSERT_TRUE(state.hasInFlight());

    const MSWindowsClipboardPublishState::Request queued =
        state.queue(latest, 10);
    EXPECT_TRUE(state.hasInFlight());
    EXPECT_TRUE(state.hasPending());
    EXPECT_FALSE(state.complete(inFlight, 11));

    MSWindowsClipboardPublishState::Request next;
    ASSERT_TRUE(state.takeNext(&next));
    EXPECT_EQ(queued.generation, next.generation);
    EXPECT_EQ(11u, next.expectedWindowsSequence);
    EXPECT_EQ(latest.get(), next.snapshot.get());
}

TEST(MSWindowsClipboardPublishStateTests,
     queuedLatestRebasesAcrossIntermediateWindowsSequence)
{
    MSWindowsClipboardPublishState state;
    const std::shared_ptr<const String> first(new String("first"));
    const std::shared_ptr<const String> latest(new String("latest"));
    state.queue(first, 10);

    MSWindowsClipboardPublishState::Request inFlight;
    ASSERT_TRUE(state.takeNext(&inFlight));

    // EmptyClipboard and SetClipboardData may advance the Windows sequence
    // before the first publish has committed its final sequence.
    const MSWindowsClipboardPublishState::Request queued =
        state.queue(latest, 11);
    EXPECT_FALSE(state.complete(inFlight, 13));

    MSWindowsClipboardPublishState::Request next;
    ASSERT_TRUE(state.takeNext(&next));
    EXPECT_EQ(queued.generation, next.generation);
    EXPECT_EQ(13u, next.expectedWindowsSequence);
    EXPECT_EQ(latest.get(), next.snapshot.get());
}

TEST(MSWindowsClipboardPublishStateTests, invalidateKeepsActualInFlightTracked)
{
    MSWindowsClipboardPublishState state;
    state.queue(std::shared_ptr<const String>(new String("snapshot")), 10, 41);
    MSWindowsClipboardPublishState::Request request;
    ASSERT_TRUE(state.takeNext(&request));

    state.invalidate();
    EXPECT_TRUE(state.hasInFlight());
    EXPECT_FALSE(state.complete(request, 11));
    EXPECT_FALSE(state.hasInFlight());
    EXPECT_FALSE(state.hasPending());

    MSWindowsClipboardPublishState::Completion completion;
    ASSERT_TRUE(state.takeCompletion(&completion));
    EXPECT_EQ(41u, completion.publicationId);
    EXPECT_EQ(MSWindowsClipboardPublishState::CompletionResult::Superseded,
              completion.result);
    EXPECT_EQ(0u, completion.committedWindowsSequence);
    EXPECT_FALSE(state.takeCompletion(&completion));
}

TEST(MSWindowsClipboardPublishStateTests,
     queueIsNotCommitAndSuccessReportsCallerPublicationId)
{
    MSWindowsClipboardPublishState state;
    state.queue(std::shared_ptr<const String>(new String("snapshot")), 10, 73);

    MSWindowsClipboardPublishState::Completion completion;
    EXPECT_FALSE(state.takeCompletion(&completion));

    MSWindowsClipboardPublishState::Request request;
    ASSERT_TRUE(state.takeNext(&request));
    EXPECT_EQ(73u, request.publicationId);
    EXPECT_TRUE(state.complete(request, 11));

    ASSERT_TRUE(state.takeCompletion(&completion));
    EXPECT_EQ(73u, completion.publicationId);
    EXPECT_EQ(MSWindowsClipboardPublishState::CompletionResult::Succeeded,
              completion.result);
    EXPECT_EQ(11u, completion.committedWindowsSequence);
    EXPECT_FALSE(state.takeCompletion(&completion));
}

TEST(MSWindowsClipboardPublishStateTests,
     retryExhaustionReportsFailureExactlyOnce)
{
    MSWindowsClipboardPublishState state;
    state.queue(std::shared_ptr<const String>(new String("snapshot")), 10, 91);

    MSWindowsClipboardPublishState::Request request;
    ASSERT_TRUE(state.takeNext(&request));
    ASSERT_TRUE(state.retry(request, 1));
    ASSERT_TRUE(state.takeNext(&request));
    EXPECT_FALSE(state.retry(request, 1));

    MSWindowsClipboardPublishState::Completion completion;
    ASSERT_TRUE(state.takeCompletion(&completion));
    EXPECT_EQ(91u, completion.publicationId);
    EXPECT_EQ(MSWindowsClipboardPublishState::CompletionResult::Failed,
              completion.result);
    EXPECT_EQ(0u, completion.committedWindowsSequence);
    EXPECT_FALSE(state.takeCompletion(&completion));
}

TEST(MSWindowsClipboardPublishStateTests,
     newerQueuedPublicationSupersedesOlderCompletion)
{
    MSWindowsClipboardPublishState state;
    state.queue(std::shared_ptr<const String>(new String("first")), 10, 101);
    MSWindowsClipboardPublishState::Request first;
    ASSERT_TRUE(state.takeNext(&first));

    state.queue(std::shared_ptr<const String>(new String("latest")), 10, 102);
    EXPECT_FALSE(state.complete(first, 11));

    MSWindowsClipboardPublishState::Completion completion;
    ASSERT_TRUE(state.takeCompletion(&completion));
    EXPECT_EQ(101u, completion.publicationId);
    EXPECT_EQ(MSWindowsClipboardPublishState::CompletionResult::Superseded,
              completion.result);

    MSWindowsClipboardPublishState::Request latest;
    ASSERT_TRUE(state.takeNext(&latest));
    EXPECT_TRUE(state.complete(latest, 12));
    ASSERT_TRUE(state.takeCompletion(&completion));
    EXPECT_EQ(102u, completion.publicationId);
    EXPECT_EQ(MSWindowsClipboardPublishState::CompletionResult::Succeeded,
              completion.result);
}

TEST(MSWindowsClipboardWorkerLifetimeTests,
     blockedProviderShutdownReturnsAtDeadlineAndLateCompletionIsFenced)
{
    std::shared_ptr<MSWindowsClipboardWorkerLifetime> lifetime(
        new MSWindowsClipboardWorkerLifetime());
    const std::weak_ptr<MSWindowsClipboardWorkerLifetime> weakLifetime(
        lifetime);
    const std::uint64_t notificationToken = lifetime->notificationToken();
    std::atomic<bool> ownerTouched(false);
    std::mutex providerMutex;
    std::condition_variable providerChanged;
    bool providerEntered = false;
    bool providerReleased = false;

    std::thread worker([lifetime, notificationToken, &ownerTouched,
                        &providerMutex, &providerChanged, &providerEntered,
                        &providerReleased]() {
        {
            std::unique_lock<std::mutex> lock(providerMutex);
            providerEntered = true;
            providerChanged.notify_all();
            providerChanged.wait(lock, [&providerReleased]() {
                return providerReleased;
            });
        }

        if (lifetime->acceptsNotification(notificationToken)) {
            ownerTouched.store(true);
        }
        lifetime->markFinished();
    });

    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(providerMutex);
        entered = providerChanged.wait_for(
            lock, std::chrono::seconds(1), [&providerEntered]() {
                return providerEntered;
            });
    }
    EXPECT_TRUE(entered);
    if (!entered) {
        {
            std::lock_guard<std::mutex> lock(providerMutex);
            providerReleased = true;
        }
        providerChanged.notify_all();
        worker.join();
        return;
    }

    lifetime->requestStop();
    const std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    EXPECT_FALSE(lifetime->waitForFinished(std::chrono::milliseconds(20)));
    const std::chrono::milliseconds elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
    EXPECT_LT(elapsed.count(), 250);

    lifetime.reset();
    EXPECT_FALSE(weakLifetime.expired());
    {
        std::lock_guard<std::mutex> lock(providerMutex);
        providerReleased = true;
    }
    providerChanged.notify_all();
    worker.join();

    EXPECT_FALSE(ownerTouched.load());
    EXPECT_TRUE(weakLifetime.expired());
}

TEST(MSWindowsClipboardWorkerLifetimeTests,
     replacementOwnerRejectsPreviousWorkerNotificationToken)
{
    MSWindowsClipboardWorkerLifetime previous;
    const std::uint64_t previousToken = previous.notificationToken();
    previous.requestStop();

    MSWindowsClipboardWorkerLifetime replacement;
    const std::uint64_t replacementToken = replacement.notificationToken();

    EXPECT_NE(previousToken, replacementToken);
    EXPECT_FALSE(previous.acceptsNotification(previousToken));
    EXPECT_FALSE(replacement.acceptsNotification(previousToken));
    EXPECT_TRUE(replacement.acceptsNotification(replacementToken));
}

TEST(MSWindowsClipboardWorkerLifetimeTests,
     blockedPublicationAfterCompletionCannotReachDestroyedOwner)
{
    struct OwnerProbe {
        explicit OwnerProbe(std::atomic<bool>* destroyed_) :
            destroyed(destroyed_)
        {
        }

        ~OwnerProbe()
        {
            destroyed->store(true);
        }

        std::atomic<bool>* destroyed;
    };

    std::shared_ptr<MSWindowsClipboardWorkerLifetime> lifetime(
        new MSWindowsClipboardWorkerLifetime());
    const std::uint64_t notificationToken = lifetime->notificationToken();
    std::atomic<bool> ownerDestroyed(false);
    std::atomic<bool> ownerTouched(false);
    std::shared_ptr<OwnerProbe> owner(new OwnerProbe(&ownerDestroyed));
    const std::weak_ptr<OwnerProbe> weakOwner(owner);
    std::mutex publicationMutex;
    std::condition_variable publicationChanged;
    bool publicationEntered = false;
    bool publicationReleased = false;

    std::thread worker([lifetime, notificationToken, weakOwner,
                        &ownerTouched, &publicationMutex,
                        &publicationChanged, &publicationEntered,
                        &publicationReleased]() {
        {
            std::unique_lock<std::mutex> lock(publicationMutex);
            publicationEntered = true;
            publicationChanged.notify_all();
            publicationChanged.wait(lock, [&publicationReleased]() {
                return publicationReleased;
            });
        }

        if (lifetime->acceptsNotification(notificationToken)) {
            const std::shared_ptr<OwnerProbe> lockedOwner = weakOwner.lock();
            if (lockedOwner) {
                ownerTouched.store(true);
            }
        }
        lifetime->markFinished();
    });

    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(publicationMutex);
        entered = publicationChanged.wait_for(
            lock, std::chrono::seconds(1), [&publicationEntered]() {
                return publicationEntered;
            });
    }
    EXPECT_TRUE(entered);
    if (!entered) {
        {
            std::lock_guard<std::mutex> lock(publicationMutex);
            publicationReleased = true;
        }
        publicationChanged.notify_all();
        worker.join();
        return;
    }

    lifetime->requestStop();
    EXPECT_FALSE(lifetime->waitForFinished(std::chrono::milliseconds(20)));
    owner.reset();
    EXPECT_TRUE(ownerDestroyed.load());
    EXPECT_TRUE(weakOwner.expired());

    {
        std::lock_guard<std::mutex> lock(publicationMutex);
        publicationReleased = true;
    }
    publicationChanged.notify_all();
    worker.join();

    EXPECT_FALSE(ownerTouched.load());
}
