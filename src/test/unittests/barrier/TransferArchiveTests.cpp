/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 */

#include "barrier/DragInformation.h"
#include "barrier/TransferArchive.h"
#include "io/filesystem.h"

#include "test/global/gtest.h"

#include <array>
#include <ctime>
#include <cstdint>
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

void writeUInt32(std::ofstream& stream, std::uint32_t value)
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

void writeArchiveMagic(std::ofstream& stream)
{
    const std::array<char, 8> magic{{'B', 'D', 'I', 'R', 'P', 'K', 'G', '1'}};
    stream.write(magic.data(), static_cast<std::streamsize>(magic.size()));
}

void appendUInt32(std::string& data, std::uint32_t value)
{
    data.push_back(static_cast<char>((value >> 24) & 0xffu));
    data.push_back(static_cast<char>((value >> 16) & 0xffu));
    data.push_back(static_cast<char>((value >> 8) & 0xffu));
    data.push_back(static_cast<char>(value & 0xffu));
}

void appendUInt64(std::string& data, std::uint64_t value)
{
    data.push_back(static_cast<char>((value >> 56) & 0xffu));
    data.push_back(static_cast<char>((value >> 48) & 0xffu));
    data.push_back(static_cast<char>((value >> 40) & 0xffu));
    data.push_back(static_cast<char>((value >> 32) & 0xffu));
    data.push_back(static_cast<char>((value >> 24) & 0xffu));
    data.push_back(static_cast<char>((value >> 16) & 0xffu));
    data.push_back(static_cast<char>((value >> 8) & 0xffu));
    data.push_back(static_cast<char>(value & 0xffu));
}

std::string makeSingleFilePackage(const std::string& relativePath,
                                  const std::string& payload)
{
    std::string data("BDIRPKG1", 8);
    data.push_back('F');
    appendUInt32(data, static_cast<std::uint32_t>(relativePath.size()));
    data.append(relativePath);
    appendUInt64(data, static_cast<std::uint64_t>(payload.size()));
    data.append(payload);
    data.push_back('E');
    appendUInt32(data, 0);
    return data;
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
    EXPECT_TRUE(TransferArchive::isPackageFile(packagePath));

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

TEST(TransferArchiveTests, extractPackageFile_roundTripsSelectionWithoutLoadingPackageString)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-file-extract-" + uniqueToken());
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
    ASSERT_TRUE(TransferArchive::isPackageFile(packagePath));

    const barrier::fs::path destinationRoot = root / "destination";
    ASSERT_TRUE(TransferArchive::extractPackageFile(packagePath, destinationRoot, error)) << error;

    EXPECT_EQ("alpha", readFileUtf8(destinationRoot / "alpha.txt"));
    EXPECT_EQ("beta", readFileUtf8(destinationRoot / "bundle" / "beta.txt"));

    barrier::fs::remove_all(root);
    barrier::fs::remove(packagePath);
}

TEST(TransferArchiveTests, extractPackageFile_rejectsOversizedPathBeforeAllocation)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-bad-path-" + uniqueToken());
    barrier::fs::create_directories(root);
    const barrier::fs::path packagePath = root / "bad-path.bdir";

    {
        std::ofstream package;
        barrier::open_utf8_path(package, packagePath, std::ios::out | std::ios::binary | std::ios::trunc);
        writeArchiveMagic(package);
        package.put('F');
        writeUInt32(package, 0xffffffffu);
    }

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackageFile(packagePath, root / "destination", error));
    EXPECT_EQ("invalid transfer package path", error);

    barrier::fs::remove_all(root);
}

TEST(TransferArchiveTests, extractPackageFile_rejectsDeclaredPayloadPastPackageEnd)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-bad-payload-" + uniqueToken());
    barrier::fs::create_directories(root);
    const barrier::fs::path packagePath = root / "bad-payload.bdir";

    {
        std::ofstream package;
        barrier::open_utf8_path(package, packagePath, std::ios::out | std::ios::binary | std::ios::trunc);
        writeArchiveMagic(package);
        package.put('F');
        writeUInt32(package, 9);
        package.write("alpha.txt", 9);
        writeUInt64(package, 1024);
        package.write("short", 5);
    }

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackageFile(packagePath, root / "destination", error));
    EXPECT_EQ("invalid transfer package file payload", error);
    EXPECT_FALSE(barrier::fs::exists(root / "destination" / "alpha.txt"));

    barrier::fs::remove_all(root);
}

TEST(TransferArchiveTests, extractPackage_rejectsTraversalPathWithoutWritingOutsideDestination)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-memory-traversal-" + uniqueToken());
    const barrier::fs::path destination = root / "destination";
    const barrier::fs::path outside = root / "outside.txt";
    barrier::fs::create_directories(destination);
    {
        std::ofstream sentinel;
        barrier::open_utf8_path(sentinel, outside, std::ios::out | std::ios::binary | std::ios::trunc);
        sentinel << "sentinel";
    }

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackage(
        makeSingleFilePackage("../outside.txt", "overwritten"),
        destination,
        error));
    EXPECT_EQ("unsafe path in transfer package", error);
    EXPECT_EQ("sentinel", readFileUtf8(outside));
    EXPECT_FALSE(barrier::fs::exists(destination / "outside.txt"));

    barrier::fs::remove_all(root);
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

TEST(TransferArchiveTests, createAndExtractSelectionPackage_roundTripsMultipleDirectoriesAndEmptyDirs)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-multidir-" + uniqueToken());
    const barrier::fs::path docs = root / "docs folder";
    const barrier::fs::path photos = root / "photos";
    barrier::fs::create_directories(docs / "empty child");
    barrier::fs::create_directories(photos / "nested");

    {
        std::ofstream file;
        barrier::open_utf8_path(file, docs / "read me.txt", std::ios::out | std::ios::binary | std::ios::trunc);
        file << "docs";
    }
    {
        std::ofstream file;
        barrier::open_utf8_path(file, photos / "nested" / "image.txt", std::ios::out | std::ios::binary | std::ios::trunc);
        file << "photo";
    }

    barrier::fs::path packagePath;
    std::string error;
    ASSERT_TRUE(TransferArchive::createSelectionPackageFile({docs, photos}, packagePath, error)) << error;

    const std::string packageData = readFileUtf8(packagePath);
    const barrier::fs::path destinationRoot = root / "destination";
    ASSERT_TRUE(TransferArchive::extractPackage(packageData, destinationRoot, error)) << error;

    EXPECT_TRUE(barrier::fs::is_directory(destinationRoot / "docs folder"));
    EXPECT_TRUE(barrier::fs::is_directory(destinationRoot / "docs folder" / "empty child"));
    EXPECT_EQ("docs", readFileUtf8(destinationRoot / "docs folder" / "read me.txt"));
    EXPECT_EQ("photo", readFileUtf8(destinationRoot / "photos" / "nested" / "image.txt"));

    barrier::fs::remove_all(root);
    barrier::fs::remove(packagePath);
}

TEST(TransferArchiveTests, createAndExtractSelectionPackage_renamesDuplicateRootNames)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-duplicates-" + uniqueToken());
    const barrier::fs::path left = root / "left" / "bundle";
    const barrier::fs::path right = root / "right" / "bundle";
    barrier::fs::create_directories(left);
    barrier::fs::create_directories(right);

    {
        std::ofstream file;
        barrier::open_utf8_path(file, left / "left.txt", std::ios::out | std::ios::binary | std::ios::trunc);
        file << "left";
    }
    {
        std::ofstream file;
        barrier::open_utf8_path(file, right / "right.txt", std::ios::out | std::ios::binary | std::ios::trunc);
        file << "right";
    }

    barrier::fs::path packagePath;
    std::string error;
    ASSERT_TRUE(TransferArchive::createSelectionPackageFile({left, right}, packagePath, error)) << error;

    const std::string packageData = readFileUtf8(packagePath);
    const barrier::fs::path destinationRoot = root / "destination";
    ASSERT_TRUE(TransferArchive::extractPackage(packageData, destinationRoot, error)) << error;

    EXPECT_EQ("left", readFileUtf8(destinationRoot / "bundle" / "left.txt"));
    EXPECT_EQ("right", readFileUtf8(destinationRoot / "bundle (1)" / "right.txt"));

    barrier::fs::remove_all(root);
    barrier::fs::remove(packagePath);
}
