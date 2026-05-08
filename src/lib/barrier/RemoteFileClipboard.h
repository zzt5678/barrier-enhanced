#pragma once

#include "barrier/Clipboard.h"
#include "io/filesystem.h"

#include <string>
#include <vector>

class IClipboard;

namespace RemoteFileClipboard {

enum class Mode {
    SourcePaths = 0,
    MaterializedPaths = 1
};

struct Data {
    Mode mode = Mode::SourcePaths;
    bool cut = false;
    std::string sessionId;
    std::vector<barrier::fs::path> paths;
};

std::string createSessionId();
std::string serialize(const Data& data);
bool parse(const std::string& payload, Data& data, std::string* error = nullptr);
bool readFromClipboard(const IClipboard& clipboard, Data& data, std::string* error = nullptr);
bool normalizeClipboard(Clipboard& clipboard, Data* normalized = nullptr, std::string* error = nullptr);
bool allPathsLookLikeImages(const Data& data);
bool stripImageFileTransferMetadata(Clipboard& clipboard);
bool buildMaterializedClipboard(const std::vector<barrier::fs::path>& paths,
                                const std::string& sessionId,
                                Clipboard& clipboard);
bool createPackage(const Data& data, barrier::fs::path& packagePath, std::string& error);
bool extractPackage(const std::string& packageData,
                    const barrier::fs::path& destinationRoot,
                    std::vector<barrier::fs::path>& roots,
                    std::string& error);

} // namespace RemoteFileClipboard
