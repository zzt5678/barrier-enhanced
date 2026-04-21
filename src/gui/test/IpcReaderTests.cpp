/*  barrier -- mouse and keyboard sharing utility
    Copyright (C) 2026 OpenAI

    This package is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    found in the file LICENSE that should have accompanied this file.

    This package is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "../src/Ipc.h"
#include "../src/IpcReader.h"

#include <gtest/gtest.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QStringList>
#include <QtCore/QThread>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <functional>
#include <memory>

namespace {

QCoreApplication& ensureCoreApplication()
{
    if (auto* existing = QCoreApplication::instance()) {
        return *existing;
    }

    static int argc = 1;
    static char appName[] = "guiunittests";
    static char* argv[] = {appName, nullptr};
    static QCoreApplication app(argc, argv);
    return app;
}

bool spinUntil(const std::function<bool()>& predicate, int timeoutMs = 1000)
{
    QElapsedTimer timer;
    timer.start();

    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(5);
    }

    return predicate();
}

QByteArray buildFrame(const QByteArray& payload)
{
    QByteArray frame;
    frame.append(kIpcMsgLogLine, 4);

    const int length = payload.size();
    char lenBuf[4];
    lenBuf[0] = (length >> 24) & 0xff;
    lenBuf[1] = (length >> 16) & 0xff;
    lenBuf[2] = (length >> 8) & 0xff;
    lenBuf[3] = length & 0xff;
    frame.append(lenBuf, 4);
    frame.append(payload);
    return frame;
}

void writeFrame(QTcpSocket& socket, const QByteArray& frame)
{
    ASSERT_EQ(socket.write(frame), frame.size());
    ASSERT_TRUE(socket.waitForBytesWritten(1000));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

struct IpcSockets {
    QTcpServer server;
    QTcpSocket client;
    std::unique_ptr<QTcpSocket> serverSocket;

    void connect()
    {
        ASSERT_TRUE(server.listen(QHostAddress::LocalHost));
        client.connectToHost(QHostAddress::LocalHost, server.serverPort());
        ASSERT_TRUE(client.waitForConnected(1000));
        ASSERT_TRUE(server.waitForNewConnection(1000));
        serverSocket.reset(server.nextPendingConnection());
        ASSERT_NE(serverSocket, nullptr);
    }
};

} // namespace

TEST(IpcReaderTests, EmitsOnlyAfterFullMessageIsAvailable)
{
    ensureCoreApplication();

    IpcSockets sockets;
    sockets.connect();

    IpcReader reader(sockets.serverSocket.get());
    QStringList lines;
    QObject::connect(&reader, &IpcReader::readLogLine,
                     [&](const QString& text) { lines.append(text); });
    reader.start();

    const QByteArray frame = buildFrame("fragmented log line");
    writeFrame(sockets.client, frame.left(3));
    EXPECT_TRUE(lines.isEmpty());

    writeFrame(sockets.client, frame.mid(3, 5));
    EXPECT_TRUE(lines.isEmpty());

    writeFrame(sockets.client, frame.mid(8));
    ASSERT_TRUE(spinUntil([&]() { return lines.size() == 1; }));
    EXPECT_EQ(lines.front().toStdString(), "fragmented log line");
}

TEST(IpcReaderTests, RejectsOversizedPayloadAndResynchronizesNextMessage)
{
    ensureCoreApplication();

    IpcSockets sockets;
    sockets.connect();

    IpcReader reader(sockets.serverSocket.get());
    QStringList lines;
    QObject::connect(&reader, &IpcReader::readLogLine,
                     [&](const QString& text) { lines.append(text); });
    reader.start();

    QByteArray oversizedHeader;
    oversizedHeader.append(kIpcMsgLogLine, 4);
    oversizedHeader.append(char(0x00));
    oversizedHeader.append(char(0x20));
    oversizedHeader.append(char(0x00));
    oversizedHeader.append(char(0x00));
    writeFrame(sockets.client, oversizedHeader + buildFrame("healthy frame"));
    ASSERT_TRUE(spinUntil([&]() { return lines.size() == 1; }));
    EXPECT_EQ(lines.front().toStdString(), "healthy frame");
}

TEST(IpcReaderTests, RejectsHighBitPayloadLengthAndResynchronizesNextMessage)
{
    ensureCoreApplication();

    IpcSockets sockets;
    sockets.connect();

    IpcReader reader(sockets.serverSocket.get());
    QStringList lines;
    QObject::connect(&reader, &IpcReader::readLogLine,
                     [&](const QString& text) { lines.append(text); });
    reader.start();

    QByteArray invalidLengthHeader;
    invalidLengthHeader.append(kIpcMsgLogLine, 4);
    invalidLengthHeader.append(char(0xff));
    invalidLengthHeader.append(char(0xff));
    invalidLengthHeader.append(char(0xff));
    invalidLengthHeader.append(char(0xff));
    writeFrame(sockets.client, invalidLengthHeader + buildFrame("after high-bit length"));
    ASSERT_TRUE(spinUntil([&]() { return lines.size() == 1; }));
    EXPECT_EQ(lines.front().toStdString(), "after high-bit length");
}
