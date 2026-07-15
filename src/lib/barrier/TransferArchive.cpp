#include "barrier/TransferArchive.h"

#include "base/Log.h"
#include "base/Unicode.h"
#include "io/filesystem.h"
#include "mt/Thread.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <unordered_set>

#if defined(WINAPI_MSWINDOWS)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace {

constexpr std::array<char, 8> kArchiveMagic{{'B', 'D', 'I', 'R', 'P', 'K', 'G', '1'}};
constexpr char kEntryDirectory = 'D';
constexpr char kEntryFile = 'F';
constexpr char kEntryEnd = 'E';
constexpr UInt32 kMaxEntryPathBytes = 64 * 1024;
constexpr std::size_t kMaxEntryCount = 64 * 1024;
constexpr std::size_t kMaxEntryDepth = 64;
constexpr std::uint64_t kMaxTotalPathBytes = 16 * 1024 * 1024;
constexpr std::uint64_t kMaxExpandedBytes = 512 * 1024 * 1024;
constexpr std::uint64_t kMaxPackageBytes = 512 * 1024 * 1024;

struct ArchiveValidationState {
    std::size_t entryCount = 0;
    std::uint64_t totalPathBytes = 0;
    std::uint64_t expandedBytes = 0;
    std::uint64_t encodedBytes = kArchiveMagic.size() + 1;
    std::unordered_set<std::string> entries;
    std::unordered_set<std::string> files;
    std::unordered_set<std::string> parentPaths;
};

bool addEncodedBytes(std::uint64_t byteCount,
                     ArchiveValidationState& state,
                     std::string& error)
{
    if (byteCount > kMaxPackageBytes - state.encodedBytes) {
        error = "transfer package exceeds size limit";
        return false;
    }
    state.encodedBytes += byteCount;
    return true;
}

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

bool readExact(std::ifstream& stream, char* buffer, std::size_t size)
{
    stream.read(buffer, static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(stream.gcount()) == size;
}

bool readUInt32(std::ifstream& stream, UInt32& value)
{
    unsigned char bytes[4] = {};
    if (!readExact(stream, reinterpret_cast<char*>(bytes), sizeof(bytes))) {
        return false;
    }
    value = (static_cast<UInt32>(bytes[0]) << 24) |
            (static_cast<UInt32>(bytes[1]) << 16) |
            (static_cast<UInt32>(bytes[2]) << 8) |
            static_cast<UInt32>(bytes[3]);
    return true;
}

bool readUInt64(std::ifstream& stream, std::uint64_t& value)
{
    unsigned char bytes[8] = {};
    if (!readExact(stream, reinterpret_cast<char*>(bytes), sizeof(bytes))) {
        return false;
    }
    value = (static_cast<std::uint64_t>(bytes[0]) << 56) |
            (static_cast<std::uint64_t>(bytes[1]) << 48) |
            (static_cast<std::uint64_t>(bytes[2]) << 40) |
            (static_cast<std::uint64_t>(bytes[3]) << 32) |
            (static_cast<std::uint64_t>(bytes[4]) << 24) |
            (static_cast<std::uint64_t>(bytes[5]) << 16) |
            (static_cast<std::uint64_t>(bytes[6]) << 8) |
            static_cast<std::uint64_t>(bytes[7]);
    return true;
}

bool hasRemainingBytes(std::ifstream& stream,
                       std::uint64_t totalSize,
                       std::uint64_t byteCount)
{
    const std::streampos position = stream.tellg();
    if (position < 0) {
        return false;
    }

    const std::uint64_t offset = static_cast<std::uint64_t>(position);
    return offset <= totalSize && byteCount <= totalSize - offset;
}

bool isSafeRelativePath(const barrier::fs::path& relativePath)
{
    if (relativePath.empty() || relativePath == "." || relativePath.is_absolute() ||
        relativePath.has_root_name() || relativePath.has_root_directory()) {
        return false;
    }

    for (const auto& part : relativePath) {
        if (part == "..") {
            return false;
        }
    }

    return true;
}

bool isReparsePath(const barrier::fs::path& path,
                   const barrier::fs::file_status& status)
{
    if (barrier::fs::is_symlink(status)) {
        return true;
    }
#if defined(WINAPI_MSWINDOWS)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    (void)path;
    return false;
#endif
}

bool readPathStatus(const barrier::fs::path& path,
                    barrier::fs::file_status& status,
                    std::string& error)
{
    std::error_code statusError;
    status = barrier::fs::symlink_status(path, statusError);
    if (statusError && statusError != std::errc::no_such_file_or_directory) {
        error = "failed to inspect extraction path";
        return false;
    }
    return true;
}

bool ensureExtractionRoot(const barrier::fs::path& destinationRoot,
                          std::string& error)
{
    if (destinationRoot.empty()) {
        error = "unsafe extraction root";
        return false;
    }

    barrier::fs::file_status status;
    if (!readPathStatus(destinationRoot, status, error)) {
        return false;
    }
    if (barrier::fs::exists(status)) {
        if (isReparsePath(destinationRoot, status) || !barrier::fs::is_directory(status)) {
            error = "unsafe extraction root";
            return false;
        }
        return true;
    }

    std::error_code createError;
    barrier::fs::create_directories(destinationRoot, createError);
    if (createError || !readPathStatus(destinationRoot, status, error) ||
        !barrier::fs::is_directory(status) || isReparsePath(destinationRoot, status)) {
        error = "unsafe extraction root";
        return false;
    }
    return true;
}

bool ensureExtractionDirectory(const barrier::fs::path& destinationRoot,
                               const barrier::fs::path& relativeDirectory,
                               std::string& error)
{
    barrier::fs::path current = destinationRoot;
    for (const auto& component : relativeDirectory) {
        if (component == ".") {
            continue;
        }
        current /= component;

        barrier::fs::file_status status;
        if (!readPathStatus(current, status, error)) {
            return false;
        }
        if (barrier::fs::exists(status)) {
            if (isReparsePath(current, status) || !barrier::fs::is_directory(status)) {
                error = "unsafe extraction path";
                return false;
            }
            continue;
        }

        std::error_code createError;
        if (!barrier::fs::create_directory(current, createError) || createError ||
            !readPathStatus(current, status, error) ||
            !barrier::fs::is_directory(status) || isReparsePath(current, status)) {
            error = "unsafe extraction path";
            return false;
        }
    }
    return true;
}

bool prepareExtractionFile(const barrier::fs::path& destinationRoot,
                           const barrier::fs::path& relativePath,
                           std::string& error)
{
    if (!ensureExtractionDirectory(destinationRoot, relativePath.parent_path(), error)) {
        return false;
    }

    const barrier::fs::path targetPath = destinationRoot / relativePath;
    barrier::fs::file_status status;
    if (!readPathStatus(targetPath, status, error)) {
        return false;
    }
    if (!barrier::fs::exists(status)) {
        return true;
    }
    error = isReparsePath(targetPath, status)
        ? "unsafe extraction path"
        : "extraction target already exists";
    return false;
}

std::string archivePathKey(const barrier::fs::path& relativePath)
{
    std::string key = relativePath.generic_u8string();
#if defined(WINAPI_MSWINDOWS)
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
#endif
    return key;
}

#if defined(WINAPI_MSWINDOWS)
bool isReservedWindowsName(const std::string& component)
{
    const std::size_t extension = component.find('.');
    std::string base = component.substr(0, extension);
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char value) {
        return static_cast<char>(std::toupper(value));
    });

    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL") {
        return true;
    }
    if (base.size() == 4 && base[3] >= '1' && base[3] <= '9') {
        return base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0;
    }
    return false;
}

bool isSafeWindowsPath(const barrier::fs::path& relativePath)
{
    for (const auto& part : relativePath) {
        const std::string component = part.u8string();
        if (component.empty() || component.back() == '.' || component.back() == ' ' ||
            isReservedWindowsName(component)) {
            return false;
        }

        for (unsigned char value : component) {
            if (value < 32 || value == '<' || value == '>' || value == ':' ||
                value == '"' || value == '|' || value == '?' || value == '*') {
                return false;
            }
        }
    }
    return true;
}
#endif

bool validateEntryPath(const std::string& relativeUtf8,
                       char type,
                       ArchiveValidationState& state,
                       barrier::fs::path& relativePath,
                       std::string& error)
{
    if (type != kEntryDirectory && type != kEntryFile) {
        error = "unknown transfer package entry type";
        return false;
    }
    if (++state.entryCount > kMaxEntryCount) {
        error = "transfer package has too many entries";
        return false;
    }
    if (relativeUtf8.find('\0') != std::string::npos ||
        relativeUtf8.find('\\') != std::string::npos) {
        error = "unsafe path in transfer package";
        return false;
    }
    if (!Unicode::isUTF8(relativeUtf8)) {
        error = "invalid transfer package path encoding";
        return false;
    }
    if (relativeUtf8.size() > kMaxTotalPathBytes - state.totalPathBytes) {
        error = "transfer package paths exceed size limit";
        return false;
    }
    state.totalPathBytes += relativeUtf8.size();
    if (!addEncodedBytes(1 + 4 + relativeUtf8.size(), state, error)) {
        return false;
    }

    try {
        relativePath = barrier::fs::u8path(relativeUtf8).lexically_normal();
    }
    catch (...) {
        error = "invalid transfer package path encoding";
        return false;
    }
#if defined(WINAPI_MSWINDOWS)
    if (relativePath.has_root_name() || relativePath.has_root_directory()) {
        error = "unsafe Windows path in transfer package";
        return false;
    }
#endif
    if (!isSafeRelativePath(relativePath)) {
        error = "unsafe path in transfer package";
        return false;
    }

    std::size_t depth = 0;
    for (const auto& part : relativePath) {
        if (part != ".") {
            ++depth;
        }
    }
    if (depth > kMaxEntryDepth) {
        error = "transfer package path is too deep";
        return false;
    }

#if defined(WINAPI_MSWINDOWS)
    if (!isSafeWindowsPath(relativePath)) {
        error = "unsafe Windows path in transfer package";
        return false;
    }
#endif

    const std::string key = archivePathKey(relativePath);
    if (state.entries.find(key) != state.entries.end()) {
        error = "duplicate or conflicting path in transfer package";
        return false;
    }

    std::size_t separator = key.find('/');
    while (separator != std::string::npos) {
        const std::string parent = key.substr(0, separator);
        if (state.files.find(parent) != state.files.end()) {
            error = "duplicate or conflicting path in transfer package";
            return false;
        }
        separator = key.find('/', separator + 1);
    }
    if (type == kEntryFile && state.parentPaths.find(key) != state.parentPaths.end()) {
        error = "duplicate or conflicting path in transfer package";
        return false;
    }

    state.entries.insert(key);
    separator = key.find('/');
    while (separator != std::string::npos) {
        state.parentPaths.insert(key.substr(0, separator));
        separator = key.find('/', separator + 1);
    }
    if (type == kEntryFile) {
        state.files.insert(key);
    }
    return true;
}

bool validateFileSize(std::uint64_t fileSize,
                      ArchiveValidationState& state,
                      std::string& error)
{
    if (fileSize > kMaxExpandedBytes - state.expandedBytes) {
        error = "transfer package expanded size exceeds limit";
        return false;
    }
    state.expandedBytes += fileSize;
    return addEncodedBytes(8 + fileSize, state, error);
}

bool validatePackageData(const std::string& packageData, std::string& error)
{
    if (packageData.size() > kMaxPackageBytes) {
        error = "transfer package exceeds size limit";
        return false;
    }
    if (packageData.size() < kArchiveMagic.size() ||
        !std::equal(kArchiveMagic.begin(), kArchiveMagic.end(), packageData.begin())) {
        error = "invalid transfer package header";
        return false;
    }

    ArchiveValidationState state;
    std::size_t offset = kArchiveMagic.size();
    while (offset < packageData.size()) {
        Thread::testCancel();
        const char type = packageData[offset++];
        if (type == kEntryEnd) {
            if (offset != packageData.size()) {
                error = "transfer package has trailing data";
                return false;
            }
            return true;
        }

        UInt32 pathSize = 0;
        if (!readUInt32(packageData, offset, pathSize) ||
            pathSize > kMaxEntryPathBytes ||
            pathSize > packageData.size() - offset) {
            error = "invalid transfer package path";
            return false;
        }

        const std::string relativeUtf8 = packageData.substr(offset, pathSize);
        offset += pathSize;
        barrier::fs::path relativePath;
        if (!validateEntryPath(relativeUtf8, type, state, relativePath, error)) {
            return false;
        }

        if (type == kEntryFile) {
            std::uint64_t fileSize = 0;
            if (!readUInt64(packageData, offset, fileSize) ||
                fileSize > static_cast<std::uint64_t>(packageData.size() - offset)) {
                error = "invalid transfer package file payload";
                return false;
            }
            if (!validateFileSize(fileSize, state, error)) {
                return false;
            }
            offset += static_cast<std::size_t>(fileSize);
        }
    }

    error = "transfer package missing end marker";
    return false;
}

bool validatePackageStream(std::ifstream& input,
                           std::uint64_t packageSize,
                           std::string& error)
{
    if (packageSize > kMaxPackageBytes) {
        error = "transfer package exceeds size limit";
        return false;
    }
    std::array<char, kArchiveMagic.size()> magic{};
    if (!readExact(input, magic.data(), magic.size()) ||
        !std::equal(kArchiveMagic.begin(), kArchiveMagic.end(), magic.begin())) {
        error = "invalid transfer package header";
        return false;
    }

    ArchiveValidationState state;
    while (true) {
        Thread::testCancel();
        char type = 0;
        input.get(type);
        if (!input.good()) {
            error = "transfer package missing end marker";
            return false;
        }
        if (type == kEntryEnd) {
            const std::streampos position = input.tellg();
            if (position < 0 || static_cast<std::uint64_t>(position) != packageSize) {
                error = "transfer package has trailing data";
                return false;
            }
            return true;
        }

        UInt32 pathSize = 0;
        if (!readUInt32(input, pathSize) || pathSize > kMaxEntryPathBytes ||
            !hasRemainingBytes(input, packageSize, pathSize)) {
            error = "invalid transfer package path";
            return false;
        }

        std::string relativeUtf8(pathSize, '\0');
        if (pathSize > 0 && !readExact(input, &relativeUtf8[0], pathSize)) {
            error = "invalid transfer package path";
            return false;
        }
        barrier::fs::path relativePath;
        if (!validateEntryPath(relativeUtf8, type, state, relativePath, error)) {
            return false;
        }

        if (type == kEntryFile) {
            std::uint64_t fileSize = 0;
            if (!readUInt64(input, fileSize) ||
                !hasRemainingBytes(input, packageSize, fileSize)) {
                error = "invalid transfer package file payload";
                return false;
            }
            if (!validateFileSize(fileSize, state, error) ||
                fileSize > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)())) {
                if (error.empty()) {
                    error = "invalid transfer package file payload";
                }
                return false;
            }
            input.seekg(static_cast<std::streamoff>(fileSize), std::ios::cur);
            if (!input.good()) {
                error = "invalid transfer package file payload";
                return false;
            }
        }
    }
}

std::string uniqueRootName(const std::string& candidate,
                           std::unordered_set<std::string>& used)
{
    if (used.insert(archivePathKey(barrier::fs::u8path(candidate))).second) {
        return candidate;
    }

    const barrier::fs::path path = barrier::fs::u8path(candidate);
    const std::string stem = path.stem().u8string();
    const std::string extension = path.extension().u8string();
    for (int i = 1; i < 1000; ++i) {
        const std::string next = stem + " (" + std::to_string(i) + ")" + extension;
        if (used.insert(archivePathKey(barrier::fs::u8path(next))).second) {
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

bool copyFilePayload(std::ofstream& output,
                     const barrier::fs::path& source,
                     std::uint64_t expectedSize,
                     std::string& error)
{
    std::ifstream input;
    barrier::open_utf8_path(input, source, std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        error = "failed to open source file";
        return false;
    }

    constexpr std::size_t kBufferSize = 64 * 1024;
    std::array<char, kBufferSize> buffer{};
    std::uint64_t remaining = expectedSize;
    while (remaining > 0) {
        Thread::testCancel();
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), static_cast<std::streamsize>(count));
        if (input.gcount() != static_cast<std::streamsize>(count)) {
            error = "source file changed during packaging";
            return false;
        }
        output.write(buffer.data(), static_cast<std::streamsize>(count));
        if (!output.good()) {
            error = "failed while writing transfer package";
            return false;
        }
        remaining -= count;
    }

    if (input.peek() != std::char_traits<char>::eof()) {
        error = "source file changed during packaging";
        return false;
    }
    if (!input.eof()) {
        error = "failed while reading source file";
        return false;
    }

    return true;
}

bool copyStreamPayload(std::ifstream& input,
                       std::ofstream& output,
                       std::uint64_t fileSize,
                       std::string& error)
{
    constexpr std::size_t kBufferSize = 64 * 1024;
    std::array<char, kBufferSize> buffer{};
    std::uint64_t remaining = fileSize;
    while (remaining > 0) {
        Thread::testCancel();
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        if (!readExact(input, buffer.data(), count)) {
            error = "invalid transfer package file payload";
            return false;
        }
        output.write(buffer.data(), static_cast<std::streamsize>(count));
        remaining -= count;
    }
    return true;
}

bool appendDirectory(std::ofstream& output,
                     const barrier::fs::path& sourceDir,
                     const std::string& rootName,
                     ArchiveValidationState& state,
                     std::string& error)
{
    barrier::fs::path validatedPath;
    if (!validateEntryPath(rootName, kEntryDirectory, state, validatedPath, error)) {
        return false;
    }
    writeEntryHeader(output, kEntryDirectory, rootName);

    for (barrier::fs::recursive_directory_iterator it(sourceDir), end; it != end; ++it) {
        Thread::testCancel();
        const barrier::fs::path entryPath = it->path();
        const barrier::fs::path relativePath = barrier::fs::relative(entryPath, sourceDir);
        const barrier::fs::path archivePath = barrier::fs::u8path(rootName) / relativePath;
        const std::string relative = archivePath.generic_u8string();

        if (it->is_symlink()) {
            error = "symbolic links are not supported in directory transfer";
            return false;
        }

        if (it->is_directory()) {
            if (!validateEntryPath(relative, kEntryDirectory, state, validatedPath, error)) {
                return false;
            }
            writeEntryHeader(output, kEntryDirectory, relative);
            continue;
        }

        if (!it->is_regular_file()) {
            error = "directory transfer only supports regular files";
            return false;
        }

        const std::uint64_t fileSize = static_cast<std::uint64_t>(it->file_size());
        if (!validateEntryPath(relative, kEntryFile, state, validatedPath, error) ||
            !validateFileSize(fileSize, state, error)) {
            return false;
        }
        writeEntryHeader(output, kEntryFile, relative);
        writeUInt64(output, fileSize);
        if (!copyFilePayload(output, entryPath, fileSize, error)) {
            return false;
        }
    }

    return true;
}

bool appendFile(std::ofstream& output,
                const barrier::fs::path& sourceFile,
                const std::string& rootName,
                ArchiveValidationState& state,
                std::string& error)
{
    barrier::fs::path validatedPath;
    const std::uint64_t fileSize = static_cast<std::uint64_t>(barrier::fs::file_size(sourceFile));
    if (!validateEntryPath(rootName, kEntryFile, state, validatedPath, error) ||
        !validateFileSize(fileSize, state, error)) {
        return false;
    }
    writeEntryHeader(output, kEntryFile, rootName);
    writeUInt64(output, fileSize);
    return copyFilePayload(output, sourceFile, fileSize, error);
}

} // namespace

bool
TransferArchive::isPackageData(const std::string& data)
{
    return data.size() >= kArchiveMagic.size() &&
           std::equal(kArchiveMagic.begin(), kArchiveMagic.end(), data.begin());
}

bool
TransferArchive::isPackageFile(const barrier::fs::path& packagePath)
{
    std::ifstream input;
    barrier::open_utf8_path(input, packagePath, std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        return false;
    }

    std::array<char, kArchiveMagic.size()> magic{};
    if (!readExact(input, magic.data(), magic.size())) {
        return false;
    }

    return std::equal(kArchiveMagic.begin(), kArchiveMagic.end(), magic.begin());
}

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

    if (!barrier::create_secure_temp_file("selection-", ".bdir", packagePath)) {
        error = "failed to create temporary package";
        return false;
    }

    std::ofstream output;
    barrier::open_utf8_path(output, packagePath, std::ios::out | std::ios::binary | std::ios::app);
    if (!output.is_open()) {
        error = "failed to open temporary package";
        barrier::fs::remove(packagePath);
        packagePath.clear();
        return false;
    }

    output.write(kArchiveMagic.data(), static_cast<std::streamsize>(kArchiveMagic.size()));

    std::unordered_set<std::string> usedRoots;
    ArchiveValidationState validationState;
    for (const barrier::fs::path& sourcePath : sourcePaths) {
        Thread::testCancel();
        if (!barrier::fs::exists(sourcePath)) {
            error = "transfer source does not exist";
            output.close();
            barrier::fs::remove(packagePath);
            packagePath.clear();
            return false;
        }

        if (barrier::fs::is_symlink(sourcePath)) {
            error = "symbolic links are not supported in file transfer";
            output.close();
            barrier::fs::remove(packagePath);
            packagePath.clear();
            return false;
        }

        const std::string rootName =
            uniqueRootName(sourcePath.filename().u8string(), usedRoots);
        const bool ok = barrier::fs::is_directory(sourcePath)
            ? appendDirectory(output, sourcePath, rootName, validationState, error)
            : appendFile(output, sourcePath, rootName, validationState, error);
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
    if (!validatePackageData(packageData, error)) {
        return false;
    }
    if (!ensureExtractionRoot(destinationRoot, error)) {
        return false;
    }

    ArchiveValidationState extractionState;
    size_t offset = kArchiveMagic.size();
    while (offset < packageData.size()) {
        Thread::testCancel();
        const char type = packageData[offset++];
        if (type == kEntryEnd) {
            return offset == packageData.size();
        }

        UInt32 pathSize = 0;
        if (!readUInt32(packageData, offset, pathSize) ||
            pathSize > kMaxEntryPathBytes ||
            offset + pathSize > packageData.size()) {
            error = "invalid transfer package path";
            return false;
        }

        const std::string relativeUtf8 = packageData.substr(offset, pathSize);
        offset += pathSize;

        barrier::fs::path relativePath;
        if (!validateEntryPath(relativeUtf8, type, extractionState, relativePath, error)) {
            return false;
        }

        const barrier::fs::path targetPath = (destinationRoot / relativePath).lexically_normal();

        if (type == kEntryDirectory) {
            if (!ensureExtractionDirectory(destinationRoot, relativePath, error)) {
                return false;
            }
            continue;
        }

        if (type != kEntryFile) {
            error = "unknown transfer package entry type";
            return false;
        }

        std::uint64_t fileSize = 0;
        if (!readUInt64(packageData, offset, fileSize) ||
            fileSize > static_cast<std::uint64_t>(packageData.size() - offset)) {
            error = "invalid transfer package file payload";
            return false;
        }
        if (!validateFileSize(fileSize, extractionState, error)) {
            return false;
        }

        if (!prepareExtractionFile(destinationRoot, relativePath, error)) {
            return false;
        }
        std::ofstream output;
        barrier::open_utf8_path(output, targetPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            error = "failed to create extracted file";
            return false;
        }

        constexpr std::size_t kWriteChunkSize = 64 * 1024;
        std::uint64_t remaining = fileSize;
        while (remaining > 0) {
            Thread::testCancel();
            const std::size_t count = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, kWriteChunkSize));
            output.write(packageData.data() + offset, static_cast<std::streamsize>(count));
            offset += count;
            remaining -= count;
        }
        output.flush();
        output.close();
        if (output.fail()) {
            error = "failed to write extracted file";
            return false;
        }
    }

    error = "transfer package missing end marker";
    return false;
}

bool
TransferArchive::extractPackageFile(const barrier::fs::path& packagePath,
                                    const barrier::fs::path& destinationRoot,
                                    std::string& error)
{
    error.clear();

    std::ifstream input;
    barrier::open_utf8_path(input, packagePath, std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        error = "failed to open transfer package";
        return false;
    }

    std::uint64_t packageSize = 0;
    try {
        packageSize = static_cast<std::uint64_t>(barrier::fs::file_size(packagePath));
    }
    catch (...) {
        error = "failed to inspect transfer package";
        return false;
    }

    if (!validatePackageStream(input, packageSize, error)) {
        return false;
    }
    if (!ensureExtractionRoot(destinationRoot, error)) {
        return false;
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(kArchiveMagic.size()), std::ios::beg);
    if (!input.good()) {
        error = "failed to read transfer package";
        return false;
    }

    ArchiveValidationState extractionState;
    while (true) {
        Thread::testCancel();
        char type = 0;
        input.get(type);
        if (!input.good()) {
            error = "transfer package missing end marker";
            return false;
        }
        if (type == kEntryEnd) {
            const std::streampos position = input.tellg();
            if (position < 0 || static_cast<std::uint64_t>(position) != packageSize) {
                error = "transfer package has trailing data";
                return false;
            }
            return true;
        }

        UInt32 pathSize = 0;
        if (!readUInt32(input, pathSize) ||
            pathSize > kMaxEntryPathBytes ||
            !hasRemainingBytes(input, packageSize, pathSize)) {
            error = "invalid transfer package path";
            return false;
        }

        std::string relativeUtf8(pathSize, '\0');
        if (pathSize > 0 && !readExact(input, &relativeUtf8[0], pathSize)) {
            error = "invalid transfer package path";
            return false;
        }

        barrier::fs::path relativePath;
        if (!validateEntryPath(relativeUtf8, type, extractionState, relativePath, error)) {
            return false;
        }

        const barrier::fs::path targetPath = (destinationRoot / relativePath).lexically_normal();

        if (type == kEntryDirectory) {
            if (!ensureExtractionDirectory(destinationRoot, relativePath, error)) {
                return false;
            }
            continue;
        }

        if (type != kEntryFile) {
            error = "unknown transfer package entry type";
            return false;
        }

        std::uint64_t fileSize = 0;
        if (!readUInt64(input, fileSize) ||
            !hasRemainingBytes(input, packageSize, fileSize)) {
            error = "invalid transfer package file payload";
            return false;
        }
        if (!validateFileSize(fileSize, extractionState, error)) {
            return false;
        }

        if (!prepareExtractionFile(destinationRoot, relativePath, error)) {
            return false;
        }
        std::ofstream output;
        barrier::open_utf8_path(output, targetPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            error = "failed to create extracted file";
            return false;
        }

        if (!copyStreamPayload(input, output, fileSize, error)) {
            return false;
        }

        output.flush();
        output.close();
        if (output.fail()) {
            error = "failed to write extracted file";
            return false;
        }
    }
}

bool
TransferArchive::extractDirectoryPackage(const std::string& packageData,
                                         const barrier::fs::path& destinationRoot,
                                         std::string& error)
{
    return extractPackage(packageData, destinationRoot, error);
}
