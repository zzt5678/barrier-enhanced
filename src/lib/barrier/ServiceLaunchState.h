/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#pragma once

#include <cstdint>
#include <string>

struct ServiceLaunchState {
    std::string command;
    std::uint8_t elevateMode = 0;
    std::string logLevel;
    std::uint32_t sessionId = 0;
    std::string ownerSid;
    std::string generation;
    std::string digest;
};

struct ServiceLaunchCandidate {
    std::uint64_t revision = 0;
    ServiceLaunchState state;
};

enum class ServiceLaunchRecoveryStatus {
    kCurrentOnly,
    kDiscardedPending,
    kDiscardedMalformedPending
};

enum class ServiceLaunchPromotionStatus {
    kMatched,
    kMissingPending,
    kMalformedPending,
    kStaleRevision,
    kIdentityMismatch
};

enum class ServiceLaunchCommitResult {
    kCommitted,
    kRejected,
    kTimedOut,
    kIndeterminate
};

enum class ServiceLaunchOwnershipDecision {
    kKeepPrevious,
    kPublishCandidate,
    kFailFast
};

bool serializeServiceLaunchState(const ServiceLaunchState& state,
                                 std::string& encoded,
                                 std::string* error = nullptr);
bool parseServiceLaunchState(const std::string& encoded,
                             ServiceLaunchState& state,
                             std::string* error = nullptr);

bool serializeServiceLaunchCandidate(const ServiceLaunchCandidate& candidate,
                                     std::string& encoded,
                                     std::string* error = nullptr);
bool parseServiceLaunchCandidate(const std::string& encoded,
                                 ServiceLaunchCandidate& candidate,
                                 std::string* error = nullptr);

bool sameServiceLaunchState(const ServiceLaunchState& left,
                            const ServiceLaunchState& right);
bool sameServiceLaunchCandidate(const ServiceLaunchCandidate& left,
                                const ServiceLaunchCandidate& right);
bool serviceLaunchCandidateMatchesCommandProfile(
    const ServiceLaunchCandidate& candidate,
    const std::string& command,
    std::uint8_t elevateMode,
    std::uint32_t sessionId,
    const std::string& ownerSid,
    const std::string& generation,
    const std::string& digest);
bool serviceLaunchInputCapabilityReady(bool secureDesktop,
                                       bool uiAccessEnabled);
bool serviceInputDesktopRequiresUiAccess(const std::string& desktopName);
bool serviceNodeInputReadinessReady(bool uiAccessRequired,
                                    bool backendReady,
                                    bool uiAccessQuerySucceeded,
                                    bool uiAccessEnabled);
bool serviceStandbyUsesPassiveInputProbe(bool serviceStandby,
                                         bool serviceActivated);
ServiceLaunchOwnershipDecision decideServiceLaunchOwnership(
    ServiceLaunchCommitResult commitResult,
    bool previousOwnerFenced);

// A restart always resumes Current. Pending is only a candidate from an
// unproven launch and is discarded even when it is well formed.
bool recoverServiceLaunchCurrent(
    const std::string& encodedCurrent,
    const std::string& encodedPending,
    ServiceLaunchState& current,
    ServiceLaunchRecoveryStatus& status,
    std::string* error = nullptr);

// Matches a watchdog readiness proof against the latest persisted candidate.
// The caller may promote matchedState to Current, then clear Pending.
ServiceLaunchPromotionStatus matchServiceLaunchPromotion(
    const std::string& encodedPending,
    const ServiceLaunchCandidate& readyCandidate,
    ServiceLaunchState& matchedState,
    std::string* error = nullptr);
