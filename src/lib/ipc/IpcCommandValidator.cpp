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
                            std::string* arguments,
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
        if (arguments != nullptr) {
            arguments->clear();
        }
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

        if (close + 1 < trimmed.size() && !isSpace(trimmed[close + 1])) {
            if (reason != nullptr) {
                *reason = "quoted executable is not followed by whitespace";
            }
            return false;
        }

        token = trimmed.substr(1, close - 1);
        if (token.empty()) {
            token.clear();
            if (arguments != nullptr) {
                arguments->clear();
            }
            return true;
        }
        if (arguments != nullptr) {
            *arguments = trimmed.substr(close + 1);
        }
        return true;
    }

    const std::string::size_type end = trimmed.find_first_of(" \t\r\n");
    token = trimmed.substr(0, end);
    if (arguments != nullptr) {
        *arguments = end == std::string::npos ? std::string() : trimmed.substr(end);
    }
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

bool isSafeTrustedExecutable(const std::string& executable,
                             IpcCommandValidator::CommandRole expectedRole,
                             std::string* reason)
{
    if (executable.empty() || executable.find('\0') != std::string::npos ||
        executable.find('"') != std::string::npos) {
        if (reason != nullptr) {
            *reason = "trusted executable path is empty or contains an unsafe character";
        }
        return false;
    }

    if (classifyBasename(basename(executable)) != expectedRole) {
        if (reason != nullptr) {
            *reason = "trusted executable does not match the command role";
        }
        return false;
    }
    return true;
}

} // namespace

namespace IpcCommandValidator {

CommandRole classifyDaemonCommand(const std::string& command,
                                  std::string* reason)
{
    std::string token;
    if (!extractExecutableToken(command, token, nullptr, reason)) {
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

bool rewriteDaemonExecutable(const std::string& command,
                             const std::string& trustedServerExecutable,
                             const std::string& trustedClientExecutable,
                             std::string& rewritten,
                             std::string* reason)
{
    if (reason != nullptr) {
        reason->clear();
    }

    std::string token;
    std::string arguments;
    if (!extractExecutableToken(command, token, &arguments, reason)) {
        return false;
    }

    if (token.empty()) {
        rewritten.clear();
        return true;
    }

    const CommandRole role = classifyBasename(basename(token));
    if (role != CommandRole::kServer && role != CommandRole::kClient) {
        if (reason != nullptr) {
            *reason = "command executable is not a weave server/client";
        }
        return false;
    }

    const std::string& trustedExecutable =
        role == CommandRole::kServer ? trustedServerExecutable : trustedClientExecutable;
    if (!isSafeTrustedExecutable(trustedExecutable, role, reason)) {
        return false;
    }

    rewritten = "\"" + trustedExecutable + "\"" + arguments;
    return true;
}

} // namespace IpcCommandValidator
