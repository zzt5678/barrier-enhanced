#define BARRIER_TEST_ENV
#include "client/Client.h"
#include "client/ServerProxy.h"

#include "barrier/ClientArgs.h"
#include "barrier/Clipboard.h"
#include "barrier/ClipboardChunk.h"
#include "barrier/FileChunk.h"
#include "barrier/FileTransferProtocol.h"
#include "barrier/FileTransferSendState.h"
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
#include <cstdint>
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

constexpr const char* kLocalFileSessionId =
    "00000000000000000000000000000011";
constexpr const char* kLocalImageFileSessionId =
    "00000000000000000000000000000012";
constexpr const char* kRemoteFileSessionId =
    "00000000000000000000000000000013";

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
        setClipboardSnapshotCount(0),
        acceptClipboardSet(true),
        asyncClipboardPublications(false),
        acceptClipboardPublication(true),
        lastClipboardPublicationId(0),
        getClipboardCount(0),
        getClipboardSnapshotCount(0),
        lastSetClipboardWasNull(false),
        enterable(true),
        failDuringEnter(false),
        failLeave(false),
        failMouseMove(false),
        inputBackendGeneration(1),
        lastSetClipboardSnapshotId(kClipboardEnd),
        clipboardAvailable(false),
        clipboardContainsFileList(false),
        clipboardSnapshotAvailable(false),
        clipboardSnapshotTime(0),
        clipboardText("stable clipboard")
    {
    }

    void enable() override { }
    void disable() override { }
    void enter() override
    {
        ++enterCount;
        if (failDuringEnter) {
            enterable = false;
        }
    }
    bool tryEnter() override
    {
        enter();
        return enterable;
    }
    bool leave() override
    {
        ++leaveCount;
        return !failLeave;
    }
    bool setClipboard(ClipboardID, const IClipboard* clipboard) override
    {
        ++setClipboardCount;
        lastSetClipboardWasNull = (clipboard == NULL);
        if (clipboard != NULL) {
            lastSetClipboard.unmarshall(IClipboard::marshall(clipboard), 0);
        }
        return acceptClipboardSet;
    }
    bool setClipboardSnapshot(
        ClipboardID id,
        const std::shared_ptr<const String>& snapshot) override
    {
        ++setClipboardSnapshotCount;
        lastSetClipboardSnapshotId = id;
        lastSetClipboardSnapshot = snapshot;
        return snapshot != NULL;
    }
    bool setClipboardSnapshot(
        ClipboardID id,
        const std::shared_ptr<const String>& snapshot,
        std::uint64_t publicationId) override
    {
        ++setClipboardSnapshotCount;
        lastSetClipboardSnapshotId = id;
        lastSetClipboardSnapshot = snapshot;
        lastClipboardPublicationId = publicationId;
        return acceptClipboardPublication && snapshot != NULL;
    }
    bool hasAsyncClipboardPublications() const override
    {
        return asyncClipboardPublications;
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
    std::uint64_t inputGeneration() const override { return inputBackendGeneration; }
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
            payload.sessionId = kLocalFileSessionId;
            payload.paths.push_back(barrier::fs::u8path("C:/local-copy.txt"));
            clipboard->add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
        }
        else {
            clipboard->add(IClipboard::kText, clipboardText);
        }
        clipboard->close();
        return true;
    }
    bool getClipboardSnapshot(
        ClipboardID id, std::shared_ptr<const String>* snapshot,
        UInt32* snapshotTime) const override
    {
        ++getClipboardSnapshotCount;
        if (!clipboardSnapshotAvailable || id != kClipboardClipboard ||
            snapshot == NULL || snapshotTime == NULL) {
            return false;
        }
        *snapshot = clipboardSnapshot;
        *snapshotTime = clipboardSnapshotTime;
        return clipboardSnapshot != NULL;
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
    bool tryFakeMouseMove(SInt32, SInt32) override
    {
        ++mouseMoveCount;
        return !failMouseMove;
    }
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
    UInt32 setClipboardSnapshotCount;
    bool acceptClipboardSet;
    bool asyncClipboardPublications;
    bool acceptClipboardPublication;
    std::uint64_t lastClipboardPublicationId;
    mutable UInt32 getClipboardCount;
    mutable UInt32 getClipboardSnapshotCount;
    bool lastSetClipboardWasNull;
    bool enterable;
    bool failDuringEnter;
    bool failLeave;
    bool failMouseMove;
    std::uint64_t inputBackendGeneration;
    Clipboard lastSetClipboard;
    ClipboardID lastSetClipboardSnapshotId;
    std::shared_ptr<const String> lastSetClipboardSnapshot;
    bool clipboardAvailable;
    bool clipboardContainsFileList;
    bool clipboardSnapshotAvailable;
    UInt32 clipboardSnapshotTime;
    std::string clipboardText;
    std::shared_ptr<const String> clipboardSnapshot;
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

class AdjustableBufferedStream : public CountingStream {
public:
    explicit AdjustableBufferedStream(UInt32* deletedCount) :
        CountingStream(deletedCount),
        bufferedOutput(0)
    {
    }

    UInt32 getBufferedOutputSize() const override
    {
        return bufferedOutput;
    }

    UInt32 bufferedOutput;
};

class BulkHandshakeStream : public barrier::IStream {
public:
    void queueInput(const std::vector<UInt8>& bytes)
    {
        input.insert(input.end(), bytes.begin(), bytes.end());
    }

    void close() override { }
    UInt32 read(void* buffer, UInt32 count) override
    {
        const UInt32 available = getSize();
        const UInt32 copied = count < available ? count : available;
        if (copied != 0) {
            std::memcpy(buffer, input.data() + inputOffset, copied);
            inputOffset += copied;
        }
        return copied;
    }
    void write(const void* buffer, UInt32 count) override
    {
        const UInt8* bytes = static_cast<const UInt8*>(buffer);
        output.insert(output.end(), bytes, bytes + count);
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
        return const_cast<BulkHandshakeStream*>(this);
    }
    bool isReady() const override { return getSize() != 0; }
    UInt32 getSize() const override
    {
        return static_cast<UInt32>(input.size() - inputOffset);
    }
    UInt32 getBufferedOutputSize() const override { return 0; }

    std::vector<UInt8> input;
    std::vector<UInt8> output;
    std::size_t inputOffset = 0;
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
        rawSendCalls(0),
        reapCalls(0)
    {
    }

    ClipboardSendResult onClipboardChanged(ClipboardID, const IClipboard* clipboard) override
    {
        ++sendCalls;
        lastClipboard.unmarshall(IClipboard::marshall(clipboard), 0);
        return result;
    }

    ClipboardSendResult onClipboardDataChanged(
        ClipboardID, const std::shared_ptr<const std::string>& data,
        bool) override
    {
        ++sendCalls;
        ++rawSendCalls;
        lastClipboardData = data;
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
    UInt32 rawSendCalls;
    UInt32 reapCalls;
    Clipboard lastClipboard;
    std::shared_ptr<const std::string> lastClipboardData;
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
        payload.sessionId = kLocalFileSessionId;
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
        payload.sessionId = kLocalImageFileSessionId;
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
            payload.sessionId = kLocalFileSessionId;
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
    payload.sessionId = kRemoteFileSessionId;
    payload.paths.push_back(barrier::fs::u8path("/tmp/remote-source.txt"));

    Clipboard clipboard;
    ASSERT_TRUE(clipboard.open(0));
    clipboard.empty();
    clipboard.add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
    clipboard.close();

    client.setClipboard(kClipboardClipboard, &clipboard);

    EXPECT_EQ(0u, platform->setClipboardCount);
    EXPECT_EQ(kRemoteFileSessionId, client.testRemoteFileClipboardSession());
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
    payload.sessionId = kRemoteFileSessionId;
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
    EXPECT_EQ(kRemoteFileSessionId, client.testRemoteFileClipboardSession());
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
    client.testSetActive(true);

    client.testHandleClipboardGrabbed(kClipboardClipboard);

    EXPECT_TRUE(client.testOwnClipboard(kClipboardClipboard));
    EXPECT_FALSE(client.testClipboardSent(kClipboardClipboard));
    EXPECT_FALSE(client.testClipboardRetryPending(kClipboardClipboard));
    EXPECT_EQ(0u, proxy->sendCalls);

    proxy->result = ServerProxy::kClipboardSendQueued;
    client.testSendClipboard(kClipboardClipboard);

    EXPECT_EQ(1u, proxy->sendCalls);
    EXPECT_TRUE(client.testClipboardSent(kClipboardClipboard));
}

TEST(ClientDisconnectTests, inactiveLegacyClipboardGrabSchedulesDeferredSend)
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
    proxy->result = ServerProxy::kClipboardSendQueued;
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);
    client.testSetActive(false);

    client.testHandleClipboardGrabbed(kClipboardClipboard);

    EXPECT_TRUE(client.testClipboardRetryPending(kClipboardClipboard));
    EXPECT_EQ(0u, proxy->sendCalls);

    client.testHandleClipboardRetry();

    EXPECT_FALSE(client.testClipboardRetryPending(kClipboardClipboard));
    EXPECT_EQ(1u, proxy->sendCalls);
}

TEST(ClientDisconnectTests, validatedSnapshotUsesImmutableSendPath)
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

    Clipboard source;
    ASSERT_TRUE(source.open(91));
    source.empty();
    source.add(IClipboard::kText, "worker-owned clipboard snapshot");
    source.close();

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    platform->clipboardSnapshotAvailable = true;
    platform->clipboardSnapshotTime = 91;
    platform->clipboardSnapshot.reset(new String(source.marshall()));
    const std::shared_ptr<const String> expected = platform->clipboardSnapshot;

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

    client.testHandleClipboardGrabbed(kClipboardClipboard);
    client.testSendClipboard(kClipboardClipboard);

    EXPECT_EQ(1u, platform->getClipboardSnapshotCount);
    EXPECT_EQ(0u, platform->getClipboardCount);
    EXPECT_EQ(1u, proxy->rawSendCalls);
    ASSERT_TRUE(proxy->lastClipboardData);
    EXPECT_EQ(expected.get(), proxy->lastClipboardData.get());
    EXPECT_TRUE(client.testClipboardSent(kClipboardClipboard));
}

TEST(ClientDisconnectTests, pendingImmutableSnapshotReapsByPointerIdentity)
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

    Clipboard source;
    ASSERT_TRUE(source.open(92));
    source.empty();
    source.add(IClipboard::kPNG, String(2 * 1024 * 1024, 'p'));
    source.close();

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    platform->clipboardSnapshotAvailable = true;
    platform->clipboardSnapshotTime = 92;
    platform->clipboardSnapshot.reset(new String(source.marshall()));
    const std::shared_ptr<const String> expected = platform->clipboardSnapshot;

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

    client.testSendClipboard(kClipboardClipboard);

    EXPECT_EQ(1u, proxy->rawSendCalls);
    EXPECT_EQ(1u, proxy->sendCalls);
    EXPECT_TRUE(client.testClipboardSendPending(kClipboardClipboard));
    EXPECT_FALSE(client.testClipboardSent(kClipboardClipboard));
    EXPECT_EQ(1u, platform->getClipboardSnapshotCount);
    EXPECT_EQ(0u, platform->getClipboardCount);
    ASSERT_TRUE(proxy->lastClipboardData);
    EXPECT_EQ(expected.get(), proxy->lastClipboardData.get());

    proxy->reapReady = true;
    proxy->reapSucceeded = true;
    client.testSendClipboard(kClipboardClipboard);

    EXPECT_EQ(1u, proxy->rawSendCalls);
    EXPECT_EQ(1u, proxy->sendCalls);
    EXPECT_GE(proxy->reapCalls, 1u);
    EXPECT_FALSE(client.testClipboardSendPending(kClipboardClipboard));
    EXPECT_TRUE(client.testClipboardSent(kClipboardClipboard));
    EXPECT_EQ(2u, platform->getClipboardSnapshotCount);
    EXPECT_EQ(0u, platform->getClipboardCount);
    EXPECT_EQ(expected.get(), proxy->lastClipboardData.get());
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

TEST(ClientDisconnectTests, remoteSnapshotQueuesImmutablePlatformPublish)
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

    Clipboard source;
    ASSERT_TRUE(source.open(0));
    source.empty();
    source.add(IClipboard::kPNG, String(2 * 1024 * 1024, 'p'));
    source.close();
    const std::shared_ptr<const String> snapshot(
        new String(source.marshall()));

    EXPECT_TRUE(client.setClipboardData(kClipboardClipboard, snapshot));
    EXPECT_EQ(0u, platform->setClipboardCount);
    EXPECT_EQ(1u, platform->setClipboardSnapshotCount);
    EXPECT_EQ(snapshot.get(), platform->lastSetClipboardSnapshot.get());
}

TEST(ClientDisconnectTests, remoteMaterializedPathsCannotPublishLocalMove)
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
    payload.mode = RemoteFileClipboard::Mode::MaterializedPaths;
    payload.cut = true;
    payload.sessionId = "0123456789abcdef0123456789abcdef";
    payload.paths.push_back(barrier::fs::u8path("C:/Windows/System32"));
    Clipboard source;
    ASSERT_TRUE(source.open(0));
    source.empty();
    source.add(IClipboard::kFileList,
               RemoteFileClipboard::serialize(payload));
    source.close();
    const std::shared_ptr<const String> snapshot(
        new String(source.marshall()));

    EXPECT_FALSE(client.setClipboardData(kClipboardClipboard, snapshot));
    EXPECT_EQ(0u, platform->setClipboardCount);
    EXPECT_EQ(0u, platform->setClipboardSnapshotCount);
}

TEST(ClientDisconnectTests, legacyPeerFileClipboardIsRejectedButTextStillPublishes)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    barrier::Screen screen(platform, &events);
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(11);

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.sessionId = "0123456789abcdef0123456789abcdef";
    payload.paths.push_back(barrier::fs::u8path("C:/remote.txt"));
    Clipboard fileClipboard;
    ASSERT_TRUE(fileClipboard.open(0));
    fileClipboard.empty();
    fileClipboard.add(IClipboard::kFileList,
                      RemoteFileClipboard::serialize(payload));
    fileClipboard.close();

    EXPECT_FALSE(client.setClipboardData(
        kClipboardClipboard,
        std::make_shared<const String>(fileClipboard.marshall())));
    EXPECT_EQ(0u, platform->setClipboardCount);
    EXPECT_EQ(0u, platform->setClipboardSnapshotCount);

    Clipboard textClipboard;
    ASSERT_TRUE(textClipboard.open(0));
    textClipboard.empty();
    textClipboard.add(IClipboard::kText, "legacy text remains compatible");
    textClipboard.close();
    EXPECT_TRUE(client.setClipboardData(
        kClipboardClipboard,
        std::make_shared<const String>(textClipboard.marshall())));
    EXPECT_EQ(1u, platform->setClipboardSnapshotCount);
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

    std::string completedData;
    size_t completedSize = 0;
    barrier::fs::path completedSpool;
    client.getFileReceiveSession().takeCompleted(
        completedData, completedSize, completedSpool);
    ASSERT_EQ("old", completedData);
    ASSERT_EQ(3u, completedSize);
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

TEST(ClientDisconnectTests, failedBackendLeavePreservesActiveInputLeaseUntilRetry)
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
    client.handshakeComplete();
    ASSERT_TRUE(client.enterInputLease(10, 20, 73, 0, false));
    ASSERT_FALSE(client.canAcceptInputHandoff());

    platform->failLeave = true;
    EXPECT_FALSE(client.leave());
    EXPECT_EQ(1u, platform->leaveCount);
    EXPECT_FALSE(client.canAcceptInputHandoff());

    platform->failLeave = false;
    EXPECT_TRUE(client.leave());
    EXPECT_EQ(2u, platform->leaveCount);
    EXPECT_TRUE(client.canAcceptInputHandoff());
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

TEST(ClientDisconnectTests, protocol110AcknowledgesSuccessfulCommittedEnter)
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
    ServerProxy proxy(&client, &stream, &events, 10);
    client.handshakeComplete();

    ProtocolUtil::writef(&stream, kMsgCPrepareEnter + 4, 10, 20, 142, 0);
    proxy.prepareEnter();
    UInt32 readySeqNum = 0;
    UInt8 ready = 0;
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDEnterReady,
                                    &readySeqNum, &ready));
    ASSERT_EQ(142u, readySeqNum);
    ASSERT_EQ(1u, ready);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 142, 0);
    ASSERT_TRUE(proxy.enter());

    EXPECT_EQ(1u, platform->enterCount);
    EXPECT_EQ(1u, platform->mouseMoveCount);
    UInt32 committedSeqNum = 0;
    UInt8 committed = 0;
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDEnterReady,
                                    &committedSeqNum, &committed));
    EXPECT_EQ(142u, committedSeqNum);
    EXPECT_EQ(1u, committed);
    EXPECT_EQ(0u, stream.getSize());
}

TEST(ClientDisconnectTests, protocol111RevokesOnlyMatchingActiveInputEpoch)
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
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    ScriptedStream stream;
    ServerProxy proxy(&client, &stream, &events, 11);
    client.handshakeComplete();

    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 41, 0);
    ASSERT_TRUE(proxy.enter());
    ASSERT_EQ(1u, platform->enterCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCRevokeInput + 4, 52, 40);
    proxy.revokeInputLeaseRequest();
    UInt32 ackSeqNum = 0;
    UInt8 revoked = 1;
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDRevokeInputAck,
                                    &ackSeqNum, &revoked));
    EXPECT_EQ(52u, ackSeqNum);
    EXPECT_EQ(0u, revoked);
    EXPECT_EQ(0u, platform->leaveCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCRevokeInput + 4, 53, 41);
    proxy.revokeInputLeaseRequest();
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDRevokeInputAck,
                                    &ackSeqNum, &revoked));
    EXPECT_EQ(53u, ackSeqNum);
    EXPECT_EQ(1u, revoked);
    EXPECT_EQ(1u, platform->leaveCount);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCRevokeInput + 4, 53, 41);
    proxy.revokeInputLeaseRequest();
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDRevokeInputAck,
                                    &ackSeqNum, &revoked));
    EXPECT_EQ(1u, revoked);
    EXPECT_EQ(1u, platform->leaveCount);
}

TEST(ClientDisconnectTests, protocol19DoesNotAddCommittedEnterAck)
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
    ServerProxy proxy(&client, &stream, &events, 9);
    client.handshakeComplete();

    ProtocolUtil::writef(&stream, kMsgCPrepareEnter + 4, 10, 20, 143, 0);
    proxy.prepareEnter();
    UInt32 readySeqNum = 0;
    UInt8 ready = 0;
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDEnterReady,
                                    &readySeqNum, &ready));
    ASSERT_EQ(143u, readySeqNum);
    ASSERT_EQ(1u, ready);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 143, 0);
    ASSERT_TRUE(proxy.enter());

    EXPECT_EQ(1u, platform->enterCount);
    EXPECT_EQ(1u, platform->mouseMoveCount);
    EXPECT_EQ(0u, stream.getSize());
}

TEST(ClientDisconnectTests, replacementEnterLeaveFailureRejectsNewLease)
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
    ServerProxy proxy(&client, &stream, &events, 10);
    client.handshakeComplete();

    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 201, 0);
    ASSERT_TRUE(proxy.enter());
    ASSERT_TRUE(proxy.m_inputActive);
    ASSERT_EQ(201u, proxy.m_seqNum);
    ASSERT_EQ(1u, platform->enterCount);
    ASSERT_EQ(1u, platform->mouseMoveCount);

    stream.clearData();
    platform->failLeave = true;
    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 30, 40, 202, 0);
    ASSERT_TRUE(proxy.enter());

    UInt32 rejectedSeqNum = 0;
    UInt8 committed = 1;
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDEnterReady,
                                    &rejectedSeqNum, &committed));
    EXPECT_EQ(202u, rejectedSeqNum);
    EXPECT_EQ(0u, committed);
    EXPECT_EQ(0u, stream.getSize());
    EXPECT_TRUE(proxy.m_inputActive);
    EXPECT_EQ(201u, proxy.m_seqNum);
    EXPECT_EQ(1u, platform->leaveCount);
    EXPECT_EQ(1u, platform->enterCount);
    EXPECT_EQ(1u, platform->mouseMoveCount);
    EXPECT_FALSE(client.canAcceptInputHandoff());
    platform->failLeave = false;
}

TEST(ClientDisconnectTests, leaveFailureRetainsProxyLeaseAndRequestsDisconnect)
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
    ServerProxy proxy(&client, &stream, &events, 10);
    client.handshakeComplete();

    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 203, 0);
    ASSERT_TRUE(proxy.enter());
    ASSERT_TRUE(proxy.m_inputActive);

    bool disconnectRequested = false;
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke(
        [&disconnectRequested](const Event& event) {
            Client::FailInfo* info =
                static_cast<Client::FailInfo*>(event.getData());
            ASSERT_TRUE(info != NULL);
            EXPECT_EQ("input backend rejected screen leave", info->m_what);
            EXPECT_TRUE(info->m_retry);
            disconnectRequested = true;
            delete info;
        }));

    stream.clearData();
    platform->failLeave = true;
    proxy.leave();

    EXPECT_TRUE(disconnectRequested);
    EXPECT_TRUE(proxy.m_inputActive);
    EXPECT_EQ(203u, proxy.m_seqNum);
    EXPECT_EQ(1u, platform->leaveCount);
    EXPECT_EQ(1u, platform->enterCount);
    platform->failLeave = false;
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

TEST(ClientDisconnectTests, changedInputGenerationRejectsPreparedCommit)
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

    ProtocolUtil::writef(&stream, kMsgCPrepareEnter + 4, 10, 20, 44, 0);
    proxy.prepareEnter();

    UInt32 readySeqNum = 0;
    UInt8 ready = 0;
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDEnterReady,
                                    &readySeqNum, &ready));
    ASSERT_EQ(44u, readySeqNum);
    ASSERT_EQ(1u, ready);

    ++platform->inputBackendGeneration;
    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 44, 0);
    proxy.enter();

    EXPECT_EQ(0u, platform->enterCount);
    EXPECT_FALSE(proxy.m_inputActive);

    UInt32 rejectedSeqNum = 0;
    UInt8 commitReady = 1;
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDEnterReady,
                                    &rejectedSeqNum, &commitReady));
    EXPECT_EQ(44u, rejectedSeqNum);
    EXPECT_EQ(0u, commitReady);
}

TEST(ClientDisconnectTests, failedPlatformEnterRejectsPreparedCommit)
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
    platform->failDuringEnter = true;
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    ScriptedStream stream;
    ServerProxy proxy(&client, &stream, &events, 7);
    client.handshakeComplete();

    ProtocolUtil::writef(&stream, kMsgCPrepareEnter + 4, 10, 20, 45, 0);
    proxy.prepareEnter();

    UInt32 readySeqNum = 0;
    UInt8 ready = 0;
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDEnterReady,
                                    &readySeqNum, &ready));
    ASSERT_EQ(45u, readySeqNum);
    ASSERT_EQ(1u, ready);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 45, 0);
    proxy.enter();

    EXPECT_EQ(1u, platform->enterCount);
    EXPECT_EQ(0u, platform->mouseMoveCount);
    EXPECT_FALSE(proxy.m_inputActive);

    UInt32 rejectedSeqNum = 0;
    UInt8 commitReady = 1;
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDEnterReady,
                                    &rejectedSeqNum, &commitReady));
    EXPECT_EQ(45u, rejectedSeqNum);
    EXPECT_EQ(0u, commitReady);
}

TEST(ClientDisconnectTests, failedInitialMousePositionRollsBackCommittedEnter)
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
    platform->failMouseMove = true;
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    ScriptedStream stream;
    ServerProxy proxy(&client, &stream, &events, 7);
    client.handshakeComplete();

    ProtocolUtil::writef(&stream, kMsgCPrepareEnter + 4, 10, 20, 46, 0);
    proxy.prepareEnter();
    UInt32 preparedSeqNum = 0;
    UInt8 prepared = 0;
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDEnterReady,
                                    &preparedSeqNum, &prepared));
    ASSERT_EQ(46u, preparedSeqNum);
    ASSERT_EQ(1u, prepared);

    stream.clearData();
    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 46, 0);
    EXPECT_TRUE(proxy.enter());

    EXPECT_EQ(1u, platform->enterCount);
    EXPECT_EQ(1u, platform->mouseMoveCount);
    EXPECT_EQ(1u, platform->leaveCount);
    EXPECT_FALSE(proxy.m_inputActive);

    UInt32 rejectedSeqNum = 0;
    UInt8 committed = 1;
    ASSERT_TRUE(ProtocolUtil::readf(&stream, kMsgDEnterReady,
                                    &rejectedSeqNum, &committed));
    EXPECT_EQ(46u, rejectedSeqNum);
    EXPECT_EQ(0u, committed);
}

TEST(ClientDisconnectTests, failedInitialPositionRollbackQuarantinesInputBackend)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    platform->failMouseMove = true;
    platform->failLeave = true;
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    NiceMock<MockStream> stream;
    EXPECT_CALL(stream, close()).Times(1);
    client.testSetStreamOnly(&stream);
    client.handshakeComplete();

    EXPECT_FALSE(client.enterInputLease(10, 20, 48, 0, false));
    EXPECT_EQ(1u, platform->enterCount);
    EXPECT_EQ(1u, platform->mouseMoveCount);
    EXPECT_EQ(1u, platform->leaveCount);
    EXPECT_FALSE(client.canAcceptInputHandoff());
    client.testSetStreamOnly(NULL);
}

TEST(ClientDisconnectTests, legacyProtocolDisconnectsWhenInitialPositionFails)
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
    platform->failMouseMove = true;
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    ScriptedStream stream;
    ServerProxy proxy(&client, &stream, &events, 6);
    client.handshakeComplete();

    bool disconnectRequested = false;
    EXPECT_CALL(events, addEvent(_)).WillOnce(Invoke(
        [&disconnectRequested](const Event& event) {
            Client::FailInfo* info =
                static_cast<Client::FailInfo*>(event.getData());
            ASSERT_TRUE(info != NULL);
            EXPECT_EQ("input backend rejected screen enter",
                      info->m_what);
            EXPECT_TRUE(info->m_retry);
            disconnectRequested = true;
            delete info;
        }));

    ProtocolUtil::writef(&stream, kMsgCEnter + 4, 10, 20, 47, 0);
    EXPECT_FALSE(proxy.enter());

    EXPECT_EQ(1u, platform->enterCount);
    EXPECT_EQ(1u, platform->mouseMoveCount);
    EXPECT_EQ(1u, platform->leaveCount);
    EXPECT_FALSE(proxy.m_inputActive);
    EXPECT_TRUE(disconnectRequested);
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

    proxy.leave();
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

    proxy.leave();
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

    proxy.leave();
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

TEST(ClientDisconnectTests, protocolNegotiationRequiresTransactionalInputVersion)
{
    SInt16 negotiatedMinor = 0;
    EXPECT_FALSE(Client::negotiateProtocolVersion(1, 6, negotiatedMinor));
    EXPECT_FALSE(Client::negotiateProtocolVersion(1, 7, negotiatedMinor));
    EXPECT_FALSE(Client::negotiateProtocolVersion(1, 9, negotiatedMinor));
    EXPECT_FALSE(Client::negotiateProtocolVersion(1, 10, negotiatedMinor));
    EXPECT_FALSE(Client::negotiateProtocolVersion(1, 11, negotiatedMinor));
    EXPECT_FALSE(Client::negotiateProtocolVersion(1, 5, negotiatedMinor));
    EXPECT_FALSE(Client::negotiateProtocolVersion(2, 0, negotiatedMinor));
    EXPECT_TRUE(Client::negotiateProtocolVersion(1, 12, negotiatedMinor));
    EXPECT_EQ(12, negotiatedMinor);
    EXPECT_TRUE(Client::negotiateProtocolVersion(1, 13, negotiatedMinor));
    EXPECT_EQ(12, negotiatedMinor);
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

TEST(ClientDisconnectTests, consecutiveManualFileRequestDoesNotCancelActiveTransfer)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());

    std::atomic<bool> releaseSender(false);
    std::shared_ptr<StreamChunker> chunker(new StreamChunker());
    client.testSetSendFileChunker(chunker);
    client.testSetSendFileIsClipboardPrefetch(false);
    client.testSetSendFileThread(new Thread([&releaseSender]() {
        while (!releaseSender.load()) {
            ARCH->sleep(0.001);
        }
    }));

    client.sendFileToServer("/tmp/second-manual.txt");

    EXPECT_TRUE(client.testHasSendFileThread());
    EXPECT_FALSE(chunker->testShouldInterrupt());
    EXPECT_TRUE(client.testPendingManualFileSend().empty());

    releaseSender.store(true);
    for (int i = 0; i < 200 && !client.testCleanupSendFileThread(false); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(client.testCleanupSendFileThread(false));
}

TEST(ClientDisconnectTests, activePackagingDoesNotEnterSenderReapPolling)
{
    NiceMock<MockEventQueue> events;
    FileEvents fileEvents;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    const UInt32 transferId = client.testAllocateSendFileTransferId();
    std::shared_ptr<barrier::FileTransferSendState> state(
        new barrier::FileTransferSendState(transferId));
    client.testSetSendFileTransactionState(state);
    client.testSetSendFileProtocolState(false, false);

    EXPECT_FALSE(client.testCompletedSendFileMayRetire());
    ASSERT_TRUE(state->fail(barrier::FileTransferReason::kIoError));
    EXPECT_TRUE(client.testCompletedSendFileMayRetire());
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
    client.testSetProtocolMinorVersion(12);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    proxy->testBindTransactionalFileTransfer(
        "00112233445566778899aabbccddeeff");
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);
    UInt32 bulkDeletedCount = 0;
    client.testAttachBulkStream(new CountingStream(&bulkDeletedCount));

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
    EXPECT_EQ(barrier::FileTransferProtocol::makeTransferId(
                  barrier::FileTransferRole::kSecondary, 1),
              client.testSendFileTransferId());
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
    client.testSetProtocolMinorVersion(12);
    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    proxy->testBindTransactionalFileTransfer(
        "00112233445566778899aabbccddeeff");
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);
    UInt32 bulkDeletedCount = 0;
    client.testAttachBulkStream(new CountingStream(&bulkDeletedCount));
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
    EXPECT_EQ(barrier::FileTransferProtocol::makeTransferId(
                  barrier::FileTransferRole::kSecondary, 1),
              client.testSendFileTransferId());
    EXPECT_TRUE(client.testHasSendFileThread());
    for (int i = 0; i < 200 && !client.testCleanupSendFileThread(true); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(client.testCleanupSendFileThread(true));
}

TEST(ClientDisconnectTests, protocol19PrefetchIsRejectedWithoutLegacyTransfer)
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
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(9);
    UInt32 controlDeletedCount = 0;
    CountingStream* controlStream = new CountingStream(&controlDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, controlStream, &events);
    client.testSetStreamOnly(controlStream);
    client.testSetServerProxy(proxy);

    const std::vector<barrier::fs::path> paths(
        1, barrier::fs::u8path("/tmp/clipboard.txt"));
    client.testSendClipboardSelectionToServer(paths);

    EXPECT_TRUE(client.testPendingFileClipboardPrefetchPaths().empty());
    EXPECT_EQ(0u, client.testSendFileTransferId());
    EXPECT_FALSE(client.testHasSendFileThread());

    UInt32 bulkDeletedCount = 0;
    client.testAttachBulkStream(new CountingStream(&bulkDeletedCount));
    client.testHandleFileKeepAlive();

    EXPECT_TRUE(client.testPendingFileClipboardPrefetchPaths().empty());
    EXPECT_EQ(0u, client.testSendFileTransferId());
    EXPECT_FALSE(client.testHasSendFileThread());
}

TEST(ClientDisconnectTests, legacyManualFileAndDragRequestsDoNotStartTransfer)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents,
                                    fileEvents, streamEvents, clipboardEvents,
                                    dataSocketEvents, socketEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(11);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0u));
    EXPECT_CALL(*stream, write(_, _)).Times(0);
    EXPECT_CALL(*stream, writeLowPriority(_, _)).Times(0);
    ServerProxy* proxy = new ServerProxy(&client, stream, &events, 11);
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);

    client.sendFileToServer("/tmp/weave-legacy-file-must-not-be-opened");
    std::string dragInfo("/tmp/weave-legacy-drag-must-not-be-opened");
    client.sendDragInfo(1, dragInfo, dragInfo.size());

    EXPECT_FALSE(client.testHasSendFileThread());
    EXPECT_EQ(0u, client.testSendFileTransferId());
    EXPECT_TRUE(client.testPendingManualFileSend().empty());
}

TEST(ClientDisconnectTests, protocol19BulkConnectionFailureKeepsRetryingWithBackoff)
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

    std::vector<double> retryDelays;
    std::vector<EventQueueTimer*> deletedTimers;
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Invoke([&retryDelays](double delay, void*) {
            retryDelays.push_back(delay);
            return reinterpret_cast<EventQueueTimer*>(
                static_cast<uintptr_t>(retryDelays.size()));
        }));
    ON_CALL(events, deleteTimer(_))
        .WillByDefault(Invoke([&deletedTimers](EventQueueTimer* timer) {
            deletedTimers.push_back(timer);
        }));

    TestScreen screen;
    NetworkAddress serverAddress("127.0.0.1", 24800);
    serverAddress.resolve();
    Client client(&events, "client", serverAddress, new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(9);
    UInt32 controlDeletedCount = 0;
    CountingStream* controlStream = new CountingStream(&controlDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, controlStream, &events);
    client.testSetStreamOnly(controlStream);
    client.testSetServerProxy(proxy);

    const std::size_t baselineTimers = retryDelays.size();
    const std::size_t baselineDeletedTimers = deletedTimers.size();
    client.testConnectBulkChannel("binding-token");

    ASSERT_EQ(baselineTimers + 1u, retryDelays.size());
    EXPECT_DOUBLE_EQ(0.25, retryDelays[baselineTimers]);
    EXPECT_TRUE(client.testHasBulkRetryTimer());
    EXPECT_EQ(1u, client.testBulkRetryAttempt());

    client.testHandleBulkRetry();

    ASSERT_EQ(baselineTimers + 2u, retryDelays.size());
    EXPECT_DOUBLE_EQ(0.5, retryDelays[baselineTimers + 1u]);
    ASSERT_EQ(baselineDeletedTimers + 1u, deletedTimers.size());
    EXPECT_EQ(reinterpret_cast<EventQueueTimer*>(
                  static_cast<uintptr_t>(baselineTimers + 1u)),
              deletedTimers[baselineDeletedTimers]);
    EXPECT_TRUE(client.testHasBulkRetryTimer());
    EXPECT_EQ(2u, client.testBulkRetryAttempt());
}

TEST(ClientDisconnectTests, protocol112RejectsLegacyOrChangedControlBinding)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents,
                                    fileEvents, streamEvents, clipboardEvents,
                                    dataSocketEvents, socketEvents);

    TestScreen screen;
    NetworkAddress serverAddress("127.0.0.1", 24800);
    serverAddress.resolve();
    Client client(&events, "client", serverAddress,
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    UInt32 controlDeletedCount = 0;
    CountingStream* controlStream = new CountingStream(&controlDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, controlStream, &events);
    client.testSetStreamOnly(controlStream);
    client.testSetServerProxy(proxy);

    client.testConnectBulkChannel("legacy-token");
    EXPECT_TRUE(client.testControlConnectionBinding().empty());
    EXPECT_TRUE(client.testBulkRetryToken().empty());

    const std::string binding("00112233445566778899aabbccddeeff");
    EXPECT_TRUE(client.testConnectBoundBulkChannel("bound-token", binding));
    EXPECT_EQ(binding, client.testControlConnectionBinding());
    EXPECT_EQ("bound-token", client.testBulkRetryToken());

    EXPECT_FALSE(client.testConnectBoundBulkChannel(
        "replacement-token", "ffeeddccbbaa99887766554433221100"));
    EXPECT_EQ(binding, client.testControlConnectionBinding());
    EXPECT_EQ("bound-token", client.testBulkRetryToken());
}

TEST(ClientDisconnectTests, protocol112ControlOfferInstallsBindingBeforeBulkDial)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents,
                                    fileEvents, streamEvents, clipboardEvents,
                                    dataSocketEvents, socketEvents);

    TestScreen screen;
    NetworkAddress serverAddress("127.0.0.1", 24800);
    serverAddress.resolve();
    Client client(&events, "client", serverAddress,
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);

    const std::string token("one-time-token");
    const std::string binding("00112233445566778899aabbccddeeff");
    BulkHandshakeStream encoded;
    ProtocolUtil::writef(&encoded, kMsgCBulkOffer1_12, &token, &binding);
    BulkHandshakeStream* controlStream = new BulkHandshakeStream();
    controlStream->queueInput(encoded.output);
    ServerProxy* proxy = new ServerProxy(
        &client, controlStream, &events, 12);
    client.testSetStreamOnly(controlStream);
    client.testSetServerProxy(proxy);

    proxy->handleDataForTest();

    EXPECT_EQ(binding, client.testControlConnectionBinding());
    EXPECT_EQ(token, client.testBulkRetryToken());
    EXPECT_EQ(binding, proxy->getConnectionBinding());
}

TEST(ClientDisconnectTests, protocol112BulkHandshakeWritesBoundHello)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    IStreamEvents streamEvents;
    ClipboardEvents clipboardEvents;
    IDataSocketEvents dataSocketEvents;
    ISocketEvents socketEvents;
    setConnectedClientEventDefaults(events, clientEvents, screenEvents,
                                    fileEvents, streamEvents, clipboardEvents,
                                    dataSocketEvents, socketEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(),
                  new DummySocketFactory(), &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    const std::string token("one-time-token");
    const std::string binding("00112233445566778899aabbccddeeff");
    client.testSetControlConnectionBinding(binding);

    BulkHandshakeStream serverHello;
    ProtocolUtil::writef(&serverHello, kMsgHello,
                         kProtocolMajorVersion, 12);
    BulkHandshakeStream* handshake = new BulkHandshakeStream();
    handshake->queueInput(serverHello.output);
    client.testSetBulkHandshake(handshake, token);
    client.testHandleBulkHandshakeData();

    BulkHandshakeStream decoded;
    decoded.queueInput(handshake->output);
    UInt8 code[4] = {};
    ASSERT_EQ(4u, decoded.read(code, 4));
    EXPECT_EQ(0, std::memcmp(code, kMsgHelloBulkBack1_12, 4));
    SInt16 major = 0;
    SInt16 minor = 0;
    std::string name;
    std::string decodedToken;
    std::string decodedBinding;
    ASSERT_TRUE(ProtocolUtil::readf(
        &decoded, kMsgHelloBulkBack1_12 + 4,
        &major, &minor, &name, &decodedToken, &decodedBinding));
    EXPECT_EQ(kProtocolMajorVersion, major);
    EXPECT_EQ(12, minor);
    EXPECT_EQ("client", name);
    EXPECT_EQ(token, decodedToken);
    EXPECT_EQ(binding, decodedBinding);
}

TEST(ClientDisconnectTests, sameInFlightBulkOfferIsIgnoredButNewTokenReplacesHandshake)
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
    NetworkAddress serverAddress("127.0.0.1", 24800);
    serverAddress.resolve();
    Client client(&events, "client", serverAddress, new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(9);

    UInt32 controlDeletedCount = 0;
    CountingStream* controlStream = new CountingStream(&controlDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, controlStream, &events);
    client.testSetStreamOnly(controlStream);
    client.testSetServerProxy(proxy);

    UInt32 handshakeDeletedCount = 0;
    client.testSetBulkHandshake(new CountingStream(&handshakeDeletedCount),
                                "in-flight-token");

    client.testConnectBulkChannel("in-flight-token");

    EXPECT_EQ(0u, handshakeDeletedCount);
    EXPECT_EQ("in-flight-token", client.testBulkBindingToken());
    EXPECT_FALSE(client.testHasBulkRetryTimer());

    client.testConnectBulkChannel("new-token");

    EXPECT_EQ(1u, handshakeDeletedCount);
    EXPECT_TRUE(client.testBulkBindingToken().empty());
    EXPECT_EQ("new-token", client.testBulkRetryToken());
    EXPECT_TRUE(client.testHasBulkRetryTimer());
}

TEST(ClientDisconnectTests, newestBulkOfferSurvivesActiveRouteAndRestartsPendingManualSend)
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
    NetworkAddress serverAddress("127.0.0.1", 24800);
    serverAddress.resolve();
    Client client(&events, "client", serverAddress, new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);

    UInt32 controlDeletedCount = 0;
    CountingStream* controlStream = new CountingStream(&controlDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, controlStream, &events);
    proxy->testBindTransactionalFileTransfer(
        "00112233445566778899aabbccddeeff");
    client.testSetStreamOnly(controlStream);
    client.testSetServerProxy(proxy);

    UInt32 oldBulkDeletedCount = 0;
    client.testAttachBulkStream(new CountingStream(&oldBulkDeletedCount),
                                "active-token");
    std::shared_ptr<barrier::BulkChannel> oldChannel =
        client.acquireBulkChannel();
    ASSERT_TRUE(oldChannel);

    const std::string binding("00112233445566778899aabbccddeeff");
    ASSERT_TRUE(client.testConnectBoundBulkChannel(
        "replacement-token-1", binding));
    ASSERT_TRUE(client.testConnectBoundBulkChannel(
        "replacement-token-2", binding));
    ASSERT_TRUE(client.testConnectBoundBulkChannel("active-token", binding));
    EXPECT_EQ("replacement-token-2", client.testBulkRetryToken());

    oldChannel->close();
    client.handleBulkDisconnected(oldChannel.get());
    EXPECT_TRUE(client.testHasBulkRetryTimer());

    const std::string pendingPath = "/tmp/weave-bulk-token-race.txt";
    client.sendFileToServer(pendingPath);
    EXPECT_EQ(pendingPath, client.testPendingManualFileSend());

    client.testHandleBulkRetry();
    EXPECT_EQ("replacement-token-2", client.testBulkRetryToken());
    EXPECT_TRUE(client.testHasBulkRetryTimer());

    UInt32 replacementBulkDeletedCount = 0;
    client.testAttachBulkStream(new CountingStream(&replacementBulkDeletedCount));
    client.testHandleFileKeepAlive();

    EXPECT_TRUE(client.testPendingManualFileSend().empty());
    EXPECT_EQ(barrier::FileTransferProtocol::makeTransferId(
                  barrier::FileTransferRole::kSecondary, 1),
              client.testSendFileTransferId());
    for (int i = 0; i < 200 && !client.testCleanupSendFileThread(true); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(client.testCleanupSendFileThread(true));
}

TEST(ClientDisconnectTests, bulkDisconnectInterruptsPinnedFileSenderWithoutControlFallback)
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
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(9);

    NiceMock<MockStream>* controlStream = new NiceMock<MockStream>();
    ON_CALL(*controlStream, getEventTarget()).WillByDefault(Return(controlStream));
    ON_CALL(*controlStream, getBufferedOutputSize()).WillByDefault(Return(0u));
    EXPECT_CALL(*controlStream, writeLowPriority(_, _)).Times(0);
    ServerProxy* proxy = new ServerProxy(&client, controlStream, &events);
    client.testSetStreamOnly(controlStream);
    client.testSetServerProxy(proxy);

    NiceMock<MockStream>* bulkStream = new NiceMock<MockStream>();
    ON_CALL(*bulkStream, getEventTarget()).WillByDefault(Return(bulkStream));
    ON_CALL(*bulkStream, getBufferedOutputSize()).WillByDefault(Return(0u));
    EXPECT_CALL(*bulkStream, writeLowPriority(_, _)).Times(0);
    client.testAttachBulkStream(bulkStream);
    std::shared_ptr<barrier::BulkChannel> channel = client.acquireBulkChannel();
    ASSERT_TRUE(channel);

    std::shared_ptr<StreamChunker> chunker(new StreamChunker());
    client.testSetSendFileTransferId(17);
    client.testSetSendFileChunker(chunker);
    client.testSetSendFileBulkChannel(channel);

    NiceMock<MockStream>* replacementStream = new NiceMock<MockStream>();
    ON_CALL(*replacementStream, getEventTarget())
        .WillByDefault(Return(replacementStream));
    ON_CALL(*replacementStream, getBufferedOutputSize())
        .WillByDefault(Return(0u));
    client.testAttachBulkStream(replacementStream);

    channel->close();
    client.handleBulkDisconnected(channel.get());

    FileChunk* chunk = FileChunk::data(
        reinterpret_cast<const UInt8*>("payload"), 7);
    chunk->m_transferId = 17;
    client.testSendFileChunk(chunk);

    EXPECT_TRUE(chunker->testShouldInterrupt());
    delete chunk;
}

TEST(ClientDisconnectTests, protocol112AllocatesSecondaryTransferIds)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);

    const UInt32 first = client.testAllocateSendFileTransferId();
    const UInt32 second = client.testAllocateSendFileTransferId();

    EXPECT_EQ(barrier::FileTransferRole::kSecondary,
              barrier::FileTransferProtocol::transferRole(first));
    EXPECT_EQ(1u, barrier::FileTransferProtocol::transferSequence(first));
    EXPECT_EQ(2u, barrier::FileTransferProtocol::transferSequence(second));
}

TEST(ClientDisconnectTests, transactionalManualAndDragReceivePreserveClipboardRevision)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    const std::string binding = "00112233445566778899aabbccddeeff";
    const std::uint64_t revisionBefore =
        client.testClipboardRevisionSequence();

    const UInt32 manualId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 1);
    ASSERT_EQ(barrier::FileTransferReason::kNone,
              client.beginTransactionalFileReceive(
                  barrier::FileTransferFrame::start(
                      binding, manualId, 7,
                      barrier::FileTransferKind::kManual)));
    EXPECT_EQ(barrier::FileTransferKind::kManual,
              client.testTransactionalReceiveKind());
    EXPECT_EQ(revisionBefore, client.testClipboardRevisionSequence());
    client.cancelTransactionalFileReceive(manualId);

    const UInt32 dragId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 2);
    ASSERT_EQ(barrier::FileTransferReason::kNone,
              client.beginTransactionalFileReceive(
                  barrier::FileTransferFrame::start(
                      binding, dragId, 7,
                      barrier::FileTransferKind::kDrag)));
    EXPECT_EQ(barrier::FileTransferKind::kDrag,
              client.testTransactionalReceiveKind());
    EXPECT_EQ(revisionBefore, client.testClipboardRevisionSequence());
    client.cancelTransactionalFileReceive(dragId);
}

TEST(ClientDisconnectTests, protocol112ClipboardPrefetchPinsRevisionAndSession)
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

    const std::string binding = "00112233445566778899aabbccddeeff";
    const std::string session = "5123456789abcdef0123456789abcdef";
    TestScreen screen;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    BulkHandshakeStream* control = new BulkHandshakeStream();
    ServerProxy* proxy = new ServerProxy(&client, control, &events, 12);
    proxy->testBindTransactionalFileTransfer(binding);
    client.testSetStreamOnly(control);
    client.testSetServerProxy(proxy);
    client.testAttachBulkStream(new BulkHandshakeStream());

    client.testSendClipboardSelectionToServer(
        {barrier::fs::u8path("/tmp/weave-missing-prefetch.txt")}, session);

    std::shared_ptr<barrier::FileTransferSendState> state =
        client.testSendFileTransactionState();
    ASSERT_TRUE(state);
    EXPECT_EQ(barrier::FileTransferKind::kClipboard, state->kind());
    EXPECT_EQ(client.testClipboardRevisionSequence(),
              state->clipboardRevision());
    EXPECT_EQ(session, state->clipboardSessionId());

    for (int i = 0; i < 200 && !client.testCleanupSendFileThread(true); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(client.testCleanupSendFileThread(true));
}

TEST(ClientDisconnectTests, protocol112RejectsInvalidClipboardPrefetchSession)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    TestScreen screen;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    client.testSendClipboardSelectionToServer(
        {barrier::fs::u8path("/tmp/clipboard.txt")},
        "ABCDEF0123456789ABCDEF0123456789");

    EXPECT_TRUE(client.testPendingFileClipboardPrefetchPaths().empty());
    EXPECT_FALSE(client.testHasSendFileTransactionState());
}

TEST(ClientDisconnectTests, protocol112ManualSendDeclaresManualIdentity)
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

    const std::string binding = "00112233445566778899aabbccddeeff";
    TestScreen screen;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    BulkHandshakeStream* control = new BulkHandshakeStream();
    ServerProxy* proxy = new ServerProxy(&client, control, &events, 12);
    proxy->testBindTransactionalFileTransfer(binding);
    client.testSetStreamOnly(control);
    client.testSetServerProxy(proxy);
    client.testAttachBulkStream(new BulkHandshakeStream());

    client.sendFileToServer("/tmp/weave-missing-manual.txt");

    std::shared_ptr<barrier::FileTransferSendState> state =
        client.testSendFileTransactionState();
    ASSERT_TRUE(state);
    EXPECT_EQ(barrier::FileTransferKind::kManual, state->kind());
    EXPECT_EQ(0u, state->clipboardRevision());
    EXPECT_TRUE(state->clipboardSessionId().empty());

    for (int i = 0; i < 200 && !client.testCleanupSendFileThread(true); ++i) {
        ARCH->sleep(0.001);
    }
    EXPECT_TRUE(client.testCleanupSendFileThread(true));
}

TEST(ClientDisconnectTests, protocol112RoutesControlAndBulkFramesWithoutFallback)
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

    const std::string binding = "00112233445566778899aabbccddeeff";
    TestScreen screen;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    BulkHandshakeStream* control = new BulkHandshakeStream();
    ServerProxy* proxy = new ServerProxy(&client, control, &events, 12);
    proxy->testBindTransactionalFileTransfer(binding);
    client.testSetStreamOnly(control);
    client.testSetServerProxy(proxy);
    BulkHandshakeStream* bulk = new BulkHandshakeStream();
    client.testAttachBulkStream(bulk);

    const UInt32 transferId = client.testAllocateSendFileTransferId();
    const std::string clipboardSession =
        "4123456789abcdef0123456789abcdef";
    std::shared_ptr<barrier::FileTransferSendState> state(
        new barrier::FileTransferSendState(
            transferId, barrier::FileTransferKind::kClipboard, 17,
            clipboardSession));
    ASSERT_TRUE(state->markStartQueued(3));
    client.testSetSendFileTransactionState(state);
    client.testSetSendFileBulkChannel(client.acquireBulkChannel());

    FileChunk* start = FileChunk::start("3", true);
    start->m_transferId = transferId;
    client.testSendFileChunk(start);
    ASSERT_GE(control->output.size(), 4u);
    EXPECT_EQ(0, std::memcmp(control->output.data(),
                             kMsgDFileTransferStart1_12, 4));
    EXPECT_TRUE(bulk->output.empty());
    BulkHandshakeStream encodedStart;
    encodedStart.queueInput(std::vector<UInt8>(
        control->output.begin() + 4, control->output.end()));
    barrier::FileTransferFrame decodedStart;
    ASSERT_TRUE(barrier::FileTransferProtocol::decode(
        control->output.data(), &encodedStart,
        barrier::FileTransferRole::kSecondary, binding, decodedStart));
    EXPECT_EQ(barrier::FileTransferKind::kClipboard, decodedStart.kind);
    EXPECT_EQ(17u, decodedStart.clipboardRevision);
    EXPECT_EQ(clipboardSession, decodedStart.clipboardSessionId);
    delete start;

    ASSERT_TRUE(state->signalStartAck(
        transferId, barrier::FileTransferReason::kNone));
    ASSERT_TRUE(state->markDataQueued(0, 3));
    FileChunk* data = FileChunk::data(
        reinterpret_cast<const UInt8*>("abc"), 3, 0);
    data->m_transferId = transferId;
    client.testSendFileChunk(data);
    ASSERT_GE(bulk->output.size(), 4u);
    EXPECT_EQ(0, std::memcmp(bulk->output.data(),
                             kMsgDFileTransferData1_12, 4));
    delete data;

    const std::size_t dataFrameBytes = bulk->output.size();
    ASSERT_TRUE(state->markEndQueued(3));
    FileChunk* end = FileChunk::end(
        "sha256:dddddddddddddddddddddddddddddddd"
        "dddddddddddddddddddddddddddddddd", 3);
    end->m_transferId = transferId;
    client.testSendFileChunk(end);
    ASSERT_GE(bulk->output.size(), dataFrameBytes + 4u);
    EXPECT_EQ(0, std::memcmp(bulk->output.data() + dataFrameBytes,
                             kMsgDFileTransferEnd1_12, 4));
    delete end;
}

TEST(ClientDisconnectTests, protocol112BulkDisconnectWakesTransactionWaiter)
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
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    UInt32 controlDeleted = 0;
    CountingStream* control = new CountingStream(&controlDeleted);
    ServerProxy* proxy = new ServerProxy(&client, control, &events, 12);
    client.testSetStreamOnly(control);
    client.testSetServerProxy(proxy);
    UInt32 bulkDeleted = 0;
    client.testAttachBulkStream(new CountingStream(&bulkDeleted));
    std::shared_ptr<barrier::BulkChannel> channel = client.acquireBulkChannel();
    ASSERT_TRUE(channel);

    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 4);
    std::shared_ptr<barrier::FileTransferSendState> state(
        new barrier::FileTransferSendState(transferId));
    ASSERT_TRUE(state->markStartQueued(1));
    client.testSetSendFileTransferId(transferId);
    client.testSetSendFileTransactionState(state);
    client.testSetSendFileBulkChannel(channel);

    client.handleBulkDisconnected(channel.get());

    EXPECT_TRUE(state->stopped());
    EXPECT_EQ(barrier::FileTransferReason::kConnectionLost, state->result());
}

TEST(ClientDisconnectTests, protocol112CancelCompletionPollsUntilBulkBacklogDrains)
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
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    UInt32 deletedCount = 0;
    AdjustableBufferedStream* bulk =
        new AdjustableBufferedStream(&deletedCount);
    bulk->bufferedOutput = 4096;
    client.testAttachBulkStream(bulk);

    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 5);
    std::shared_ptr<barrier::FileTransferSendState> state(
        new barrier::FileTransferSendState(transferId));
    ASSERT_TRUE(state->markStartQueued(4096));
    ASSERT_TRUE(state->signalStartAck(
        transferId, barrier::FileTransferReason::kNone));
    state->interrupt();
    client.testSetSendFileTransferId(transferId);
    client.testSetSendFileTransactionState(state);
    client.testSetSendFileBulkChannel(client.acquireBulkChannel());
    client.testSetSendFileProtocolState(true, true);

    client.testServiceSendFileCompletion();

    EXPECT_TRUE(client.testHasSendFileTransactionState());
    EXPECT_TRUE(client.testHasSendFileReapTimer());

    bulk->bufferedOutput = 0;
    client.testHandleSendFileReap();

    EXPECT_FALSE(client.testHasSendFileTransactionState());
    EXPECT_FALSE(client.testHasSendFileReapTimer());
}

TEST(ClientDisconnectTests, protocol112StalledCancelDrainFailsBulkRouteClosed)
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
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    client.testSetProtocolMinorVersion(12);
    UInt32 deletedCount = 0;
    AdjustableBufferedStream* bulk =
        new AdjustableBufferedStream(&deletedCount);
    bulk->bufferedOutput = 4096;
    client.testAttachBulkStream(bulk);

    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 6);
    std::shared_ptr<barrier::FileTransferSendState> state(
        new barrier::FileTransferSendState(transferId));
    ASSERT_TRUE(state->markStartQueued(4096));
    ASSERT_TRUE(state->signalStartAck(
        transferId, barrier::FileTransferReason::kNone));
    state->interrupt();
    client.testSetSendFileTransferId(transferId);
    client.testSetSendFileTransactionState(state);
    client.testSetSendFileBulkChannel(client.acquireBulkChannel());
    client.testSetSendFileProtocolState(true, true);
    client.testSetSendFileDrainPollState(3000, 500, 4096);

    client.testServiceSendFileCompletion();

    EXPECT_FALSE(client.acquireBulkChannel());
    EXPECT_FALSE(client.testHasSendFileTransactionState());
    EXPECT_FALSE(client.testHasSendFileReapTimer());
}

TEST(ClientDisconnectTests, failedBulkInputPauseReleasesReceiveGeneration)
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
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    UInt32 bulkDeletedCount = 0;
    client.testAttachBulkStream(new CountingStream(&bulkDeletedCount));
    std::shared_ptr<barrier::BulkChannel> channel = client.acquireBulkChannel();
    ASSERT_TRUE(channel);

    FileReceiveSession& session = client.getFileReceiveSession();
    ASSERT_TRUE(session.begin(4, FileChunk::kMemoryReceiveLimit, 64));
    const std::uint64_t failedGeneration = session.generation();

    client.handleBulkInputPauseFailed(channel.get(), failedGeneration);

    EXPECT_EQ(FileReceiveSession::kIdle, session.state());
    EXPECT_FALSE(session.matchesGeneration(failedGeneration));
    EXPECT_TRUE(session.begin(4, FileChunk::kMemoryReceiveLimit, 64));
}

TEST(ClientDisconnectTests, completedBulkInputPauseFailureReleasesReceiveGeneration)
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
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    UInt32 bulkDeletedCount = 0;
    client.testAttachBulkStream(new CountingStream(&bulkDeletedCount));
    std::shared_ptr<barrier::BulkChannel> channel = client.acquireBulkChannel();
    ASSERT_TRUE(channel);

    FileReceiveSession& session = client.getFileReceiveSession();
    ASSERT_TRUE(session.begin(0, FileChunk::kMemoryReceiveLimit, 64));
    ASSERT_TRUE(session.finish());
    ASSERT_EQ(FileReceiveSession::kComplete, session.state());
    const std::uint64_t failedGeneration = session.generation();

    client.handleBulkDisconnected(channel.get(), failedGeneration);

    EXPECT_EQ(FileReceiveSession::kIdle, session.state());
    EXPECT_FALSE(session.matchesGeneration(failedGeneration));
    EXPECT_TRUE(session.begin(4, FileChunk::kMemoryReceiveLimit, 64));
}

TEST(ClientDisconnectTests, staleBulkInputPauseFailureDoesNotResetNewReceive)
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
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, ClientArgs());
    UInt32 bulkDeletedCount = 0;
    client.testAttachBulkStream(new CountingStream(&bulkDeletedCount));
    std::shared_ptr<barrier::BulkChannel> channel = client.acquireBulkChannel();
    ASSERT_TRUE(channel);

    FileReceiveSession& session = client.getFileReceiveSession();
    ASSERT_TRUE(session.begin(3, FileChunk::kMemoryReceiveLimit, 64));
    const std::uint64_t staleGeneration = session.generation();
    session.reset();
    ASSERT_TRUE(session.begin(4, FileChunk::kMemoryReceiveLimit, 64));
    const std::uint64_t currentGeneration = session.generation();

    client.handleBulkDisconnected(channel.get(), staleGeneration);

    EXPECT_TRUE(session.matchesGeneration(currentGeneration));
    EXPECT_EQ(FileReceiveSession::kReceiving, session.state());
    EXPECT_EQ(4u, session.expectedSize());
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

TEST(ClientDisconnectTests, manualAndDragDropsPreserveClipboardWhileWritingFiles)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    UInt32 publishRequests = 0;
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&publishRequests](const Event& event) {
            if (event.getDataObject() != NULL) {
                Client::FileClipboardReadyInfo* info =
                    static_cast<Client::FileClipboardReadyInfo*>(
                        event.getDataObject());
                if (info->m_publishClipboard) {
                    ++publishRequests;
                }
            }
            Event::deleteData(event);
        }));

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    const barrier::fs::path root =
        barrier::fs::temp_directory_path() /
        barrier::fs::u8path("weave-client-transfer-kind-clipboard-test");
    barrier::fs::remove_all(root);
    barrier::fs::create_directories(root);

    client.testWriteDroppedFileTransfer(
        barrier::FileTransferKind::kManual, root.u8string(),
        "manual.txt", "manual payload");
    client.testWriteDroppedFileTransfer(
        barrier::FileTransferKind::kDrag, root.u8string(),
        "drag.txt", "drag payload");

    EXPECT_EQ(0u, publishRequests);
    EXPECT_TRUE(barrier::fs::exists(root / "manual.txt"));
    EXPECT_TRUE(barrier::fs::exists(root / "drag.txt"));

    client.testWriteDroppedFileTransfer(
        barrier::FileTransferKind::kClipboard, root.u8string(),
        "clipboard.txt", "clipboard payload");

    EXPECT_EQ(1u, publishRequests);
    EXPECT_TRUE(barrier::fs::exists(root / "clipboard.txt"));
    barrier::fs::remove_all(root);
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

TEST(ClientDisconnectTests, generationlessFileChunkDoesNotWriteOnLegacyConnection)
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
    client.testSetProtocolMinorVersion(11);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));

    EXPECT_CALL(*stream, write(_, _)).Times(0);
    EXPECT_CALL(*stream, writeLowPriority(_, _)).Times(0);

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
    payload.sessionId = kRemoteFileSessionId;
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
    client.testHandleFileClipboardReady(kRemoteFileSessionId, readyPaths, false);

    EXPECT_TRUE(client.testRemoteFileClipboardSession().empty());
    EXPECT_TRUE(client.testReadyFileClipboardSession().empty());
    EXPECT_TRUE(client.testReadyFileClipboardPaths().empty());
    EXPECT_EQ(0u, platform->setClipboardCount);
}

TEST(ClientDisconnectTests, remoteClipboardGrabSupersedesPendingRemoteFileClipboard)
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

    std::vector<std::string> readyPaths(1, "/tmp/materialized-a.txt");
    client.testSetFileClipboardSessions("remote-file-a", "remote-file-a",
                                        readyPaths);
    const std::uint64_t previousRevision =
        client.testClipboardRevisionSequence();

    client.grabClipboard(kClipboardClipboard);
    client.testHandleFileClipboardReady("remote-file-a", readyPaths, false);

    EXPECT_GT(client.testClipboardRevisionSequence(), previousRevision);
    EXPECT_TRUE(client.testRemoteFileClipboardSession().empty());
    EXPECT_TRUE(client.testReadyFileClipboardSession().empty());
    EXPECT_TRUE(client.testReadyFileClipboardPaths().empty());
    EXPECT_EQ(1u, platform->setClipboardCount);
    EXPECT_TRUE(platform->lastSetClipboardWasNull);
}

TEST(ClientDisconnectTests, legacyLocalFileListClipboardDoesNotSendMetadataOrPackage)
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
    client.testSetProtocolMinorVersion(8);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    proxy->result = ServerProxy::kClipboardSendQueued;
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);

    client.testSendClipboard(kClipboardClipboard);

    EXPECT_EQ(0u, proxy->sendCalls);
    EXPECT_FALSE(client.testClipboardSent(kClipboardClipboard));
    EXPECT_FALSE(client.testHasSendFileThread());
    EXPECT_TRUE(client.testPendingFileClipboardPrefetchPaths().empty());
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

TEST(ClientDisconnectTests,
     asyncMaterializedClipboardQueueWaitsForPlatformCommit)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    platform->asyncClipboardPublications = true;
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    std::vector<std::string> readyPaths(1, "/tmp/materialized.txt");
    client.testSetFileClipboardSessions(kRemoteFileSessionId, kRemoteFileSessionId,
                                        readyPaths);
    client.testHandleFileClipboardReady(kRemoteFileSessionId, readyPaths, true);

    ASSERT_NE(0u, platform->lastClipboardPublicationId);
    EXPECT_EQ(platform->lastClipboardPublicationId,
              client.testPendingClipboardPublicationId());
    EXPECT_EQ(kRemoteFileSessionId, client.testRemoteFileClipboardSession());
    EXPECT_FALSE(client.testMaterializedClipboardCommitted());

    client.testHandleClipboardPublished(
        platform->lastClipboardPublicationId,
        IScreen::ClipboardPublicationResult::Succeeded);

    EXPECT_EQ(0u, client.testPendingClipboardPublicationId());
    EXPECT_TRUE(client.testRemoteFileClipboardSession().empty());
    EXPECT_TRUE(client.testMaterializedClipboardCommitted());
}

TEST(ClientDisconnectTests,
     failedAsyncMaterializedClipboardRetainsPendingSession)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    platform->asyncClipboardPublications = true;
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    std::vector<std::string> readyPaths(1, "/tmp/materialized.txt");
    client.testSetFileClipboardSessions(kRemoteFileSessionId, kRemoteFileSessionId,
                                        readyPaths);
    client.testHandleFileClipboardReady(kRemoteFileSessionId, readyPaths, true);
    const std::uint64_t publicationId =
        client.testPendingClipboardPublicationId();
    ASSERT_NE(0u, publicationId);

    client.testHandleClipboardPublished(
        publicationId, IScreen::ClipboardPublicationResult::Failed);

    EXPECT_EQ(0u, client.testPendingClipboardPublicationId());
    EXPECT_EQ(kRemoteFileSessionId, client.testRemoteFileClipboardSession());
    EXPECT_FALSE(client.testMaterializedClipboardCommitted());
}

TEST(ClientDisconnectTests,
     failedSynchronousMaterializedClipboardRetainsPendingSession)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    platform->acceptClipboardSet = false;
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    std::vector<std::string> readyPaths(1, "/tmp/materialized.txt");
    client.testSetFileClipboardSessions(kRemoteFileSessionId, kRemoteFileSessionId,
                                        readyPaths);
    client.testHandleFileClipboardReady(kRemoteFileSessionId, readyPaths, true);

    EXPECT_EQ(kRemoteFileSessionId, client.testRemoteFileClipboardSession());
    EXPECT_FALSE(client.testMaterializedClipboardCommitted());
}

TEST(ClientDisconnectTests,
     supersededAsyncMaterializedClipboardCannotCommitNewRevision)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);

    EnterPlatformScreen* platform = new EnterPlatformScreen();
    platform->asyncClipboardPublications = true;
    barrier::Screen screen(platform, &events);
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);

    std::vector<std::string> readyPaths(1, "/tmp/materialized.txt");
    client.testSetFileClipboardSessions(kRemoteFileSessionId, kRemoteFileSessionId,
                                        readyPaths);
    client.testHandleFileClipboardReady(kRemoteFileSessionId, readyPaths, true);
    const std::uint64_t stalePublicationId =
        client.testPendingClipboardPublicationId();
    ASSERT_NE(0u, stalePublicationId);

    client.testSupersedeFileClipboard();
    client.testHandleClipboardPublished(
        stalePublicationId,
        IScreen::ClipboardPublicationResult::Succeeded);

    EXPECT_FALSE(client.testMaterializedClipboardCommitted());
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
    client.testSetClipboardOwnership(kClipboardClipboard, true);

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

TEST(ClientDisconnectTests, leaveDoesNotRetryClipboardWithoutLocalOwnership)
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
    ASSERT_TRUE(client.leave());
    client.testHandleClipboardRetry();

    EXPECT_FALSE(client.testClipboardRetryPending(kClipboardClipboard));
    EXPECT_EQ(0u, platform->getClipboardCount);
    EXPECT_EQ(0u, proxy->sendCalls);
}

TEST(ClientDisconnectTests, clipboardRetrySkipsWhenOwnershipWasLostBeforeTimer)
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
    client.testSetClipboardOwnership(kClipboardClipboard, true);

    client.enter(0, 0, 0, 0, false);
    ASSERT_TRUE(client.leave());
    ASSERT_TRUE(client.testClipboardRetryPending(kClipboardClipboard));

    client.testSetClipboardOwnership(kClipboardClipboard, false);
    client.testHandleClipboardRetry();

    EXPECT_FALSE(client.testClipboardRetryPending(kClipboardClipboard));
    EXPECT_EQ(0u, client.testClipboardRetryCount(kClipboardClipboard));
    EXPECT_EQ(0u, platform->getClipboardCount);
    EXPECT_EQ(0u, proxy->sendCalls);
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
    client.testSetClipboardOwnership(kClipboardClipboard, true);
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

TEST(ClientDisconnectTests, legacyImageClipboardStripsFileMetadataButStillSendsPng)
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
    client.testSetProtocolMinorVersion(11);

    UInt32 streamDeletedCount = 0;
    CountingStream* stream = new CountingStream(&streamDeletedCount);
    PendingClipboardServerProxy* proxy =
        new PendingClipboardServerProxy(&client, stream, &events);
    proxy->result = ServerProxy::kClipboardSendQueued;
    client.testSetStreamOnly(stream);
    client.testSetServerProxy(proxy);
    client.testSendClipboard(kClipboardClipboard);

    ASSERT_EQ(1u, proxy->sendCalls);
    ASSERT_TRUE(proxy->lastClipboard.open(0));
    EXPECT_TRUE(proxy->lastClipboard.has(IClipboard::kPNG));
    EXPECT_FALSE(proxy->lastClipboard.has(IClipboard::kFileList));
    EXPECT_FALSE(proxy->lastClipboard.has(IClipboard::kText));
    proxy->lastClipboard.close();
    EXPECT_FALSE(client.testHasSendFileThread());
}

TEST(ClientDisconnectTests, rejectedLegacyFileListDoesNotPoisonNextPlainTextClipboard)
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
    client.testSetProtocolMinorVersion(8);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));

    client.testAttachStream(stream);
    client.testSendClipboard(kClipboardClipboard);
    EXPECT_TRUE(clipboardMarks.empty());

    client.testSendClipboard(kClipboardClipboard);
    ASSERT_GE(clipboardMarks.size(), 3u);
    EXPECT_EQ(kDataStart, clipboardMarks.front());
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

TEST(ClientDisconnectTests, asyncSnapshotReadyWhileActiveDoesNotScheduleSend)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    ClipboardEvents clipboardEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);
    clipboardEvents.setEvents(&events);
    ON_CALL(events, forClipboard()).WillByDefault(ReturnRef(clipboardEvents));

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    client.testSetClipboardOwnership(kClipboardClipboard, true);
    client.testSetActive(true);

    client.testHandleClipboardChanged(kClipboardClipboard);

    EXPECT_FALSE(client.testClipboardRetryPending(kClipboardClipboard));
}

TEST(ClientDisconnectTests, asyncSnapshotReadyWhileInactiveSchedulesSend)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    ClipboardEvents clipboardEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);
    clipboardEvents.setEvents(&events);
    ON_CALL(events, forClipboard()).WillByDefault(ReturnRef(clipboardEvents));
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    client.testSetClipboardOwnership(kClipboardClipboard, true);
    client.testSetActive(false);

    client.testHandleClipboardChanged(kClipboardClipboard);

    EXPECT_TRUE(client.testClipboardRetryPending(kClipboardClipboard));
}

TEST(ClientDisconnectTests, asyncSnapshotCompletionRearmsExpiredRetryBudget)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    ClipboardEvents clipboardEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);
    clipboardEvents.setEvents(&events);
    ON_CALL(events, forClipboard()).WillByDefault(ReturnRef(clipboardEvents));
    ON_CALL(events, newOneShotTimer(_, _))
        .WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    client.testSetClipboardOwnership(kClipboardClipboard, true);
    client.testSetClipboardRetryCount(kClipboardClipboard, 20);

    client.testHandleClipboardChanged(kClipboardClipboard);

    EXPECT_TRUE(client.testClipboardRetryPending(kClipboardClipboard));
    EXPECT_EQ(0u, client.testClipboardRetryCount(kClipboardClipboard));
}

TEST(ClientDisconnectTests, asyncSnapshotCompletionForRemoteClipboardIsIgnored)
{
    NiceMock<MockEventQueue> events;
    ClientEvents clientEvents;
    IScreenEvents screenEvents;
    FileEvents fileEvents;
    ClipboardEvents clipboardEvents;
    setClientEventDefaults(events, clientEvents, screenEvents, fileEvents);
    clipboardEvents.setEvents(&events);
    ON_CALL(events, forClipboard()).WillByDefault(ReturnRef(clipboardEvents));

    TestScreen screen;
    ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new DummySocketFactory(),
                  &screen, args);
    client.testSetClipboardOwnership(kClipboardClipboard, false);

    client.testHandleClipboardChanged(kClipboardClipboard);

    EXPECT_FALSE(client.testClipboardRetryPending(kClipboardClipboard));
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

TEST(ClientDisconnectTests, legacyMaterializedFileListClipboardIsNotSentToServer)
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
    client.testSetProtocolMinorVersion(8);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));
    ON_CALL(*stream, getBufferedOutputSize()).WillByDefault(Return(0));

    client.testAttachStream(stream);
    EXPECT_CALL(events, addEvent(_)).Times(0);
    client.testSendClipboard(kClipboardClipboard);
    EXPECT_FALSE(client.testHasSendFileThread());
}
