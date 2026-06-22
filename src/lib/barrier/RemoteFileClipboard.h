#pragma once

#include "barrier/Clipboard.h"
#include "io/filesystem.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

class IClipboard;

namespace RemoteFileClipboard {

enum class Mode {
    SourcePaths = 0,
    MaterializedPaths = 1
};

enum class AutomaticSharingStatus {
    Safe,
    SafeAfterRemovingImageFileMetadata,
    ContainsFileList
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
AutomaticSharingStatus prepareForAutomaticClipboardSharing(Clipboard& clipboard);
bool buildMaterializedClipboard(const std::vector<barrier::fs::path>& paths,
                                const std::string& sessionId,
                                Clipboard& clipboard);
barrier::fs::path materializedSessionRoot(const barrier::fs::path& cacheRoot,
                                          const std::string& sessionId);
bool createPackage(const Data& data, barrier::fs::path& packagePath, std::string& error);
bool extractPackage(const std::string& packageData,
                    const barrier::fs::path& destinationRoot,
                    std::vector<barrier::fs::path>& roots,
                    std::string& error);
bool extractPackageFile(const barrier::fs::path& packagePath,
                        const barrier::fs::path& destinationRoot,
                        std::vector<barrier::fs::path>& roots,
                        std::string& error);
void pruneMaterializedCache(const barrier::fs::path& cacheRoot,
                            const std::string& keepSessionId,
                            std::size_t maxSessions,
                            std::uintmax_t maxBytes);

} // namespace RemoteFileClipboard
