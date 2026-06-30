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
