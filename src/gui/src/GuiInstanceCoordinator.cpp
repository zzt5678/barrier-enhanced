/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "GuiInstanceCoordinator.h"

#include <QDir>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QThread>

#include <utility>

namespace {

QString userIdentitySuffix()
{
    const QString identity = QDir::cleanPath(QDir::homePath());
    return QString::number(qHash(identity), 16);
}

} // namespace

GuiInstanceCoordinator::GuiInstanceCoordinator(const QString& lockPath,
                                               const QString& serverName,
                                               QObject* parent) :
    QObject(parent),
    m_lockFile(lockPath),
    m_server(this),
    m_serverName(serverName)
{
    m_lockFile.setStaleLockTime(30000);
}

GuiInstanceCoordinator::StartResult
GuiInstanceCoordinator::start(int attempts, int retryDelayMs)
{
    attempts = qMax(attempts, 1);
    retryDelayMs = qMax(retryDelayMs, 0);

    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (m_lockFile.tryLock(0)) {
            QLocalServer::removeServer(m_serverName);
            m_server.setSocketOptions(QLocalServer::UserAccessOption);
            if (!m_server.listen(m_serverName)) {
                m_lockFile.unlock();
                return StartResult::Unavailable;
            }
            QObject::connect(&m_server, &QLocalServer::newConnection,
                             this, [this]() { handlePendingConnections(); });
            return StartResult::Primary;
        }

        if (requestActivation(qMax(retryDelayMs, 50))) {
            return StartResult::ExistingActivated;
        }

        if (attempt + 1 < attempts && retryDelayMs > 0) {
            QThread::msleep(static_cast<unsigned long>(retryDelayMs));
        }
    }

    return StartResult::Unavailable;
}

void
GuiInstanceCoordinator::setActivationHandler(std::function<void()> handler)
{
    m_activationHandler = std::move(handler);
    if (m_activationPending && m_activationHandler) {
        m_activationPending = false;
        m_activationHandler();
    }
}

QString
GuiInstanceCoordinator::defaultLockPath()
{
    QString runtimeDir =
        QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimeDir.isEmpty()) {
        runtimeDir = QDir::temp().filePath(
            QStringLiteral("weave-") + userIdentitySuffix());
    }
    QDir().mkpath(runtimeDir);
    return QDir(runtimeDir).filePath(QStringLiteral("weave-gui.lock"));
}

QString
GuiInstanceCoordinator::defaultServerName()
{
    return QStringLiteral("weave-gui-") + userIdentitySuffix();
}

bool
GuiInstanceCoordinator::requestActivation(int timeoutMs) const
{
    QLocalSocket socket;
    socket.connectToServer(m_serverName, QIODevice::WriteOnly);
    if (!socket.waitForConnected(timeoutMs)) {
        return false;
    }

    socket.write("activate\n");
    socket.waitForBytesWritten(timeoutMs);
    socket.disconnectFromServer();
    return true;
}

void
GuiInstanceCoordinator::handlePendingConnections()
{
    bool receivedActivation = false;
    while (m_server.hasPendingConnections()) {
        QLocalSocket* socket = m_server.nextPendingConnection();
        if (socket == nullptr) {
            continue;
        }
        receivedActivation = true;
        socket->readAll();
        socket->disconnectFromServer();
        socket->deleteLater();
    }

    if (!receivedActivation) {
        return;
    }
    if (m_activationHandler) {
        m_activationHandler();
    }
    else {
        m_activationPending = true;
    }
}
