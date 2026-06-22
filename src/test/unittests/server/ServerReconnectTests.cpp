#define BARRIER_TEST_ENV

#include "server/ClientProxy.h"
#include "server/ClientProxy1_6.h"
#include "server/Config.h"
#include "barrier/Clipboard.h"
#include "barrier/FileChunk.h"
#include "barrier/IPlatformScreen.h"
#include "barrier/RemoteFileClipboard.h"
#include "arch/Arch.h"
#include "base/IEventJob.h"
#include "io/filesystem.h"
#include "mt/Thread.h"
#include "mt/XThread.h"
#include "test/global/TestEventQueue.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/barrier/MockScreen.h"
#include "test/mock/io/MockStream.h"
#include "test/mock/server/MockConfig.h"
#include "test/mock/server/MockInputFilter.h"
#include "test/mock/server/MockPrimaryClient.h"
#include "test/global/gmock.h"
#include "test/global/gtest.h"

#include <atomic>
#include <algorithm>
#include <fstream>
#include <system_error>
#include <vector>

bool testServerPrepareTransferSource(const char* filename,
                                     barrier::fs::path& sourcePath,
                                     barrier::fs::path& tempPackagePath,
                                     std::string& error);

#define private public
#include "server/Server.h"
#undef private

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {

std::string invalidTransferPackageData()
{
    return std::string("BDIRPKG1F", 9);
}

class RecordingClient : public ClientProxy
{
public:
    explicit RecordingClient(const std::string& name, UInt32* deletedCount = NULL) :
        ClientProxy(name, new NiceMock<MockStream>()),
        deletedCount(deletedCount),
        shapeX(0),
        shapeY(0),
        shapeW(1024),
        shapeH(768),
        enterCount(0),
        enterX(0),
        enterY(0),
        enterSeqNum(0),
        enterMask(0),
        enterForScreensaver(true),
        mouseMoveCount(0),
        mouseMoveX(0),
        mouseMoveY(0),
        fileChunkCount(0),
        lastFileChunkMark(0),
        lastFileChunkSize(0),
        dragInfoCount(0),
        lastDragFileCount(0),
        lastDragInfoSize(0),
        clipboardAvailable(false),
        grabClipboardCount(0),
        lastGrabClipboardId(kClipboardEnd),
        clipboardDirtyCount(0),
        lastClipboardDirty(false),
        setClipboardCount(0)
    {
    }

    ~RecordingClient()
    {
        if (deletedCount != NULL) {
            ++(*deletedCount);
        }
    }

    void* getEventTarget() const override { return const_cast<RecordingClient*>(this); }
    bool getClipboard(ClipboardID, IClipboard* clipboard) const override
    {
        if (!clipboardAvailable) {
            return false;
        }
        static_cast<Clipboard*>(clipboard)->unmarshall(sourceClipboard.marshall(), 0);
        return true;
    }
    void getShape(SInt32& x, SInt32& y, SInt32& width, SInt32& height) const override
    {
        x = shapeX;
        y = shapeY;
        width = shapeW;
        height = shapeH;
    }
    void getCursorPos(SInt32& x, SInt32& y) const override
    {
        x = 100;
        y = 200;
    }

    void enter(SInt32 xAbs, SInt32 yAbs, UInt32 seqNum, KeyModifierMask mask,
               bool forScreensaver) override
    {
        ++enterCount;
        enterX = xAbs;
        enterY = yAbs;
        enterSeqNum = seqNum;
        enterMask = mask;
        enterForScreensaver = forScreensaver;
    }
    bool leave() override { return true; }
    void setClipboard(ClipboardID id, const IClipboard* clipboard) override
    {
        ++setClipboardCount;
        setClipboardIds.push_back(id);
        lastSetClipboard.unmarshall(IClipboard::marshall(clipboard), 0);
    }
    void grabClipboard(ClipboardID id) override
    {
        ++grabClipboardCount;
        lastGrabClipboardId = id;
    }
    void setClipboardDirty(ClipboardID, bool dirty) override
    {
        ++clipboardDirtyCount;
        lastClipboardDirty = dirty;
    }
    void keyDown(KeyID, KeyModifierMask, KeyButton) override { }
    void keyRepeat(KeyID, KeyModifierMask, SInt32, KeyButton) override { }
    void keyUp(KeyID, KeyModifierMask, KeyButton) override { }
    void mouseDown(ButtonID) override { }
    void mouseUp(ButtonID) override { }
    void mouseMove(SInt32 xAbs, SInt32 yAbs) override
    {
        ++mouseMoveCount;
        mouseMoveX = xAbs;
        mouseMoveY = yAbs;
    }
    void mouseRelativeMove(SInt32, SInt32) override { }
    void mouseWheel(SInt32, SInt32) override { }
    void screensaver(bool) override { }
    void resetOptions() override { }
    void setOptions(const OptionsList&) override { }
    void sendDragInfo(UInt32 fileCount, const char* info, size_t size) override
    {
        ++dragInfoCount;
        lastDragFileCount = fileCount;
        lastDragInfo.assign(info, size);
        lastDragInfoSize = size;
    }
    void fileChunkSending(UInt8 mark, char*, size_t dataSize) override
    {
        ++fileChunkCount;
        lastFileChunkMark = mark;
        lastFileChunkSize = dataSize;
    }

    UInt32* deletedCount;
    SInt32 shapeX;
    SInt32 shapeY;
    SInt32 shapeW;
    SInt32 shapeH;
    UInt32 enterCount;
    SInt32 enterX;
    SInt32 enterY;
    UInt32 enterSeqNum;
    KeyModifierMask enterMask;
    bool enterForScreensaver;
    UInt32 mouseMoveCount;
    SInt32 mouseMoveX;
    SInt32 mouseMoveY;
    UInt32 fileChunkCount;
    UInt8 lastFileChunkMark;
    size_t lastFileChunkSize;
    UInt32 dragInfoCount;
    UInt32 lastDragFileCount;
    std::string lastDragInfo;
    size_t lastDragInfoSize;
    Clipboard sourceClipboard;
    bool clipboardAvailable;
    UInt32 grabClipboardCount;
    ClipboardID lastGrabClipboardId;
    UInt32 clipboardDirtyCount;
    bool lastClipboardDirty;
    UInt32 setClipboardCount;
    std::vector<ClipboardID> setClipboardIds;
    Clipboard lastSetClipboard;
};

class DeferringClientProxy16 : public ClientProxy1_6
{
public:
    DeferringClientProxy16(const std::string& name,
                           barrier::IStream* stream,
                           Server* server,
                           IEventQueue* events,
                           UInt32* deletedCount) :
        ClientProxy1_6(name, stream, server, events),
        cleanupAllowed(false),
        cleanupCalls(0),
        deletedCount(deletedCount)
    {
    }

    ~DeferringClientProxy16()
    {
        if (deletedCount != NULL) {
            ++(*deletedCount);
        }
    }

    bool cleanupClipboardSendThread(bool) override
    {
        ++cleanupCalls;
        return cleanupAllowed;
    }

    bool cleanupAllowed;
    UInt32 cleanupCalls;
    UInt32* deletedCount;
};

class LeavingFailsClient : public RecordingClient
{
public:
    explicit LeavingFailsClient(const std::string& name) :
        RecordingClient(name)
    {
    }

    bool leave() override { return false; }
};

class UnenterablePrimaryClient : public PrimaryClient
{
public:
    explicit UnenterablePrimaryClient(barrier::Screen* screen = NULL) :
        PrimaryClient("primary", screen),
        enterCount(0)
    {
    }

    bool canEnter() const override { return false; }
    SInt32 getJumpZoneSize() const override { return 1; }
    bool isLockedToScreen() const override { return false; }
    KeyModifierMask getToggleMask() const override { return 0; }
    void enter(SInt32, SInt32, UInt32, KeyModifierMask, bool) override { ++enterCount; }
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

    UInt32 enterCount;
};

class EnterablePrimaryClient : public PrimaryClient
{
public:
    explicit EnterablePrimaryClient(barrier::Screen* screen = NULL) :
        PrimaryClient("primary", screen),
        enterCount(0),
        enterX(0),
        enterY(0),
        enterSeqNum(0),
        enterMask(0),
        enterForScreensaver(true)
    {
    }

    void* getEventTarget() const override { return const_cast<EnterablePrimaryClient*>(this); }
    bool canEnter() const override { return true; }
    SInt32 getJumpZoneSize() const override { return 1; }
    bool isLockedToScreen() const override { return false; }
    KeyModifierMask getToggleMask() const override { return 0; }
    bool leave() override { return true; }
    void enter(SInt32 xAbs, SInt32 yAbs, UInt32 seqNum, KeyModifierMask mask,
               bool forScreensaver) override
    {
        ++enterCount;
        enterX = xAbs;
        enterY = yAbs;
        enterSeqNum = seqNum;
        enterMask = mask;
        enterForScreensaver = forScreensaver;
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

    UInt32 enterCount;
    SInt32 enterX;
    SInt32 enterY;
    UInt32 enterSeqNum;
    KeyModifierMask enterMask;
    bool enterForScreensaver;
};

class ClipboardPrimaryClient : public PrimaryClient
{
public:
    ClipboardPrimaryClient() :
        PrimaryClient("primary", NULL),
        clipboardAvailable(false),
        clipboardDirtyCount(0),
        setClipboardCount(0),
        lastSetClipboardId(kClipboardEnd)
    {
    }

    bool getClipboard(ClipboardID, IClipboard* clipboard) const override
    {
        if (!clipboardAvailable) {
            return false;
        }
        static_cast<Clipboard*>(clipboard)->unmarshall(sourceClipboard.marshall(), 0);
        return true;
    }

    void setClipboard(ClipboardID id, const IClipboard*) override
    {
        ++setClipboardCount;
        lastSetClipboardId = id;
        setClipboardIds.push_back(id);
    }
    void setClipboardDirty(ClipboardID, bool) override { ++clipboardDirtyCount; }

    Clipboard sourceClipboard;
    bool clipboardAvailable;
    UInt32 clipboardDirtyCount;
    UInt32 setClipboardCount;
    ClipboardID lastSetClipboardId;
    std::vector<ClipboardID> setClipboardIds;
};

class DragPlatformScreen : public IPlatformScreen
{
public:
    DragPlatformScreen() :
        IPlatformScreen(NULL),
        draggingStarted(false)
    {
    }

    void enable() override { }
    void disable() override { }
    void enter() override { }
    bool leave() override { return true; }
    bool setClipboard(ClipboardID, const IClipboard*) override { return true; }
    void checkClipboards() override { }
    void openScreensaver(bool) override { }
    void closeScreensaver() override { }
    void screensaver(bool) override { }
    void resetOptions() override { }
    void setOptions(const OptionsList&) override { }
    void setSequenceNumber(UInt32) override { }
    void setDraggingStarted(bool started) override { draggingStarted = started; }
    bool isPrimary() const override { return true; }
    void* getEventTarget() const override { return const_cast<DragPlatformScreen*>(this); }
    bool getClipboard(ClipboardID, IClipboard*) const override { return false; }
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
    void fakeMouseButton(ButtonID, bool) override { }
    void fakeMouseMove(SInt32, SInt32) override { }
    void fakeMouseRelativeMove(SInt32, SInt32) const override { }
    void fakeMouseWheel(SInt32, SInt32) const override { }
    void updateKeyMap() override { }
    void updateKeyState() override { }
    void setHalfDuplexMask(KeyModifierMask) override { }
    void fakeKeyDown(KeyID, KeyModifierMask, KeyButton) override { }
    bool fakeKeyRepeat(KeyID, KeyModifierMask, SInt32, KeyButton) override { return true; }
    bool fakeKeyUp(KeyButton) override { return true; }
    void fakeAllKeysUp() override { }
    bool fakeCtrlAltDel() override { return false; }
    bool isKeyDown(KeyButton) const override { return false; }
    KeyModifierMask getActiveModifiers() const override { return 0; }
    KeyModifierMask pollActiveModifiers() const override { return 0; }
    SInt32 pollActiveGroup() const override { return 0; }
    void pollPressedKeys(KeyButtonSet&) const override { }
    String& getDraggingFilename() override { return draggingFilename; }
    void clearDraggingFilename() override { draggingFilename.clear(); }
    bool isDraggingStarted() override { return draggingStarted; }
    bool isFakeDraggingStarted() override { return false; }
    void fakeDraggingFiles(DragFileList) override { }
    const String& getDropTarget() const override { return dropTarget; }
    void setDropTarget(const String& target) override { dropTarget = target; }
    void handleSystemEvent(const Event&, void*) override { }

    bool draggingStarted;
    String draggingFilename;
    String dropTarget;
};

void setEventTypeDefaults(MockEventQueue& events,
                          ClientProxyEvents& clientProxyEvents,
                          IScreenEvents& screenEvents,
                          ClipboardEvents& clipboardEvents,
                          ServerEvents& serverEvents)
{
    clientProxyEvents.setEvents(&events);
    screenEvents.setEvents(&events);
    clipboardEvents.setEvents(&events);
    serverEvents.setEvents(&events);

    ON_CALL(events, forClientProxy()).WillByDefault(ReturnRef(clientProxyEvents));
    ON_CALL(events, forIScreen()).WillByDefault(ReturnRef(screenEvents));
    ON_CALL(events, forClipboard()).WillByDefault(ReturnRef(clipboardEvents));
    ON_CALL(events, forServer()).WillByDefault(ReturnRef(serverEvents));
    ON_CALL(events, registerTypeOnce(_, _)).WillByDefault(Return(100));
    ON_CALL(events, newOneShotTimer(_, _)).WillByDefault(Return(reinterpret_cast<EventQueueTimer*>(1)));
}

void initializeServer(Server& server, Config& config, PrimaryClient& primary,
                      MockEventQueue& events, RecordingClient& active)
{
    server.m_config = &config;
    server.m_primaryClient = &primary;
    server.m_events = &events;
    server.m_active = &active;
    server.m_activeSaver = NULL;
    server.m_switchScreen = NULL;
    server.m_switchWaitDelay = 0.0;
    server.m_switchWaitTimer = NULL;
    server.m_switchTwoTapDelay = 0.0;
    server.m_switchTwoTapEngaged = false;
    server.m_switchTwoTapArmed = false;
    server.m_switchNeedsShift = false;
    server.m_switchNeedsControl = false;
    server.m_switchNeedsAlt = false;
    server.m_seqNum = 7;
    server.m_x = 321;
    server.m_y = 654;
    server.m_enableClipboard = false;
    server.m_lockedToScreen = false;
    server.m_expectedFileSize = 0;
    server.m_receivedFileData.clear();
    server.m_receivedFileSpoolPath.clear();
    server.m_sendFileThread = NULL;
    server.m_writeToDropDirThread = NULL;
    server.m_args = ServerArgs();
    server.m_clients.insert(std::make_pair(active.getName(), &active));
    server.m_clientSet.insert(&active);
}

Clipboard makeSourcePathsClipboard(const barrier::fs::path& path)
{
    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.sessionId = "local-file-list";
    payload.paths.push_back(path);

    Clipboard clipboard;
    clipboard.open(0);
    clipboard.empty();
    clipboard.add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
    clipboard.close();
    return clipboard;
}

Clipboard makeMaterializedPathsClipboard(const barrier::fs::path& path)
{
    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::MaterializedPaths;
    payload.sessionId = "remote-ready-session";
    payload.paths.push_back(path);

    Clipboard clipboard;
    clipboard.open(0);
    clipboard.empty();
    clipboard.add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
    clipboard.close();
    return clipboard;
}

Clipboard makeImageClipboardWithSourcePathMetadata(const barrier::fs::path& path)
{
    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.sessionId = "local-image-file-list";
    payload.paths.push_back(path);

    Clipboard clipboard;
    clipboard.open(0);
    clipboard.empty();
    clipboard.add(IClipboard::kPNG, "fake-png");
    clipboard.add(IClipboard::kFileList, RemoteFileClipboard::serialize(payload));
    clipboard.add(IClipboard::kText, path.u8string() + "\n");
    clipboard.close();
    return clipboard;
}

Clipboard makeTextClipboard(const std::string& text)
{
    Clipboard clipboard;
    clipboard.open(0);
    clipboard.empty();
    clipboard.add(IClipboard::kText, text);
    clipboard.close();
    return clipboard;
}

}

TEST(ServerReconnectTests, constructorInitializesTransferAndCursorState)
{
    TestEventQueue events;
    NiceMock<MockScreen> screen;
    NiceMock<MockPrimaryClient> primary;
    NiceMock<MockConfig> config;
    NiceMock<MockInputFilter> inputFilter;

    ON_CALL(config, isScreen(_)).WillByDefault(Return(true));
    ON_CALL(config, getInputFilter()).WillByDefault(Return(&inputFilter));
    ON_CALL(primary, getEventTarget()).WillByDefault(Return(&primary));
    ON_CALL(primary, getToggleMask()).WillByDefault(Return(0));

    ServerArgs args;
    Server server(config, &primary, &screen, &events, args);
    server.m_mock = true;

    EXPECT_TRUE(server.isReceivedFileSizeValid());
    EXPECT_EQ(0u, server.m_expectedFileSize);
    EXPECT_TRUE(server.m_receivedFileData.empty());
    EXPECT_TRUE(server.m_receivedFileSpoolPath.empty());
    EXPECT_EQ(0, server.m_x);
    EXPECT_EQ(0, server.m_y);
}

TEST(ServerReconnectTests, replayClipboardsToActiveDoesNotPublishForeignSourcePaths)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    ClipboardPrimaryClient primary;
    RecordingClient client("client");
    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);
    server.m_enableClipboard = true;
    server.m_active = &primary;

    Server::ClipboardInfo& clipboard = server.m_clipboards[kClipboardClipboard];
    clipboard.m_clipboardOwner = client.getName();
    clipboard.m_clipboard = makeSourcePathsClipboard(barrier::fs::u8path("/tmp/client-only-file.txt"));

    server.replayClipboardsToActive();

    EXPECT_EQ(primary.setClipboardIds.end(),
              std::find(primary.setClipboardIds.begin(),
                        primary.setClipboardIds.end(),
                        kClipboardClipboard));
}

TEST(ServerReconnectTests, replayClipboardsToActiveDoesNotPrefetchPrimarySourcePaths)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    ClipboardPrimaryClient primary;
    RecordingClient client("client");
    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);
    server.m_enableClipboard = true;
    server.m_active = &client;

    Server::ClipboardInfo& clipboard = server.m_clipboards[kClipboardClipboard];
    clipboard.m_clipboardOwner = primary.getName();
    clipboard.m_clipboard = makeSourcePathsClipboard(barrier::fs::u8path("/tmp/primary-file.txt"));

    server.replayClipboardsToActive();

    EXPECT_EQ(client.setClipboardIds.end(),
              std::find(client.setClipboardIds.begin(),
                        client.setClipboardIds.end(),
                        kClipboardClipboard));
    EXPECT_EQ(0u, client.fileChunkCount);
    EXPECT_EQ(NULL, server.m_sendFileThread);
    EXPECT_EQ(NULL, server.m_sendFileTarget);
}

TEST(ServerReconnectTests, sendDragInfoUsesSynchronousPathAndClearsState)
{
    Config config;
    config.addScreen("target");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    RecordingClient target("target");
    Server server;
    initializeServer(server, config, primary, events, target);

    const barrier::fs::path firstPath =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-drag-info-first.txt");
    const barrier::fs::path secondPath =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-drag-info-second.txt");
    {
        std::ofstream first(firstPath.u8string().c_str(), std::ios::out | std::ios::trunc);
        first << "first";
        std::ofstream second(secondPath.u8string().c_str(), std::ios::out | std::ios::trunc);
        second << "second";
    }

    DragPlatformScreen* platformScreen = new DragPlatformScreen();
    platformScreen->draggingFilename =
        firstPath.u8string() + "\n" + secondPath.u8string() + "\n";
    barrier::Screen screen(platformScreen, &events);
    server.m_screen = &screen;

    server.sendDragInfo(&target);

    EXPECT_EQ(1u, target.dragInfoCount);
    EXPECT_EQ(2u, target.lastDragFileCount);
    EXPECT_EQ(target.lastDragInfo.size(), target.lastDragInfoSize);
    EXPECT_FALSE(target.lastDragInfo.empty());
    EXPECT_TRUE(server.m_dragFileList.empty());

    barrier::fs::remove(firstPath);
    barrier::fs::remove(secondPath);
}

TEST(ServerReconnectTests, primaryDragAtEdgeSendsDragInfoAndSwitchesToTarget)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("target");
    ASSERT_TRUE(config.connect("primary", kRight, 0.0f, 1.0f, "target", 0.0f, 1.0f));

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    EnterablePrimaryClient primary;
    RecordingClient target("target");
    Server server;
    initializeServer(server, config, primary, events, target);
    server.m_active = &primary;
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);
    server.m_x = 1022;
    server.m_y = 100;
    server.m_args.m_enableDragDrop = true;

    const barrier::fs::path draggedPath =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-drag-switch.txt");
    {
        std::ofstream dragged(draggedPath.u8string().c_str(), std::ios::out | std::ios::trunc);
        dragged << "drag";
    }

    DragPlatformScreen* platformScreen = new DragPlatformScreen();
    platformScreen->draggingStarted = true;
    platformScreen->draggingFilename = draggedPath.u8string() + "\n";
    barrier::Screen screen(platformScreen, &events);
    server.m_screen = &screen;

    EXPECT_TRUE(server.onMouseMovePrimary(1023, 100));

    EXPECT_EQ(&target, server.m_active);
    EXPECT_EQ(1u, target.dragInfoCount);
    EXPECT_EQ(1u, target.lastDragFileCount);
    EXPECT_EQ(1u, target.enterCount);
    EXPECT_EQ(0, target.enterX);
    EXPECT_EQ(100, target.enterY);

    barrier::fs::remove(draggedPath);
}

TEST(ServerReconnectTests, completedTransferSnapshotOwnsSpoolBeforeNextReceive)
{
    Config config;
    config.addScreen("primary");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    RecordingClient active("primary");
    Server server;
    initializeServer(server, config, primary, events, active);

    const barrier::fs::path spoolPath =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-server-snapshot.part");
    barrier::fs::remove(spoolPath);
    {
        std::ofstream file(spoolPath.u8string().c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        file << "payload";
    }

    DragInformation entry;
    String filename("payload.txt");
    entry.setFilename(filename);

    server.m_expectedFileSize = 7;
    server.m_receivedFileData = "memory-copy";
    server.m_receivedFileSpoolPath = spoolPath;
    server.m_fakeDragFileList.push_back(entry);
    server.m_remoteFileClipboardSession = "session-1";

    auto transfer = server.testTakeCompletedFileTransfer();

    EXPECT_EQ(0u, server.m_expectedFileSize);
    EXPECT_TRUE(server.m_receivedFileData.empty());
    EXPECT_TRUE(server.m_receivedFileSpoolPath.empty());
    EXPECT_TRUE(server.m_fakeDragFileList.empty());
    EXPECT_TRUE(barrier::fs::exists(spoolPath));

    ASSERT_TRUE(transfer);
    EXPECT_EQ(7u, transfer->expectedSize);
    EXPECT_EQ("memory-copy", transfer->data);
    EXPECT_EQ(spoolPath, transfer->spoolPath);
    EXPECT_EQ(1u, transfer->dragFileList.size());
    EXPECT_EQ("session-1", transfer->remoteFileClipboardSession);

    FileChunk::releaseReceiveBuffer(transfer->data, transfer->expectedSize, &transfer->spoolPath);
    EXPECT_FALSE(barrier::fs::exists(spoolPath));
}

TEST(ServerReconnectTests, fileReceiveCompleteDoesNotBlockOnBusyDropDirWriter)
{
    Config config;
    config.addScreen("primary");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    FileEvents fileEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);
    fileEvents.setEvents(&events);
    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));

    NiceMock<MockPrimaryClient> primary;
    RecordingClient active("primary");
    Server server;
    initializeServer(server, config, primary, events, active);

    std::atomic<bool> releaseWriter(false);
    std::atomic<bool> writerCancelled(false);
    server.testSetWriteToDropDirThread(new Thread([&releaseWriter, &writerCancelled]() {
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

    server.m_expectedFileSize = 4;
    server.m_receivedFileData = "data";

    server.onFileRecieveCompleted();

    EXPECT_FALSE(writerCancelled.load());
    EXPECT_TRUE(server.testHasWriteToDropDirThread());
    EXPECT_EQ(1u, server.testPendingDropDirTransferCount());
    EXPECT_EQ(0u, server.m_expectedFileSize);
    EXPECT_TRUE(server.m_receivedFileData.empty());
    EXPECT_TRUE(server.m_receivedFileSpoolPath.empty());

    releaseWriter.store(true);
    EXPECT_TRUE(server.testCleanupWriteToDropDirThread());
    server.testDrainDropDirTransferQueue();
    EXPECT_EQ(0u, server.testPendingDropDirTransferCount());
    EXPECT_TRUE(server.testHasWriteToDropDirThread());
    EXPECT_TRUE(server.testCleanupWriteToDropDirThread());
    EXPECT_FALSE(server.testHasWriteToDropDirThread());
}

TEST(ServerReconnectTests, dropDirWriterQueueCapsBufferedMemory)
{
    Config config;
    config.addScreen("primary");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    RecordingClient active("primary");
    Server server;
    initializeServer(server, config, primary, events, active);

    const std::string payload(17 * 1024 * 1024, 'x');
    server.testQueueDropDirTransfer(payload);
    server.testQueueDropDirTransfer(payload);

    EXPECT_EQ(1u, server.testPendingDropDirTransferCount());
}

TEST(ServerReconnectTests, clientDisconnectReleasesPartialReceiveSpool)
{
    Config config;
    config.addScreen("primary");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    RecordingClient active("primary");
    Server server;
    initializeServer(server, config, primary, events, active);

    const barrier::fs::path spoolPath =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-server-disconnect-spool.part");
    barrier::fs::remove(spoolPath);
    {
        std::ofstream file(spoolPath.u8string().c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        file << "partial";
    }

    server.m_expectedFileSize = 1024;
    server.m_receivedFileData = "stale";
    server.m_receivedFileSpoolPath = spoolPath;

    server.handleClientDisconnected(Event(), new RecordingClient("client"));

    EXPECT_EQ(0u, server.m_expectedFileSize);
    EXPECT_TRUE(server.m_receivedFileData.empty());
    EXPECT_TRUE(server.m_receivedFileSpoolPath.empty());
    EXPECT_FALSE(barrier::fs::exists(spoolPath));
}

TEST(ServerReconnectTests, cleanupSendFileThreadReleasesAllDeferredClients)
{
    Config config;
    config.addScreen("primary");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    RecordingClient active("primary");
    Server server;
    initializeServer(server, config, primary, events, active);

    UInt32 deletedCount = 0;
    server.m_deferredDeleteClients.insert(new RecordingClient("old-1", &deletedCount));
    server.m_deferredDeleteClients.insert(new RecordingClient("old-2", &deletedCount));

    EXPECT_TRUE(server.cleanupSendFileThread(true));

    EXPECT_EQ(2u, deletedCount);
    EXPECT_TRUE(server.m_deferredDeleteClients.empty());
}

TEST(ServerReconnectTests, reapSendFileThreadKeepsTargetUntilCompletionChunk)
{
    Config config;
    config.addScreen("primary");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    RecordingClient active("primary");
    RecordingClient target("client");
    Server server;
    initializeServer(server, config, primary, events, active);
    server.m_clientSet.insert(&target);
    server.m_sendFileTarget = &target;

    std::atomic<bool> releaseSender(false);
    server.m_sendFileThread = new Thread([&releaseSender]() {
        while (!releaseSender.load()) {
            ARCH->sleep(0.01);
            Thread::testCancel();
        }
    });

    EXPECT_FALSE(server.reapSendFileThreadIfReady());
    EXPECT_EQ(&target, server.m_sendFileTarget);

    releaseSender.store(true);
    bool reaped = false;
    for (int i = 0; i < 100 && !reaped; ++i) {
        reaped = server.reapSendFileThreadIfReady();
        if (!reaped) {
            ARCH->sleep(0.01);
        }
    }

    EXPECT_TRUE(reaped);
    EXPECT_EQ(&target, server.m_sendFileTarget);

    server.m_sendFileTarget = NULL;
    server.m_clientSet.erase(&target);
}

TEST(ServerReconnectTests, clientProxy16DeleteDefersUntilClipboardSenderStops)
{
    Config config;
    config.addScreen("primary");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    IStreamEvents streamEvents;
    FileEvents fileEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);
    streamEvents.setEvents(&events);
    fileEvents.setEvents(&events);
    ON_CALL(events, forIStream()).WillByDefault(ReturnRef(streamEvents));
    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, adoptHandler(_, _, _)).WillByDefault(Invoke([](Event::Type, void*, IEventJob* job) {
        delete job;
    }));

    NiceMock<MockPrimaryClient> primary;
    RecordingClient active("primary");
    Server server;
    initializeServer(server, config, primary, events, active);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));

    UInt32 deletedCount = 0;
    DeferringClientProxy16* client =
        new DeferringClientProxy16("client", stream, &server, &events, &deletedCount);

    EXPECT_FALSE(server.deleteClientIfReady(client));

    EXPECT_EQ(0u, deletedCount);
    EXPECT_EQ(1u, client->cleanupCalls);
    EXPECT_EQ(1u, server.m_deferredDeleteClients.count(client));

    client->cleanupAllowed = true;
    server.deleteDeferredClients();

    EXPECT_EQ(1u, deletedCount);
    EXPECT_TRUE(server.m_deferredDeleteClients.empty());
}

TEST(ServerReconnectTests, fileKeepAliveReapsDeferredClientAfterClipboardSenderStops)
{
    Config config;
    config.addScreen("primary");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    IStreamEvents streamEvents;
    FileEvents fileEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);
    streamEvents.setEvents(&events);
    fileEvents.setEvents(&events);
    ON_CALL(events, forIStream()).WillByDefault(ReturnRef(streamEvents));
    ON_CALL(events, forFile()).WillByDefault(ReturnRef(fileEvents));
    ON_CALL(events, adoptHandler(_, _, _)).WillByDefault(Invoke([](Event::Type, void*, IEventJob* job) {
        delete job;
    }));

    NiceMock<MockPrimaryClient> primary;
    RecordingClient active("primary");
    Server server;
    initializeServer(server, config, primary, events, active);

    NiceMock<MockStream>* stream = new NiceMock<MockStream>();
    ON_CALL(*stream, getEventTarget()).WillByDefault(Return(stream));

    UInt32 deletedCount = 0;
    DeferringClientProxy16* client =
        new DeferringClientProxy16("client", stream, &server, &events, &deletedCount);

    EXPECT_FALSE(server.deleteClientIfReady(client));
    ASSERT_EQ(1u, server.m_deferredDeleteClients.count(client));

    client->cleanupAllowed = true;
    server.handleFileKeepAliveEvent(Event(), NULL);

    EXPECT_EQ(1u, deletedCount);
    EXPECT_TRUE(server.m_deferredDeleteClients.empty());
}

TEST(ServerReconnectTests, fileKeepAliveDoesNotDeleteDeferredSendTargetUntilCompletionChunk)
{
    Config config;
    config.addScreen("primary");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    RecordingClient active("primary");
    Server server;
    initializeServer(server, config, primary, events, active);

    UInt32 deletedCount = 0;
    RecordingClient* client = new RecordingClient("old-client", &deletedCount);
    server.m_sendFileTarget = client;
    server.m_deferredDeleteClients.insert(client);

    std::atomic<bool> releaseSender(false);
    server.m_sendFileThread = new Thread([&releaseSender]() {
        while (!releaseSender.load()) {
            ARCH->sleep(0.01);
            Thread::testCancel();
        }
    });

    server.handleFileKeepAliveEvent(Event(), NULL);
    EXPECT_EQ(0u, deletedCount);
    EXPECT_EQ(1u, server.m_deferredDeleteClients.count(client));

    releaseSender.store(true);
    for (int i = 0; i < 100 && server.m_sendFileThread != NULL; ++i) {
        server.handleFileKeepAliveEvent(Event(), NULL);
        if (server.m_sendFileThread != NULL) {
            ARCH->sleep(0.01);
        }
    }

    EXPECT_EQ(0u, deletedCount);
    EXPECT_EQ(1u, server.m_deferredDeleteClients.count(client));
    EXPECT_EQ(NULL, server.m_sendFileThread);
    EXPECT_EQ(client, server.m_sendFileTarget);

    FileChunk* chunk = FileChunk::end();
    server.onFileChunkSending(chunk);

    EXPECT_EQ(1u, deletedCount);
    EXPECT_TRUE(server.m_deferredDeleteClients.empty());
    EXPECT_EQ(NULL, server.m_sendFileTarget);

    delete chunk;
}

TEST(ServerReconnectTests, fileKeepAliveKeepsConnectedTargetUntilCompletionChunk)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    RecordingClient active("primary");
    Server server;
    initializeServer(server, config, primary, events, active);

    UInt32 deletedCount = 0;
    RecordingClient* client = new RecordingClient("client", &deletedCount);
    server.m_clientSet.insert(client);
    server.m_sendFileTarget = client;
    server.m_sendFileThread = new Thread([]() { });

    for (int i = 0; i < 100 && server.m_sendFileThread != NULL; ++i) {
        server.handleFileKeepAliveEvent(Event(), NULL);
        if (server.m_sendFileThread != NULL) {
            ARCH->sleep(0.01);
        }
    }

    EXPECT_EQ(0u, deletedCount);
    EXPECT_EQ(client, server.m_sendFileTarget);
    EXPECT_EQ(NULL, server.m_sendFileThread);

    FileChunk* chunk = FileChunk::end();
    server.onFileChunkSending(chunk);

    EXPECT_EQ(1u, client->fileChunkCount);
    EXPECT_EQ(kDataEnd, client->lastFileChunkMark);
    EXPECT_EQ(NULL, server.m_sendFileTarget);

    delete chunk;

    server.m_clientSet.erase(client);
    delete client;
}

TEST(ServerReconnectTests, activeClientDisconnectEntersPrimaryAndClearsPendingSwitch)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    UInt32 screenSwitchedCount = 0;
    const Event::Type screenSwitchedType = serverEvents.screenSwitched();
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&screenSwitchedCount, screenSwitchedType](const Event& event) {
            if (event.getType() == screenSwitchedType && event.getData() != NULL) {
                ++screenSwitchedCount;
            }
            Event::deleteData(event);
        }));

    DragPlatformScreen* platformScreen = new DragPlatformScreen();
    barrier::Screen screen(platformScreen, &events);
    EnterablePrimaryClient primary(&screen);
    RecordingClient* client = new RecordingClient("client");
    Server server;
    initializeServer(server, config, primary, events, *client);
    server.m_screen = &screen;
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);
    server.m_switchScreen = client;
    server.m_x = 5;
    server.m_y = 100;

    server.handleClientDisconnected(Event(), client);

    EXPECT_EQ(&primary, server.m_active);
    EXPECT_EQ(NULL, server.m_switchScreen);
    EXPECT_EQ(1u, primary.enterCount);
    EXPECT_EQ(512, primary.enterX);
    EXPECT_EQ(384, primary.enterY);
    EXPECT_EQ(7u, primary.enterSeqNum);
    EXPECT_EQ(0u, primary.enterMask);
    EXPECT_FALSE(primary.enterForScreensaver);
    EXPECT_EQ(1u, screenSwitchedCount);
}

TEST(ServerReconnectTests, activeClientDisconnectDoesNotEnterUnavailablePrimary)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    UInt32 screenSwitchedCount = 0;
    const Event::Type screenSwitchedType = serverEvents.screenSwitched();
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&screenSwitchedCount, screenSwitchedType](const Event& event) {
            if (event.getType() == screenSwitchedType && event.getData() != NULL) {
                ++screenSwitchedCount;
            }
            Event::deleteData(event);
        }));

    DragPlatformScreen* platformScreen = new DragPlatformScreen();
    barrier::Screen screen(platformScreen, &events);
    UnenterablePrimaryClient primary(&screen);
    RecordingClient* client = new RecordingClient("client");
    Server server;
    initializeServer(server, config, primary, events, *client);
    server.m_screen = &screen;
    server.m_x = 5;
    server.m_y = 100;

    server.handleClientDisconnected(Event(), client);

    EXPECT_EQ(&primary, server.m_active);
    EXPECT_EQ(0u, primary.enterCount);
    EXPECT_EQ(NULL, server.m_switchScreen);
    EXPECT_EQ(0, server.m_xDelta);
    EXPECT_EQ(0, server.m_yDelta);
    EXPECT_EQ(0u, screenSwitchedCount);
}

TEST(ServerReconnectTests, activeClientShapeChangedToUnusableRecoversToPrimary)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    UInt32 screenSwitchedCount = 0;
    const Event::Type screenSwitchedType = serverEvents.screenSwitched();
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&screenSwitchedCount, screenSwitchedType](const Event& event) {
            if (event.getType() == screenSwitchedType && event.getData() != NULL) {
                ++screenSwitchedCount;
            }
            Event::deleteData(event);
        }));

    DragPlatformScreen* platformScreen = new DragPlatformScreen();
    barrier::Screen screen(platformScreen, &events);
    EnterablePrimaryClient primary(&screen);
    RecordingClient client("client");
    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_screen = &screen;
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);
    server.m_switchScreen = &client;
    server.m_x = 100;
    server.m_y = 100;

    client.shapeW = 0;
    client.shapeH = 0;

    server.handleShapeChanged(Event(), &client);

    EXPECT_EQ(&primary, server.m_active);
    EXPECT_EQ(NULL, server.m_switchScreen);
    EXPECT_EQ(1u, primary.enterCount);
    EXPECT_EQ(512, primary.enterX);
    EXPECT_EQ(384, primary.enterY);
    EXPECT_EQ(1u, screenSwitchedCount);
}

TEST(ServerReconnectTests, adoptClient_replacesActiveSameNameClient_entersNewClient)
{
    Config config;
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    ON_CALL(primary, getToggleMask()).WillByDefault(Return(KeyModifierCapsLock));
    ON_CALL(primary, getEventTarget()).WillByDefault(Return(&primary));

    RecordingClient oldClient("client");
    RecordingClient newClient("client");
    Server server;
    initializeServer(server, config, primary, events, oldClient);

    server.adoptClient(&newClient);

    EXPECT_EQ(&newClient, server.m_active);
    EXPECT_EQ(1u, newClient.enterCount);
    EXPECT_EQ(321, newClient.enterX);
    EXPECT_EQ(654, newClient.enterY);
    EXPECT_EQ(8u, newClient.enterSeqNum);
    EXPECT_EQ(KeyModifierCapsLock, newClient.enterMask);
    EXPECT_FALSE(newClient.enterForScreensaver);
}

TEST(ServerReconnectTests, adoptClientReplacingSameNameInterruptsOldSendTarget)
{
    Config config;
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    ON_CALL(primary, getToggleMask()).WillByDefault(Return(0));
    ON_CALL(primary, getEventTarget()).WillByDefault(Return(&primary));

    RecordingClient oldClient("client");
    RecordingClient newClient("client");
    Server server;
    initializeServer(server, config, primary, events, oldClient);

    server.m_sendFileTarget = &oldClient;
    server.m_sendFileThread = new Thread([]() { });

    server.adoptClient(&newClient);

    for (int i = 0; i < 100 && server.m_sendFileThread != NULL; ++i) {
        server.handleFileKeepAliveEvent(Event(), NULL);
        if (server.m_sendFileThread != NULL) {
            ARCH->sleep(0.01);
        }
    }

    EXPECT_EQ(&newClient, server.m_active);
    EXPECT_EQ(NULL, server.m_sendFileThread);
    EXPECT_EQ(NULL, server.m_sendFileTarget);
    EXPECT_EQ(0u, oldClient.fileChunkCount);
}

TEST(ServerReconnectTests, secondaryMotion_clampsActiveClientWhenPrimaryCannotBeEntered)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");
    ASSERT_TRUE(config.connect("client", kLeft, 0.0f, 1.0f, "primary", 0.0f, 1.0f));

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    UnenterablePrimaryClient primary;
    RecordingClient client("client");
    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);
    server.m_x = 5;
    server.m_y = 100;

    server.onMouseMoveSecondary(-20, 0);

    EXPECT_EQ(&client, server.m_active);
    EXPECT_GE(server.m_x, client.shapeX);
    EXPECT_LT(server.m_x, client.shapeX + client.shapeW);
    EXPECT_EQ(100, server.m_y);
    EXPECT_EQ(1u, client.mouseMoveCount);
    EXPECT_EQ(server.m_x, client.mouseMoveX);
    EXPECT_EQ(100, client.mouseMoveY);
    EXPECT_EQ(NULL, server.m_switchScreen);
}

TEST(ServerReconnectTests, secondaryMotion_repeatedEdgePressureKeepsCursorInsideActiveClient)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");
    ASSERT_TRUE(config.connect("client", kLeft, 0.0f, 1.0f, "primary", 0.0f, 1.0f));

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    UnenterablePrimaryClient primary;
    RecordingClient client("client");
    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);
    server.m_x = 5;
    server.m_y = 100;

    server.onMouseMoveSecondary(-20, 0);
    EXPECT_EQ(&client, server.m_active);
    EXPECT_GE(server.m_x, client.shapeX);
    EXPECT_LT(server.m_x, client.shapeX + client.shapeW);
    EXPECT_EQ(NULL, server.m_switchScreen);

    server.onMouseMoveSecondary(30, 0);
    EXPECT_EQ(&client, server.m_active);
    EXPECT_GE(server.m_x, client.shapeX);
    EXPECT_LT(server.m_x, client.shapeX + client.shapeW);
    EXPECT_EQ(NULL, server.m_switchScreen);

    server.onMouseMoveSecondary(-100, 0);
    EXPECT_EQ(&client, server.m_active);
    EXPECT_GE(server.m_x, client.shapeX);
    EXPECT_LT(server.m_x, client.shapeX + client.shapeW);
    EXPECT_EQ(100, server.m_y);
    EXPECT_EQ(NULL, server.m_switchScreen);
    EXPECT_EQ(server.m_x, client.mouseMoveX);
    EXPECT_EQ(server.m_y, client.mouseMoveY);
}

TEST(ServerReconnectTests, avoidJumpZoneIgnoresConfiguredButUnavailableNeighbor)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");
    ASSERT_TRUE(config.connect("primary", kRight, 0.0f, 1.0f, "client", 0.0f, 1.0f));

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    EnterablePrimaryClient primary;
    RecordingClient client("client");
    client.shapeW = 0;
    client.shapeH = 0;

    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_active = &primary;
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);

    SInt32 x = 1023;
    SInt32 y = 100;

    server.avoidJumpZone(&primary, kLeft, x, y);

    EXPECT_EQ(1023, x);
    EXPECT_EQ(100, y);
}

TEST(ServerReconnectTests, avoidJumpZoneStillHonorsUsableNeighbor)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");
    ASSERT_TRUE(config.connect("primary", kRight, 0.0f, 1.0f, "client", 0.0f, 1.0f));

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    EnterablePrimaryClient primary;
    RecordingClient client("client");

    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_active = &primary;
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);

    SInt32 x = 1023;
    SInt32 y = 100;

    server.avoidJumpZone(&primary, kLeft, x, y);

    EXPECT_EQ(1022, x);
    EXPECT_EQ(100, y);
}

TEST(ServerReconnectTests, secondaryMotion_reanchorsActiveClientWhenLeaveFails)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");
    config.addScreen("other");
    ASSERT_TRUE(config.connect("client", kRight, 0.0f, 1.0f, "other", 0.0f, 1.0f));

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    ON_CALL(primary, getToggleMask()).WillByDefault(Return(0));

    LeavingFailsClient client("client");
    RecordingClient other("other");
    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_clients.insert(std::make_pair(other.getName(), &other));
    server.m_clientSet.insert(&other);
    server.m_x = client.shapeX + client.shapeW - 2;
    server.m_y = 100;

    server.onMouseMoveSecondary(20, 0);

    EXPECT_EQ(&client, server.m_active);
    EXPECT_GE(server.m_x, client.shapeX);
    EXPECT_LT(server.m_x, client.shapeX + client.shapeW);
    EXPECT_EQ(100, server.m_y);
    EXPECT_EQ(0, server.m_xDelta);
    EXPECT_EQ(0, server.m_yDelta);
    EXPECT_EQ(NULL, server.m_switchScreen);
    EXPECT_EQ(1u, client.mouseMoveCount);
    EXPECT_EQ(server.m_x, client.mouseMoveX);
    EXPECT_EQ(server.m_y, client.mouseMoveY);
    EXPECT_EQ(0u, other.enterCount);
}

TEST(ServerReconnectTests, fileChunkSendingUsesTransferTargetWhenActiveChanges)
{
    Config config;
    config.addScreen("target");
    config.addScreen("other");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    RecordingClient target("target");
    RecordingClient other("other");
    Server server;
    initializeServer(server, config, primary, events, target);
    server.m_clients.insert(std::make_pair(other.getName(), &other));
    server.m_clientSet.insert(&other);
    server.m_active = &other;
    server.setFileTransferForTest(&target, 7);

    FileChunk* chunk = FileChunk::data(reinterpret_cast<const UInt8*>("abc"), 3);
    chunk->m_transferId = 7;

    server.onFileChunkSending(chunk);

    EXPECT_EQ(1u, target.fileChunkCount);
    EXPECT_EQ(0u, other.fileChunkCount);
    EXPECT_EQ(kDataChunk, target.lastFileChunkMark);
    EXPECT_EQ(3u, target.lastFileChunkSize);

    delete chunk;
}

TEST(ServerReconnectTests, staleFileChunkAfterReconnectDoesNotWriteToNewTarget)
{
    Config config;
    config.addScreen("target");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    RecordingClient target("target");
    Server server;
    initializeServer(server, config, primary, events, target);
    server.setFileTransferForTest(&target, 2);

    FileChunk* chunk = FileChunk::data(reinterpret_cast<const UInt8*>("abc"), 3);
    chunk->m_transferId = 1;

    server.onFileChunkSending(chunk);

    EXPECT_EQ(0u, target.fileChunkCount);

    delete chunk;
}

TEST(ServerReconnectTests, generationlessFileChunkStillWritesOnGenerationlessTransfer)
{
    Config config;
    config.addScreen("target");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    RecordingClient target("target");
    Server server;
    initializeServer(server, config, primary, events, target);
    server.setFileTransferForTest(&target, 0);

    FileChunk* chunk = FileChunk::data(reinterpret_cast<const UInt8*>("abc"), 3);

    server.onFileChunkSending(chunk);

    EXPECT_EQ(1u, target.fileChunkCount);
    EXPECT_EQ(kDataChunk, target.lastFileChunkMark);
    EXPECT_EQ(3u, target.lastFileChunkSize);

    delete chunk;
}

TEST(ServerReconnectTests, fileChunkSendingDropsChunkWhenTransferTargetDisconnected)
{
    Config config;
    config.addScreen("target");
    config.addScreen("other");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    RecordingClient target("target");
    RecordingClient other("other");
    Server server;
    initializeServer(server, config, primary, events, target);
    server.m_clients.insert(std::make_pair(other.getName(), &other));
    server.m_clientSet.insert(&other);
    server.m_clientSet.erase(&target);
    server.m_active = &other;
    server.setFileTransferForTest(&target, 7);

    FileChunk* chunk = FileChunk::data(reinterpret_cast<const UInt8*>("abc"), 3);
    chunk->m_transferId = 7;

    server.onFileChunkSending(chunk);

    EXPECT_EQ(0u, target.fileChunkCount);
    EXPECT_EQ(0u, other.fileChunkCount);

    delete chunk;
}

TEST(ServerReconnectTests, onClipboardChangedBlocksLocalFileListWithoutDirtyingOtherClients)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    ClipboardPrimaryClient primary;
    RecordingClient client("client");
    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);
    server.m_enableClipboard = true;
    server.m_active = &client;
    server.m_remoteFileClipboardSession = "stale-session";
    server.m_readyFileClipboardSession = "ready-session";
    server.m_readyFileClipboardPaths.push_back(barrier::fs::u8path("/tmp/stale"));

    Server::ClipboardInfo& clipboard = server.m_clipboards[kClipboardClipboard];
    clipboard.m_clipboardOwner = primary.getName();
    clipboard.m_clipboardSeqNum = 10;
    clipboard.m_pendingPrimaryFetch = true;
    primary.sourceClipboard = makeSourcePathsClipboard(barrier::fs::u8path("/tmp/local-file.txt"));
    primary.clipboardAvailable = true;

    server.onClipboardChanged(&primary, kClipboardClipboard, 10);

    EXPECT_FALSE(clipboard.m_pendingPrimaryFetch);
    EXPECT_TRUE(server.m_remoteFileClipboardSession.empty());
    EXPECT_TRUE(server.m_readyFileClipboardSession.empty());
    EXPECT_TRUE(server.m_readyFileClipboardPaths.empty());
    EXPECT_EQ(0u, client.clipboardDirtyCount);
    EXPECT_EQ(0u, client.setClipboardCount);
    EXPECT_EQ(0u, primary.clipboardDirtyCount);
    EXPECT_EQ(0u, primary.setClipboardCount);

	RemoteFileClipboard::Data remoteFileClipboard;
	ASSERT_TRUE(RemoteFileClipboard::readFromClipboard(
	    clipboard.m_clipboard, remoteFileClipboard));
	EXPECT_EQ(RemoteFileClipboard::Mode::SourcePaths, remoteFileClipboard.mode);
	ASSERT_EQ(1u, remoteFileClipboard.paths.size());
	EXPECT_EQ("/tmp/local-file.txt", remoteFileClipboard.paths[0].u8string());
}

TEST(ServerReconnectTests, handleClipboardGrabbedDoesNotPreGrabOtherScreensForLocalFileList)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    ClipboardPrimaryClient primary;
    RecordingClient client("client");
    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);
    server.m_enableClipboard = true;
    server.m_active = &client;
    server.m_remoteFileClipboardSession = "stale-session";
    server.m_readyFileClipboardSession = "ready-session";
    server.m_readyFileClipboardPaths.push_back(barrier::fs::u8path("/tmp/stale"));
    primary.sourceClipboard = makeSourcePathsClipboard(barrier::fs::u8path("/tmp/local-file.txt"));
    primary.clipboardAvailable = true;

    IScreen::ClipboardInfo eventInfo;
    eventInfo.m_id = kClipboardClipboard;
    eventInfo.m_sequenceNumber = 10;
    server.handleClipboardGrabbed(
        Event(Event::kUnknown, NULL, &eventInfo, Event::kDontFreeData),
        &primary);

    Server::ClipboardInfo& clipboard = server.m_clipboards[kClipboardClipboard];
    EXPECT_FALSE(clipboard.m_pendingPrimaryFetch);
    EXPECT_TRUE(server.m_remoteFileClipboardSession.empty());
    EXPECT_TRUE(server.m_readyFileClipboardSession.empty());
    EXPECT_TRUE(server.m_readyFileClipboardPaths.empty());
    EXPECT_EQ(0u, client.grabClipboardCount);
    EXPECT_EQ(0u, client.clipboardDirtyCount);
    EXPECT_EQ(0u, client.setClipboardCount);
    EXPECT_EQ(1u, primary.clipboardDirtyCount);

    RemoteFileClipboard::Data remoteFileClipboard;
    ASSERT_TRUE(RemoteFileClipboard::readFromClipboard(
        clipboard.m_clipboard, remoteFileClipboard));
    EXPECT_EQ(RemoteFileClipboard::Mode::SourcePaths, remoteFileClipboard.mode);
    ASSERT_EQ(1u, remoteFileClipboard.paths.size());
    EXPECT_EQ("/tmp/local-file.txt", remoteFileClipboard.paths[0].u8string());
}

TEST(ServerReconnectTests, onClipboardChangedForwardsTextAfterBlockingLocalFileList)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    ClipboardPrimaryClient primary;
    RecordingClient client("client");
    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);
    server.m_enableClipboard = true;
    server.m_active = &client;
    server.m_remoteFileClipboardSession = "stale-session";
    server.m_readyFileClipboardSession = "ready-session";
    server.m_readyFileClipboardPaths.push_back(barrier::fs::u8path("/tmp/stale"));

    Server::ClipboardInfo& clipboard = server.m_clipboards[kClipboardClipboard];
    clipboard.m_clipboardOwner = primary.getName();
    clipboard.m_clipboardSeqNum = 10;
    clipboard.m_pendingPrimaryFetch = true;
    primary.sourceClipboard = makeSourcePathsClipboard(barrier::fs::u8path("/tmp/local-file.txt"));
    primary.clipboardAvailable = true;

    server.onClipboardChanged(&primary, kClipboardClipboard, 10);

    EXPECT_FALSE(clipboard.m_pendingPrimaryFetch);
    EXPECT_TRUE(server.m_remoteFileClipboardSession.empty());
    EXPECT_TRUE(server.m_readyFileClipboardSession.empty());
    EXPECT_TRUE(server.m_readyFileClipboardPaths.empty());
    EXPECT_EQ(0u, client.clipboardDirtyCount);
    EXPECT_EQ(0u, client.setClipboardCount);

    clipboard.m_pendingPrimaryFetch = true;
    primary.sourceClipboard = makeTextClipboard("plain text after local file copy");

    server.onClipboardChanged(&primary, kClipboardClipboard, 11);

    EXPECT_FALSE(clipboard.m_pendingPrimaryFetch);
    EXPECT_EQ(1u, client.clipboardDirtyCount);
    EXPECT_TRUE(client.lastClipboardDirty);
    EXPECT_EQ(1u, client.setClipboardCount);
    ASSERT_TRUE(clipboard.m_clipboard.has(IClipboard::kText));
    EXPECT_EQ("plain text after local file copy", clipboard.m_clipboard.get(IClipboard::kText));
    ASSERT_TRUE(client.lastSetClipboard.has(IClipboard::kText));
    EXPECT_EQ("plain text after local file copy", client.lastSetClipboard.get(IClipboard::kText));
    EXPECT_FALSE(client.lastSetClipboard.has(IClipboard::kFileList));
}

TEST(ServerReconnectTests, onClipboardChangedForwardsMaterializedFileList)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    ClipboardPrimaryClient primary;
    RecordingClient client("client");
    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);
    server.m_enableClipboard = true;
    server.m_active = &client;

    Server::ClipboardInfo& clipboard = server.m_clipboards[kClipboardClipboard];
    clipboard.m_clipboardOwner = primary.getName();
    clipboard.m_clipboardSeqNum = 10;
    clipboard.m_pendingPrimaryFetch = true;
    primary.sourceClipboard = makeMaterializedPathsClipboard(
        barrier::fs::u8path("/tmp/weave-cache/remote-file.txt"));
    primary.clipboardAvailable = true;

    server.onClipboardChanged(&primary, kClipboardClipboard, 10);

    EXPECT_FALSE(clipboard.m_pendingPrimaryFetch);
    EXPECT_EQ(1u, client.clipboardDirtyCount);
    EXPECT_TRUE(client.lastClipboardDirty);
    EXPECT_EQ(1u, client.setClipboardCount);
    EXPECT_EQ(1u, primary.clipboardDirtyCount);
    EXPECT_EQ(0u, primary.setClipboardCount);

    RemoteFileClipboard::Data remoteFileClipboard;
    ASSERT_TRUE(RemoteFileClipboard::readFromClipboard(
        clipboard.m_clipboard, remoteFileClipboard));
    EXPECT_EQ(RemoteFileClipboard::Mode::MaterializedPaths, remoteFileClipboard.mode);
    EXPECT_EQ("remote-ready-session", remoteFileClipboard.sessionId);
}

TEST(ServerReconnectTests, onClipboardChangedStripsImageFileListMetadataBeforeForwarding)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    ClipboardPrimaryClient primary;
    RecordingClient client("client");
    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);
    server.m_enableClipboard = true;
    server.m_active = &client;

    Server::ClipboardInfo& clipboard = server.m_clipboards[kClipboardClipboard];
    clipboard.m_clipboardOwner = primary.getName();
    clipboard.m_clipboardSeqNum = 10;
    clipboard.m_pendingPrimaryFetch = true;
    primary.sourceClipboard = makeImageClipboardWithSourcePathMetadata(
        barrier::fs::u8path("/tmp/image-copy.png"));
    primary.clipboardAvailable = true;

    server.onClipboardChanged(&primary, kClipboardClipboard, 10);

    EXPECT_FALSE(clipboard.m_pendingPrimaryFetch);
    EXPECT_EQ(1u, client.clipboardDirtyCount);
    EXPECT_TRUE(client.lastClipboardDirty);
    EXPECT_EQ(1u, client.setClipboardCount);
    EXPECT_TRUE(clipboard.m_clipboard.has(IClipboard::kPNG));
    EXPECT_FALSE(clipboard.m_clipboard.has(IClipboard::kFileList));
    EXPECT_FALSE(clipboard.m_clipboard.has(IClipboard::kText));
    ASSERT_TRUE(client.lastSetClipboard.has(IClipboard::kPNG));
    EXPECT_FALSE(client.lastSetClipboard.has(IClipboard::kFileList));
    EXPECT_FALSE(client.lastSetClipboard.has(IClipboard::kText));
}

TEST(ServerReconnectTests, delayedSwitchReanchorsActiveWhenPrimaryCannotBeEntered)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    UnenterablePrimaryClient primary;
    RecordingClient client("client");
    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_clients.insert(std::make_pair(primary.getName(), &primary));
    server.m_clientSet.insert(&primary);
    server.m_x = client.shapeX - 20;
    server.m_y = 100;
    server.m_switchScreen = &primary;
    server.m_switchWaitX = 0;
    server.m_switchWaitY = 0;

    server.handleSwitchWaitTimeout(Event(Event::kUnknown), NULL);

    EXPECT_EQ(&client, server.m_active);
    EXPECT_GE(server.m_x, client.shapeX);
    EXPECT_LT(server.m_x, client.shapeX + client.shapeW);
    EXPECT_EQ(100, server.m_y);
    EXPECT_EQ(1u, client.mouseMoveCount);
    EXPECT_EQ(server.m_x, client.mouseMoveX);
    EXPECT_EQ(100, client.mouseMoveY);
    EXPECT_EQ(NULL, server.m_switchScreen);
}

TEST(ServerReconnectTests, invalidFileCompletionReleasesReceiveState)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    NiceMock<MockPrimaryClient> primary;
    ON_CALL(primary, getToggleMask()).WillByDefault(Return(KeyModifierCapsLock));
    ON_CALL(primary, getEventTarget()).WillByDefault(Return(&primary));

    RecordingClient client("client");
    Server server;
    initializeServer(server, config, primary, events, client);

    const barrier::fs::path spoolPath =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-server-invalid-complete.part");
    barrier::fs::remove(spoolPath);
    {
        std::ofstream file(spoolPath.u8string().c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        file << "partial";
    }

    server.getExpectedFileSize() = 1024;
    server.getReceivedFileData() = "stale";
    server.getReceivedFileSpoolPath() = spoolPath;

    server.onFileRecieveCompleted();

    EXPECT_EQ(0u, server.getExpectedFileSize());
    EXPECT_TRUE(server.getReceivedFileData().empty());
    EXPECT_TRUE(server.getReceivedFileSpoolPath().empty());
    EXPECT_FALSE(barrier::fs::exists(spoolPath));
}

TEST(ServerReconnectTests, singleFileTransferRejectsSymlinkSource)
{
    Server server;

    const barrier::fs::path target =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-server-regular-source.txt");
    const barrier::fs::path link =
        barrier::fs::temp_directory_path() / barrier::fs::u8path("weave-server-symlink-source.txt");
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
    EXPECT_FALSE(testServerPrepareTransferSource(link.u8string().c_str(),
                                                 sourcePath,
                                                 tempPackagePath,
                                                 error));
    EXPECT_EQ("transfer source must be a regular file", error);
    EXPECT_TRUE(tempPackagePath.empty());

    barrier::fs::remove(link, ec);
    barrier::fs::remove(target, ec);
}

TEST(ServerReconnectTests, invalidRemoteClipboardPackageDoesNotPublishReadyClipboard)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    UInt32 readyEvents = 0;
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&readyEvents](const Event& event) {
            if (event.getDataObject() != NULL) {
                ++readyEvents;
            }
            Event::deleteData(event);
        }));

    NiceMock<MockPrimaryClient> primary;
    RecordingClient client("client");
    Server server;
    initializeServer(server, config, primary, events, client);

    server.testWriteRemoteClipboardTransfer(invalidTransferPackageData(), "bad-session");

    EXPECT_EQ(0u, readyEvents);
}

TEST(ServerReconnectTests, staleFileClipboardReadyDoesNotReplacePendingSession)
{
    Config config;
    config.addScreen("primary");
    config.addScreen("client");

    NiceMock<MockEventQueue> events;
    ClientProxyEvents clientProxyEvents;
    IScreenEvents screenEvents;
    ClipboardEvents clipboardEvents;
    ServerEvents serverEvents;
    setEventTypeDefaults(events, clientProxyEvents, screenEvents, clipboardEvents, serverEvents);

    UInt32 publishedEvents = 0;
    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&publishedEvents](const Event& event) {
            if (event.getDataObject() != NULL) {
                ++publishedEvents;
            }
            Event::deleteData(event);
        }));

    NiceMock<MockPrimaryClient> primary;
    RecordingClient client("client");
    Server server;
    initializeServer(server, config, primary, events, client);
    server.m_remoteFileClipboardSession = "current-session";
    server.m_readyFileClipboardSession = "ready-session";
    server.m_readyFileClipboardPaths.push_back("/tmp/current-ready.txt");

    Server::FileClipboardReadyInfo* info = new Server::FileClipboardReadyInfo();
    info->m_sessionId = "stale-session";
    info->m_paths.push_back("/tmp/stale-ready.txt");
    info->m_publishClipboard = false;
    Event event(Event::kUnknown, &server, info);
    event.setDataObject(info);

    server.handleFileClipboardReadyEvent(event, NULL);
    Event::deleteData(event);

    EXPECT_EQ("current-session", server.m_remoteFileClipboardSession);
    EXPECT_EQ("ready-session", server.m_readyFileClipboardSession);
    ASSERT_EQ(1u, server.m_readyFileClipboardPaths.size());
    EXPECT_EQ("/tmp/current-ready.txt", server.m_readyFileClipboardPaths[0]);
    EXPECT_EQ(0u, publishedEvents);
}
