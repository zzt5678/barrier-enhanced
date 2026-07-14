#define BARRIER_TEST_ENV
#include "net/SecureSocket.h"

#include "arch/Arch.h"
#include "base/EventTypes.h"
#include "net/ISocketMultiplexerJob.h"
#include "net/NetworkAddress.h"
#include "net/SocketMultiplexer.h"
#include "mt/Lock.h"
#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {

class CallbackMultiplexerJob : public ISocketMultiplexerJob {
public:
    CallbackMultiplexerJob(ArchSocket socket,
                           std::function<void()> callback,
                           std::atomic<int>& destructionCount) :
        m_socket(socket),
        m_callback(std::move(callback)),
        m_destructionCount(destructionCount)
    {
    }

    ~CallbackMultiplexerJob() override
    {
        m_destructionCount.fetch_add(1, std::memory_order_release);
    }

    MultiplexerJobStatus run(bool, bool, bool) override
    {
        m_callback();
        return {false, {}};
    }

    ArchSocket getSocket() const override { return m_socket; }
    bool isReadable() const override { return true; }
    bool isWritable() const override { return false; }

private:
    ArchSocket m_socket;
    std::function<void()> m_callback;
    std::atomic<int>& m_destructionCount;
};

class ConnectedSocketPair {
public:
    ConnectedSocketPair()
    {
        ArchSocket listener = nullptr;
        try {
            NetworkAddress address;
            for (int port = 41000; port < 42000; ++port) {
                listener = ARCH->newSocket(
                    IArchNetwork::kINET, IArchNetwork::kSTREAM);
                try {
                    address = NetworkAddress("127.0.0.1", port);
                    address.resolve();
                    ARCH->setReuseAddrOnSocket(listener, true);
                    ARCH->bindSocket(listener, address.getAddress());
                    ARCH->listenOnSocket(listener);
                    break;
                }
                catch (...) {
                    ARCH->closeSocket(listener);
                    listener = nullptr;
                }
            }

            if (listener == nullptr) {
                throw std::runtime_error("cannot bind loopback test socket");
            }

            m_peer = ARCH->newSocket(
                IArchNetwork::kINET, IArchNetwork::kSTREAM);
            ARCH->connectSocket(m_peer, address.getAddress());

            IArchNetwork::PollEntry entry;
            entry.m_socket = listener;
            entry.m_events = IArchNetwork::kPOLLIN;
            entry.m_revents = 0;
            if (ARCH->pollSocket(&entry, 1, 2.0) == 0) {
                throw std::runtime_error("loopback test connection timed out");
            }

            m_socket = ARCH->acceptSocket(listener, nullptr);
            if (m_socket == nullptr) {
                throw std::runtime_error("cannot accept loopback test connection");
            }
            ARCH->closeSocket(listener);
            listener = nullptr;
        }
        catch (...) {
            if (listener != nullptr) {
                ARCH->closeSocket(listener);
            }
            close();
            throw;
        }
    }

    ~ConnectedSocketPair()
    {
        close();
    }

    ArchSocket releaseSocket()
    {
        ArchSocket socket = m_socket;
        m_socket = nullptr;
        return socket;
    }

    void signalReadable()
    {
        const char byte = 1;
        const double deadline = ARCH->time() + 2.0;
        size_t written = 0;
        while (written == 0 && ARCH->time() < deadline) {
            written = ARCH->writeSocket(m_peer, &byte, sizeof(byte));
            ARCH->sleep(0.001);
        }
        if (written == 0) {
            throw std::runtime_error("cannot signal loopback test socket");
        }
    }

private:
    void close()
    {
        if (m_socket != nullptr) {
            ARCH->closeSocket(m_socket);
            m_socket = nullptr;
        }
        if (m_peer != nullptr) {
            ARCH->closeSocket(m_peer);
            m_peer = nullptr;
        }
    }

    ArchSocket m_socket = nullptr;
    ArchSocket m_peer = nullptr;
};

class TestableSecureSocket : public SecureSocket {
public:
    TestableSecureSocket(IEventQueue* events,
                         SocketMultiplexer* multiplexer) :
        SecureSocket(events, multiplexer, IArchNetwork::kINET,
                     ConnectionSecurityLevel::ENCRYPTED)
    {
    }

    TestableSecureSocket(IEventQueue* events,
                         SocketMultiplexer* multiplexer,
                         ArchSocket socket) :
        SecureSocket(events, multiplexer, socket,
                     ConnectionSecurityLevel::ENCRYPTED)
    {
    }

    void markConnected()
    {
        m_connected = true;
        m_readable = true;
        m_writable = true;
    }

    void failTLSWithLock()
    {
        Lock lock(&getMutex());
        testDisconnectTLSFailureNoLock();
    }

    void failPermanentTLSWithLock()
    {
        Lock lock(&getMutex());
        testDisconnectPermanentTLSFailureNoLock();
    }

    void exhaustSecureConnectRetriesWithLock()
    {
        Lock lock(&getMutex());
        testSecureConnectRetryExhaustedNoLock();
    }

    void installFailingJob(std::atomic<bool>& invoked,
                           std::atomic<int>& destructionCount)
    {
        setJob(std::make_unique<CallbackMultiplexerJob>(
            rawSocket(),
            [this, &invoked]() {
                failTLSWithLock();
                invoked.store(true, std::memory_order_release);
            },
            destructionCount));
    }

    bool connected() const { return m_connected; }
    bool readable() const { return m_readable; }
    bool writable() const { return m_writable; }
    ArchSocket rawSocket() { return getSocket(); }
};

struct EventCounts {
    int stopRetry = 0;
    int disconnected = 0;
    int inputShutdown = 0;
};

void setEventDefaults(MockEventQueue& events,
                      ISocketEvents& socketEvents,
                      IStreamEvents& streamEvents,
                      IDataSocketEvents& dataSocketEvents)
{
    socketEvents.setEvents(&events);
    streamEvents.setEvents(&events);
    dataSocketEvents.setEvents(&events);

    ON_CALL(events, forISocket()).WillByDefault(ReturnRef(socketEvents));
    ON_CALL(events, forIStream()).WillByDefault(ReturnRef(streamEvents));
    ON_CALL(events, forIDataSocket()).WillByDefault(ReturnRef(dataSocketEvents));
    ON_CALL(events, removeHandler(_, _)).WillByDefault(Return());

    auto nextEventType = std::make_shared<int>(100);
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([nextEventType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = (*nextEventType)++;
            }
            return type;
        }));
}

void countEvent(EventCounts& counts,
                Event::Type stopRetryType,
                Event::Type disconnectedType,
                Event::Type inputShutdownType,
                void* target,
                const Event& event)
{
    if (event.getTarget() != target) {
        return;
    }

    if (event.getType() == stopRetryType) {
        ++counts.stopRetry;
    }
    else if (event.getType() == disconnectedType) {
        ++counts.disconnected;
    }
    else if (event.getType() == inputShutdownType) {
        ++counts.inputShutdown;
    }
}

}

TEST(SecureSocketTests, transientTlsFailureClosesTransportAndAllowsRetry)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    SocketMultiplexer multiplexer;
    EventCounts counts;

    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);

    TestableSecureSocket socket(&events, &multiplexer);
    const Event::Type stopRetryType = socketEvents.stopRetry();
    const Event::Type disconnectedType = socketEvents.disconnected();
    const Event::Type inputShutdownType = streamEvents.inputShutdown();

    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&](const Event& event) {
            countEvent(counts, stopRetryType, disconnectedType, inputShutdownType,
                       socket.getEventTarget(), event);
        }));

    socket.markConnected();
    ASSERT_NE(nullptr, socket.rawSocket());

    socket.failTLSWithLock();

    EXPECT_FALSE(socket.connected());
    EXPECT_FALSE(socket.readable());
    EXPECT_FALSE(socket.writable());
    EXPECT_EQ(nullptr, socket.rawSocket());
    EXPECT_EQ(nullptr, socket.newJob().get());
    EXPECT_EQ(0, counts.stopRetry);
    EXPECT_EQ(1, counts.disconnected);
    EXPECT_EQ(1, counts.inputShutdown);

    socket.failTLSWithLock();

    EXPECT_EQ(0, counts.stopRetry);
    EXPECT_EQ(1, counts.disconnected);
    EXPECT_EQ(1, counts.inputShutdown);
}

TEST(SecureSocketTests, tlsFailureLetsMultiplexerRetireCurrentJob)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    SocketMultiplexer multiplexer;
    std::atomic<bool> invoked(false);
    std::atomic<int> jobDestructions(0);

    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);

    {
        ConnectedSocketPair sockets;
        TestableSecureSocket socket(&events, &multiplexer,
                                    sockets.releaseSocket());
        socket.installFailingJob(invoked, jobDestructions);
        sockets.signalReadable();

        const double deadline = ARCH->time() + 2.0;
        while ((!invoked.load(std::memory_order_acquire) ||
                jobDestructions.load(std::memory_order_acquire) == 0) &&
               ARCH->time() < deadline) {
            ARCH->sleep(0.001);
        }

        EXPECT_TRUE(invoked.load(std::memory_order_acquire));
        EXPECT_EQ(1, jobDestructions.load(std::memory_order_acquire));
    }

    EXPECT_EQ(1, jobDestructions.load(std::memory_order_acquire));
}

TEST(SecureSocketTests, secureConnectRetryExhaustionAllowsReconnect)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    SocketMultiplexer multiplexer;
    EventCounts counts;

    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);

    TestableSecureSocket socket(&events, &multiplexer);
    const Event::Type stopRetryType = socketEvents.stopRetry();
    const Event::Type disconnectedType = socketEvents.disconnected();
    const Event::Type inputShutdownType = streamEvents.inputShutdown();

    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&](const Event& event) {
            countEvent(counts, stopRetryType, disconnectedType, inputShutdownType,
                       socket.getEventTarget(), event);
        }));

    socket.markConnected();
    socket.exhaustSecureConnectRetriesWithLock();

    EXPECT_EQ(0, counts.stopRetry);
    EXPECT_EQ(1, counts.disconnected);
    EXPECT_EQ(1, counts.inputShutdown);
    EXPECT_FALSE(socket.connected());
    EXPECT_EQ(nullptr, socket.rawSocket());
}

TEST(SecureSocketTests, permanentTlsFailureStopsRetryOnce)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    SocketMultiplexer multiplexer;
    EventCounts counts;

    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);

    TestableSecureSocket socket(&events, &multiplexer);
    const Event::Type stopRetryType = socketEvents.stopRetry();
    const Event::Type disconnectedType = socketEvents.disconnected();
    const Event::Type inputShutdownType = streamEvents.inputShutdown();

    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&](const Event& event) {
            countEvent(counts, stopRetryType, disconnectedType, inputShutdownType,
                       socket.getEventTarget(), event);
        }));

    socket.markConnected();
    socket.failPermanentTLSWithLock();

    EXPECT_EQ(1, counts.stopRetry);
    EXPECT_EQ(1, counts.disconnected);
    EXPECT_EQ(1, counts.inputShutdown);

    socket.failPermanentTLSWithLock();

    EXPECT_EQ(1, counts.stopRetry);
    EXPECT_EQ(1, counts.disconnected);
    EXPECT_EQ(1, counts.inputShutdown);
}

TEST(SecureSocketTests, permanentFailureAfterTransportFailureStillStopsRetry)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    SocketMultiplexer multiplexer;
    EventCounts counts;

    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);

    TestableSecureSocket socket(&events, &multiplexer);
    const Event::Type stopRetryType = socketEvents.stopRetry();
    const Event::Type disconnectedType = socketEvents.disconnected();
    const Event::Type inputShutdownType = streamEvents.inputShutdown();

    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&](const Event& event) {
            countEvent(counts, stopRetryType, disconnectedType, inputShutdownType,
                       socket.getEventTarget(), event);
        }));

    socket.markConnected();
    socket.failTLSWithLock();
    socket.failPermanentTLSWithLock();
    socket.failPermanentTLSWithLock();

    EXPECT_EQ(1, counts.stopRetry);
    EXPECT_EQ(1, counts.disconnected);
    EXPECT_EQ(1, counts.inputShutdown);
}
