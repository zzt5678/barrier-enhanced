/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#include "barrier/ServiceLaunchState.h"

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <vector>

namespace {

const char kHeader[] = "WEAVE-SERVICE-LAUNCH 1";
const char kCandidateHeader[] = "WEAVE-SERVICE-PENDING 1";
const std::size_t kMaximumEncodedBytes = 128u * 1024u;
const std::size_t kMaximumCandidateBytes = 256u * 1024u;
const std::size_t kMaximumCommandBytes = 32767u;

void setError(std::string* error, const char* message)
{
    if (error != nullptr) {
        *error = message;
    }
}

void clearError(std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
}

bool validShape(const ServiceLaunchState& state, std::string* error)
{
    if (state.command.size() > kMaximumCommandBytes ||
        state.logLevel.size() > 64u || state.ownerSid.size() > 256u ||
        state.generation.size() > 128u || state.digest.size() > 128u ||
        state.elevateMode > 2u) {
        setError(error, "service launch state field exceeds its limit");
        return false;
    }

    const bool hasProfile = state.sessionId != 0 || !state.ownerSid.empty() ||
        !state.generation.empty() || !state.digest.empty();
    if (state.command.empty()) {
        if (hasProfile) {
            setError(error, "stopped service state must not retain a launch profile");
            return false;
        }
        return true;
    }
    if (state.sessionId == 0 || state.ownerSid.empty() ||
        state.generation.empty() || state.digest.empty()) {
        setError(error, "running service state requires a complete launch profile");
        return false;
    }
    return true;
}

std::string hexEncode(const std::string& value)
{
    static const char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2u);
    for (std::string::const_iterator i = value.begin(); i != value.end(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(*i);
        encoded.push_back(digits[byte >> 4]);
        encoded.push_back(digits[byte & 0x0f]);
    }
    return encoded;
}

int hexDigit(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

bool hexDecode(const std::string& encoded, std::string& value)
{
    if ((encoded.size() & 1u) != 0u) {
        return false;
    }
    value.clear();
    value.reserve(encoded.size() / 2u);
    for (std::size_t i = 0; i < encoded.size(); i += 2u) {
        const int high = hexDigit(encoded[i]);
        const int low = hexDigit(encoded[i + 1u]);
        if (high < 0 || low < 0) {
            value.clear();
            return false;
        }
        value.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

bool field(const std::string& line, const char* name, std::string& value)
{
    const std::string prefix = std::string(name) + "=";
    if (line.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    return hexDecode(line.substr(prefix.size()), value);
}

} // namespace

bool serializeServiceLaunchState(const ServiceLaunchState& state,
                                 std::string& encoded,
                                 std::string* error)
{
    encoded.clear();
    clearError(error);
    if (!validShape(state, error)) {
        return false;
    }

    std::ostringstream output;
    output << kHeader << "\n"
           << "command=" << hexEncode(state.command) << "\n"
           << "elevate=" << static_cast<unsigned int>(state.elevateMode) << "\n"
           << "log=" << hexEncode(state.logLevel) << "\n"
           << "session=" << state.sessionId << "\n"
           << "owner=" << hexEncode(state.ownerSid) << "\n"
           << "generation=" << hexEncode(state.generation) << "\n"
           << "digest=" << hexEncode(state.digest) << "\n";
    encoded = output.str();
    if (encoded.size() > kMaximumEncodedBytes) {
        encoded.clear();
        setError(error, "encoded service launch state exceeds its limit");
        return false;
    }
    return true;
}

bool parseServiceLaunchState(const std::string& encoded,
                             ServiceLaunchState& state,
                             std::string* error)
{
    state = ServiceLaunchState();
    clearError(error);
    if (encoded.empty() || encoded.size() > kMaximumEncodedBytes ||
        encoded.find('\r') != std::string::npos) {
        setError(error, "service launch state size or line endings are invalid");
        return false;
    }

    std::istringstream input(encoded);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    if (lines.size() != 8u || lines[0] != kHeader) {
        setError(error, "service launch state header or field count is invalid");
        return false;
    }

    std::string elevate;
    std::string session;
    if (!field(lines[1], "command", state.command) ||
        lines[2].compare(0, 8u, "elevate=") != 0 ||
        !field(lines[3], "log", state.logLevel) ||
        lines[4].compare(0, 8u, "session=") != 0 ||
        !field(lines[5], "owner", state.ownerSid) ||
        !field(lines[6], "generation", state.generation) ||
        !field(lines[7], "digest", state.digest)) {
        setError(error, "service launch state fields are malformed or reordered");
        state = ServiceLaunchState();
        return false;
    }
    elevate = lines[2].substr(8u);
    session = lines[4].substr(8u);

    char* end = nullptr;
    errno = 0;
    const unsigned long elevateValue = std::strtoul(elevate.c_str(), &end, 10);
    if (errno != 0 || end == elevate.c_str() || *end != '\0' ||
        elevateValue > 2u) {
        setError(error, "service launch elevation is invalid");
        state = ServiceLaunchState();
        return false;
    }
    state.elevateMode = static_cast<std::uint8_t>(elevateValue);

    end = nullptr;
    errno = 0;
    const unsigned long long sessionValue =
        std::strtoull(session.c_str(), &end, 10);
    if (errno != 0 || end == session.c_str() || *end != '\0' ||
        sessionValue > (std::numeric_limits<std::uint32_t>::max)()) {
        setError(error, "service launch session is invalid");
        state = ServiceLaunchState();
        return false;
    }
    state.sessionId = static_cast<std::uint32_t>(sessionValue);

    if (!validShape(state, error)) {
        state = ServiceLaunchState();
        return false;
    }
    return true;
}

bool serializeServiceLaunchCandidate(const ServiceLaunchCandidate& candidate,
                                     std::string& encoded,
                                     std::string* error)
{
    encoded.clear();
    clearError(error);
    if (candidate.revision == 0) {
        setError(error, "service launch candidate revision must be non-zero");
        return false;
    }
    if (candidate.state.command.empty()) {
        setError(error, "service launch candidate must describe a running state");
        return false;
    }

    std::string encodedState;
    if (!serializeServiceLaunchState(candidate.state, encodedState, error)) {
        return false;
    }

    std::ostringstream output;
    output << kCandidateHeader << "\n"
           << "revision=" << candidate.revision << "\n"
           << "state=" << hexEncode(encodedState) << "\n";
    encoded = output.str();
    if (encoded.size() > kMaximumCandidateBytes) {
        encoded.clear();
        setError(error, "encoded service launch candidate exceeds its limit");
        return false;
    }
    return true;
}

bool parseServiceLaunchCandidate(const std::string& encoded,
                                 ServiceLaunchCandidate& candidate,
                                 std::string* error)
{
    candidate = ServiceLaunchCandidate();
    clearError(error);
    if (encoded.empty() || encoded.size() > kMaximumCandidateBytes ||
        encoded.find('\r') != std::string::npos) {
        setError(error, "service launch candidate size or line endings are invalid");
        return false;
    }

    std::istringstream input(encoded);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    if (lines.size() != 3u || lines[0] != kCandidateHeader ||
        lines[1].compare(0, 9u, "revision=") != 0) {
        setError(error, "service launch candidate header or fields are invalid");
        return false;
    }

    char* end = nullptr;
    errno = 0;
    const std::string revision = lines[1].substr(9u);
    const unsigned long long revisionValue =
        std::strtoull(revision.c_str(), &end, 10);
    if (errno != 0 || end == revision.c_str() || *end != '\0' ||
        revisionValue == 0) {
        setError(error, "service launch candidate revision is invalid");
        return false;
    }

    std::string encodedState;
    if (!field(lines[2], "state", encodedState) ||
        !parseServiceLaunchState(encodedState, candidate.state, error)) {
        if (error != nullptr && error->empty()) {
            setError(error, "service launch candidate state is invalid");
        }
        candidate = ServiceLaunchCandidate();
        return false;
    }
    if (candidate.state.command.empty()) {
        setError(error, "service launch candidate must describe a running state");
        candidate = ServiceLaunchCandidate();
        return false;
    }
    candidate.revision = static_cast<std::uint64_t>(revisionValue);
    return true;
}

bool sameServiceLaunchState(const ServiceLaunchState& left,
                            const ServiceLaunchState& right)
{
    return left.command == right.command &&
        left.elevateMode == right.elevateMode &&
        left.logLevel == right.logLevel &&
        left.sessionId == right.sessionId &&
        left.ownerSid == right.ownerSid &&
        left.generation == right.generation &&
        left.digest == right.digest;
}

bool sameServiceLaunchCandidate(const ServiceLaunchCandidate& left,
                                const ServiceLaunchCandidate& right)
{
    return left.revision == right.revision &&
        sameServiceLaunchState(left.state, right.state);
}

bool serviceLaunchCandidateMatchesCommandProfile(
    const ServiceLaunchCandidate& candidate,
    const std::string& command,
    std::uint8_t elevateMode,
    std::uint32_t sessionId,
    const std::string& ownerSid,
    const std::string& generation,
    const std::string& digest)
{
    return candidate.revision != 0 &&
        candidate.state.command == command &&
        candidate.state.elevateMode == elevateMode &&
        candidate.state.sessionId == sessionId &&
        candidate.state.ownerSid == ownerSid &&
        candidate.state.generation == generation &&
        candidate.state.digest == digest;
}

bool serviceLaunchInputCapabilityReady(bool secureDesktop,
                                       bool uiAccessEnabled)
{
    return !secureDesktop || uiAccessEnabled;
}

bool serviceNodeInputReadinessReady(bool uiAccessRequired,
                                    bool backendReady,
                                    bool uiAccessQuerySucceeded,
                                    bool uiAccessEnabled)
{
    if (!backendReady) {
        return false;
    }
    return !uiAccessRequired ||
        (uiAccessQuerySucceeded && uiAccessEnabled);
}

bool serviceStandbyUsesPassiveInputProbe(bool serviceStandby,
                                         bool serviceActivated)
{
    return serviceStandby && !serviceActivated;
}

ServiceLaunchOwnershipDecision decideServiceLaunchOwnership(
    ServiceLaunchCommitResult commitResult,
    bool previousOwnerFenced)
{
    if (commitResult == ServiceLaunchCommitResult::kIndeterminate) {
        return ServiceLaunchOwnershipDecision::kFailFast;
    }
    if (commitResult != ServiceLaunchCommitResult::kCommitted) {
        return ServiceLaunchOwnershipDecision::kKeepPrevious;
    }
    return previousOwnerFenced
        ? ServiceLaunchOwnershipDecision::kPublishCandidate
        : ServiceLaunchOwnershipDecision::kFailFast;
}

bool recoverServiceLaunchCurrent(
    const std::string& encodedCurrent,
    const std::string& encodedPending,
    ServiceLaunchState& current,
    ServiceLaunchRecoveryStatus& status,
    std::string* error)
{
    current = ServiceLaunchState();
    status = ServiceLaunchRecoveryStatus::kCurrentOnly;
    clearError(error);
    if (!parseServiceLaunchState(encodedCurrent, current, error)) {
        return false;
    }
    if (encodedPending.empty()) {
        return true;
    }

    ServiceLaunchCandidate ignored;
    std::string pendingError;
    if (parseServiceLaunchCandidate(encodedPending, ignored, &pendingError)) {
        status = ServiceLaunchRecoveryStatus::kDiscardedPending;
    }
    else {
        status = ServiceLaunchRecoveryStatus::kDiscardedMalformedPending;
        if (error != nullptr) {
            *error = pendingError;
        }
    }
    return true;
}

ServiceLaunchPromotionStatus matchServiceLaunchPromotion(
    const std::string& encodedPending,
    const ServiceLaunchCandidate& readyCandidate,
    ServiceLaunchState& matchedState,
    std::string* error)
{
    matchedState = ServiceLaunchState();
    clearError(error);
    if (encodedPending.empty()) {
        setError(error, "service launch pending candidate is missing");
        return ServiceLaunchPromotionStatus::kMissingPending;
    }

    ServiceLaunchCandidate persisted;
    if (!parseServiceLaunchCandidate(encodedPending, persisted, error)) {
        return ServiceLaunchPromotionStatus::kMalformedPending;
    }
    if (persisted.revision != readyCandidate.revision) {
        setError(error, "service launch readiness revision is stale");
        return ServiceLaunchPromotionStatus::kStaleRevision;
    }
    if (!sameServiceLaunchCandidate(persisted, readyCandidate)) {
        setError(error, "service launch readiness identity does not match pending");
        return ServiceLaunchPromotionStatus::kIdentityMismatch;
    }

    matchedState = persisted.state;
    return ServiceLaunchPromotionStatus::kMatched;
}
