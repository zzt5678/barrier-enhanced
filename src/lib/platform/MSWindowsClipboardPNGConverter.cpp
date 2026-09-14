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

#include "MSWindowsClipboardPNGConverter.h"
#include "base/Log.h"
#include <windows.h>
#include <cstring>

// Static format ID for PNG - registered at runtime
static UINT s_pngFormatId = 0;

MSWindowsClipboardPNGConverter::MSWindowsClipboardPNGConverter()
{
    // Register PNG clipboard format on first use
    if (s_pngFormatId == 0) {
        s_pngFormatId = RegisterClipboardFormat("image/png");
        if (s_pngFormatId == 0) {
            LOG((CLOG_WARN "failed to register PNG clipboard format"));
        } else {
            LOG((CLOG_DEBUG1 "registered PNG clipboard format: %u", s_pngFormatId));
        }
    }
}

MSWindowsClipboardPNGConverter::~MSWindowsClipboardPNGConverter()
{
    // do nothing
}

IClipboard::EFormat
MSWindowsClipboardPNGConverter::getFormat() const
{
    return IClipboard::kPNG;
}

UINT
MSWindowsClipboardPNGConverter::getWin32Format() const
{
    return s_pngFormatId;
}

HANDLE
MSWindowsClipboardPNGConverter::fromIClipboard(const std::string& pngData) const
{
    // Verify PNG signature
    if (pngData.size() < 8) {
        return NULL;
    }

    if (std::memcmp(pngData.data(), "\x89PNG\r\n\x1a\n", 8) != 0) {
        return NULL;
    }

    // Copy PNG data to memory handle
    HGLOBAL gData = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, pngData.size());
    if (gData != NULL) {
        char* dst = static_cast<char*>(GlobalLock(gData));
        if (dst != NULL) {
            memcpy(dst, pngData.data(), pngData.size());
            GlobalUnlock(gData);
        } else {
            GlobalFree(gData);
            gData = NULL;
        }
    }

    return gData;
}

std::string
MSWindowsClipboardPNGConverter::toIClipboard(HANDLE data) const
{
    if (data == NULL) {
        return {};
    }

    LPVOID src = GlobalLock(data);
    if (src == NULL) {
        return {};
    }

    SIZE_T srcSize = GlobalSize(data);

    // Verify PNG signature
    if (srcSize < 8 ||
        reinterpret_cast<UInt8*>(src)[0] != 0x89 ||
        reinterpret_cast<UInt8*>(src)[1] != 'P' ||
        reinterpret_cast<UInt8*>(src)[2] != 'N' ||
        reinterpret_cast<UInt8*>(src)[3] != 'G') {
        GlobalUnlock(data);
        return {};
    }

    std::string result(static_cast<const char*>(src), srcSize);
    GlobalUnlock(data);

    return result;
}

UINT
MSWindowsClipboardPNGConverter::getStaticFormatId()
{
    return s_pngFormatId;
}
