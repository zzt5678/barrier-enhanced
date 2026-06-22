#define BARRIER_TEST_ENV

#include "arch/XArch.h"
#include "net/TCPSocket.h"
#include "net/SocketMultiplexer.h"
#include "mt/Lock.h"
#include "test/global/TestEventQueue.h"
#include "test/global/gtest.h"

#include <string>

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

    bool connected() const
    {
        return m_connected;
    }
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

TEST(TCPSocketTests, lowPriorityWriteOverOutputBudgetDropsPayloadWithoutDisconnecting)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    TimeoutTCPSocket socket(&events, &multiplexer, TimeoutTCPSocket::kTimeoutOnWrite);
    const std::string payload(8 * 1024 * 1024 + 1, 'x');

    socket.writeLowPriority(payload.data(), payload.size());

    EXPECT_EQ(0u, socket.getBufferedOutputSize());
    EXPECT_TRUE(socket.connected());
}

TEST(TCPSocketTests, lowPriorityWriteOverOutputBudgetPreservesExistingBuffer)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    TimeoutTCPSocket socket(&events, &multiplexer, TimeoutTCPSocket::kTimeoutOnWrite);
    const std::string acceptedPayload(8 * 1024 * 1024, 'x');
    const std::string rejectedPayload(1, 'y');

    socket.writeLowPriority(acceptedPayload.data(), acceptedPayload.size());
    ASSERT_EQ(8u * 1024u * 1024u, socket.getBufferedOutputSize());

    socket.writeLowPriority(rejectedPayload.data(), rejectedPayload.size());

    EXPECT_EQ(8u * 1024u * 1024u, socket.getBufferedOutputSize());
    EXPECT_TRUE(socket.connected());
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

TEST(TCPSocketTests, lowPriorityOutputWriteSizeIsCappedToWindow)
{
    TestEventQueue events;
    SocketMultiplexer multiplexer;
    OutputWindowTCPSocket socket(&events, &multiplexer);
    const std::string payload(512 * 1024, 'x');

    socket.writeLowPriority(payload.data(), payload.size());

    EXPECT_EQ(128u * 1024u, socket.lowPriorityWriteSize());
    EXPECT_TRUE(socket.connected());
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
