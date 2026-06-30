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
#include "../src/IpcClient.h"
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

int readBigEndianInt(const QByteArray& data, int offset)
{
    return ((static_cast<unsigned char>(data[offset]) << 24) |
            (static_cast<unsigned char>(data[offset + 1]) << 16) |
            (static_cast<unsigned char>(data[offset + 2]) << 8) |
            static_cast<unsigned char>(data[offset + 3]));
}

bool waitForAvailableBytes(QTcpSocket& socket, int expectedBytes, int timeoutMs = 1000)
{
    QElapsedTimer timer;
    timer.start();

    while (socket.bytesAvailable() < expectedBytes && timer.elapsed() < timeoutMs) {
        if (!socket.waitForReadyRead(50)) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
    }

    return socket.bytesAvailable() >= expectedBytes;
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

QByteArray sendCommandFrame(ElevateMode elevate)
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) {
        ADD_FAILURE() << "failed to listen for IPC command test";
        return QByteArray();
    }

    std::unique_ptr<QTcpSocket> rawClient(new QTcpSocket());
    rawClient->connectToHost(QHostAddress::LocalHost, server.serverPort());
    if (!rawClient->waitForConnected(1000)) {
        ADD_FAILURE() << "failed to connect IPC command test client";
        return QByteArray();
    }
    if (!server.waitForNewConnection(1000)) {
        ADD_FAILURE() << "failed to accept IPC command test client";
        return QByteArray();
    }
    std::unique_ptr<QTcpSocket> serverSocket(server.nextPendingConnection());
    if (!serverSocket) {
        ADD_FAILURE() << "missing accepted IPC command socket";
        return QByteArray();
    }

    IpcClient commandClient(rawClient.release());
    commandClient.sendCommand("weaves --example", elevate);
    EXPECT_TRUE(commandClient.waitForBytesWrittenForTest(1000));

    if (!waitForAvailableBytes(*serverSocket, 8)) {
        ADD_FAILURE() << "timed out waiting for IPC command header";
        return QByteArray();
    }

    QByteArray frame = serverSocket->peek(8);
    const int commandLength = readBigEndianInt(frame, 4);
    if (commandLength < 0) {
        ADD_FAILURE() << "invalid IPC command length";
        return QByteArray();
    }

    const int frameLength = 8 + commandLength + 1;
    if (!waitForAvailableBytes(*serverSocket, frameLength)) {
        ADD_FAILURE() << "timed out waiting for full IPC command frame";
        return QByteArray();
    }

    return serverSocket->read(frameLength);
}

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

TEST(IpcClientTests, SendCommandWritesElevateModeByte)
{
    ensureCoreApplication();

    const struct {
        ElevateMode mode;
        unsigned char expected;
    } cases[] = {
        {ElevateAsNeeded, 0},
        {ElevateAlways, 1},
        {ElevateNever, 2},
        {static_cast<ElevateMode>(-1), 0},
        {static_cast<ElevateMode>(257), 0},
        {static_cast<ElevateMode>(258), 0},
        {static_cast<ElevateMode>(999), 0},
    };

    for (const auto& testCase : cases) {
        const QByteArray frame = sendCommandFrame(testCase.mode);

        ASSERT_GE(frame.size(), 10);
        EXPECT_EQ(QByteArray(frame.constData(), 4), QByteArray(kIpcMsgCommand, 4));

        const int commandLength = readBigEndianInt(frame, 4);
        ASSERT_EQ(commandLength, 16);
        ASSERT_EQ(frame.size(), 4 + 4 + commandLength + 1);
        EXPECT_EQ(QByteArray(frame.constData() + 8, commandLength), QByteArray("weaves --example"));
        EXPECT_EQ(static_cast<unsigned char>(frame[8 + commandLength]), testCase.expected);
    }
}

TEST(IpcClientTests, QueuesOneCommandUntilSocketConnectsAfterHello)
{
    ensureCoreApplication();

    QTcpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost));

    QTcpSocket* rawClient = new QTcpSocket();
    IpcClient commandClient(rawClient);

    commandClient.sendCommand("first-command", ElevateAlways);
    commandClient.sendCommand("second-command", ElevateNever);

    rawClient->connectToHost(QHostAddress::LocalHost, server.serverPort());
    ASSERT_TRUE(rawClient->waitForConnected(1000));
    ASSERT_TRUE(server.waitForNewConnection(1000));

    std::unique_ptr<QTcpSocket> serverSocket(server.nextPendingConnection());
    ASSERT_NE(serverSocket, nullptr);
    EXPECT_TRUE(commandClient.waitForBytesWrittenForTest(1000));

    const QByteArray expectedCommand("second-command");
    const int expectedBytes = 9 + 8 + expectedCommand.size() + 1;
    ASSERT_TRUE(waitForAvailableBytes(*serverSocket, expectedBytes));

    const QByteArray bytes = serverSocket->read(expectedBytes);
    EXPECT_EQ(QByteArray(bytes.constData(), 4), QByteArray(kIpcMsgHello, 4));
    EXPECT_EQ(static_cast<unsigned char>(bytes[4]), static_cast<unsigned char>(kIpcClientGui));
    EXPECT_EQ(QByteArray(bytes.constData() + 9, 4), QByteArray(kIpcMsgCommand, 4));
    EXPECT_EQ(readBigEndianInt(bytes, 13), expectedCommand.size());
    EXPECT_EQ(QByteArray(bytes.constData() + 17, expectedCommand.size()), expectedCommand);
    EXPECT_EQ(static_cast<unsigned char>(bytes[17 + expectedCommand.size()]), 2);
}
