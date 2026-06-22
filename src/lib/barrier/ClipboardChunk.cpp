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

#include "barrier/ClipboardChunk.h"

#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "io/IStream.h"
#include "base/Log.h"
#include <cstring>

static const size_t kClipboardReceiveReserveLimit = 16 * 1024 * 1024;

const size_t ClipboardChunk::kMaxReceiveSize = 64 * 1024 * 1024;

static bool
canAppendClipboardData(
        const ClipboardChunk::ReceiveBuffer& buffer,
        const String& data)
{
    return buffer.data.size() <= buffer.expectedSize &&
        data.size() <= buffer.expectedSize - buffer.data.size();
}

void
ClipboardChunk::ReceiveBuffer::clear()
{
    data.clear();
    expectedSize = 0;
    inProgress = false;
}

void
ClipboardChunk::ReceiveBuffer::release()
{
    String().swap(data);
    expectedSize = 0;
    inProgress = false;
}

ClipboardChunk::ClipboardChunk(size_t size) :
    Chunk(size)
{
        m_dataSize = size - CLIPBOARD_CHUNK_META_SIZE;
}

ClipboardChunk*
ClipboardChunk::start(
                    ClipboardID id,
                    UInt32 sequence,
                    const String& size)
{
    size_t sizeLength = size.size();
    ClipboardChunk* start = new ClipboardChunk(sizeLength + CLIPBOARD_CHUNK_META_SIZE);
    char* chunk = start->m_chunk;

    chunk[0] = id;
    std::memcpy (&chunk[1], &sequence, 4);
    chunk[5] = kDataStart;
    memcpy(&chunk[6], size.c_str(), sizeLength);
    chunk[sizeLength + CLIPBOARD_CHUNK_META_SIZE - 1] = '\0';

    return start;
}

ClipboardChunk*
ClipboardChunk::data(
                    ClipboardID id,
                    UInt32 sequence,
                    const String& data)
{
    size_t dataSize = data.size();
    ClipboardChunk* chunk = new ClipboardChunk(dataSize + CLIPBOARD_CHUNK_META_SIZE);
    char* chunkData = chunk->m_chunk;

    chunkData[0] = id;
    std::memcpy (&chunkData[1], &sequence, 4);
    chunkData[5] = kDataChunk;
    memcpy(&chunkData[6], data.c_str(), dataSize);
    chunkData[dataSize + CLIPBOARD_CHUNK_META_SIZE - 1] = '\0';

    return chunk;
}

ClipboardChunk*
ClipboardChunk::end(ClipboardID id, UInt32 sequence)
{
    ClipboardChunk* end = new ClipboardChunk(CLIPBOARD_CHUNK_META_SIZE);
    char* chunk = end->m_chunk;

    chunk[0] = id;
    std::memcpy (&chunk[1], &sequence, 4);
    chunk[5] = kDataEnd;
    chunk[CLIPBOARD_CHUNK_META_SIZE - 1] = '\0';

    return end;
}

ClipboardChunk*
ClipboardChunk::cancel(ClipboardID id, UInt32 sequence)
{
    ClipboardChunk* cancel = new ClipboardChunk(CLIPBOARD_CHUNK_META_SIZE);
    char* chunk = cancel->m_chunk;

    chunk[0] = id;
    std::memcpy (&chunk[1], &sequence, 4);
    chunk[5] = kDataCancel;
    chunk[CLIPBOARD_CHUNK_META_SIZE - 1] = '\0';

    return cancel;
}

int
ClipboardChunk::assemble(barrier::IStream* stream,
                    ReceiveBuffer& buffer,
                    ClipboardID& id,
                    UInt32& sequence)
{
    UInt8 mark;
    String data;

    if (!ProtocolUtil::readf(stream, kMsgDClipboard + 4, &id, &sequence, &mark, &data)) {
        buffer.release();
        return kError;
    }

    if (id >= kClipboardEnd) {
        buffer.release();
        return kError;
    }

    if (mark == kDataStart) {
        buffer.expectedSize = barrier::string::stringToSizeType(data);
        LOG((CLOG_DEBUG "start receiving clipboard data"));
        String().swap(buffer.data);
        buffer.inProgress = true;
        if (buffer.expectedSize > kMaxReceiveSize) {
            LOG((CLOG_ERR "refusing clipboard transfer larger than receive limit, expected size=%d limit=%d",
                buffer.expectedSize, kMaxReceiveSize));
            buffer.release();
            return kError;
        }
        if (buffer.expectedSize <= kClipboardReceiveReserveLimit) {
            buffer.data.reserve(buffer.expectedSize);
        }
        return kStart;
    }
    else if (mark == kDataChunk) {
        if (!buffer.inProgress) {
            LOG((CLOG_WARN "ignoring clipboard chunk without an active receive"));
            buffer.release();
            return kError;
        }
        if (!canAppendClipboardData(buffer, data)) {
            LOG((CLOG_ERR "corrupted clipboard data, received chunk exceeds expected size=%d current size=%d chunk size=%d",
                buffer.expectedSize, buffer.data.size(), data.size()));
            buffer.release();
            return kError;
        }
        buffer.data.append(data);
        return kNotFinish;
    }
    else if (mark == kDataEnd) {
        if (!buffer.inProgress) {
            LOG((CLOG_WARN "ignoring clipboard end without an active receive"));
            buffer.release();
            return kError;
        }
        // validate
        if (buffer.expectedSize != buffer.data.size()) {
            LOG((CLOG_ERR "corrupted clipboard data, expected size=%d actual size=%d", buffer.expectedSize, buffer.data.size()));
            buffer.release();
            return kError;
        }
        buffer.inProgress = false;
        return kFinish;
    }
    else if (mark == kDataCancel) {
        LOG((CLOG_WARN "clipboard transfer cancelled by sender"));
        buffer.release();
        return kError;
    }

    LOG((CLOG_ERR "clipboard transmission failed: unknown error"));
    buffer.release();
    return kError;
}

void
ClipboardChunk::send(barrier::IStream* stream, void* data)
{
    ClipboardChunk* clipboardData = static_cast<ClipboardChunk*>(data);

    LOG((CLOG_DEBUG1 "sending clipboard chunk"));

    char* chunk = clipboardData->m_chunk;
    ClipboardID id = chunk[0];
    UInt32 sequence;
    std::memcpy (&sequence, &chunk[1], 4);
    UInt8 mark = chunk[5];
    String dataChunk(&chunk[6], clipboardData->m_dataSize);

    switch (mark) {
    case kDataStart:
        LOG((CLOG_DEBUG2 "sending clipboard chunk start: size=%s", dataChunk.c_str()));
        break;

    case kDataChunk:
        LOG((CLOG_DEBUG2 "sending clipboard chunk data: size=%i", dataChunk.size()));
        break;

    case kDataEnd:
        LOG((CLOG_DEBUG2 "sending clipboard finished"));
        break;

    case kDataCancel:
        LOG((CLOG_DEBUG2 "sending clipboard cancelled"));
        break;
    }

    ProtocolUtil::writefLowPriority(stream, kMsgDClipboard, id, sequence, mark, &dataChunk);
}
