/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/ServerLaunchProfile.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace {

void writeFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(contents.size(), file.write(contents));
    file.close();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

} // namespace

TEST(ServerLaunchProfileTests, serviceArgumentsContainNoUserControlledPaths)
{
    QStringList args;

    EXPECT_TRUE(ServerLaunchProfile::appendProfileDirectoryArgument(
        args, true, QStringLiteral("C:/Users/Alice/AppData/Local/Weave")));
    EXPECT_TRUE(ServerLaunchProfile::appendServerConfigArgument(
        args, true, QStringLiteral("C:/Users/Alice/AppData/Local/Weave/barrier.sgc")));

    EXPECT_TRUE(args.isEmpty());
}

TEST(ServerLaunchProfileTests, desktopArgumentsPreserveProfileAndConfigPaths)
{
    QStringList args;

    EXPECT_TRUE(ServerLaunchProfile::appendProfileDirectoryArgument(
        args, false, QStringLiteral("C:/Users/Alice/AppData/Local/Weave")));
    EXPECT_TRUE(ServerLaunchProfile::appendServerConfigArgument(
        args, false, QStringLiteral("C:/Users/Alice/custom.sgc")));

    EXPECT_EQ(QStringList({
                  QStringLiteral("--profile-dir"),
                  QStringLiteral("C:/Users/Alice/AppData/Local/Weave"),
                  QStringLiteral("-c"),
                  QStringLiteral("C:/Users/Alice/custom.sgc")}),
              args);
}

TEST(ServerLaunchProfileTests, desktopArgumentsRejectEmptyRequiredPaths)
{
    QStringList args;

    EXPECT_FALSE(ServerLaunchProfile::appendProfileDirectoryArgument(args, false, QString()));
    EXPECT_FALSE(ServerLaunchProfile::appendServerConfigArgument(args, false, QString()));
    EXPECT_TRUE(args.isEmpty());
}

TEST(ServerLaunchProfileTests, canonicalConfigPathUsesTheProfileDirectory)
{
    EXPECT_EQ(QDir::cleanPath(QStringLiteral("/profile/barrier.sgc")),
              ServerLaunchProfile::canonicalConfigPath(
                  QStringLiteral("/profile"), QStringLiteral("barrier.sgc")));
}

TEST(ServerLaunchProfileTests, persistContentsCommitsOwnerOnlyFile)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString destination = directory.filePath(QStringLiteral("barrier.sgc"));
    const QByteArray contents("section: screens\nend\n");

    const auto result = ServerLaunchProfile::persistContents(contents, destination);

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(contents, readFile(destination));
    const QFileDevice::Permissions permissions = QFileInfo(destination).permissions();
    EXPECT_TRUE(permissions.testFlag(QFileDevice::ReadOwner));
    EXPECT_TRUE(permissions.testFlag(QFileDevice::WriteOwner));
#if !defined(Q_OS_WIN)
    // Qt's Windows permissions API exposes the legacy read-only attribute,
    // not distinct group/other ACL entries.
    EXPECT_FALSE(permissions.testFlag(QFileDevice::ReadGroup));
    EXPECT_FALSE(permissions.testFlag(QFileDevice::WriteGroup));
    EXPECT_FALSE(permissions.testFlag(QFileDevice::ReadOther));
    EXPECT_FALSE(permissions.testFlag(QFileDevice::WriteOther));
#endif
}

TEST(ServerLaunchProfileTests, oversizeContentsDoNotReplaceExistingConfig)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString destination = directory.filePath(QStringLiteral("barrier.sgc"));
    const QByteArray original("known-good-config");
    writeFile(destination, original);

    const auto result = ServerLaunchProfile::persistContents(
        QByteArray(static_cast<int>(ServerLaunchProfile::maximumConfigBytes() + 1), 'x'),
        destination);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(original, readFile(destination));
}

TEST(ServerLaunchProfileTests, persistExternalCopiesSelectedConfig)
{
    QTemporaryDir sourceDirectory;
    QTemporaryDir destinationDirectory;
    ASSERT_TRUE(sourceDirectory.isValid());
    ASSERT_TRUE(destinationDirectory.isValid());
    const QString source = sourceDirectory.filePath(QStringLiteral("selected.sgc"));
    const QString destination = destinationDirectory.filePath(QStringLiteral("barrier.sgc"));
    const QByteArray contents("section: links\nend\n");
    writeFile(source, contents);

    const auto result = ServerLaunchProfile::persistExternal(source, destination);

    ASSERT_TRUE(result.ok) << result.error.toStdString();
    EXPECT_EQ(contents, readFile(destination));
}

TEST(ServerLaunchProfileTests, symbolicLinkSourceIsRejectedWhenSupported)
{
    QTemporaryDir sourceDirectory;
    QTemporaryDir destinationDirectory;
    ASSERT_TRUE(sourceDirectory.isValid());
    ASSERT_TRUE(destinationDirectory.isValid());
    const QString source = sourceDirectory.filePath(QStringLiteral("source.sgc"));
    const QString link = sourceDirectory.filePath(QStringLiteral("selected.sgc"));
    const QString destination = destinationDirectory.filePath(QStringLiteral("barrier.sgc"));
    writeFile(source, QByteArray("config"));

    if (!QFile::link(source, link) || !QFileInfo(link).isSymLink()) {
        GTEST_SKIP() << "Qt/filesystem symbolic links are unavailable";
    }

    const auto result = ServerLaunchProfile::persistExternal(link, destination);

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(QFileInfo::exists(destination));
}

TEST(ServerLaunchProfileTests, symbolicLinkDestinationIsNotReplacedWhenSupported)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString target = directory.filePath(QStringLiteral("target.sgc"));
    const QString destination = directory.filePath(QStringLiteral("barrier.sgc"));
    writeFile(target, QByteArray("protected"));

    if (!QFile::link(target, destination) || !QFileInfo(destination).isSymLink()) {
        GTEST_SKIP() << "Qt/filesystem symbolic links are unavailable";
    }

    const auto result = ServerLaunchProfile::persistContents(
        QByteArray("replacement"), destination);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(QByteArray("protected"), readFile(target));
}
