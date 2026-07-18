/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/SecureRandom.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>

TEST(SecureRandomTests, providerProducesFixedWidthLowercaseHex)
{
    std::string output = "stale";
    const bool generated = barrier::SecureRandom::generateHex(
        6, output,
        [](unsigned char* bytes, std::size_t size) {
            const unsigned char values[] = {0x00, 0x01, 0x0f, 0x10, 0xab, 0xff};
            if (size != sizeof(values)) {
                return false;
            }
            std::copy(values, values + sizeof(values), bytes);
            return true;
        });

    EXPECT_TRUE(generated);
    EXPECT_EQ("00010f10abff", output);
}

TEST(SecureRandomTests, providerFailureClearsOutput)
{
    std::string output = "stale";
    const bool generated = barrier::SecureRandom::generateHex(
        16, output,
        [](unsigned char* bytes, std::size_t size) {
            std::fill(bytes, bytes + size, 0xa5);
            return false;
        });

    EXPECT_FALSE(generated);
    EXPECT_TRUE(output.empty());
}

TEST(SecureRandomTests, providerExceptionClearsOutputWithoutEscaping)
{
    std::string output = "stale";
    EXPECT_NO_THROW({
        const bool generated = barrier::SecureRandom::generateHex(
            16, output,
            [](unsigned char*, std::size_t) -> bool {
                throw std::runtime_error("provider failed");
            });
        EXPECT_FALSE(generated);
    });
    EXPECT_TRUE(output.empty());
}
