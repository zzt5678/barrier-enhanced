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

#include "ipc/IpcLogOutputter.h"

#include "ipc/IpcServer.h"
#include "ipc/IpcMessage.h"
#include "ipc/Ipc.h"
#include "ipc/IpcClientProxy.h"
#include "mt/Thread.h"
#include "mt/ThreadShutdown.h"
#include "arch/Arch.h"
#include "arch/XArch.h"
#include "base/Event.h"
#include "base/EventQueue.h"
#include "base/TMethodEventJob.h"

enum EIpcLogOutputter {
    kBufferMaxSize = 1000,
    kMaxSendLines = 100,
    kBufferRateWriteLimit = 1000, // writes per kBufferRateTime
    kBufferRateTimeLimit = 1, // seconds
    kBufferMaxBytes = 256 * 1024,
    kBufferMaxLineBytes = 64 * 1024
};

IpcLogOutputter::IpcLogOutputter(IpcServer& ipcServer, EIpcClientType clientType, bool useThread) :
    m_ipcServer(ipcServer),
    m_bufferBytes(0),
    m_sending(false),
    m_bufferThread(nullptr),
    m_running(false),
    m_notifyCond(ARCH->newCondVar()),
    m_notifyMutex(ARCH->newMutex()),
    m_bufferWaiting(false),
    m_bufferThreadId(0),
    m_bufferMaxSize(kBufferMaxSize),
    m_bufferMaxBytes(kBufferMaxBytes),
    m_bufferMaxLineBytes(kBufferMaxLineBytes),
    m_bufferRateWriteLimit(kBufferRateWriteLimit),
    m_bufferRateTimeLimit(kBufferRateTimeLimit),
    m_bufferWriteCount(0),
    m_bufferRateStart(ARCH->time()),
    m_clientType(clientType)
{
    if (useThread) {
        m_bufferThread = new Thread([this](){ buffer_thread(); });
    }
}

IpcLogOutputter::~IpcLogOutputter()
{
    close();

    if (m_bufferThread != nullptr) {
        m_bufferThread->cancel();
        m_bufferThread->unblockPollSocket();
        barrier::waitForFinalThreadShutdown(
            "IPC log buffer thread",
            barrier::kFinalThreadShutdownDeadlineSeconds,
            [this](double timeout) {
                return m_bufferThread->wait(timeout);
            });
        delete m_bufferThread;
    }

    ARCH->closeCondVar(m_notifyCond);
    ARCH->closeMutex(m_notifyMutex);
}

void
IpcLogOutputter::open(const char* title)
{
}

void
IpcLogOutputter::close()
{
    if (m_bufferThread != nullptr) {
        setRunning(false);
        notifyBuffer();
        m_bufferThread->wait(5);
    }
}

void
IpcLogOutputter::show(bool showIfEmpty)
{
}

bool
IpcLogOutputter::write(ELevel, const char* text)
{
    // ignore events from the buffer thread (would cause recursion).
    if (m_bufferThread != nullptr &&
        Thread::getCurrentThread().getID() == m_bufferThreadId) {
        return true;
    }

    appendBuffer(text);
    notifyBuffer();

    return true;
}

void IpcLogOutputter::appendBuffer(const std::string& text)
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);

    double elapsed = ARCH->time() - m_bufferRateStart;
    if (elapsed < m_bufferRateTimeLimit) {
        if (m_bufferWriteCount >= m_bufferRateWriteLimit) {
            // discard the log line if we've logged too much.
            return;
        }
    }
    else {
        m_bufferWriteCount = 0;
        m_bufferRateStart = ARCH->time();
    }

    if (m_bufferMaxSize == 0 || m_bufferMaxBytes == 0) {
        return;
    }

    std::string line = text;
    const std::string truncatedSuffix = "... [truncated]";
    if (m_bufferMaxLineBytes > 0 && line.size() > m_bufferMaxLineBytes) {
        if (m_bufferMaxLineBytes > truncatedSuffix.size()) {
            line.resize(m_bufferMaxLineBytes - truncatedSuffix.size());
            line.append(truncatedSuffix);
        }
        else {
            line.resize(m_bufferMaxLineBytes);
        }
    }

    if (line.size() > m_bufferMaxBytes) {
        if (m_bufferMaxBytes > truncatedSuffix.size()) {
            line.resize(m_bufferMaxBytes - truncatedSuffix.size());
            line.append(truncatedSuffix);
        }
        else {
            line.resize(m_bufferMaxBytes);
        }
    }

    while (m_buffer.size() >= m_bufferMaxSize ||
           (!m_buffer.empty() && m_bufferBytes + line.size() > m_bufferMaxBytes)) {
        popOldestBufferedLine();
    }

    m_buffer.push_back(line);
    m_bufferBytes += line.size();
    m_bufferWriteCount++;
}

bool
IpcLogOutputter::isRunning()
{
    std::lock_guard<std::mutex> lock(m_runningMutex);
    return m_running;
}

void
IpcLogOutputter::setRunning(bool running)
{
    std::lock_guard<std::mutex> lock(m_runningMutex);
    m_running = running;
}

bool
IpcLogOutputter::hasBufferedLines()
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    return !m_buffer.empty();
}

void IpcLogOutputter::buffer_thread()
{
    m_bufferThreadId = Thread::getCurrentThread().getID();
    setRunning(true);

    try {
        while (isRunning()) {
            {
                ArchMutexLock lock(m_notifyMutex);
                while (isRunning() &&
                       (!hasBufferedLines() || !m_ipcServer.hasClients(m_clientType))) {
                    ARCH->waitCondVar(m_notifyCond, m_notifyMutex, 1.0);
                }
            }

            sendBuffer();
        }
    }
    catch (XArch& e) {
        LOG((CLOG_ERR "ipc log buffer thread error, %s", e.what()));
    }

    LOG((CLOG_DEBUG "ipc log buffer thread finished"));
}

void
IpcLogOutputter::notifyBuffer()
{
    ArchMutexLock lock(m_notifyMutex);
    ARCH->broadcastCondVar(m_notifyCond);
}

std::string IpcLogOutputter::getChunk(size_t count)
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);

    if (m_buffer.size() < count) {
        count = m_buffer.size();
    }

    std::string chunk;
    for (size_t i = 0; i < count; i++) {
        chunk.append(m_buffer.front());
        chunk.append("\n");
        popOldestBufferedLine();
    }
    return chunk;
}

void
IpcLogOutputter::popOldestBufferedLine()
{
    if (m_buffer.empty()) {
        return;
    }

    m_bufferBytes -= m_buffer.front().size();
    m_buffer.pop_front();
}

void
IpcLogOutputter::sendBuffer()
{
    if (!hasBufferedLines() || !m_ipcServer.hasClients(m_clientType)) {
        return;
    }

    IpcLogLineMessage message(getChunk(kMaxSendLines));
    m_sending = true;
    m_ipcServer.send(message, kIpcClientGui);
    m_sending = false;
}

void
IpcLogOutputter::bufferMaxSize(UInt16 bufferMaxSize)
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    m_bufferMaxSize = bufferMaxSize;
    while (m_buffer.size() > m_bufferMaxSize) {
        popOldestBufferedLine();
    }
}

UInt16
IpcLogOutputter::bufferMaxSize() const
{
    return m_bufferMaxSize;
}

void
IpcLogOutputter::bufferMaxBytes(size_t bufferMaxBytes)
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    m_bufferMaxBytes = bufferMaxBytes;
    while (m_bufferBytes > m_bufferMaxBytes) {
        popOldestBufferedLine();
    }
}

size_t
IpcLogOutputter::bufferMaxBytes() const
{
    return m_bufferMaxBytes;
}

void
IpcLogOutputter::bufferMaxLineBytes(size_t bufferMaxLineBytes)
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    m_bufferMaxLineBytes = bufferMaxLineBytes;
}

size_t
IpcLogOutputter::bufferMaxLineBytes() const
{
    return m_bufferMaxLineBytes;
}

void
IpcLogOutputter::bufferRateLimit(UInt16 writeLimit, double timeLimit)
{
    m_bufferRateWriteLimit = writeLimit;
    m_bufferRateTimeLimit = timeLimit;
}
