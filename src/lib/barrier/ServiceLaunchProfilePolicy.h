/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

enum class ServiceLaunchRole {
    kClient,
    kServer
};

struct ServiceLaunchFileRule {
    const char* relativePath;
    std::uint64_t maximumBytes;
    bool required;
    bool fingerprintDatabase;
    bool allowEmpty;
};

const ServiceLaunchFileRule* serviceLaunchFileRules(
    ServiceLaunchRole role, std::size_t& count);
const ServiceLaunchFileRule* findServiceLaunchFileRule(
    ServiceLaunchRole role, const std::string& relativePath);
bool isServiceLaunchFileSizeAllowed(const ServiceLaunchFileRule& rule,
                                    std::uint64_t size);
bool validateServiceFingerprintDatabase(const std::string& contents);
bool validateServiceLaunchPem(const std::string& contents);
bool validateServiceLaunchServerConfig(const std::string& contents);
const char* serviceLaunchRoleName(ServiceLaunchRole role);
