/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ipc/ElevationPolicy.h"

#include "ipc/IpcMessage.h"

#include <cerrno>
#include <climits>
#include <cctype>
#include <cstdlib>

namespace {

bool parseModeSetting(const std::string& setting, int& mode)
{
    const char* begin = setting.c_str();
    char* end = NULL;
    errno = 0;
    const long value = std::strtol(begin, &end, 10);
    if (begin == end || errno == ERANGE || value < INT_MIN || value > INT_MAX) {
        return false;
    }

    while (*end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end))) {
            return false;
        }
        ++end;
    }

    mode = static_cast<int>(value);
    return true;
}

}

namespace ElevationPolicy {

UInt8 normalizeMode(int mode)
{
    if (mode < IpcCommandMessage::kElevateAsNeeded ||
        mode > IpcCommandMessage::kElevateNever) {
        return IpcCommandMessage::kElevateAsNeeded;
    }
    return static_cast<UInt8>(mode);
}

UInt8 modeFromSettings(const std::string& elevateModeSetting,
                       const std::string& legacyElevateSetting)
{
    if (!elevateModeSetting.empty()) {
        int mode = IpcCommandMessage::kElevateAsNeeded;
        return parseModeSetting(elevateModeSetting, mode) ?
            normalizeMode(mode) :
            IpcCommandMessage::kElevateAsNeeded;
    }

    return legacyElevateSetting == "1" ?
        IpcCommandMessage::kElevateAlways :
        IpcCommandMessage::kElevateAsNeeded;
}

bool shouldElevateProcess(UInt8 mode)
{
    return normalizeMode(mode) == IpcCommandMessage::kElevateAlways;
}

bool shouldAutoElevate(UInt8 mode, const std::string& desktopName)
{
    return normalizeMode(mode) != IpcCommandMessage::kElevateNever &&
        !desktopName.empty() &&
        desktopName != "Default";
}

bool shouldRelaunchOnDesktopSwitch(UInt8 mode)
{
    return normalizeMode(mode) == IpcCommandMessage::kElevateAsNeeded;
}

bool commandRequiresRelaunch(const std::string& currentCommand, UInt8 currentMode,
                             const std::string& requestedCommand, UInt8 requestedMode)
{
    return currentCommand != requestedCommand ||
        normalizeMode(currentMode) != normalizeMode(requestedMode);
}

} // namespace ElevationPolicy
