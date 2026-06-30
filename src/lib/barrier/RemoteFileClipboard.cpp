#include "barrier/RemoteFileClipboard.h"

#include "barrier/IClipboard.h"
#include "barrier/TransferArchive.h"
#include "mt/Thread.h"
#include "mt/XThread.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_set>

namespace {

constexpr std::array<char, 8> kRemoteClipboardMagic{{'W', 'F', 'C', 'L', 'I', 'P', '1', 0}};

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

std::string safeSessionDirectoryName(const std::string& sessionId)
{
    std::ostringstream stream;
    for (size_t i = 0; i < sessionId.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(sessionId[i]);
        if (std::isalnum(c) != 0 || c == '-' || c == '_') {
            stream << static_cast<char>(c);
        }
        else {
            stream << '_'
                   << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<unsigned int>(c)
                   << std::dec;
        }
    }

    std::string directoryName = stream.str();
    if (directoryName.empty()) {
        directoryName = "session";
    }
    if (directoryName.size() > 128) {
        directoryName.resize(128);
    }
    return directoryName;
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

bool hasFileListFormat(const Clipboard& clipboard)
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

bool collectMaterializedRoots(const barrier::fs::path& destinationRoot,
                              std::vector<barrier::fs::path>& roots,
                              std::string& error);

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
    return uniqueToken();
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

bool parse(const std::string& payload, Data& data, std::string* error)
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

    bool ok = true;
    const UInt32 sessionSize = readUInt32(payload, offset, ok);
    if (!ok || offset + sessionSize > payload.size()) {
        return fail("remote file clipboard session id is invalid");
    }

    data = Data();
    data.mode = mode == 1 ? Mode::MaterializedPaths : Mode::SourcePaths;
    data.cut = cut != 0;
    data.sessionId.assign(payload.data() + offset, payload.data() + offset + sessionSize);
    offset += sessionSize;

    const UInt32 pathCount = readUInt32(payload, offset, ok);
    if (!ok) {
        return fail("remote file clipboard path count is invalid");
    }
    if (pathCount > kMaxClipboardPathCount ||
        pathCount > (payload.size() - offset) / sizeof(UInt32)) {
        return fail("remote file clipboard path count is too large");
    }

    data.paths.reserve(pathCount);
    std::size_t totalPathBytes = 0;
    for (UInt32 i = 0; i < pathCount; ++i) {
        const UInt32 size = readUInt32(payload, offset, ok);
        if (!ok || offset + size > payload.size()) {
            return fail("remote file clipboard path entry is invalid");
        }

        const std::string pathUtf8(payload.data() + offset, payload.data() + offset + size);
        std::string validationError;
        if (!validatePathUtf8ForAppend(data.paths.size(), totalPathBytes, pathUtf8, &validationError)) {
            return fail(validationError.c_str());
        }

        data.paths.push_back(barrier::fs::u8path(pathUtf8));
        totalPathBytes += pathUtf8.size();
        offset += size;
    }

    if (!validatePathList(data.paths, error)) {
        return false;
    }

    return true;
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
            if (!parse(formats[i].second, data, error)) {
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
            if (!parse(formats[i].second, fileList)) {
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
    if (!hasFileListFormat(clipboard)) {
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
    if (!validatePathList(data.paths)) {
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
    roots.clear();

    try {
        barrier::fs::remove_all(destinationRoot);
        barrier::fs::create_directories(destinationRoot);

        if (!TransferArchive::extractPackage(packageData, destinationRoot, error)) {
            barrier::fs::remove_all(destinationRoot);
            return false;
        }

        return collectMaterializedRoots(destinationRoot, roots, error);
    }
    catch (XThread&) {
        barrier::fs::remove_all(destinationRoot);
        throw;
    }
}

bool extractPackageFile(const barrier::fs::path& packagePath,
                        const barrier::fs::path& destinationRoot,
                        std::vector<barrier::fs::path>& roots,
                        std::string& error)
{
    roots.clear();

    try {
        barrier::fs::remove_all(destinationRoot);
        barrier::fs::create_directories(destinationRoot);

        if (!TransferArchive::extractPackageFile(packagePath, destinationRoot, error)) {
            barrier::fs::remove_all(destinationRoot);
            return false;
        }

        return collectMaterializedRoots(destinationRoot, roots, error);
    }
    catch (XThread&) {
        barrier::fs::remove_all(destinationRoot);
        throw;
    }
}

void pruneMaterializedCache(const barrier::fs::path& cacheRoot,
                            const std::string& keepSessionId,
                            std::size_t maxSessions,
                            std::uintmax_t maxBytes)
{
    if (cacheRoot.empty() || !barrier::fs::exists(cacheRoot)) {
        return;
    }

    const barrier::fs::path keepRoot = keepSessionId.empty()
        ? barrier::fs::path()
        : materializedSessionRoot(cacheRoot, keepSessionId);

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
