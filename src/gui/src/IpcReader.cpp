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

// uncomment to debug this end of IPC chatter
//#define BARRIER_IPC_VERBOSE

#include "IpcReader.h"
#include <QTcpSocket>
#include "Ipc.h"
#include <QDebug>
#include <QMutex>
#include <QByteArray>
#include <algorithm>
#include <cstring>
#include <limits>

#ifdef BARRIER_IPC_VERBOSE
#include <iostream>
#define IPC_LOG(x) (x)
#else // not defined BARRIER_IPC_VERBOSE
#define IPC_LOG(x)
#endif

namespace {
constexpr int kMessageCodeSize = 4;
constexpr int kMessageLengthSize = 4;
constexpr int kMaxLogLineBytes = 1024 * 1024;

QByteArray messageHeader(const char* code)
{
    return QByteArray(code, kMessageCodeSize);
}

void discardUntilNextHeader(QByteArray& buffer, int searchStart)
{
    const QByteArray headers[] = {
        messageHeader(kIpcMsgLogLine),
        messageHeader(kIpcMsgStopAck),
    };
    int nextHeader = -1;
    for (const QByteArray& header : headers) {
        const int candidate = buffer.indexOf(header, searchStart);
        if (candidate >= 0 && (nextHeader < 0 || candidate < nextHeader)) {
            nextHeader = candidate;
        }
    }
    if (nextHeader >= 0) {
        buffer.remove(0, nextHeader);
        return;
    }

    int keepBytes = 0;
    const int maxSuffix = std::min(buffer.size(), kMessageCodeSize - 1);
    for (int size = maxSuffix; size > 0; --size) {
        for (const QByteArray& header : headers) {
            if (buffer.right(size) == header.left(size)) {
                keepBytes = size;
                break;
            }
        }
        if (keepBytes != 0) {
            break;
        }
    }

    if (keepBytes > 0) {
        buffer = buffer.right(keepBytes);
    }
    else {
        buffer.clear();
    }
}
}

IpcReader::IpcReader(QTcpSocket* socket) :
m_Socket(socket)
{
    connect(m_Socket, &QTcpSocket::disconnected,
            this, &IpcReader::resetBuffer);
}

IpcReader::~IpcReader()
{
}

void IpcReader::start()
{
    connect(m_Socket, SIGNAL(readyRead()), this, SLOT(read()));
}

void IpcReader::stop()
{
    disconnect(m_Socket, SIGNAL(readyRead()), this, SLOT(read()));
    resetBuffer();
}

void IpcReader::resetBuffer()
{
    QMutexLocker locker(&m_Mutex);
    m_Buffer.clear();
}

void IpcReader::read()
{
    QMutexLocker locker(&m_Mutex);
    IPC_LOG(std::cout << "ready read" << std::endl);

    const QByteArray newData = m_Socket->readAll();
    if (newData.isEmpty()) {
        return;
    }

    m_Buffer.append(newData);

    while (true) {
        if (m_Buffer.size() < kMessageCodeSize) {
            return;
        }

        const char* bufferData = m_Buffer.constData();
        IPC_LOG(std::cout << "ipc read: "
                          << QByteArray(bufferData, kMessageCodeSize).constData()
                          << std::endl);

        const bool isLog =
            memcmp(bufferData, kIpcMsgLogLine, kMessageCodeSize) == 0;
        const bool isStopAck =
            memcmp(bufferData, kIpcMsgStopAck, kMessageCodeSize) == 0;
        if (!isLog && !isStopAck) {
            qWarning() << "Invalid IPC message header, resynchronizing buffered data";
            discardUntilNextHeader(m_Buffer, 1);
            continue;
        }

        if (isStopAck) {
            constexpr int kStopAckSize = kMessageCodeSize + 16;
            if (m_Buffer.size() < kStopAckSize) {
                return;
            }
            const quint64 requestId = bytesToUInt64(
                bufferData + kMessageCodeSize);
            const quint64 generation = bytesToUInt64(
                bufferData + kMessageCodeSize + 8);
            m_Buffer.remove(0, kStopAckSize);
            serviceStopAcknowledged(requestId, generation);
            continue;
        }

        if (m_Buffer.size() < kMessageCodeSize + kMessageLengthSize) {
            return;
        }

        const int len = bytesToInt(bufferData + kMessageCodeSize, kMessageLengthSize);
        if (len < 0 || len > kMaxLogLineBytes) {
            qWarning() << "Invalid IPC payload length" << len << ", resynchronizing buffered data";
            discardUntilNextHeader(m_Buffer, kMessageCodeSize);
            continue;
        }

        const int totalMessageSize = kMessageCodeSize + kMessageLengthSize + len;
        if (m_Buffer.size() < totalMessageSize) {
            return;
        }

        IPC_LOG(std::cout << "reading log line" << std::endl);
        const QByteArray lineData = m_Buffer.mid(kMessageCodeSize + kMessageLengthSize, len);
        m_Buffer.remove(0, totalMessageSize);
        readLogLine(QString::fromUtf8(lineData.constData(), lineData.size()));
    }
}

int IpcReader::bytesToInt(const char *buffer, int size)
{
    if (size == 1) {
        return (unsigned char)buffer[0];
    }
    else if (size == 2) {
        return
            (((unsigned char)buffer[0]) << 8) +
              (unsigned char)buffer[1];
    }
    else if (size == 4) {
        const unsigned int value =
            (static_cast<unsigned int>(static_cast<unsigned char>(buffer[0])) << 24) +
            (static_cast<unsigned int>(static_cast<unsigned char>(buffer[1])) << 16) +
            (static_cast<unsigned int>(static_cast<unsigned char>(buffer[2])) << 8) +
             static_cast<unsigned int>(static_cast<unsigned char>(buffer[3]));
        if (value > static_cast<unsigned int>(std::numeric_limits<int>::max())) {
            return -1;
        }
        return static_cast<int>(value);
    }
    else {
        return 0;
    }
}

quint64 IpcReader::bytesToUInt64(const char* buffer)
{
    quint64 value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) |
            static_cast<unsigned char>(buffer[i]);
    }
    return value;
}
