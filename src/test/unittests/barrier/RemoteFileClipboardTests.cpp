#define BARRIER_TEST_ENV

#include "test/global/gtest.h"

#include "barrier/RemoteFileClipboard.h"

#include "io/filesystem.h"

#include <chrono>
#include <fstream>
#include <string>

namespace {

constexpr const char* kSessionIdA = "00000000000000000000000000000001";
constexpr const char* kSessionIdB = "00000000000000000000000000000002";
constexpr const char* kSessionIdC = "00000000000000000000000000000003";

void appendUInt32(std::string& output, UInt32 value)
{
    output.push_back(static_cast<char>((value >> 24) & 0xffu));
    output.push_back(static_cast<char>((value >> 16) & 0xffu));
    output.push_back(static_cast<char>((value >> 8) & 0xffu));
    output.push_back(static_cast<char>(value & 0xffu));
}

std::string makeRemoteFileClipboardPayload(unsigned char mode,
                                           unsigned char cut,
                                           const std::string& sessionId,
                                           const std::string& path)
{
    std::string output("WFCLIP1", 7);
    output.push_back('\0');
    output.push_back(static_cast<char>(mode));
    output.push_back(static_cast<char>(cut));
    appendUInt32(output, static_cast<UInt32>(sessionId.size()));
    output.append(sessionId);
    appendUInt32(output, 1);
    appendUInt32(output, static_cast<UInt32>(path.size()));
    output.append(path);
    return output;
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

bool hasEntryWithPrefix(const barrier::fs::path& directory,
                        const std::string& prefix)
{
    std::error_code error;
    barrier::fs::directory_iterator it(directory, error);
    const barrier::fs::directory_iterator end;
    while (!error && it != end) {
        const std::string name = it->path().filename().u8string();
        if (name.compare(0, prefix.size(), prefix) == 0) {
            return true;
        }
        it.increment(error);
    }
    return false;
}

void createCompetingEmptyDestination(const barrier::fs::path& destinationRoot)
{
    barrier::fs::create_directory(destinationRoot);
}

}

TEST(RemoteFileClipboardTests, serializeRoundTripsPaths)
{
    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.sessionId = kSessionIdA;
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
    ASSERT_EQ(RemoteFileClipboard::kSessionIdHexBytes, normalized.sessionId.size());
    for (char value : normalized.sessionId) {
        EXPECT_TRUE((value >= '0' && value <= '9') ||
                    (value >= 'a' && value <= 'f'));
    }
}

TEST(RemoteFileClipboardTests, wireParserRejectsUnassignedSourceSession)
{
    const std::string payload = makeRemoteFileClipboardPayload(
        0, 0, std::string(), "/tmp/source.txt");
    RemoteFileClipboard::Data parsed;
    std::string error;

    EXPECT_FALSE(RemoteFileClipboard::parse(payload, parsed, &error));
    EXPECT_EQ("remote file clipboard session id is invalid", error);
    EXPECT_TRUE(RemoteFileClipboard::parseLocalClipboard(
        payload, parsed, &error)) << error;
    EXPECT_TRUE(parsed.sessionId.empty());
    ASSERT_EQ(1u, parsed.paths.size());
    EXPECT_EQ("/tmp/source.txt", parsed.paths[0].u8string());
}

TEST(RemoteFileClipboardTests, createSessionIdUsesFixedWidthLowercaseHex)
{
    const std::string first = RemoteFileClipboard::createSessionId();
    const std::string second = RemoteFileClipboard::createSessionId();

    ASSERT_EQ(32u, first.size());
    ASSERT_EQ(32u, second.size());
    EXPECT_NE(first, second);
    for (char value : first) {
        EXPECT_TRUE((value >= '0' && value <= '9') ||
                    (value >= 'a' && value <= 'f'));
    }

    RemoteFileClipboard::Data parsed;
    parsed.mode = RemoteFileClipboard::Mode::SourcePaths;
    parsed.sessionId = first;
    parsed.paths.push_back(barrier::fs::u8path("/tmp/generated-session"));
    EXPECT_TRUE(RemoteFileClipboard::parse(
        RemoteFileClipboard::serialize(parsed), parsed));
}

TEST(RemoteFileClipboardTests, parseRejectsUnknownMode)
{
    RemoteFileClipboard::Data parsed;
    std::string error;
    EXPECT_FALSE(RemoteFileClipboard::parse(
        makeRemoteFileClipboardPayload(2, 0, kSessionIdA, "/tmp/example"),
        parsed, &error));
    EXPECT_EQ("remote file clipboard mode is invalid", error);
}

TEST(RemoteFileClipboardTests, parseRejectsNonBooleanCutFlag)
{
    RemoteFileClipboard::Data parsed;
    std::string error;
    EXPECT_FALSE(RemoteFileClipboard::parse(
        makeRemoteFileClipboardPayload(0, 2, kSessionIdA, "/tmp/example"),
        parsed, &error));
    EXPECT_EQ("remote file clipboard cut flag is invalid", error);
}

TEST(RemoteFileClipboardTests, parseRequiresFixedWidthLowercaseHexSessionId)
{
    RemoteFileClipboard::Data parsed;
    std::string error;

    EXPECT_FALSE(RemoteFileClipboard::parse(
        makeRemoteFileClipboardPayload(0, 0, "0123456789abcdef", "/tmp/example"),
        parsed, &error));
    EXPECT_EQ("remote file clipboard session id is invalid", error);

    EXPECT_FALSE(RemoteFileClipboard::parse(
        makeRemoteFileClipboardPayload(
            0, 0, "0000000000000000000000000000000A", "/tmp/example"),
        parsed, &error));
    EXPECT_EQ("remote file clipboard session id is invalid", error);

    EXPECT_FALSE(RemoteFileClipboard::parse(
        makeRemoteFileClipboardPayload(
            0, 0, "0000000000000000000000000000000-", "/tmp/example"),
        parsed, &error));
    EXPECT_EQ("remote file clipboard session id is invalid", error);

    EXPECT_FALSE(RemoteFileClipboard::parse(
        makeRemoteFileClipboardPayload(
            0, 0, "000000000000000000000000000000001", "/tmp/example"),
        parsed, &error));
    EXPECT_EQ("remote file clipboard session id is invalid", error);

    EXPECT_TRUE(RemoteFileClipboard::parse(
        makeRemoteFileClipboardPayload(0, 0, kSessionIdA, "/tmp/example"),
        parsed, &error)) << error;
    EXPECT_EQ(kSessionIdA, parsed.sessionId);
}

TEST(RemoteFileClipboardTests, buildMaterializedClipboardRejectsLegacySessionId)
{
    const std::vector<barrier::fs::path> paths{
        barrier::fs::u8path("/tmp/materialized-file")};
    Clipboard clipboard;

    EXPECT_FALSE(RemoteFileClipboard::buildMaterializedClipboard(
        paths, "legacy-session", clipboard));
    EXPECT_TRUE(RemoteFileClipboard::buildMaterializedClipboard(
        paths, kSessionIdA, clipboard));

    RemoteFileClipboard::Data parsed;
    ASSERT_TRUE(RemoteFileClipboard::readFromClipboard(clipboard, parsed));
    EXPECT_EQ(RemoteFileClipboard::Mode::MaterializedPaths, parsed.mode);
    EXPECT_EQ(kSessionIdA, parsed.sessionId);
}

TEST(RemoteFileClipboardTests, parseRejectsInvalidUtf8AndEmbeddedNulPaths)
{
    RemoteFileClipboard::Data parsed;
    std::string error;
    const std::string invalidUtf8("/tmp/\xc3\x28", 7);
    EXPECT_FALSE(RemoteFileClipboard::parse(
        makeRemoteFileClipboardPayload(0, 0, kSessionIdA, invalidUtf8),
        parsed, &error));
    EXPECT_EQ("remote file clipboard path entry is invalid", error);

    const std::string embeddedNul("/tmp/a\0b", 8);
    EXPECT_FALSE(RemoteFileClipboard::parse(
        makeRemoteFileClipboardPayload(0, 0, kSessionIdA, embeddedNul),
        parsed, &error));
    EXPECT_EQ("remote file clipboard path entry is invalid", error);
}

TEST(RemoteFileClipboardTests, parseRejectsTrailingData)
{
    std::string payload =
        makeRemoteFileClipboardPayload(0, 0, kSessionIdA, "/tmp/example");
    payload.push_back('x');

    RemoteFileClipboard::Data parsed;
    std::string error;
    EXPECT_FALSE(RemoteFileClipboard::parse(payload, parsed, &error));
    EXPECT_EQ("remote file clipboard payload has trailing data", error);
}

TEST(RemoteFileClipboardTests, parseFailureDoesNotModifyOutput)
{
    RemoteFileClipboard::Data parsed;
    parsed.mode = RemoteFileClipboard::Mode::MaterializedPaths;
    parsed.cut = true;
    parsed.sessionId = kSessionIdC;
    parsed.paths.push_back(barrier::fs::u8path("/tmp/keep"));

    std::string error;
    EXPECT_FALSE(RemoteFileClipboard::parse(
        makeRemoteFileClipboardPayload(0, 0, kSessionIdA, std::string()),
        parsed, &error));
    EXPECT_EQ(RemoteFileClipboard::Mode::MaterializedPaths, parsed.mode);
    EXPECT_TRUE(parsed.cut);
    EXPECT_EQ(kSessionIdC, parsed.sessionId);
    ASSERT_EQ(1u, parsed.paths.size());
    EXPECT_EQ("/tmp/keep", parsed.paths[0].u8string());
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

TEST(RemoteFileClipboardTests, parseRejectsPathLengthThatCannotFitPayload)
{
    std::string payload("WFCLIP1", 7);
    payload.push_back('\0');
    payload.push_back('\0');
    payload.push_back('\0');
    appendUInt32(payload, static_cast<UInt32>(std::string(kSessionIdA).size()));
    payload.append(kSessionIdA);
    appendUInt32(payload, 1);
    appendUInt32(payload, 0xffffffffu);

    RemoteFileClipboard::Data parsed;
    EXPECT_FALSE(RemoteFileClipboard::parse(payload, parsed));
}

TEST(RemoteFileClipboardTests, parseRejectsPathCountOverSharedLimit)
{
    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.sessionId = kSessionIdA;
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
    payload.sessionId = kSessionIdA;
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
    payload.sessionId = kSessionIdA;
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

TEST(RemoteFileClipboardTests, materializedSessionRootHashesRemoteSessionId)
{
    const barrier::fs::path cacheRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-session-root-test");
    const barrier::fs::path sessionRoot =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, "../bad/session:id");

    EXPECT_EQ(cacheRoot, sessionRoot.parent_path());
    EXPECT_EQ("weave-remote-clipboard-session-root-test", cacheRoot.filename().u8string());
    EXPECT_NE("../bad/session:id", sessionRoot.filename().u8string());
    EXPECT_EQ(64u, sessionRoot.filename().u8string().size());
}

TEST(RemoteFileClipboardTests, materializedSessionRootDoesNotCollideAfterLongPrefix)
{
    const barrier::fs::path cacheRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-collision-test");
    const std::string sharedPrefix(128, 'a');

    const barrier::fs::path first =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, sharedPrefix + "-first");
    const barrier::fs::path second =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, sharedPrefix + "-second");

    EXPECT_NE(first, second);
}

TEST(RemoteFileClipboardTests, materializedSessionRootScopesSessionByOriginAndRevision)
{
    const barrier::fs::path cacheRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-scope-test");

    const barrier::fs::path first = RemoteFileClipboard::materializedSessionRoot(
        cacheRoot, "peer-a", 41, kSessionIdA);
    const barrier::fs::path otherPeer = RemoteFileClipboard::materializedSessionRoot(
        cacheRoot, "peer-b", 41, kSessionIdA);
    const barrier::fs::path otherRevision = RemoteFileClipboard::materializedSessionRoot(
        cacheRoot, "peer-a", 42, kSessionIdA);

    EXPECT_NE(first, otherPeer);
    EXPECT_NE(first, otherRevision);
    EXPECT_EQ(cacheRoot, first.parent_path());
}

TEST(RemoteFileClipboardTests, pathsMatchUsesNormalizedUnorderedMultiset)
{
    RemoteFileClipboard::Data payload;
    payload.paths.push_back(barrier::fs::u8path("/tmp/first/../first.txt"));
    payload.paths.push_back(barrier::fs::u8path("/tmp/second.txt"));

    EXPECT_TRUE(RemoteFileClipboard::pathsMatch(
        payload, {"/tmp/second.txt", "/tmp/first.txt"}));
    EXPECT_FALSE(RemoteFileClipboard::pathsMatch(
        payload, {"/tmp/first.txt", "/tmp/first.txt"}));
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
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, kSessionIdA);
    std::vector<barrier::fs::path> roots;
    ASSERT_TRUE(RemoteFileClipboard::extractPackage(packageData, oldSessionRoot, roots, error)) << error;
    ASSERT_EQ(1u, roots.size());
    const barrier::fs::path oldMaterializedFile =
        oldSessionRoot / "source" / "hello.txt";
    ASSERT_EQ("hello", readFile(oldMaterializedFile));

    const barrier::fs::path newSessionRoot =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, kSessionIdB);
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
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, kSessionIdA);
    const barrier::fs::path middleRoot =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, kSessionIdB);
    const barrier::fs::path currentRoot =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, kSessionIdC);
    writeSizedFile(oldRoot / "payload.bin", 8);
    writeSizedFile(middleRoot / "payload.bin", 8);
    writeSizedFile(currentRoot / "payload.bin", 8);
    setSessionAge(oldRoot, 3);
    setSessionAge(middleRoot, 2);
    setSessionAge(currentRoot, 1);

    RemoteFileClipboard::pruneMaterializedCache(cacheRoot, kSessionIdC, 2, 0);

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
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, kSessionIdA);
    const barrier::fs::path currentRoot =
        RemoteFileClipboard::materializedSessionRoot(cacheRoot, kSessionIdC);
    writeSizedFile(oldRoot / "payload.bin", 16);
    writeSizedFile(currentRoot / "payload.bin", 16);
    setSessionAge(oldRoot, 2);
    setSessionAge(currentRoot, 1);

    RemoteFileClipboard::pruneMaterializedCache(cacheRoot, kSessionIdC, 8, 16);

    EXPECT_FALSE(barrier::fs::exists(oldRoot));
    EXPECT_TRUE(barrier::fs::exists(currentRoot));

    barrier::fs::remove_all(tempRoot);
}

TEST(RemoteFileClipboardTests, extractPackagePreservesDestinationRootWhenPackageIsInvalid)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-bad-package-test");
    const barrier::fs::path destinationRoot = tempRoot / "dest";
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(destinationRoot / "stale");
    writeSizedFile(destinationRoot / "stale" / "sentinel", 4);

    std::vector<barrier::fs::path> roots;
    roots.push_back(tempRoot / "old-root");
    std::string error;

    EXPECT_FALSE(RemoteFileClipboard::extractPackage(
        "not a transfer package", destinationRoot, roots, error));
    EXPECT_TRUE(roots.empty());
    EXPECT_TRUE(barrier::fs::exists(destinationRoot / "stale" / "sentinel"));

    barrier::fs::remove_all(tempRoot);
}

TEST(RemoteFileClipboardTests, extractPackageFilePreservesDestinationRootWhenPackageIsInvalid)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-bad-package-file-test");
    const barrier::fs::path destinationRoot = tempRoot / "dest";
    const barrier::fs::path packagePath = tempRoot / "bad-package.bdir";
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(destinationRoot / "stale");
    writeSizedFile(destinationRoot / "stale" / "sentinel", 4);
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
    EXPECT_TRUE(barrier::fs::exists(destinationRoot / "stale" / "sentinel"));
    EXPECT_TRUE(barrier::fs::exists(packagePath));

    barrier::fs::remove_all(tempRoot);
}

TEST(RemoteFileClipboardTests, validRetryFailsClosedAndPreservesExistingDestination)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-retry-test");
    const barrier::fs::path sourceRoot = tempRoot / "source";
    const barrier::fs::path destinationRoot = tempRoot / "dest";
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(sourceRoot);

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.paths.push_back(sourceRoot);

    writeSizedFile(sourceRoot / "payload.bin", 4);
    barrier::fs::path firstPackage;
    std::string error;
    ASSERT_TRUE(RemoteFileClipboard::createPackage(payload, firstPackage, error)) << error;

    std::vector<barrier::fs::path> roots;
    ASSERT_TRUE(RemoteFileClipboard::extractPackageFile(
        firstPackage, destinationRoot, roots, error)) << error;
    writeSizedFile(destinationRoot / "stale-sentinel", 3);

    writeSizedFile(sourceRoot / "payload.bin", 9);
    barrier::fs::path secondPackage;
    ASSERT_TRUE(RemoteFileClipboard::createPackage(payload, secondPackage, error)) << error;
    EXPECT_FALSE(RemoteFileClipboard::extractPackageFile(
        secondPackage, destinationRoot, roots, error));

    EXPECT_TRUE(roots.empty());
    EXPECT_EQ("remote file clipboard destination already exists", error);
    EXPECT_EQ(4u, barrier::fs::file_size(destinationRoot / "source" / "payload.bin"));
    EXPECT_TRUE(barrier::fs::exists(destinationRoot / "stale-sentinel"));
    EXPECT_FALSE(hasEntryWithPrefix(tempRoot, "dest.staging-"));
    EXPECT_FALSE(hasEntryWithPrefix(tempRoot, "dest.backup-"));

    barrier::fs::remove(firstPackage);
    barrier::fs::remove(secondPackage);
    barrier::fs::remove_all(tempRoot);
}

TEST(RemoteFileClipboardTests, destinationCreatedAtCommitIsNeverReplaced)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() /
        barrier::fs::u8path("weave-remote-clipboard-commit-race-test");
    const barrier::fs::path sourceRoot = tempRoot / "source";
    const barrier::fs::path destinationRoot = tempRoot / "dest";
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(sourceRoot);
    writeSizedFile(sourceRoot / "payload.bin", 4);

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.paths.push_back(sourceRoot);

    barrier::fs::path packagePath;
    std::string error;
    ASSERT_TRUE(RemoteFileClipboard::createPackage(payload, packagePath, error)) << error;

    std::vector<barrier::fs::path> roots;
    EXPECT_FALSE(RemoteFileClipboard::extractPackageFileWithCommitHookForTest(
        packagePath, destinationRoot, roots, error,
        createCompetingEmptyDestination));

    EXPECT_TRUE(roots.empty());
    EXPECT_EQ("remote file clipboard destination already exists", error);
    EXPECT_TRUE(barrier::fs::is_directory(destinationRoot));
    EXPECT_TRUE(barrier::fs::is_empty(destinationRoot));
    EXPECT_FALSE(hasEntryWithPrefix(tempRoot, "dest.staging-"));

    barrier::fs::remove(packagePath);
    barrier::fs::remove_all(tempRoot);
}

#if !defined(_WIN32)
TEST(RemoteFileClipboardTests, danglingDestinationSymlinkIsNeverReplaced)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-remote-clipboard-dangling-link-test");
    const barrier::fs::path sourceRoot = tempRoot / "source";
    const barrier::fs::path destinationRoot = tempRoot / "dest";
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(sourceRoot);
    writeSizedFile(sourceRoot / "payload.bin", 4);

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.paths.push_back(sourceRoot);

    barrier::fs::path packagePath;
    std::string error;
    ASSERT_TRUE(RemoteFileClipboard::createPackage(payload, packagePath, error)) << error;
    barrier::fs::create_symlink(tempRoot / "missing-target", destinationRoot);

    std::vector<barrier::fs::path> roots;
    EXPECT_FALSE(RemoteFileClipboard::extractPackageFile(
        packagePath, destinationRoot, roots, error));
    EXPECT_TRUE(roots.empty());
    EXPECT_EQ("remote file clipboard destination already exists", error);
    EXPECT_TRUE(barrier::fs::is_symlink(barrier::fs::symlink_status(destinationRoot)));
    EXPECT_FALSE(barrier::fs::exists(tempRoot / "missing-target"));
    EXPECT_FALSE(hasEntryWithPrefix(tempRoot, "dest.staging-"));
    EXPECT_FALSE(hasEntryWithPrefix(tempRoot, "dest.backup-"));

    barrier::fs::remove(packagePath);
    barrier::fs::remove_all(tempRoot);
}
#endif

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
    payload.sessionId = kSessionIdA;
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
