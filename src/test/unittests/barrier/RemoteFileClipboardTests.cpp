#include "test/global/gtest.h"

#include "barrier/RemoteFileClipboard.h"

#include "io/filesystem.h"

#include <fstream>

namespace {

std::string readFile(const barrier::fs::path& path)
{
    std::ifstream file(path.u8string().c_str(), std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return std::string();
    }

    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
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

    std::vector<barrier::fs::path> roots;
    ASSERT_TRUE(RemoteFileClipboard::extractPackage(packageData, tempRoot / "dest", roots, error)) << error;
    ASSERT_EQ(1u, roots.size());
    EXPECT_EQ("hello", readFile(tempRoot / "dest" / "source" / "folder" / "hello.txt"));

    barrier::fs::remove(packagePath);
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
