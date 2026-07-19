/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <cstdint>
#include <string>

namespace IpcCommandValidator {

enum class CommandRole {
    kEmpty,
    kServer,
    kClient,
    kInvalid
};

struct SanitizedDaemonRequest {
    CommandRole role = CommandRole::kInvalid;
    std::string command;
    std::uint8_t elevateMode = 0;
    bool ignoredUnsafeArguments = false;
    bool elevationDowngraded = false;
};

CommandRole classifyDaemonCommand(const std::string& command,
                                  std::string* reason = nullptr);
bool isAllowedDaemonCommand(const std::string& command,
                            std::string* reason = nullptr);
bool isServerCommand(const std::string& command);
bool rewriteDaemonExecutable(const std::string& command,
                             const std::string& trustedServerExecutable,
                             const std::string& trustedClientExecutable,
                             std::string& rewritten,
                             std::string* reason = nullptr);
bool sanitizeDaemonRequest(const std::string& command,
                           std::uint8_t requestedElevateMode,
                           const std::string& trustedServerExecutable,
                           const std::string& trustedClientExecutable,
                           SanitizedDaemonRequest& sanitized,
                           std::string* reason = nullptr);
bool restrictElevatedDesktopCommand(const std::string& command,
                                    std::string& restricted,
                                    std::string* reason = nullptr);
bool appendTrustedProfileDirectory(const std::string& command,
                                   const std::string& profileDirectory,
                                   std::string& augmented,
                                   std::string* reason = nullptr);
bool deriveServiceStandbyCommand(const std::string& command,
                                 std::string& derived,
                                 std::string* reason = nullptr);

} // namespace IpcCommandValidator
