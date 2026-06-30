#pragma once

#include "barrier/Clipboard.h"
#include "io/filesystem.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

class IClipboard;

namespace RemoteFileClipboard {

constexpr std::size_t kMaxClipboardPathCount = 1024;
constexpr std::size_t kMaxClipboardPathBytes = 64 * 1024;
constexpr std::size_t kMaxClipboardTotalPathBytes = 1024 * 1024;
constexpr std::size_t kMaxNativeFileSelectionBytes = 4 * 1024 * 1024;

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
bool validatePathList(const std::vector<barrier::fs::path>& paths,
                      std::string* error = nullptr);
bool validatePathUtf8ForAppend(std::size_t currentPathCount,
                               std::size_t currentTotalBytes,
                               const std::string& pathUtf8,
                               std::string* error = nullptr);
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
