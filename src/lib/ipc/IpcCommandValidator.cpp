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
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <iterator>
#include <set>
#include <vector>

namespace {

bool isSpace(char ch)
{
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

void setReason(std::string* reason, const std::string& value)
{
    if (reason != nullptr) {
        *reason = value;
    }
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

bool tokenizeCommand(const std::string& command,
                     std::vector<std::string>& tokens,
                     std::string* reason)
{
    tokens.clear();
    if (command.size() > 32767) {
        setReason(reason, "command exceeds the Windows command-line limit");
        return false;
    }
    if (command.find('\0') != std::string::npos) {
        setReason(reason, "command contains embedded NUL");
        return false;
    }

    std::size_t cursor = 0;
    while (cursor < command.size()) {
        while (cursor < command.size() && isSpace(command[cursor])) {
            ++cursor;
        }
        if (cursor == command.size()) {
            break;
        }
        if (tokens.size() >= 128) {
            setReason(reason, "command contains too many arguments");
            return false;
        }

        std::string token;
        bool inQuotes = false;
        bool sawCharacter = false;
        while (cursor < command.size()) {
            if (!inQuotes && isSpace(command[cursor])) {
                break;
            }

            std::size_t backslashes = 0;
            while (cursor < command.size() && command[cursor] == '\\') {
                ++backslashes;
                ++cursor;
            }
            if (cursor < command.size() && command[cursor] == '"') {
                token.append(backslashes / 2, '\\');
                if ((backslashes % 2) != 0) {
                    token.push_back('"');
                }
                else {
                    inQuotes = !inQuotes;
                }
                sawCharacter = true;
                ++cursor;
                continue;
            }

            token.append(backslashes, '\\');
            if (cursor == command.size() || (!inQuotes && isSpace(command[cursor]))) {
                break;
            }
            const unsigned char ch = static_cast<unsigned char>(command[cursor]);
            if (ch < 0x20) {
                setReason(reason, "command contains an unsafe control character");
                return false;
            }
            token.push_back(command[cursor++]);
            sawCharacter = true;
            if (token.size() > 4096) {
                setReason(reason, "command argument exceeds the size limit");
                return false;
            }
        }
        if (inQuotes) {
            setReason(reason, "quoted command argument is unterminated");
            return false;
        }
        if (!sawCharacter && token.empty()) {
            setReason(reason, "command contains an empty unquoted argument");
            return false;
        }
        tokens.push_back(token);
    }
    return true;
}

std::string quoteArgument(const std::string& argument)
{
    if (!argument.empty() &&
        argument.find_first_of(" \t\r\n\"") == std::string::npos) {
        return argument;
    }

    std::string quoted("\"");
    std::size_t backslashes = 0;
    for (char ch : argument) {
        if (ch == '\\') {
            ++backslashes;
            continue;
        }
        if (ch == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
            quoted.push_back('"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, '\\');
        backslashes = 0;
        quoted.push_back(ch);
    }
    quoted.append(backslashes * 2, '\\');
    quoted.push_back('"');
    return quoted;
}

std::string assembleCommand(const std::vector<std::string>& arguments)
{
    std::string command;
    for (const std::string& argument : arguments) {
        if (!command.empty()) {
            command.push_back(' ');
        }
        command += quoteArgument(argument);
    }
    return command;
}

bool isLogLevel(const std::string& value)
{
    static const char* const levels[] = {
        "FATAL", "ERROR", "WARNING", "NOTE", "INFO", "DEBUG",
        "DEBUG1", "DEBUG2"
    };
    return std::find(std::begin(levels), std::end(levels), value) !=
        std::end(levels);
}

bool isScreenName(const std::string& value)
{
    if (value.empty() || value.size() > 255) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '.' || ch == '_' || ch == '-';
    });
}

bool isEndpoint(const std::string& value)
{
    if (value.empty() || value.size() > 1024 || value[0] == '-') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch >= 0x20 && ch != 0x7f && ch != '"';
    });
}

bool parseScrollAmount(const std::string& value, std::string& canonical)
{
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (value.empty() || end == value.c_str() || *end != '\0' ||
        errno == ERANGE || parsed < -32768 || parsed > 32767) {
        return false;
    }
    canonical = std::to_string(parsed);
    return true;
}

bool isPathOption(const std::string& option)
{
    return option == "-l" || option == "--log" ||
        option == "--drop-dir" || option == "--profile-dir" ||
        option == "--plugin-dir" || option == "-c" ||
        option == "--config" || option == "--screen-change-script";
}

bool takeValue(const std::vector<std::string>& tokens, std::size_t& index,
               const std::string& option, std::string& value,
               std::string* reason)
{
    if (index + 1 >= tokens.size()) {
        setReason(reason, "missing value for daemon option " + option);
        return false;
    }
    value = tokens[++index];
    return true;
}

void addFlag(std::vector<std::string>& arguments,
             std::set<std::string>& flags,
             const std::string& flag)
{
    if (flags.insert(flag).second) {
        arguments.push_back(flag);
    }
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

bool sanitizeDaemonRequest(const std::string& command,
                           std::uint8_t requestedElevateMode,
                           const std::string& trustedServerExecutable,
                           const std::string& trustedClientExecutable,
                           SanitizedDaemonRequest& sanitized,
                           std::string* reason)
{
    if (reason != nullptr) {
        reason->clear();
    }
    sanitized = SanitizedDaemonRequest();

    // Elevation mode 1 (Always) is intentionally not a GUI capability. The
    // watchdog may still obtain a privileged token for a secure desktop when
    // mode 0 (AsNeeded) requires it.
    sanitized.elevationDowngraded = requestedElevateMode == 1;
    sanitized.elevateMode = requestedElevateMode == 2 ? 2 : 0;

    std::vector<std::string> tokens;
    if (!tokenizeCommand(command, tokens, reason)) {
        return false;
    }
    if (tokens.empty() || (tokens.size() == 1 && tokens[0].empty())) {
        sanitized.role = CommandRole::kEmpty;
        return true;
    }

    const CommandRole role = classifyBasename(basename(tokens[0]));
    if (role != CommandRole::kServer && role != CommandRole::kClient) {
        setReason(reason, "command executable is not a weave server/client");
        return false;
    }
    const std::string& trustedExecutable = role == CommandRole::kServer
        ? trustedServerExecutable : trustedClientExecutable;
    if (!isSafeTrustedExecutable(trustedExecutable, role, reason)) {
        return false;
    }

    std::vector<std::string> safeArguments;
    std::set<std::string> flags;
    std::string clientEndpoint;
    safeArguments.push_back(trustedExecutable);

    for (std::size_t i = 1; i < tokens.size(); ++i) {
        const std::string& option = tokens[i];
        std::string value;
        if (isPathOption(option)) {
            if (!takeValue(tokens, i, option, value, reason)) {
                return false;
            }
            sanitized.ignoredUnsafeArguments = true;
            continue;
        }
        if (option == "--disable-crypto" ||
            option == "--disable-client-cert-checking") {
            sanitized.ignoredUnsafeArguments = true;
            continue;
        }
        if (option == "-d" || option == "--debug") {
            if (!takeValue(tokens, i, option, value, reason)) {
                return false;
            }
            if (!isLogLevel(value)) {
                setReason(reason, "daemon command has an invalid log level");
                return false;
            }
            if (flags.insert("--debug").second) {
                safeArguments.push_back("--debug");
                safeArguments.push_back(value);
            }
            continue;
        }
        if (option == "-n" || option == "--name") {
            if (!takeValue(tokens, i, option, value, reason)) {
                return false;
            }
            if (!isScreenName(value)) {
                setReason(reason, "daemon command has an invalid screen name");
                return false;
            }
            if (flags.insert("--name").second) {
                safeArguments.push_back("--name");
                safeArguments.push_back(value);
            }
            continue;
        }
        if (option == "-a" || option == "--address") {
            if (role != CommandRole::kServer) {
                setReason(reason, "client daemon command cannot set a listen address");
                return false;
            }
            if (!takeValue(tokens, i, option, value, reason)) {
                return false;
            }
            if (!isEndpoint(value)) {
                setReason(reason, "daemon command has an invalid listen address");
                return false;
            }
            if (flags.insert("--address").second) {
                safeArguments.push_back("--address");
                safeArguments.push_back(value);
            }
            continue;
        }
        if (option == "--yscroll") {
            if (role != CommandRole::kClient) {
                setReason(reason, "server daemon command cannot set client scrolling");
                return false;
            }
            if (!takeValue(tokens, i, option, value, reason)) {
                return false;
            }
            std::string canonical;
            if (!parseScrollAmount(value, canonical)) {
                setReason(reason, "daemon command has an invalid scroll amount");
                return false;
            }
            if (flags.insert("--yscroll").second) {
                safeArguments.push_back("--yscroll");
                safeArguments.push_back(canonical);
            }
            continue;
        }
        if (option == "-f" || option == "--no-daemon") {
            addFlag(safeArguments, flags, "--no-daemon");
            continue;
        }
        if (option == "-1" || option == "--no-restart") {
            addFlag(safeArguments, flags, "--no-restart");
            continue;
        }
        if (option == "--restart" || option == "--no-tray" ||
            option == "--ipc" || option == "--enable-drag-drop" ||
            option == "--game-mode" || option == "--low-latency-mode" ||
            option == "--nested-remote-mode" || option == "--enable-crypto") {
            addFlag(safeArguments, flags, option);
            continue;
        }
        if (option == "--stop-on-desk-switch") {
            // Reconstructed from the normalized elevation mode below.
            continue;
        }
        if ((option == "--camp" || option == "--no-camp") &&
            role == CommandRole::kClient) {
            addFlag(safeArguments, flags, option);
            continue;
        }
        if (!option.empty() && option[0] != '-' &&
            role == CommandRole::kClient && clientEndpoint.empty() &&
            isEndpoint(option)) {
            clientEndpoint = option;
            continue;
        }

        setReason(reason, "daemon command contains a disallowed argument: " + option);
        return false;
    }

    if (role == CommandRole::kClient && clientEndpoint.empty()) {
        setReason(reason, "client daemon command has no server address");
        return false;
    }
    addFlag(safeArguments, flags, "--no-daemon");
    addFlag(safeArguments, flags, "--no-tray");
    addFlag(safeArguments, flags, "--ipc");
    if (sanitized.elevateMode == 0) {
        addFlag(safeArguments, flags, "--stop-on-desk-switch");
    }
    if (!clientEndpoint.empty()) {
        safeArguments.push_back(clientEndpoint);
    }

    sanitized.role = role;
    sanitized.command = assembleCommand(safeArguments);
    return true;
}

bool restrictElevatedDesktopCommand(const std::string& command,
                                    std::string& restricted,
                                    std::string* reason)
{
    if (reason != nullptr) {
        reason->clear();
    }
    std::vector<std::string> tokens;
    if (!tokenizeCommand(command, tokens, reason) || tokens.empty()) {
        if (tokens.empty() && reason != nullptr && reason->empty()) {
            *reason = "elevated desktop command is empty";
        }
        return false;
    }

    std::vector<std::string> safeArguments;
    safeArguments.reserve(tokens.size());
    safeArguments.push_back(tokens[0]);
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        const std::string& option = tokens[i];
        if (isPathOption(option)) {
            std::string ignored;
            if (!takeValue(tokens, i, option, ignored, reason)) {
                return false;
            }
            continue;
        }
        if (option == "--enable-drag-drop") {
            continue;
        }
        safeArguments.push_back(option);
    }
    restricted = assembleCommand(safeArguments);
    return true;
}

bool appendTrustedProfileDirectory(const std::string& command,
                                   const std::string& profileDirectory,
                                   std::string& augmented,
                                   std::string* reason)
{
    if (reason != nullptr) {
        reason->clear();
    }

    std::vector<std::string> tokens;
    if (!tokenizeCommand(command, tokens, reason) || tokens.empty()) {
        if (tokens.empty() && reason != nullptr && reason->empty()) {
            *reason = "service launch command is empty";
        }
        return false;
    }

    if (profileDirectory.size() < 3u || profileDirectory.size() > 32767u ||
        !std::isalpha(static_cast<unsigned char>(profileDirectory[0])) ||
        profileDirectory[1] != ':' ||
        (profileDirectory[2] != '\\' && profileDirectory[2] != '/')) {
        setReason(reason, "trusted profile directory is not an absolute Windows path");
        return false;
    }
    for (char ch : profileDirectory) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (value < 0x20u || value == 0x7fu || ch == '"') {
            setReason(reason, "trusted profile directory contains an unsafe character");
            return false;
        }
    }

    std::size_t segmentStart = 3u;
    for (std::size_t i = segmentStart; i <= profileDirectory.size(); ++i) {
        if (i != profileDirectory.size() &&
            profileDirectory[i] != '\\' && profileDirectory[i] != '/') {
            continue;
        }
        const std::string segment = profileDirectory.substr(segmentStart,
                                                             i - segmentStart);
        if (segment == "." || segment == "..") {
            setReason(reason, "trusted profile directory contains a relative segment");
            return false;
        }
        segmentStart = i + 1u;
    }

    if (std::find(tokens.begin(), tokens.end(), "--profile-dir") != tokens.end()) {
        setReason(reason, "service launch command already contains --profile-dir");
        return false;
    }

    tokens.push_back("--profile-dir");
    tokens.push_back(profileDirectory);
    augmented = assembleCommand(tokens);
    if (augmented.size() > 32767u) {
        augmented.clear();
        setReason(reason, "service launch command exceeds the Windows command-line limit");
        return false;
    }
    return true;
}

} // namespace IpcCommandValidator
