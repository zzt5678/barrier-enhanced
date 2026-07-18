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
#include "barrier/FileTransferSendState.h"
#include "barrier/ClipboardChunk.h"
#include "barrier/protocol_types.h"
#include "barrier/TransferDigest.h"
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
#include <cstdint>
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

class ActiveFileSendStateGuard {
public:
    ActiveFileSendStateGuard(
        std::shared_ptr<barrier::FileTransferSendState>* active,
        const std::shared_ptr<barrier::FileTransferSendState>& state) :
        m_active(active),
        m_state(state)
    {
        if (m_state) {
            std::atomic_store(m_active, m_state);
        }
    }

    ~ActiveFileSendStateGuard()
    {
        if (!m_state) {
            return;
        }

        std::shared_ptr<barrier::FileTransferSendState> expected = m_state;
        std::atomic_compare_exchange_strong(
            m_active,
            &expected,
            std::shared_ptr<barrier::FileTransferSendState>());
    }

private:
    std::shared_ptr<barrier::FileTransferSendState>* m_active;
    std::shared_ptr<barrier::FileTransferSendState> m_state;
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
    const std::shared_ptr<barrier::ClipboardSendAttempt>& attempt,
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
    sizeMessage->setSendRoute(stream, bulkChannel, attempt);

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
        dataChunk->setSendRoute(stream, bulkChannel, attempt);

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
        cancel->setSendRoute(stream, bulkChannel, attempt);
        Event cancelEvent(events->forClipboard().clipboardSending(), eventTarget, cancel);
        cancelEvent.setDataObject(cancel);
        events->addEvent(cancelEvent);
        return false;
    }

    ClipboardChunk* end = ClipboardChunk::end(id, sequence);
    end->setSendRoute(stream, bulkChannel, attempt);
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
    sendFile(filename, events, eventTarget, stream, transferId,
             std::shared_ptr<barrier::FileTransferSendState>());
}

void
StreamChunker::sendFile(const char* filename,
                IEventQueue* events,
                void* eventTarget,
                barrier::IStream* stream,
                UInt32 transferId,
                const std::shared_ptr<barrier::FileTransferSendState>&
                    transactionState)
{
    FileMaintenanceEventGuard maintenanceEvent(events, eventTarget);
    ActiveFileSendStateGuard activeState(
        &m_activeFileSendState, transactionState);
    if (transactionState && transactionState->transferId() != transferId) {
        transactionState->fail(
            barrier::FileTransferReason::kProtocolError);
        throw std::invalid_argument(
            "transaction state transfer id does not match sendFile");
    }

    std::fstream file(filename, std::ios::in | std::ios::binary);

    if (!file.is_open()) {
        if (transactionState) {
            transactionState->fail(barrier::FileTransferReason::kIoError);
        }
        throw runtime_error("failed to open file");
    }
    FileInterruptResetGuard resetInterrupt(m_interruptFile);
    barrier::TransferDigest digest;
    if (!digest.isReady()) {
        if (transactionState) {
            transactionState->fail(barrier::FileTransferReason::kIoError);
        }
        throw runtime_error("failed to initialize file transfer digest");
    }

    // check file size
    file.seekg (0, std::ios::end);
    const std::streamoff measuredSize = file.tellg();
    if (measuredSize < 0) {
        if (transactionState) {
            transactionState->fail(barrier::FileTransferReason::kIoError);
        }
        throw runtime_error("failed to determine file size");
    }
    if (static_cast<std::uintmax_t>(measuredSize) > FileChunk::kMaxReceiveSize) {
        LOG((CLOG_WARN
            "refusing file transfer larger than receive limit, size=%llu limit=%llu",
            static_cast<unsigned long long>(measuredSize),
            static_cast<unsigned long long>(FileChunk::kMaxReceiveSize)));
        if (transactionState) {
            transactionState->fail(barrier::FileTransferReason::kSizeLimit);
        }
        return;
    }
    const size_t size = static_cast<size_t>(measuredSize);
    const auto shouldStop = [this, transactionState]() {
        return shouldInterrupt() ||
            (transactionState && transactionState->stopped());
    };

    const size_t maxQueuedBulkEvents = getMaxQueuedFileEvents(size, stream);
    if (!waitForQueuedEventBudget(events, maxQueuedBulkEvents, 0.001,
            kMaxFileQueuedEventWaits, shouldStop) ||
        !waitForBufferedOutputBudget(stream, kMaxFileBufferedOutputBytes, 0.001,
            kMaxFileOutputThrottleWaits, shouldStop)) {
        LOG((CLOG_DEBUG "file transmission not started because output is not ready"));
        if (transactionState && !transactionState->stopped()) {
            transactionState->fail(barrier::FileTransferReason::kTimeout);
        }
        file.close();
        return;
    }

    // send first message (file size)
    String fileSize = barrier::string::sizeTypeToString(size);
    if (transactionState &&
        !transactionState->markStartQueued(static_cast<UInt32>(size))) {
        return;
    }
    FileChunk* sizeMessage = FileChunk::start(
        fileSize, static_cast<bool>(transactionState));
    sizeMessage->m_transferId = transferId;

    Event sizeEvent(events->forFile().fileChunkSending(), eventTarget, sizeMessage);
    sizeEvent.setDataObject(sizeMessage);
    events->addEvent(sizeEvent);

    if (transactionState) {
        const barrier::FileTransferReason startResult =
            transactionState->waitForStartAck();
        if (startResult != barrier::FileTransferReason::kNone) {
            if (startResult == barrier::FileTransferReason::kTimeout ||
                startResult == barrier::FileTransferReason::kCancelled) {
                transactionState->markCancelQueued(startResult);
                FileChunk* cancel = FileChunk::cancel(startResult);
                cancel->m_transferId = transferId;
                Event cancelEvent(
                    events->forFile().fileChunkSending(), eventTarget, cancel);
                cancelEvent.setDataObject(cancel);
                events->addEvent(cancelEvent);
            }
            return;
        }
    }

    // send chunk messages with a fixed chunk size
    size_t sentLength = 0;
    size_t bytesSinceYield = 0;
    barrier::FileTransferReason failureReason =
        barrier::FileTransferReason::kNone;
    const size_t chunkSize = getChunkSize(size);
    std::vector<char> chunkBuffer(chunkSize);
    file.seekg (0, std::ios::beg);

    while (!transactionState || size != 0) {
        Thread::testCancel();
        if (shouldStop()) {
            LOG((CLOG_DEBUG "file transmission interrupted"));
            failureReason = transactionState &&
                    transactionState->result() !=
                        barrier::FileTransferReason::kNone ?
                transactionState->result() :
                barrier::FileTransferReason::kCancelled;
            break;
        }

        events->addEvent(Event(events->forFile().keepAlive(), eventTarget));
        if (!waitForQueuedEventBudget(events, maxQueuedBulkEvents, 0.001,
                kMaxFileQueuedEventWaits, shouldStop)) {
            failureReason = transactionState &&
                    transactionState->result() !=
                        barrier::FileTransferReason::kNone ?
                transactionState->result() :
                barrier::FileTransferReason::kTimeout;
            break;
        }
        if (!waitForBufferedOutputBudget(stream, kMaxFileBufferedOutputBytes, 0.001,
                kMaxFileOutputThrottleWaits, shouldStop)) {
            failureReason = transactionState &&
                    transactionState->result() !=
                        barrier::FileTransferReason::kNone ?
                transactionState->result() :
                barrier::FileTransferReason::kTimeout;
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
            if (transactionState) {
                failureReason = barrier::FileTransferReason::kIoError;
                break;
            }
            throw runtime_error("failed to read file");
        }
        if (!digest.update(chunkBuffer.data(), bytesToRead)) {
            LOG((CLOG_ERR "failed to update file transfer digest"));
            failureReason = barrier::FileTransferReason::kIoError;
            break;
        }
        if (transactionState && !transactionState->markDataQueued(
                static_cast<UInt32>(sentLength),
                static_cast<UInt32>(bytesToRead))) {
            failureReason = transactionState->result() !=
                    barrier::FileTransferReason::kNone ?
                transactionState->result() :
                barrier::FileTransferReason::kProtocolError;
            break;
        }
        FileChunk* fileChunk = transactionState ?
            FileChunk::data(
                reinterpret_cast<const UInt8*>(chunkBuffer.data()),
                bytesToRead,
                static_cast<UInt32>(sentLength)) :
            FileChunk::data(
                reinterpret_cast<const UInt8*>(chunkBuffer.data()),
                bytesToRead);
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

    String encodedDigest;
    if (sentLength == size && !digest.finish(encodedDigest)) {
        LOG((CLOG_ERR "failed to finalize file transfer digest"));
        failureReason = barrier::FileTransferReason::kIoError;
        sentLength = 0;
    }

    if (sentLength != size) {
        LOG((CLOG_DEBUG "file transmission stopped before completion, sent=%d expected=%d",
            sentLength, size));
        if (transactionState &&
            failureReason == barrier::FileTransferReason::kNone) {
            failureReason = barrier::FileTransferReason::kCancelled;
        }
        if (transactionState &&
            failureReason == barrier::FileTransferReason::kConnectionLost) {
            return;
        }
        if (transactionState) {
            transactionState->markCancelQueued(failureReason);
        }
        FileChunk* cancel = transactionState ?
            FileChunk::cancel(failureReason) : FileChunk::cancel();
        cancel->m_transferId = transferId;
        Event cancelEvent(events->forFile().fileChunkSending(), eventTarget, cancel);
        cancelEvent.setDataObject(cancel);
        events->addEvent(cancelEvent);
        file.close();
        return;
    }

    // send last message
    if (transactionState && !transactionState->markEndQueued(
            static_cast<UInt32>(sentLength))) {
        const barrier::FileTransferReason reason =
            transactionState->result() != barrier::FileTransferReason::kNone ?
                transactionState->result() :
                barrier::FileTransferReason::kProtocolError;
        if (reason != barrier::FileTransferReason::kConnectionLost) {
            transactionState->markCancelQueued(reason);
            FileChunk* cancel = FileChunk::cancel(reason);
            cancel->m_transferId = transferId;
            Event cancelEvent(
                events->forFile().fileChunkSending(), eventTarget, cancel);
            cancelEvent.setDataObject(cancel);
            events->addEvent(cancelEvent);
        }
        return;
    }
    FileChunk* end = transactionState ?
        FileChunk::end(encodedDigest, static_cast<UInt32>(sentLength)) :
        FileChunk::end(encodedDigest);
    end->m_transferId = transferId;
    Event endEvent(events->forFile().fileChunkSending(), eventTarget, end);
    endEvent.setDataObject(end);
    events->addEvent(endEvent);

    file.close();
    if (transactionState) {
        transactionState->waitForCommitAck();
    }
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
                const std::shared_ptr<barrier::BulkChannel>& bulkChannel,
                const std::shared_ptr<barrier::ClipboardSendAttempt>& attempt)
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
        attempt,
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
                const std::shared_ptr<barrier::BulkChannel>& bulkChannel,
                const std::shared_ptr<barrier::ClipboardSendAttempt>& attempt)
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
        attempt,
        0,
        false,
        std::function<bool()>());
}

void
StreamChunker::interruptFile()
{
    m_interruptFile.store(true);
    const std::shared_ptr<barrier::FileTransferSendState> activeState =
        std::atomic_load(&m_activeFileSendState);
    if (activeState) {
        activeState->interrupt();
    }
    LOG((CLOG_INFO "previous dragged file has become invalid"));
}

bool
StreamChunker::shouldInterrupt() const
{
    return m_interruptFile.load();
}
