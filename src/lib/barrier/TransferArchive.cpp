#include "barrier/TransferArchive.h"

#include "base/Log.h"
#include "io/filesystem.h"

#include <array>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <unordered_set>
#include <sstream>
#include <thread>

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

constexpr std::array<char, 8> kArchiveMagic{{'B', 'D', 'I', 'R', 'P', 'K', 'G', '1'}};
constexpr char kEntryDirectory = 'D';
constexpr char kEntryFile = 'F';
constexpr char kEntryEnd = 'E';

void writeUInt32(std::ofstream& stream, UInt32 value)
{
    const unsigned char bytes[4] = {
        static_cast<unsigned char>((value >> 24) & 0xffu),
        static_cast<unsigned char>((value >> 16) & 0xffu),
        static_cast<unsigned char>((value >> 8) & 0xffu),
        static_cast<unsigned char>(value & 0xffu),
    };
    stream.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void writeUInt64(std::ofstream& stream, std::uint64_t value)
{
    const unsigned char bytes[8] = {
        static_cast<unsigned char>((value >> 56) & 0xffu),
        static_cast<unsigned char>((value >> 48) & 0xffu),
        static_cast<unsigned char>((value >> 40) & 0xffu),
        static_cast<unsigned char>((value >> 32) & 0xffu),
        static_cast<unsigned char>((value >> 24) & 0xffu),
        static_cast<unsigned char>((value >> 16) & 0xffu),
        static_cast<unsigned char>((value >> 8) & 0xffu),
        static_cast<unsigned char>(value & 0xffu),
    };
    stream.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

bool readUInt32(const std::string& data, size_t& offset, UInt32& value)
{
    if (offset + 4 > data.size()) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data() + offset);
    value = (static_cast<UInt32>(bytes[0]) << 24) |
            (static_cast<UInt32>(bytes[1]) << 16) |
            (static_cast<UInt32>(bytes[2]) << 8) |
            static_cast<UInt32>(bytes[3]);
    offset += 4;
    return true;
}

bool readUInt64(const std::string& data, size_t& offset, std::uint64_t& value)
{
    if (offset + 8 > data.size()) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data() + offset);
    value = (static_cast<std::uint64_t>(bytes[0]) << 56) |
            (static_cast<std::uint64_t>(bytes[1]) << 48) |
            (static_cast<std::uint64_t>(bytes[2]) << 40) |
            (static_cast<std::uint64_t>(bytes[3]) << 32) |
            (static_cast<std::uint64_t>(bytes[4]) << 24) |
            (static_cast<std::uint64_t>(bytes[5]) << 16) |
            (static_cast<std::uint64_t>(bytes[6]) << 8) |
            static_cast<std::uint64_t>(bytes[7]);
    offset += 8;
    return true;
}

bool isSafeRelativePath(const barrier::fs::path& relativePath)
{
    if (relativePath.empty() || relativePath.is_absolute()) {
        return false;
    }

    for (const auto& part : relativePath) {
        if (part == "..") {
            return false;
        }
    }

    return true;
}

std::string uniqueRootName(const std::string& candidate,
                           std::unordered_set<std::string>& used)
{
    if (used.insert(candidate).second) {
        return candidate;
    }

    const barrier::fs::path path = barrier::fs::u8path(candidate);
    const std::string stem = path.stem().u8string();
    const std::string extension = path.extension().u8string();
    for (int i = 1; i < 1000; ++i) {
        const std::string next = stem + " (" + std::to_string(i) + ")" + extension;
        if (used.insert(next).second) {
            return next;
        }
    }

    return candidate;
}

void writeEntryHeader(std::ofstream& stream, char type, const std::string& relativePath)
{
    stream.put(type);
    writeUInt32(stream, static_cast<UInt32>(relativePath.size()));
    if (!relativePath.empty()) {
        stream.write(relativePath.data(), static_cast<std::streamsize>(relativePath.size()));
    }
}

bool copyFilePayload(std::ofstream& output, const barrier::fs::path& source, std::string& error)
{
    std::ifstream input;
    barrier::open_utf8_path(input, source, std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        error = "failed to open source file";
        return false;
    }

    constexpr std::size_t kBufferSize = 64 * 1024;
    std::array<char, kBufferSize> buffer{};
    while (input.good()) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            output.write(buffer.data(), count);
        }
    }

    if (!input.eof()) {
        error = "failed while reading source file";
        return false;
    }

    return true;
}

bool appendDirectory(std::ofstream& output,
                     const barrier::fs::path& sourceDir,
                     const std::string& rootName,
                     std::string& error)
{
    writeEntryHeader(output, kEntryDirectory, rootName);

    for (barrier::fs::recursive_directory_iterator it(sourceDir), end; it != end; ++it) {
        const barrier::fs::path entryPath = it->path();
        const barrier::fs::path relativePath = barrier::fs::relative(entryPath, sourceDir);
        const barrier::fs::path archivePath = barrier::fs::u8path(rootName) / relativePath;
        const std::string relative = archivePath.generic_u8string();

        if (it->is_symlink()) {
            error = "symbolic links are not supported in directory transfer";
            return false;
        }

        if (it->is_directory()) {
            writeEntryHeader(output, kEntryDirectory, relative);
            continue;
        }

        if (!it->is_regular_file()) {
            error = "directory transfer only supports regular files";
            return false;
        }

        writeEntryHeader(output, kEntryFile, relative);
        writeUInt64(output, static_cast<std::uint64_t>(it->file_size()));
        if (!copyFilePayload(output, entryPath, error)) {
            return false;
        }
    }

    return true;
}

bool appendFile(std::ofstream& output,
                const barrier::fs::path& sourceFile,
                const std::string& rootName,
                std::string& error)
{
    writeEntryHeader(output, kEntryFile, rootName);
    writeUInt64(output, static_cast<std::uint64_t>(barrier::fs::file_size(sourceFile)));
    return copyFilePayload(output, sourceFile, error);
}

} // namespace

bool
TransferArchive::createSelectionPackageFile(const std::vector<barrier::fs::path>& sourcePaths,
                                            barrier::fs::path& packagePath,
                                            std::string& error)
{
    error.clear();
    packagePath.clear();

    if (sourcePaths.empty()) {
        error = "transfer selection is empty";
        return false;
    }

    const auto tempName = std::string("selection-") + uniqueToken() + ".bdir";
    packagePath = barrier::fs::temp_directory_path() / barrier::fs::u8path(tempName);

    std::ofstream output;
    barrier::open_utf8_path(output, packagePath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        error = "failed to open temporary package";
        return false;
    }

    output.write(kArchiveMagic.data(), static_cast<std::streamsize>(kArchiveMagic.size()));

    std::unordered_set<std::string> usedRoots;
    for (const barrier::fs::path& sourcePath : sourcePaths) {
        if (!barrier::fs::exists(sourcePath)) {
            error = "transfer source does not exist";
            output.close();
            barrier::fs::remove(packagePath);
            packagePath.clear();
            return false;
        }

        const std::string rootName =
            uniqueRootName(sourcePath.filename().u8string(), usedRoots);
        const bool ok = barrier::fs::is_directory(sourcePath)
            ? appendDirectory(output, sourcePath, rootName, error)
            : appendFile(output, sourcePath, rootName, error);
        if (!ok) {
            output.close();
            barrier::fs::remove(packagePath);
            packagePath.clear();
            return false;
        }
    }

    output.put(kEntryEnd);
    output.flush();
    output.close();

    if (output.fail()) {
        error = "failed to finalize temporary package";
        barrier::fs::remove(packagePath);
        packagePath.clear();
        return false;
    }

    return true;
}

bool
TransferArchive::createDirectoryPackageFile(const barrier::fs::path& sourceDir,
                                            barrier::fs::path& packagePath,
                                            std::string& error)
{
    return createSelectionPackageFile({sourceDir}, packagePath, error);
}

bool
TransferArchive::extractPackage(const std::string& packageData,
                                const barrier::fs::path& destinationRoot,
                                std::string& error)
{
    error.clear();

    if (packageData.size() < kArchiveMagic.size() ||
        !std::equal(kArchiveMagic.begin(), kArchiveMagic.end(), packageData.begin())) {
        error = "invalid transfer package header";
        return false;
    }

    size_t offset = kArchiveMagic.size();
    while (offset < packageData.size()) {
        const char type = packageData[offset++];
        if (type == kEntryEnd) {
            return true;
        }

        UInt32 pathSize = 0;
        if (!readUInt32(packageData, offset, pathSize) || offset + pathSize > packageData.size()) {
            error = "invalid transfer package path";
            return false;
        }

        const std::string relativeUtf8 = packageData.substr(offset, pathSize);
        offset += pathSize;

        const barrier::fs::path relativePath = barrier::fs::u8path(relativeUtf8).lexically_normal();
        if (!isSafeRelativePath(relativePath)) {
            error = "unsafe path in transfer package";
            return false;
        }

        const barrier::fs::path targetPath = (destinationRoot / relativePath).lexically_normal();

        if (type == kEntryDirectory) {
            barrier::fs::create_directories(targetPath);
            continue;
        }

        if (type != kEntryFile) {
            error = "unknown transfer package entry type";
            return false;
        }

        std::uint64_t fileSize = 0;
        if (!readUInt64(packageData, offset, fileSize) ||
            offset + static_cast<size_t>(fileSize) > packageData.size()) {
            error = "invalid transfer package file payload";
            return false;
        }

        barrier::fs::create_directories(targetPath.parent_path());
        std::ofstream output;
        barrier::open_utf8_path(output, targetPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            error = "failed to create extracted file";
            return false;
        }

        output.write(packageData.data() + offset, static_cast<std::streamsize>(fileSize));
        output.flush();
        output.close();
        if (output.fail()) {
            error = "failed to write extracted file";
            return false;
        }

        offset += static_cast<size_t>(fileSize);
    }

    error = "transfer package missing end marker";
    return false;
}

bool
TransferArchive::extractDirectoryPackage(const std::string& packageData,
                                         const barrier::fs::path& destinationRoot,
                                         std::string& error)
{
    return extractPackage(packageData, destinationRoot, error);
}
