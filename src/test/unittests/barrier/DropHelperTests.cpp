#include "test/global/gtest.h"

#include "barrier/DropHelper.h"
#include "barrier/TransferArchive.h"

#include "io/filesystem.h"

#include <array>
#include <fstream>

namespace {

DragFileList makeSingleFileList(const String& filename, size_t size)
{
    DragFileList files;
    DragInformation info;
    String name = filename;
    info.setFilename(name);
    info.setFilesize(size);
    info.setEntryType(DragInformation::File);
    files.push_back(info);
    return files;
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

bool hasUnpackStagingDirs(const barrier::fs::path& root)
{
    for (barrier::fs::directory_iterator it(root), end; it != end; ++it) {
        const std::string name = it->path().filename().u8string();
        if (name.compare(0, std::string(".barrier-unpack-").size(),
                         ".barrier-unpack-") == 0 ||
            name.compare(0, std::string(".weave-unpack-").size(),
                         ".weave-unpack-") == 0) {
            return true;
        }
    }
    return false;
}

barrier::fs::path withWindowsExtendedLengthPrefix(
    const barrier::fs::path& path)
{
#if defined(_WIN32)
    const std::wstring native = barrier::fs::absolute(path).native();
    if (native.compare(0, 4, L"\\\\?\\") == 0) {
        return barrier::fs::path(native);
    }
    if (native.compare(0, 2, L"\\\\") == 0) {
        return barrier::fs::path(L"\\\\?\\UNC\\" + native.substr(2));
    }
    return barrier::fs::path(L"\\\\?\\" + native);
#else
    return path;
#endif
}

} // namespace

TEST(DropHelperTests, writeToDir_clearsTransferStateWhenDestinationIsEmpty)
{
    String data(1024 * 1024, 'x');
    DragFileList files = makeSingleFileList("example.txt", data.size());

    const std::vector<String> dropped = DropHelper::writeToDir("", files, data);

    EXPECT_TRUE(dropped.empty());
    EXPECT_TRUE(files.empty());
    EXPECT_TRUE(data.empty());
    EXPECT_LT(data.capacity(), 1024u * 1024u);
}

TEST(DropHelperTests, writeToDir_writesFileAndClearsTransferState)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-drop-helper-test");
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(tempRoot);

    String data("payload");
    DragFileList files = makeSingleFileList("example.txt", data.size());

    const std::vector<String> dropped = DropHelper::writeToDir(tempRoot.u8string(), files, data);

    ASSERT_EQ(1u, dropped.size());
    EXPECT_EQ("payload", readFile(tempRoot / "example.txt"));
    EXPECT_TRUE(files.empty());
    EXPECT_TRUE(data.empty());

    barrier::fs::remove_all(tempRoot);
}

TEST(DropHelperTests, writeToDir_rejectsUnsafePortableLeafNames)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() /
        barrier::fs::u8path("weave-drop-helper-unsafe-name-test");
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(tempRoot);

    const std::array<const char*, 10> unsafeNames{{
        "file.txt:stream", "CON.txt", "report.", "report ",
        "bad<name.txt", "../escape.txt", "folder/name.txt", "LPT1.log",
        "CON .txt", "CONOUT$"
    }};
    for (const char* unsafeName : unsafeNames) {
        String data("payload");
        DragFileList files = makeSingleFileList(unsafeName, data.size());

        const std::vector<String> dropped =
            DropHelper::writeToDir(tempRoot.u8string(), files, data);

        EXPECT_TRUE(dropped.empty()) << unsafeName;
        EXPECT_TRUE(files.empty()) << unsafeName;
        EXPECT_TRUE(data.empty()) << unsafeName;
    }
    EXPECT_TRUE(barrier::fs::is_empty(tempRoot));

    barrier::fs::remove_all(tempRoot);
}

TEST(DropHelperTests, writeToDir_doesNotOverwriteExistingFinalTarget)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() /
        barrier::fs::u8path("weave-drop-helper-final-target-test");
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(tempRoot);
    {
        std::ofstream existing(
            (tempRoot / "example.txt").u8string().c_str(),
            std::ios::out | std::ios::binary | std::ios::trunc);
        existing << "user data";
    }

    String data("payload");
    DragFileList files = makeSingleFileList("example.txt", data.size());

    const std::vector<String> dropped =
        DropHelper::writeToDir(tempRoot.u8string(), files, data);

    ASSERT_EQ(1u, dropped.size());
    EXPECT_EQ("user data", readFile(tempRoot / "example.txt"));
    EXPECT_EQ("payload", readFile(tempRoot / "example (1).txt"));
    EXPECT_EQ((tempRoot / "example (1).txt").u8string(), dropped.front());

    barrier::fs::remove_all(tempRoot);
}

TEST(DropHelperTests, writeToDir_doesNotOverwriteExistingPartFile)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-drop-helper-part-test");
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(tempRoot);

    {
        std::ofstream part((tempRoot / "example.txt.barrier-part").u8string().c_str(),
                           std::ios::out | std::ios::binary | std::ios::trunc);
        part << "user data";
    }

    String data("payload");
    DragFileList files = makeSingleFileList("example.txt", data.size());

    const std::vector<String> dropped = DropHelper::writeToDir(tempRoot.u8string(), files, data);

    ASSERT_EQ(1u, dropped.size());
    EXPECT_EQ("payload", readFile(tempRoot / "example.txt"));
    EXPECT_EQ("user data", readFile(tempRoot / "example.txt.barrier-part"));
    EXPECT_TRUE(files.empty());
    EXPECT_TRUE(data.empty());

    barrier::fs::remove_all(tempRoot);
}

TEST(DropHelperTests, writeToDirFromFile_writesFileAndClearsTransferState)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-drop-helper-file-test");
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(tempRoot);

    const barrier::fs::path sourcePath = tempRoot / "received.part";
    {
        std::ofstream file(sourcePath.u8string().c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        file << "payload";
    }

    DragFileList files = makeSingleFileList("example.txt", 7);

    const std::vector<String> dropped = DropHelper::writeToDirFromFile(
        tempRoot.u8string(), files, sourcePath);

    ASSERT_EQ(1u, dropped.size());
    EXPECT_EQ("payload", readFile(tempRoot / "example.txt"));
    EXPECT_TRUE(files.empty());
    EXPECT_TRUE(barrier::fs::exists(sourcePath));

    barrier::fs::remove_all(tempRoot);
}

TEST(DropHelperTests, writeToDirFromFile_rejectsUnsafePortableLeafName)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() /
        barrier::fs::u8path("weave-drop-helper-spool-unsafe-name-test");
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(tempRoot);
    const barrier::fs::path sourcePath = tempRoot / "received.part";
    {
        std::ofstream source(sourcePath.u8string().c_str(),
                             std::ios::out | std::ios::binary |
                                 std::ios::trunc);
        source << "payload";
    }
    DragFileList files = makeSingleFileList("NUL.txt", 7);

    const std::vector<String> dropped = DropHelper::writeToDirFromFile(
        tempRoot.u8string(), files, sourcePath);

    EXPECT_TRUE(dropped.empty());
    EXPECT_TRUE(files.empty());
    EXPECT_EQ("payload", readFile(sourcePath));
    EXPECT_FALSE(barrier::fs::exists(tempRoot / "NUL.txt"));

    barrier::fs::remove_all(tempRoot);
}

TEST(DropHelperTests, writeToDirFromFile_doesNotOverwriteExistingPartFile)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-drop-helper-file-part-test");
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(tempRoot);

    const barrier::fs::path sourcePath = tempRoot / "received.part";
    {
        std::ofstream file(sourcePath.u8string().c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        file << "payload";
    }
    {
        std::ofstream part((tempRoot / "example.txt.barrier-part").u8string().c_str(),
                           std::ios::out | std::ios::binary | std::ios::trunc);
        part << "user data";
    }

    DragFileList files = makeSingleFileList("example.txt", 7);

    const std::vector<String> dropped = DropHelper::writeToDirFromFile(
        tempRoot.u8string(), files, sourcePath);

    ASSERT_EQ(1u, dropped.size());
    EXPECT_EQ("payload", readFile(tempRoot / "example.txt"));
    EXPECT_EQ("user data", readFile(tempRoot / "example.txt.barrier-part"));
    EXPECT_TRUE(files.empty());
    EXPECT_TRUE(barrier::fs::exists(sourcePath));

    barrier::fs::remove_all(tempRoot);
}

TEST(DropHelperTests, writeToDirFromFile_extractsPackageForDirectoryDrop)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-drop-helper-package-test");
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(tempRoot / "source" / "folder");
    barrier::fs::create_directories(tempRoot / "drop");

    {
        std::ofstream file((tempRoot / "source" / "folder" / "hello.txt").u8string().c_str(),
                           std::ios::out | std::ios::binary | std::ios::trunc);
        file << "hello";
    }

    barrier::fs::path packagePath;
    std::string error;
    ASSERT_TRUE(TransferArchive::createDirectoryPackageFile(tempRoot / "source", packagePath, error)) << error;

    DragFileList files;
    DragInformation info;
    String name("source");
    info.setFilename(name);
    info.setEntryType(DragInformation::Directory);
    files.push_back(info);

    const std::vector<String> dropped = DropHelper::writeToDirFromFile(
        (tempRoot / "drop").u8string(), files, packagePath);

    ASSERT_EQ(1u, dropped.size());
    EXPECT_EQ("hello", readFile(tempRoot / "drop" / "source" / "folder" / "hello.txt"));
    EXPECT_TRUE(files.empty());
    EXPECT_TRUE(barrier::fs::exists(packagePath));
    EXPECT_FALSE(hasUnpackStagingDirs(tempRoot / "drop"));

    barrier::fs::remove(packagePath);
    barrier::fs::remove_all(tempRoot);
}

TEST(DropHelperTests, writeToDirFromFile_keepsPublishedRootsWhenLaterRootFails)
{
    const barrier::fs::path tempRoot =
        withWindowsExtendedLengthPrefix(
            barrier::fs::temp_directory_path() /
            barrier::fs::u8path("weave-drop-helper-partial-bundle-test"));
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(tempRoot / "source");
    barrier::fs::create_directories(tempRoot / "drop");

    const std::string longName(255, 'z');
    {
        std::ofstream first;
        barrier::open_utf8_path(
            first, tempRoot / "source" / "a-success.txt",
            std::ios::out | std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(first.is_open());
        first << "published";
        std::ofstream blocked;
        barrier::open_utf8_path(
            blocked,
            tempRoot / "source" / barrier::fs::u8path(longName),
            std::ios::out | std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(blocked.is_open());
        blocked << "incoming";
        std::ofstream existing;
        barrier::open_utf8_path(
            existing,
            tempRoot / "drop" / barrier::fs::u8path(longName),
            std::ios::out | std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(existing.is_open());
        existing << "existing";
    }

    barrier::fs::path packagePath;
    std::string error;
    ASSERT_TRUE(TransferArchive::createSelectionPackageFile(
        {tempRoot / "source" / "a-success.txt",
         tempRoot / "source" / barrier::fs::u8path(longName)},
        packagePath, error)) << error;

    DragFileList files;
    DragInformation firstInfo;
    String firstName("a-success.txt");
    firstInfo.setFilename(firstName);
    firstInfo.setEntryType(DragInformation::File);
    files.push_back(firstInfo);
    DragInformation blockedInfo;
    String blockedName(longName);
    blockedInfo.setFilename(blockedName);
    blockedInfo.setEntryType(DragInformation::File);
    files.push_back(blockedInfo);

    const std::vector<String> dropped = DropHelper::writeToDirFromFile(
        (tempRoot / "drop").u8string(), files, packagePath);

    ASSERT_EQ(1u, dropped.size());
    EXPECT_EQ((tempRoot / "drop" / "a-success.txt").u8string(),
              dropped.front());
    EXPECT_EQ("published", readFile(tempRoot / "drop" / "a-success.txt"));
    EXPECT_EQ("existing", readFile(
        tempRoot / "drop" / barrier::fs::u8path(longName)));
    EXPECT_FALSE(hasUnpackStagingDirs(tempRoot / "drop"));
    EXPECT_TRUE(files.empty());

    barrier::fs::remove(packagePath);
    barrier::fs::remove_all(tempRoot);
}

TEST(DropHelperTests, writeToDir_removesStagingAndClearsTransferStateWhenPackageIsInvalid)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-drop-helper-bad-package-test");
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(tempRoot);

    String data("not a transfer package");
    DragFileList files;
    DragInformation info;
    String name("folder");
    info.setFilename(name);
    info.setEntryType(DragInformation::Directory);
    files.push_back(info);

    const std::vector<String> dropped = DropHelper::writeToDir(tempRoot.u8string(), files, data);

    EXPECT_TRUE(dropped.empty());
    EXPECT_TRUE(files.empty());
    EXPECT_TRUE(data.empty());
    EXPECT_FALSE(hasUnpackStagingDirs(tempRoot));

    barrier::fs::remove_all(tempRoot);
}

TEST(DropHelperTests, writeToDirFromFile_removesStagingAndClearsTransferStateWhenPackageIsInvalid)
{
    const barrier::fs::path tempRoot =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-drop-helper-bad-spooled-package-test");
    barrier::fs::remove_all(tempRoot);
    barrier::fs::create_directories(tempRoot / "drop");

    const barrier::fs::path sourcePath = tempRoot / "bad-package.bdir";
    {
        std::ofstream file(sourcePath.u8string().c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        file << "not a transfer package";
    }

    DragFileList files;
    DragInformation info;
    String name("folder");
    info.setFilename(name);
    info.setEntryType(DragInformation::Directory);
    files.push_back(info);

    const std::vector<String> dropped = DropHelper::writeToDirFromFile(
        (tempRoot / "drop").u8string(), files, sourcePath);

    EXPECT_TRUE(dropped.empty());
    EXPECT_TRUE(files.empty());
    EXPECT_TRUE(barrier::fs::exists(sourcePath));
    EXPECT_FALSE(hasUnpackStagingDirs(tempRoot / "drop"));

    barrier::fs::remove_all(tempRoot);
}
