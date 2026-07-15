#define BARRIER_TEST_ENV
#include "server/ClientProxy1_6.h"
#include "server/ClientProxy1_7.h"
#include "server/ClientProxy1_8.h"

#include "barrier/Clipboard.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "arch/Arch.h"
#include "base/Stopwatch.h"
#include "mt/Thread.h"
#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/io/MockStream.h"
#include "test/mock/server/MockServer.h"

#include <atomic>
#include <cstring>
#include <vector>

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

class RecordingStream : public barrier::IStream {
public:
    void clear()
    {
        data.clear();
        offset = 0;
    }

    void close() override { }
    UInt32 read(void* buffer, UInt32 count) override
    {
        const UInt32 available = getSize();
        const UInt32 copied = count < available ? count : available;
        if (copied != 0) {
            std::memcpy(buffer, data.data() + offset, copied);
            offset += copied;
        }
        return copied;
    }
    void write(const void* buffer, UInt32 count) override
    {
        const UInt8* bytes = static_cast<const UInt8*>(buffer);
        data.insert(data.end(), bytes, bytes + count);
    }
    void writeLowPriority(const void* buffer, UInt32 count) override
    {
        write(buffer, count);
    }
    void flush() override { }
    void shutdownInput() override { }
    void shutdownOutput() override { }
    void* getEventTarget() const override
    {
        return const_cast<RecordingStream*>(this);
    }
    bool isReady() const override { return getSize() != 0; }
    UInt32 getSize() const override
    {
        return static_cast<UInt32>(data.size() - offset);
    }
    UInt32 getBufferedOutputSize() const override { return 0; }

private:
    std::vector<UInt8> data;
    std::size_t offset = 0;
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

TEST(ClientProxyLifecycleTests, clientProxy17PublishesDecodedHandoffReadiness)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientProxyEvents clientProxyEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    clientProxyEvents.setEvents(&events);
    ON_CALL(events, forClientProxy()).WillByDefault(ReturnRef(clientProxyEvents));

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    const UInt8 payload[] = { 0x00, 0x00, 0x00, 0x2a, 0x01 };
    size_t offset = 0;
    ON_CALL(*stream, read(_, _)).WillByDefault(
        Invoke([&](void* buffer, UInt32 count) -> UInt32 {
            const UInt32 available = static_cast<UInt32>(sizeof(payload) - offset);
            const UInt32 copied = count < available ? count : available;
            std::memcpy(buffer, payload + offset, copied);
            offset += copied;
            return copied;
        }));

    NiceMock<MockServer> server;
    ClientProxy1_7 proxy("client", stream, &server, &events);
    const Event::Type readyType = clientProxyEvents.inputHandoffReady();
    EXPECT_CALL(events, addEvent(_)).WillOnce(
        Invoke([&](const Event& event) {
            EXPECT_EQ(readyType, event.getType());
            EXPECT_EQ(proxy.getEventTarget(), event.getTarget());
            BaseClientProxy::InputHandoffReadyInfo* info =
                static_cast<BaseClientProxy::InputHandoffReadyInfo*>(
                    event.getDataObject());
            ASSERT_NE(nullptr, info);
            EXPECT_EQ(42u, info->m_seqNum);
            EXPECT_TRUE(info->m_ready);
            Event::deleteData(event);
        }));

    EXPECT_TRUE(proxy.parseMessage(
        reinterpret_cast<const UInt8*>(kMsgDEnterReady)));
}

TEST(ClientProxyLifecycleTests, clientProxy18FramesEveryInputWithEpochAndSequence)
{
    NiceMock<MockEventQueue> events;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    FileEvents fileEvents;
    ClientProxyEvents clientProxyEvents;
    Event::Type nextType = Event::kLast;
    setClientProxy16EventDefaults(events, streamEvents, clipboardEvents,
                                  fileEvents, nextType);
    clientProxyEvents.setEvents(&events);
    ON_CALL(events, forClientProxy()).WillByDefault(ReturnRef(clientProxyEvents));

    RecordingStream* stream = new RecordingStream();
    NiceMock<MockServer> server;
    ClientProxy1_8 proxy("client", stream, &server, &events);

    stream->clear();
    proxy.enter(10, 20, 42, 0, false);
    SInt16 x = 0;
    SInt16 y = 0;
    UInt32 epoch = 0;
    UInt16 mask = 0;
    ASSERT_TRUE(ProtocolUtil::readf(stream, kMsgCEnter,
                                    &x, &y, &epoch, &mask));
    EXPECT_EQ(42u, epoch);

    stream->clear();
    proxy.mouseMove(30, 40);
    UInt32 sequence = 0;
    ASSERT_TRUE(ProtocolUtil::readf(stream, kMsgDMouseMove1_8,
                                    &epoch, &sequence, &x, &y));
    EXPECT_EQ(42u, epoch);
    EXPECT_EQ(1u, sequence);
    EXPECT_EQ(30, x);
    EXPECT_EQ(40, y);

    stream->clear();
    proxy.mouseDown(kButtonLeft);
    UInt8 button = 0;
    ASSERT_TRUE(ProtocolUtil::readf(stream, kMsgDMouseDown1_8,
                                    &epoch, &sequence, &button));
    EXPECT_EQ(42u, epoch);
    EXPECT_EQ(2u, sequence);
    EXPECT_EQ(kButtonLeft, button);

    stream->clear();
    proxy.keyDownBroadcast(7, KeyModifierControl, 9);
    UInt8 flags = 0;
    UInt16 key = 0;
    UInt16 keyMask = 0;
    UInt16 keyButton = 0;
    ASSERT_TRUE(ProtocolUtil::readf(stream, kMsgDKeyDown1_8,
                                    &epoch, &sequence, &flags,
                                    &key, &keyMask, &keyButton));
    EXPECT_EQ(42u, epoch);
    EXPECT_EQ(3u, sequence);
    EXPECT_EQ(kInputMessageBroadcast, flags);
    EXPECT_EQ(7u, key);
    EXPECT_EQ(KeyModifierControl, keyMask);
    EXPECT_EQ(9u, keyButton);

    stream->clear();
    proxy.mouseWheel(-120, 120);
    SInt16 xDelta = 0;
    SInt16 yDelta = 0;
    ASSERT_TRUE(ProtocolUtil::readf(stream, kMsgDMouseWheel1_8,
                                    &epoch, &sequence, &xDelta, &yDelta));
    EXPECT_EQ(42u, epoch);
    EXPECT_EQ(4u, sequence);
    EXPECT_EQ(-120, xDelta);
    EXPECT_EQ(120, yDelta);
}

TEST(ClientProxyLifecycleTests, clientProxy16CleanupRequestsCancelWithoutWaiting)
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
    ClientProxy1_6 proxy("client", stream, &server, &events);

    std::atomic<bool> started(false);
    std::atomic<bool> release(false);
    proxy.testSetClipboardSendThread(new Thread([&started, &release]() {
        started.store(true);
        while (!release.load()) {
        }
    }));
    while (!started.load()) {
        ARCH->sleep(0.001);
    }

    Stopwatch elapsed;
    EXPECT_FALSE(proxy.cleanupClipboardSendThread(true));
    EXPECT_LT(elapsed.getTime(), 0.1);

    release.store(true);
    for (int i = 0; i < 100 && !proxy.cleanupClipboardSendThread(false); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(proxy.cleanupClipboardSendThread(false));
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
    for (int i = 0; i < 200 && !proxy.cleanupClipboardSendThread(true); ++i) {
        ARCH->sleep(0.001);
    }
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
    for (int i = 0; i < 200 && !proxy.cleanupClipboardSendThread(false); ++i) {
        ARCH->sleep(0.001);
    }
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
