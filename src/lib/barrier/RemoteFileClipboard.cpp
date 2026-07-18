#include "barrier/RemoteFileClipboard.h"

#include "barrier/IClipboard.h"
#include "barrier/SecureRandom.h"
#include "barrier/TransferArchive.h"
#include "barrier/TransferDigest.h"
#include "base/Unicode.h"
#include "mt/Thread.h"
#include "mt/XThread.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {

constexpr std::array<char, 8> kRemoteClipboardMagic{{'W', 'F', 'C', 'L', 'I', 'P', '1', 0}};

enum class SessionPolicy {
    RequireAssigned,
    AllowUnassignedLocalSource
};

#if defined(_WIN32)

std::wstring normalizeWindowsPathText(std::wstring path)
{
    static const std::wstring kExtendedUncPrefix = L"\\\\?\\UNC\\";
    static const std::wstring kExtendedPrefix = L"\\\\?\\";
    if (path.compare(0, kExtendedUncPrefix.size(), kExtendedUncPrefix) == 0) {
        path = L"\\\\" + path.substr(kExtendedUncPrefix.size());
    }
    else if (path.compare(0, kExtendedPrefix.size(), kExtendedPrefix) == 0) {
        path.erase(0, kExtendedPrefix.size());
    }

    for (wchar_t& character : path) {
        if (character == L'/') {
            character = L'\\';
        }
    }

    while (path.size() > 3 && path.back() == L'\\') {
        path.pop_back();
    }
    return path;
}

int compareWindowsPaths(const std::wstring& lhs, const std::wstring& rhs)
{
    const std::size_t maxLength =
        static_cast<std::size_t>((std::numeric_limits<int>::max)());
    if (lhs.size() <= maxLength && rhs.size() <= maxLength) {
        const int result = CompareStringOrdinal(
            lhs.c_str(), static_cast<int>(lhs.size()),
            rhs.c_str(), static_cast<int>(rhs.size()), TRUE);
        if (result != 0) {
            return result;
        }
    }

    // Invalid or oversized input must not be considered equal through a
    // failed Win32 comparison. Keep a deterministic, fail-closed ordering.
    if (lhs == rhs) {
        return CSTR_EQUAL;
    }
    return lhs < rhs ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
}

bool windowsPathsEqual(const std::wstring& lhs, const std::wstring& rhs)
{
    return compareWindowsPaths(lhs, rhs) == CSTR_EQUAL;
}

bool makeWindowsPathKey(const barrier::fs::path& source, std::wstring& key)
{
    barrier::fs::path normalized = source.lexically_normal();
    normalized.make_preferred();
    while (normalized != normalized.root_path() && !normalized.has_filename()) {
        const barrier::fs::path parent = normalized.parent_path();
        if (parent == normalized) {
            break;
        }
        normalized = parent;
    }

    key = normalized.native();
    if (key.empty() || key.find(L'\0') != std::wstring::npos) {
        return false;
    }
    key = normalizeWindowsPathText(std::move(key));
    return !key.empty();
}

bool ordinalPathMultisetsEqual(std::vector<std::wstring> lhs,
                               std::vector<std::wstring> rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }

    const auto less = [](const std::wstring& left, const std::wstring& right) {
        return compareWindowsPaths(left, right) == CSTR_LESS_THAN;
    };
    std::sort(lhs.begin(), lhs.end(), less);
    std::sort(rhs.begin(), rhs.end(), less);
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (!windowsPathsEqual(lhs[i], rhs[i])) {
            return false;
        }
    }
    return true;
}

#endif

void writeUInt32(std::string& output, UInt32 value)
{
    output.push_back(static_cast<char>((value >> 24) & 0xffu));
    output.push_back(static_cast<char>((value >> 16) & 0xffu));
    output.push_back(static_cast<char>((value >> 8) & 0xffu));
    output.push_back(static_cast<char>(value & 0xffu));
}

UInt32 readUInt32(const std::string& input, size_t& offset, bool& ok)
{
    if (!ok || offset + 4 > input.size()) {
        ok = false;
        return 0;
    }

    const unsigned char* bytes =
        reinterpret_cast<const unsigned char*>(input.data() + offset);
    offset += 4;
    return (static_cast<UInt32>(bytes[0]) << 24) |
           (static_cast<UInt32>(bytes[1]) << 16) |
           (static_cast<UInt32>(bytes[2]) << 8) |
           static_cast<UInt32>(bytes[3]);
}

bool setPathValidationError(std::string* error, const char* message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool validatePathUtf8ForAppendImpl(std::size_t currentPathCount,
                                   std::size_t currentTotalBytes,
                                   const std::string& pathUtf8,
                                   std::string* error)
{
    if (currentPathCount >= RemoteFileClipboard::kMaxClipboardPathCount) {
        return setPathValidationError(error, "remote file clipboard path count is too large");
    }
    if (pathUtf8.empty()) {
        return setPathValidationError(error, "remote file clipboard path entry is invalid");
    }
    if (pathUtf8.find('\0') != std::string::npos || !Unicode::isUTF8(pathUtf8)) {
        return setPathValidationError(error, "remote file clipboard path entry is invalid");
    }
    if (pathUtf8.size() > RemoteFileClipboard::kMaxClipboardPathBytes) {
        return setPathValidationError(error, "remote file clipboard path entry is too large");
    }
    if (pathUtf8.size() > RemoteFileClipboard::kMaxClipboardTotalPathBytes ||
        currentTotalBytes > RemoteFileClipboard::kMaxClipboardTotalPathBytes - pathUtf8.size()) {
        return setPathValidationError(error, "remote file clipboard path list is too large");
    }

    return true;
}

bool validatePathListImpl(const std::vector<barrier::fs::path>& paths,
                          std::string* error)
{
    if (paths.empty()) {
        return setPathValidationError(error, "remote file clipboard path list is empty");
    }
    if (paths.size() > RemoteFileClipboard::kMaxClipboardPathCount) {
        return setPathValidationError(error, "remote file clipboard path count is too large");
    }

    std::size_t totalBytes = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        const std::string pathUtf8 = paths[i].u8string();
        if (!validatePathUtf8ForAppendImpl(i, totalBytes, pathUtf8, error)) {
            return false;
        }
        totalBytes += pathUtf8.size();
    }

    return true;
}

bool hasImageExtension(const barrier::fs::path& path)
{
    std::string extension = path.extension().u8string();
    if (extension.empty()) {
        return false;
    }

    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    return extension == ".png" ||
           extension == ".jpg" ||
           extension == ".jpeg" ||
           extension == ".bmp" ||
           extension == ".gif" ||
           extension == ".tif" ||
           extension == ".tiff" ||
           extension == ".webp";
}

std::string trimLine(std::string value)
{
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                              value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }

    size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    return value.substr(start);
}

bool textLooksLikeImagePathList(const std::string& text)
{
    std::istringstream lines(text);
    std::string line;
    bool foundPath = false;
    while (std::getline(lines, line)) {
        line = trimLine(line);
        if (line.empty()) {
            continue;
        }

        const std::string filePrefix = "file://";
        if (line.compare(0, filePrefix.size(), filePrefix) == 0) {
            std::string path = line.substr(filePrefix.size());
            const std::string localhostPrefix = "localhost/";
            if (path.compare(0, localhostPrefix.size(), localhostPrefix) == 0) {
                path = path.substr(std::string("localhost").size());
            }
            line = path;
        }

        if (!hasImageExtension(barrier::fs::u8path(line))) {
            return false;
        }
        foundPath = true;
    }

    return foundPath;
}

bool isValidSessionId(const std::string& sessionId, bool allowEmpty)
{
    if (sessionId.empty()) {
        // A local source snapshot may be unassigned until normalizeClipboard().
        return allowEmpty;
    }
    if (sessionId.size() != RemoteFileClipboard::kSessionIdHexBytes) {
        return false;
    }

    for (const unsigned char value : sessionId) {
        const bool lowercaseHex =
            (value >= 'a' && value <= 'f') ||
            (value >= '0' && value <= '9');
        if (!lowercaseHex) {
            return false;
        }
    }
    return true;
}

std::string safeSessionDirectoryName(const std::string& sessionId)
{
    barrier::TransferDigest digest;
    std::string encoded;
    if (!digest.update(sessionId.data(), sessionId.size()) ||
        !digest.finish(encoded) ||
        encoded.compare(0, 7, "sha256:") != 0) {
        throw std::runtime_error("could not hash remote file clipboard session id");
    }
    return encoded.substr(7);
}

std::vector<std::pair<IClipboard::EFormat, std::string>> snapshotClipboard(const Clipboard& clipboard)
{
    std::vector<std::pair<IClipboard::EFormat, std::string>> formats;
    if (!clipboard.open(0)) {
        return formats;
    }

    for (UInt32 format = 0; format < IClipboard::kNumFormats; ++format) {
        const IClipboard::EFormat clipboardFormat =
            static_cast<IClipboard::EFormat>(format);
        if (clipboard.has(clipboardFormat)) {
            formats.push_back(std::make_pair(clipboardFormat, clipboard.get(clipboardFormat)));
        }
    }
    clipboard.close();

    return formats;
}

bool rewriteClipboard(Clipboard& clipboard,
                      const std::vector<std::pair<IClipboard::EFormat, std::string>>& formats)
{
    if (!clipboard.open(0)) {
        return false;
    }

    clipboard.empty();
    for (size_t i = 0; i < formats.size(); ++i) {
        clipboard.add(formats[i].first, formats[i].second);
    }
    clipboard.close();
    return true;
}

bool hasFileListFormat(const IClipboard& clipboard)
{
    if (!clipboard.open(0)) {
        return false;
    }

    const bool hasFileList = clipboard.has(IClipboard::kFileList);
    clipboard.close();
    return hasFileList;
}

struct CacheSession {
    barrier::fs::path root;
    bool keep = false;
    std::uintmax_t bytes = 0;
    barrier::fs::file_time_type modified;
};

std::uintmax_t directorySize(const barrier::fs::path& root)
{
    std::uintmax_t bytes = 0;
    std::error_code error;
    barrier::fs::recursive_directory_iterator it(
        root,
        barrier::fs::directory_options::skip_permission_denied,
        error);
    const barrier::fs::recursive_directory_iterator end;
    while (!error && it != end) {
        const barrier::fs::path path = it->path();
        if (barrier::fs::is_regular_file(path, error) && !error) {
            bytes += barrier::fs::file_size(path, error);
            if (error) {
                error.clear();
            }
        }
        it.increment(error);
    }
    return bytes;
}

barrier::fs::file_time_type pathModifiedTime(const barrier::fs::path& path)
{
    std::error_code error;
    const barrier::fs::file_time_type modified = barrier::fs::last_write_time(path, error);
    if (!error) {
        return modified;
    }
    return barrier::fs::file_time_type::min();
}

} // namespace

namespace RemoteFileClipboard {

bool containsFileList(const IClipboard& clipboard)
{
    return hasFileListFormat(clipboard);
}

bool collectMaterializedRoots(const barrier::fs::path& destinationRoot,
                              std::vector<barrier::fs::path>& roots,
                              std::string& error);

template <typename PackageExtractor>
bool extractPackageAtomically(const barrier::fs::path& destinationRoot,
                              std::vector<barrier::fs::path>& roots,
                              std::string& error,
                              PackageExtractor extract,
                              void (*beforeCommit)(const barrier::fs::path&) = nullptr)
{
    roots.clear();
    barrier::fs::path stagingRoot;

    const auto cleanupStaging = [&stagingRoot]() {
        if (!stagingRoot.empty()) {
            std::error_code ignored;
            barrier::fs::remove_all(stagingRoot, ignored);
        }
    };

    const auto ensureDestinationAbsent = [&destinationRoot, &error]() {
        std::error_code statusError;
        const barrier::fs::file_status status =
            barrier::fs::symlink_status(destinationRoot, statusError);
        if (statusError && statusError.default_error_condition() ==
                std::errc::no_such_file_or_directory) {
            return true;
        }
        if (statusError) {
            error = "remote file clipboard destination could not be inspected: " +
                statusError.message();
            return false;
        }
        if (status.type() != barrier::fs::file_type::not_found) {
            error = "remote file clipboard destination already exists";
            return false;
        }
        return true;
    };

    try {
        if (destinationRoot.empty() || destinationRoot.filename().empty()) {
            error = "remote file clipboard destination is invalid";
            return false;
        }
        const barrier::fs::path parent = destinationRoot.parent_path();
        if (parent.empty()) {
            error = "remote file clipboard destination parent is invalid";
            return false;
        }
        barrier::fs::create_directories(parent);
        if (!ensureDestinationAbsent()) {
            return false;
        }

        for (int attempt = 0; attempt < 8; ++attempt) {
            const std::string token = createSessionId();
            if (token.empty()) {
                error = "remote file clipboard staging token could not be created";
                return false;
            }

            const barrier::fs::path candidate = parent / barrier::fs::u8path(
                destinationRoot.filename().u8string() + ".staging-" + token);
            std::error_code createError;
            if (barrier::fs::create_directory(candidate, createError)) {
                stagingRoot = candidate;
                break;
            }
            if (createError &&
                createError != std::make_error_code(std::errc::file_exists)) {
                error = "remote file clipboard staging directory could not be created: " +
                    createError.message();
                return false;
            }
        }

        if (stagingRoot.empty()) {
            error = "remote file clipboard staging directory could not be reserved";
            return false;
        }
        if (!extract(stagingRoot, error)) {
            cleanupStaging();
            return false;
        }

        std::vector<barrier::fs::path> stagingRoots;
        if (!collectMaterializedRoots(stagingRoot, stagingRoots, error)) {
            cleanupStaging();
            return false;
        }
        std::vector<barrier::fs::path> committedRoots;
        committedRoots.reserve(stagingRoots.size());
        for (const barrier::fs::path& stagingPath : stagingRoots) {
            committedRoots.push_back(destinationRoot / stagingPath.filename());
        }
        if (!ensureDestinationAbsent()) {
            cleanupStaging();
            return false;
        }
        if (beforeCommit != nullptr) {
            beforeCommit(destinationRoot);
        }

        std::error_code commitError;
        const barrier::RenameNoReplaceResult commitResult =
            barrier::rename_no_replace(stagingRoot, destinationRoot, commitError);
        if (commitResult != barrier::RenameNoReplaceResult::kSuccess) {
            if (commitResult == barrier::RenameNoReplaceResult::kTargetExists) {
                error = "remote file clipboard destination already exists";
            }
            else {
                error = "remote file clipboard commit failed";
                if (commitError) {
                    error += ": " + commitError.message();
                }
            }
            cleanupStaging();
            return false;
        }
        stagingRoot.clear();
        roots.swap(committedRoots);
        return true;
    }
    catch (XThread&) {
        cleanupStaging();
        throw;
    }
    catch (const std::exception& exception) {
        cleanupStaging();
        roots.clear();
        error = exception.what();
        return false;
    }
}

bool validatePathList(const std::vector<barrier::fs::path>& paths,
                      std::string* error)
{
    return validatePathListImpl(paths, error);
}

bool validatePathUtf8ForAppend(std::size_t currentPathCount,
                               std::size_t currentTotalBytes,
                               const std::string& pathUtf8,
                               std::string* error)
{
    return validatePathUtf8ForAppendImpl(currentPathCount, currentTotalBytes, pathUtf8, error);
}

std::string createSessionId()
{
    std::string sessionId;
    if (!barrier::SecureRandom::generateHex(kSessionIdRandomBytes, sessionId)) {
        return std::string();
    }
    return sessionId;
}

std::string serialize(const Data& data)
{
    std::string output(kRemoteClipboardMagic.begin(), kRemoteClipboardMagic.end());
    output.push_back(static_cast<char>(data.mode == Mode::MaterializedPaths ? 1 : 0));
    output.push_back(static_cast<char>(data.cut ? 1 : 0));

    writeUInt32(output, static_cast<UInt32>(data.sessionId.size()));
    output.append(data.sessionId);

    writeUInt32(output, static_cast<UInt32>(data.paths.size()));
    for (size_t i = 0; i < data.paths.size(); ++i) {
        const std::string path = data.paths[i].u8string();
        writeUInt32(output, static_cast<UInt32>(path.size()));
        output.append(path);
    }

    return output;
}

static bool parseWithSessionPolicy(const std::string& payload, Data& data,
                                   SessionPolicy sessionPolicy,
                                   std::string* error)
{
    auto fail = [error](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (payload.size() < kRemoteClipboardMagic.size() + 2) {
        return fail("remote file clipboard payload is too small");
    }

    if (!std::equal(kRemoteClipboardMagic.begin(), kRemoteClipboardMagic.end(), payload.begin())) {
        return fail("remote file clipboard payload header is invalid");
    }

    size_t offset = kRemoteClipboardMagic.size();
    const unsigned char mode = static_cast<unsigned char>(payload[offset++]);
    const unsigned char cut = static_cast<unsigned char>(payload[offset++]);
    if (mode > 1) {
        return fail("remote file clipboard mode is invalid");
    }
    if (cut > 1) {
        return fail("remote file clipboard cut flag is invalid");
    }

    bool ok = true;
    const UInt32 sessionSize = readUInt32(payload, offset, ok);
    const bool allowUnassignedLocalSource =
        sessionPolicy == SessionPolicy::AllowUnassignedLocalSource &&
        mode == 0;
    const bool sessionSizeIsValid = sessionSize == kSessionIdHexBytes ||
        (allowUnassignedLocalSource && sessionSize == 0);
    if (!ok || !sessionSizeIsValid || sessionSize > payload.size() - offset) {
        return fail("remote file clipboard session id is invalid");
    }

    Data parsed;
    parsed.mode = mode == 1 ? Mode::MaterializedPaths : Mode::SourcePaths;
    parsed.cut = cut == 1;
    parsed.sessionId.assign(payload.data() + offset,
                            payload.data() + offset + sessionSize);
    offset += sessionSize;
    if (!isValidSessionId(parsed.sessionId,
            allowUnassignedLocalSource &&
            parsed.mode == Mode::SourcePaths)) {
        return fail("remote file clipboard session id is invalid");
    }

    const UInt32 pathCount = readUInt32(payload, offset, ok);
    if (!ok) {
        return fail("remote file clipboard path count is invalid");
    }
    if (pathCount > kMaxClipboardPathCount ||
        pathCount > (payload.size() - offset) / sizeof(UInt32)) {
        return fail("remote file clipboard path count is too large");
    }

    parsed.paths.reserve(pathCount);
    std::size_t totalPathBytes = 0;
    for (UInt32 i = 0; i < pathCount; ++i) {
        const UInt32 size = readUInt32(payload, offset, ok);
        if (!ok || size > payload.size() - offset) {
            return fail("remote file clipboard path entry is invalid");
        }

        const std::string pathUtf8(payload.data() + offset, payload.data() + offset + size);
        std::string validationError;
        if (!validatePathUtf8ForAppend(parsed.paths.size(), totalPathBytes,
                                       pathUtf8, &validationError)) {
            return fail(validationError.c_str());
        }

        try {
            parsed.paths.push_back(barrier::fs::u8path(pathUtf8));
        }
        catch (const std::exception&) {
            return fail("remote file clipboard path entry is invalid");
        }
        totalPathBytes += pathUtf8.size();
        offset += size;
    }

    if (offset != payload.size()) {
        return fail("remote file clipboard payload has trailing data");
    }
    if (!validatePathList(parsed.paths, error)) {
        return false;
    }

    data = std::move(parsed);
    return true;
}

bool parse(const std::string& payload, Data& data, std::string* error)
{
    return parseWithSessionPolicy(
        payload, data, SessionPolicy::RequireAssigned, error);
}

bool parseLocalClipboard(const std::string& payload, Data& data,
                         std::string* error)
{
    return parseWithSessionPolicy(
        payload, data, SessionPolicy::AllowUnassignedLocalSource, error);
}

bool readFromClipboard(const IClipboard& clipboard, Data& data, std::string* error)
{
    if (!clipboard.open(0)) {
        if (error != nullptr) {
            *error = "clipboard could not be opened";
        }
        return false;
    }

    const bool hasFileList = clipboard.has(IClipboard::kFileList);
    const std::string payload = hasFileList ? clipboard.get(IClipboard::kFileList) : std::string();
    clipboard.close();

    if (!hasFileList) {
        if (error != nullptr) {
            *error = "clipboard has no file list";
        }
        return false;
    }

    return parse(payload, data, error);
}

bool normalizeClipboard(Clipboard& clipboard, Data* normalized, std::string* error)
{
    const std::vector<std::pair<IClipboard::EFormat, std::string>> formats =
        snapshotClipboard(clipboard);
    if (formats.empty()) {
        if (error != nullptr) {
            *error = "clipboard is empty";
        }
        return false;
    }

    Data data;
    bool found = false;
    for (size_t i = 0; i < formats.size(); ++i) {
        if (formats[i].first == IClipboard::kFileList) {
            if (!parseLocalClipboard(formats[i].second, data, error)) {
                return false;
            }
            found = true;
            break;
        }
    }

    if (!found) {
        if (error != nullptr) {
            *error = "clipboard has no file list";
        }
        return false;
    }

    if (data.mode == Mode::MaterializedPaths) {
        if (normalized != nullptr) {
            *normalized = data;
        }
        return true;
    }

    if (data.sessionId.empty()) {
        data.sessionId = createSessionId();
        if (data.sessionId.empty()) {
            if (error != nullptr) {
                *error = "remote file clipboard session id could not be created";
            }
            return false;
        }
    }

    std::vector<std::pair<IClipboard::EFormat, std::string>> rewritten = formats;
    for (size_t i = 0; i < rewritten.size(); ++i) {
        if (rewritten[i].first == IClipboard::kFileList) {
            rewritten[i].second = serialize(data);
            break;
        }
    }

    if (!rewriteClipboard(clipboard, rewritten)) {
        if (error != nullptr) {
            *error = "clipboard could not be rewritten";
        }
        return false;
    }

    if (normalized != nullptr) {
        *normalized = data;
    }
    return true;
}

bool pathsMatch(const Data& data, const std::vector<std::string>& paths)
{
    if (data.paths.size() != paths.size()) {
        return false;
    }

#if defined(_WIN32)
    try {
        std::vector<std::wstring> actual;
        std::vector<std::wstring> expected;
        actual.reserve(data.paths.size());
        expected.reserve(paths.size());
        for (std::size_t i = 0; i < data.paths.size(); ++i) {
            std::wstring actualPath;
            std::wstring expectedPath;
            if (!makeWindowsPathKey(data.paths[i], actualPath) ||
                !makeWindowsPathKey(barrier::fs::u8path(paths[i]), expectedPath)) {
                return false;
            }
            actual.push_back(std::move(actualPath));
            expected.push_back(std::move(expectedPath));
        }
        return ordinalPathMultisetsEqual(
            std::move(actual), std::move(expected));
    }
    catch (const std::exception&) {
        return false;
    }
#else
    const auto normalizedKey = [](const barrier::fs::path& path) {
        return path.lexically_normal().generic_u8string();
    };

    try {
        std::vector<std::string> actual;
        std::vector<std::string> expected;
        actual.reserve(data.paths.size());
        expected.reserve(paths.size());
        for (size_t i = 0; i < data.paths.size(); ++i) {
            actual.push_back(normalizedKey(data.paths[i]));
            expected.push_back(normalizedKey(barrier::fs::u8path(paths[i])));
        }
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        return actual == expected;
    }
    catch (const std::exception&) {
        return false;
    }
#endif
}

bool allPathsLookLikeImages(const Data& data)
{
    if (data.paths.empty()) {
        return false;
    }

    for (size_t i = 0; i < data.paths.size(); ++i) {
        if (!hasImageExtension(data.paths[i])) {
            return false;
        }
    }

    return true;
}

bool stripImageFileTransferMetadata(Clipboard& clipboard)
{
    const std::vector<std::pair<IClipboard::EFormat, std::string>> formats =
        snapshotClipboard(clipboard);
    if (formats.empty()) {
        return false;
    }

    bool hasImagePayload = false;
    bool hasFileList = false;
    Data fileList;
    for (size_t i = 0; i < formats.size(); ++i) {
        if (formats[i].first == IClipboard::kPNG ||
            formats[i].first == IClipboard::kBitmap) {
            hasImagePayload = true;
        }
        else if (formats[i].first == IClipboard::kFileList) {
            if (!parseLocalClipboard(formats[i].second, fileList)) {
                return false;
            }
            hasFileList = true;
        }
    }

    if (!hasImagePayload || !hasFileList || !allPathsLookLikeImages(fileList)) {
        return false;
    }

    std::vector<std::pair<IClipboard::EFormat, std::string>> rewritten;
    rewritten.reserve(formats.size());
    for (size_t i = 0; i < formats.size(); ++i) {
        if (formats[i].first == IClipboard::kFileList) {
            continue;
        }

        if (formats[i].first == IClipboard::kText &&
            textLooksLikeImagePathList(formats[i].second)) {
            continue;
        }

        rewritten.push_back(formats[i]);
    }

    if (rewritten.size() == formats.size()) {
        return false;
    }

    return rewriteClipboard(clipboard, rewritten);
}

AutomaticSharingStatus prepareForAutomaticClipboardSharing(Clipboard& clipboard)
{
    if (!containsFileList(clipboard)) {
        return AutomaticSharingStatus::Safe;
    }

    Data fileList;
    if (readFromClipboard(clipboard, fileList) &&
        fileList.mode == Mode::MaterializedPaths) {
        return AutomaticSharingStatus::Safe;
    }

    if (stripImageFileTransferMetadata(clipboard)) {
        return AutomaticSharingStatus::SafeAfterRemovingImageFileMetadata;
    }

    return AutomaticSharingStatus::ContainsFileList;
}

bool buildMaterializedClipboard(const std::vector<barrier::fs::path>& paths,
                                const std::string& sessionId,
                                Clipboard& clipboard)
{
    Data data;
    data.mode = Mode::MaterializedPaths;
    data.sessionId = sessionId;
    data.paths = paths;
    if (!isValidSessionId(data.sessionId, false) ||
        !validatePathList(data.paths)) {
        return false;
    }

    if (!clipboard.open(0)) {
        return false;
    }
    clipboard.empty();
    clipboard.add(IClipboard::kFileList, serialize(data));
    clipboard.close();
    return true;
}

barrier::fs::path materializedSessionRoot(const barrier::fs::path& cacheRoot,
                                          const std::string& sessionId)
{
    return cacheRoot / barrier::fs::u8path(safeSessionDirectoryName(sessionId));
}

barrier::fs::path materializedSessionRoot(const barrier::fs::path& cacheRoot,
                                          const std::string& origin,
                                          std::uint64_t revision,
                                          const std::string& sessionId)
{
    const std::string scopedKey = std::to_string(origin.size()) + ":" + origin +
        ":" + std::to_string(revision) + ":" + sessionId;
    return cacheRoot / barrier::fs::u8path(safeSessionDirectoryName(scopedKey));
}

bool createPackage(const Data& data, barrier::fs::path& packagePath, std::string& error)
{
    if (!validatePathList(data.paths, &error)) {
        return false;
    }

    std::vector<barrier::fs::path> sourcePaths = data.paths;
    return TransferArchive::createSelectionPackageFile(sourcePaths, packagePath, error);
}

bool extractPackage(const std::string& packageData,
                    const barrier::fs::path& destinationRoot,
                    std::vector<barrier::fs::path>& roots,
                    std::string& error)
{
    return extractPackageAtomically(
        destinationRoot, roots, error,
        [&packageData](const barrier::fs::path& stagingRoot,
                       std::string& extractError) {
            return TransferArchive::extractPackage(
                packageData, stagingRoot, extractError);
        });
}

bool extractPackageFile(const barrier::fs::path& packagePath,
                        const barrier::fs::path& destinationRoot,
                        std::vector<barrier::fs::path>& roots,
                        std::string& error)
{
    return extractPackageAtomically(
        destinationRoot, roots, error,
        [&packagePath](const barrier::fs::path& stagingRoot,
                       std::string& extractError) {
            return TransferArchive::extractPackageFile(
                packagePath, stagingRoot, extractError);
        });
}

bool extractPackageFileWithCommitHookForTest(
    const barrier::fs::path& packagePath,
    const barrier::fs::path& destinationRoot,
    std::vector<barrier::fs::path>& roots,
    std::string& error,
    void (*beforeCommit)(const barrier::fs::path&))
{
    return extractPackageAtomically(
        destinationRoot, roots, error,
        [&packagePath](const barrier::fs::path& stagingRoot,
                       std::string& extractError) {
            return TransferArchive::extractPackageFile(
                packagePath, stagingRoot, extractError);
        },
        beforeCommit);
}

void pruneMaterializedCache(const barrier::fs::path& cacheRoot,
                            const std::string& keepSessionId,
                            std::size_t maxSessions,
                            std::uintmax_t maxBytes)
{
    const barrier::fs::path keepRoot = keepSessionId.empty()
        ? barrier::fs::path()
        : materializedSessionRoot(cacheRoot, keepSessionId);
    pruneMaterializedCacheKeepingRoot(cacheRoot, keepRoot, maxSessions, maxBytes);
}

void pruneMaterializedCacheKeepingRoot(const barrier::fs::path& cacheRoot,
                                       const barrier::fs::path& keepRoot,
                                       std::size_t maxSessions,
                                       std::uintmax_t maxBytes)
{
    if (cacheRoot.empty() || !barrier::fs::exists(cacheRoot)) {
        return;
    }

    std::vector<CacheSession> sessions;
    std::error_code error;
    for (barrier::fs::directory_iterator it(cacheRoot, error), end; !error && it != end;
         it.increment(error)) {
        if (!it->is_directory(error) || error) {
            error.clear();
            continue;
        }

        CacheSession session;
        session.root = it->path();
        session.keep = !keepRoot.empty() && session.root == keepRoot;
        session.bytes = directorySize(session.root);
        session.modified = pathModifiedTime(session.root);
        sessions.push_back(session);
    }

    std::uintmax_t totalBytes = 0;
    for (const CacheSession& session : sessions) {
        totalBytes += session.bytes;
    }

    std::sort(sessions.begin(), sessions.end(), [](const CacheSession& lhs, const CacheSession& rhs) {
        if (lhs.keep != rhs.keep) {
            return !lhs.keep && rhs.keep;
        }
        if (lhs.modified != rhs.modified) {
            return lhs.modified < rhs.modified;
        }
        return lhs.root.u8string() < rhs.root.u8string();
    });

    std::size_t sessionCount = sessions.size();
    for (const CacheSession& session : sessions) {
        if (session.keep) {
            continue;
        }
        const bool tooManySessions = maxSessions != 0 && sessionCount > maxSessions;
        const bool tooManyBytes = maxBytes != 0 && totalBytes > maxBytes;
        if (!tooManySessions && !tooManyBytes) {
            break;
        }

        std::error_code removeError;
        const std::uintmax_t removed = barrier::fs::remove_all(session.root, removeError);
        if (!removeError) {
            --sessionCount;
            totalBytes = session.bytes > totalBytes ? 0 : totalBytes - session.bytes;
        }
        (void)removed;
    }
}

bool collectMaterializedRoots(const barrier::fs::path& destinationRoot,
                              std::vector<barrier::fs::path>& roots,
                              std::string& error)
{
    roots.clear();

    for (barrier::fs::directory_iterator it(destinationRoot), end; it != end; ++it) {
        Thread::testCancel();
        roots.push_back(it->path());
    }

    std::sort(roots.begin(), roots.end(),
              [](const barrier::fs::path& lhs, const barrier::fs::path& rhs) {
                  return lhs.u8string() < rhs.u8string();
              });

    if (roots.empty()) {
        error = "remote file clipboard package extracted no files";
        barrier::fs::remove_all(destinationRoot);
        return false;
    }

    return true;
}

} // namespace RemoteFileClipboard
