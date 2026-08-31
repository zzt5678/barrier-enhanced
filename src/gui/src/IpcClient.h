/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2012 Nick Bolton
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <QObject>
#include <QAbstractSocket>
#include <QString>
#include <QTimer>
#include <QtGlobal>

#include "ElevateMode.h"

class QTcpSocket;
class IpcReader;

class IpcClient : public QObject
{
    Q_OBJECT

public:
    IpcClient();
#if defined(BARRIER_TEST_ENV)
    explicit IpcClient(QTcpSocket* socket);
#endif
    virtual ~IpcClient();

    void sendHello();
    void sendCommand(const QString& command, ElevateMode elevate);
    quint64 requestServiceStop();
    bool abandonServiceStopRequest(quint64 requestId);
    bool flushPendingWrites(int timeoutMs);
    void connectToHost();
    void disconnectFromHost();

public slots:
    void retryConnect();

private:
    void initializeSocket(QTcpSocket* socket);
    void intToBytes(int value, char* buffer, int size);
    void writeCommand(const QString& command, ElevateMode elevate);
    bool writeStopRequest(quint64 requestId);

private slots:
    void connected();
    void error(QAbstractSocket::SocketError error);
    void handleReadLogLine(const QString& text);
    void handleServiceStopAcknowledged(quint64 requestId,
                                       quint64 commandGeneration);

signals:
    void readLogLine(const QString& text);
    void infoMessage(const QString& text);
    void errorMessage(const QString& text);
    void serviceStopAcknowledged(quint64 requestId,
                                 quint64 commandGeneration);

private:
    QTcpSocket* m_Socket;
    IpcReader* m_Reader;
    bool m_ReaderStarted;
    bool m_Enabled;
    bool m_HasPendingCommand;
    QString m_PendingCommand;
    ElevateMode m_PendingElevate;
    QTimer m_RetryTimer;
    quint64 m_PendingStopRequestId;
};
