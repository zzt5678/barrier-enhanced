/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2024 Barrier Contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "XWindowsClipboardPNGConverter.h"
#include "base/Log.h"
#include "ext/lodepng/lodepng.h"

// BMP is little-endian
static inline
UInt32 fromLEU32(const UInt8* data)
{
    return static_cast<UInt32>(data[0]) |
            (static_cast<UInt32>(data[1]) <<  8) |
            (static_cast<UInt32>(data[2]) << 16) |
            (static_cast<UInt32>(data[3]) << 24);
}

//
// XWindowsClipboardPNGConverter
//

XWindowsClipboardPNGConverter::XWindowsClipboardPNGConverter(Display* display) :
    m_atom(XInternAtom(display, "image/png", False))
{
    // do nothing
}

XWindowsClipboardPNGConverter::~XWindowsClipboardPNGConverter()
{
    // do nothing
}

IClipboard::EFormat
XWindowsClipboardPNGConverter::getFormat() const
{
    return IClipboard::kPNG;
}

Atom
XWindowsClipboardPNGConverter::getAtom() const
{
    return m_atom;
}

int
XWindowsClipboardPNGConverter::getDataSize() const
{
    return 8;
}

std::string
XWindowsClipboardPNGConverter::fromIClipboard(const std::string& data) const
{
    // data is already in PNG format, return as-is
    return data;
}

std::string
XWindowsClipboardPNGConverter::toIClipboard(const std::string& pngData) const
{
    // PNG data from clipboard - verify it's valid
    if (pngData.size() < 8) {
        return {};
    }

    // Verify PNG signature
    if (pngData[0] != 0x89 || pngData[1] != 'P' ||
        pngData[2] != 'N' || pngData[3] != 'G') {
        return {};
    }

    // Return PNG data as-is for internal use
    return pngData;
}

std::string
XWindowsClipboardPNGConverter::convertBMPToPNG(const UInt8* bmpData, UInt32 width, UInt32 height, UInt32 depth) const
{
    // BMP data is in BGR/BGRA format, need to convert to RGBA for lodepng
    std::vector<unsigned char> image;
    image.reserve(width * height * 4);

    if (depth == 32) {
        // BGRA -> RGBA
        for (UInt32 y = 0; y < height; ++y) {
            for (UInt32 x = 0; x < width; ++x) {
                UInt32 offset = (y * width + x) * 4;
                image.push_back(bmpData[offset + 2]); // R
                image.push_back(bmpData[offset + 1]); // G
                image.push_back(bmpData[offset + 0]); // B
                image.push_back(bmpData[offset + 3]); // A
            }
        }
    } else {
        // BGR -> RGBA
        for (UInt32 y = 0; y < height; ++y) {
            for (UInt32 x = 0; x < width; ++x) {
                UInt32 offset = (y * width + x) * 3;
                image.push_back(bmpData[offset + 2]); // R
                image.push_back(bmpData[offset + 1]); // G
                image.push_back(bmpData[offset + 0]); // B
                image.push_back(255); // A (opaque)
            }
        }
    }

    // Encode to PNG
    std::vector<unsigned char> png;
    unsigned error = lodepng::encode(png, image, width, height, LCT_RGBA);
    if (error) {
        LOG((CLOG_ERR "PNG encoding failed: %s", lodepng_error_text(error)));
        return {};
    }

    return std::string(reinterpret_cast<const char*>(png.data()), png.size());
}

std::string
XWindowsClipboardPNGConverter::convertPNGToBMP(const std::string& pngData, UInt32& width, UInt32& height, UInt32& depth) const
{
    // Decode PNG to RGBA
    std::vector<unsigned char> image;
    unsigned error = lodepng::decode(image, width, height, pngData, LCT_RGBA);
    if (error) {
        LOG((CLOG_ERR "PNG decoding failed: %s", lodepng_error_text(error)));
        return {};
    }

    depth = 32;

    // Convert RGBA to BGRA for BMP format
    std::string bmp;
    bmp.reserve(width * height * 4);

    // BMP info header (BITMAPINFOHEADER = 40 bytes)
    UInt8 header[40] = {0};
    *reinterpret_cast<UInt32*>(header + 0) = 40;  // biSize
    *reinterpret_cast<SInt32*>(header + 4) = static_cast<SInt32>(width);  // biWidth
    *reinterpret_cast<SInt32*>(header + 8) = -static_cast<SInt32>(height);  // biHeight (negative for top-down)
    *reinterpret_cast<UInt16*>(header + 12) = 1;  // biPlanes
    *reinterpret_cast<UInt16*>(header + 14) = 32;  // biBitCount
    *reinterpret_cast<UInt32*>(header + 16) = 0;  // biCompression (BI_RGB)

    bmp.append(reinterpret_cast<const char*>(header), 40);

    // BMP pixel data (BGRA, bottom-up)
    for (SInt32 y = static_cast<SInt32>(height) - 1; y >= 0; --y) {
        for (UInt32 x = 0; x < width; ++x) {
            UInt32 offset = (y * width + x) * 4;
            bmp.push_back(static_cast<char>(image[offset + 2])); // B
            bmp.push_back(static_cast<char>(image[offset + 1])); // G
            bmp.push_back(static_cast<char>(image[offset + 0])); // R
            bmp.push_back(static_cast<char>(image[offset + 3])); // A
        }
    }

    return bmp;
}
