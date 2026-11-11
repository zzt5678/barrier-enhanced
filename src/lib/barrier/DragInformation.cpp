/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2013-2016 Symless Ltd.
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

#include "barrier/DragInformation.h"
#include "base/Log.h"
#include "io/filesystem.h"
#include <charconv>
#include <algorithm>
#include <cstdint>

#include <fstream>
#include <sstream>
#include <stdexcept>

using namespace std;

DragInformation::DragInformation() :
    m_filename(),
    m_filesize(0),
    m_entryType(File)
{
}

void
DragInformation::parseDragInfo(DragFileList& dragFileList, UInt32 fileNum, String data)
{
    dragFileList.clear();
    DragFileList parsed;
    size_t offset = 0;
    const size_t fields = std::count(data.begin(), data.end(), ',');
    const bool typed = fields == static_cast<uint64_t>(fileNum) * 3;
    if (!typed && fields != static_cast<uint64_t>(fileNum) * 2) {
        LOG((CLOG_WARN "discarding malformed drag metadata field count"));
        return;
    }
    auto reject = [] { LOG((CLOG_WARN "discarding malformed drag metadata")); };
    for (UInt32 index = 0; index < fileNum; ++index) {
        const size_t nameEnd = data.find(',', offset);
        if (nameEnd == String::npos || nameEnd == offset) { reject(); return; }
        const String path = data.substr(offset, nameEnd - offset);
        const size_t slash = path.find_last_of("/\\");
        String name = path.substr(slash == String::npos ? 0 : slash + 1);
        if (name.empty() || name == "." || name == "..") { reject(); return; }
        offset = nameEnd + 1;
        const size_t sizeEnd = data.find(',', offset);
        if (sizeEnd == String::npos || sizeEnd == offset) { reject(); return; }
        size_t size = 0;
        const char* first = data.data() + offset;
        const char* last = data.data() + sizeEnd;
        const auto converted = std::from_chars(first, last, size);
        if (converted.ec != std::errc() || converted.ptr != last) { reject(); return; }
        offset = sizeEnd + 1;
        EntryType type = File;
        if (typed) {
            if (offset + 1 >= data.size() || data[offset + 1] != ',' ||
                (data[offset] != 'F' && data[offset] != 'D')) { reject(); return; }
            type = data[offset] == 'D' ? Directory : File;
            offset += 2;
        }
        DragInformation entry;
        entry.setFilename(name);
        entry.setFilesize(size);
        entry.setEntryType(type);
        parsed.push_back(entry);
    }
    if (offset != data.size()) { reject(); return; }
    dragFileList.swap(parsed);

}

String
DragInformation::getDragFileExtension(String filename)
{
    size_t findResult = string::npos;
    findResult = filename.find_last_of(".", filename.size());
    if (findResult != string::npos) {
        return filename.substr(findResult + 1, filename.size() - findResult - 1);
    }
    else {
        return "";
    }
}

int
DragInformation::setupDragInfo(DragFileList& fileList, String& output)
{
    DragFileList valid;
    for (auto& entry : fileList) {
        // This wire format has no escaping. Do not emit ambiguous metadata.
        if (entry.getFilename().empty() || entry.getFilename().find(',') != String::npos) {
            LOG((CLOG_WARN "cannot encode drag filename in legacy metadata"));
            continue;
        }
        const String size = entry.isDirectory() ? String("0") : getFileSize(entry.getFilename());
        if (size.empty()) {
            LOG((CLOG_WARN "skipping unreadable drag file"));
            continue;
        }
        output += entry.getFilename() + "," + size + ",";
        output.push_back(static_cast<char>(entry.getEntryType()));
        output.push_back(',');
        valid.push_back(entry);
    }
    fileList.swap(valid);
    return static_cast<int>(fileList.size());

}

bool
DragInformation::isFileValid(String filename)
{
    bool result = false;
    std::fstream file;
    barrier::open_utf8_path(file, barrier::fs::u8path(filename), ios::in|ios::binary);

    if (file.is_open()) {
        result = true;
    }

    file. close();

    return result;
}

String
DragInformation::getFileSize(String& filename)
{
    std::error_code ec;
    const auto size = barrier::fs::file_size(barrier::fs::u8path(filename), ec);
    return ec ? String() : std::to_string(size);
}
