/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <string>

namespace IpcCommandValidator {

enum class CommandRole {
    kEmpty,
    kServer,
    kClient,
    kInvalid
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

} // namespace IpcCommandValidator
