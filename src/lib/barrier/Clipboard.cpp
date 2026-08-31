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

#include "barrier/Clipboard.h"

static const size_t kClipboardFormatReleaseThreshold = 1024 * 1024;
static const size_t kClipboardSnapshotExactLimit = 1024 * 1024;
static const std::uint64_t kFnv1aOffset = 1469598103934665603ull;
static const std::uint64_t kFnv1aPrime = 1099511628211ull;

static std::uint64_t
hashClipboardData(const String& data)
{
    std::uint64_t hash = kFnv1aOffset;
    for (String::const_iterator i = data.begin(); i != data.end(); ++i) {
        hash ^= static_cast<unsigned char>(*i);
        hash *= kFnv1aPrime;
    }
    return hash;
}

static UInt32
readClipboardUInt32(const char* data)
{
    const unsigned char* bytes =
        reinterpret_cast<const unsigned char*>(data);
    return (static_cast<UInt32>(bytes[0]) << 24) |
           (static_cast<UInt32>(bytes[1]) << 16) |
           (static_cast<UInt32>(bytes[2]) << 8) |
            static_cast<UInt32>(bytes[3]);
}

static bool
inspectMarshalledClipboard(const String& data, UInt32 targetFormat,
                           bool* found)
{
    if (data.size() < 4 || found == NULL) {
        return false;
    }

    *found = false;
    size_t offset = 0;
    const UInt32 formatCount = readClipboardUInt32(data.data());
    if (formatCount > 1024) {
        return false;
    }
    offset += 4;
    for (UInt32 i = 0; i < formatCount; ++i) {
        if (data.size() - offset < 8) {
            return false;
        }
        const UInt32 encodedFormat = readClipboardUInt32(data.data() + offset);
        const UInt32 size = readClipboardUInt32(data.data() + offset + 4);
        offset += 8;
        if (size > data.size() - offset) {
            return false;
        }
        *found = *found || encodedFormat == targetFormat;
        offset += size;
    }
    return offset == data.size();
}

void
ClipboardDataSnapshot::set(const String& data)
{
    m_valid = true;
    m_size = data.size();
    m_hash = hashClipboardData(data);

    if (data.size() <= kClipboardSnapshotExactLimit) {
        m_exactData = data;
        m_hasExactData = true;
    }
    else {
        String().swap(m_exactData);
        m_hasExactData = false;
    }
}

bool
ClipboardDataSnapshot::matches(const String& data) const
{
    if (!m_valid || data.size() != m_size) {
        return false;
    }

    if (m_hasExactData) {
        return data == m_exactData;
    }

    return hashClipboardData(data) == m_hash;
}

void
ClipboardDataSnapshot::clear()
{
    m_valid = false;
    m_hasExactData = false;
    m_size = 0;
    m_hash = 0;
    String().swap(m_exactData);
}

bool
Clipboard::marshalledHasFormat(const String& data, IClipboard::EFormat format)
{
    if (format < 0 || format >= IClipboard::kNumFormats) {
        return false;
    }
    bool found = false;
    return inspectMarshalledClipboard(
        data, static_cast<UInt32>(format), &found) && found;
}

bool
Clipboard::isValidMarshalled(const String& data)
{
    bool ignored = false;
    return inspectMarshalledClipboard(
        data, static_cast<UInt32>(IClipboard::kNumFormats), &ignored);
}

//
// Clipboard
//

Clipboard::Clipboard() :
    m_open(false),
    m_owner(false)
{
    open(0);
    empty();
    close();
}

Clipboard::~Clipboard()
{
    // do nothing
}

bool
Clipboard::empty()
{
    assert(m_open);

    // clear all data
    for (SInt32 index = 0; index < kNumFormats; ++index) {
        if (m_data[index].capacity() > kClipboardFormatReleaseThreshold) {
            String().swap(m_data[index]);
        }
        else {
            m_data[index].clear();
        }
        m_added[index] = false;
    }

    // save time
    m_timeOwned = m_time;

    // we're the owner now
    m_owner = true;

    return true;
}

void
Clipboard::add(EFormat format, const String& data)
{
    assert(m_open);
    assert(m_owner);

    m_data[format]  = data;
    m_added[format] = true;
}

bool
Clipboard::open(Time time) const
{
    assert(!m_open);

    m_open = true;
    m_time = time;

    return true;
}

void
Clipboard::close() const
{
    assert(m_open);

    m_open = false;
}

Clipboard::Time
Clipboard::getTime() const
{
    return m_timeOwned;
}

bool
Clipboard::has(EFormat format) const
{
    assert(m_open);
    return m_added[format];
}

String
Clipboard::get(EFormat format) const
{
    assert(m_open);
    return m_data[format];
}

void
Clipboard::unmarshall(const String& data, Time time)
{
    IClipboard::unmarshall(this, data, time);
}

String
Clipboard::marshall() const
{
    return IClipboard::marshall(this);
}
