#include "io/filesystem.h"

#include "common/common.h"
#include "test/global/gtest.h"

#include <cstdlib>
#include <fstream>

#if !SYSAPI_WIN32
#include <sys/stat.h>
#endif

namespace {

#if !SYSAPI_WIN32
class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const char* value) :
        m_name(name),
        m_hadValue(std::getenv(name) != NULL),
        m_previous(m_hadValue ? std::getenv(name) : "")
    {
        setenv(m_name.c_str(), value, 1);
    }

    ~ScopedEnvironmentVariable()
    {
        if (m_hadValue) {
            setenv(m_name.c_str(), m_previous.c_str(), 1);
        }
        else {
            unsetenv(m_name.c_str());
        }
    }

private:
    std::string m_name;
    bool m_hadValue;
    std::string m_previous;
};
#endif

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

TEST(FilesystemTests, renameNoReplacePreservesExistingTarget)
{
    const barrier::fs::path root = barrier::fs::temp_directory_path() /
        barrier::fs::u8path("weave-rename-no-replace-test");
    barrier::fs::remove_all(root);
    barrier::fs::create_directories(root);
    const barrier::fs::path source = root / "source.txt";
    const barrier::fs::path target = root / "target.txt";
    {
        std::ofstream file(source.u8string().c_str(), std::ios::binary);
        file << "incoming";
    }
    {
        std::ofstream file(target.u8string().c_str(), std::ios::binary);
        file << "user data";
    }

    std::error_code error;
    EXPECT_EQ(barrier::RenameNoReplaceResult::kTargetExists,
              barrier::rename_no_replace(source, target, error));
    EXPECT_FALSE(error);
    EXPECT_EQ("incoming", readFile(source));
    EXPECT_EQ("user data", readFile(target));

    barrier::fs::remove_all(root);
}

TEST(FilesystemTests, renameNoReplaceCommitsFileAndDirectory)
{
    const barrier::fs::path root = barrier::fs::temp_directory_path() /
        barrier::fs::u8path("weave-rename-no-replace-success-test");
    barrier::fs::remove_all(root);
    barrier::fs::create_directories(root / "source-dir");
    {
        std::ofstream file((root / "source.txt").u8string().c_str(),
                           std::ios::binary);
        file << "incoming";
    }
    {
        std::ofstream file(
            (root / "source-dir" / "nested.txt").u8string().c_str(),
            std::ios::binary);
        file << "nested";
    }

    std::error_code error;
    EXPECT_EQ(barrier::RenameNoReplaceResult::kSuccess,
              barrier::rename_no_replace(
                  root / "source.txt", root / "target.txt", error));
    EXPECT_FALSE(error);
    EXPECT_FALSE(barrier::fs::exists(root / "source.txt"));
    EXPECT_EQ("incoming", readFile(root / "target.txt"));

    EXPECT_EQ(barrier::RenameNoReplaceResult::kSuccess,
              barrier::rename_no_replace(
                  root / "source-dir", root / "target-dir", error));
    EXPECT_FALSE(error);
    EXPECT_FALSE(barrier::fs::exists(root / "source-dir"));
    EXPECT_EQ("nested", readFile(root / "target-dir" / "nested.txt"));

    barrier::fs::remove_all(root);
}

#if !SYSAPI_WIN32
TEST(FilesystemTests, createSecureTempFileDoesNotThrowForInvalidTempDirectory)
{
    ScopedEnvironmentVariable tempDirectory(
        "TMPDIR", "/definitely/missing/weave-temp-directory");
    barrier::fs::path path;

    EXPECT_NO_THROW({
        EXPECT_FALSE(barrier::create_secure_temp_file(
            "weave-test-", ".tmp", path));
    });
    EXPECT_TRUE(path.empty());
}
#endif
