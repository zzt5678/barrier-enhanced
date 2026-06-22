#include "test/global/gtest.h"

#include "barrier/DropHelper.h"
#include "barrier/TransferArchive.h"

#include "io/filesystem.h"

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

bool hasBarrierUnpackDirs(const barrier::fs::path& root)
{
    for (barrier::fs::directory_iterator it(root), end; it != end; ++it) {
        const std::string name = it->path().filename().u8string();
        if (name.compare(0, std::string(".barrier-unpack-").size(), ".barrier-unpack-") == 0) {
            return true;
        }
    }
    return false;
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
    EXPECT_FALSE(hasBarrierUnpackDirs(tempRoot));

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
    EXPECT_FALSE(hasBarrierUnpackDirs(tempRoot / "drop"));

    barrier::fs::remove_all(tempRoot);
}
