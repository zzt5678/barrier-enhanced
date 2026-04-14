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

#include <fstream>
#include <stdexcept>
#include <vector>

using namespace std;

namespace {

size_t getChunkSize(size_t totalSize)
{
    if (totalSize >= 256 * 1024 * 1024) {
        return 256 * 1024;
    }
    if (totalSize >= 32 * 1024 * 1024) {
        return 128 * 1024;
    }
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

void cooperativeYield(size_t& bytesSinceYield, size_t threshold)
{
    if (bytesSinceYield >= threshold) {
        bytesSinceYield = 0;
        ARCH->sleep(0.0);
    }
}

void waitForBufferedOutputBudget(barrier::IStream* stream,
                                 UInt32 maxBufferedBytes,
                                 double sleepSeconds)
{
    if (stream == nullptr) {
        return;
    }

    UInt32 waitCount = 0;
    while (stream->getBufferedOutputSize() > maxBufferedBytes) {
        ++waitCount;
        ARCH->sleep(sleepSeconds);
    }

    if (waitCount > 0) {
        LOG((CLOG_DEBUG2 "bulk output throttled: waits=%u maxBufferedBudget=%u slept=%.3fs",
            waitCount, maxBufferedBytes, waitCount * sleepSeconds));
    }
}

void queueClipboardChunks(
                String data,
                size_t size,
                ClipboardID id,
                UInt32 sequence,
                IEventQueue* events,
                void* eventTarget,
                barrier::IStream* stream)
{
    String dataSize = barrier::string::sizeTypeToString(size);
    ClipboardChunk* sizeMessage = ClipboardChunk::start(id, sequence, dataSize);

    events->addEvent(Event(events->forClipboard().clipboardSending(), eventTarget, sizeMessage));

    size_t sentLength = 0;
    size_t bytesSinceYield = 0;
    const size_t chunkSize = getClipboardChunkSize(size);

    while (true) {
        events->addEvent(Event(events->forFile().keepAlive(), eventTarget));
        waitForBufferedOutputBudget(stream, 128 * 1024, 0.001);

        size_t bytesToSend = chunkSize;
        if (sentLength + bytesToSend > size) {
            bytesToSend = size - sentLength;
        }

        String chunk(data.data() + sentLength, bytesToSend);
        ClipboardChunk* dataChunk = ClipboardChunk::data(id, sequence, chunk);

        events->addEvent(Event(events->forClipboard().clipboardSending(), eventTarget, dataChunk));

        sentLength += bytesToSend;
        bytesSinceYield += bytesToSend;
        cooperativeYield(bytesSinceYield, 64 * 1024);
        if (sentLength == size) {
            break;
        }
    }

    ClipboardChunk* end = ClipboardChunk::end(id, sequence);
    events->addEvent(Event(events->forClipboard().clipboardSending(), eventTarget, end));

    LOG((CLOG_DEBUG "sent clipboard size=%d", sentLength));
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
                barrier::IStream* stream)
{
    m_interruptFile.store(false);

    std::fstream file(filename, std::ios::in | std::ios::binary);

    if (!file.is_open()) {
        throw runtime_error("failed to open file");
    }

    // check file size
    file.seekg (0, std::ios::end);
    size_t size = (size_t)file.tellg();

    // send first message (file size)
    String fileSize = barrier::string::sizeTypeToString(size);
    FileChunk* sizeMessage = FileChunk::start(fileSize);

    events->addEvent(Event(events->forFile().fileChunkSending(), eventTarget, sizeMessage));

    // send chunk messages with a fixed chunk size
    size_t sentLength = 0;
    size_t bytesSinceYield = 0;
    const size_t chunkSize = getChunkSize(size);
    std::vector<char> chunkBuffer(chunkSize);
    file.seekg (0, std::ios::beg);

    while (true) {
        if (shouldInterrupt()) {
            LOG((CLOG_DEBUG "file transmission interrupted"));
            break;
        }

        events->addEvent(Event(events->forFile().keepAlive(), eventTarget));
        waitForBufferedOutputBudget(stream, 512 * 1024, 0.001);

        // make sure we don't read too much from the mock data.
        size_t bytesToRead = chunkSize;
        if (sentLength + bytesToRead > size) {
            bytesToRead = size - sentLength;
        }

        file.read(chunkBuffer.data(), bytesToRead);
        if (!file) {
            throw runtime_error("failed to read file");
        }
        FileChunk* fileChunk = FileChunk::data(
            reinterpret_cast<const UInt8*>(chunkBuffer.data()), bytesToRead);

        events->addEvent(Event(events->forFile().fileChunkSending(), eventTarget, fileChunk));

        sentLength += bytesToRead;
        bytesSinceYield += bytesToRead;
        cooperativeYield(bytesSinceYield, 256 * 1024);

        if (sentLength == size) {
            break;
        }
    }

    // send last message
    FileChunk* end = FileChunk::end();

    events->addEvent(Event(events->forFile().fileChunkSending(), eventTarget, end));

    file.close();
}

void
StreamChunker::sendClipboard(
                String& data,
                size_t size,
                ClipboardID id,
                UInt32 sequence,
                IEventQueue* events,
                void* eventTarget,
                barrier::IStream* stream)
{
    queueClipboardChunks(data, size, id, sequence, events, eventTarget, stream);
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
