/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ipc/IpcCommandValidator.h"

#include <algorithm>
#include <cctype>

namespace {

bool isSpace(char ch)
{
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

std::string trim(const std::string& value)
{
    const auto begin = std::find_if_not(value.begin(), value.end(), isSpace);
    const auto end = std::find_if_not(value.rbegin(), value.rend(), isSpace).base();
    return begin < end ? std::string(begin, end) : std::string();
}

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool extractExecutableToken(const std::string& command,
                            std::string& token,
                            std::string* reason)
{
    if (command.find('\0') != std::string::npos) {
        if (reason != nullptr) {
            *reason = "command contains embedded NUL";
        }
        return false;
    }

    const std::string trimmed = trim(command);
    if (trimmed.empty() || trimmed == "\"\"") {
        token.clear();
        return true;
    }

    if (trimmed[0] == '"') {
        const std::string::size_type close = trimmed.find('"', 1);
        if (close == std::string::npos) {
            if (reason != nullptr) {
                *reason = "quoted command is unterminated";
            }
            return false;
        }

        token = trimmed.substr(1, close - 1);
        if (token.empty()) {
            token.clear();
            return true;
        }
        return true;
    }

    const std::string::size_type end = trimmed.find_first_of(" \t\r\n");
    token = trimmed.substr(0, end);
    return true;
}

std::string basename(std::string token)
{
    const std::string::size_type slash = token.find_last_of("/\\");
    if (slash != std::string::npos) {
        token.erase(0, slash + 1);
    }
    return lowerAscii(token);
}

IpcCommandValidator::CommandRole classifyBasename(const std::string& name)
{
    if (name == "weaves" || name == "weaves.exe" ||
        name == "barriers" || name == "barriers.exe") {
        return IpcCommandValidator::CommandRole::kServer;
    }

    if (name == "weavec" || name == "weavec.exe" ||
        name == "barrierc" || name == "barrierc.exe") {
        return IpcCommandValidator::CommandRole::kClient;
    }

    return IpcCommandValidator::CommandRole::kInvalid;
}

} // namespace

namespace IpcCommandValidator {

CommandRole classifyDaemonCommand(const std::string& command,
                                  std::string* reason)
{
    std::string token;
    if (!extractExecutableToken(command, token, reason)) {
        return CommandRole::kInvalid;
    }

    if (token.empty()) {
        return CommandRole::kEmpty;
    }

    const std::string name = basename(token);
    const CommandRole role = classifyBasename(name);
    if (role == CommandRole::kInvalid && reason != nullptr) {
        *reason = "command executable is not a weave server/client";
    }
    return role;
}

bool isAllowedDaemonCommand(const std::string& command,
                            std::string* reason)
{
    return classifyDaemonCommand(command, reason) != CommandRole::kInvalid;
}

bool isServerCommand(const std::string& command)
{
    return classifyDaemonCommand(command) == CommandRole::kServer;
}

} // namespace IpcCommandValidator
