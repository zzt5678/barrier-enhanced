#define BARRIER_TEST_ENV
#include "net/SecureSocket.h"

#include "base/EventTypes.h"
#include "net/SocketMultiplexer.h"
#include "mt/Lock.h"
#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"

#include <memory>

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {

class TestableSecureSocket : public SecureSocket {
public:
    TestableSecureSocket(IEventQueue* events,
                         SocketMultiplexer* multiplexer) :
        SecureSocket(events, multiplexer, IArchNetwork::kINET,
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

TEST(SecureSocketTests, tlsFailureClosesTransportAndNotifiesOnce)
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
    EXPECT_EQ(1, counts.stopRetry);
    EXPECT_EQ(1, counts.disconnected);
    EXPECT_EQ(1, counts.inputShutdown);

    socket.failTLSWithLock();

    EXPECT_EQ(1, counts.stopRetry);
    EXPECT_EQ(1, counts.disconnected);
    EXPECT_EQ(1, counts.inputShutdown);
}
