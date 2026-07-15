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
#include <limits>
#include <sstream>
#include <thread>

#ifdef WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

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
    return data;
}

void appendDirectoryEntry(std::string& data, const std::string& relativePath)
{
    data.push_back('D');
    appendUInt32(data, static_cast<std::uint32_t>(relativePath.size()));
    data.append(relativePath);
}

void appendFileEntry(std::string& data,
                     const std::string& relativePath,
                     const std::string& payload)
{
    data.push_back('F');
    appendUInt32(data, static_cast<std::uint32_t>(relativePath.size()));
    data.append(relativePath);
    appendUInt64(data, static_cast<std::uint64_t>(payload.size()));
    data.append(payload);
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

TEST(TransferArchiveTests, extractPackage_rejectsDeclaredPayloadSizeThatWouldOverflowBoundsCheck)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-memory-overflow-payload-" + uniqueToken());
    const barrier::fs::path destination = root / "destination";
    barrier::fs::create_directories(destination);

    std::string packageData("BDIRPKG1", 8);
    packageData.push_back('F');
    appendUInt32(packageData, 9);
    packageData.append("alpha.txt", 9);
    appendUInt64(packageData, std::numeric_limits<std::uint64_t>::max());
    packageData.append("short", 5);

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackage(packageData, destination, error));
    EXPECT_EQ("invalid transfer package file payload", error);
    EXPECT_FALSE(barrier::fs::exists(destination / "alpha.txt"));

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

TEST(TransferArchiveTests, extractPackage_rejectsExistingFileWithoutOverwriting)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-existing-target-" + uniqueToken());
    const barrier::fs::path destination = root / "destination";
    const barrier::fs::path existing = destination / "same.txt";
    barrier::fs::create_directories(destination);
    {
        std::ofstream sentinel;
        barrier::open_utf8_path(sentinel, existing, std::ios::out | std::ios::binary | std::ios::trunc);
        sentinel << "sentinel";
    }

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackage(
        makeSingleFilePackage("same.txt", "overwritten"),
        destination,
        error));
    EXPECT_EQ("extraction target already exists", error);
    EXPECT_EQ("sentinel", readFileUtf8(existing));

    barrier::fs::remove_all(root);
}

#ifndef WIN32
TEST(TransferArchiveTests, extractPackage_rejectsDestinationRootSymbolicLink)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-root-link-" + uniqueToken());
    const barrier::fs::path outside = root / "outside";
    const barrier::fs::path destination = root / "destination";
    barrier::fs::create_directories(outside);
    barrier::fs::create_directory_symlink(outside, destination);

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackage(
        makeSingleFilePackage("escaped.txt", "payload"),
        destination,
        error));
    EXPECT_EQ("unsafe extraction root", error);
    EXPECT_FALSE(barrier::fs::exists(outside / "escaped.txt"));

    barrier::fs::remove_all(root);
}

TEST(TransferArchiveTests, extractPackage_rejectsParentSymbolicLink)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-parent-link-" + uniqueToken());
    const barrier::fs::path outside = root / "outside";
    const barrier::fs::path destination = root / "destination";
    barrier::fs::create_directories(outside);
    barrier::fs::create_directories(destination);
    barrier::fs::create_directory_symlink(outside, destination / "linked");

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackage(
        makeSingleFilePackage("linked/escaped.txt", "payload"),
        destination,
        error));
    EXPECT_EQ("unsafe extraction path", error);
    EXPECT_FALSE(barrier::fs::exists(outside / "escaped.txt"));

    barrier::fs::remove_all(root);
}
#endif

TEST(TransferArchiveTests, extractPackage_rejectsDuplicateNormalizedPathBeforeWriting)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-duplicate-path-" + uniqueToken());
    const barrier::fs::path destination = root / "destination";

    std::string packageData("BDIRPKG1", 8);
    appendFileEntry(packageData, "folder/../same.txt", "first");
    appendFileEntry(packageData, "same.txt", "second");
    packageData.push_back('E');

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackage(packageData, destination, error));
    EXPECT_EQ("duplicate or conflicting path in transfer package", error);
    EXPECT_FALSE(barrier::fs::exists(destination));

    barrier::fs::remove_all(root);
}

TEST(TransferArchiveTests, extractPackageFile_rejectsDuplicatePathBeforeWriting)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-file-duplicate-" + uniqueToken());
    barrier::fs::create_directories(root);
    const barrier::fs::path packagePath = root / "duplicate.bdir";
    const barrier::fs::path destination = root / "destination";

    std::string packageData("BDIRPKG1", 8);
    appendFileEntry(packageData, "same.txt", "first");
    appendFileEntry(packageData, "same.txt", "second");
    packageData.push_back('E');
    {
        std::ofstream package;
        barrier::open_utf8_path(package, packagePath, std::ios::out | std::ios::binary | std::ios::trunc);
        package.write(packageData.data(), static_cast<std::streamsize>(packageData.size()));
    }

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackageFile(packagePath, destination, error));
    EXPECT_EQ("duplicate or conflicting path in transfer package", error);
    EXPECT_FALSE(barrier::fs::exists(destination));

    barrier::fs::remove_all(root);
}

TEST(TransferArchiveTests, extractPackage_rejectsPathBeyondMaximumDepthBeforeWriting)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-deep-path-" + uniqueToken());
    const barrier::fs::path destination = root / "destination";

    std::string deepPath;
    for (int i = 0; i < 65; ++i) {
        if (!deepPath.empty()) {
            deepPath += '/';
        }
        deepPath += "d";
    }

    std::string packageData("BDIRPKG1", 8);
    appendDirectoryEntry(packageData, deepPath);
    packageData.push_back('E');

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackage(packageData, destination, error));
    EXPECT_EQ("transfer package path is too deep", error);
    EXPECT_FALSE(barrier::fs::exists(destination));

    barrier::fs::remove_all(root);
}

TEST(TransferArchiveTests, extractPackage_rejectsInvalidUtf8PathBeforeWriting)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-invalid-utf8-" + uniqueToken());
    const barrier::fs::path destination = root / "destination";
    const std::string invalidPath("bad\xc3\x28.txt", 9);
    const std::string packageData = makeSingleFilePackage(invalidPath, "payload");

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackage(packageData, destination, error));
    EXPECT_EQ("invalid transfer package path encoding", error);
    EXPECT_FALSE(barrier::fs::exists(destination));

    barrier::fs::remove_all(root);
}

TEST(TransferArchiveTests, extractPackage_rejectsTooManyEntriesBeforeWriting)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-entry-limit-" + uniqueToken());
    const barrier::fs::path destination = root / "destination";

    std::string packageData("BDIRPKG1", 8);
    for (std::size_t i = 0; i < 65537; ++i) {
        appendDirectoryEntry(packageData, "d" + std::to_string(i));
    }
    packageData.push_back('E');

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackage(packageData, destination, error));
    EXPECT_EQ("transfer package has too many entries", error);
    EXPECT_FALSE(barrier::fs::exists(destination));

    barrier::fs::remove_all(root);
}

TEST(TransferArchiveTests, extractPackage_rejectsTrailingDataAfterEndMarker)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-trailing-data-" + uniqueToken());
    const barrier::fs::path destination = root / "destination";
    const std::string packageData("BDIRPKG1Etrailing", 17);

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackage(packageData, destination, error));
    EXPECT_EQ("transfer package has trailing data", error);
    EXPECT_FALSE(barrier::fs::exists(destination));

    barrier::fs::remove_all(root);
}

TEST(TransferArchiveTests, extractPackageFile_rejectsTrailingDataAfterEndMarker)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-file-trailing-" + uniqueToken());
    barrier::fs::create_directories(root);
    const barrier::fs::path packagePath = root / "trailing.bdir";
    const barrier::fs::path destination = root / "destination";
    {
        std::ofstream package;
        barrier::open_utf8_path(package, packagePath, std::ios::out | std::ios::binary | std::ios::trunc);
        package.write("BDIRPKG1Etrailing", 17);
    }

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackageFile(packagePath, destination, error));
    EXPECT_EQ("transfer package has trailing data", error);
    EXPECT_FALSE(barrier::fs::exists(destination));

    barrier::fs::remove_all(root);
}

TEST(TransferArchiveTests, createSelectionPackageFile_rejectsOversizedExpandedPayload)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-source-size-" + uniqueToken());
    barrier::fs::create_directories(root);
    const barrier::fs::path sourcePath = root / "oversized.bin";
    {
        std::ofstream source;
        barrier::open_utf8_path(source, sourcePath, std::ios::out | std::ios::binary | std::ios::trunc);
        source.seekp(static_cast<std::streamoff>(512) * 1024 * 1024);
        source.put('x');
    }

    barrier::fs::path packagePath;
    std::string error;
    EXPECT_FALSE(TransferArchive::createSelectionPackageFile({sourcePath}, packagePath, error));
    EXPECT_EQ("transfer package expanded size exceeds limit", error);
    EXPECT_TRUE(packagePath.empty());

    barrier::fs::remove_all(root);
}

TEST(TransferArchiveTests, createSelectionPackageFile_rejectsPayloadWhoseArchiveExceedsTransportLimit)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-package-size-" + uniqueToken());
    barrier::fs::create_directories(root);
    const barrier::fs::path sourcePath = root / "maximum.bin";
    {
        std::ofstream source;
        barrier::open_utf8_path(source, sourcePath, std::ios::out | std::ios::binary | std::ios::trunc);
        source.seekp(static_cast<std::streamoff>(512) * 1024 * 1024 - 1);
        source.put('x');
    }

    barrier::fs::path packagePath;
    std::string error;
    EXPECT_FALSE(TransferArchive::createSelectionPackageFile({sourcePath}, packagePath, error));
    EXPECT_EQ("transfer package exceeds size limit", error);
    EXPECT_TRUE(packagePath.empty());

    barrier::fs::remove_all(root);
}

#ifndef WIN32
TEST(TransferArchiveTests, createSelectionPackageFile_rejectsRootSymbolicLink)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-source-link-" + uniqueToken());
    barrier::fs::create_directories(root);
    const barrier::fs::path targetPath = root / "target.txt";
    const barrier::fs::path linkPath = root / "link.txt";
    {
        std::ofstream target;
        barrier::open_utf8_path(target, targetPath, std::ios::out | std::ios::binary | std::ios::trunc);
        target << "payload";
    }
    barrier::fs::create_symlink(targetPath, linkPath);

    barrier::fs::path packagePath;
    std::string error;
    EXPECT_FALSE(TransferArchive::createSelectionPackageFile({linkPath}, packagePath, error));
    EXPECT_EQ("symbolic links are not supported in file transfer", error);
    EXPECT_TRUE(packagePath.empty());

    barrier::fs::remove_all(root);
}
#endif

#ifdef WIN32
TEST(TransferArchiveTests, extractPackage_rejectsWindowsUnsafeNamesBeforeWriting)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-windows-name-" + uniqueToken());
    const barrier::fs::path destination = root / "destination";
    const std::array<const char*, 5> unsafePaths{{
        "file.txt:secret", "CON.txt", "folder./file.txt", "bad<name.txt", "C:drive-relative.txt"
    }};

    for (const char* unsafePath : unsafePaths) {
        std::string packageData = makeSingleFilePackage(unsafePath, "payload");
        std::string error;
        EXPECT_FALSE(TransferArchive::extractPackage(packageData, destination, error)) << unsafePath;
        EXPECT_EQ("unsafe Windows path in transfer package", error) << unsafePath;
        EXPECT_FALSE(barrier::fs::exists(destination)) << unsafePath;
    }

    barrier::fs::remove_all(root);
}

TEST(TransferArchiveTests, extractPackage_rejectsWindowsCaseFoldCollisionBeforeWriting)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-windows-collision-" + uniqueToken());
    const barrier::fs::path destination = root / "destination";

    std::string packageData("BDIRPKG1", 8);
    appendFileEntry(packageData, "Readme.txt", "first");
    appendFileEntry(packageData, "README.TXT", "second");
    packageData.push_back('E');

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackage(packageData, destination, error));
    EXPECT_EQ("duplicate or conflicting path in transfer package", error);
    EXPECT_FALSE(barrier::fs::exists(destination));

    barrier::fs::remove_all(root);
}

TEST(TransferArchiveTests, extractPackage_rejectsWindowsDirectoryReparsePoint)
{
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("barrier-windows-reparse-" + uniqueToken());
    const barrier::fs::path outside = root / "outside";
    const barrier::fs::path destination = root / "destination";
    barrier::fs::create_directories(outside);
    barrier::fs::create_directories(destination);

    constexpr DWORD kAllowUnprivilegedCreate = 0x2;
    const barrier::fs::path linked = destination / "linked";
    if (!CreateSymbolicLinkW(linked.c_str(), outside.c_str(),
                             SYMBOLIC_LINK_FLAG_DIRECTORY | kAllowUnprivilegedCreate)) {
        barrier::fs::remove_all(root);
        GTEST_SKIP() << "Windows symbolic-link creation is unavailable";
    }

    std::string error;
    EXPECT_FALSE(TransferArchive::extractPackage(
        makeSingleFilePackage("linked/escaped.txt", "payload"),
        destination,
        error));
    EXPECT_EQ("unsafe extraction path", error);
    EXPECT_FALSE(barrier::fs::exists(outside / "escaped.txt"));

    barrier::fs::remove_all(root);
}
#endif

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
