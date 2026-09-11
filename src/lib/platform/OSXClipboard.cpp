/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2004 Chris Schoeneman
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

#include "platform/OSXClipboard.h"

#include "barrier/Clipboard.h"
#include "barrier/RemoteFileClipboard.h"
#include "platform/OSXClipboardUTF16Converter.h"
#include "platform/OSXClipboardTextConverter.h"
#include "platform/OSXClipboardBMPConverter.h"
#include "platform/OSXClipboardPNGConverter.h"
#include "platform/OSXClipboardHTMLConverter.h"
#include "base/Log.h"
#include "arch/XArch.h"

#include <climits>
#include <cstdint>
#include <cstring>

namespace {

struct CFDataDeleter {
    void operator()(const __CFData* value) const { CFRelease(value); }
};
using OwnedCFData = std::unique_ptr<const __CFData, CFDataDeleter>;

CFStringRef kFileUrlFlavor()
{
    return CFSTR("public.file-url");
}

std::string utf8FromCFString(CFStringRef value)
{
    if (value == NULL) {
        return std::string();
    }

    const CFIndex length = CFStringGetLength(value);
    const CFIndex size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string output(static_cast<size_t>(size), '\0');
    if (!CFStringGetCString(value, &output[0], size, kCFStringEncodingUTF8)) {
        return std::string();
    }
    output.resize(strlen(output.c_str()));
    return output;
}

std::vector<barrier::fs::path> readFileUrls(PasteboardRef pboard)
{
    std::vector<barrier::fs::path> paths;
    ItemCount itemCount = 0;
    if (PasteboardGetItemCount(pboard, &itemCount) != noErr) {
        return paths;
    }

    for (ItemCount i = 1; i <= itemCount; ++i) {
        PasteboardItemID item;
        if (PasteboardGetItemIdentifier(pboard, i, &item) != noErr) {
            continue;
        }

        PasteboardFlavorFlags flags;
        if (PasteboardGetItemFlavorFlags(pboard, item, kFileUrlFlavor(), &flags) != noErr) {
            continue;
        }

        CFDataRef buffer = NULL;
        if (PasteboardCopyItemFlavorData(pboard, item, kFileUrlFlavor(), &buffer) != noErr || buffer == NULL) {
            if (buffer != NULL) {
                CFRelease(buffer);
            }
            continue;
        }

        const std::string urlBytes(
            reinterpret_cast<const char*>(CFDataGetBytePtr(buffer)),
            static_cast<size_t>(CFDataGetLength(buffer)));
        CFRelease(buffer);

        CFURLRef url = CFURLCreateWithBytes(
            kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(urlBytes.data()),
            urlBytes.size(),
            kCFStringEncodingUTF8,
            NULL);
        if (url == NULL) {
            continue;
        }

        UInt8 pathBuffer[PATH_MAX];
        if (CFURLGetFileSystemRepresentation(url, true, pathBuffer, sizeof(pathBuffer))) {
            paths.push_back(barrier::fs::u8path(reinterpret_cast<const char*>(pathBuffer)));
        }
        CFRelease(url);
    }

    return paths;
}

bool writeFileUrls(PasteboardRef pboard, const std::vector<barrier::fs::path>& paths)
{
    for (size_t i = 0; i < paths.size(); ++i) {
        const std::string utf8Path = paths[i].u8string();
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(
            kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(utf8Path.data()),
            utf8Path.size(),
            false);
        if (url == NULL) {
            return false;
        }

        CFStringRef urlString = CFURLGetString(url);
        const std::string utf8Url = utf8FromCFString(urlString);
        CFRelease(url);
        if (utf8Url.empty()) {
            return false;
        }

        CFDataRef dataRef = CFDataCreate(
            kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(utf8Url.data()),
            utf8Url.size());
        if (dataRef == NULL) {
            return false;
        }

        const OSStatus status = PasteboardPutItemFlavor(
            pboard,
            reinterpret_cast<PasteboardItemID>(static_cast<std::uintptr_t>(i + 1)),
            kFileUrlFlavor(),
            dataRef,
            kPasteboardFlavorNoFlags);
        CFRelease(dataRef);
        if (status != noErr) {
            return false;
        }
    }

    return true;
}

} // namespace

//
// OSXClipboard
//

OSXClipboard::OSXClipboard(CFStringRef pasteboardName) :
    m_time(0),
    m_pboard(NULL)
{
    m_converters.push_back(std::make_unique<OSXClipboardHTMLConverter>());
    m_converters.push_back(std::make_unique<OSXClipboardBMPConverter>());
    m_converters.push_back(std::make_unique<OSXClipboardPNGConverter>());
    m_converters.push_back(std::make_unique<OSXClipboardUTF16Converter>());
    m_converters.push_back(std::make_unique<OSXClipboardTextConverter>());



    OSStatus createErr = PasteboardCreate(pasteboardName, &m_pboard);
    if (createErr != noErr) {
        LOG((CLOG_DEBUG "failed to create clipboard reference: error %i", createErr));
        LOG((CLOG_ERR "unable to connect to pasteboard, clipboard sharing disabled", createErr));
        if (m_pboard != NULL) {
            CFRelease(m_pboard);
            m_pboard = NULL;
        }
        return;

    }

    // PasteboardSynchronize returns flags, not an OSStatus error code.
    PasteboardSynchronize(m_pboard);
}

OSXClipboard::~OSXClipboard()
{
    clearConverters();
    if (m_pboard != NULL) {
        CFRelease(m_pboard);
    }
}

    bool
OSXClipboard::empty()
{
    LOG((CLOG_DEBUG "emptying clipboard"));
    if (m_pboard == NULL)
        return false;

    OSStatus err = PasteboardClear(m_pboard);
    if (err != noErr) {
        LOG((CLOG_DEBUG "failed to clear clipboard: error %i", err));
        return false;
    }

    return true;
}

    bool
OSXClipboard::synchronize()
{
    if (m_pboard == NULL)
        return false;

    PasteboardSyncFlags flags = PasteboardSynchronize(m_pboard);
    LOG((CLOG_DEBUG2 "flags: %x", flags));

    if (flags & kPasteboardModified) {
        return true;
    }
    return false;
}

void OSXClipboard::add(EFormat format, const std::string& data)
{
    if (m_pboard == NULL)
        return;

    if (format == IClipboard::kFileList) {
        RemoteFileClipboard::Data payload;
        if (RemoteFileClipboard::parse(data, payload) &&
            payload.mode == RemoteFileClipboard::Mode::MaterializedPaths) {
            writeFileUrls(m_pboard, payload.paths);
        }
        return;
    }

    LOG((CLOG_DEBUG "add %d bytes to clipboard format: %d", data.size(), format));
    if (format == IClipboard::kText) {
        LOG((CLOG_DEBUG " format of data to be added to clipboard was kText"));
    }
    else if (format == IClipboard::kBitmap) {
        LOG((CLOG_DEBUG " format of data to be added to clipboard was kBitmap"));
    }
    else if (format == IClipboard::kHTML) {
        LOG((CLOG_DEBUG " format of data to be added to clipboard was kHTML"));
    }
    else if (format == IClipboard::kPNG) {
        LOG((CLOG_DEBUG " format of data to be added to clipboard was kPNG"));
    }

    for (ConverterList::const_iterator index = m_converters.begin();
            index != m_converters.end(); ++index) {

        IOSXClipboardConverter* converter = index->get();

        // skip converters for other formats
        if (converter->getFormat() == format) {
            std::string osXData = converter->fromIClipboard(data);
            CFStringRef flavorType = converter->getOSXFormat();
            OwnedCFData dataRef(CFDataCreate(kCFAllocatorDefault,
                reinterpret_cast<const UInt8*>(osXData.data()), osXData.size()));
            if (!dataRef) {
                LOG((CLOG_WARN "could not allocate clipboard flavor data"));
                continue;
            }
            PasteboardItemID itemID = 0;

            const OSStatus status = PasteboardPutItemFlavor(
                m_pboard,
                itemID,
                flavorType,
                dataRef.get(),
                kPasteboardFlavorNoFlags);
            if (status != noErr) {
                LOG((CLOG_WARN "could not publish clipboard flavor: error %i", status));
                continue;
            }

            LOG((CLOG_DEBUG "added %d bytes to clipboard format: %d", data.size(), format));
        }

    }
}

bool
OSXClipboard::open(Time time) const
{
    if (m_pboard == NULL)
        return false;

    LOG((CLOG_DEBUG "opening clipboard"));
    m_time = time;
    return true;
}

void
OSXClipboard::close() const
{
    LOG((CLOG_DEBUG "closing clipboard"));
    /* not needed */
}

IClipboard::Time
OSXClipboard::getTime() const
{
    return m_time;
}

bool
OSXClipboard::has(EFormat format) const
{
    if (m_pboard == NULL)
        return false;

    if (format == IClipboard::kFileList) {
        return !readFileUrls(m_pboard).empty();
    }

    PasteboardItemID item = nullptr;
    if (PasteboardGetItemIdentifier(m_pboard, (CFIndex) 1, &item) != noErr) {
        return false;
    }

    for (ConverterList::const_iterator index = m_converters.begin();
            index != m_converters.end(); ++index) {
        IOSXClipboardConverter* converter = index->get();
        if (converter->getFormat() == format) {
            PasteboardFlavorFlags flags;
            CFStringRef type = converter->getOSXFormat();

            OSStatus res;

            if ((res = PasteboardGetItemFlavorFlags(m_pboard, item, type, &flags)) == noErr) {
                return true;
            }
        }
    }

    return false;
}

std::string OSXClipboard::get(EFormat format) const
{
    CFStringRef type;
    PasteboardItemID item = nullptr;
    std::string result;

    if (m_pboard == NULL)
        return result;

    if (format == IClipboard::kFileList) {
        RemoteFileClipboard::Data payload;
        payload.mode = RemoteFileClipboard::Mode::SourcePaths;
        payload.paths = readFileUrls(m_pboard);
        return payload.paths.empty() ? std::string() : RemoteFileClipboard::serialize(payload);
    }

    if (PasteboardGetItemIdentifier(m_pboard, (CFIndex) 1, &item) != noErr) {
        return result;
    }


    // find the converter for the first clipboard format we can handle
    IOSXClipboardConverter* converter = NULL;
    for (ConverterList::const_iterator index = m_converters.begin();
            index != m_converters.end(); ++index) {
        converter = index->get();

        PasteboardFlavorFlags flags;
        type = converter->getOSXFormat();

        if (converter->getFormat() == format &&
                PasteboardGetItemFlavorFlags(m_pboard, item, type, &flags) == noErr) {
            break;
        }
        converter = NULL;
    }

    // if no converter then we don't recognize any formats
    if (converter == NULL) {
        LOG((CLOG_DEBUG "Unable to find converter for data"));
        return result;
    }

    // get the clipboard data.
    CFDataRef buffer = NULL;
    const OSStatus status = PasteboardCopyItemFlavorData(m_pboard, item, type, &buffer);
    OwnedCFData ownedBuffer(buffer);
    if (status != noErr || !ownedBuffer) {
        LOG((CLOG_DEBUG "could not read clipboard flavor: error %i", status));
        return result;
    }
    try {
        const CFIndex length = CFDataGetLength(ownedBuffer.get());
        if (length > 0) {
            result.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(ownedBuffer.get())),
                          static_cast<size_t>(length));
        }
    }
    catch (...) {
        LOG((CLOG_DEBUG "unknown exception in OSXClipboard::get"));
        RETHROW_XTHREAD
    }

    return converter->toIClipboard(result);
}

    void
OSXClipboard::clearConverters()
{
    m_converters.clear();
}
