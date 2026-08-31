/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace ServerLaunchProfile {

struct PersistResult {
    bool ok = false;
    QString error;
};

qint64 maximumConfigBytes();

QString canonicalConfigPath(const QString& profileDirectory,
                            const QString& configFileName);

PersistResult persistContents(const QByteArray& contents,
                              const QString& destination);

PersistResult persistExternal(const QString& source,
                              const QString& destination);

bool appendProfileDirectoryArgument(QStringList& args,
                                    bool serviceMode,
                                    const QString& profileDirectory);

bool appendServerConfigArgument(QStringList& args,
                                bool serviceMode,
                                const QString& configFilename);

} // namespace ServerLaunchProfile
