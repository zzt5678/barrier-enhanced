#define BARRIER_TEST_ENV
#include "server/ClientProxy1_6.h"

#include "barrier/Clipboard.h"
#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/io/MockStream.h"
#include "test/mock/server/MockServer.h"

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {

void setClientProxy16EventDefaults(MockEventQueue& events,
                                   IStreamEvents& streamEvents,
                                   ClipboardEvents& clipboardEvents,
                                   FileEvents& fileEvents,
                                   Event::Type& nextType)
{
    streamEvents.setEvents(&events);
    clipboardEvents.setEvents(&events);
    fileEvents.setEvents(&events);

    ON_CALL(events, forIStream()).WillByDefault(ReturnRef(streamEvents));
    ON_CALL(events, forClipboard()).WillByDefault(ReturnRef(clipboardEvents));
    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, getQueuedEventCount()).WillByDefault(Return(0));
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([&nextType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = nextType++;
            }
            return type;
        }));
}

Clipboard makeLargeClipboard()
{
    Clipboard clipboard;
    clipboard.open(40);
    clipboard.empty();
    clipboard.add(IClipboard::kText, std::string(300 * 1024, 'x'));
    clipboard.close();
    return clipboard;
}

class CountingClipboard : public IClipboard {
public:
    bool empty() override { return true; }
    void add(EFormat, const String&) override { }
    bool open(Time) const override
    {
        ++openCount;
        return true;
    }
    void close() const override { ++closeCount; }
    Time getTime() const override { return 0; }
    bool has(EFormat format) const override
    {
        ++hasCount;
        return format == kText;
    }
    String get(EFormat format) const override
    {
        ++getCount;
        return format == kText ? String(300 * 1024, 'x') : String();
    }

    mutable int openCount = 0;
    mutable int closeCount = 0;
    mutable int hasCount = 0;
    mutable int getCount = 0;
};

class CleanupFailingClientProxy1_6 : public ClientProxy1_6 {
public:
    CleanupFailingClientProxy1_6(const std::string& name,
                                 barrier::IStream* stream,
                                 Server* server,
                                 IEventQueue* events) :
        ClientProxy1_6(name, stream, server, events)
    {
    }

    bool cleanupClipboardSendThread(bool) override
    {
        ++cleanupCalls;
        return false;
    }

    int cleanupCalls = 0;
};

class CleanupClearingClientProxy1_6 : public ClientProxy1_6 {
public:
    CleanupClearingClientProxy1_6(const std::string& name,
                                  barrier::IStream* stream,
                                  Server* server,
                                  IEventQueue* events) :
        ClientProxy1_6(name, stream, server, events)
    {
    }

    bool cleanupClipboardSendThread(bool) override
    {
        ++cleanupCalls;
        setClipboardDirty(kClipboardClipboard, false);
        return true;
    }

    int cleanupCalls = 0;
};

}

TEST(ClientProxyLifecycleTests, clientProxy16RemovesClipboardSendingHandlerOnDestruction)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents, fileEvents, nextType);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));

    NiceMock<MockServer> server;

    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(clipboardEvents.clipboardSending(), _));

    {
        ClientProxy1_6 proxy("client", stream, &server, &events);
    }
}

TEST(ClientProxyLifecycleTests, clientProxy16KeepsClipboardDirtyWhileAsyncSendIsPending)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents, fileEvents, nextType);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));

    NiceMock<MockServer> server;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    ClientProxy1_6 proxy("client", stream, &server, &events);
    Clipboard clipboard = makeLargeClipboard();

    proxy.setClipboardDirty(kClipboardClipboard, true);
    proxy.setClipboard(kClipboardClipboard, &clipboard);

    EXPECT_TRUE(proxy.testClipboardDirty(kClipboardClipboard));
    EXPECT_TRUE(proxy.cleanupClipboardSendThread(true));
}

TEST(ClientProxyLifecycleTests, clientProxy16ClearsClipboardDirtyAfterAsyncSendCompletes)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents, fileEvents, nextType);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));

    NiceMock<MockServer> server;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    ClientProxy1_6 proxy("client", stream, &server, &events);
    Clipboard clipboard = makeLargeClipboard();

    proxy.setClipboardDirty(kClipboardClipboard, true);
    proxy.setClipboard(kClipboardClipboard, &clipboard);
    EXPECT_TRUE(proxy.cleanupClipboardSendThread(false));

    EXPECT_FALSE(proxy.testClipboardDirty(kClipboardClipboard));
}

TEST(ClientProxyLifecycleTests, clientProxy16CleanupFailureSkipsClipboardCopyAndMarshall)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents, fileEvents, nextType);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));

    NiceMock<MockServer> server;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    CleanupFailingClientProxy1_6 proxy("client", stream, &server, &events);
    CountingClipboard clipboard;

    proxy.setClipboardDirty(kClipboardClipboard, true);
    proxy.setClipboard(kClipboardClipboard, &clipboard);

    EXPECT_EQ(1, proxy.cleanupCalls);
    EXPECT_TRUE(proxy.testClipboardDirty(kClipboardClipboard));
    EXPECT_EQ(0, clipboard.openCount);
    EXPECT_EQ(0, clipboard.hasCount);
    EXPECT_EQ(0, clipboard.getCount);
    EXPECT_EQ(0, clipboard.closeCount);
}

TEST(ClientProxyLifecycleTests, clientProxy16CompletedAsyncSendDoesNotReplayClipboard)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents, fileEvents, nextType);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));

    NiceMock<MockServer> server;
    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());

    CleanupClearingClientProxy1_6 proxy("client", stream, &server, &events);
    CountingClipboard clipboard;

    proxy.setClipboardDirty(kClipboardClipboard, true);
    proxy.setClipboard(kClipboardClipboard, &clipboard);

    EXPECT_EQ(1, proxy.cleanupCalls);
    EXPECT_FALSE(proxy.testClipboardDirty(kClipboardClipboard));
    EXPECT_EQ(0, clipboard.openCount);
    EXPECT_EQ(0, clipboard.hasCount);
    EXPECT_EQ(0, clipboard.getCount);
    EXPECT_EQ(0, clipboard.closeCount);
}
