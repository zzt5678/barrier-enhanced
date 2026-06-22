#include "barrier/FileChunk.h"
#include "barrier/protocol_types.h"
#include "base/Event.h"
#include "io/IStream.h"
#include "io/filesystem.h"

#include "test/global/gtest.h"

#include <algorithm>
#include <cstring>
#include <vector>

class FileChunkTestStream : public barrier::IStream {
public:
    explicit FileChunkTestStream(const std::vector<UInt8>& data) :
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
appendFileChunkMessage(std::vector<UInt8>& data, UInt8 mark, const String& payload)
{
    data.push_back(mark);
    appendUInt32(data, static_cast<UInt32>(payload.size()));
    data.insert(data.end(), payload.begin(), payload.end());
}

TEST(FileChunkTests, assemble_mismatchedEndReleasesReceiveBuffer)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "1048576");
    appendFileChunkMessage(data, kDataChunk, String(1024 * 1024, 'x'));
    appendFileChunkMessage(data, kDataEnd, "");
    FileChunkTestStream stream(data);

    String received;
    size_t expectedSize = 0;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_GE(received.capacity(), 1024u * 1024u);
    const size_t largeCapacity = received.capacity();

    expectedSize += 1;
    EXPECT_EQ(kError, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_LT(received.capacity(), largeCapacity / 2);
    EXPECT_EQ(0u, expectedSize);
}

TEST(FileChunkTests, assemble_incompleteEndReleasesReceiveBuffer)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "1048576");
    appendFileChunkMessage(data, kDataChunk, String(1024, 'x'));
    appendFileChunkMessage(data, kDataEnd, "");
    FileChunkTestStream stream(data);

    String received;
    size_t expectedSize = 0;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_EQ(1024u, received.size());
    EXPECT_EQ(kError, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(0u, expectedSize);
}

TEST(FileChunkTests, assemble_cancelReleasesPartialReceiveBuffer)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "1048576");
    appendFileChunkMessage(data, kDataChunk, String(1024, 'x'));
    appendFileChunkMessage(data, kDataCancel, "");
    FileChunkTestStream stream(data);

    String received;
    size_t expectedSize = 0;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_EQ(1024u, received.size());
    EXPECT_EQ(kError, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(0u, expectedSize);
}

TEST(FileChunkTests, eventDeleteDataDeletesChunkObject)
{
    FileChunk* chunk = FileChunk::start("10");
    Event event(100, NULL, chunk);
    event.setDataObject(chunk);

    EXPECT_EQ(chunk, event.getData());

    Event::deleteData(event);
}

TEST(FileChunkTests, releaseReceiveBufferClearsDataCapacityAndExpectedSize)
{
    String received(1024 * 1024, 'x');
    size_t expectedSize = received.size();
    const size_t largeCapacity = received.capacity();

    FileChunk::releaseReceiveBuffer(received, expectedSize);

    EXPECT_TRUE(received.empty());
    EXPECT_LT(received.capacity(), largeCapacity / 2);
    EXPECT_EQ(0u, expectedSize);
}

TEST(FileChunkTests, assemble_readFailureReleasesReceiveBuffer)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "1048576");
    appendFileChunkMessage(data, kDataChunk, String(1024 * 1024, 'x'));
    FileChunkTestStream stream(data);

    String received;
    size_t expectedSize = 0;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_GE(received.capacity(), 1024u * 1024u);
    const size_t largeCapacity = received.capacity();

    EXPECT_EQ(kError, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_LT(received.capacity(), largeCapacity / 2);
    EXPECT_EQ(0u, expectedSize);
}

TEST(FileChunkTests, assemble_newStartDoesNotRetainPreviousPeakCapacity)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "1048576");
    appendFileChunkMessage(data, kDataChunk, String(1024 * 1024, 'x'));
    appendFileChunkMessage(data, kDataStart, "16");
    FileChunkTestStream stream(data);

    String received;
    size_t expectedSize = 0;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_GE(received.capacity(), 1024u * 1024u);
    const size_t largeCapacity = received.capacity();

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_EQ(16u, expectedSize);
    EXPECT_LT(received.capacity(), largeCapacity / 2);
}

TEST(FileChunkTests, assemble_chunkLargerThanExpectedReleasesReceiveBuffer)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "8");
    appendFileChunkMessage(data, kDataChunk, String(1024 * 1024, 'x'));
    FileChunkTestStream stream(data);

    String received;
    size_t expectedSize = 0;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_EQ(kError, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_LT(received.capacity(), 1024u * 1024u);
    EXPECT_EQ(0u, expectedSize);
}

TEST(FileChunkTests, assemble_endAfterReceiveErrorDoesNotFinish)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "8");
    appendFileChunkMessage(data, kDataChunk, String(1024, 'x'));
    appendFileChunkMessage(data, kDataEnd, "");
    FileChunkTestStream stream(data);

    String received;
    size_t expectedSize = 0;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_EQ(kError, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_EQ(kError, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_EQ(0u, expectedSize);
}

TEST(FileChunkTests, assemble_zeroByteTransferCanFinish)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "0");
    appendFileChunkMessage(data, kDataEnd, "");
    FileChunkTestStream stream(data);

    String received;
    size_t expectedSize = 0;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_EQ(kFinish, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(0u, expectedSize);
}

TEST(FileChunkTests, assemble_receiveErrorDoesNotPoisonAnotherReceiveBuffer)
{
    std::vector<UInt8> failingData;
    appendFileChunkMessage(failingData, kDataStart, "8");
    appendFileChunkMessage(failingData, kDataChunk, String(1024, 'x'));
    FileChunkTestStream failingStream(failingData);

    std::vector<UInt8> validData;
    appendFileChunkMessage(validData, kDataStart, "0");
    appendFileChunkMessage(validData, kDataEnd, "");
    FileChunkTestStream validStream(validData);

    String failingReceived;
    size_t failingExpectedSize = 0;
    String validReceived;
    size_t validExpectedSize = 0;

    EXPECT_EQ(kStart, FileChunk::assemble(&validStream, validReceived, validExpectedSize));
    EXPECT_EQ(kStart, FileChunk::assemble(&failingStream, failingReceived, failingExpectedSize));
    EXPECT_EQ(kError, FileChunk::assemble(&failingStream, failingReceived, failingExpectedSize));
    EXPECT_EQ(kFinish, FileChunk::assemble(&validStream, validReceived, validExpectedSize));
}

TEST(FileChunkTests, assemble_startLargerThanReceiveLimitIsRejected)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart,
        barrier::string::sizeTypeToString(FileChunk::kMaxReceiveSize + 1));
    FileChunkTestStream stream(data);

    String received;
    size_t expectedSize = 0;

    EXPECT_EQ(kError, FileChunk::assemble(&stream, received, expectedSize));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(0u, expectedSize);
}

TEST(FileChunkTests, assemble_largeTransferUsesSpoolFileAndKeepsMemoryEmpty)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart,
        barrier::string::sizeTypeToString(FileChunk::kMemoryReceiveLimit + 1));
    appendFileChunkMessage(data, kDataChunk, "abc");
    FileChunkTestStream stream(data);

    String received;
    size_t expectedSize = 0;
    barrier::fs::path spoolPath;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, received, expectedSize, &spoolPath));
    EXPECT_EQ(FileChunk::kMemoryReceiveLimit + 1, expectedSize);
    ASSERT_FALSE(spoolPath.empty());
    EXPECT_TRUE(barrier::fs::exists(spoolPath));
    EXPECT_TRUE(received.empty());

    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, received, expectedSize, &spoolPath));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(3u, barrier::fs::file_size(spoolPath));

    const barrier::fs::path savedSpoolPath = spoolPath;
    FileChunk::releaseReceiveBuffer(received, expectedSize, &spoolPath);

    EXPECT_TRUE(received.empty());
    EXPECT_EQ(0u, expectedSize);
    EXPECT_TRUE(spoolPath.empty());
    EXPECT_FALSE(barrier::fs::exists(savedSpoolPath));
}

TEST(FileChunkTests, assemble_spooledMismatchRemovesSpool)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart,
        barrier::string::sizeTypeToString(FileChunk::kMemoryReceiveLimit + 1));
    appendFileChunkMessage(data, kDataChunk, "abc");
    appendFileChunkMessage(data, kDataEnd, "");
    FileChunkTestStream stream(data);

    String received;
    size_t expectedSize = 0;
    barrier::fs::path spoolPath;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, received, expectedSize, &spoolPath));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, received, expectedSize, &spoolPath));
    const barrier::fs::path savedSpoolPath = spoolPath;
    ASSERT_TRUE(barrier::fs::exists(savedSpoolPath));

    EXPECT_EQ(kError, FileChunk::assemble(&stream, received, expectedSize, &spoolPath));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(0u, expectedSize);
    EXPECT_TRUE(spoolPath.empty());
    EXPECT_FALSE(barrier::fs::exists(savedSpoolPath));
}

TEST(FileChunkTests, assemble_releaseReceiveBufferClearsSpooledReceiveState)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart,
        barrier::string::sizeTypeToString(FileChunk::kMemoryReceiveLimit + 1));
    appendFileChunkMessage(data, kDataChunk, "abc");
    appendFileChunkMessage(data, kDataChunk, "def");
    appendFileChunkMessage(data, kDataEnd, "");
    FileChunkTestStream stream(data);

    String received;
    size_t expectedSize = 0;
    barrier::fs::path spoolPath;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, received, expectedSize, &spoolPath));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, received, expectedSize, &spoolPath));
    const barrier::fs::path savedSpoolPath = spoolPath;
    ASSERT_TRUE(barrier::fs::exists(savedSpoolPath));

    FileChunk::releaseReceiveBuffer(received, expectedSize, &spoolPath);
    EXPECT_TRUE(spoolPath.empty());
    EXPECT_FALSE(barrier::fs::exists(savedSpoolPath));

    EXPECT_EQ(kError, FileChunk::assemble(&stream, received, expectedSize, &spoolPath));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(0u, expectedSize);
    EXPECT_TRUE(spoolPath.empty());

    EXPECT_EQ(kError, FileChunk::assemble(&stream, received, expectedSize, &spoolPath));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(0u, expectedSize);
    EXPECT_TRUE(spoolPath.empty());
}
