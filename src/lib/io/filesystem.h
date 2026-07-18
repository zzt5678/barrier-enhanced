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

#ifndef BARRIER_LIB_IO_FILESYSTEM_H
#define BARRIER_LIB_IO_FILESYSTEM_H

#include <cstdio>
#include <iosfwd>
#include <ios>
#include <filesystem>
#include <system_error>

namespace barrier {

using namespace std::filesystem;
namespace fs = std::filesystem;

enum class RenameNoReplaceResult {
    kSuccess,
    kTargetExists,
    kError
};

void open_utf8_path(std::ifstream& stream, const fs::path& path,
                    std::ios_base::openmode mode = std::ios_base::in);
void open_utf8_path(std::ofstream& stream, const fs::path& path,
                    std::ios_base::openmode mode = std::ios_base::out);
void open_utf8_path(std::fstream& stream, const fs::path& path,
                    std::ios_base::openmode mode = std::ios_base::in | std::ios_base::out);

std::FILE* fopen_utf8_path(const fs::path& path, const std::string& mode);
bool create_secure_temp_file(const std::string& prefix, const std::string& suffix,
                             fs::path& path) noexcept;
bool create_secure_temp_file_in_directory(const fs::path& directory,
                                          const std::string& prefix,
                                          const std::string& suffix,
                                          fs::path& path) noexcept;
RenameNoReplaceResult rename_no_replace(const fs::path& source,
                                        const fs::path& target,
                                        std::error_code& error) noexcept;

} // namespace barrier

#endif // BARRIER_LIB_IO_FILESYSTEM_H
