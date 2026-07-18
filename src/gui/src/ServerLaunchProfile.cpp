/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ServerLaunchProfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace {

constexpr qint64 kMaximumConfigBytes = 1024 * 1024;

ServerLaunchProfile::PersistResult failure(const QString& message)
{
    return {false, message};
}

bool existingPathContainsSymbolicLink(const QString& path)
{
    const QString absolutePath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QDir absoluteDirectory(absolutePath);
    const QString rootPath = absoluteDirectory.rootPath();
    QString relativePath = QDir(rootPath).relativeFilePath(absolutePath);
    relativePath.replace(QChar('\\'), QChar('/'));
    const QStringList parts = relativePath.split(
        QChar('/'), QString::SkipEmptyParts);

    QString currentPath = rootPath;
    for (const QString& part : parts) {
        currentPath = QDir(currentPath).filePath(part);
        const QFileInfo info(currentPath);
        if (info.isSymLink()) {
            return true;
        }
        if (!info.exists()) {
            break;
        }
    }

    return false;
}

ServerLaunchProfile::PersistResult validateDestination(const QString& destination)
{
    if (destination.isEmpty()) {
        return failure(QStringLiteral("The service configuration path is empty."));
    }

    const QFileInfo destinationInfo(destination);
    if (destinationInfo.isSymLink()) {
        return failure(QStringLiteral("The service configuration path is a symbolic link."));
    }
    if (destinationInfo.exists() && !destinationInfo.isFile()) {
        return failure(QStringLiteral("The service configuration path is not a regular file."));
    }

    const QString directoryPath = destinationInfo.absolutePath();
    if (existingPathContainsSymbolicLink(directoryPath)) {
        return failure(QStringLiteral("The service configuration directory contains a symbolic link."));
    }
    if (!QDir().mkpath(directoryPath)) {
        return failure(QStringLiteral("The service configuration directory could not be created."));
    }

    const QFileInfo directoryInfo(directoryPath);
    if (!directoryInfo.exists() || !directoryInfo.isDir() || directoryInfo.isSymLink()) {
        return failure(QStringLiteral("The service configuration directory is invalid."));
    }

    return {true, QString()};
}

} // namespace

namespace ServerLaunchProfile {

qint64 maximumConfigBytes()
{
    return kMaximumConfigBytes;
}

QString canonicalConfigPath(const QString& profileDirectory,
                            const QString& configFileName)
{
    return QDir::cleanPath(QDir(profileDirectory).filePath(configFileName));
}

PersistResult persistContents(const QByteArray& contents,
                              const QString& destination)
{
    if (contents.size() > kMaximumConfigBytes) {
        return failure(QStringLiteral("The service configuration exceeds the 1 MiB limit."));
    }

    const PersistResult destinationResult = validateDestination(destination);
    if (!destinationResult.ok) {
        return destinationResult;
    }

    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) {
        return failure(QStringLiteral("The service configuration could not be opened: %1")
                           .arg(output.errorString()));
    }

    const QFileDevice::Permissions ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!output.setPermissions(ownerOnly)) {
        output.cancelWriting();
        return failure(QStringLiteral("Owner-only service configuration permissions could not be set."));
    }

    if (output.write(contents) != contents.size()) {
        const QString error = output.errorString();
        output.cancelWriting();
        return failure(QStringLiteral("The service configuration could not be written: %1")
                           .arg(error));
    }

    if (!output.commit()) {
        return failure(QStringLiteral("The service configuration could not be committed atomically: %1")
                           .arg(output.errorString()));
    }

    if (!QFile::setPermissions(destination, ownerOnly)) {
        return failure(QStringLiteral("Owner-only service configuration permissions could not be verified."));
    }

    return {true, QString()};
}

PersistResult persistExternal(const QString& source,
                              const QString& destination)
{
    if (source.isEmpty()) {
        return failure(QStringLiteral("The selected configuration path is empty."));
    }
    if (existingPathContainsSymbolicLink(source)) {
        return failure(QStringLiteral("The selected configuration path contains a symbolic link."));
    }

    const QFileInfo sourceInfo(source);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        return failure(QStringLiteral("The selected configuration is not a regular file."));
    }
    if (sourceInfo.size() > kMaximumConfigBytes) {
        return failure(QStringLiteral("The selected configuration exceeds the 1 MiB limit."));
    }

    QFile input(source);
    if (!input.open(QIODevice::ReadOnly)) {
        return failure(QStringLiteral("The selected configuration could not be opened: %1")
                           .arg(input.errorString()));
    }

    const QByteArray contents = input.read(kMaximumConfigBytes + 1);
    if (input.error() != QFileDevice::NoError) {
        return failure(QStringLiteral("The selected configuration could not be read: %1")
                           .arg(input.errorString()));
    }
    if (contents.size() > kMaximumConfigBytes) {
        return failure(QStringLiteral("The selected configuration exceeds the 1 MiB limit."));
    }

    return persistContents(contents, destination);
}

bool appendProfileDirectoryArgument(QStringList& args,
                                    bool serviceMode,
                                    const QString& profileDirectory)
{
    if (serviceMode) {
        return true;
    }
    if (profileDirectory.isEmpty()) {
        return false;
    }

    args << QStringLiteral("--profile-dir") << profileDirectory;
    return true;
}

bool appendServerConfigArgument(QStringList& args,
                                bool serviceMode,
                                const QString& configFilename)
{
    if (serviceMode) {
        return true;
    }
    if (configFilename.isEmpty()) {
        return false;
    }

    args << QStringLiteral("-c") << configFilename;
    return true;
}

} // namespace ServerLaunchProfile
