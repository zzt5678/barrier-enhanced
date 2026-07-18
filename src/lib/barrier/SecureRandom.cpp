/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/SecureRandom.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <limits>
#include <vector>

namespace {

const char kHexDigits[] = "0123456789abcdef";

class SensitiveBytes {
public:
    explicit SensitiveBytes(std::size_t size) : values(size)
    {
    }

    ~SensitiveBytes()
    {
        if (!values.empty()) {
            OPENSSL_cleanse(values.data(), values.size());
        }
    }

    std::vector<unsigned char> values;
};

} // namespace

namespace barrier {

bool
SecureRandom::generateHex(std::size_t byteCount, std::string& output)
{
    if (byteCount > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        output.clear();
        return false;
    }

    return generateHex(
        byteCount, output,
        [](unsigned char* bytes, std::size_t size) {
            return RAND_bytes(bytes, static_cast<int>(size)) == 1;
        });
}

bool
SecureRandom::generateHex(std::size_t byteCount, std::string& output,
                          const Provider& provider)
{
    output.clear();
    if (!provider ||
        byteCount > std::numeric_limits<std::size_t>::max() / 2) {
        return false;
    }
    if (byteCount == 0) {
        return true;
    }

    SensitiveBytes randomBytes(byteCount);
    try {
        if (!provider(randomBytes.values.data(), randomBytes.values.size())) {
            return false;
        }
    }
    catch (...) {
        return false;
    }

    output.resize(byteCount * 2);
    for (std::size_t i = 0; i < byteCount; ++i) {
        output[i * 2] = kHexDigits[randomBytes.values[i] >> 4];
        output[i * 2 + 1] = kHexDigits[randomBytes.values[i] & 0x0f];
    }
    return true;
}

} // namespace barrier
