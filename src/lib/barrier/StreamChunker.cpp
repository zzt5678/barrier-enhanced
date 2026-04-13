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

#include "mt/Lock.h"
#include "mt/Mutex.h"
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

}

bool StreamChunker::s_isChunkingFile = false;
bool StreamChunker::s_interruptFile = false;
Mutex* StreamChunker::s_interruptMutex = NULL;

void
StreamChunker::sendFile(const char* filename,
                IEventQueue* events,
                void* eventTarget)
{
    s_isChunkingFile = true;

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
    const size_t chunkSize = getChunkSize(size);
    std::vector<char> chunkBuffer(chunkSize);
    file.seekg (0, std::ios::beg);

    while (true) {
        if (s_interruptFile) {
            s_interruptFile = false;
            LOG((CLOG_DEBUG "file transmission interrupted"));
            break;
        }

        events->addEvent(Event(events->forFile().keepAlive(), eventTarget));

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

        if (sentLength == size) {
            break;
        }
    }

    // send last message
    FileChunk* end = FileChunk::end();

    events->addEvent(Event(events->forFile().fileChunkSending(), eventTarget, end));

    file.close();

    s_isChunkingFile = false;
}

void
StreamChunker::sendClipboard(
                String& data,
                size_t size,
                ClipboardID id,
                UInt32 sequence,
                IEventQueue* events,
                void* eventTarget)
{
    // send first message (data size)
    String dataSize = barrier::string::sizeTypeToString(size);
    ClipboardChunk* sizeMessage = ClipboardChunk::start(id, sequence, dataSize);

    events->addEvent(Event(events->forClipboard().clipboardSending(), eventTarget, sizeMessage));

    // send clipboard chunk with a fixed size
    size_t sentLength = 0;
    const size_t chunkSize = 32 * 1024;

    while (true) {
        events->addEvent(Event(events->forFile().keepAlive(), eventTarget));

        // make sure we don't read too much from the mock data.
        size_t bytesToSend = chunkSize;
        if (sentLength + bytesToSend > size) {
            bytesToSend = size - sentLength;
        }

        String chunk(data.substr(sentLength, bytesToSend).c_str(), bytesToSend);
        ClipboardChunk* dataChunk = ClipboardChunk::data(id, sequence, chunk);

        events->addEvent(Event(events->forClipboard().clipboardSending(), eventTarget, dataChunk));

        sentLength += bytesToSend;
        if (sentLength == size) {
            break;
        }
    }

    // send last message
    ClipboardChunk* end = ClipboardChunk::end(id, sequence);

    events->addEvent(Event(events->forClipboard().clipboardSending(), eventTarget, end));

    LOG((CLOG_DEBUG "sent clipboard size=%d", sentLength));
}

void
StreamChunker::interruptFile()
{
    if (s_isChunkingFile) {
        s_interruptFile = true;
        LOG((CLOG_INFO "previous dragged file has become invalid"));
    }
}
