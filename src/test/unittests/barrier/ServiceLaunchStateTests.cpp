/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#include "barrier/ServiceLaunchState.h"

#include "test/global/gtest.h"

namespace {

ServiceLaunchState runningState(const std::string& command,
                                const std::string& generation)
{
    ServiceLaunchState state;
    state.command = command;
    state.elevateMode = 1;
    state.logLevel = "INFO";
    state.sessionId = 7;
    state.ownerSid = "S-1-5-21-1-2-3-4";
    state.generation = generation;
    state.digest = "sha256:0123456789abcdef0123456789abcdef"
                   "0123456789abcdef0123456789abcdef";
    return state;
}

std::string encodeState(const ServiceLaunchState& state)
{
    std::string encoded;
    EXPECT_TRUE(serializeServiceLaunchState(state, encoded));
    return encoded;
}

std::string encodeCandidate(const ServiceLaunchCandidate& candidate)
{
    std::string encoded;
    EXPECT_TRUE(serializeServiceLaunchCandidate(candidate, encoded));
    return encoded;
}

} // namespace

TEST(ServiceLaunchStateTests, roundTripsOneAtomicRegistryValue)
{
    ServiceLaunchState source;
    source.command = "\"C:\\Program Files\\Weave\\weaves.exe\" --name a=b\nline";
    source.elevateMode = 2;
    source.logLevel = "INFO";
    source.sessionId = 7;
    source.ownerSid = "S-1-5-21-1-2-3-4";
    source.generation = "v1-0123456789abcdef0123456789abcdef";
    source.digest = "sha256:0123456789abcdef0123456789abcdef"
                    "0123456789abcdef0123456789abcdef";

    std::string encoded;
    ASSERT_TRUE(serializeServiceLaunchState(source, encoded));

    ServiceLaunchState parsed;
    ASSERT_TRUE(parseServiceLaunchState(encoded, parsed));
    EXPECT_EQ(source.command, parsed.command);
    EXPECT_EQ(source.elevateMode, parsed.elevateMode);
    EXPECT_EQ(source.logLevel, parsed.logLevel);
    EXPECT_EQ(source.sessionId, parsed.sessionId);
    EXPECT_EQ(source.ownerSid, parsed.ownerSid);
    EXPECT_EQ(source.generation, parsed.generation);
    EXPECT_EQ(source.digest, parsed.digest);
}

TEST(ServiceLaunchStateTests, stoppedStateClearsProfileInSameCommit)
{
    ServiceLaunchState stopped;
    std::string encoded;
    ASSERT_TRUE(serializeServiceLaunchState(stopped, encoded));

    ServiceLaunchState parsed;
    ASSERT_TRUE(parseServiceLaunchState(encoded, parsed));
    EXPECT_TRUE(parsed.command.empty());
    EXPECT_EQ(0u, parsed.sessionId);
    EXPECT_TRUE(parsed.ownerSid.empty());
    EXPECT_TRUE(parsed.generation.empty());
    EXPECT_TRUE(parsed.digest.empty());
}

TEST(ServiceLaunchStateTests, refusesPartialRunningProfile)
{
    ServiceLaunchState partial;
    partial.command = "weavec.exe";
    partial.sessionId = 4;

    std::string encoded;
    std::string error;
    EXPECT_FALSE(serializeServiceLaunchState(partial, encoded, &error));
    EXPECT_TRUE(encoded.empty());
    EXPECT_FALSE(error.empty());
}

TEST(ServiceLaunchStateTests, refusesMalformedOrExtendedRecords)
{
    ServiceLaunchState state;
    EXPECT_FALSE(parseServiceLaunchState("WEAVE-SERVICE-LAUNCH 1\n", state));
    EXPECT_FALSE(parseServiceLaunchState(
        "WEAVE-SERVICE-LAUNCH 1\n"
        "command=0g\n"
        "elevate=0\n"
        "log=\n"
        "session=0\n"
        "owner=\n"
        "generation=\n"
        "digest=\n", state));
    EXPECT_FALSE(parseServiceLaunchState(
        "WEAVE-SERVICE-LAUNCH 1\n"
        "command=\n"
        "elevate=0\n"
        "log=\n"
        "session=0\n"
        "owner=\n"
        "generation=\n"
        "digest=\n"
        "extra=\n", state));
}

TEST(ServiceLaunchStateTests, refusesProfileOnStoppedState)
{
    ServiceLaunchState stopped;
    stopped.ownerSid = "S-1-5-18";
    std::string encoded;
    EXPECT_FALSE(serializeServiceLaunchState(stopped, encoded));
}

TEST(ServiceLaunchStateTests, candidateRoundTripsIndependentRevision)
{
    ServiceLaunchCandidate source;
    source.revision = 42;
    source.state = runningState("weavec.exe --name alpha", "profile-a");

    ServiceLaunchCandidate parsed;
    ASSERT_TRUE(parseServiceLaunchCandidate(encodeCandidate(source), parsed));
    EXPECT_TRUE(sameServiceLaunchCandidate(source, parsed));
}

TEST(ServiceLaunchStateTests, candidateRefusesStoppedStateAndZeroRevision)
{
    ServiceLaunchCandidate candidate;
    std::string encoded;
    EXPECT_FALSE(serializeServiceLaunchCandidate(candidate, encoded));

    candidate.state = runningState("weavec.exe --name alpha", "profile-a");
    EXPECT_FALSE(serializeServiceLaunchCandidate(candidate, encoded));

    candidate.revision = 1;
    candidate.state = ServiceLaunchState();
    EXPECT_FALSE(serializeServiceLaunchCandidate(candidate, encoded));
}

TEST(ServiceLaunchStateTests, restartUsesCurrentAAndDiscardsPendingB)
{
    const ServiceLaunchState currentA =
        runningState("weavec.exe --name alpha", "profile-a");
    ServiceLaunchCandidate pendingB;
    pendingB.revision = 8;
    pendingB.state = runningState("weavec.exe --name beta", "profile-b");

    ServiceLaunchState recovered;
    ServiceLaunchRecoveryStatus status =
        ServiceLaunchRecoveryStatus::kCurrentOnly;
    ASSERT_TRUE(recoverServiceLaunchCurrent(
        encodeState(currentA), encodeCandidate(pendingB), recovered, status));
    EXPECT_TRUE(sameServiceLaunchState(currentA, recovered));
    EXPECT_EQ(ServiceLaunchRecoveryStatus::kDiscardedPending, status);
}

TEST(ServiceLaunchStateTests, restartUsesCurrentBAndDiscardsStalePending)
{
    const ServiceLaunchState currentB =
        runningState("weavec.exe --name beta", "profile-b");
    ServiceLaunchCandidate stalePending;
    stalePending.revision = 7;
    stalePending.state = currentB;

    ServiceLaunchState recovered;
    ServiceLaunchRecoveryStatus status =
        ServiceLaunchRecoveryStatus::kCurrentOnly;
    ASSERT_TRUE(recoverServiceLaunchCurrent(
        encodeState(currentB), encodeCandidate(stalePending), recovered,
        status));
    EXPECT_TRUE(sameServiceLaunchState(currentB, recovered));
    EXPECT_EQ(ServiceLaunchRecoveryStatus::kDiscardedPending, status);
}

TEST(ServiceLaunchStateTests, staleReadyRevisionCannotPromote)
{
    ServiceLaunchCandidate pending;
    pending.revision = 12;
    pending.state = runningState("weavec.exe --name beta", "profile-b");
    ServiceLaunchCandidate staleReady = pending;
    staleReady.revision = 11;

    ServiceLaunchState matched;
    EXPECT_EQ(ServiceLaunchPromotionStatus::kStaleRevision,
              matchServiceLaunchPromotion(
                  encodeCandidate(pending), staleReady, matched));
    EXPECT_TRUE(matched.command.empty());
}

TEST(ServiceLaunchStateTests, malformedPendingDoesNotReplaceCurrent)
{
    const ServiceLaunchState current =
        runningState("weavec.exe --name alpha", "profile-a");
    ServiceLaunchState recovered;
    ServiceLaunchRecoveryStatus recoveryStatus =
        ServiceLaunchRecoveryStatus::kCurrentOnly;
    ASSERT_TRUE(recoverServiceLaunchCurrent(
        encodeState(current), "not-a-candidate", recovered, recoveryStatus));
    EXPECT_TRUE(sameServiceLaunchState(current, recovered));
    EXPECT_EQ(ServiceLaunchRecoveryStatus::kDiscardedMalformedPending,
              recoveryStatus);

    ServiceLaunchCandidate ready;
    ready.revision = 1;
    ready.state = current;
    ServiceLaunchState matched;
    EXPECT_EQ(ServiceLaunchPromotionStatus::kMalformedPending,
              matchServiceLaunchPromotion(
                  "not-a-candidate", ready, matched));
}

TEST(ServiceLaunchStateTests, sameProfileGenerationDifferentCommandIsNotIdentity)
{
    ServiceLaunchCandidate pending;
    pending.revision = 21;
    pending.state = runningState("weavec.exe --name alpha", "profile-shared");
    ServiceLaunchCandidate wrongCommand = pending;
    wrongCommand.state.command = "weavec.exe --name beta";

    ServiceLaunchState matched;
    EXPECT_EQ(ServiceLaunchPromotionStatus::kIdentityMismatch,
              matchServiceLaunchPromotion(
                  encodeCandidate(pending), wrongCommand, matched));
    EXPECT_FALSE(serviceLaunchCandidateMatchesCommandProfile(
        pending, wrongCommand.state.command, pending.state.elevateMode,
        pending.state.sessionId, pending.state.ownerSid,
        pending.state.generation, pending.state.digest));
    EXPECT_TRUE(serviceLaunchCandidateMatchesCommandProfile(
        pending, pending.state.command, pending.state.elevateMode,
        pending.state.sessionId, pending.state.ownerSid,
        pending.state.generation, pending.state.digest));
    EXPECT_FALSE(serviceLaunchCandidateMatchesCommandProfile(
        pending, pending.state.command, pending.state.elevateMode,
        pending.state.sessionId + 1, pending.state.ownerSid,
        pending.state.generation, pending.state.digest));
}

TEST(ServiceLaunchStateTests, durableCommitPrecedesOwnershipPublication)
{
    EXPECT_EQ(ServiceLaunchOwnershipDecision::kKeepPrevious,
              decideServiceLaunchOwnership(
                  ServiceLaunchCommitResult::kRejected, false));
    EXPECT_EQ(ServiceLaunchOwnershipDecision::kKeepPrevious,
              decideServiceLaunchOwnership(
                  ServiceLaunchCommitResult::kTimedOut, false));
    EXPECT_EQ(ServiceLaunchOwnershipDecision::kFailFast,
              decideServiceLaunchOwnership(
                  ServiceLaunchCommitResult::kIndeterminate, false));
    EXPECT_EQ(ServiceLaunchOwnershipDecision::kFailFast,
              decideServiceLaunchOwnership(
                  ServiceLaunchCommitResult::kIndeterminate, true));
    EXPECT_EQ(ServiceLaunchOwnershipDecision::kPublishCandidate,
              decideServiceLaunchOwnership(
                  ServiceLaunchCommitResult::kCommitted, true));
    EXPECT_EQ(ServiceLaunchOwnershipDecision::kFailFast,
              decideServiceLaunchOwnership(
                  ServiceLaunchCommitResult::kCommitted, false));
}

TEST(ServiceLaunchStateTests, secureDesktopRequiresUiAccessCapability)
{
    EXPECT_TRUE(serviceLaunchInputCapabilityReady(false, false));
    EXPECT_FALSE(serviceLaunchInputCapabilityReady(true, false));
    EXPECT_TRUE(serviceLaunchInputCapabilityReady(true, true));
}

TEST(ServiceLaunchStateTests, windowsNodeReadinessRequiresObservedUiAccess)
{
    EXPECT_FALSE(serviceNodeInputReadinessReady(true, false, true, true));
    EXPECT_FALSE(serviceNodeInputReadinessReady(true, true, false, true));
    EXPECT_FALSE(serviceNodeInputReadinessReady(true, true, true, false));
    EXPECT_TRUE(serviceNodeInputReadinessReady(true, true, true, true));
}

TEST(ServiceLaunchStateTests, nonWindowsNodeReadinessOnlyRequiresBackend)
{
    EXPECT_FALSE(serviceNodeInputReadinessReady(false, false, false, false));
    EXPECT_TRUE(serviceNodeInputReadinessReady(false, true, false, false));
}

TEST(ServiceLaunchStateTests, standbyUsesPassiveProbeUntilActivationIsCommitted)
{
    EXPECT_FALSE(serviceStandbyUsesPassiveInputProbe(false, false));
    EXPECT_FALSE(serviceStandbyUsesPassiveInputProbe(false, true));
    EXPECT_TRUE(serviceStandbyUsesPassiveInputProbe(true, false));
    EXPECT_FALSE(serviceStandbyUsesPassiveInputProbe(true, true));
}
