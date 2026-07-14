#include "test/global/gtest.h"

#include "barrier/RemoteFileClipboard.h"

#include "io/filesystem.h"

#include <chrono>
#include <fstream>
#include <string>

namespace {

void appendUInt32(std::string& output, UInt32 value)
{
    output.push_back(static_cast<char>((value >> 24) & 0xffu));
    output.push_back(static_cast<char>((value >> 16) & 0xffu));
    output.push_back(static_cast<char>((value >> 8) & 0xffu));
    output.push_back(static_cast<char>(value & 0xffu));
}

std::string readFile(const barrier::fs::path& path)
{
    std::ifstream file(path.u8string().c_str(), std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return std::string();
    }

    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

void writeSizedFile(const barrier::fs::path& path, std::size_t size)
{
    barrier::fs::create_directories(path.parent_path());
    std::ofstream file(path.u8string().c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    file << std::string(size, 'x');
}

void setSessionAge(const barrier::fs::path& path, int ageHours)
{
    const auto time = barrier::fs::file_time_type::clock::now() - std::chrono::hours(ageHours);
    barrier::fs::last_write_time(path, time);
}

}

TEST(RemoteFileClipboardTests, serializeRoundTripsPaths)
{
    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.sessionId = "session-1";
    payload.paths.push_back(barrier::fs::u8path("/tmp/example.txt"));
    payload.paths.push_back(barrier::fs::u8path("/tmp/folder"));

    RemoteFileClipboard::Data parsed;
    ASSERT_TRUE(RemoteFileClipboard::parse(RemoteFileClipboard::serialize(payload), parsed));
    EXPECT_EQ(payload.sessionId, parsed.sessionId);
    EXPECT_EQ(payload.paths.size(), parsed.paths.size());
    EXPECT_EQ(payload.paths[0].u8string(), parsed.paths[0].u8string());
    EXPECT_EQ(payload.paths[1].u8string(), parsed.paths[1].u8string());
}

TEST(RemoteFileClipboardTests, normalizeClipboardAddsSessionId)
{
    Clipboard clipboard;
    ASSERT_TRUE(clipboard.open(0));
    clipboard.empty();

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.paths.push_back(barrier::fs::u8path("/tmp/source.txt"));
    clipboard.add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
    clipboard.close();

    RemoteFileClipboard::Data normalized;
    ASSERT_TRUE(RemoteFileClipboard::normalizeClipboard(clipboard, &normalized));
    EXPECT_FALSE(normalized.sessionId.empty());
}

TEST(RemoteFileClipboardTests, parseRejectsPathCountThatCannotFitPayload)
{
    std::string payload("WFCLIP1", 7);
    payload.push_back('\0');
    payload.push_back('\0');
    payload.push_back('\0');
    appendUInt32(payload, 0);
    appendUInt32(payload, 0xffffffffu);

    RemoteFileClipboard::Data parsed;
    EXPECT_FALSE(RemoteFileClipboard::parse(payload, parsed));
}

TEST(RemoteFileClipboardTests, parseRejectsPathCountOverSharedLimit)
{
    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    for (std::size_t i = 0; i < RemoteFileClipboard::kMaxClipboardPathCount + 1; ++i) {
        payload.paths.push_back(barrier::fs::u8path("/tmp/file-" + std::to_string(i)));
    }

    RemoteFileClipboard::Data parsed;
    std::string error;
    EXPECT_FALSE(RemoteFileClipboard::parse(RemoteFileClipboard::serialize(payload), parsed, &error));
    EXPECT_EQ("remote file clipboard path count is too large", error);
}

TEST(RemoteFileClipboardTests, parseRejectsSinglePathOverSharedLimit)
{
    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.paths.push_back(barrier::fs::u8path("/tmp/" + std::string(RemoteFileClipboard::kMaxClipboardPathBytes, 'a')));

    RemoteFileClipboard::Data parsed;
    std::string error;
    EXPECT_FALSE(RemoteFileClipboard::parse(RemoteFileClipboard::serialize(payload), parsed, &error));
    EXPECT_EQ("remote file clipboard path entry is too large", error);
}

TEST(RemoteFileClipboardTests, parseRejectsTotalPathBytesOverSharedLimit)
{
    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    for (std::size_t i = 0; i < 17; ++i) {
        payload.paths.push_back(barrier::fs::u8path("/tmp/" + std::string(64 * 1024 - 5, 'a')));
    }

    RemoteFileClipboard::Data parsed;
    std::string error;
    EXPECT_FALSE(RemoteFileClipboard::parse(RemoteFileClipboard::serialize(payload), parsed, &error));
    EXPECT_EQ("remote file clipboard path list is too large", error);
}

TEST(RemoteFileClipboardTests, createPackageRejectsOversizedPathListBeforeArchiveCreation)
{
    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.paths.push_back(barrier::fs::u8path("/tmp/" + std::string(RemoteFileClipboard::kMaxClipboardPathBytes, 'a')));

    barrier::fs::path packagePath;
    std::string error;
    EXPECT_FALSE(RemoteFileClipboard::createPackage(payload, packagePath, error));
    EXPECT_EQ("remote file clipboard path entry is too large", error);
    EXPECT_TRUE(packagePath.empty());
}

TEST(RemoteFileClipboardTests, createAndExtractPackageRestoresRoots)
{
    const barrier::fs::path tempRoot = barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-test");
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(tempRoot / "source" / "folder");

    {
        std::ofstream file((tempRoot / "source" / "folder" / "hello.txt").u8string().c_str(),
                           std::ios::out | std::ios::binary | std::ios::trunc);
        file << "hello";
    }

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.paths.push_back(tempRoot / "source");

    barrier::fs::path packagePath;
    std::string error;
    ASSERT_TRUE(RemoteFileClipboard::createPackage(payload, packagePath, error)) << error;

    std::ifstream packageFile(packagePath.u8string().c_str(), std::ios::in | std::ios::binary);
    const std::string packageData((std::istreambuf_iterator<char>(packageFile)),
                                  std::istreambuf_iterator<char>());
    packageFile.close();

    std::vector<barrier::fs::path> roots;
    ASSERT_TRUE(RemoteFileClipboard::extractPackage(packageData, tempRoot / "dest", roots, error)) << error;
    ASSERT_EQ(1u, roots.size());
    EXPECT_EQ("hello", readFile(tempRoot / "dest" / "source" / "folder" / "hello.txt"));

    barrier::fs::remove(packagePath);
    barrier::fs::remove_all(tempRoot);
}

TEST(RemoteFileClipboardTests, extractPackageFileRestoresRoots)
{
    const barrier::fs::path tempRoot = barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-file-test");
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(tempRoot / "source" / "folder");

    {
        std::ofstream file((tempRoot / "source" / "folder" / "hello.txt").u8string().c_str(),
                           std::ios::out | std::ios::binary | std::ios::trunc);
        file << "hello";
    }

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.paths.push_back(tempRoot / "source");

    barrier::fs::path packagePath;
    std::string error;
    ASSERT_TRUE(RemoteFileClipboard::createPackage(payload, packagePath, error)) << error;

    std::vector<barrier::fs::path> roots;
    ASSERT_TRUE(RemoteFileClipboard::extractPackageFile(packagePath, tempRoot / "dest", roots, error)) << error;
    ASSERT_EQ(1u, roots.size());
    EXPECT_EQ("hello", readFile(tempRoot / "dest" / "source" / "folder" / "hello.txt"));

    barrier::fs::remove(packagePath);
    barrier::fs::remove_all(tempRoot);
}

TEST(RemoteFileClipboardTests, materializedSessionRootSanitizesRemoteSessionId)
{
    const barrier::fs::path cacheRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-session-root-test");
    const barrier::fs::path sessionRoot =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, "../bad/session:id");

    EXPECT_EQ(cacheRoot, sessionRoot.parent_path());
    EXPECT_EQ("weave-remote-clipboard-session-root-test", cacheRoot.filename().u8string());
    EXPECT_NE("../bad/session:id", sessionRoot.filename().u8string());
    EXPECT_EQ("_2e_2e_2fbad_2fsession_3aid", sessionRoot.filename().u8string());
}

TEST(RemoteFileClipboardTests, failedNewSessionExtractionPreservesPreviousSessionFiles)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-session-preserve-test");
    const barrier::fs::path cacheRoot = tempRoot / "cache";
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(tempRoot / "source");

    {
        std::ofstream file((tempRoot / "source" / "hello.txt").u8string().c_str(),
                           std::ios::out | std::ios::binary | std::ios::trunc);
        file << "hello";
    }

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.paths.push_back(tempRoot / "source");

    barrier::fs::path packagePath;
    std::string error;
    ASSERT_TRUE(RemoteFileClipboard::createPackage(payload, packagePath, error)) << error;

    std::ifstream packageFile(packagePath.u8string().c_str(), std::ios::in | std::ios::binary);
    const std::string packageData((std::istreambuf_iterator<char>(packageFile)),
                                  std::istreambuf_iterator<char>());
    packageFile.close();

    const barrier::fs::path oldSessionRoot =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, "old-session");
    std::vector<barrier::fs::path> roots;
    ASSERT_TRUE(RemoteFileClipboard::extractPackage(packageData, oldSessionRoot, roots, error)) << error;
    ASSERT_EQ(1u, roots.size());
    const barrier::fs::path oldMaterializedFile =
        oldSessionRoot / "source" / "hello.txt";
    ASSERT_EQ("hello", readFile(oldMaterializedFile));

    const barrier::fs::path newSessionRoot =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, "new-session");
    EXPECT_FALSE(RemoteFileClipboard::extractPackage(
        "not a transfer package", newSessionRoot, roots, error));

    EXPECT_TRUE(barrier::fs::exists(oldMaterializedFile));
    EXPECT_EQ("hello", readFile(oldMaterializedFile));
    EXPECT_FALSE(barrier::fs::exists(newSessionRoot));

    barrier::fs::remove(packagePath);
    barrier::fs::remove_all(tempRoot);
}

TEST(RemoteFileClipboardTests, pruneMaterializedCache_removesOldSessionsOverCount)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-prune-count-test");
    const barrier::fs::path cacheRoot = tempRoot / "cache";
    barrier::fs::remove_all(tempRoot);

    const barrier::fs::path oldRoot =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, "old");
    const barrier::fs::path middleRoot =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, "middle");
    const barrier::fs::path currentRoot =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, "current");
    writeSizedFile(oldRoot / "payload.bin", 8);
    writeSizedFile(middleRoot / "payload.bin", 8);
    writeSizedFile(currentRoot / "payload.bin", 8);
    setSessionAge(oldRoot, 3);
    setSessionAge(middleRoot, 2);
    setSessionAge(currentRoot, 1);

    RemoteFileClipboard::pruneMaterializedCache(cacheRoot, "current", 2, 0);

    EXPECT_FALSE(barrier::fs::exists(oldRoot));
    EXPECT_TRUE(barrier::fs::exists(middleRoot));
    EXPECT_TRUE(barrier::fs::exists(currentRoot));

    barrier::fs::remove_all(tempRoot);
}

TEST(RemoteFileClipboardTests, pruneMaterializedCache_keepsCurrentSessionWhenOverBytes)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-prune-bytes-test");
    const barrier::fs::path cacheRoot = tempRoot / "cache";
    barrier::fs::remove_all(tempRoot);

    const barrier::fs::path oldRoot =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, "old");
    const barrier::fs::path currentRoot =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, "current");
    writeSizedFile(oldRoot / "payload.bin", 16);
    writeSizedFile(currentRoot / "payload.bin", 16);
    setSessionAge(oldRoot, 2);
    setSessionAge(currentRoot, 1);

    RemoteFileClipboard::pruneMaterializedCache(cacheRoot, "current", 8, 16);

    EXPECT_FALSE(barrier::fs::exists(oldRoot));
    EXPECT_TRUE(barrier::fs::exists(currentRoot));

    barrier::fs::remove_all(tempRoot);
}

TEST(RemoteFileClipboardTests, extractPackageRemovesDestinationRootWhenPackageIsInvalid)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-bad-package-test");
    const barrier::fs::path destinationRoot = tempRoot / "dest";
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(destinationRoot / "stale");

    std::vector<barrier::fs::path> roots;
    roots.push_back(tempRoot / "old-root");
    std::string error;

    EXPECT_FALSE(RemoteFileClipboard::extractPackage(
        "not a transfer package", destinationRoot, roots, error));
    EXPECT_TRUE(roots.empty());
    EXPECT_FALSE(barrier::fs::exists(destinationRoot));

    barrier::fs::remove_all(tempRoot);
}

TEST(RemoteFileClipboardTests, extractPackageFileRemovesDestinationRootWhenPackageIsInvalid)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-bad-package-file-test");
    const barrier::fs::path destinationRoot = tempRoot / "dest";
    const barrier::fs::path packagePath = tempRoot / "bad-package.bdir";
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(destinationRoot / "stale");
    barrier::fs::create_directories(tempRoot);

    {
        std::ofstream file(packagePath.u8string().c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        file << "not a transfer package";
    }

    std::vector<barrier::fs::path> roots;
    roots.push_back(tempRoot / "old-root");
    std::string error;

    EXPECT_FALSE(RemoteFileClipboard::extractPackageFile(packagePath, destinationRoot, roots, error));
    EXPECT_TRUE(roots.empty());
    EXPECT_FALSE(barrier::fs::exists(destinationRoot));
    EXPECT_TRUE(barrier::fs::exists(packagePath));

    barrier::fs::remove_all(tempRoot);
}

TEST(RemoteFileClipboardTests, allPathsLookLikeImages_acceptsOnlyImageExtensions)
{
    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.paths.push_back(barrier::fs::u8path("/tmp/example.png"));
    payload.paths.push_back(barrier::fs::u8path("/tmp/photo.JPEG"));

    EXPECT_TRUE(RemoteFileClipboard::allPathsLookLikeImages(payload));

    payload.paths.push_back(barrier::fs::u8path("/tmp/readme.txt"));
    EXPECT_FALSE(RemoteFileClipboard::allPathsLookLikeImages(payload));
}

TEST(RemoteFileClipboardTests, stripImageFileTransferMetadata_removesFileListAndPathText)
{
    Clipboard clipboard;
    ASSERT_TRUE(clipboard.open(0));
    clipboard.empty();
    clipboard.add(IClipboard::kPNG, "fake-png");

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.paths.push_back(barrier::fs::u8path("/tmp/example.png"));
    clipboard.add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
    clipboard.add(IClipboard::kText, "/tmp/example.png\n");
    clipboard.close();

    ASSERT_TRUE(RemoteFileClipboard::stripImageFileTransferMetadata(clipboard));
    ASSERT_TRUE(clipboard.open(0));
    EXPECT_TRUE(clipboard.has(IClipboard::kPNG));
    EXPECT_FALSE(clipboard.has(IClipboard::kFileList));
    EXPECT_FALSE(clipboard.has(IClipboard::kText));
    clipboard.close();
}

TEST(RemoteFileClipboardTests, stripImageFileTransferMetadata_ignoresPlainTextClipboard)
{
    Clipboard clipboard;
    ASSERT_TRUE(clipboard.open(0));
    clipboard.empty();
    clipboard.add(IClipboard::kText, "plain text");
    clipboard.close();

    EXPECT_FALSE(RemoteFileClipboard::stripImageFileTransferMetadata(clipboard));
    ASSERT_TRUE(clipboard.open(0));
    EXPECT_TRUE(clipboard.has(IClipboard::kText));
    EXPECT_EQ("plain text", clipboard.get(IClipboard::kText));
    clipboard.close();
}

TEST(RemoteFileClipboardTests, prepareForAutomaticClipboardSharing_blocksLocalFileList)
{
    Clipboard clipboard;
    ASSERT_TRUE(clipboard.open(0));
    clipboard.empty();

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.paths.push_back(barrier::fs::u8path("/tmp/example.txt"));
    clipboard.add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
    clipboard.add(IClipboard::kText, "/tmp/example.txt\n");
    clipboard.close();

    EXPECT_EQ(RemoteFileClipboard::AutomaticSharingStatus::ContainsFileList,
              RemoteFileClipboard::prepareForAutomaticClipboardSharing(clipboard));
    ASSERT_TRUE(clipboard.open(0));
    EXPECT_TRUE(clipboard.has(IClipboard::kFileList));
    EXPECT_TRUE(clipboard.has(IClipboard::kText));
    clipboard.close();
}

TEST(RemoteFileClipboardTests, prepareForAutomaticClipboardSharing_allowsMaterializedFileList)
{
    Clipboard clipboard;
    ASSERT_TRUE(clipboard.open(0));
    clipboard.empty();

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::MaterializedPaths;
    payload.sessionId = "remote-ready-session";
    payload.paths.push_back(barrier::fs::u8path("/tmp/weave-cache/example.txt"));
    clipboard.add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
    clipboard.close();

    EXPECT_EQ(RemoteFileClipboard::AutomaticSharingStatus::Safe,
              RemoteFileClipboard::prepareForAutomaticClipboardSharing(clipboard));
    ASSERT_TRUE(clipboard.open(0));
    EXPECT_TRUE(clipboard.has(IClipboard::kFileList));
    clipboard.close();
}

TEST(RemoteFileClipboardTests, prepareForAutomaticClipboardSharing_blocksNativeFileList)
{
    Clipboard clipboard;
    ASSERT_TRUE(clipboard.open(0));
    clipboard.empty();
    clipboard.add(IClipboard::kFileList, "native-file-list");
    clipboard.add(IClipboard::kText, "/tmp/native-file.txt\n");
    clipboard.close();

    EXPECT_EQ(RemoteFileClipboard::AutomaticSharingStatus::ContainsFileList,
              RemoteFileClipboard::prepareForAutomaticClipboardSharing(clipboard));
    ASSERT_TRUE(clipboard.open(0));
    EXPECT_TRUE(clipboard.has(IClipboard::kFileList));
    EXPECT_TRUE(clipboard.has(IClipboard::kText));
    clipboard.close();
}

TEST(RemoteFileClipboardTests, prepareForAutomaticClipboardSharing_stripsImageSourceFileList)
{
    Clipboard clipboard;
    ASSERT_TRUE(clipboard.open(0));
    clipboard.empty();
    clipboard.add(IClipboard::kPNG, "fake-png");

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.paths.push_back(barrier::fs::u8path("/tmp/example.png"));
    clipboard.add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
    clipboard.add(IClipboard::kText, "/tmp/example.png\n");
    clipboard.close();

    EXPECT_EQ(RemoteFileClipboard::AutomaticSharingStatus::SafeAfterRemovingImageFileMetadata,
              RemoteFileClipboard::prepareForAutomaticClipboardSharing(clipboard));
    ASSERT_TRUE(clipboard.open(0));
    EXPECT_TRUE(clipboard.has(IClipboard::kPNG));
    EXPECT_FALSE(clipboard.has(IClipboard::kFileList));
    EXPECT_FALSE(clipboard.has(IClipboard::kText));
    clipboard.close();
}

TEST(RemoteFileClipboardTests, prepareForAutomaticClipboardSharing_allowsPlainText)
{
    Clipboard clipboard;
    ASSERT_TRUE(clipboard.open(0));
    clipboard.empty();
    clipboard.add(IClipboard::kText, "plain text");
    clipboard.close();

    EXPECT_EQ(RemoteFileClipboard::AutomaticSharingStatus::Safe,
              RemoteFileClipboard::prepareForAutomaticClipboardSharing(clipboard));
    ASSERT_TRUE(clipboard.open(0));
    EXPECT_TRUE(clipboard.has(IClipboard::kText));
    EXPECT_EQ("plain text", clipboard.get(IClipboard::kText));
    clipboard.close();
}
