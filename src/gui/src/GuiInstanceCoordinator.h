/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QString>

#include <functional>

class GuiInstanceCoordinator : public QObject
{
public:
    enum class StartResult {
        Primary,
        ExistingActivated,
        Unavailable
    };

    GuiInstanceCoordinator(const QString& lockPath,
                           const QString& serverName,
                           QObject* parent = nullptr);

    StartResult start(int attempts = 20, int retryDelayMs = 100);
    void setActivationHandler(std::function<void()> handler);

    static QString defaultLockPath();
    static QString defaultServerName();

private:
    bool requestActivation(int timeoutMs) const;
    void handlePendingConnections();

    QLockFile m_lockFile;
    QLocalServer m_server;
    QString m_serverName;
    std::function<void()> m_activationHandler;
    bool m_activationPending = false;
};
