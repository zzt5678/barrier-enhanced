/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2015-2016 Symless Ltd.
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

#include "barrier/FileChunk.h"

#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "io/IStream.h"
#include "io/filesystem.h"
#include "base/Stopwatch.h"
#include "base/Log.h"

#include <fstream>
#include <map>

static const UInt16 kIntervalThreshold = 1;
static const size_t kFileReceiveReserveLimit = 64 * 1024 * 1024;

const size_t FileChunk::kMaxReceiveSize = 512 * 1024 * 1024;
const size_t FileChunk::kMemoryReceiveLimit = 32 * 1024 * 1024;

namespace {

struct ReceiveState {
    ReceiveState() : inProgress(false), failed(false), receivedSize(0) { }
    bool inProgress;
    bool failed;
    size_t receivedSize;
};

typedef std::map<const void*, ReceiveState> ReceiveStateMap;

ReceiveState&
receiveStateFor(const String& dataReceived)
{
    static thread_local ReceiveStateMap states;
    return states[&dataReceived];
}

bool makeReceiveSpoolPath(barrier::fs::path& path)
{
    return barrier::create_secure_temp_file("weave-receive-", ".part", path);
}

bool appendToReceiveSpool(const barrier::fs::path& path, const String& content)
{
    std::ofstream output;
    barrier::open_utf8_path(output, path, std::ios::out | std::ios::binary | std::ios::app);
    if (!output.is_open()) {
        return false;
    }

    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    output.close();
    return !output.fail();
}

bool canAppendReceivedFileData(size_t currentSize, const String& content, size_t expectedSize)
{
    return currentSize <= expectedSize &&
        content.size() <= expectedSize - currentSize;
}

} // namespace

void
FileChunk::releaseReceiveBuffer(String& dataReceived,
                                size_t& expectedSize,
                                barrier::fs::path* spoolPath)
{
    ReceiveState& receiveState = receiveStateFor(dataReceived);
    receiveState.inProgress = false;
    receiveState.failed = false;
    receiveState.receivedSize = 0;
    String().swap(dataReceived);
    expectedSize = 0;
    if (spoolPath != NULL && !spoolPath->empty()) {
        barrier::fs::remove(*spoolPath);
        spoolPath->clear();
    }
}

FileChunk::FileChunk(size_t size) :
    Chunk(size),
    m_transferId(0)
{
        m_dataSize = size - FILE_CHUNK_META_SIZE;
}

FileChunk*
FileChunk::start(const String& size)
{
    size_t sizeLength = size.size();
    FileChunk* start = new FileChunk(sizeLength + FILE_CHUNK_META_SIZE);
    char* chunk = start->m_chunk;
    chunk[0] = kDataStart;
    memcpy(&chunk[1], size.c_str(), sizeLength);
    chunk[sizeLength + 1] = '\0';

    return start;
}

FileChunk*
FileChunk::data(const UInt8* data, size_t dataSize)
{
    FileChunk* chunk = new FileChunk(dataSize + FILE_CHUNK_META_SIZE);
    char* chunkData = chunk->m_chunk;
    chunkData[0] = kDataChunk;
    memcpy(&chunkData[1], data, dataSize);
    chunkData[dataSize + 1] = '\0';

    return chunk;
}

FileChunk*
FileChunk::end()
{
    FileChunk* end = new FileChunk(FILE_CHUNK_META_SIZE);
    char* chunk = end->m_chunk;
    chunk[0] = kDataEnd;
    chunk[1] = '\0';

    return end;
}

FileChunk*
FileChunk::cancel()
{
    FileChunk* cancel = new FileChunk(FILE_CHUNK_META_SIZE);
    char* chunk = cancel->m_chunk;
    chunk[0] = kDataCancel;
    chunk[1] = '\0';

    return cancel;
}

int
FileChunk::assemble(barrier::IStream* stream,
                    String& dataReceived,
                    size_t& expectedSize,
                    barrier::fs::path* spoolPath)
{
    // parse
    UInt8 mark = 0;
    String content;
    static thread_local size_t receivedDataSize = 0;
    static thread_local double elapsedTime = 0;
    static thread_local Stopwatch stopwatch;
    ReceiveState& receiveState = receiveStateFor(dataReceived);

    if (!ProtocolUtil::readf(stream, kMsgDFileTransfer + 4, &mark, &content)) {
        releaseReceiveBuffer(dataReceived, expectedSize, spoolPath);
        receiveState.inProgress = false;
        receiveState.failed = true;
        receiveState.receivedSize = 0;
        return kError;
    }

    switch (mark) {
    case kDataStart:
        releaseReceiveBuffer(dataReceived, expectedSize, spoolPath);
        receiveState.inProgress = true;
        receiveState.failed = false;
        receiveState.receivedSize = 0;
        expectedSize = barrier::string::stringToSizeType(content);
        if (expectedSize > kMaxReceiveSize) {
            LOG((CLOG_ERR "refusing file transfer larger than receive limit, expected size=%d limit=%d",
                expectedSize, kMaxReceiveSize));
            releaseReceiveBuffer(dataReceived, expectedSize, spoolPath);
            receiveState.inProgress = false;
            receiveState.failed = true;
            receiveState.receivedSize = 0;
            return kError;
        }
        if (spoolPath != NULL && expectedSize > kMemoryReceiveLimit) {
            if (!makeReceiveSpoolPath(*spoolPath)) {
                LOG((CLOG_ERR "failed to create receive spool file: %s",
                    spoolPath->u8string().c_str()));
                releaseReceiveBuffer(dataReceived, expectedSize, spoolPath);
                receiveState.inProgress = false;
                receiveState.failed = true;
                receiveState.receivedSize = 0;
                return kError;
            }
            LOG((CLOG_DEBUG "spooling received file payload to %s size=%d",
                spoolPath->u8string().c_str(), expectedSize));
        }
        else if (expectedSize <= kFileReceiveReserveLimit) {
            dataReceived.reserve(expectedSize);
        }
        receivedDataSize = 0;
        elapsedTime = 0;
        stopwatch.reset();

        if (CLOG->getFilter() >= kDEBUG2) {
            LOG((CLOG_DEBUG2 "recv file size=%s", content.c_str()));
            stopwatch.start();
        }
        return kStart;

    case kDataChunk:
        if (!receiveState.inProgress || receiveState.failed) {
            LOG((CLOG_WARN "ignoring file chunk without an active receive"));
            return kError;
        }
        if (expectedSize > kMaxReceiveSize) {
            LOG((CLOG_ERR "refusing file chunk because expected size exceeds receive limit, expected size=%d limit=%d",
                expectedSize, kMaxReceiveSize));
            releaseReceiveBuffer(dataReceived, expectedSize, spoolPath);
            receiveState.inProgress = false;
            receiveState.failed = true;
            receiveState.receivedSize = 0;
            return kError;
        }
        if (spoolPath != NULL && !spoolPath->empty()) {
            if (!canAppendReceivedFileData(receiveState.receivedSize, content, expectedSize)) {
                LOG((CLOG_ERR "corrupted file data, received chunk exceeds expected size=%d current size=%d chunk size=%d",
                    expectedSize, receiveState.receivedSize, content.size()));
                releaseReceiveBuffer(dataReceived, expectedSize, spoolPath);
                receiveState.inProgress = false;
                receiveState.failed = true;
                receiveState.receivedSize = 0;
                return kError;
            }
            if (!appendToReceiveSpool(*spoolPath, content)) {
                LOG((CLOG_ERR "failed to append received file spool: %s",
                    spoolPath->u8string().c_str()));
                releaseReceiveBuffer(dataReceived, expectedSize, spoolPath);
                receiveState.inProgress = false;
                receiveState.failed = true;
                receiveState.receivedSize = 0;
                return kError;
            }
        }
        else if (!canAppendReceivedFileData(receiveState.receivedSize, content, expectedSize)) {
            LOG((CLOG_ERR "corrupted file data, received chunk exceeds expected size=%d current size=%d chunk size=%d",
                expectedSize, receiveState.receivedSize, content.size()));
            releaseReceiveBuffer(dataReceived, expectedSize, spoolPath);
            receiveState.inProgress = false;
            receiveState.failed = true;
            receiveState.receivedSize = 0;
            return kError;
        }
        else {
            dataReceived.append(content);
        }
        receiveState.receivedSize += content.size();
        if (CLOG->getFilter() >= kDEBUG2) {
                LOG((CLOG_DEBUG2 "recv file chunk size=%i", content.size()));
                double interval = stopwatch.getTime();
                receivedDataSize += content.size();
                LOG((CLOG_DEBUG2 "recv file interval=%f s", interval));
                if (interval >= kIntervalThreshold) {
                    double averageSpeed = receivedDataSize / interval / 1000;
                    LOG((CLOG_DEBUG2 "recv file average speed=%f kb/s", averageSpeed));

                    receivedDataSize = 0;
                    elapsedTime += interval;
                    stopwatch.reset();
                }
            }
        return kNotFinish;

    case kDataEnd:
        if (!receiveState.inProgress || receiveState.failed) {
            LOG((CLOG_WARN "ignoring file transfer end without a valid receive"));
            releaseReceiveBuffer(dataReceived, expectedSize, spoolPath);
            receiveState.inProgress = false;
            receiveState.failed = true;
            receiveState.receivedSize = 0;
            return kError;
        }
        if (expectedSize != receiveState.receivedSize) {
            LOG((CLOG_ERR "corrupted file data, expected size=%d actual size=%d",
                expectedSize, receiveState.receivedSize));
            releaseReceiveBuffer(dataReceived, expectedSize, spoolPath);
            receiveState.inProgress = false;
            receiveState.failed = true;
            receiveState.receivedSize = 0;
            return kError;
        }

        if (CLOG->getFilter() >= kDEBUG2) {
            LOG((CLOG_DEBUG2 "file transfer finished"));
            elapsedTime += stopwatch.getTime();
            double averageSpeed = expectedSize / elapsedTime / 1000;
            LOG((CLOG_DEBUG2 "file transfer finished: total time consumed=%f s", elapsedTime));
            LOG((CLOG_DEBUG2 "file transfer finished: total data received=%i kb", expectedSize / 1000));
            LOG((CLOG_DEBUG2 "file transfer finished: total average speed=%f kb/s", averageSpeed));
        }
        receiveState.inProgress = false;
        receiveState.failed = false;
        receiveState.receivedSize = 0;
        return kFinish;

    case kDataCancel:
        LOG((CLOG_WARN "file transfer cancelled by sender"));
        releaseReceiveBuffer(dataReceived, expectedSize, spoolPath);
        receiveState.inProgress = false;
        receiveState.failed = true;
        receiveState.receivedSize = 0;
        return kError;
    }

    releaseReceiveBuffer(dataReceived, expectedSize, spoolPath);
    receiveState.inProgress = false;
    receiveState.failed = true;
    receiveState.receivedSize = 0;
    return kError;
}

void
FileChunk::send(barrier::IStream* stream, UInt8 mark, char* data, size_t dataSize)
{
    String chunk(data, dataSize);

    switch (mark) {
    case kDataStart:
        LOG((CLOG_DEBUG2 "sending file chunk start: size=%s", data));
        break;

    case kDataChunk:
        LOG((CLOG_DEBUG2 "sending file chunk: size=%i", chunk.size()));
        break;

    case kDataEnd:
        LOG((CLOG_DEBUG2 "sending file finished"));
        break;

    case kDataCancel:
        LOG((CLOG_DEBUG2 "sending file cancelled"));
        break;
    }

    ProtocolUtil::writefLowPriority(stream, kMsgDFileTransfer, mark, &chunk);
}
