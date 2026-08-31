#define BARRIER_TEST_ENV

#include "arch/XArch.h"
#include "net/TCPSocket.h"
#include "net/SocketMultiplexer.h"
#include "mt/Lock.h"
#include "test/global/TestEventQueue.h"
#include "test/global/gtest.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

class TimeoutTCPSocket : public TCPSocket {
public:
    enum TimeoutMode {
        kTimeoutOnRead,
        kTimeoutOnWrite
    };

    TimeoutTCPSocket(IEventQueue* events,
                     SocketMultiplexer* multiplexer,
                     TimeoutMode mode) :
        TCPSocket(events, multiplexer, IArchNetwork::kINET),
        m_mode(mode)
    {
        m_connected = true;
        m_readable = true;
        m_writable = true;
    }

    EJobResult doRead() override
    {
        if (m_mode == kTimeoutOnRead) {
            throw XArchNetworkTimedOut("read timeout");
        }
        return kRetry;
    }

    EJobResult doWrite() override
    {
        if (m_mode == kTimeoutOnWrite) {
            throw XArchNetworkTimedOut("write timeout");
        }
        return kRetry;
    }

    bool connected() const
    {
        return m_connected;
    }

    bool queueInputBytes(size_t size)
    {
        const std::string payload(size, 'x');
        Lock lock(&getMutex());
        return queueInputOrDisconnectNoLock(payload.data(), static_cast<UInt32>(payload.size()));
    }

private:
    TimeoutMode m_mode;
};

class InputBackpressureTCPSocket : public TCPSocket {
public:
    InputBackpressureTCPSocket(IEventQueue* events,
                               SocketMultiplexer* multiplexer) :
        TCPSocket(events, multiplexer, IArchNetwork::kINET)
    {
        m_connected = true;
        m_readable = true;
        m_writable = true;
    }

    bool connected() const
    {
        return m_connected;
    }

    bool queueInputBytes(size_t size)
    {
        const std::string payload(size, 'x');
        Lock lock(&getMutex());
        return queueInputOrDisconnectNoLock(payload.data(), static_cast<UInt32>(payload.size()));
    }

    size_t inputReadSize(size_t maxReadSize)
    {
        Lock lock(&getMutex());
        return getInputReadSizeNoLock(maxReadSize);
    }
};

class OutputWindowTCPSocket : public TCPSocket {
public:
    OutputWindowTCPSocket(IEventQueue* events,
                          SocketMultiplexer* multiplexer) :
        TCPSocket(events, multiplexer, IArchNetwork::kINET)
    {
        m_connected = true;
        m_readable = true;
        m_writable = true;
    }

    UInt32 highPriorityWriteSize()
    {
        Lock lock(&getMutex());
        return getOutputWriteSizeNoLock(m_outputBuffer);
    }

    UInt32 lowPriorityWriteSize()
    {
        Lock lock(&getMutex());
        return getOutputWriteSizeNoLock(m_lowPriorityOutputBuffer);
    }

    void completeHighPriorityWrite(UInt32 bytes)
    {
        Lock lock(&getMutex());
        discardWrittenData(m_outputBuffer, static_cast<int>(bytes));
    }

    bool connected() const
    {
        return m_connected;
    }
};

class RecordingEventQueue : public TestEventQueue {
public:
    void addEvent(const Event& event) override
    {
        m_types.push_back(event.getType());
        TestEventQueue::addEvent(event);
    }

    size_t count(Event::Type type) const
    {
        size_t result = 0;
        for (std::vector<Event::Type>::const_iterator i = m_types.begin();
             i != m_types.end(); ++i) {
            if (*i == type) {
                ++result;
            }
        }
        return result;
    }

    size_t firstIndex(Event::Type type) const
    {
        for (size_t i = 0; i < m_types.size(); ++i) {
            if (m_types[i] == type) {
                return i;
            }
        }
        return m_types.size();
    }

private:
    std::vector<Event::Type> m_types;
};

class ScriptedReadTCPSocket : public TCPSocket {
public:
    enum Script {
        kImmediateTransient,
        kPayloadThenTransient,
        kPayloadThenEof,
        kImmediateEof,
    };

    ScriptedReadTCPSocket(IEventQueue* events,
                          SocketMultiplexer* multiplexer,
                          Script script) :
        TCPSocket(events, multiplexer, IArchNetwork::kINET),
        m_script(script),
        m_readCount(0)
    {
        m_connected = true;
        m_readable = true;
        m_writable = true;
    }

    bool readWantsRetry()
    {
        return doRead() == kRetry;
    }

    bool connected() const
    {
        return m_connected;
    }

    bool readable() const
    {
        return m_readable;
    }

protected:
    size_t readSocketNoLock(void* buffer, size_t size) override
    {
        const int readCount = m_readCount++;
        if (m_script == kImmediateTransient ||
            (m_script == kPayloadThenTransient && readCount > 0)) {
            throw XArchNetworkInterrupted("socket read would block");
        }
        if (m_script == kImmediateEof ||
            (m_script == kPayloadThenEof && readCount > 0)) {
            return 0;
        }
        if (readCount == 0) {
            const char payload[] = "abc";
            const size_t payloadSize = sizeof(payload) - 1;
            EXPECT_GE(size, payloadSize);
            std::memcpy(buffer, payload, payloadSize);
            return payloadSize;
        }

        ADD_FAILURE() << "unexpected scripted socket read";
        return 0;
    }

private:
    Script m_script;
    int m_readCount;
};

}

TEST(TCPSocketTests, serviceConnectedRetriesReadTimeoutWithoutDisconnecting)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    TimeoutTCPSocket socket(&events, &multiplexer, TimeoutTCPSocket::kTimeoutOnRead);

    MultiplexerJobStatus status =
        socket.testServiceConnected(nullptr, true, false, false);

    EXPECT_TRUE(status.continue_servicing);
    EXPECT_EQ(nullptr, status.new_job.get());
    EXPECT_TRUE(socket.connected());
}

TEST(TCPSocketTests, serviceConnectedRetriesWriteTimeoutWithoutDisconnecting)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    TimeoutTCPSocket socket(&events, &multiplexer, TimeoutTCPSocket::kTimeoutOnWrite);

    MultiplexerJobStatus status =
        socket.testServiceConnected(nullptr, false, true, false);

    EXPECT_TRUE(status.continue_servicing);
    EXPECT_EQ(nullptr, status.new_job.get());
    EXPECT_TRUE(socket.connected());
}

TEST(TCPSocketTests, lowPriorityWriteOverOutputBudgetDisconnectsInsteadOfDropping)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    TimeoutTCPSocket socket(&events, &multiplexer, TimeoutTCPSocket::kTimeoutOnWrite);
    const std::string payload(16 * 1024 * 1024 + 1, 'x');

    socket.writeLowPriority(payload.data(), payload.size());

    EXPECT_EQ(0u, socket.getBufferedOutputSize());
    EXPECT_FALSE(socket.connected());
}

TEST(TCPSocketTests, lowPriorityWritesSharePrimaryFifoBudget)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    TimeoutTCPSocket socket(&events, &multiplexer, TimeoutTCPSocket::kTimeoutOnWrite);
    const std::string acceptedPayload(16 * 1024 * 1024, 'x');
    const std::string overflowPayload(1, 'y');

    socket.writeLowPriority(acceptedPayload.data(), acceptedPayload.size());
    ASSERT_EQ(16u * 1024u * 1024u, socket.getBufferedOutputSize());

    socket.writeLowPriority(overflowPayload.data(), overflowPayload.size());

    EXPECT_EQ(0u, socket.getBufferedOutputSize());
    EXPECT_FALSE(socket.connected());
}

TEST(TCPSocketTests, highPriorityWriteOverOutputBudgetStillDisconnects)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    TimeoutTCPSocket socket(&events, &multiplexer, TimeoutTCPSocket::kTimeoutOnWrite);
    const std::string payload(16 * 1024 * 1024 + 1, 'x');

    socket.write(payload.data(), payload.size());

    EXPECT_EQ(0u, socket.getBufferedOutputSize());
    EXPECT_FALSE(socket.connected());
}

TEST(TCPSocketTests, lowPriorityOutputUsesPrimaryFifo)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    OutputWindowTCPSocket socket(&events, &multiplexer);
    const std::string payload(512 * 1024, 'x');

    socket.writeLowPriority(payload.data(), payload.size());

    EXPECT_EQ(512u * 1024u, socket.highPriorityWriteSize());
    EXPECT_EQ(0u, socket.lowPriorityWriteSize());
    EXPECT_TRUE(socket.connected());
}

TEST(TCPSocketTests, completedWritesAdvanceMonotonicOutputCounter)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    OutputWindowTCPSocket socket(&events, &multiplexer);
    const std::string payload("abcdefgh");

    EXPECT_EQ(0u, socket.getOutputBytesWritten());

    socket.write(payload.data(), static_cast<UInt32>(payload.size()));
    socket.completeHighPriorityWrite(3u);
    EXPECT_EQ(3u, socket.getOutputBytesWritten());

    socket.completeHighPriorityWrite(5u);
    EXPECT_EQ(8u, socket.getOutputBytesWritten());
}

TEST(TCPSocketTests, queuedInputAdvancesMonotonicReceiveCounter)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    InputBackpressureTCPSocket socket(&events, &multiplexer);

    EXPECT_EQ(0u, socket.getInputBytesReceived());

    ASSERT_TRUE(socket.queueInputBytes(3));
    EXPECT_EQ(3u, socket.getInputBytesReceived());

    EXPECT_EQ(2u, socket.read(nullptr, 2));
    EXPECT_EQ(3u, socket.getInputBytesReceived());

    ASSERT_TRUE(socket.queueInputBytes(5));
    EXPECT_EQ(8u, socket.getInputBytesReceived());
}

TEST(TCPSocketTests, highPriorityOutputWriteSizeIsNotCappedByLowPriorityWindow)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    OutputWindowTCPSocket socket(&events, &multiplexer);
    const std::string payload(512 * 1024, 'x');

    socket.write(payload.data(), payload.size());

    EXPECT_EQ(512u * 1024u, socket.highPriorityWriteSize());
    EXPECT_TRUE(socket.connected());
}

TEST(TCPSocketTests, inputOverBudgetDisconnectsWithoutRetainingBuffer)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    TimeoutTCPSocket socket(&events, &multiplexer, TimeoutTCPSocket::kTimeoutOnRead);

    EXPECT_TRUE(socket.queueInputBytes(32 * 1024 * 1024));
    EXPECT_EQ(32u * 1024u * 1024u, socket.getSize());

    EXPECT_FALSE(socket.queueInputBytes(1));

    EXPECT_EQ(0u, socket.getSize());
    EXPECT_FALSE(socket.connected());
}

TEST(TCPSocketTests, inputBackpressureStopsReadRegistrationAtLimit)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    InputBackpressureTCPSocket socket(&events, &multiplexer);

    ASSERT_TRUE(socket.queueInputBytes(32 * 1024 * 1024));

    EXPECT_EQ(nullptr, socket.newJob().get());
    EXPECT_TRUE(socket.connected());
}

TEST(TCPSocketTests, inputBackpressureCapsReadSizeToRemainingBudget)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    InputBackpressureTCPSocket socket(&events, &multiplexer);

    EXPECT_EQ(65536u, socket.inputReadSize(65536));

    ASSERT_TRUE(socket.queueInputBytes(32 * 1024 * 1024 - 1));
    EXPECT_EQ(1u, socket.inputReadSize(65536));

    ASSERT_TRUE(socket.queueInputBytes(1));
    EXPECT_EQ(0u, socket.inputReadSize(65536));
    EXPECT_TRUE(socket.connected());
}

TEST(TCPSocketTests, inputBackpressureDoesNotDisconnectWhenReadIsSignaledAtLimit)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    InputBackpressureTCPSocket socket(&events, &multiplexer);

    ASSERT_TRUE(socket.queueInputBytes(32 * 1024 * 1024));

    MultiplexerJobStatus status =
        socket.testServiceConnected(nullptr, true, false, false);

    EXPECT_FALSE(status.continue_servicing);
    EXPECT_EQ(nullptr, status.new_job.get());
    EXPECT_EQ(32u * 1024u * 1024u, socket.getSize());
    EXPECT_TRUE(socket.connected());
}

TEST(TCPSocketTests, inputBackpressureResumesReadRegistrationAfterDrainingToLowWatermark)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    InputBackpressureTCPSocket socket(&events, &multiplexer);

    ASSERT_TRUE(socket.queueInputBytes(32 * 1024 * 1024));
    ASSERT_EQ(nullptr, socket.newJob().get());

    EXPECT_EQ(16u * 1024u * 1024u, socket.read(nullptr, 16 * 1024 * 1024));

    EXPECT_NE(nullptr, socket.newJob().get());
    EXPECT_TRUE(socket.connected());
}

TEST(TCPSocketTests, transientReadAfterPayloadKeepsConnectionAndBufferedInput)
{
    RecordingEventQueue events;
    SocketMultiplexer multiplexer;
    ScriptedReadTCPSocket socket(
        &events, &multiplexer,
        ScriptedReadTCPSocket::kPayloadThenTransient);
    const Event::Type inputReady = events.forIStream().inputReady();
    const Event::Type inputShutdown = events.forIStream().inputShutdown();

    EXPECT_TRUE(socket.readWantsRetry());
    EXPECT_TRUE(socket.connected());
    EXPECT_TRUE(socket.readable());
    EXPECT_EQ(1u, events.count(inputReady));
    EXPECT_EQ(0u, events.count(inputShutdown));
    ASSERT_EQ(3u, socket.getSize());

    char payload[3] = {};
    EXPECT_EQ(3u, socket.read(payload, sizeof(payload)));
    EXPECT_EQ("abc", std::string(payload, sizeof(payload)));
}

TEST(TCPSocketTests, transientReadBeforePayloadKeepsConnectionOpen)
{
    RecordingEventQueue events;
    SocketMultiplexer multiplexer;
    ScriptedReadTCPSocket socket(
        &events, &multiplexer,
        ScriptedReadTCPSocket::kImmediateTransient);
    const Event::Type inputReady = events.forIStream().inputReady();
    const Event::Type inputShutdown = events.forIStream().inputShutdown();

    EXPECT_TRUE(socket.readWantsRetry());
    EXPECT_TRUE(socket.connected());
    EXPECT_TRUE(socket.readable());
    EXPECT_EQ(0u, socket.getSize());
    EXPECT_EQ(0u, events.count(inputReady));
    EXPECT_EQ(0u, events.count(inputShutdown));
}

TEST(TCPSocketTests, eofAfterPayloadQueuesDataBeforeInputShutdown)
{
    RecordingEventQueue events;
    SocketMultiplexer multiplexer;
    ScriptedReadTCPSocket socket(
        &events, &multiplexer,
        ScriptedReadTCPSocket::kPayloadThenEof);
    const Event::Type inputReady = events.forIStream().inputReady();
    const Event::Type inputShutdown = events.forIStream().inputShutdown();

    EXPECT_FALSE(socket.readWantsRetry());
    EXPECT_TRUE(socket.connected());
    EXPECT_FALSE(socket.readable());
    EXPECT_EQ(3u, socket.getSize());
    ASSERT_EQ(1u, events.count(inputReady));
    ASSERT_EQ(1u, events.count(inputShutdown));
    EXPECT_LT(events.firstIndex(inputReady), events.firstIndex(inputShutdown));
}

TEST(TCPSocketTests, eofWithoutPayloadSignalsInputShutdown)
{
    RecordingEventQueue events;
    SocketMultiplexer multiplexer;
    ScriptedReadTCPSocket socket(
        &events, &multiplexer,
        ScriptedReadTCPSocket::kImmediateEof);
    const Event::Type inputReady = events.forIStream().inputReady();
    const Event::Type inputShutdown = events.forIStream().inputShutdown();

    EXPECT_FALSE(socket.readWantsRetry());
    EXPECT_TRUE(socket.connected());
    EXPECT_FALSE(socket.readable());
    EXPECT_EQ(0u, socket.getSize());
    EXPECT_EQ(0u, events.count(inputReady));
    EXPECT_EQ(1u, events.count(inputShutdown));
}
