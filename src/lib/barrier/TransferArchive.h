#pragma once

#include "io/filesystem.h"

#include <string>
#include <vector>

class TransferArchive {
public:
    static bool isSafePortablePathComponent(const std::string& component);
    static bool isPackageData(const std::string& data);
    static bool isPackageFile(const barrier::fs::path& packagePath);
    static bool createSelectionPackageFile(const std::vector<barrier::fs::path>& sourcePaths,
                                           barrier::fs::path& packagePath,
                                           std::string& error);
    static bool createDirectoryPackageFile(const barrier::fs::path& sourceDir,
                                           barrier::fs::path& packagePath,
                                           std::string& error);
    static bool extractPackage(const std::string& packageData,
                               const barrier::fs::path& destinationRoot,
                               std::string& error);
    static bool extractPackageFile(const barrier::fs::path& packagePath,
                                   const barrier::fs::path& destinationRoot,
                                   std::string& error);
    static bool extractDirectoryPackage(const std::string& packageData,
                                        const barrier::fs::path& destinationRoot,
                                        std::string& error);
};
