/*
    barrier -- mouse and keyboard sharing utility
    Copyright (C) 2021 Barrier contributors

    This package is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    found in the file LICENSE that should have accompanied this file.

    This package is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "net/SecureUtils.h"

#include "test/global/gtest.h"

#include <cstdint>
#include <vector>

namespace barrier {

TEST(SecureUtilsTest, FormatSslFingerprintHexWithSeparators)
{
    const std::vector<std::uint8_t> fingerprint = {
        0x28, 0xfd, 0x0a, 0x98, 0x8a, 0x0e, 0xa1, 0x6c,
        0xd7, 0xe8, 0x6c, 0xa7, 0xee, 0x58, 0x41, 0x71,
        0xca, 0xb2, 0x8e, 0x49, 0x25, 0x94, 0x90, 0x25,
        0x26, 0x05, 0x8d, 0xaf, 0x63, 0xed, 0x2e, 0x30,
    };

    ASSERT_EQ(format_ssl_fingerprint(fingerprint, true),
              "28:FD:0A:98:8A:0E:A1:6C:D7:E8:6C:A7:EE:58:41:71:"
              "CA:B2:8E:49:25:94:90:25:26:05:8D:AF:63:ED:2E:30");
}

TEST(SecureUtilsTest, CreateFingerprintRandomArt)
{
    ASSERT_EQ(create_fingerprint_randomart(std::vector<std::uint8_t>(32, 0x00)),
              "+-----------------+\n"
              "|E....            |\n"
              "|     .           |\n"
              "|      .          |\n"
              "|       .         |\n"
              "|        S        |\n"
              "|                 |\n"
              "|                 |\n"
              "|                 |\n"
              "|                 |\n"
              "+-----------------+");
    ASSERT_EQ(create_fingerprint_randomart(std::vector<std::uint8_t>(32, 0xff)),
              "+-----------------+\n"
              "|                 |\n"
              "|                 |\n"
              "|                 |\n"
              "|                 |\n"
              "|        S        |\n"
              "|         .       |\n"
              "|          .      |\n"
              "|           .     |\n"
              "|            ....E|\n"
              "+-----------------+");
    const std::vector<std::uint8_t> incrementalFingerprint = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    };
    ASSERT_EQ(create_fingerprint_randomart(incrementalFingerprint),
              "+-----------------+\n"
              "|^^O@@E.          |\n"
              "|@@O++..          |\n"
              "|o+.. ..          |\n"
              "|       .         |\n"
              "|        S        |\n"
              "|                 |\n"
              "|                 |\n"
              "|                 |\n"
              "|                 |\n"
              "+-----------------+");
}

} // namespace barrier
