#define BARRIER_TEST_ENV
#include "client/Client.h"
#include "client/ServerProxy.h"

#include "barrier/ClientArgs.h"
#include "barrier/Clipboard.h"
#include "barrier/ClipboardChunk.h"
#include "barrier/FileChunk.h"
#include "barrier/IPlatformScreen.h"
#include "barrier/RemoteFileClipboard.h"
#include "barrier/StreamChunker.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "barrier/Screen.h"
#include "arch/Arch.h"
#include "base/Stopwatch.h"
#include "base/EventTypes.h"
#include "io/filesystem.h"
#include "mt/Thread.h"
#include "mt/XThread.h"
#include "net/ISocketFactory.h"
#include "net/NetworkAddress.h"
#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/io/MockStream.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

bool testClientPrepareTransferSource(const char* filename,
                                     barrier::fs::path& sourcePath,
                                     barrier::fs::path& tempPackagePath,
                                     std::string& error);

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::AtLeast;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {

std::string invalidTransferPackageData()
{
    return std::string("BDIRPKG1F", 9);
}

class DummySocketFactory : public ISocketFactory {
public:
    IDataSocket* create(IArchNetwork::EAddressFamily,
                        ConnectionSecurityLevel) const override
    {
        return NULL;
    }

    IListenSocket* createListen(IArchNetwork::EAddressFamily,
                                ConnectionSecurityLevel) const override
    {
        return NULL;
    }
};

class TestScreen : public barrier::Screen {
public:
    TestScreen() : barrier::Screen() { }

    void* getEventTarget() const override
    {
        return const_cast<TestScreen*>(this);
    }
};

class EnterPlatformScreen : public IPlatformScreen
{
public:
    EnterPlatformScreen() :
        IPlatformScreen(NULL),
        enterCount(0),
        leaveCount(0),
        mouseMoveCount(0),
        mouseRelativeMoveCount(0),
        mouseWheelCount(0),
        mouseDownCount(0),
        mouseUpCount(0),
        keyDownCount(0),
        keyRepeatCount(0),
        keyUpCount(0),
        sequenceNumber(0),
        setClipboardCount(0),
        getClipboardCount(0),
        lastSetClipboardWasNull(false),
        enterable(true),
        clipboardAvailable(false),
        clipboardContainsFileList(false),
        clipboardText("stable clipboard")
    {
    }

    void enable() override { }
    void disable() override { }
    void enter() override { ++enterCount; }
    bool leave() override
    {
        ++leaveCount;
        return true;
    }
    bool setClipboard(ClipboardID, const IClipboard* clipboard) override
    {
        ++setClipboardCount;
        lastSetClipboardWasNull = (clipboard == NULL);
        if (clipboard != NULL) {
            lastSetClipboard.unmarshall(IClipboard::marshall(clipboard), 0);
        }
        return true;
    }
    void checkClipboards() override { }
    void openScreensaver(bool) override { }
    void closeScreensaver() override { }
    void screensaver(bool) override { }
    void resetOptions() override { }
    void setOptions(const OptionsList&) override { }
    void setSequenceNumber(UInt32 value) override { sequenceNumber = value; }
    void setDraggingStarted(bool) override { }
    bool isPrimary() const override { return false; }
    bool canEnter() const override { return enterable; }
    void* getEventTarget() const override { return const_cast<EnterPlatformScreen*>(this); }
    bool getClipboard(ClipboardID id, IClipboard* clipboard) const override
    {
        ++getClipboardCount;
        if (!clipboardAvailable || id != kClipboardClipboard) {
            return false;
        }
        if (!clipboard->open(42)) {
            return false;
        }
        clipboard->empty();
        if (clipboardContainsFileList) {
            RemoteFileClipboard::Data payload;
            payload.mode = RemoteFileClipboard::Mode::SourcePaths;
            payload.sessionId = "local-source-session";
            payload.paths.push_back(barrier::fs::u8path("C:/local-copy.txt"));
            clipboard->add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
        }
        else {
            clipboard->add(IClipboard::kText, clipboardText);
        }
        clipboard->close();
        return true;
    }
    void getShape(SInt32& x, SInt32& y, SInt32& width, SInt32& height) const override
    {
        x = 0;
        y = 0;
        width = 1024;
        height = 768;
    }
    void getCursorPos(SInt32& x, SInt32& y) const override
    {
        x = 100;
        y = 100;
    }
    void reconfigure(UInt32) override { }
    void warpCursor(SInt32, SInt32) override { }
    UInt32 registerHotKey(KeyID, KeyModifierMask) override { return 0; }
    void unregisterHotKey(UInt32) override { }
    void fakeInputBegin() override { }
    void fakeInputEnd() override { }
    SInt32 getJumpZoneSize() const override { return 0; }
    bool isAnyMouseButtonDown(UInt32&) const override { return false; }
    void getCursorCenter(SInt32& x, SInt32& y) const override
    {
        x = 512;
        y = 384;
    }
    void fakeMouseButton(ButtonID, bool press) override
    {
        if (press) {
            ++mouseDownCount;
        }
        else {
            ++mouseUpCount;
        }
    }
    void fakeMouseMove(SInt32, SInt32) override { ++mouseMoveCount; }
    void fakeMouseRelativeMove(SInt32, SInt32) const override { ++mouseRelativeMoveCount; }
    void fakeMouseWheel(SInt32, SInt32) const override { ++mouseWheelCount; }
    void updateKeyMap() override { }
    void updateKeyState() override { }
    void setHalfDuplexMask(KeyModifierMask) override { }
    void fakeKeyDown(KeyID, KeyModifierMask, KeyButton) override { ++keyDownCount; }
    bool fakeKeyRepeat(KeyID, KeyModifierMask, SInt32, KeyButton) override
    {
        ++keyRepeatCount;
        return true;
    }
    bool fakeKeyUp(KeyButton) override
    {
        ++keyUpCount;
        return true;
    }
    void fakeAllKeysUp() override { }
    bool fakeCtrlAltDel() override { return false; }
    bool isKeyDown(KeyButton) const override { return false; }
    KeyModifierMask getActiveModifiers() const override { return 0; }
    KeyModifierMask pollActiveModifiers() const override { return 0; }
    SInt32 pollActiveGroup() const override { return 0; }
    void pollPressedKeys(KeyButtonSet&) const override { }
    String& getDraggingFilename() override { return draggingFilename; }
    void clearDraggingFilename() override { draggingFilename.clear(); }
    bool isDraggingStarted() override { return false; }
    bool isFakeDraggingStarted() override { return false; }
    void fakeDraggingFiles(DragFileList) override { }
    const String& getDropTarget() const override { return dropTarget; }
    void setDropTarget(const String& target) override { dropTarget = target; }
    void handleSystemEvent(const Event&, void*) override { }

    UInt32 enterCount;
    UInt32 leaveCount;
    UInt32 mouseMoveCount;
    mutable UInt32 mouseRelativeMoveCount;
    mutable UInt32 mouseWheelCount;
    UInt32 mouseDownCount;
    UInt32 mouseUpCount;
    UInt32 keyDownCount;
    UInt32 keyRepeatCount;
    UInt32 keyUpCount;
    UInt32 sequenceNumber;
    UInt32 setClipboardCount;
    mutable UInt32 getClipboardCount;
    bool lastSetClipboardWasNull;
    bool enterable;
    Clipboard lastSetClipboard;
    bool clipboardAvailable;
    bool clipboardContainsFileList;
    std::string clipboardText;
    String draggingFilename;
    String dropTarget;
};

class CountingStream : public barrier::IStream {
public:
    explicit CountingStream(UInt32* deletedCount) :
        m_deletedCount(deletedCount)
    {
    }

    ~CountingStream()
    {
        ++(*m_deletedCount);
    }

    void close() override { }
    UInt32 read(void*, UInt32) override { return 0; }
    void write(const void*, UInt32) override { }
    void writeLowPriority(const void*, UInt32) override { }
    void flush() override { }
    void shutdownInput() override { }
    void shutdownOutput() override { }
    void* getEventTarget() const override { return const_cast<CountingStream*>(this); }
    bool isReady() const override { return false; }
    UInt32 getSize() const override { return 0; }
    UInt32 getBufferedOutputSize() const override { return 0; }

private:
    UInt32* m_deletedCount;
};

class ScriptedStream : public barrier::IStream {
public:
    void clearData()
    {
        m_data.clear();
        m_offset = 0;
    }

    void close() override { }
    UInt32 read(void* buffer, UInt32 count) override
    {
        const UInt32 remaining = getSize();
        const UInt32 copied = count < remaining ? count : remaining;
        if (copied != 0) {
            std::memcpy(buffer, &m_data[m_offset], copied);
            m_offset += copied;
        }
        return copied;
    }
    void write(const void* buffer, UInt32 count) override
    {
        const UInt8* bytes = static_cast<const UInt8*>(buffer);
        m_data.insert(m_data.end(), bytes, bytes + count);
    }
    void writeLowPriority(const void* buffer, UInt32 count) override { write(buffer, count); }
    void flush() override { }
    void shutdownInput() override { }
    void shutdownOutput() override { }
    void* getEventTarget() const override { return const_cast<ScriptedStream*>(this); }
    bool isReady() const override { return getSize() != 0; }
    UInt32 getSize() const override
    {
        return static_cast<UInt32>(m_data.size() - m_offset);
    }
    UInt32 getBufferedOutputSize() const override { return 0; }

private:
    std::vector<UInt8> m_data;
    std::size_t m_offset = 0;
};

class DeferringServerProxy : public ServerProxy {
public:
    DeferringServerProxy(Client* client,
                         barrier::IStream* stream,
                         IEventQueue* events,
                         UInt32* deletedCount) :
        ServerProxy(client, stream, events),
        cleanupAllowed(false),
        cleanupCalls(0),
        m_deletedCount(deletedCount)
    {
    }

    ~DeferringServerProxy()
    {
        ++(*m_deletedCount);
    }

    bool cleanupClipboardSendThread(bool) override
    {
        ++cleanupCalls;
        return cleanupAllowed;
    }

    bool cleanupAllowed;
    UInt32 cleanupCalls;

private:
    UInt32* m_deletedCount;
};

class PendingClipboardServerProxy : public ServerProxy {
public:
    PendingClipboardServerProxy(Client* client,
                                barrier::IStream* stream,
                                IEventQueue* events) :
        ServerProxy(client, stream, events),
        result(kClipboardSendPending),
        reapReady(false),
        reapSucceeded(false),
        sendCalls(0),
        reapCalls(0)
    {
    }

    ClipboardSendResult onClipboardChanged(ClipboardID, const IClipboard* clipboard) override
    {
        ++sendCalls;
        lastClipboard.unmarshall(IClipboard::marshall(clipboard), 0);
        return result;
    }

    bool cleanupClipboardSendThread(bool) override
    {
        return true;
    }

    bool reapClipboardSendResult(ClipboardID, bool& succeeded) override
    {
        ++reapCalls;
        if (!reapReady) {
            return false;
        }
        succeeded = reapSucceeded;
        return true;
    }

    ClipboardSendResult result;
    bool reapReady;
    bool reapSucceeded;
    UInt32 sendCalls;
    UInt32 reapCalls;
    Clipboard lastClipboard;
};

class StableTextClipboardScreen : public TestScreen {
public:
    bool getClipboard(ClipboardID id, IClipboard* clipboard) const override
    {
        if (id != kClipboardClipboard) {
            return false;
        }
        if (!clipboard->open(30)) {
            return false;
        }
        clipboard->empty();
        clipboard->add(IClipboard::kText, "stable clipboard");
        clipboard->close();
        return true;
    }
};

class FileListClipboardScreen : public TestScreen {
public:
    FileListClipboardScreen() :
        mode(RemoteFileClipboard::Mode::SourcePaths)
    {
    }

    bool getClipboard(ClipboardID id, IClipboard* clipboard) const override
    {
        if (id != kClipboardClipboard) {
            return false;
        }

        RemoteFileClipboard::Data payload;
        payload.mode = mode;
        payload.sessionId = "local-source-session";
        payload.paths.push_back(barrier::fs::u8path("/tmp/local-copy.txt"));

        if (!clipboard->open(10)) {
            return false;
        }
        clipboard->empty();
        clipboard->add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
        clipboard->close();
        return true;
    }

    RemoteFileClipboard::Mode mode;
};

class ImageFileListClipboardScreen : public TestScreen {
public:
    bool getClipboard(ClipboardID id, IClipboard* clipboard) const override
    {
        if (id != kClipboardClipboard) {
            return false;
        }

        RemoteFileClipboard::Data payload;
        payload.mode = RemoteFileClipboard::Mode::SourcePaths;
        payload.sessionId = "local-image-source-session";
        payload.paths.push_back(barrier::fs::u8path("/tmp/local-photo.png"));

        if (!clipboard->open(11)) {
            return false;
        }
        clipboard->empty();
        clipboard->add(IClipboard::kPNG, "fake-png");
        clipboard->add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
        clipboard->add(IClipboard::kText, "/tmp/local-photo.png\n");
        clipboard->close();
        return true;
    }
};

class FlakyTextClipboardScreen : public TestScreen {
public:
    FlakyTextClipboardScreen() :
        calls(0)
    {
    }

    bool getClipboard(ClipboardID id, IClipboard* clipboard) const override
    {
        ++calls;
        if (id != kClipboardClipboard || calls == 1) {
            return false;
        }

        if (!clipboard->open(20)) {
            return false;
        }
        clipboard->empty();
        clipboard->add(IClipboard::kText, "real clipboard");
        clipboard->close();
        return true;
    }

    mutable UInt32 calls;
};

class TextThenEmptyClipboardScreen : public TestScreen {
public:
    TextThenEmptyClipboardScreen() :
        calls(0)
    {
    }

    bool getClipboard(ClipboardID id, IClipboard* clipboard) const override
    {
        ++calls;
        if (id != kClipboardClipboard || !clipboard->open(20 + calls)) {
            return false;
        }

        clipboard->empty();
        if (calls == 1) {
            clipboard->add(IClipboard::kText, "sensitive text");
        }
        clipboard->close();
        return true;
    }

    mutable UInt32 calls;
};

class FileListThenTextClipboardScreen : public TestScreen {
public:
    FileListThenTextClipboardScreen() :
        calls(0)
    {
    }

    bool getClipboard(ClipboardID id, IClipboard* clipboard) const override
    {
        ++calls;
        if (id != kClipboardClipboard) {
            return false;
        }

        if (!clipboard->open(10 + calls)) {
            return false;
        }
        clipboard->empty();
        if (calls == 1) {
            RemoteFileClipboard::Data payload;
            payload.mode = RemoteFileClipboard::Mode::SourcePaths;
            payload.sessionId = "local-source-session";
            payload.paths.push_back(barrier::fs::u8path("/tmp/local-copy.txt"));
            clipboard->add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
        }
        else {
            clipboard->add(IClipboard::kText, "plain text after file-list");
        }
        clipboard->close();
        return true;
    }

    mutable UInt32 calls;
};

void setClientEventDefaults(MockEventQueue& events,
                            ClientEvents& clientEvents,
                            IScreenEvents& screenEvents,
                            FileEvents& fileEvents)
{
    clientEvents.setEvents(&events);
    screenEvents.setEvents(&events);
    fileEvents.setEvents(&events);

    ON_CALL(events, forClient()).WillByDefault(ReturnRef(clientEvents));
    ON_CALL(events, forIScreen()).WillByDefault(ReturnRef(screenEvents));
    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, registerTypeOnce(_, _)).WillByDefault(Return(100));
}

void setConnectedClientEventDefaults(MockEventQueue& events,
                                     ClientEvents& clientEvents,
                                     IScreenEvents& screenEvents,
                                     FileEvents& fileEvents,
                                     IStreamEvents& streamEvents,
                                     ClipboardEvents& clipboardEvents,
                                     IDataSocketEvents& dataSocketEvents,
                                     ISocketEvents& socketEvents)
{
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);
    streamEvents.setEvents(&events);
    clipboardEvents.setEvents(&events);
    dataSocketEvents.setEvents(&events);
    socketEvents.setEvents(&events);

    ON_CALL(events, forIStream()).WillByDefault(ReturnRef(streamEvents));
    ON_CALL(events, forClipboard()).WillByDefault(ReturnRef(clipboardEvents));
    ON_CALL(events, forIDataSocket()).WillByDefault(ReturnRef(dataSocketEvents));
    ON_CALL(events, forISocket()).WillByDefault(ReturnRef(socketEvents));
    ON_CALL(events, newOneShotTimer(_, _)).WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));
}

}

TEST(ClientDisconnectTests, disconnectWithoutMessageIsIdempotent)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    EXPECT_CALL(events, addEvent(_)).Times(1);

    client.disconnect(NULL);
    client.disconnect(NULL);
}

TEST(ClientDisconnectTests, setupConnectingRegistersStopRetryBeforeSecureConnectCompletes)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents,
                                    dataSocketEvents, socketEvents);

    TestScreen screen;
    ClientArgs args;
    args.m_enableCrypto = true;
    NiceMock<MockStream> stream;
    ON_CALL(stream, getEventTarget()).WillByDefault(Return(&stream));

    EXPECT_CALL(events, adoptHandler(_, _, _)).Times(AnyNumber());
    EXPECT_CALL(events, removeHandler(_, _)).Times(AnyNumber());
    EXPECT_CALL(events, adoptHandler(_, &stream, _)).Times(3);
    EXPECT_CALL(events, removeHandler(_, &stream)).Times(4);

    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    client.testSetupConnecting(&stream);
    client.testCleanupConnecting();
    client.testSetStreamOnly(NULL);
}

TEST(ClientDisconnectTests, setClipboardDoesNotPublishSourcePathsToSystemClipboard)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.sessionId = "remote-source-session";
    payload.paths.push_back(barrier::fs::u8path("/tmp/remote-source.txt"));

    Clipboard clipboard;
    ASSERT_TRUE(clipboard.open(0));
    clipboard.empty();
    clipboard.add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
    clipboard.close();

    client.setClipboard(kClipboardClipboard, &clipboard);

    EXPECT_EQ(0u, platform->setClipboardCount);
    EXPECT_EQ("remote-source-session", client.testRemoteFileClipboardSession());
}

TEST(ClientDisconnectTests, repeatedSourcePathsMetadataKeepsClipboardRevision)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.sessionId = "remote-source-session";
    payload.paths.push_back(barrier::fs::u8path("/tmp/remote-source.txt"));

    Clipboard clipboard;
    ASSERT_TRUE(clipboard.open(0));
    clipboard.empty();
    clipboard.add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
    clipboard.close();

    client.setClipboard(kClipboardClipboard, &clipboard);
    const std::uint64_t firstRevision = client.testClipboardRevisionSequence();
    client.setClipboard(kClipboardClipboard, &clipboard);

    EXPECT_NE(0u, firstRevision);
    EXPECT_EQ(firstRevision, client.testClipboardRevisionSequence());
    EXPECT_EQ("remote-source-session", client.testRemoteFileClipboardSession());
    EXPECT_EQ(0u, platform->setClipboardCount);
}

TEST(ClientDisconnectTests, setClipboardStillPublishesPlainText)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    client.testSetFileClipboardSessions("stale-source", "stale-ready",
                                        std::vector<std::string>(1, "/tmp/stale"));

    Clipboard clipboard;
    ASSERT_TRUE(clipboard.open(0));
    clipboard.empty();
    clipboard.add(IClipboard::kText, "plain text");
    clipboard.close();

    client.setClipboard(kClipboardClipboard, &clipboard);

    EXPECT_EQ(1u, platform->setClipboardCount);
    EXPECT_FALSE(platform->lastSetClipboardWasNull);
    ASSERT_TRUE(platform->lastSetClipboard.open(0));
    ASSERT_TRUE(platform->lastSetClipboard.has(IClipboard::kText));
    EXPECT_EQ("plain text", platform->lastSetClipboard.get(IClipboard::kText));
    platform->lastSetClipboard.close();
    EXPECT_TRUE(client.testRemoteFileClipboardSession().empty());
    EXPECT_TRUE(client.testReadyFileClipboardSession().empty());
    EXPECT_TRUE(client.testReadyFileClipboardPaths().empty());
}

TEST(ClientDisconnectTests, localClipboardGrabDoesNotSendUntilClipboardIsExplicitlySynced)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    StableTextClipboardScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);

    client.testHandleClipboardGrabbed(kClipboardClipboard);

    EXPECT_TRUE(client.testOwnClipboard(kClipboardClipboard));
    EXPECT_FALSE(client.testClipboardSent(kClipboardClipboard));
    EXPECT_EQ(0u, proxy->sendCalls);

    proxy->result = ServerProxy::kClipboardSendQueued;
    client.testSendClipboard(kClipboardClipboard);

    EXPECT_EQ(1u, proxy->sendCalls);
    EXPECT_TRUE(client.testClipboardSent(kClipboardClipboard));
}

TEST(ClientDisconnectTests, remoteClipboardDoesNotOverwriteUnsentLocalClipboard)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    platform->clipboardAvailable = true;
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);

    client.testHandleClipboardGrabbed(kClipboardClipboard);

    Clipboard remoteClipboard;
    ASSERT_TRUE(remoteClipboard.open(0));
    remoteClipboard.empty();
    remoteClipboard.add(IClipboard::kText, "remote clipboard");
    remoteClipboard.close();

    client.setClipboard(kClipboardClipboard, &remoteClipboard);

    EXPECT_TRUE(client.testOwnClipboard(kClipboardClipboard));
    EXPECT_FALSE(client.testClipboardSent(kClipboardClipboard));
    EXPECT_EQ(0u, platform->setClipboardCount);
    EXPECT_EQ(0u, proxy->sendCalls);

    proxy->result = ServerProxy::kClipboardSendQueued;
    client.testSendClipboard(kClipboardClipboard);
    EXPECT_EQ(1u, proxy->sendCalls);
    EXPECT_TRUE(client.testClipboardSent(kClipboardClipboard));

    client.setClipboard(kClipboardClipboard, &remoteClipboard);

    EXPECT_FALSE(client.testOwnClipboard(kClipboardClipboard));
    EXPECT_EQ(1u, platform->setClipboardCount);
    ASSERT_TRUE(platform->lastSetClipboard.open(0));
    ASSERT_TRUE(platform->lastSetClipboard.has(IClipboard::kText));
    EXPECT_EQ("remote clipboard", platform->lastSetClipboard.get(IClipboard::kText));
    platform->lastSetClipboard.close();
}

TEST(ClientDisconnectTests, invalidFileCompletionReleasesReceiveState)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    ASSERT_TRUE(client.getFileReceiveSession().begin(1024, 0, 0));
    ASSERT_TRUE(client.getFileReceiveSession().append("partial"));
    const barrier::fs::path spoolPath = client.getFileReceiveSession().spoolPath();

    client.testOnFileRecieveCompleted();

    EXPECT_EQ(0u, client.getFileReceiveSession().expectedSize());
    EXPECT_TRUE(client.getFileReceiveSession().data().empty());
    EXPECT_TRUE(client.getFileReceiveSession().spoolPath().empty());
    EXPECT_FALSE(barrier::fs::exists(spoolPath));
}

TEST(ClientDisconnectTests, staleFileCompletionDoesNotResetNewReceive)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    ASSERT_TRUE(client.getFileReceiveSession().begin(3, 1024, 1024));
    const std::uint64_t staleGeneration =
        client.getFileReceiveSession().generation();
    ASSERT_TRUE(client.getFileReceiveSession().append("old"));
    ASSERT_TRUE(client.getFileReceiveSession().finish());

    ASSERT_TRUE(client.getFileReceiveSession().begin(4, 1024, 1024));
    const std::uint64_t currentGeneration =
        client.getFileReceiveSession().generation();
    client.testOnFileRecieveCompleted(staleGeneration);

    EXPECT_GT(currentGeneration, staleGeneration);
    EXPECT_TRUE(client.getFileReceiveSession().matchesGeneration(currentGeneration));
    EXPECT_EQ(FileReceiveSession::kReceiving,
              client.getFileReceiveSession().state());
    EXPECT_EQ(4u, client.getFileReceiveSession().expectedSize());
}

TEST(ClientDisconnectTests, cleanupConnectionReleasesPartialReceiveSpool)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    ASSERT_TRUE(client.getFileReceiveSession().begin(1024, 0, 0));
    ASSERT_TRUE(client.getFileReceiveSession().append("partial"));
    const barrier::fs::path spoolPath = client.getFileReceiveSession().spoolPath();

    client.testCleanupConnection();

    EXPECT_EQ(0u, client.getFileReceiveSession().expectedSize());
    EXPECT_TRUE(client.getFileReceiveSession().data().empty());
    EXPECT_TRUE(client.getFileReceiveSession().spoolPath().empty());
    EXPECT_FALSE(barrier::fs::exists(spoolPath));
}

TEST(ClientDisconnectTests, cleanupSendFileThreadReleasesDetachedStream)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 deletedCount = 0;
    client.testSetDetachedSendFileStream(new CountingStream(&deletedCount));
    client.testSetDetachedSendFileStream(new CountingStream(&deletedCount));

    EXPECT_TRUE(client.testCleanupSendFileThread(true));
    EXPECT_EQ(2u, deletedCount);
}

TEST(ClientDisconnectTests, cleanupSendFileThreadRequestsCancelWithoutWaiting)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    std::atomic<bool> started(false);
    std::atomic<bool> release(false);
    client.testSetSendFileThread(new Thread([&started, &release]() {
        started.store(true);
        while (!release.load()) {
        }
    }));
    while (!started.load()) {
        ARCH->sleep(0.001);
    }

    Stopwatch elapsed;
    EXPECT_FALSE(client.testCleanupSendFileThread(true));
    EXPECT_LT(elapsed.getTime(), 0.1);

    release.store(true);
    for (int i = 0; i < 100 && client.testHasSendFileThread(); ++i) {
        client.testCleanupSendFileThread(false);
        ARCH->sleep(0.001);
    }
    EXPECT_FALSE(client.testHasSendFileThread());
}

TEST(ClientDisconnectTests, fileReceiveCompleteDoesNotBlockOnBusyDropDirWriter)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    std::atomic<bool> releaseWriter(false);
    std::atomic<bool> writerCancelled(false);
    client.testSetWriteToDropDirThread(new Thread([&releaseWriter, &writerCancelled]() {
        try {
            while (!releaseWriter.load()) {
                ARCH->sleep(0.01);
                Thread::testCancel();
            }
        }
        catch (XThread&) {
            writerCancelled.store(true);
            throw;
        }
    }));

    client.testStartDropDirTransfer("data");

    EXPECT_FALSE(writerCancelled.load());
    EXPECT_TRUE(client.testHasWriteToDropDirThread());
    EXPECT_EQ(1u, client.testPendingDropDirTransferCount());

    releaseWriter.store(true);
    for (int i = 0; i < 200 && client.testHasWriteToDropDirThread(); ++i) {
        client.testCleanupWriteToDropDirThread();
        ARCH->sleep(0.001);
    }
    ASSERT_FALSE(client.testHasWriteToDropDirThread());
    client.testDrainDropDirTransferQueue();
    EXPECT_EQ(0u, client.testPendingDropDirTransferCount());
    EXPECT_TRUE(client.testHasWriteToDropDirThread());
    for (int i = 0; i < 200 && client.testHasWriteToDropDirThread(); ++i) {
        client.testCleanupWriteToDropDirThread();
        ARCH->sleep(0.001);
    }
    EXPECT_FALSE(client.testHasWriteToDropDirThread());
}

TEST(ClientDisconnectTests, dropDirWriterQueueCapsBufferedMemory)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    const std::string payload(17 * 1024 * 1024, 'x');
    client.testQueueDropDirTransfer(payload);
    client.testQueueDropDirTransfer(payload);

    EXPECT_EQ(1u, client.testPendingDropDirTransferCount());
}

TEST(ClientDisconnectTests, cleanupScreenDefersServerProxyUntilClipboardSenderStops)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    UInt32 proxyDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    DeferringServerProxy* proxy =
        new DeferringServerProxy(&client, stream, &events, &proxyDeletedCount);
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);

    client.testCleanupScreen();

    EXPECT_EQ(0u, proxyDeletedCount);
    EXPECT_EQ(0u, streamDeletedCount);
    EXPECT_EQ(1u, client.testDetachedServerProxyCount());
    EXPECT_EQ(1u, client.testDetachedSendFileStreamCount());
    EXPECT_GE(proxy->cleanupCalls, 1u);

    proxy->cleanupAllowed = true;
    client.testReleaseDetachedServerProxies();

    EXPECT_EQ(1u, proxyDeletedCount);
    EXPECT_EQ(1u, streamDeletedCount);
    EXPECT_EQ(0u, client.testDetachedServerProxyCount());
    EXPECT_EQ(0u, client.testDetachedSendFileStreamCount());
}

TEST(ClientDisconnectTests, fileKeepAliveReapsDetachedServerProxyAfterClipboardSenderStops)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    UInt32 proxyDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    DeferringServerProxy* proxy =
        new DeferringServerProxy(&client, stream, &events, &proxyDeletedCount);
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);

    client.testCleanupScreen();
    ASSERT_EQ(1u, client.testDetachedServerProxyCount());
    ASSERT_EQ(1u, client.testDetachedSendFileStreamCount());

    proxy->cleanupAllowed = true;
    client.testHandleFileKeepAlive();

    EXPECT_EQ(1u, proxyDeletedCount);
    EXPECT_EQ(1u, streamDeletedCount);
    EXPECT_EQ(0u, client.testDetachedServerProxyCount());
    EXPECT_EQ(0u, client.testDetachedSendFileStreamCount());
}

TEST(ClientDisconnectTests, reapSendFileThreadReleasesDetachedStreamWhenThreadFinishes)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    client.testSetDetachedSendFileStream(new CountingStream(&streamDeletedCount));

    std::atomic<bool> releaseSender(false);
    client.testSetSendFileThread(new Thread([&releaseSender]() {
        while (!releaseSender.load()) {
            ARCH->sleep(0.01);
            Thread::testCancel();
        }
    }));

    EXPECT_FALSE(client.testReapSendFileThreadIfReady());
    EXPECT_EQ(0u, streamDeletedCount);

    releaseSender.store(true);
    bool reaped = false;
    for (int i = 0; i < 100 && !reaped; ++i) {
        reaped = client.testReapSendFileThreadIfReady();
        if (!reaped) {
            ARCH->sleep(0.01);
        }
    }

    EXPECT_TRUE(reaped);
    EXPECT_EQ(1u, streamDeletedCount);
    EXPECT_EQ(0u, client.testDetachedSendFileStreamCount());
}

TEST(ClientDisconnectTests, enterInterruptsFileSenderWithoutLosingThreadHandle)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    EnterPlatformScreen* platformScreen = new EnterPlatformScreen();
    barrier::Screen screen(platformScreen, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    std::atomic<bool> releaseSender(false);
    client.testSetSendFileChunker(std::make_shared<StreamChunker>());
    client.testSetSendFileThread(new Thread([&releaseSender]() {
        while (!releaseSender.load()) {
            ARCH->sleep(0.01);
            Thread::testCancel();
        }
    }));

    client.enter(10, 20, 0, 0, false);

    EXPECT_EQ(1u, platformScreen->enterCount);
    EXPECT_EQ(1u, platformScreen->mouseMoveCount);
    EXPECT_TRUE(client.testHasSendFileThread());
    EXPECT_FALSE(client.testReapSendFileThreadIfReady());

    releaseSender.store(true);
    bool reaped = false;
    for (int i = 0; i < 100 && !reaped; ++i) {
        reaped = client.testReapSendFileThreadIfReady();
        if (!reaped) {
            ARCH->sleep(0.01);
        }
    }

    EXPECT_TRUE(reaped);
    EXPECT_FALSE(client.testHasSendFileThread());
    EXPECT_TRUE(client.leave());
}

TEST(ClientDisconnectTests, enterSynchronizesClipboardSequenceAfterReconnect)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    client.enter(10, 20, 42, 0, false);

    EXPECT_EQ(42u, platform->sequenceNumber);
    EXPECT_TRUE(client.leave());
}

TEST(ClientDisconnectTests, inactiveClientRejectsPointerButPreservesKeyboardBroadcast)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    client.keyDown(1, 0, 1);
    client.keyRepeat(1, 0, 1, 1);
    client.keyUp(1, 0, 1);
    client.mouseDown(kButtonLeft);
    client.mouseUp(kButtonLeft);
    client.mouseMove(20, 30);
    client.mouseRelativeMove(2, 3);
    client.mouseWheel(0, 120);

    EXPECT_EQ(1u, platform->keyDownCount);
    EXPECT_EQ(1u, platform->keyRepeatCount);
    EXPECT_EQ(1u, platform->keyUpCount);
    EXPECT_EQ(0u, platform->mouseDownCount);
    EXPECT_EQ(0u, platform->mouseUpCount);
    EXPECT_EQ(0u, platform->mouseMoveCount);
    EXPECT_EQ(0u, platform->mouseRelativeMoveCount);
    EXPECT_EQ(0u, platform->mouseWheelCount);

    client.enter(10, 20, 1, 0, false);
    client.keyDown(1, 0, 1);
    client.keyRepeat(1, 0, 1, 1);
    client.keyUp(1, 0, 1);
    client.mouseDown(kButtonLeft);
    client.mouseUp(kButtonLeft);
    client.mouseMove(20, 30);
    client.mouseRelativeMove(2, 3);
    client.mouseWheel(0, 120);

    EXPECT_EQ(2u, platform->keyDownCount);
    EXPECT_EQ(2u, platform->keyRepeatCount);
    EXPECT_EQ(2u, platform->keyUpCount);
    EXPECT_EQ(1u, platform->mouseDownCount);
    EXPECT_EQ(1u, platform->mouseUpCount);
    EXPECT_EQ(2u, platform->mouseMoveCount);
    EXPECT_EQ(1u, platform->mouseRelativeMoveCount);
    EXPECT_EQ(1u, platform->mouseWheelCount);

    ASSERT_TRUE(client.leave());
    client.keyDown(1, 0, 1);
    client.keyRepeat(1, 0, 1, 1);
    client.keyUp(1, 0, 1);
    client.mouseDown(kButtonLeft);
    client.mouseUp(kButtonLeft);
    client.mouseMove(20, 30);
    client.mouseRelativeMove(2, 3);
    client.mouseWheel(0, 120);

    EXPECT_EQ(3u, platform->keyDownCount);
    EXPECT_EQ(3u, platform->keyRepeatCount);
    EXPECT_EQ(3u, platform->keyUpCount);
    EXPECT_EQ(1u, platform->mouseDownCount);
    EXPECT_EQ(1u, platform->mouseUpCount);
    EXPECT_EQ(2u, platform->mouseMoveCount);
    EXPECT_EQ(1u, platform->mouseRelativeMoveCount);
    EXPECT_EQ(1u, platform->mouseWheelCount);
}

TEST(ClientDisconnectTests, disconnectRevokesPointerLeaseBeforeReconnect)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    ScriptedStream firstStream;
    client.testSetServerProxy(new ServerProxy(&client, &firstStream, &events));
    client.handshakeComplete();
    client.enter(10, 20, 1, 0, false);
    ASSERT_EQ(1u, platform->enterCount);
    ASSERT_EQ(1u, platform->mouseMoveCount);

    client.disconnect(NULL);
    EXPECT_EQ(1u, platform->leaveCount);

    client.mouseMove(30, 40);
    client.mouseDown(kButtonLeft);
    EXPECT_EQ(1u, platform->mouseMoveCount);
    EXPECT_EQ(0u, platform->mouseDownCount);

    ScriptedStream secondStream;
    client.testSetServerProxy(new ServerProxy(&client, &secondStream, &events));
    client.handshakeComplete();
    client.enter(50, 60, 2, 0, false);
    EXPECT_EQ(2u, platform->enterCount);
    EXPECT_EQ(2u, platform->mouseMoveCount);

    client.testCleanupScreen();
    client.testCleanupConnection();
}

TEST(ClientDisconnectTests, staleEnterSequenceDoesNotReactivateClient)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    ScriptedStream stream;
    ServerProxy proxy(&client, &stream, &events);

    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 10, 0);
    proxy.enter();
    proxy.leave();

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 30, 40, 9, 0);
    proxy.enter();

    EXPECT_EQ(1u, platform->enterCount);
    EXPECT_EQ(1u, platform->leaveCount);
}

TEST(ClientDisconnectTests, newerEnterAcrossSequenceWrapReplacesLeaseWithoutStaleMotion)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    ScriptedStream stream;
    ServerProxy proxy(&client, &stream, &events);

    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 0xffffffffu, 0);
    proxy.enter();

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 30, 40, 0xffffffffu, 0);
    proxy.enter();
    EXPECT_EQ(1u, platform->enterCount);
    EXPECT_EQ(0u, platform->leaveCount);

    proxy.m_compressMouse = true;
    proxy.m_xMouse = 500;
    proxy.m_yMouse = 500;
    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 50, 60, 0, 0);
    proxy.enter();

    EXPECT_EQ(2u, platform->enterCount);
    EXPECT_EQ(1u, platform->leaveCount);
    EXPECT_EQ(2u, platform->mouseMoveCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgDMouseDown + 4, kButtonLeft);
    proxy.mouseDown();
    EXPECT_EQ(1u, platform->mouseDownCount);

    proxy.leave();
    proxy.leave();
    EXPECT_EQ(2u, platform->leaveCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgDMouseUp + 4, kButtonLeft);
    proxy.mouseUp();
    EXPECT_EQ(0u, platform->mouseUpCount);
}

TEST(ClientDisconnectTests, transactionalPrepareDoesNotEnterUntilMatchingCommit)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    ScriptedStream stream;
    ServerProxy proxy(&client, &stream, &events, 7);
    client.handshakeComplete();

    ProtocolUtil::writef(&stream, kMsgCPrepareEnter + 4, 10, 20, 42, 0);
    proxy.prepareEnter();

    UInt32 readySeqNum = 0;
    UInt8 ready = 0;
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDEnterReady,
                                    &readySeqNum, &ready));
    EXPECT_EQ(42u, readySeqNum);
    EXPECT_EQ(1u, ready);
    EXPECT_EQ(0u, platform->enterCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 42, 0);
    proxy.enter();

    EXPECT_EQ(1u, platform->enterCount);
    EXPECT_EQ(1u, platform->mouseMoveCount);
}

TEST(ClientDisconnectTests, rejectedTransactionalPrepareBlocksCommitUntilAbort)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    platform->enterable = false;
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    ScriptedStream stream;
    ServerProxy proxy(&client, &stream, &events, 7);
    client.handshakeComplete();

    ProtocolUtil::writef(&stream, kMsgCPrepareEnter + 4, 10, 20, 43, 0);
    proxy.prepareEnter();

    UInt32 readySeqNum = 0;
    UInt8 ready = 1;
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDEnterReady,
                                    &readySeqNum, &ready));
    EXPECT_EQ(43u, readySeqNum);
    EXPECT_EQ(0u, ready);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 43, 0);
    proxy.enter();
    EXPECT_EQ(0u, platform->enterCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCAbortEnter + 4, 43);
    proxy.abortEnter();
    EXPECT_FALSE(proxy.m_hasPreparedEnter);
}

TEST(ClientDisconnectTests, protocol18RejectsStaleEpochAndReplayedInput)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    ScriptedStream stream;
    ServerProxy proxy(&client, &stream, &events, 8);

    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 100, 0);
    proxy.enter();
    ASSERT_EQ(1u, platform->mouseMoveCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgDMouseMove + 4, 25, 35);
    EXPECT_EQ(ServerProxy::kUnknown, proxy.parseMessage(
        reinterpret_cast<const UInt8*>(kMsgDMouseMove)));
    EXPECT_EQ(1u, platform->mouseMoveCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8MM%4i%4i%2i%2i" + 4,
                         99, 1, 30, 40);
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8MM")));
    EXPECT_EQ(1u, platform->mouseMoveCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8MM%4i%4i%2i%2i" + 4,
                         100, 10, 30, 40);
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8MM")));
    EXPECT_EQ(2u, platform->mouseMoveCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8MM%4i%4i%2i%2i" + 4,
                         100, 10, 31, 41);
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8MM")));
    EXPECT_EQ(2u, platform->mouseMoveCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8MM%4i%4i%2i%2i" + 4,
                         100, 9, 32, 42);
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8MM")));
    EXPECT_EQ(2u, platform->mouseMoveCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8MM%4i%4i%2i%2i" + 4,
                         100, 11, 33, 43);
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8MM")));
    EXPECT_EQ(3u, platform->mouseMoveCount);
}

TEST(ClientDisconnectTests, protocol17AcceptsLegacyInputFrames)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    ScriptedStream stream;
    ServerProxy proxy(&client, &stream, &events, 7);

    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 100, 0);
    proxy.enter();
    ASSERT_EQ(1u, platform->mouseMoveCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgDMouseMove + 4, 25, 35);
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>(kMsgDMouseMove)));
    EXPECT_EQ(2u, platform->mouseMoveCount);
}

TEST(ClientDisconnectTests, protocol18RequiresActiveLeaseUnlessKeyboardBroadcast)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    ScriptedStream stream;
    ServerProxy proxy(&client, &stream, &events, 8);

    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 200, 0);
    proxy.enter();
    proxy.leave();

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8KD%4i%4i%1i%2i%2i%2i" + 4,
                         200, 1, 0, 1, 0, 1);
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8KD")));
    EXPECT_EQ(0u, platform->keyDownCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8KD%4i%4i%1i%2i%2i%2i" + 4,
                         200, 2, 1, 1, 0, 1);
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8KD")));
    EXPECT_EQ(1u, platform->keyDownCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8KD%4i%4i%1i%2i%2i%2i" + 4,
                         199, 3, 1, 1, 0, 1);
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8KD")));
    EXPECT_EQ(1u, platform->keyDownCount);
}

TEST(ClientDisconnectTests, protocol18AcceptsInputSequenceAfterEpochWrap)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    ScriptedStream stream;
    ServerProxy proxy(&client, &stream, &events, 8);

    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 0xffffffffu, 0);
    proxy.enter();

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8MM%4i%4i%2i%2i" + 4,
                         0xffffffffu, 0xffffffffu, 30, 40);
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8MM")));
    ASSERT_EQ(2u, platform->mouseMoveCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8MM%4i%4i%2i%2i" + 4,
                         0xffffffffu, 0, 31, 41);
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8MM")));
    EXPECT_EQ(3u, platform->mouseMoveCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 50, 60, 0, 0);
    proxy.enter();

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8MM%4i%4i%2i%2i" + 4,
                         0xffffffffu, 1, 32, 42);
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8MM")));
    EXPECT_EQ(4u, platform->mouseMoveCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8MM%4i%4i%2i%2i" + 4,
                         0, 1, 33, 43);
    EXPECT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8MM")));
    EXPECT_EQ(5u, platform->mouseMoveCount);
}

TEST(ClientDisconnectTests, protocol18LeaveReleasesOnlyEpochOwnedPressedInput)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    ScriptedStream stream;
    ServerProxy proxy(&client, &stream, &events, 8);

    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 300, 0);
    proxy.enter();

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8MD%4i%4i%1i" + 4,
                         300, 1, kButtonLeft);
    ASSERT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8MD")));

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8KD%4i%4i%1i%2i%2i%2i" + 4,
                         300, 2, 0, 7, 0, 9);
    ASSERT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8KD")));

    stream.clearData();
    ProtocolUtil::writef(&stream, "D8KD%4i%4i%1i%2i%2i%2i" + 4,
                         300, 3, 1, 8, 0, 10);
    ASSERT_EQ(ServerProxy::kOkay, proxy.parseMessage(
        reinterpret_cast<const UInt8*>("D8KD")));
    ASSERT_EQ(1u, platform->mouseDownCount);
    ASSERT_EQ(2u, platform->keyDownCount);

    proxy.leave();

    EXPECT_EQ(1u, platform->mouseUpCount);
    EXPECT_EQ(1u, platform->keyUpCount);
    EXPECT_EQ(1u, platform->leaveCount);
}

TEST(ClientDisconnectTests, protocolNegotiationFallsBackToPreviousStableMinor)
{
    SInt16 negotiatedMinor = 0;
    EXPECT_TRUE(Client::negotiateProtocolVersion(1, 6, negotiatedMinor));
    EXPECT_EQ(6, negotiatedMinor);
    EXPECT_TRUE(Client::negotiateProtocolVersion(1, 7, negotiatedMinor));
    EXPECT_EQ(7, negotiatedMinor);
    EXPECT_TRUE(Client::negotiateProtocolVersion(1, 9, negotiatedMinor));
    EXPECT_EQ(9, negotiatedMinor);
    EXPECT_TRUE(Client::negotiateProtocolVersion(1, 10, negotiatedMinor));
    EXPECT_EQ(9, negotiatedMinor);
    EXPECT_FALSE(Client::negotiateProtocolVersion(1, 5, negotiatedMinor));
    EXPECT_FALSE(Client::negotiateProtocolVersion(2, 0, negotiatedMinor));
}

TEST(ClientDisconnectTests, newestFileClipboardSupersedesActivePrefetchWithoutWaiting)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    std::atomic<bool> releaseSender(false);
    std::shared_ptr<StreamChunker> chunker(new StreamChunker());
    client.testSetSendFileChunker(chunker);
    client.testSetSendFileIsClipboardPrefetch(true);
    client.testSetSendFileThread(new Thread([&releaseSender]() {
        while (!releaseSender.load()) {
            ARCH->sleep(0.01);
            Thread::testCancel();
        }
    }));

    std::vector<barrier::fs::path> first(1, barrier::fs::u8path("/tmp/first.txt"));
    std::vector<barrier::fs::path> latest(1, barrier::fs::u8path("/tmp/latest.txt"));
    Stopwatch elapsed;
    client.testSendClipboardSelectionToServer(first);
    client.testSendClipboardSelectionToServer(latest);

    EXPECT_LT(elapsed.getTime(), 0.5);
    EXPECT_TRUE(chunker->testShouldInterrupt());
    EXPECT_EQ(latest, client.testPendingFileClipboardPrefetchPaths());

    releaseSender.store(true);
    for (int i = 0; i < 200 && !client.testCleanupSendFileThread(false); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(client.testCleanupSendFileThread(false));
}

TEST(ClientDisconnectTests, fileClipboardWaitsForManualSenderWithoutInterruptingIt)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    std::atomic<bool> releaseSender(false);
    std::shared_ptr<StreamChunker> chunker(new StreamChunker());
    client.testSetSendFileChunker(chunker);
    client.testSetSendFileIsClipboardPrefetch(false);
    client.testSetSendFileThread(new Thread([&releaseSender]() {
        while (!releaseSender.load()) {
            ARCH->sleep(0.01);
            Thread::testCancel();
        }
    }));

    std::vector<barrier::fs::path> latest(1, barrier::fs::u8path("/tmp/latest.txt"));
    client.testSendClipboardSelectionToServer(latest);

    EXPECT_FALSE(chunker->testShouldInterrupt());
    EXPECT_EQ(latest, client.testPendingFileClipboardPrefetchPaths());

    releaseSender.store(true);
    for (int i = 0; i < 200 && !client.testCleanupSendFileThread(false); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(client.testCleanupSendFileThread(false));
}

TEST(ClientDisconnectTests, newerTextClipboardCancelsPendingFilePrefetch)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    std::atomic<bool> releaseSender(false);
    std::shared_ptr<StreamChunker> chunker(new StreamChunker());
    client.testSetSendFileChunker(chunker);
    client.testSetSendFileIsClipboardPrefetch(true);
    client.testSetSendFileThread(new Thread([&releaseSender]() {
        while (!releaseSender.load()) {
            ARCH->sleep(0.01);
            Thread::testCancel();
        }
    }));
    client.testSendClipboardSelectionToServer(
        std::vector<barrier::fs::path>(1, barrier::fs::u8path("/tmp/stale.txt")));

    client.testSupersedeFileClipboard();

    EXPECT_TRUE(client.testPendingFileClipboardPrefetchPaths().empty());
    EXPECT_TRUE(chunker->testShouldInterrupt());

    releaseSender.store(true);
    for (int i = 0; i < 200 && !client.testCleanupSendFileThread(false); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(client.testCleanupSendFileThread(false));
}

TEST(ClientDisconnectTests, fileKeepAliveStartsNewestPendingClipboardPrefetch)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);

    std::atomic<bool> releaseSender(false);
    std::shared_ptr<StreamChunker> chunker(new StreamChunker());
    client.testSetSendFileChunker(chunker);
    client.testSetSendFileIsClipboardPrefetch(true);
    client.testSetSendFileThread(new Thread([&releaseSender]() {
        while (!releaseSender.load()) {
            ARCH->sleep(0.01);
            Thread::testCancel();
        }
    }));

    std::vector<barrier::fs::path> latest(1, barrier::fs::u8path("/tmp/latest.txt"));
    client.testSendClipboardSelectionToServer(latest);
    releaseSender.store(true);
    for (int i = 0;
         i < 200 && !client.testPendingFileClipboardPrefetchPaths().empty(); ++i) {
        client.testHandleFileKeepAlive();
        ARCH->sleep(0.001);
    }

    EXPECT_TRUE(client.testPendingFileClipboardPrefetchPaths().empty());
    EXPECT_EQ(1u, client.testSendFileTransferId());
    EXPECT_TRUE(client.testHasSendFileThread());

    for (int i = 0; i < 200 && !client.testCleanupSendFileThread(true); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(client.testCleanupSendFileThread(true));
}

TEST(ClientDisconnectTests, pendingPrefetchWaitsForQueuedTransferTerminator)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);
    client.testSetSendFileIsClipboardPrefetch(true);
    client.testSetSendFileProtocolState(true, false);

    const std::vector<barrier::fs::path> latest(
        1, barrier::fs::u8path("/tmp/latest.txt"));
    client.testSendClipboardSelectionToServer(latest);

    EXPECT_EQ(latest, client.testPendingFileClipboardPrefetchPaths());
    EXPECT_EQ(0u, client.testSendFileTransferId());
    EXPECT_FALSE(client.testHasSendFileThread());

    client.testSetSendFileProtocolState(true, true);
    client.testHandleFileKeepAlive();

    EXPECT_TRUE(client.testPendingFileClipboardPrefetchPaths().empty());
    EXPECT_EQ(1u, client.testSendFileTransferId());
    EXPECT_TRUE(client.testHasSendFileThread());
    for (int i = 0; i < 200 && !client.testCleanupSendFileThread(true); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(client.testCleanupSendFileThread(true));
}

TEST(ClientDisconnectTests, pendingAsyncClipboardSendIsNotMarkedSentUntilReaped)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    StableTextClipboardScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);

    client.testSendClipboard(kClipboardClipboard);

    EXPECT_EQ(1u, proxy->sendCalls);
    EXPECT_TRUE(client.testClipboardSendPending(kClipboardClipboard));
    EXPECT_FALSE(client.testClipboardSent(kClipboardClipboard));

    proxy->reapReady = true;
    proxy->reapSucceeded = true;
    client.testSendClipboard(kClipboardClipboard);

    EXPECT_EQ(1u, proxy->sendCalls);
    EXPECT_GE(proxy->reapCalls, 1u);
    EXPECT_FALSE(client.testClipboardSendPending(kClipboardClipboard));
    EXPECT_TRUE(client.testClipboardSent(kClipboardClipboard));
}

TEST(ClientDisconnectTests, failedAsyncClipboardSendIsRetriedBeforeMarkedSent)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    StableTextClipboardScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);

    client.testSendClipboard(kClipboardClipboard);
    EXPECT_FALSE(client.testClipboardSent(kClipboardClipboard));

    proxy->reapReady = true;
    proxy->reapSucceeded = false;
    proxy->result = ServerProxy::kClipboardSendQueued;
    client.testSendClipboard(kClipboardClipboard);

    EXPECT_EQ(2u, proxy->sendCalls);
    EXPECT_FALSE(client.testClipboardSendPending(kClipboardClipboard));
    EXPECT_TRUE(client.testClipboardSent(kClipboardClipboard));
}

TEST(ClientDisconnectTests, invalidRemoteClipboardPackageDoesNotPublishReadyClipboard)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    UInt32 readyEvents = 0;
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&readyEvents](const Event& event) {
            if (event.getDataObject() != NULL) {
                ++readyEvents;
            }
            Event::deleteData(event);
        }));

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    client.testWriteRemoteClipboardTransfer(invalidTransferPackageData(), "bad-session");

    EXPECT_EQ(0u, readyEvents);
}

TEST(ClientDisconnectTests, staleFileChunkAfterDisconnectIsIgnored)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    FileChunk* chunk = FileChunk::data(
        reinterpret_cast<const UInt8*>("stale"), 5);
    client.testSendFileChunk(chunk);

    delete chunk;
}

TEST(ClientDisconnectTests, staleFileChunkAfterReconnectDoesNotWriteToNewStream)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));

    EXPECT_CALL(*stream, write(_, _)).Times(0);
    EXPECT_CALL(*stream, writeLowPriority(_, _)).Times(0);

    client.testAttachStream(stream);
    client.testSetSendFileTransferId(2);

    FileChunk* chunk = FileChunk::data(
        reinterpret_cast<const UInt8*>("old"), 3);
    chunk->m_transferId = 1;
    client.testSendFileChunk(chunk);

    delete chunk;
}

TEST(ClientDisconnectTests, sameTransferIdStaleFileChunkAfterReconnectIsDropped)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    NiceMock<MockStream>* oldStream = new NiceMock<MockStream>();
    ON_CALL(*oldStream, getEventTarget()).WillByDefault(Return(oldStream));
    ON_CALL(*oldStream, getBufferedOutputSize()).WillByDefault(Return(0));
    client.testAttachStream(oldStream);
    client.testSetSendFileTransferId(7);

    client.disconnect(NULL);

    NiceMock<MockStream>* newStream = new NiceMock<MockStream>();
    ON_CALL(*newStream, getEventTarget()).WillByDefault(Return(newStream));
    ON_CALL(*newStream, getBufferedOutputSize()).WillByDefault(Return(0));

    EXPECT_CALL(*newStream, write(_, _)).Times(0);
    EXPECT_CALL(*newStream, writeLowPriority(_, _)).Times(0);

    client.testAttachStream(newStream);

    FileChunk* chunk = FileChunk::data(
        reinterpret_cast<const UInt8*>("old-same-generation"), 19);
    chunk->m_transferId = 7;
    client.testSendFileChunk(chunk);

    delete chunk;
}

TEST(ClientDisconnectTests, generationlessFileChunkStillWritesOnGenerationlessConnection)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));

    EXPECT_CALL(*stream, write(_, _)).Times(0);
    EXPECT_CALL(*stream, writeLowPriority(_, _)).Times(1);

    client.testAttachStream(stream);

    FileChunk* chunk = FileChunk::data(
        reinterpret_cast<const UInt8*>("old-compatible"), 14);
    client.testSendFileChunk(chunk);

    delete chunk;
}

TEST(ClientDisconnectTests, staleFileClipboardReadyDoesNotReplacePendingSession)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    std::vector<std::string> readyPaths;
    readyPaths.push_back("/tmp/current-ready.txt");
    client.testSetFileClipboardSessions("current-session", "ready-session", readyPaths);

    std::vector<std::string> stalePaths;
    stalePaths.push_back("/tmp/stale-ready.txt");
    client.testHandleFileClipboardReady("stale-session", stalePaths, false);

    EXPECT_EQ("current-session", client.testRemoteFileClipboardSession());
    EXPECT_EQ("ready-session", client.testReadyFileClipboardSession());
    ASSERT_EQ(1u, client.testReadyFileClipboardPaths().size());
    EXPECT_EQ("/tmp/current-ready.txt", client.testReadyFileClipboardPaths()[0]);
}

TEST(ClientDisconnectTests, localClipboardGrabSupersedesPendingRemoteFileClipboard)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.sessionId = "remote-file-a";
    payload.paths.push_back(barrier::fs::u8path("/tmp/remote-file-a.txt"));
    Clipboard remoteClipboard;
    ASSERT_TRUE(remoteClipboard.open(0));
    remoteClipboard.empty();
    remoteClipboard.add(IClipboard::kFileList,
                        RemoteFileClipboard::serialize(payload));
    remoteClipboard.close();
    client.setClipboard(kClipboardClipboard, &remoteClipboard);

    client.testHandleClipboardGrabbed(kClipboardClipboard);

    std::vector<std::string> readyPaths(1, "/tmp/materialized-a.txt");
    client.testHandleFileClipboardReady("remote-file-a", readyPaths, false);

    EXPECT_TRUE(client.testRemoteFileClipboardSession().empty());
    EXPECT_TRUE(client.testReadyFileClipboardSession().empty());
    EXPECT_TRUE(client.testReadyFileClipboardPaths().empty());
    EXPECT_EQ(0u, platform->setClipboardCount);
}

TEST(ClientDisconnectTests, localFileListClipboardStartsMetadataAndPackageTransfer)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    platform->clipboardAvailable = true;
    platform->clipboardContainsFileList = true;
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    proxy->result = ServerProxy::kClipboardSendQueued;
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);

    client.testSendClipboard(kClipboardClipboard);

    EXPECT_EQ(1u, proxy->sendCalls);
    EXPECT_TRUE(client.testClipboardSent(kClipboardClipboard));
    EXPECT_TRUE(client.testHasSendFileThread());

    RemoteFileClipboard::Data metadata;
    ASSERT_TRUE(RemoteFileClipboard::readFromClipboard(proxy->lastClipboard, metadata));
    EXPECT_EQ(RemoteFileClipboard::Mode::SourcePaths, metadata.mode);
    EXPECT_FALSE(metadata.sessionId.empty());
    ASSERT_EQ(1u, metadata.paths.size());
    EXPECT_EQ("C:/local-copy.txt", metadata.paths[0].u8string());
}

TEST(ClientDisconnectTests, materializedRemoteFileListIsNotEchoedAsLocalCopy)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    platform->clipboardAvailable = true;
    platform->clipboardContainsFileList = true;
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    proxy->result = ServerProxy::kClipboardSendQueued;
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);

    std::vector<std::string> readyPaths;
    readyPaths.push_back("C:/local-copy.txt");
    client.testSetFileClipboardSessions("remote-session", "remote-session", readyPaths);
    client.testHandleFileClipboardReady("remote-session", readyPaths, true);

    client.testSendClipboard(kClipboardClipboard);

    EXPECT_EQ(0u, proxy->sendCalls);
    EXPECT_FALSE(client.testHasSendFileThread());
    EXPECT_TRUE(client.testReadyFileClipboardSession().empty());
    EXPECT_TRUE(client.testReadyFileClipboardPaths().empty());
}

TEST(ClientDisconnectTests, blockedLocalFileListCannotBeOverwrittenByRemoteClipboard)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    platform->clipboardAvailable = true;
    platform->clipboardContainsFileList = true;
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);

    client.testHandleClipboardGrabbed(kClipboardClipboard);
    client.testSendClipboard(kClipboardClipboard);

    EXPECT_TRUE(client.testOwnClipboard(kClipboardClipboard));
    EXPECT_FALSE(client.testClipboardSent(kClipboardClipboard));
    EXPECT_EQ(1u, proxy->sendCalls);

    Clipboard remoteClipboard;
    ASSERT_TRUE(remoteClipboard.open(0));
    remoteClipboard.empty();
    remoteClipboard.add(IClipboard::kText, "remote clipboard");
    remoteClipboard.close();

    client.setClipboard(kClipboardClipboard, &remoteClipboard);

    EXPECT_TRUE(client.testOwnClipboard(kClipboardClipboard));
    EXPECT_FALSE(client.testClipboardSent(kClipboardClipboard));
    EXPECT_EQ(0u, platform->setClipboardCount);
}

TEST(ClientDisconnectTests, leaveDefersClipboardReadAndDoesNotRetryUnsupportedSelection)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    platform->clipboardAvailable = true;
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);

    client.enter(0, 0, 0, 0, false);
    EXPECT_TRUE(client.leave());

    EXPECT_EQ(0u, platform->getClipboardCount);
    EXPECT_EQ(0u, proxy->sendCalls);
    EXPECT_TRUE(client.testClipboardRetryPending(kClipboardClipboard));
    EXPECT_FALSE(client.testClipboardRetryPending(kClipboardSelection));

    proxy->result = ServerProxy::kClipboardSendQueued;
    client.testHandleClipboardRetry();

    EXPECT_EQ(1u, platform->getClipboardCount);
    EXPECT_EQ(1u, proxy->sendCalls);
    EXPECT_FALSE(client.testClipboardRetryPending(kClipboardClipboard));
}

TEST(ClientDisconnectTests, repeatedLeaveRearmsDeferredClipboardSnapshot)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    UInt32 timersCreated = 0;
    std::vector<EventQueueTimer*> deletedTimers;
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Invoke([&timersCreated](double, void*) {
            ++timersCreated;
            return reinterpret_cast<EventQueueTimer*>(
                static_cast<uintptr_t>(timersCreated));
        }));
    ON_CALL(events, deleteTimer(_))
        .WillByDefault(Invoke([&deletedTimers](EventQueueTimer* timer) {
            deletedTimers.push_back(timer);
        }));

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);
    const UInt32 baselineTimers = timersCreated;

    client.enter(0, 0, 1, 0, false);
    ASSERT_TRUE(client.leave());
    ASSERT_EQ(baselineTimers + 1, timersCreated);

    client.enter(0, 0, 2, 0, false);
    ASSERT_TRUE(client.leave());

    EXPECT_EQ(baselineTimers + 2, timersCreated);
    ASSERT_EQ(1u, deletedTimers.size());
    EXPECT_EQ(reinterpret_cast<EventQueueTimer*>(
                  static_cast<uintptr_t>(baselineTimers + 1)),
              deletedTimers[0]);
    EXPECT_TRUE(client.testClipboardRetryPending(kClipboardClipboard));
}

TEST(ClientDisconnectTests, imageFileListClipboardIsNotDowngradedToPngAndSent)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    ImageFileListClipboardScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));
    EXPECT_CALL(*stream, write(_, _)).Times(0);
    EXPECT_CALL(*stream, writeLowPriority(_, _)).Times(0);

    client.testAttachStream(stream);
    client.testSendClipboard(kClipboardClipboard);
}

TEST(ClientDisconnectTests, fileListTransferDoesNotPoisonNextPlainTextClipboard)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    std::vector<UInt8> clipboardMarks;
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&clipboardEvents, &clipboardMarks](const Event& event) {
            if (event.getType() == clipboardEvents.clipboardSending() &&
                event.getData() != NULL) {
                ClipboardChunk* chunk = static_cast<ClipboardChunk*>(event.getData());
                clipboardMarks.push_back(static_cast<UInt8>(chunk->m_chunk[5]));
            }
            Event::deleteData(event);
        }));

    FileListThenTextClipboardScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));

    client.testAttachStream(stream);
    client.testSendClipboard(kClipboardClipboard);
    ASSERT_GE(clipboardMarks.size(), 3u);
    EXPECT_EQ(kDataStart, clipboardMarks.front());
    EXPECT_EQ(kDataEnd, clipboardMarks.back());
    const std::size_t fileClipboardMarkCount = clipboardMarks.size();

    client.testSendClipboard(kClipboardClipboard);
    ASSERT_GE(clipboardMarks.size(), fileClipboardMarkCount + 3u);
    EXPECT_EQ(kDataStart, clipboardMarks[fileClipboardMarkCount]);
    EXPECT_EQ(kDataEnd, clipboardMarks.back());
}

TEST(ClientDisconnectTests, singleFileTransferRejectsSymlinkSource)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    const barrier::fs::path target =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-client-regular-source.txt");
    const barrier::fs::path link =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-client-symlink-source.txt");
    std::error_code ec;
    barrier::fs::remove(target, ec);
    barrier::fs::remove(link, ec);
    {
        std::ofstream file(target.u8string().c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        file << "payload";
    }
    barrier::fs::create_symlink(target, link, ec);
    if (ec) {
        barrier::fs::remove(target, ec);
        GTEST_SKIP() << "filesystem does not allow symlink creation: " << ec.message();
    }

    barrier::fs::path sourcePath;
    barrier::fs::path tempPackagePath;
    std::string error;
    EXPECT_FALSE(testClientPrepareTransferSource(link.u8string().c_str(),
                                                 sourcePath,
                                                 tempPackagePath,
                                                 error));
    EXPECT_EQ("transfer source must be a regular file", error);
    EXPECT_TRUE(tempPackagePath.empty());

    barrier::fs::remove(link, ec);
    barrier::fs::remove(target, ec);
}

TEST(ClientDisconnectTests, clipboardReadFailureRetriesWithoutPublishingEmptyClipboard)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    UInt32 timersCreated = 0;
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Invoke([&timersCreated](double, void*) {
            ++timersCreated;
            return reinterpret_cast<EventQueueTimer*>(1);
        }));
    UInt32 queuedClipboardChunks = 0;
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&queuedClipboardChunks](const Event& event) {
            if (event.getDataObject() != NULL) {
                ++queuedClipboardChunks;
            }
            Event::deleteData(event);
        }));

    FlakyTextClipboardScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));

    client.testAttachStream(stream);
    const UInt32 baselineTimers = timersCreated;
    const UInt32 baselineChunks = queuedClipboardChunks;

    client.testSendClipboard(kClipboardClipboard);
    EXPECT_EQ(1u, screen.calls);
    EXPECT_EQ(baselineTimers + 1, timersCreated);
    EXPECT_EQ(baselineChunks, queuedClipboardChunks);

    client.testSendClipboard(kClipboardClipboard);
    EXPECT_EQ(2u, screen.calls);
    EXPECT_EQ(baselineTimers + 1, timersCreated);
    EXPECT_GT(queuedClipboardChunks, baselineChunks);
}

TEST(ClientDisconnectTests, successfulEmptyClipboardSnapshotClearsPreviousText)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    TextThenEmptyClipboardScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    proxy->result = ServerProxy::kClipboardSendQueued;
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);
    client.testSendClipboard(kClipboardClipboard);
    EXPECT_EQ(1u, screen.calls);
    EXPECT_EQ(1u, proxy->sendCalls);

    client.testSendClipboard(kClipboardClipboard);

    EXPECT_EQ(2u, screen.calls);
    EXPECT_EQ(2u, proxy->sendCalls);
    ASSERT_TRUE(proxy->lastClipboard.open(0));
    for (UInt32 format = 0; format < IClipboard::kNumFormats; ++format) {
        EXPECT_FALSE(proxy->lastClipboard.has(
            static_cast<IClipboard::EFormat>(format)));
    }
    proxy->lastClipboard.close();
}

TEST(ClientDisconnectTests, materializedFileListClipboardIsSentToServer)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents, fileEvents,
                                    streamEvents, clipboardEvents, dataSocketEvents,
                                    socketEvents);

    FileListClipboardScreen screen;
    screen.mode = RemoteFileClipboard::Mode::MaterializedPaths;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));

    client.testAttachStream(stream);
    EXPECT_CALL(events, addEvent(_)).Times(AtLeast(3));
    client.testSendClipboard(kClipboardClipboard);
}
