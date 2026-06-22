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
#include "barrier/TransferArchive.h"

#include "base/Log.h"
#include "io/filesystem.h"
#include "mt/Thread.h"
#include "mt/XThread.h"

#include <ctime>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace {

std::string uniqueToken()
{
    std::ostringstream stream;
    stream << std::hex
           << static_cast<unsigned long long>(std::time(nullptr))
           << "-"
           << static_cast<unsigned long long>(
               std::hash<std::thread::id>{}(std::this_thread::get_id()))
           << "-"
           << static_cast<unsigned long long>(std::rand());
    return stream.str();
}

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

    for (int attempt = 0; attempt < 128; ++attempt) {
        barrier::fs::path next =
            destination / barrier::fs::u8path(
                stem + " (" + uniqueToken() + ")" + extension);
        if (!barrier::fs::exists(next)) {
            return next;
        }
    }

    throw std::runtime_error("could not allocate unique drop target path");
}

bool copy_file_payload(const barrier::fs::path& source,
                       const barrier::fs::path& target)
{
    std::ifstream input;
    barrier::open_utf8_path(input, source, std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        return false;
    }

    std::ofstream output;
    barrier::open_utf8_path(output, target, std::ios::out | std::ios::binary | std::ios::app);
    if (!output.is_open()) {
        return false;
    }

    constexpr std::size_t kBufferSize = 64 * 1024;
    std::array<char, kBufferSize> buffer{};
    while (input.good()) {
        Thread::testCancel();
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            output.write(buffer.data(), count);
        }
    }

    output.flush();
    output.close();
    return input.eof() && !output.fail();
}

bool create_drop_temp_file(const barrier::fs::path& directory,
                           barrier::fs::path& tempTarget)
{
    return barrier::create_secure_temp_file_in_directory(
        directory, ".weave-drop-", ".part", tempTarget);
}

bool write_memory_payload(std::ofstream& output, const String& data)
{
    constexpr std::size_t kBufferSize = 64 * 1024;
    std::size_t offset = 0;
    while (offset < data.size()) {
        Thread::testCancel();
        const std::size_t count = std::min(kBufferSize, data.size() - offset);
        output.write(data.data() + offset, static_cast<std::streamsize>(count));
        if (output.fail()) {
            return false;
        }
        offset += count;
    }
    return true;
}

void remove_path_quietly(const barrier::fs::path& path)
{
    std::error_code error;
    barrier::fs::remove_all(path, error);
}

class ScopedPathCleanup {
public:
    explicit ScopedPathCleanup(barrier::fs::path path) :
        m_path(std::move(path)),
        m_active(true)
    {
    }

    ~ScopedPathCleanup()
    {
        if (m_active && !m_path.empty()) {
            remove_path_quietly(m_path);
        }
    }

    void cleanupNow()
    {
        if (m_active && !m_path.empty()) {
            remove_path_quietly(m_path);
        }
    }

    void dismiss()
    {
        m_active = false;
    }

private:
    barrier::fs::path m_path;
    bool m_active;
};

}

std::vector<String>
DropHelper::writeToDir(const String& destination, DragFileList& fileList, String& data)
{
    LOG((CLOG_DEBUG "dropping file, files=%i target=%s", fileList.size(), destination.c_str()));
    std::vector<String> droppedPaths;
    const auto clearTransferState = [&fileList, &data]() {
        fileList.clear();
        String().swap(data);
    };

    if (!destination.empty() && fileList.size() > 0) {
        const barrier::fs::path dropDirectory = barrier::fs::u8path(destination);
        barrier::fs::create_directories(dropDirectory);

        if (fileList.size() > 1 || fileList.at(0).isDirectory()) {
            std::string error;
            const barrier::fs::path stagingRoot =
                dropDirectory / barrier::fs::u8path(".barrier-unpack-" + uniqueToken());
            barrier::fs::create_directories(stagingRoot);
            ScopedPathCleanup stagingCleanup(stagingRoot);
            try {
                if (!TransferArchive::extractPackage(data, stagingRoot, error)) {
                    stagingCleanup.cleanupNow();
                    LOG((CLOG_ERR "drop directory failed: %s", error.c_str()));
                    clearTransferState();
                    return droppedPaths;
                }

                for (const auto& entry : barrier::fs::directory_iterator(stagingRoot)) {
                    Thread::testCancel();
                    const barrier::fs::path finalTarget =
                        unique_drop_target_path(dropDirectory, entry.path().filename().u8string());
                    barrier::fs::rename(entry.path(), finalTarget);
                    droppedPaths.push_back(finalTarget.u8string());
                }
                stagingCleanup.cleanupNow();
                stagingCleanup.dismiss();
            }
            catch (...) {
                clearTransferState();
                throw;
            }

            LOG((CLOG_INFO "dropped transfer bundle (%zu item(s)) in \"%s\"",
                 fileList.size(),
                 destination.c_str()));

            clearTransferState();
            return droppedPaths;
        }

        const barrier::fs::path dropTarget =
            unique_drop_target_path(dropDirectory, fileList.at(0).getFilename());
        barrier::fs::path tempTarget;
        if (!create_drop_temp_file(dropTarget.parent_path(), tempTarget)) {
            LOG((CLOG_ERR "drop file failed: can not create temporary file in %s",
                dropTarget.parent_path().u8string().c_str()));
            clearTransferState();
            return droppedPaths;
        }
        ScopedPathCleanup tempCleanup(tempTarget);

        std::ofstream file;
        barrier::open_utf8_path(file, tempTarget, std::ios::out | std::ios::binary | std::ios::app);
        if (!file.is_open()) {
            LOG((CLOG_ERR "drop file failed: can not open %s", tempTarget.u8string().c_str()));
            clearTransferState();
            return droppedPaths;
        }

        const bool writeOk = write_memory_payload(file, data);
        file.flush();
        file.close();

        if (!writeOk || file.fail()) {
            LOG((CLOG_ERR "drop file failed while writing %s", tempTarget.u8string().c_str()));
            clearTransferState();
            return droppedPaths;
        }

        barrier::fs::rename(tempTarget, dropTarget);
        tempCleanup.dismiss();
        droppedPaths.push_back(dropTarget.u8string());

        LOG((CLOG_INFO "dropped file \"%s\" in \"%s\"",
             dropTarget.filename().u8string().c_str(),
             destination.c_str()));

        clearTransferState();
    }
    else {
        LOG((CLOG_ERR "drop file failed: drop target is empty"));
        clearTransferState();
    }
    return droppedPaths;
}

std::vector<String>
DropHelper::writeToDirFromFile(const String& destination,
                               DragFileList& fileList,
                               const barrier::fs::path& sourcePath)
{
    LOG((CLOG_DEBUG "dropping spooled file, files=%i target=%s source=%s",
        fileList.size(), destination.c_str(), sourcePath.u8string().c_str()));
    std::vector<String> droppedPaths;
    const auto clearTransferState = [&fileList]() {
        fileList.clear();
    };

    if (destination.empty() || fileList.empty()) {
        LOG((CLOG_ERR "drop file failed: drop target is empty"));
        clearTransferState();
        return droppedPaths;
    }

    const barrier::fs::path dropDirectory = barrier::fs::u8path(destination);
    barrier::fs::create_directories(dropDirectory);

    if (fileList.size() > 1 || fileList.at(0).isDirectory()) {
        std::string error;
        const barrier::fs::path stagingRoot =
            dropDirectory / barrier::fs::u8path(".barrier-unpack-" + uniqueToken());
        barrier::fs::create_directories(stagingRoot);
        ScopedPathCleanup stagingCleanup(stagingRoot);
        try {
            if (!TransferArchive::extractPackageFile(sourcePath, stagingRoot, error)) {
                stagingCleanup.cleanupNow();
                LOG((CLOG_ERR "drop directory failed: %s", error.c_str()));
                clearTransferState();
                return droppedPaths;
            }

            for (const auto& entry : barrier::fs::directory_iterator(stagingRoot)) {
                Thread::testCancel();
                const barrier::fs::path finalTarget =
                    unique_drop_target_path(dropDirectory, entry.path().filename().u8string());
                barrier::fs::rename(entry.path(), finalTarget);
                droppedPaths.push_back(finalTarget.u8string());
            }
            stagingCleanup.cleanupNow();
            stagingCleanup.dismiss();
        }
        catch (...) {
            clearTransferState();
            throw;
        }

        LOG((CLOG_INFO "dropped spooled transfer bundle (%zu item(s)) in \"%s\"",
             fileList.size(),
             destination.c_str()));

        clearTransferState();
        return droppedPaths;
    }

    const barrier::fs::path dropTarget =
        unique_drop_target_path(dropDirectory, fileList.at(0).getFilename());
    barrier::fs::path tempTarget;
    if (!create_drop_temp_file(dropTarget.parent_path(), tempTarget)) {
        LOG((CLOG_ERR "drop file failed: can not create temporary file in %s",
            dropTarget.parent_path().u8string().c_str()));
        clearTransferState();
        return droppedPaths;
    }
    ScopedPathCleanup tempCleanup(tempTarget);

    if (!copy_file_payload(sourcePath, tempTarget)) {
        LOG((CLOG_ERR "drop file failed while copying %s", tempTarget.u8string().c_str()));
        clearTransferState();
        return droppedPaths;
    }

    barrier::fs::rename(tempTarget, dropTarget);
    tempCleanup.dismiss();
    droppedPaths.push_back(dropTarget.u8string());

    LOG((CLOG_INFO "dropped spooled file \"%s\" in \"%s\"",
         dropTarget.filename().u8string().c_str(),
         destination.c_str()));

    clearTransferState();
    return droppedPaths;
}
