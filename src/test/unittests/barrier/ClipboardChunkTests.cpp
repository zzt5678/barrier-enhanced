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
#include "barrier/protocol_types.h"
#include "base/Event.h"
#include "io/IStream.h"

#include "test/global/gtest.h"

#include <algorithm>
#include <cstring>
#include <vector>

class ClipboardChunkTestStream : public barrier::IStream {
public:
    explicit ClipboardChunkTestStream(const std::vector<UInt8>& data) :
        m_data(data)
    {
    }

    void close() override { }

    UInt32 read(void* buffer, UInt32 n) override
    {
        UInt32 remaining = static_cast<UInt32>(m_data.size() - m_offset);
        UInt32 count = std::min(n, remaining);
        if (count == 0) {
            return 0;
        }

        std::memcpy(buffer, &m_data[m_offset], count);
        m_offset += count;
        return count;
    }

    void write(const void*, UInt32) override { }
    void writeLowPriority(const void*, UInt32) override { }
    void flush() override { }
    void shutdownInput() override { }
    void shutdownOutput() override { }
    void* getEventTarget() const override { return NULL; }
    bool isReady() const override { return m_offset < m_data.size(); }
    UInt32 getSize() const override { return static_cast<UInt32>(m_data.size() - m_offset); }
    UInt32 getBufferedOutputSize() const override { return 0; }

private:
    std::vector<UInt8> m_data;
    size_t m_offset = 0;
};

static void
appendUInt32(std::vector<UInt8>& data, UInt32 value)
{
    data.push_back(static_cast<UInt8>((value >> 24) & 0xff));
    data.push_back(static_cast<UInt8>((value >> 16) & 0xff));
    data.push_back(static_cast<UInt8>((value >> 8) & 0xff));
    data.push_back(static_cast<UInt8>(value & 0xff));
}

static void
appendClipboardChunkMessage(
        std::vector<UInt8>& data,
        ClipboardID id,
        UInt32 sequence,
        UInt8 mark,
        const String& payload)
{
    data.push_back(id);
    appendUInt32(data, sequence);
    data.push_back(mark);
    appendUInt32(data, static_cast<UInt32>(payload.size()));
    data.insert(data.end(), payload.begin(), payload.end());
}

TEST(ClipboardChunkTests, start_formatStartChunk)
{
    ClipboardID id = 0;
    UInt32 sequence = 0;
    String mockDataSize("10");
    ClipboardChunk* chunk = ClipboardChunk::start(id, sequence, mockDataSize);

    EXPECT_EQ(id, chunk->m_chunk[0]);
    EXPECT_EQ(sequence, (UInt32)chunk->m_chunk[1]);
    EXPECT_EQ(kDataStart, chunk->m_chunk[5]);
    EXPECT_EQ('1', chunk->m_chunk[6]);
    EXPECT_EQ('0', chunk->m_chunk[7]);
    EXPECT_EQ('\0', chunk->m_chunk[8]);

    delete chunk;
}

TEST(ClipboardChunkTests, data_formatDataChunk)
{
    ClipboardID id = 0;
    UInt32 sequence = 1;
    String mockData("mock data");
    ClipboardChunk* chunk = ClipboardChunk::data(id, sequence, mockData);

    EXPECT_EQ(id, chunk->m_chunk[0]);
    EXPECT_EQ(sequence, (UInt32)chunk->m_chunk[1]);
    EXPECT_EQ(kDataChunk, chunk->m_chunk[5]);
    EXPECT_EQ('m', chunk->m_chunk[6]);
    EXPECT_EQ('o', chunk->m_chunk[7]);
    EXPECT_EQ('c', chunk->m_chunk[8]);
    EXPECT_EQ('k', chunk->m_chunk[9]);
    EXPECT_EQ(' ', chunk->m_chunk[10]);
    EXPECT_EQ('d', chunk->m_chunk[11]);
    EXPECT_EQ('a', chunk->m_chunk[12]);
    EXPECT_EQ('t', chunk->m_chunk[13]);
    EXPECT_EQ('a', chunk->m_chunk[14]);
    EXPECT_EQ('\0', chunk->m_chunk[15]);

    delete chunk;
}

TEST(ClipboardChunkTests, end_formatDataChunk)
{
    ClipboardID id = 1;
    UInt32 sequence = 1;
    ClipboardChunk* chunk = ClipboardChunk::end(id, sequence);

    EXPECT_EQ(id, chunk->m_chunk[0]);
    EXPECT_EQ(sequence, (UInt32)chunk->m_chunk[1]);
    EXPECT_EQ(kDataEnd, chunk->m_chunk[5]);
    EXPECT_EQ('\0', chunk->m_chunk[6]);

    delete chunk;
}

TEST(ClipboardChunkTests, cancel_formatDataChunk)
{
    ClipboardID id = 1;
    UInt32 sequence = 1;
    ClipboardChunk* chunk = ClipboardChunk::cancel(id, sequence);

    EXPECT_EQ(id, chunk->m_chunk[0]);
    EXPECT_EQ(sequence, (UInt32)chunk->m_chunk[1]);
    EXPECT_EQ(kDataCancel, chunk->m_chunk[5]);
    EXPECT_EQ('\0', chunk->m_chunk[6]);

    delete chunk;
}

TEST(ClipboardChunkTests, eventDeleteDataDeletesChunkObject)
{
    ClipboardChunk* chunk = ClipboardChunk::start(0, 1, "10");
    Event event(100, NULL, chunk);
    event.setDataObject(chunk);

    EXPECT_EQ(chunk, event.getData());

    Event::deleteData(event);
}

TEST(ClipboardChunkTests, assemble_startUsesPerReceiveBufferState)
{
    std::vector<UInt8> firstData;
    appendClipboardChunkMessage(firstData, 0, 1, kDataStart, "10");
    ClipboardChunkTestStream firstStream(firstData);

    std::vector<UInt8> secondData;
    appendClipboardChunkMessage(secondData, 0, 2, kDataStart, "20");
    ClipboardChunkTestStream secondStream(secondData);

    ClipboardChunk::ReceiveBuffer firstBuffer;
    ClipboardChunk::ReceiveBuffer secondBuffer;
    ClipboardID id = 0;
    UInt32 sequence = 0;

    EXPECT_EQ(kStart, ClipboardChunk::assemble(&firstStream, firstBuffer, id, sequence));
    EXPECT_EQ(kStart, ClipboardChunk::assemble(&secondStream, secondBuffer, id, sequence));

    EXPECT_EQ(10u, firstBuffer.expectedSize);
    EXPECT_EQ(20u, secondBuffer.expectedSize);
    EXPECT_TRUE(firstBuffer.inProgress);
    EXPECT_TRUE(secondBuffer.inProgress);
}

TEST(ClipboardChunkTests, assemble_errorReleasesReceiveBuffer)
{
    std::vector<UInt8> data;
    appendClipboardChunkMessage(data, 0, 1, kDataStart, "1048576");
    appendClipboardChunkMessage(data, 0, 1, kDataChunk, String(1024 * 1024, 'x'));
    appendClipboardChunkMessage(data, 0, 1, kDataEnd, "");
    ClipboardChunkTestStream stream(data);

    ClipboardChunk::ReceiveBuffer buffer;
    ClipboardID id = 0;
    UInt32 sequence = 0;

    EXPECT_EQ(kStart, ClipboardChunk::assemble(&stream, buffer, id, sequence));
    EXPECT_EQ(kNotFinish, ClipboardChunk::assemble(&stream, buffer, id, sequence));
    EXPECT_GE(buffer.data.capacity(), 1024u * 1024u);
    const size_t largeCapacity = buffer.data.capacity();

    buffer.expectedSize += 1;
    EXPECT_EQ(kError, ClipboardChunk::assemble(&stream, buffer, id, sequence));
    EXPECT_LT(buffer.data.capacity(), largeCapacity / 2);
    EXPECT_EQ(0u, buffer.expectedSize);
    EXPECT_FALSE(buffer.inProgress);
}

TEST(ClipboardChunkTests, assemble_newStartDoesNotRetainPreviousPeakCapacity)
{
    std::vector<UInt8> data;
    appendClipboardChunkMessage(data, 0, 1, kDataStart, "1048576");
    appendClipboardChunkMessage(data, 0, 1, kDataChunk, String(1024 * 1024, 'x'));
    appendClipboardChunkMessage(data, 0, 2, kDataStart, "16");
    ClipboardChunkTestStream stream(data);

    ClipboardChunk::ReceiveBuffer buffer;
    ClipboardID id = 0;
    UInt32 sequence = 0;

    EXPECT_EQ(kStart, ClipboardChunk::assemble(&stream, buffer, id, sequence));
    EXPECT_EQ(kNotFinish, ClipboardChunk::assemble(&stream, buffer, id, sequence));
    EXPECT_GE(buffer.data.capacity(), 1024u * 1024u);
    const size_t largeCapacity = buffer.data.capacity();

    EXPECT_EQ(kStart, ClipboardChunk::assemble(&stream, buffer, id, sequence));
    EXPECT_EQ(16u, buffer.expectedSize);
    EXPECT_LT(buffer.data.capacity(), largeCapacity / 2);
    EXPECT_TRUE(buffer.inProgress);
}

TEST(ClipboardChunkTests, assemble_chunkLargerThanExpectedReleasesReceiveBuffer)
{
    std::vector<UInt8> data;
    appendClipboardChunkMessage(data, 0, 1, kDataStart, "8");
    appendClipboardChunkMessage(data, 0, 1, kDataChunk, String(1024 * 1024, 'x'));
    ClipboardChunkTestStream stream(data);

    ClipboardChunk::ReceiveBuffer buffer;
    ClipboardID id = 0;
    UInt32 sequence = 0;

    EXPECT_EQ(kStart, ClipboardChunk::assemble(&stream, buffer, id, sequence));
    EXPECT_EQ(kError, ClipboardChunk::assemble(&stream, buffer, id, sequence));
    EXPECT_LT(buffer.data.capacity(), 1024u * 1024u);
    EXPECT_EQ(0u, buffer.expectedSize);
    EXPECT_FALSE(buffer.inProgress);
}

TEST(ClipboardChunkTests, assemble_cancelReleasesReceiveBuffer)
{
    std::vector<UInt8> data;
    appendClipboardChunkMessage(data, 0, 1, kDataStart, "1048576");
    appendClipboardChunkMessage(data, 0, 1, kDataChunk, String(1024 * 1024, 'x'));
    appendClipboardChunkMessage(data, 0, 1, kDataCancel, "");
    ClipboardChunkTestStream stream(data);

    ClipboardChunk::ReceiveBuffer buffer;
    ClipboardID id = 0;
    UInt32 sequence = 0;

    EXPECT_EQ(kStart, ClipboardChunk::assemble(&stream, buffer, id, sequence));
    EXPECT_EQ(kNotFinish, ClipboardChunk::assemble(&stream, buffer, id, sequence));
    EXPECT_GE(buffer.data.capacity(), 1024u * 1024u);

    EXPECT_EQ(kError, ClipboardChunk::assemble(&stream, buffer, id, sequence));
    EXPECT_LT(buffer.data.capacity(), 512u * 1024u);
    EXPECT_EQ(0u, buffer.expectedSize);
    EXPECT_FALSE(buffer.inProgress);
}

TEST(ClipboardChunkTests, assemble_endWithoutStartReturnsError)
{
    std::vector<UInt8> data;
    appendClipboardChunkMessage(data, 0, 1, kDataEnd, "");
    ClipboardChunkTestStream stream(data);

    ClipboardChunk::ReceiveBuffer buffer;
    ClipboardID id = 0;
    UInt32 sequence = 0;

    EXPECT_EQ(kError, ClipboardChunk::assemble(&stream, buffer, id, sequence));
    EXPECT_EQ(0u, buffer.expectedSize);
    EXPECT_TRUE(buffer.data.empty());
    EXPECT_FALSE(buffer.inProgress);
}

TEST(ClipboardChunkTests, assemble_chunkWithoutStartReturnsError)
{
    std::vector<UInt8> data;
    appendClipboardChunkMessage(data, 0, 1, kDataChunk, "stale");
    ClipboardChunkTestStream stream(data);

    ClipboardChunk::ReceiveBuffer buffer;
    ClipboardID id = 0;
    UInt32 sequence = 0;

    EXPECT_EQ(kError, ClipboardChunk::assemble(&stream, buffer, id, sequence));
    EXPECT_EQ(0u, buffer.expectedSize);
    EXPECT_TRUE(buffer.data.empty());
    EXPECT_FALSE(buffer.inProgress);
}

TEST(ClipboardChunkTests, assemble_rejectsClipboardLargerThanReceiveLimit)
{
    std::vector<UInt8> data;
    appendClipboardChunkMessage(
        data,
        0,
        1,
        kDataStart,
        barrier::string::sizeTypeToString(ClipboardChunk::kMaxReceiveSize + 1));
    ClipboardChunkTestStream stream(data);

    ClipboardChunk::ReceiveBuffer buffer;
    ClipboardID id = 0;
    UInt32 sequence = 0;

    EXPECT_EQ(kError, ClipboardChunk::assemble(&stream, buffer, id, sequence));
    EXPECT_EQ(0u, buffer.expectedSize);
    EXPECT_TRUE(buffer.data.empty());
    EXPECT_FALSE(buffer.inProgress);
}
