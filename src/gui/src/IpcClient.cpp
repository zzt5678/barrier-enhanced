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

#include "IpcClient.h"
#include <QTcpSocket>
#include <QHostAddress>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <iostream>
#include <QTimer>
#include "IpcReader.h"
#include "Ipc.h"
#include <QDataStream>
#include <cstring>

namespace {

char normalizeElevateMode(ElevateMode elevate)
{
    switch (elevate) {
        case ElevateAlways:
            return 1;
        case ElevateNever:
            return 2;
        case ElevateAsNeeded:
        default:
            return 0;
    }
}

}

IpcClient::IpcClient() :
m_ReaderStarted(false),
m_Enabled(false),
m_HasPendingCommand(false),
m_PendingElevate(ElevateAsNeeded),
m_RetryTimer(this),
m_PendingStopRequestId(0)
{
    initializeSocket(new QTcpSocket(this));
}

#if defined(BARRIER_TEST_ENV)
IpcClient::IpcClient(QTcpSocket* socket) :
m_ReaderStarted(false),
m_Enabled(false),
m_HasPendingCommand(false),
m_PendingElevate(ElevateAsNeeded),
m_RetryTimer(this),
m_PendingStopRequestId(0)
{
    initializeSocket(socket);
}

#endif

IpcClient::~IpcClient()
{
}

void IpcClient::initializeSocket(QTcpSocket* socket)
{
    m_Socket = socket;
    m_Socket->setParent(this);
    connect(m_Socket, SIGNAL(connected()), this, SLOT(connected()));
    connect(m_Socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(error(QAbstractSocket::SocketError)));

    m_Reader = new IpcReader(m_Socket);
    m_Reader->setParent(this);
    connect(m_Reader, SIGNAL(readLogLine(const QString&)), this, SLOT(handleReadLogLine(const QString&)));
    connect(m_Reader, &IpcReader::serviceStopAcknowledged,
            this, &IpcClient::handleServiceStopAcknowledged);

    m_RetryTimer.setSingleShot(true);
    m_RetryTimer.setInterval(1000);
    connect(&m_RetryTimer, &QTimer::timeout, this, &IpcClient::retryConnect);
}

void IpcClient::connected()
{
    m_RetryTimer.stop();
    sendHello();
    if (m_HasPendingCommand) {
        writeCommand(m_PendingCommand, m_PendingElevate);
        m_HasPendingCommand = false;
        m_PendingCommand.clear();
    }
    if (m_PendingStopRequestId != 0 &&
        !writeStopRequest(m_PendingStopRequestId)) {
        errorMessage("service stop request remains queued after an IPC write failure");
    }
    infoMessage("connection established");
}

void IpcClient::connectToHost()
{
    m_Enabled = true;

    if (m_Socket->state() == QAbstractSocket::ConnectedState ||
        m_Socket->state() == QAbstractSocket::ConnectingState) {
        return;
    }

    infoMessage("connecting to service...");
    m_Socket->connectToHost(QHostAddress(QHostAddress::LocalHost), IPC_PORT);

    if (!m_ReaderStarted) {
        m_Reader->start();
        m_ReaderStarted = true;
    }
}

void IpcClient::disconnectFromHost()
{
    m_Enabled = false;
    m_HasPendingCommand = false;
    m_PendingCommand.clear();
    m_RetryTimer.stop();
    infoMessage("service disconnect");
    m_Reader->stop();
    m_ReaderStarted = false;
    m_Socket->close();
}

void IpcClient::error(QAbstractSocket::SocketError error)
{
    QString text;
    switch (error) {
        case 0: text = "connection refused"; break;
        case 1: text = "remote host closed"; break;
        default: text = QString("code=%1").arg(error); break;
    }

    errorMessage(QString("ipc connection error, %1").arg(text));

    if (m_Enabled && !m_RetryTimer.isActive()) {
        m_RetryTimer.start();
    }
}

void IpcClient::retryConnect()
{
    if (m_Enabled) {
        connectToHost();
    }
}

void IpcClient::sendHello()
{
    QDataStream stream(m_Socket);
    stream.writeRawData(kIpcMsgHello, 4);

    char typeBuf[1];
    typeBuf[0] = kIpcClientGui;
    stream.writeRawData(typeBuf, 1);

    char pidBuf[4];
    intToBytes(static_cast<int>(QCoreApplication::applicationPid()), pidBuf, 4);
    stream.writeRawData(pidBuf, 4);
}

void IpcClient::sendCommand(const QString& command, ElevateMode const elevate)
{
    if (m_Socket->state() != QAbstractSocket::ConnectedState) {
        m_PendingCommand = command;
        m_PendingElevate = elevate;
        m_HasPendingCommand = true;
        infoMessage("service command queued until connection is established");
        return;
    }

    writeCommand(command, elevate);
}

quint64 IpcClient::requestServiceStop()
{
    if (m_PendingStopRequestId != 0) {
        if (m_Socket->state() == QAbstractSocket::ConnectedState) {
            if (!writeStopRequest(m_PendingStopRequestId)) {
                errorMessage("service stop request remains queued after an IPC write failure");
            }
        }
        else if (m_Enabled) {
            connectToHost();
        }
        return m_PendingStopRequestId;
    }

    do {
        m_PendingStopRequestId = QRandomGenerator::system()->generate64();
    } while (m_PendingStopRequestId == 0);

    if (m_Socket->state() == QAbstractSocket::ConnectedState) {
        if (!writeStopRequest(m_PendingStopRequestId)) {
            errorMessage("service stop request remains queued after an IPC write failure");
        }
    }
    else {
        infoMessage("service stop request queued until connection is established");
        if (m_Enabled) {
            connectToHost();
        }
    }
    return m_PendingStopRequestId;
}

bool IpcClient::abandonServiceStopRequest(quint64 requestId)
{
    if (requestId == 0 || requestId != m_PendingStopRequestId) {
        return false;
    }

    m_PendingStopRequestId = 0;
    return true;
}

bool IpcClient::writeStopRequest(quint64 requestId)
{
    if (requestId == 0 ||
        m_Socket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }

    QByteArray frame(kIpcMsgStopRequest, 4);
    char requestBuf[8];
    for (int byte = 0; byte < 8; ++byte) {
        const int shift = 56 - (byte * 8);
        requestBuf[byte] = static_cast<char>((requestId >> shift) & 0xffu);
    }
    frame.append(requestBuf, 8);
    return m_Socket->write(frame) == frame.size();
}

bool IpcClient::flushPendingWrites(int timeoutMs)
{
    if (m_Socket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }

    QElapsedTimer deadline;
    deadline.start();
    while (m_Socket->bytesToWrite() > 0) {
        const int remaining = timeoutMs - static_cast<int>(deadline.elapsed());
        if (remaining <= 0 ||
            (!m_Socket->waitForBytesWritten(remaining) &&
             m_Socket->bytesToWrite() > 0)) {
            return false;
        }
    }
    return true;
}

void IpcClient::writeCommand(const QString& command, ElevateMode const elevate)
{
    QDataStream stream(m_Socket);

    stream.writeRawData(kIpcMsgCommand, 4);

    std::string stdStringCommand = command.toStdString();
    const char* charCommand = stdStringCommand.c_str();
    int length = (int)strlen(charCommand);

    char lenBuf[4];
    intToBytes(length, lenBuf, 4);
    stream.writeRawData(lenBuf, 4);
    stream.writeRawData(charCommand, length);

    char elevateBuf[1];
    elevateBuf[0] = normalizeElevateMode(elevate);
    stream.writeRawData(elevateBuf, 1);
}

void IpcClient::handleReadLogLine(const QString& text)
{
    readLogLine(text);
}

void IpcClient::handleServiceStopAcknowledged(
    quint64 requestId, quint64 commandGeneration)
{
    if (requestId == 0 || requestId != m_PendingStopRequestId) {
        return;
    }

    m_PendingStopRequestId = 0;
    serviceStopAcknowledged(requestId, commandGeneration);
}

// TODO: qt must have a built in way of converting int to bytes.
void IpcClient::intToBytes(int value, char *buffer, int size)
{
    if (size == 1) {
        buffer[0] = value & 0xff;
    }
    else if (size == 2) {
        buffer[0] = (value >> 8) & 0xff;
        buffer[1] = value & 0xff;
    }
    else if (size == 4) {
        buffer[0] = (value >> 24) & 0xff;
        buffer[1] = (value >> 16) & 0xff;
        buffer[2] = (value >> 8) & 0xff;
        buffer[3] = value & 0xff;
    }
    else {
        // TODO: other sizes, if needed.
    }
}
