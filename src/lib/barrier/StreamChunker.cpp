/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2013-2016 Symless Ltd.
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

#include "barrier/StreamChunker.h"

#include "barrier/FileChunk.h"
#include "barrier/ClipboardChunk.h"
#include "barrier/protocol_types.h"
#include "base/EventTypes.h"
#include "base/Event.h"
#include "base/IEventQueue.h"
#include "base/EventTypes.h"
#include "base/Log.h"
#include "base/Stopwatch.h"
#include "base/String.h"
#include "arch/Arch.h"
#include "io/IStream.h"
#include "mt/Thread.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <vector>

using namespace std;

namespace {

const UInt32 kMaxFileOutputThrottleWaits = 120000;
const UInt32 kMaxFileQueuedEventWaits = 5000;
const UInt32 kMaxClipboardOutputThrottleWaits = 5000;
const UInt32 kMaxClipboardQueuedEventWaits = 5000;
const UInt32 kMaxQueuedFilePayloadBytes = 4 * 1024 * 1024;
const UInt32 kMaxFileBufferedOutputBytes = 256 * 1024;

class FileInterruptResetGuard {
public:
    explicit FileInterruptResetGuard(std::atomic<bool>& interrupted) :
        m_interrupted(interrupted)
    {
    }

    ~FileInterruptResetGuard()
    {
        m_interrupted.store(false);
    }

private:
    std::atomic<bool>& m_interrupted;
};

class FileMaintenanceEventGuard {
public:
    FileMaintenanceEventGuard(IEventQueue* events, void* eventTarget) :
        m_events(events),
        m_eventTarget(eventTarget)
    {
    }

    ~FileMaintenanceEventGuard()
    {
        if (m_events != nullptr) {
            m_events->addEvent(Event(m_events->forFile().keepAlive(), m_eventTarget));
        }
    }

private:
    IEventQueue* m_events;
    void* m_eventTarget;
};

size_t getChunkSize(size_t totalSize)
{
    if (totalSize >= 4 * 1024 * 1024) {
        return 64 * 1024;
    }
    return 32 * 1024;
}

size_t getClipboardChunkSize(size_t totalSize)
{
    if (totalSize >= 16 * 1024 * 1024) {
        return 32 * 1024;
    }
    if (totalSize >= 1 * 1024 * 1024) {
        return 16 * 1024;
    }
    return 8 * 1024;
}

size_t getMaxQueuedFileEvents(size_t fileSize, barrier::IStream* stream)
{
    if (stream == nullptr) {
        return 0;
    }

    const size_t chunkSize = getChunkSize(fileSize);
    return std::max<size_t>(1, kMaxQueuedFilePayloadBytes / chunkSize);
}

void cooperativeYield(size_t& bytesSinceYield, size_t threshold)
{
    if (bytesSinceYield >= threshold) {
        bytesSinceYield = 0;
        ARCH->sleep(0.0);
    }
}

bool waitForBufferedOutputBudget(barrier::IStream* stream,
                                  UInt32 maxBufferedBytes,
                                  double sleepSeconds,
                                  UInt32 maxWaitCount,
                                  const std::function<bool()>& shouldInterrupt)
{
    if (stream == nullptr) {
        return true;
    }

    UInt32 waitCount = 0;
    UInt32 bufferedBytes = stream->getBufferedOutputSize();
    while (bufferedBytes > maxBufferedBytes) {
        if (shouldInterrupt && shouldInterrupt()) {
            LOG((CLOG_DEBUG "bulk output throttling interrupted"));
            return false;
        }
        if (maxWaitCount != 0 && waitCount >= maxWaitCount) {
            LOG((CLOG_WARN "bulk output throttling timed out: waits=%u maxBufferedBudget=%u",
                waitCount, maxBufferedBytes));
            return false;
        }
        ARCH->sleep(sleepSeconds);
        const UInt32 nextBufferedBytes = stream->getBufferedOutputSize();
        if (nextBufferedBytes < bufferedBytes) {
            waitCount = 0;
        }
        else {
            ++waitCount;
        }
        bufferedBytes = nextBufferedBytes;
    }

    if (waitCount > 0) {
        LOG((CLOG_DEBUG2 "bulk output throttled: waits=%u maxBufferedBudget=%u slept=%.3fs",
            waitCount, maxBufferedBytes, waitCount * sleepSeconds));
    }

    return true;
}

bool hasBufferedOutputBudget(barrier::IStream* stream, UInt32 maxBufferedBytes)
{
    return stream == nullptr || stream->getBufferedOutputSize() <= maxBufferedBytes;
}

bool waitForQueuedEventBudget(IEventQueue* events,
                              size_t maxQueuedEvents,
                              double sleepSeconds,
                              UInt32 maxWaitCount,
                              const std::function<bool()>& shouldInterrupt)
{
    if (events == nullptr || maxQueuedEvents == 0) {
        return true;
    }

    UInt32 waitCount = 0;
    while (events->getQueuedEventCount() >= maxQueuedEvents) {
        if (shouldInterrupt && shouldInterrupt()) {
            LOG((CLOG_DEBUG "bulk event queue throttling interrupted"));
            return false;
        }
        if (maxWaitCount != 0 && waitCount >= maxWaitCount) {
            LOG((CLOG_WARN "bulk event queue throttling timed out: waits=%u maxQueuedEvents=%lu",
                waitCount,
                static_cast<unsigned long>(maxQueuedEvents)));
            return false;
        }
        ++waitCount;
        ARCH->sleep(sleepSeconds);
    }

    if (waitCount > 0) {
        LOG((CLOG_DEBUG2 "bulk event queue throttled: waits=%u maxQueuedEvents=%lu slept=%.3fs",
            waitCount,
            static_cast<unsigned long>(maxQueuedEvents),
            waitCount * sleepSeconds));
    }

    return true;
}

bool queueClipboardChunks(
                const String& data,
                size_t size,
                ClipboardID id,
                UInt32 sequence,
                IEventQueue* events,
                void* eventTarget,
                barrier::IStream* stream,
                const std::shared_ptr<barrier::BulkChannel>& bulkChannel,
                size_t maxQueuedBulkEvents,
                bool waitForBudgets,
                const std::function<bool()>& shouldInterrupt)
{
    if (size > ClipboardChunk::kMaxReceiveSize) {
        LOG((CLOG_WARN "refusing clipboard transfer larger than receive limit, size=%d limit=%d",
            size, ClipboardChunk::kMaxReceiveSize));
        return false;
    }

    if (waitForBudgets) {
        if (!waitForQueuedEventBudget(events, maxQueuedBulkEvents, 0.001,
                kMaxClipboardQueuedEventWaits, shouldInterrupt) ||
            !waitForBufferedOutputBudget(stream, 128 * 1024, 0.001,
                kMaxClipboardOutputThrottleWaits, shouldInterrupt)) {
            LOG((CLOG_DEBUG "clipboard transmission not started because output is not ready"));
            return false;
        }
    }
    else if (!hasBufferedOutputBudget(stream, 128 * 1024)) {
        return false;
    }

    String dataSize = barrier::string::sizeTypeToString(size);
    ClipboardChunk* sizeMessage = ClipboardChunk::start(id, sequence, dataSize);
    sizeMessage->setSendRoute(stream, bulkChannel);

    Event sizeEvent(events->forClipboard().clipboardSending(), eventTarget, sizeMessage);
    sizeEvent.setDataObject(sizeMessage);
    events->addEvent(sizeEvent);

    size_t sentLength = 0;
    size_t bytesSinceYield = 0;
    const size_t chunkSize = getClipboardChunkSize(size);
    bool completed = false;

    while (true) {
        Thread::testCancel();
        if (shouldInterrupt && shouldInterrupt()) {
            LOG((CLOG_DEBUG "clipboard transmission interrupted"));
            break;
        }

        if (waitForBudgets) {
            if (!waitForQueuedEventBudget(events, maxQueuedBulkEvents, 0.001,
                    kMaxClipboardQueuedEventWaits, shouldInterrupt) ||
                !waitForBufferedOutputBudget(stream, 128 * 1024, 0.001,
                    kMaxClipboardOutputThrottleWaits, shouldInterrupt)) {
                break;
            }
        }
        else if (!hasBufferedOutputBudget(stream, 128 * 1024)) {
            break;
        }

        events->addEvent(Event(events->forFile().keepAlive(), eventTarget));
        if (waitForBudgets &&
            !waitForQueuedEventBudget(events, maxQueuedBulkEvents, 0.001,
                kMaxClipboardQueuedEventWaits, shouldInterrupt)) {
            break;
        }

        size_t bytesToSend = chunkSize;
        if (sentLength + bytesToSend > size) {
            bytesToSend = size - sentLength;
        }

        String chunk(data.data() + sentLength, bytesToSend);
        ClipboardChunk* dataChunk = ClipboardChunk::data(id, sequence, chunk);
        dataChunk->setSendRoute(stream, bulkChannel);

        Event dataEvent(events->forClipboard().clipboardSending(), eventTarget, dataChunk);
        dataEvent.setDataObject(dataChunk);
        events->addEvent(dataEvent);

        sentLength += bytesToSend;
        bytesSinceYield += bytesToSend;
        cooperativeYield(bytesSinceYield, 64 * 1024);
        if (sentLength == size) {
            completed = true;
            break;
        }
    }

    if (!completed) {
        LOG((CLOG_WARN "clipboard transmission stopped before completion, sent=%d expected=%d",
            sentLength, size));
        ClipboardChunk* cancel = ClipboardChunk::cancel(id, sequence);
        cancel->setSendRoute(stream, bulkChannel);
        Event cancelEvent(events->forClipboard().clipboardSending(), eventTarget, cancel);
        cancelEvent.setDataObject(cancel);
        events->addEvent(cancelEvent);
        return false;
    }

    ClipboardChunk* end = ClipboardChunk::end(id, sequence);
    end->setSendRoute(stream, bulkChannel);
    Event endEvent(events->forClipboard().clipboardSending(), eventTarget, end);
    endEvent.setDataObject(end);
    events->addEvent(endEvent);

    LOG((CLOG_DEBUG "sent clipboard size=%d", sentLength));
    return true;
}

}

StreamChunker::StreamChunker() :
    m_interruptFile(false)
{
}

void
StreamChunker::sendFile(const char* filename,
                IEventQueue* events,
                void* eventTarget,
                barrier::IStream* stream,
                UInt32 transferId)
{
    FileMaintenanceEventGuard maintenanceEvent(events, eventTarget);
    std::fstream file(filename, std::ios::in | std::ios::binary);

    if (!file.is_open()) {
        throw runtime_error("failed to open file");
    }
    FileInterruptResetGuard resetInterrupt(m_interruptFile);

    // check file size
    file.seekg (0, std::ios::end);
    size_t size = (size_t)file.tellg();

    const size_t maxQueuedBulkEvents = getMaxQueuedFileEvents(size, stream);
    if (!waitForQueuedEventBudget(events, maxQueuedBulkEvents, 0.001,
            kMaxFileQueuedEventWaits,
            [this]() { return shouldInterrupt(); }) ||
        !waitForBufferedOutputBudget(stream, kMaxFileBufferedOutputBytes, 0.001,
            kMaxFileOutputThrottleWaits,
            [this]() { return shouldInterrupt(); })) {
        LOG((CLOG_DEBUG "file transmission not started because output is not ready"));
        file.close();
        return;
    }

    // send first message (file size)
    String fileSize = barrier::string::sizeTypeToString(size);
    FileChunk* sizeMessage = FileChunk::start(fileSize);
    sizeMessage->m_transferId = transferId;

    Event sizeEvent(events->forFile().fileChunkSending(), eventTarget, sizeMessage);
    sizeEvent.setDataObject(sizeMessage);
    events->addEvent(sizeEvent);

    // send chunk messages with a fixed chunk size
    size_t sentLength = 0;
    size_t bytesSinceYield = 0;
    const size_t chunkSize = getChunkSize(size);
    std::vector<char> chunkBuffer(chunkSize);
    file.seekg (0, std::ios::beg);

    while (true) {
        Thread::testCancel();
        if (shouldInterrupt()) {
            LOG((CLOG_DEBUG "file transmission interrupted"));
            break;
        }

        events->addEvent(Event(events->forFile().keepAlive(), eventTarget));
        if (!waitForQueuedEventBudget(events, maxQueuedBulkEvents, 0.001,
                kMaxFileQueuedEventWaits,
                [this]() { return shouldInterrupt(); })) {
            break;
        }
        if (!waitForBufferedOutputBudget(stream, kMaxFileBufferedOutputBytes, 0.001,
                kMaxFileOutputThrottleWaits,
                [this]() { return shouldInterrupt(); })) {
            break;
        }

        // make sure we don't read too much from the mock data.
        size_t bytesToRead = chunkSize;
        if (sentLength + bytesToRead > size) {
            bytesToRead = size - sentLength;
        }

        file.read(chunkBuffer.data(), bytesToRead);
        Thread::testCancel();
        if (!file) {
            throw runtime_error("failed to read file");
        }
        FileChunk* fileChunk = FileChunk::data(
            reinterpret_cast<const UInt8*>(chunkBuffer.data()), bytesToRead);
        fileChunk->m_transferId = transferId;

        Event dataEvent(events->forFile().fileChunkSending(), eventTarget, fileChunk);
        dataEvent.setDataObject(fileChunk);
        events->addEvent(dataEvent);

        sentLength += bytesToRead;
        bytesSinceYield += bytesToRead;
        cooperativeYield(bytesSinceYield, 256 * 1024);

        if (sentLength == size) {
            break;
        }
    }

    if (sentLength != size) {
        LOG((CLOG_DEBUG "file transmission stopped before completion, sent=%d expected=%d",
            sentLength, size));
        FileChunk* cancel = FileChunk::cancel();
        cancel->m_transferId = transferId;
        Event cancelEvent(events->forFile().fileChunkSending(), eventTarget, cancel);
        cancelEvent.setDataObject(cancel);
        events->addEvent(cancelEvent);
        file.close();
        return;
    }

    // send last message
    FileChunk* end = FileChunk::end();
    end->m_transferId = transferId;
    Event endEvent(events->forFile().fileChunkSending(), eventTarget, end);
    endEvent.setDataObject(end);
    events->addEvent(endEvent);

    file.close();
}

bool
StreamChunker::sendClipboardData(
                const String& data,
                size_t size,
                ClipboardID id,
                UInt32 sequence,
                IEventQueue* events,
                void* eventTarget,
                barrier::IStream* stream,
                const std::shared_ptr<barrier::BulkChannel>& bulkChannel)
{
    const size_t maxQueuedBulkEvents = stream == nullptr ? 0 : 32;
    FileInterruptResetGuard resetInterrupt(m_interruptFile);
    return queueClipboardChunks(
        data,
        size,
        id,
        sequence,
        events,
        eventTarget,
        stream,
        bulkChannel,
        maxQueuedBulkEvents,
        true,
        [this]() { return shouldInterrupt(); });
}

bool
StreamChunker::sendClipboard(
                const String& data,
                size_t size,
                ClipboardID id,
                UInt32 sequence,
                IEventQueue* events,
                void* eventTarget,
                barrier::IStream* stream,
                const std::shared_ptr<barrier::BulkChannel>& bulkChannel)
{
    return queueClipboardChunks(
        data,
        size,
        id,
        sequence,
        events,
        eventTarget,
        stream,
        bulkChannel,
        0,
        false,
        std::function<bool()>());
}

void
StreamChunker::interruptFile()
{
    m_interruptFile.store(true);
    LOG((CLOG_INFO "previous dragged file has become invalid"));
}

bool
StreamChunker::shouldInterrupt() const
{
    return m_interruptFile.load();
}
