/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 */

#include "barrier/DragInformation.h"
#include "barrier/TransferArchive.h"
#include "io/filesystem.h"

#include "test/global/gtest.h"

#include <ctime>
#include <cstdlib>
#include <fstream>
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

std::string readFileUtf8(const barrier::fs::path& path)
{
    std::ifstream input;
    barrier::open_utf8_path(input, path, std::ios::in | std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}

TEST(TransferArchiveTests, createAndExtractDirectoryPackage_roundTripsTree)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-transfer-" + uniqueToken());
    const barrier::fs::path sourceDir = root / "source-dir";
    const barrier::fs::path nestedDir = sourceDir / "nested";
    barrier::fs::create_directories(nestedDir);

    {
        std::ofstream file;
        barrier::open_utf8_path(file, sourceDir / "root.txt", std::ios::out | std::ios::binary | std::ios::trunc);
        file << "root";
    }
    {
        std::ofstream file;
        barrier::open_utf8_path(file, nestedDir / "child.txt", std::ios::out | std::ios::binary | std::ios::trunc);
        file << "child";
    }

    barrier::fs::path packagePath;
    std::string error;
    ASSERT_TRUE(TransferArchive::createDirectoryPackageFile(sourceDir, packagePath, error)) << error;

    const std::string packageData = readFileUtf8(packagePath);
    ASSERT_FALSE(packageData.empty());

    const barrier::fs::path destinationRoot = root / "destination";
    ASSERT_TRUE(TransferArchive::extractDirectoryPackage(packageData, destinationRoot, error)) << error;

    EXPECT_TRUE(barrier::fs::is_directory(destinationRoot / "source-dir"));
    EXPECT_TRUE(barrier::fs::is_directory(destinationRoot / "source-dir" / "nested"));
    EXPECT_EQ("root", readFileUtf8(destinationRoot / "source-dir" / "root.txt"));
    EXPECT_EQ("child", readFileUtf8(destinationRoot / "source-dir" / "nested" / "child.txt"));

    barrier::fs::remove_all(root);
    barrier::fs::remove(packagePath);
}

TEST(TransferArchiveTests, dragInfo_roundTripsDirectoryType)
{
    DragInformation entry;
    String filename("/tmp/example-folder");
    entry.setFilename(filename);
    entry.setEntryType(DragInformation::Directory);

    DragFileList dragFileList;
    dragFileList.push_back(entry);

    String output;
    ASSERT_EQ(1, DragInformation::setupDragInfo(dragFileList, output));

    DragFileList parsed;
    DragInformation::parseDragInfo(parsed, 1, output);

    ASSERT_EQ(1u, parsed.size());
    EXPECT_TRUE(parsed.front().isDirectory());
    EXPECT_EQ("example-folder", parsed.front().getFilename());
}

TEST(TransferArchiveTests, createAndExtractSelectionPackage_roundTripsMultipleRoots)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-selection-" + uniqueToken());
    barrier::fs::create_directories(root);

    const barrier::fs::path sourceFile = root / "alpha.txt";
    const barrier::fs::path sourceDir = root / "bundle";
    barrier::fs::create_directories(sourceDir);
    {
        std::ofstream file;
        barrier::open_utf8_path(file, sourceFile, std::ios::out | std::ios::binary | std::ios::trunc);
        file << "alpha";
    }
    {
        std::ofstream file;
        barrier::open_utf8_path(file, sourceDir / "beta.txt", std::ios::out | std::ios::binary | std::ios::trunc);
        file << "beta";
    }

    barrier::fs::path packagePath;
    std::string error;
    ASSERT_TRUE(TransferArchive::createSelectionPackageFile({sourceFile, sourceDir}, packagePath, error)) << error;

    const std::string packageData = readFileUtf8(packagePath);
    const barrier::fs::path destinationRoot = root / "destination";
    ASSERT_TRUE(TransferArchive::extractPackage(packageData, destinationRoot, error)) << error;

    EXPECT_EQ("alpha", readFileUtf8(destinationRoot / "alpha.txt"));
    EXPECT_EQ("beta", readFileUtf8(destinationRoot / "bundle" / "beta.txt"));

    barrier::fs::remove_all(root);
    barrier::fs::remove(packagePath);
}
