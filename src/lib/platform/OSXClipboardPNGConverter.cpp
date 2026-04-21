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

#include "OSXClipboardPNGConverter.h"
#include "base/Log.h"

OSXClipboardPNGConverter::OSXClipboardPNGConverter()
{
    // do nothing
}

OSXClipboardPNGConverter::~OSXClipboardPNGConverter()
{
    // do nothing
}

IClipboard::EFormat
OSXClipboardPNGConverter::getFormat() const
{
    return IClipboard::kPNG;
}

CFStringRef
OSXClipboardPNGConverter::getOSXFormat() const
{
    // Use standard macOS PNG pasteboard type
    return CFSTR("public.png");
}

std::string
OSXClipboardPNGConverter::fromIClipboard(const std::string& pngData) const
{
    // PNG data from clipboard is already in proper format
    // Just verify PNG signature
    if (pngData.size() < 8) {
        return {};
    }

    if (static_cast<unsigned char>(pngData[0]) != 0x89 || pngData[1] != 'P' ||
        pngData[2] != 'N' || pngData[3] != 'G') {
        return {};
    }

    return pngData;
}

std::string
OSXClipboardPNGConverter::toIClipboard(const std::string& pngData) const
{
    // PNG data - return as-is for macOS pasteboard
    return fromIClipboard(pngData);
}
