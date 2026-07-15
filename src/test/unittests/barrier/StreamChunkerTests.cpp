/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 */

#include "barrier/StreamChunker.h"
#include "barrier/ClipboardChunk.h"
#include "barrier/protocol_types.h"
#include "base/Event.h"
#include "base/EventTypes.h"
#include "barrier/FileChunk.h"
#include "io/filesystem.h"

#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/io/MockStream.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {

std::string uniqueToken()
{
    std::ostringstream stream;
    stream << std::hex
           << static_cast<unsigned long long>(std::time(nullptr))
           << "-"
           << static_cast<unsigned long long>(
               std::hash<std::thread::id>{}(std::this_thread::get_id()))
           << "-"
           << static_cast<unsigned long long>(std::rand());
    return stream.str();
}

barrier::fs::path writeTempFile(size_t size)
{
    const barrier::fs::path path =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("stream-chunker-" + uniqueToken());
    std::ofstream output;
    barrier::open_utf8_path(output, path, std::ios::out | std::ios::binary | std::ios::trunc);
    std::string block(4096, 'x');
    size_t written = 0;
    while (written < size) {
        const size_t toWrite = std::min(block.size(), size - written);
        output.write(block.data(), toWrite);
        written += toWrite;
    }
    return path;
}

barrier::fs::path writeSparseTempFile(size_t size)
{
    const barrier::fs::path path =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("stream-chunker-sparse-" + uniqueToken());
    std::ofstream output;
    barrier::open_utf8_path(output, path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (size > 0) {
        output.seekp(static_cast<std::streamoff>(size - 1));
        const char marker = '\0';
        output.write(&marker, 1);
    }
    return path;
}

}

TEST(StreamChunkerTests, sendFileThrottlesQueuedBulkEvents)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    FileEvents fileEvents;
    fileEvents.setEvents(&events);
    StreamChunker chunker;

    size_t queuedEvents = 0;
    size_t maxQueuedEvents = 0;
    size_t throttleChecks = 0;
    Event::Type nextType = Event::kLast;

    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([&nextType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = nextType++;
            }
            return type;
        }));
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&queuedEvents, &maxQueuedEvents](const Event& event) {
            ++queuedEvents;
            maxQueuedEvents = std::max(maxQueuedEvents, queuedEvents);
            Event::deleteData(event);
        }));
    ON_CALL(events, getQueuedEventCount())
        .WillByDefault(Invoke([&queuedEvents, &throttleChecks, &chunker]() {
            if (queuedEvents >= 16) {
                --queuedEvents;
                ++throttleChecks;
                chunker.interruptFile();
                return static_cast<size_t>(16);
            }
            return queuedEvents;
        }));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(0u));

    const barrier::fs::path path = writeSparseTempFile(256 * 1024 * 1024);
    chunker.sendFile(path.u8string().c_str(), &events, &events, &stream);

    EXPECT_GT(throttleChecks, 0u);
    EXPECT_LE(maxQueuedEvents, 18u);

    barrier::fs::remove(path);
}

TEST(StreamChunkerTests, largeFileChunksDoNotExceedAtomicityFallbackSize)
{
    NiceMock<MockEventQueue> events;
    FileEvents fileEvents;
    fileEvents.setEvents(&events);
    size_t largestDataChunk = 0;
    Event::Type nextType = Event::kLast;

    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([&nextType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = nextType++;
            }
            return type;
        }));
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&fileEvents, &largestDataChunk](const Event& event) {
            if (event.getType() == fileEvents.fileChunkSending() &&
                event.getData() != nullptr) {
                const auto* chunk = static_cast<const FileChunk*>(event.getData());
                if (chunk->m_chunk[0] == kDataChunk) {
                    largestDataChunk = std::max(largestDataChunk, chunk->m_dataSize);
                }
            }
            Event::deleteData(event);
        }));

    const barrier::fs::path path = writeSparseTempFile(40 * 1024 * 1024);
    StreamChunker chunker;
    chunker.sendFile(path.u8string().c_str(), &events, &events);

    EXPECT_GT(largestDataChunk, 0u);
    EXPECT_LE(largestDataChunk, 64u * 1024u);

    barrier::fs::remove(path);
}

TEST(StreamChunkerTests, sendFileQueuesFinalKeepAliveAfterCompletion)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    FileEvents fileEvents;
    fileEvents.setEvents(&events);

    size_t keepAliveEvents = 0;
    Event::Type nextType = Event::kLast;

    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([&nextType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = nextType++;
            }
            return type;
        }));
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&fileEvents, &keepAliveEvents](const Event& event) {
            if (event.getType() == fileEvents.keepAlive()) {
                ++keepAliveEvents;
            }
            Event::deleteData(event);
        }));
    ON_CALL(events, getQueuedEventCount()).WillByDefault(Return(0u));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(0u));

    const barrier::fs::path path = writeTempFile(1024);
    StreamChunker chunker;
    chunker.sendFile(path.u8string().c_str(), &events, &events, &stream);

    EXPECT_GE(keepAliveEvents, 2u);

    barrier::fs::remove(path);
}

TEST(StreamChunkerTests, sendFileEndCarriesSha256Digest)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    FileEvents fileEvents;
    fileEvents.setEvents(&events);

    String endDigest;
    Event::Type nextType = Event::kLast;

    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([&nextType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = nextType++;
            }
            return type;
        }));
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&fileEvents, &endDigest](const Event& event) {
            if (event.getType() == fileEvents.fileChunkSending() &&
                event.getData() != NULL) {
                const auto* chunk = static_cast<const FileChunk*>(event.getData());
                if (chunk->m_chunk[0] == kDataEnd) {
                    endDigest.assign(&chunk->m_chunk[1], chunk->m_dataSize);
                }
            }
            Event::deleteData(event);
        }));
    ON_CALL(events, getQueuedEventCount()).WillByDefault(Return(0u));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(0u));

    const barrier::fs::path path = writeTempFile(3);
    StreamChunker chunker;
    chunker.sendFile(path.u8string().c_str(), &events, &events, &stream);

    EXPECT_EQ(
        "sha256:cd2eb0837c9b4c962c22d2ff8b5441b7b45805887f051d39bf133b583baf6860",
        endDigest);

    barrier::fs::remove(path);
}

TEST(StreamChunkerTests, sendFileQueuesFinalKeepAliveAfterOpenFailure)
{
    NiceMock<MockEventQueue> events;
    FileEvents fileEvents;
    fileEvents.setEvents(&events);

    size_t keepAliveEvents = 0;
    Event::Type nextType = Event::kLast;

    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([&nextType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = nextType++;
            }
            return type;
        }));
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&fileEvents, &keepAliveEvents](const Event& event) {
            if (event.getType() == fileEvents.keepAlive()) {
                ++keepAliveEvents;
            }
            Event::deleteData(event);
        }));

    StreamChunker chunker;
    EXPECT_THROW(chunker.sendFile("/tmp/weave-missing-stream-chunker-test-file",
                                  &events, &events), std::runtime_error);

    EXPECT_EQ(1u, keepAliveEvents);
}

TEST(StreamChunkerTests, sendClipboardDoesNotStartWhenOutputAlreadyStalled)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    FileEvents fileEvents;
    ClipboardEvents clipboardEvents;
    fileEvents.setEvents(&events);
    clipboardEvents.setEvents(&events);

    std::vector<UInt8> clipboardMarks;
    Event::Type nextType = Event::kLast;

    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, forClipboard()).WillByDefault(ReturnRef(clipboardEvents));
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([&nextType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = nextType++;
            }
            return type;
        }));
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&clipboardEvents, &clipboardMarks](const Event& event) {
            if (event.getType() == clipboardEvents.clipboardSending() &&
                event.getData() != NULL) {
                auto* chunk = static_cast<ClipboardChunk*>(event.getData());
                clipboardMarks.push_back(static_cast<UInt8>(chunk->m_chunk[5]));
            }
            Event::deleteData(event);
        }));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(256u * 1024u));

    String data(1024 * 1024, 'x');
    EXPECT_FALSE(StreamChunker::sendClipboard(data, data.size(), kClipboardClipboard, 7,
                                             &events, &events, &stream));

    EXPECT_TRUE(clipboardMarks.empty());
}

TEST(StreamChunkerTests, sendClipboardQueuesCancelWhenOutputStallsAfterStart)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    FileEvents fileEvents;
    ClipboardEvents clipboardEvents;
    fileEvents.setEvents(&events);
    clipboardEvents.setEvents(&events);

    std::vector<UInt8> clipboardMarks;
    Event::Type nextType = Event::kLast;
    size_t bufferChecks = 0;

    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, forClipboard()).WillByDefault(ReturnRef(clipboardEvents));
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([&nextType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = nextType++;
            }
            return type;
        }));
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&clipboardEvents, &clipboardMarks](const Event& event) {
            if (event.getType() == clipboardEvents.clipboardSending() &&
                event.getData() != NULL) {
                auto* chunk = static_cast<ClipboardChunk*>(event.getData());
                clipboardMarks.push_back(static_cast<UInt8>(chunk->m_chunk[5]));
            }
            Event::deleteData(event);
        }));
    ON_CALL(events, getQueuedEventCount()).WillByDefault(Return(0u));
    ON_CALL(stream, getBufferedOutputSize())
        .WillByDefault(Invoke([&bufferChecks]() {
            return bufferChecks++ == 0 ? 0u : 256u * 1024u;
        }));

    String data(1024 * 1024, 'x');
    EXPECT_FALSE(StreamChunker::sendClipboard(data, data.size(), kClipboardClipboard, 7,
                                             &events, &events, &stream));

    ASSERT_EQ(2u, clipboardMarks.size());
    EXPECT_EQ(kDataStart, clipboardMarks[0]);
    EXPECT_EQ(kDataCancel, clipboardMarks[1]);
}

TEST(StreamChunkerTests, sendClipboardDoesNotWaitForQueuedEventBudget)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    FileEvents fileEvents;
    ClipboardEvents clipboardEvents;
    fileEvents.setEvents(&events);
    clipboardEvents.setEvents(&events);

    std::vector<UInt8> clipboardMarks;
    Event::Type nextType = Event::kLast;

    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, forClipboard()).WillByDefault(ReturnRef(clipboardEvents));
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([&nextType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = nextType++;
            }
            return type;
        }));
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&clipboardEvents, &clipboardMarks](const Event& event) {
            if (event.getType() == clipboardEvents.clipboardSending() &&
                event.getData() != NULL) {
                auto* chunk = static_cast<ClipboardChunk*>(event.getData());
                clipboardMarks.push_back(static_cast<UInt8>(chunk->m_chunk[5]));
            }
            Event::deleteData(event);
        }));
    EXPECT_CALL(events, getQueuedEventCount()).Times(0);
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(0u));

    String data("clipboard");
    EXPECT_TRUE(StreamChunker::sendClipboard(data, data.size(), kClipboardClipboard, 7,
                                            &events, &events, &stream));

    ASSERT_EQ(3u, clipboardMarks.size());
    EXPECT_EQ(kDataStart, clipboardMarks[0]);
    EXPECT_EQ(kDataChunk, clipboardMarks[1]);
    EXPECT_EQ(kDataEnd, clipboardMarks[2]);
}

TEST(StreamChunkerTests, sendClipboardDataThrottlesQueuedBulkEvents)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    FileEvents fileEvents;
    ClipboardEvents clipboardEvents;
    fileEvents.setEvents(&events);
    clipboardEvents.setEvents(&events);

    std::vector<UInt8> clipboardMarks;
    Event::Type nextType = Event::kLast;
    size_t queuedEvents = 0;
    size_t maxQueuedEvents = 0;
    size_t throttleChecks = 0;

    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, forClipboard()).WillByDefault(ReturnRef(clipboardEvents));
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([&nextType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = nextType++;
            }
            return type;
        }));
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&clipboardEvents, &clipboardMarks, &queuedEvents, &maxQueuedEvents](const Event& event) {
            ++queuedEvents;
            maxQueuedEvents = std::max(maxQueuedEvents, queuedEvents);
            if (event.getType() == clipboardEvents.clipboardSending() &&
                event.getData() != NULL) {
                auto* chunk = static_cast<ClipboardChunk*>(event.getData());
                clipboardMarks.push_back(static_cast<UInt8>(chunk->m_chunk[5]));
            }
            Event::deleteData(event);
        }));
    ON_CALL(events, getQueuedEventCount())
        .WillByDefault(Invoke([&queuedEvents, &throttleChecks]() {
            if (queuedEvents >= 32) {
                --queuedEvents;
                ++throttleChecks;
                return static_cast<size_t>(32);
            }
            return queuedEvents;
        }));
    ON_CALL(stream, getBufferedOutputSize()).WillByDefault(Return(0u));

    String data(2 * 1024 * 1024, 'x');
    StreamChunker chunker;
    EXPECT_TRUE(chunker.sendClipboardData(data, data.size(), kClipboardClipboard, 7,
                                          &events, &events, &stream));

    EXPECT_GT(throttleChecks, 0u);
    EXPECT_LE(maxQueuedEvents, 33u);
    ASSERT_GE(clipboardMarks.size(), 3u);
    EXPECT_EQ(kDataStart, clipboardMarks.front());
    EXPECT_EQ(kDataEnd, clipboardMarks.back());
}

TEST(StreamChunkerTests, sendClipboardRejectsPayloadLargerThanReceiveLimit)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;

    EXPECT_CALL(events, addEvent(_)).Times(0);

    String data("small-placeholder");
    EXPECT_FALSE(StreamChunker::sendClipboard(
        data,
        ClipboardChunk::kMaxReceiveSize + 1,
        kClipboardClipboard,
        7,
        &events,
        &events,
        &stream));
}

TEST(StreamChunkerTests, sendFileDoesNotStartWhenOutputAlreadyStalled)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    NiceMock<MockStream> readyStream;
    FileEvents fileEvents;
    fileEvents.setEvents(&events);

    std::vector<UInt8> fileMarks;
    Event::Type nextType = Event::kLast;
    StreamChunker chunker;

    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([&nextType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = nextType++;
            }
            return type;
        }));
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&fileEvents, &fileMarks](const Event& event) {
            if (event.getType() == fileEvents.fileChunkSending() &&
                event.getData() != NULL) {
                auto* chunk = static_cast<FileChunk*>(event.getData());
                fileMarks.push_back(chunk->m_chunk[0]);
            }
            Event::deleteData(event);
        }));
    ON_CALL(events, getQueuedEventCount()).WillByDefault(Return(0u));
    ON_CALL(stream, getBufferedOutputSize())
        .WillByDefault(Invoke([&chunker]() {
            chunker.interruptFile();
            return 1024u * 1024u;
        }));
    ON_CALL(readyStream, getBufferedOutputSize()).WillByDefault(Return(0u));

    const barrier::fs::path path = writeTempFile(1024);
    chunker.sendFile(path.u8string().c_str(), &events, &events, &stream);

    EXPECT_TRUE(fileMarks.empty());

    chunker.sendFile(path.u8string().c_str(), &events, &events, &readyStream);
    ASSERT_EQ(3u, fileMarks.size());
    EXPECT_EQ(kDataStart, fileMarks[0]);
    EXPECT_EQ(kDataChunk, fileMarks[1]);
    EXPECT_EQ(kDataEnd, fileMarks[2]);

    barrier::fs::remove(path);
}

TEST(StreamChunkerTests, sendFileWaitsWhileOutputBufferMakesProgress)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    FileEvents fileEvents;
    fileEvents.setEvents(&events);

    std::vector<UInt8> fileMarks;
    Event::Type nextType = Event::kLast;
    size_t bufferChecks = 0;

    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([&nextType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = nextType++;
            }
            return type;
        }));
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&fileEvents, &fileMarks](const Event& event) {
            if (event.getType() == fileEvents.fileChunkSending() &&
                event.getData() != NULL) {
                auto* chunk = static_cast<FileChunk*>(event.getData());
                fileMarks.push_back(chunk->m_chunk[0]);
            }
            Event::deleteData(event);
        }));
    ON_CALL(events, getQueuedEventCount()).WillByDefault(Return(0u));
    ON_CALL(stream, getBufferedOutputSize())
        .WillByDefault(Invoke([&bufferChecks]() {
            const UInt32 samples[] = {
                1024u * 1024u,
                900u * 1024u,
                700u * 1024u,
                400u * 1024u
            };
            if (bufferChecks < sizeof(samples) / sizeof(samples[0])) {
                return samples[bufferChecks++];
            }
            return 0u;
        }));

    const barrier::fs::path path = writeTempFile(1024);
    StreamChunker chunker;
    chunker.sendFile(path.u8string().c_str(), &events, &events, &stream);

    ASSERT_EQ(3u, fileMarks.size());
    EXPECT_EQ(kDataStart, fileMarks[0]);
    EXPECT_EQ(kDataChunk, fileMarks[1]);
    EXPECT_EQ(kDataEnd, fileMarks[2]);

    barrier::fs::remove(path);
}

TEST(StreamChunkerTests, sendFileQueuesCancelWhenOutputStallsAfterStart)
{
    NiceMock<MockEventQueue> events;
    NiceMock<MockStream> stream;
    FileEvents fileEvents;
    fileEvents.setEvents(&events);

    std::vector<UInt8> fileMarks;
    Event::Type nextType = Event::kLast;
    StreamChunker chunker;
    size_t bufferChecks = 0;

    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([&nextType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = nextType++;
            }
            return type;
        }));
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&fileEvents, &fileMarks](const Event& event) {
            if (event.getType() == fileEvents.fileChunkSending() &&
                event.getData() != NULL) {
                auto* chunk = static_cast<FileChunk*>(event.getData());
                fileMarks.push_back(chunk->m_chunk[0]);
            }
            Event::deleteData(event);
        }));
    ON_CALL(events, getQueuedEventCount()).WillByDefault(Return(0u));
    ON_CALL(stream, getBufferedOutputSize())
        .WillByDefault(Invoke([&chunker, &bufferChecks]() {
            if (bufferChecks++ == 0) {
                return 0u;
            }
            chunker.interruptFile();
            return 1024u * 1024u;
        }));

    const barrier::fs::path path = writeTempFile(1024 * 1024);
    chunker.sendFile(path.u8string().c_str(), &events, &events, &stream);

    ASSERT_EQ(2u, fileMarks.size());
    EXPECT_EQ(kDataStart, fileMarks[0]);
    EXPECT_EQ(kDataCancel, fileMarks[1]);

    barrier::fs::remove(path);
}
