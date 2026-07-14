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
#include "base/Log.h"

static const size_t kFileReceiveReserveLimit = 64 * 1024 * 1024;

const size_t FileChunk::kMaxReceiveSize = 512 * 1024 * 1024;
const size_t FileChunk::kMemoryReceiveLimit = 32 * 1024 * 1024;

void
FileChunk::releaseReceiveBuffer(FileReceiveSession& session)
{
    session.reset();
}

void
FileChunk::releaseReceiveBuffer(String& dataReceived,
                                size_t& expectedSize,
                                barrier::fs::path* spoolPath)
{
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
                    FileReceiveSession& session)
{
    // parse
    UInt8 mark = 0;
    String content;

    if (!ProtocolUtil::readf(stream, kMsgDFileTransfer + 4, &mark, &content)) {
        session.fail();
        return kError;
    }

    switch (mark) {
    case kDataStart:
    {
        const size_t expectedSize = barrier::string::stringToSizeType(content);
        if (expectedSize > kMaxReceiveSize) {
            LOG((CLOG_ERR "refusing file transfer larger than receive limit, expected size=%llu limit=%llu",
                static_cast<unsigned long long>(expectedSize),
                static_cast<unsigned long long>(kMaxReceiveSize)));
            session.fail();
            return kError;
        }
        if (!session.begin(expectedSize,
                           kMemoryReceiveLimit,
                           kFileReceiveReserveLimit)) {
            LOG((CLOG_ERR "failed to initialize file receive session, expected size=%llu",
                static_cast<unsigned long long>(expectedSize)));
            session.fail();
            return kError;
        }
        if (!session.spoolPath().empty()) {
            LOG((CLOG_DEBUG "spooling received file payload to %s size=%llu",
                session.spoolPath().u8string().c_str(),
                static_cast<unsigned long long>(expectedSize)));
        }
        LOG((CLOG_DEBUG2 "recv file size=%s", content.c_str()));
        return kStart;
    }

    case kDataChunk:
    {
        const size_t contentSize = content.size();
        if (session.state() != FileReceiveSession::kReceiving) {
            LOG((CLOG_WARN "ignoring file chunk without an active receive"));
            return kError;
        }
        if (!session.append(std::move(content))) {
            LOG((CLOG_ERR "failed to append file data, expected size=%llu current size=%llu chunk size=%llu",
                static_cast<unsigned long long>(session.expectedSize()),
                static_cast<unsigned long long>(session.receivedSize()),
                static_cast<unsigned long long>(contentSize)));
            session.fail();
            return kError;
        }
        LOG((CLOG_DEBUG2 "recv file chunk size=%llu",
            static_cast<unsigned long long>(contentSize)));
        return kNotFinish;
    }

    case kDataEnd:
        if (session.state() != FileReceiveSession::kReceiving) {
            LOG((CLOG_WARN "ignoring file transfer end without a valid receive"));
            session.fail();
            return kError;
        }
        if (!session.finish()) {
            LOG((CLOG_ERR "corrupted file data, expected size=%llu actual size=%llu",
                static_cast<unsigned long long>(session.expectedSize()),
                static_cast<unsigned long long>(session.receivedSize())));
            session.fail();
            return kError;
        }
        LOG((CLOG_DEBUG2 "file transfer finished: total data received=%llu kb",
            static_cast<unsigned long long>(session.expectedSize() / 1000)));
        return kFinish;

    case kDataCancel:
        LOG((CLOG_WARN "file transfer cancelled by sender"));
        session.fail();
        return kError;
    }

    session.fail();
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
