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
#include <functional>
#include <string>

namespace barrier {

class SecureRandom {
public:
    using Provider = std::function<bool(unsigned char*, std::size_t)>;

    static bool generateHex(std::size_t byteCount, std::string& output);
    static bool generateHex(std::size_t byteCount, std::string& output,
                            const Provider& provider);
};

} // namespace barrier
