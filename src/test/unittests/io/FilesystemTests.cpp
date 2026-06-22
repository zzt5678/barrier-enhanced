#include "io/filesystem.h"

#include "common/common.h"
#include "test/global/gtest.h"

#include <fstream>

#if !SYSAPI_WIN32
#include <sys/stat.h>
#endif

namespace {

bool endsWith(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string readFile(const barrier::fs::path& path)
{
    std::ifstream file;
    barrier::open_utf8_path(file, path, std::ios::in | std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

} // namespace

TEST(FilesystemTests, createSecureTempFileCreatesWritableFileWithPrefixSuffix)
{
    barrier::fs::path path;

    ASSERT_TRUE(barrier::create_secure_temp_file("weave-test-", ".tmp", path));
    EXPECT_FALSE(path.empty());
    EXPECT_TRUE(barrier::fs::exists(path));
    EXPECT_EQ(0u, path.filename().u8string().find("weave-test-"));
    EXPECT_TRUE(endsWith(path.filename().u8string(), ".tmp"));

    std::ofstream output;
    barrier::open_utf8_path(output, path, std::ios::out | std::ios::binary | std::ios::app);
    ASSERT_TRUE(output.is_open());
    output << "payload";
    output.close();
    EXPECT_EQ("payload", readFile(path));

#if !SYSAPI_WIN32
    struct stat info;
    ASSERT_EQ(0, stat(path.native().c_str(), &info));
    EXPECT_EQ(0, info.st_mode & (S_IRWXG | S_IRWXO));
#endif

    barrier::fs::remove(path);
}

TEST(FilesystemTests, createSecureTempFileReturnsFalseWhenParentIsMissing)
{
    barrier::fs::path path;

    EXPECT_FALSE(barrier::create_secure_temp_file(
        "weave-missing-parent/", ".tmp", path));
    EXPECT_TRUE(path.empty());
}
