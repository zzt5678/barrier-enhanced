/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "common/basic_types.h"

#include <string>

namespace ElevationPolicy {

UInt8 normalizeMode(int mode);
UInt8 modeFromSettings(const std::string& elevateModeSetting,
                       const std::string& legacyElevateSetting);
bool shouldElevateProcess(UInt8 mode);
bool shouldAutoElevate(UInt8 mode, const std::string& desktopName);

} // namespace ElevationPolicy
