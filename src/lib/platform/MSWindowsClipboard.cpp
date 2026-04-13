/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
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

#include "platform/MSWindowsClipboard.h"

#include "platform/MSWindowsClipboardTextConverter.h"
#include "platform/MSWindowsClipboardUTF16Converter.h"
#include "platform/MSWindowsClipboardBitmapConverter.h"
#include "platform/MSWindowsClipboardPNGConverter.h"
#include "platform/MSWindowsClipboardHTMLConverter.h"
#include "platform/MSWindowsClipboardFacade.h"
#include "arch/win32/ArchMiscWindows.h"
#include "base/Log.h"
#include "ext/lodepng/lodepng.h"

static std::string convertBMPToPNG(const std::string& dibData);

//
// MSWindowsClipboard
//

UINT                    MSWindowsClipboard::s_ownershipFormat = 0;

MSWindowsClipboard::MSWindowsClipboard(HWND window) :
    m_window(window),
    m_time(0),
    m_facade(new MSWindowsClipboardFacade()),
    m_deleteFacade(true)
{
    // add converters, most desired first
    m_converters.push_back(new MSWindowsClipboardUTF16Converter);
    m_converters.push_back(new MSWindowsClipboardBitmapConverter);
    m_converters.push_back(new MSWindowsClipboardPNGConverter);
    m_converters.push_back(new MSWindowsClipboardHTMLConverter);
}

MSWindowsClipboard::~MSWindowsClipboard()
{
    clearConverters();

    // dependency injection causes confusion over ownership, so we need
    // logic to decide whether or not we delete the facade. there must
    // be a more elegant way of doing this.
    if (m_deleteFacade)
        delete m_facade;
}

void
MSWindowsClipboard::setFacade(IMSWindowsClipboardFacade& facade)
{
    delete m_facade;
    m_facade = &facade;
    m_deleteFacade = false;
}

bool
MSWindowsClipboard::emptyUnowned()
{
    LOG((CLOG_DEBUG "empty clipboard"));

    // empty the clipboard (and take ownership)
    if (!EmptyClipboard()) {
        // unable to cause this in integ tests, but this error has never
        // actually been reported by users.
        LOG((CLOG_DEBUG "failed to grab clipboard"));
        return false;
    }

    return true;
}

bool
MSWindowsClipboard::empty()
{
    if (!emptyUnowned()) {
        return false;
    }

    // mark clipboard as being owned by barrier
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, 1);
    if (NULL == SetClipboardData(getOwnershipFormat(), data)) {
        LOG((CLOG_DEBUG "failed to set clipboard data"));
        GlobalFree(data);
        return false;
    }

    return true;
}

void
MSWindowsClipboard::add(EFormat format, const std::string& data)
{
    LOG((CLOG_DEBUG "add %d bytes to clipboard format: %d", data.size(), format));

    // convert data to win32 form
    for (ConverterList::const_iterator index = m_converters.begin();
                                index != m_converters.end(); ++index) {
        IMSWindowsClipboardConverter* converter = *index;

        // skip converters for other formats
        if (converter->getFormat() == format) {
            HANDLE win32Data = converter->fromIClipboard(data);
            if (win32Data != NULL) {
                UINT win32Format = converter->getWin32Format();
                m_facade->write(win32Data, win32Format);
            }
        }
    }
}

bool
MSWindowsClipboard::open(Time time) const
{
    LOG((CLOG_DEBUG "open clipboard"));

    if (!OpenClipboard(m_window)) {
        // unable to cause this in integ tests; but this can happen!
        // * http://symless.com/pm/issues/86
        // * http://symless.com/pm/issues/1256
        // logging improved to see if we can catch more info next time.
        LOG((CLOG_WARN "failed to open clipboard: %d", GetLastError()));
        return false;
    }

    m_time = time;

    return true;
}

void
MSWindowsClipboard::close() const
{
    LOG((CLOG_DEBUG "close clipboard"));
    CloseClipboard();
}

IClipboard::Time
MSWindowsClipboard::getTime() const
{
    return m_time;
}

bool
MSWindowsClipboard::has(EFormat format) const
{
    // Special handling for PNG format: check for PNG first, then fallback to BMP
    if (format == IClipboard::kPNG) {
        // Check if PNG format is available
        UINT pngFormat = MSWindowsClipboardPNGConverter::getStaticFormatId();
        if (pngFormat != 0 && IsClipboardFormatAvailable(pngFormat)) {
            return true;
        }
        // Fallback: check if BMP is available (most apps provide BMP instead of PNG)
        if (IsClipboardFormatAvailable(CF_DIB)) {
            return true;
        }
        return false;
    }

    // Original logic for other formats
    for (ConverterList::const_iterator index = m_converters.begin();
                                index != m_converters.end(); ++index) {
        IMSWindowsClipboardConverter* converter = *index;
        if (converter->getFormat() == format) {
            if (IsClipboardFormatAvailable(converter->getWin32Format())) {
                return true;
            }
        }
    }
    return false;
}

std::string MSWindowsClipboard::get(EFormat format) const
{
    // Special handling for PNG format: try PNG first, then fallback to BMP→PNG conversion
    if (format == IClipboard::kPNG) {
        // Find PNG converter
        IMSWindowsClipboardConverter* pngConverter = nullptr;
        for (ConverterList::const_iterator index = m_converters.begin();
            index != m_converters.end(); ++index) {
            if ((*index)->getFormat() == IClipboard::kPNG) {
                pngConverter = *index;
                break;
            }
        }

        // Try PNG format first
        UINT pngFormat = MSWindowsClipboardPNGConverter::getStaticFormatId();
        if (pngFormat != 0) {
            HANDLE pngData = GetClipboardData(pngFormat);
            if (pngData != NULL && pngConverter != nullptr) {
                std::string result = pngConverter->toIClipboard(pngData);
                if (!result.empty()) {
                    LOG((CLOG_DEBUG "Got PNG from clipboard directly"));
                    return result;
                }
            }
        }

        // Fallback: convert BMP to PNG
        HANDLE bmpData = GetClipboardData(CF_DIB);
        if (bmpData != NULL) {
            // Get bitmap data size
            SIZE_T bmpSize = GlobalSize(bmpData);
            LPVOID bmpPtr = GlobalLock(bmpData);
            if (bmpPtr != NULL) {
                std::string dibData(static_cast<const char*>(bmpPtr), bmpSize);
                GlobalUnlock(bmpData);

                LOG((CLOG_DEBUG "Converting BMP (%u bytes) to PNG", bmpSize));
                return convertBMPToPNG(dibData);
            }
        }

        LOG((CLOG_DEBUG "No PNG or BMP data available for PNG format"));
        return {};
    }

    // Original logic for other formats
    // find the converter for the first clipboard format we can handle
    IMSWindowsClipboardConverter* converter = NULL;
    for (ConverterList::const_iterator index = m_converters.begin();
        index != m_converters.end(); ++index) {

        converter = *index;
        if (converter->getFormat() == format) {
            break;
        }
        converter = NULL;
    }

    // if no converter then we don't recognize any formats
    if (converter == NULL) {
        LOG((CLOG_WARN "no converter for format %d", format));
        return {};
    }

    // get a handle to the clipboard data
    HANDLE win32Data = GetClipboardData(converter->getWin32Format());
    if (win32Data == NULL) {
        // nb: can't cause this using integ tests; this is only caused when
        // the selected converter returns an invalid format -- which you
        // cannot cause using public functions.
        return {};
    }

    // convert
    return converter->toIClipboard(win32Data);
}

void
MSWindowsClipboard::clearConverters()
{
    for (ConverterList::iterator index = m_converters.begin();
                                index != m_converters.end(); ++index) {
        delete *index;
    }
    m_converters.clear();
}

bool
MSWindowsClipboard::isOwnedByBarrier()
{
    // create ownership format if we haven't yet
    if (s_ownershipFormat == 0) {
        s_ownershipFormat = RegisterClipboardFormat(TEXT("BarrierOwnership"));
    }
    return (IsClipboardFormatAvailable(getOwnershipFormat()) != 0);
}

UINT
MSWindowsClipboard::getOwnershipFormat()
{
    // create ownership format if we haven't yet
    if (s_ownershipFormat == 0) {
        s_ownershipFormat = RegisterClipboardFormat(TEXT("BarrierOwnership"));
    }

    // return the format
    return s_ownershipFormat;
}

//
// Helper function to convert Windows BMP (DIB) data to PNG
//
static std::string convertBMPToPNG(const std::string& dibData)
{
    // DIB format: BITMAPINFOHEADER + pixel data
    // Pixel data can be BI_RGB (no compression) or BI_RLE8/BI_RLE4

    if (dibData.size() < 40) {
        LOG((CLOG_WARN "DIB data too small for header"));
        return {};
    }

    const UInt8* header = reinterpret_cast<const UInt8*>(dibData.data());

    // Get dimensions (BITMAPINFOHEADER is 40 bytes)
    UInt32 headerSize = *reinterpret_cast<const UInt32*>(header + 0);
    if (headerSize != 40) {
        LOG((CLOG_WARN "Unsupported DIB header size: %u", headerSize));
        return {};
    }

    SInt32 width = *reinterpret_cast<const SInt32*>(header + 4);
    SInt32 height = *reinterpret_cast<const SInt32*>(header + 8);
    UInt16 bitCount = *reinterpret_cast<const UInt16*>(header + 14);
    UInt32 compression = *reinterpret_cast<const UInt32*>(header + 16);

    // Height can be negative for top-down DIB
    bool topDown = (height < 0);
    if (height < 0) {
        height = -height;
    }

    // Only support uncompressed 24-bit or 32-bit DIB
    if (compression != 0) {  // BI_RGB = 0
        LOG((CLOG_WARN "Compressed DIB not supported"));
        return {};
    }

    if (bitCount != 24 && bitCount != 32) {
        LOG((CLOG_WARN "Unsupported DIB bit count: %u", bitCount));
        return {};
    }

    UInt32 bytesPerPixel = bitCount / 8;
    UInt32 rowSize = ((width * bitCount + 31) / 32) * 4;  // Row is aligned to 4 bytes
    UInt32 expectedDataSize = 40 + rowSize * height;

    if (dibData.size() < expectedDataSize) {
        LOG((CLOG_WARN "DIB data size mismatch: expected %u, got %u", expectedDataSize, dibData.size()));
        return {};
    }

    const UInt8* pixelData = header + 40;

    // Convert to RGBA for lodepng
    std::vector<unsigned char> rgba;
    rgba.reserve(width * height * 4);

    for (SInt32 y = 0; y < static_cast<SInt32>(height); ++y) {
        SInt32 srcY = topDown ? y : (height - 1 - y);
        const UInt8* row = pixelData + srcY * rowSize;

        for (SInt32 x = 0; x < width; ++x) {
            UInt32 offset = x * bytesPerPixel;

            // DIB is usually BGR/BGRA format
            unsigned char b = row[offset + 0];
            unsigned char g = row[offset + 1];
            unsigned char r = row[offset + 2];
            unsigned char a = (bitCount == 32) ? row[offset + 3] : 255;

            // Convert to RGBA for lodepng
            rgba.push_back(r);
            rgba.push_back(g);
            rgba.push_back(b);
            rgba.push_back(a);
        }
    }

    // Encode to PNG
    std::vector<unsigned char> png;
    unsigned error = lodepng::encode(png, rgba, width, height, LCT_RGBA);
    if (error) {
        LOG((CLOG_ERR "PNG encoding failed: %s", lodepng_error_text(error)));
        return {};
    }

    return std::string(reinterpret_cast<const char*>(png.data()), png.size());
}
