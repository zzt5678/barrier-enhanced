/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2014-2016 Symless Ltd.
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

#include "barrier/DropHelper.h"

#include "base/Log.h"
#include "io/filesystem.h"

#include <fstream>

namespace {

barrier::fs::path unique_drop_target_path(const barrier::fs::path& destination,
                                          const String& filename)
{
    barrier::fs::path candidate = destination / barrier::fs::u8path(filename);
    if (!barrier::fs::exists(candidate)) {
        return candidate;
    }

    const auto stem = candidate.stem().u8string();
    const auto extension = candidate.extension().u8string();
    for (int index = 1; index < 1000; ++index) {
        barrier::fs::path next =
            destination / barrier::fs::u8path(
                stem + " (" + std::to_string(index) + ")" + extension);
        if (!barrier::fs::exists(next)) {
            return next;
        }
    }

    return candidate;
}

}

void
DropHelper::writeToDir(const String& destination, DragFileList& fileList, String& data)
{
    LOG((CLOG_DEBUG "dropping file, files=%i target=%s", fileList.size(), destination.c_str()));

    if (!destination.empty() && fileList.size() > 0) {
        const barrier::fs::path dropDirectory = barrier::fs::u8path(destination);
        barrier::fs::create_directories(dropDirectory);

        const barrier::fs::path dropTarget =
            unique_drop_target_path(dropDirectory, fileList.at(0).getFilename());
        const barrier::fs::path tempTarget =
            dropTarget.parent_path() / barrier::fs::u8path(dropTarget.filename().u8string() + ".barrier-part");

        std::ofstream file;
        barrier::open_utf8_path(file, tempTarget, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            LOG((CLOG_ERR "drop file failed: can not open %s", tempTarget.u8string().c_str()));
            return;
        }

        file.write(data.c_str(), data.size());
        file.flush();
        file.close();

        if (file.fail()) {
            LOG((CLOG_ERR "drop file failed while writing %s", tempTarget.u8string().c_str()));
            barrier::fs::remove(tempTarget);
            return;
        }

        barrier::fs::rename(tempTarget, dropTarget);

        LOG((CLOG_INFO "dropped file \"%s\" in \"%s\"",
             dropTarget.filename().u8string().c_str(),
             destination.c_str()));

        fileList.clear();
        String().swap(data);
    }
    else {
        LOG((CLOG_ERR "drop file failed: drop target is empty"));
    }
}
