/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "barrier/ServiceLaunchProfilePolicy.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <cstdint>
#include <string>

enum class ServiceLaunchProfileError {
    kNone,
    kInvalidArgument,
    kIdentityMismatch,
    kKnownFolder,
    kImpersonation,
    kUnsafeSource,
    kMissingRequiredFile,
    kFileSize,
    kRead,
    kValidation,
    kDigest,
    kDestination,
    kWrite,
    kCommit
};

struct ServiceLaunchProfileResult {
    ServiceLaunchProfileError error = ServiceLaunchProfileError::kNone;
    std::uint32_t systemError = 0;
    std::string detail;
    std::wstring profilePath;
    std::string generation;
    std::string digest;
    std::string ownerSid;
    bool newlyStaged = false;

    bool success() const
    {
        return error == ServiceLaunchProfileError::kNone;
    }
};

// The source path is intentionally not an argument. It is always resolved as
// FOLDERID_LocalAppData/Barrier for authenticatedUserToken.
ServiceLaunchProfileResult stageWindowsServiceLaunchProfile(
    HANDLE authenticatedUserToken,
    const std::string& authenticatedUserSid,
    ServiceLaunchRole role,
    const std::string& reusableGeneration = std::string(),
    const std::string& reusableDigest = std::string());

// Removes a generation that was staged but never committed to the registry.
// The destination is re-derived from the authenticated owner and generation;
// the caller-supplied path is accepted only when it exactly matches.
bool discardWindowsServiceLaunchProfile(
    const ServiceLaunchProfileResult& stagedProfile);

// Loads only the immutable generation named by the registry transaction. The
// path is derived from FOLDERID_ProgramData and none of its components are
// supplied by the caller.
ServiceLaunchProfileResult loadWindowsServiceLaunchProfile(
    const std::string& ownerSid,
    const std::string& generation,
    const std::string& digest,
    ServiceLaunchRole role);
