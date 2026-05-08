#include "barrier/RemoteFileClipboard.h"

#include "barrier/IClipboard.h"
#include "barrier/TransferArchive.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <cctype>
#include <sstream>
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

bool isSafePathList(const std::vector<barrier::fs::path>& paths)
{
    if (paths.empty()) {
        return false;
    }

    for (size_t i = 0; i < paths.size(); ++i) {
        if (paths[i].empty()) {
            return false;
        }
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

} // namespace

namespace RemoteFileClipboard {

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

    data.paths.reserve(pathCount);
    for (UInt32 i = 0; i < pathCount; ++i) {
        const UInt32 size = readUInt32(payload, offset, ok);
        if (!ok || offset + size > payload.size()) {
            return fail("remote file clipboard path entry is invalid");
        }

        data.paths.push_back(barrier::fs::u8path(payload.substr(offset, size)));
        offset += size;
    }

    if (!isSafePathList(data.paths)) {
        return fail("remote file clipboard path list is empty");
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

bool buildMaterializedClipboard(const std::vector<barrier::fs::path>& paths,
                                const std::string& sessionId,
                                Clipboard& clipboard)
{
    Data data;
    data.mode = Mode::MaterializedPaths;
    data.sessionId = sessionId;
    data.paths = paths;
    if (!isSafePathList(data.paths)) {
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

bool createPackage(const Data& data, barrier::fs::path& packagePath, std::string& error)
{
    if (!isSafePathList(data.paths)) {
        error = "remote file clipboard path list is empty";
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

    barrier::fs::remove_all(destinationRoot);
    barrier::fs::create_directories(destinationRoot);

    if (!TransferArchive::extractPackage(packageData, destinationRoot, error)) {
        barrier::fs::remove_all(destinationRoot);
        return false;
    }

    for (barrier::fs::directory_iterator it(destinationRoot), end; it != end; ++it) {
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
