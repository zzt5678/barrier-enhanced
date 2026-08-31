/*
    barrier -- mouse and keyboard sharing utility
    Copyright (C) Barrier contributors

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

#include "filesystem.h"
#if SYSAPI_WIN32
#define WIN32_LEAN_AND_MEAN
#include "common/win32/encoding_utilities.h"
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#elif defined(__APPLE__)
#include <stdio.h>
#endif
#endif
#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

namespace barrier {

namespace {

template<class Stream>
void open_utf8_path_impl(Stream& stream, const fs::path& path, std::ios_base::openmode mode)
{
#if SYSAPI_WIN32
    // on Windows we need to use a non-standard constructor from wchar_t* string
    // which fs::path::native() returns
    stream.open(path.native().c_str(), mode);
#else
    stream.open(path.native().c_str(), mode);
#endif
}

} // namespace

void open_utf8_path(std::ifstream& stream, const fs::path& path, std::ios_base::openmode mode)
{
    open_utf8_path_impl(stream, path, mode);
}

void open_utf8_path(std::ofstream& stream, const fs::path& path, std::ios_base::openmode mode)
{
    open_utf8_path_impl(stream, path, mode);
}

void open_utf8_path(std::fstream& stream, const fs::path& path, std::ios_base::openmode mode)
{
    open_utf8_path_impl(stream, path, mode);
}

std::FILE* fopen_utf8_path(const fs::path& path, const std::string& mode)
{
#if SYSAPI_WIN32
    auto wchar_mode = utf8_to_win_char(mode);
    return _wfopen(path.native().c_str(),
                   reinterpret_cast<wchar_t*>(wchar_mode.data()));
#else
    return std::fopen(path.native().c_str(), mode.c_str());
#endif
}

bool create_secure_temp_file(const std::string& prefix, const std::string& suffix,
                             fs::path& path) noexcept
{
    path.clear();

    try {
        std::error_code error;
        const fs::path directory = fs::temp_directory_path(error);
        return !error && !directory.empty() &&
            create_secure_temp_file_in_directory(
                directory, prefix, suffix, path);
    }
    catch (...) {
        path.clear();
        return false;
    }
}

bool create_secure_temp_file_in_directory(const fs::path& directory,
                                          const std::string& prefix,
                                          const std::string& suffix,
                                          fs::path& path) noexcept
{
    path.clear();

    try {
#if SYSAPI_WIN32
    if (directory.empty()) {
        return false;
    }

    std::random_device randomDevice;
    for (int attempt = 0; attempt < 128; ++attempt) {
        std::wostringstream name;
        name << directory.native();
        if (name.tellp() > 0) {
            const wchar_t last = name.str().back();
            if (last != L'\\' && last != L'/') {
                name << L'\\';
            }
        }
        for (char c : prefix) {
            name << static_cast<wchar_t>(c);
        }
        name << std::hex
             << static_cast<unsigned long long>(
                    std::chrono::steady_clock::now().time_since_epoch().count())
             << L"-"
             << static_cast<unsigned long long>(GetCurrentProcessId())
             << L"-"
             << static_cast<unsigned long long>(randomDevice());
        for (char c : suffix) {
            name << static_cast<wchar_t>(c);
        }

        const std::wstring candidate = name.str();
        HANDLE handle = CreateFileW(candidate.c_str(),
                                    GENERIC_READ | GENERIC_WRITE,
                                    0,
                                    NULL,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                                    NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            path = fs::path(candidate);
            return true;
        }
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return false;
#else
    if (directory.empty()) {
        return false;
    }

    std::string native = (directory / fs::u8path(prefix + "XXXXXX" + suffix)).native();
    std::vector<char> buffer(native.begin(), native.end());
    buffer.push_back('\0');

    int fd;
    if (suffix.empty()) {
        fd = mkstemp(buffer.data());
    }
    else {
        fd = mkstemps(buffer.data(), static_cast<int>(suffix.size()));
    }
    if (fd < 0) {
        return false;
    }

    fchmod(fd, S_IRUSR | S_IWUSR);
    close(fd);
    path = fs::path(buffer.data());
    return true;
#endif
    }
    catch (...) {
        path.clear();
        return false;
    }
}

RenameNoReplaceResult rename_no_replace(const fs::path& source,
                                        const fs::path& target,
                                        std::error_code& error) noexcept
{
    error.clear();
    try {
#if SYSAPI_WIN32
        if (MoveFileExW(source.native().c_str(), target.native().c_str(),
                        MOVEFILE_WRITE_THROUGH) != FALSE) {
            return RenameNoReplaceResult::kSuccess;
        }
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
            return RenameNoReplaceResult::kTargetExists;
        }
        error = std::error_code(static_cast<int>(code), std::system_category());
        return RenameNoReplaceResult::kError;
#elif defined(__linux__)
        if (::syscall(SYS_renameat2, AT_FDCWD, source.native().c_str(),
                      AT_FDCWD, target.native().c_str(),
                      RENAME_NOREPLACE) == 0) {
            return RenameNoReplaceResult::kSuccess;
        }
        if (errno == EEXIST || errno == ENOTEMPTY) {
            return RenameNoReplaceResult::kTargetExists;
        }
        error = std::error_code(errno, std::generic_category());
        return RenameNoReplaceResult::kError;
#elif defined(__APPLE__)
        if (::renamex_np(source.native().c_str(), target.native().c_str(),
                         RENAME_EXCL) == 0) {
            return RenameNoReplaceResult::kSuccess;
        }
        if (errno == EEXIST || errno == ENOTEMPTY) {
            return RenameNoReplaceResult::kTargetExists;
        }
        error = std::error_code(errno, std::generic_category());
        return RenameNoReplaceResult::kError;
#else
        // A hard link gives regular files an atomic, no-replace fallback.
        // Directory publication fails closed on platforms without such a
        // primitive instead of using POSIX rename's overwrite semantics.
        std::error_code statusError;
        if (!fs::is_regular_file(source, statusError) || statusError) {
            error = std::make_error_code(std::errc::operation_not_supported);
            return RenameNoReplaceResult::kError;
        }
        fs::create_hard_link(source, target, error);
        if (error == std::errc::file_exists) {
            error.clear();
            return RenameNoReplaceResult::kTargetExists;
        }
        if (error) {
            return RenameNoReplaceResult::kError;
        }
        std::error_code removeError;
        if (!fs::remove(source, removeError) || removeError) {
            std::error_code rollbackError;
            fs::remove(target, rollbackError);
            error = removeError ? removeError :
                std::make_error_code(std::errc::io_error);
            return RenameNoReplaceResult::kError;
        }
        return RenameNoReplaceResult::kSuccess;
#endif
    }
    catch (const std::system_error& exception) {
        error = exception.code();
    }
    catch (...) {
        error = std::make_error_code(std::errc::io_error);
    }
    return RenameNoReplaceResult::kError;
}

} // namespace barrier
