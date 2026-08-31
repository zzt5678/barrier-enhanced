/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <cstdint>
#include <string>

enum class ServiceDataDirectoryError {
    kNone,
    kInvalidPath,
    kSecurityDescriptor,
    kPrivilege,
    kCreate,
    kOpen,
    kUnsafeObject,
    kApplySecurity,
    kVerifySecurity
};

struct ServiceDataDirectoryResult {
    ServiceDataDirectoryError error = ServiceDataDirectoryError::kNone;
    std::uint32_t systemError = 0;

    bool success() const
    {
        return error == ServiceDataDirectoryError::kNone;
    }
};

// The service data path crosses process/configuration boundaries as UTF-8.
// Conversion is strict so an invalid sequence can never select another path.
ServiceDataDirectoryResult ensureProtectedServiceDataDirectory(
    const std::string& path);
