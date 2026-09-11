#include "ActionBus.h"

#include "gtest/gtest.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QDateTime>
#include <QTemporaryDir>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace { void writeFile(const QString& path, const QByteArray& data); }

TEST(ActionBusTests, copyRecursively_preservesExistingDestination)
{
    QTemporaryDir dir;
    const QString source = dir.filePath("source.txt");
    const QString target = dir.filePath("target.txt");
    writeFile(source, "new");
    writeFile(target, "original");
    QString error;
    EXPECT_FALSE(ActionBus::testCopyRecursively(source, target, &error));
    QFile file(target);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(QByteArray("original"), file.readAll());
}

TEST(ActionBusTests, copyRecursively_boundsEmptyDirectories)
{
    QTemporaryDir dir;
    const QString source = dir.filePath("source");
    for (int i = 0; i < 5001; ++i) {
        ASSERT_TRUE(QDir().mkpath(QDir(source).filePath(QString::number(i))));
    }
    QString error;
    EXPECT_FALSE(ActionBus::testCopyRecursively(source, dir.filePath("target"), &error));
    EXPECT_FALSE(error.isEmpty());
}

namespace {

void writeFile(const QString& path, const QByteArray& data = QByteArray("payload"))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(data);
}

void setModifiedTime(const QString& path, const QDateTime& time)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadWrite));
    EXPECT_TRUE(file.setFileTime(time, QFileDevice::FileModificationTime));
}

} // namespace

TEST(ActionBusTests, pruneManagedInbox_removesOldMarkedItemsButKeepsUnmanagedFiles)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString inbox = dir.path();
    const QString oldItem = QDir(inbox).filePath(QStringLiteral("old.txt"));
    const QString currentItem = QDir(inbox).filePath(QStringLiteral("current.txt"));
    const QString unmanaged = QDir(inbox).filePath(QStringLiteral("user-owned.txt"));

    writeFile(oldItem);
    writeFile(currentItem);
    writeFile(unmanaged);
    ActionBus::testMarkInboxTargetManaged(oldItem);
    ActionBus::testMarkInboxTargetManaged(currentItem);

    const QString oldMarker = QDir(inbox).filePath(QStringLiteral(".old.txt.weave-managed"));
    const QString currentMarker = QDir(inbox).filePath(QStringLiteral(".current.txt.weave-managed"));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    setModifiedTime(oldMarker, now.addSecs(-60));
    setModifiedTime(currentMarker, now);

    ActionBus::testPruneManagedInboxDir(inbox, 1, -1);

    EXPECT_FALSE(QFileInfo::exists(oldItem));
    EXPECT_FALSE(QFileInfo::exists(oldMarker));
    EXPECT_TRUE(QFileInfo::exists(currentItem));
    EXPECT_TRUE(QFileInfo::exists(currentMarker));
    EXPECT_TRUE(QFileInfo::exists(unmanaged));
}

TEST(ActionBusTests, batchPreservesCollidingNamesAndContents)
{
    QTemporaryDir dir;
    const QString a = dir.filePath("a/results.csv");
    const QString b = dir.filePath("b/results.csv");
    writeFile(a, "first");
    writeFile(b, "second");
    QString error;
    const QString target = dir.filePath("saved");
    ASSERT_TRUE(ActionBus::testCopyInboxBatch({a, b}, target, &error)) << error.toStdString();
    const auto files = QDir(target).entryList(QDir::Files);
    ASSERT_EQ(2, files.size());
    EXPECT_TRUE(files.contains("results.csv"));
    EXPECT_TRUE(files.contains("results-1.csv"));
    QList<QByteArray> contents;
    for (const auto& name : files) {
        QFile file(QDir(target).filePath(name));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        contents << file.readAll();
    }
    EXPECT_TRUE(contents.contains("first"));
    EXPECT_TRUE(contents.contains("second"));
}

TEST(ActionBusTests, batchBudgetIsSharedAndFailureRemovesStaging)
{
    QTemporaryDir dir;
    const QString a = dir.filePath("a");
    const QString b = dir.filePath("b");
    writeFile(a, QByteArray(6, 'a'));
    writeFile(b, QByteArray(6, 'b'));
    QString error;
    EXPECT_FALSE(ActionBus::testCopyInboxBatch({a, b}, dir.filePath("saved"), &error, 10, 10));
    EXPECT_FALSE(QFileInfo::exists(dir.filePath("saved")));
    EXPECT_TRUE(QDir(dir.path()).entryList({".weave-staging-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
    EXPECT_TRUE(QFileInfo::exists(a));
    EXPECT_TRUE(QFileInfo::exists(b));
}

TEST(ActionBusTests, batchRefusesExistingTargetAndCleansPartialCopy)
{
    QTemporaryDir dir;
    const QString a = dir.filePath("a");
    const QString target = dir.filePath("saved");
    writeFile(a, "new");
    writeFile(target, "original");
    QString error;
    EXPECT_FALSE(ActionBus::testCopyInboxBatch({a}, target, &error));
    QFile file(target);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(QByteArray("original"), file.readAll());
    EXPECT_FALSE(ActionBus::testCopyInboxBatch({a, dir.filePath("missing")}, dir.filePath("batch"), &error));
    EXPECT_FALSE(QFileInfo::exists(dir.filePath("batch")));
    EXPECT_TRUE(QDir(dir.path()).entryList({".weave-staging-*"}, QDir::Dirs | QDir::Hidden).isEmpty());
}

TEST(ActionBusTests, batchBoundsDepth)
{
    QTemporaryDir dir;
    QString path = dir.filePath("source");
    for (int i = 0; i < 66; ++i) { path += "/d"; }
    ASSERT_TRUE(QDir().mkpath(path));
    QString error;
    EXPECT_FALSE(ActionBus::testCopyInboxBatch({dir.filePath("source")}, dir.filePath("saved"), &error));
    EXPECT_TRUE(error.contains("64"));
    EXPECT_FALSE(QFileInfo::exists(dir.filePath("saved")));
}

TEST(ActionBusTests, pruneManagedInbox_removesMarkedFoldersOverByteBudgetWithoutDeletingUnmanagedFolders)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString inbox = dir.path();
    const QString oldFolder = QDir(inbox).filePath(QStringLiteral("old-folder"));
    const QString unmanagedFolder = QDir(inbox).filePath(QStringLiteral("user-folder"));
    writeFile(QDir(oldFolder).filePath(QStringLiteral("large.bin")), QByteArray(32, 'x'));
    writeFile(QDir(unmanagedFolder).filePath(QStringLiteral("small.bin")), QByteArray(4, 'x'));
    ActionBus::testMarkInboxTargetManaged(oldFolder);

    const QString oldMarker = QDir(oldFolder).filePath(QStringLiteral(".weave-managed"));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    setModifiedTime(oldMarker, now.addSecs(-60));

    ActionBus::testPruneManagedInboxDir(inbox, 10, 64);

    EXPECT_FALSE(QFileInfo::exists(oldFolder));
    EXPECT_TRUE(QFileInfo::exists(unmanagedFolder));
}

TEST(ActionBusTests, pathIsInside_handlesWindowsStyleSeparatorsBeforePrefixCheck)
{
    EXPECT_TRUE(ActionBus::testPathIsInside(
        QStringLiteral("C:\\Users\\weave\\AppData\\Roaming\\Weave\\workflow\\inbox"),
        QStringLiteral("C:/Users/weave/AppData/Roaming/Weave/workflow/inbox/item.txt")));
    EXPECT_TRUE(ActionBus::testPathIsInside(
        QStringLiteral("C:/Users/weave/AppData/Roaming/Weave/workflow/inbox"),
        QStringLiteral("C:\\Users\\weave\\AppData\\Roaming\\Weave\\workflow\\inbox\\folder\\item.txt")));
    EXPECT_FALSE(ActionBus::testPathIsInside(
        QStringLiteral("C:\\Users\\weave\\AppData\\Roaming\\Weave\\workflow\\inbox"),
        QStringLiteral("C:\\Users\\weave\\AppData\\Roaming\\Weave\\workflow\\inbox-other\\item.txt")));
}

#if defined(Q_OS_UNIX)
TEST(ActionBusTests, copyRecursively_refusesSymbolicLinks)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString sourceDir = QDir(dir.path()).filePath(QStringLiteral("source"));
    const QString targetDir = QDir(dir.path()).filePath(QStringLiteral("target"));
    const QString realFile = QDir(sourceDir).filePath(QStringLiteral("real.txt"));
    const QString linkFile = QDir(sourceDir).filePath(QStringLiteral("linked.txt"));
    writeFile(realFile, QByteArray("safe"));

    ASSERT_EQ(0, ::symlink(realFile.toLocal8Bit().constData(), linkFile.toLocal8Bit().constData()));

    QString error;
    EXPECT_FALSE(ActionBus::testCopyRecursively(sourceDir, targetDir, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("symbolic link")));
    EXPECT_FALSE(QFileInfo::exists(QDir(targetDir).filePath(QStringLiteral("linked.txt"))));
}
#endif
