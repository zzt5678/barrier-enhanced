#include "barrier/FileChunk.h"
#include "barrier/protocol_types.h"
#include "arch/Arch.h"
#include "base/Event.h"
#include "io/IStream.h"
#include "io/filesystem.h"

#include "test/global/gtest.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

static bool
waitForReceiveState(FileReceiveSession& session,
                    FileReceiveSession::State state,
                    int attempts = 500)
{
    for (int i = 0; i < attempts; ++i) {
        if (session.state() == state) {
            return true;
        }
        ARCH->sleep(0.001);
    }
    return session.state() == state;
}

static barrier::fs::path
waitForSpoolPath(FileReceiveSession& session, int attempts = 500)
{
    barrier::fs::path path;
    for (int i = 0; i < attempts && path.empty(); ++i) {
        path = session.spoolPath();
        if (path.empty()) {
            ARCH->sleep(0.001);
        }
    }
    return path;
}

static bool
waitForPathRemoval(const barrier::fs::path& path, int attempts = 500)
{
    for (int i = 0; i < attempts; ++i) {
        if (!barrier::fs::exists(path)) {
            return true;
        }
        ARCH->sleep(0.001);
    }
    return !barrier::fs::exists(path);
}

static std::string
readFileBytes(const barrier::fs::path& path)
{
    std::ifstream input;
    barrier::open_utf8_path(input, path, std::ios::in | std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

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
        UInt32 count = n < remaining ? n : remaining;
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
    appendFileChunkMessage(data, kDataStart, "1048577");
    appendFileChunkMessage(data, kDataChunk, String(1024 * 1024, 'x'));
    appendFileChunkMessage(data, kDataEnd, "");
    FileChunkTestStream stream(data);

    FileReceiveSession session;
    const String& received = session.data();

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, session));
    EXPECT_GE(received.capacity(), 1024u * 1024u);
    const size_t largeCapacity = received.capacity();

    EXPECT_EQ(kError, FileChunk::assemble(&stream, session));
    EXPECT_LT(received.capacity(), largeCapacity / 2);
    EXPECT_EQ(0u, session.expectedSize());
}

TEST(FileChunkTests, assemble_incompleteEndReleasesReceiveBuffer)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "1048576");
    appendFileChunkMessage(data, kDataChunk, String(1024, 'x'));
    appendFileChunkMessage(data, kDataEnd, "");
    FileChunkTestStream stream(data);

    FileReceiveSession session;
    const String& received = session.data();

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, session));
    EXPECT_EQ(1024u, received.size());
    EXPECT_EQ(kError, FileChunk::assemble(&stream, session));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(0u, session.expectedSize());
}

TEST(FileChunkTests, assemble_cancelReleasesPartialReceiveBuffer)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "1048576");
    appendFileChunkMessage(data, kDataChunk, String(1024, 'x'));
    appendFileChunkMessage(data, kDataCancel, "");
    FileChunkTestStream stream(data);

    FileReceiveSession session;
    const String& received = session.data();

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, session));
    EXPECT_EQ(1024u, received.size());
    EXPECT_EQ(kError, FileChunk::assemble(&stream, session));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(0u, session.expectedSize());
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

    FileReceiveSession session;
    const String& received = session.data();

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, session));
    EXPECT_GE(received.capacity(), 1024u * 1024u);
    const size_t largeCapacity = received.capacity();

    EXPECT_EQ(kError, FileChunk::assemble(&stream, session));
    EXPECT_LT(received.capacity(), largeCapacity / 2);
    EXPECT_EQ(0u, session.expectedSize());
}

TEST(FileChunkTests, assemble_newStartDoesNotRetainPreviousPeakCapacity)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "1048576");
    appendFileChunkMessage(data, kDataChunk, String(1024 * 1024, 'x'));
    appendFileChunkMessage(data, kDataStart, "16");
    FileChunkTestStream stream(data);

    FileReceiveSession session;
    const String& received = session.data();

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, session));
    EXPECT_GE(received.capacity(), 1024u * 1024u);
    const size_t largeCapacity = received.capacity();

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(16u, session.expectedSize());
    EXPECT_LT(received.capacity(), largeCapacity / 2);
}

TEST(FileChunkTests, assemble_chunkLargerThanExpectedReleasesReceiveBuffer)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "8");
    appendFileChunkMessage(data, kDataChunk, String(1024 * 1024, 'x'));
    FileChunkTestStream stream(data);

    FileReceiveSession session;
    const String& received = session.data();

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kError, FileChunk::assemble(&stream, session));
    EXPECT_LT(received.capacity(), 1024u * 1024u);
    EXPECT_EQ(0u, session.expectedSize());
}

TEST(FileChunkTests, assemble_endAfterReceiveErrorDoesNotFinish)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "8");
    appendFileChunkMessage(data, kDataChunk, String(1024, 'x'));
    appendFileChunkMessage(data, kDataEnd, "");
    FileChunkTestStream stream(data);

    FileReceiveSession session;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kError, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kError, FileChunk::assemble(&stream, session));
    EXPECT_EQ(0u, session.expectedSize());
}

TEST(FileChunkTests, assemble_zeroByteTransferCanFinish)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "0");
    appendFileChunkMessage(data, kDataEnd, "");
    FileChunkTestStream stream(data);

    FileReceiveSession session;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kFinish, FileChunk::assemble(&stream, session));
    EXPECT_TRUE(session.data().empty());
    EXPECT_EQ(0u, session.expectedSize());
}

TEST(FileChunkTests, assemble_legacyEmptyEndStillFinishes)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "3");
    appendFileChunkMessage(data, kDataChunk, "abc");
    appendFileChunkMessage(data, kDataEnd, "");
    FileChunkTestStream stream(data);

    FileReceiveSession session;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kFinish, FileChunk::assemble(&stream, session));
    EXPECT_EQ("abc", session.data());
}

TEST(FileChunkTests, assemble_matchingSha256EndFinishes)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "3");
    appendFileChunkMessage(data, kDataChunk, "abc");
    appendFileChunkMessage(
        data,
        kDataEnd,
        "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    FileChunkTestStream stream(data);

    FileReceiveSession session;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kFinish, FileChunk::assemble(&stream, session));
    EXPECT_EQ("abc", session.data());
}

TEST(FileChunkTests, assemble_mismatchedSha256EndFailsAndClearsPayload)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "3");
    appendFileChunkMessage(data, kDataChunk, "abc");
    appendFileChunkMessage(
        data,
        kDataEnd,
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    FileChunkTestStream stream(data);

    FileReceiveSession session;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kError, FileChunk::assemble(&stream, session));
    EXPECT_TRUE(session.data().empty());
    EXPECT_EQ(0u, session.expectedSize());
}

TEST(FileChunkTests, assemble_malformedSha256EndFailsAndClearsPayload)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart, "3");
    appendFileChunkMessage(data, kDataChunk, "abc");
    appendFileChunkMessage(data, kDataEnd, "sha256:not-a-digest");
    FileChunkTestStream stream(data);

    FileReceiveSession session;

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kError, FileChunk::assemble(&stream, session));
    EXPECT_TRUE(session.data().empty());
    EXPECT_EQ(0u, session.expectedSize());
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

    FileReceiveSession failingSession;
    FileReceiveSession validSession;

    EXPECT_EQ(kStart, FileChunk::assemble(&validStream, validSession));
    EXPECT_EQ(kStart, FileChunk::assemble(&failingStream, failingSession));
    EXPECT_EQ(kError, FileChunk::assemble(&failingStream, failingSession));
    EXPECT_EQ(kFinish, FileChunk::assemble(&validStream, validSession));
}

TEST(FileChunkTests, assemble_startLargerThanReceiveLimitIsRejected)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart,
        barrier::string::sizeTypeToString(FileChunk::kMaxReceiveSize + 1));
    FileChunkTestStream stream(data);

    FileReceiveSession session;

    EXPECT_EQ(kError, FileChunk::assemble(&stream, session));
    EXPECT_TRUE(session.data().empty());
    EXPECT_EQ(0u, session.expectedSize());
}

TEST(FileChunkTests, assemble_largeTransferUsesSpoolFileAndKeepsMemoryEmpty)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart,
        barrier::string::sizeTypeToString(FileChunk::kMemoryReceiveLimit + 1));
    appendFileChunkMessage(data, kDataChunk, "abc");
    FileChunkTestStream stream(data);

    FileReceiveSession session;
    const String& received = session.data();

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(FileChunk::kMemoryReceiveLimit + 1, session.expectedSize());
    const barrier::fs::path spoolPath = waitForSpoolPath(session);
    ASSERT_FALSE(spoolPath.empty());
    EXPECT_TRUE(barrier::fs::exists(spoolPath));
    EXPECT_TRUE(received.empty());

    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, session));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(3u, session.receivedSize());

    FileChunk::releaseReceiveBuffer(session);

    EXPECT_TRUE(received.empty());
    EXPECT_EQ(0u, session.expectedSize());
    EXPECT_TRUE(session.spoolPath().empty());
    EXPECT_TRUE(waitForPathRemoval(spoolPath));
}

TEST(FileChunkTests, assemble_spooledMismatchRemovesSpool)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart,
        barrier::string::sizeTypeToString(FileChunk::kMemoryReceiveLimit + 1));
    appendFileChunkMessage(data, kDataChunk, "abc");
    appendFileChunkMessage(data, kDataEnd, "");
    FileChunkTestStream stream(data);

    FileReceiveSession session;
    const String& received = session.data();

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, session));
    const barrier::fs::path spoolPath = waitForSpoolPath(session);
    ASSERT_FALSE(spoolPath.empty());
    ASSERT_TRUE(barrier::fs::exists(spoolPath));

    EXPECT_EQ(kError, FileChunk::assemble(&stream, session));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(0u, session.expectedSize());
    EXPECT_TRUE(session.spoolPath().empty());
    EXPECT_TRUE(waitForPathRemoval(spoolPath));
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

    FileReceiveSession session;
    const String& received = session.data();

    EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
    EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, session));
    const barrier::fs::path spoolPath = waitForSpoolPath(session);
    ASSERT_FALSE(spoolPath.empty());
    ASSERT_TRUE(barrier::fs::exists(spoolPath));

    FileChunk::releaseReceiveBuffer(session);
    EXPECT_TRUE(session.spoolPath().empty());
    EXPECT_TRUE(waitForPathRemoval(spoolPath));

    EXPECT_EQ(kError, FileChunk::assemble(&stream, session));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(0u, session.expectedSize());
    EXPECT_TRUE(session.spoolPath().empty());

    EXPECT_EQ(kError, FileChunk::assemble(&stream, session));
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(0u, session.expectedSize());
    EXPECT_TRUE(session.spoolPath().empty());
}

TEST(FileChunkTests, receiveSessionKeepsOneSpoolHandleOpenUntilTransferEnds)
{
    FileReceiveSession session;

    ASSERT_TRUE(session.begin(6, 0, 0));
    const std::uint64_t generation = session.generation();
    ASSERT_GT(generation, 0u);

    EXPECT_TRUE(session.append("abc"));
    EXPECT_TRUE(session.append("def"));
    EXPECT_EQ(6u, session.receivedSize());

    EXPECT_TRUE(session.finish());
    EXPECT_TRUE(session.state() == FileReceiveSession::kFinalizing ||
                session.state() == FileReceiveSession::kComplete);
    ASSERT_TRUE(waitForReceiveState(session, FileReceiveSession::kComplete));
    EXPECT_FALSE(session.isSpoolOpen());
    EXPECT_TRUE(session.isComplete());
    EXPECT_EQ(generation, session.generation());
    EXPECT_EQ(1u, session.spoolOpenCount());
    EXPECT_EQ(6u, barrier::fs::file_size(session.spoolPath()));
}

TEST(FileChunkTests, receiveSessionVerifiesSha256BeforeCompletingSpool)
{
    FileReceiveSession session;

    ASSERT_TRUE(session.begin(6, 0, 0));
    ASSERT_TRUE(session.append("abc"));
    ASSERT_TRUE(session.append("def"));
    ASSERT_TRUE(session.finish(
        "sha256:bef57ec7f53a6d40beb640a780a639c83bc29ac8a9816f1fc6c5c6dcd93c4721"));
    ASSERT_TRUE(waitForReceiveState(session, FileReceiveSession::kComplete));

    EXPECT_EQ(1u, session.spoolOpenCount());
    EXPECT_EQ("abcdef", readFileBytes(session.spoolPath()));
}

TEST(FileChunkTests, receiveSessionRejectsSha256MismatchAndRemovesSpool)
{
    FileReceiveSession session;

    ASSERT_TRUE(session.begin(6, 0, 0));
    ASSERT_TRUE(session.append("abc"));
    ASSERT_TRUE(session.append("def"));
    const barrier::fs::path spoolPath = waitForSpoolPath(session);
    ASSERT_FALSE(spoolPath.empty());

    EXPECT_FALSE(session.finish(
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    session.fail();

    EXPECT_EQ(FileReceiveSession::kFailed, session.state());
    EXPECT_TRUE(session.spoolPath().empty());
    EXPECT_TRUE(waitForPathRemoval(spoolPath));
}

TEST(FileChunkTests, receiveSessionRejectsChunkLargerThanAsyncQueueBudget)
{
    FileReceiveSession session;

    ASSERT_TRUE(session.begin(8, 0, 0, 4));
    EXPECT_FALSE(session.append("12345"));

    session.fail();
    EXPECT_EQ(FileReceiveSession::kFailed, session.state());
}

TEST(FileChunkTests, receiveSessionGenerationRejectsStaleCompletion)
{
    FileReceiveSession session;

    ASSERT_TRUE(session.begin(6, 0, 0));
    const std::uint64_t firstGeneration = session.generation();
    ASSERT_TRUE(session.append("old"));
    session.reset();
    EXPECT_FALSE(session.matchesGeneration(firstGeneration));

    ASSERT_TRUE(session.begin(3, 0, 0));
    const std::uint64_t secondGeneration = session.generation();
    EXPECT_GT(secondGeneration, firstGeneration);
    EXPECT_FALSE(session.matchesGeneration(firstGeneration));
    EXPECT_TRUE(session.matchesGeneration(secondGeneration));
    ASSERT_TRUE(session.append("new"));
    ASSERT_TRUE(session.finish());
    ASSERT_TRUE(waitForReceiveState(session, FileReceiveSession::kComplete));
    EXPECT_EQ(3u, barrier::fs::file_size(session.spoolPath()));
}

TEST(FileChunkTests, receiveSessionDestructorRemovesPartialSpool)
{
    std::vector<UInt8> data;
    appendFileChunkMessage(data, kDataStart,
        barrier::string::sizeTypeToString(FileChunk::kMemoryReceiveLimit + 1));
    appendFileChunkMessage(data, kDataChunk, "abc");
    FileChunkTestStream stream(data);
    barrier::fs::path spoolPath;

    {
        FileReceiveSession session;
        EXPECT_EQ(kStart, FileChunk::assemble(&stream, session));
        EXPECT_EQ(kNotFinish, FileChunk::assemble(&stream, session));
        spoolPath = waitForSpoolPath(session);
        ASSERT_FALSE(spoolPath.empty());
    }

    EXPECT_TRUE(waitForPathRemoval(spoolPath));
}

TEST(FileChunkTests, receiveSessionCanStartAgainAfterFailure)
{
    FileReceiveSession session;

    ASSERT_TRUE(session.begin(6, 0, 0));
    ASSERT_TRUE(session.append("abc"));
    const barrier::fs::path abandonedPath = waitForSpoolPath(session);
    session.fail();

    EXPECT_TRUE(waitForPathRemoval(abandonedPath));
    ASSERT_TRUE(session.begin(3, 0, 0));
    ASSERT_TRUE(session.append("xyz"));
    ASSERT_TRUE(session.finish());
    ASSERT_TRUE(waitForReceiveState(session, FileReceiveSession::kComplete));
    EXPECT_EQ(3u, barrier::fs::file_size(session.spoolPath()));
    EXPECT_EQ(1u, session.spoolOpenCount());
}
