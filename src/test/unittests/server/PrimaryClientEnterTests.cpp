#include "server/PrimaryClient.h"
#include "barrier/Screen.h"
#include "barrier/IPlatformScreen.h"
#include "base/EventQueue.h"
#include "test/global/gtest.h"
#include <vector>
#include <string>

namespace {
// Only the native platform boundary is replaced. PrimaryClient and Screen
// retain their real dispatch, sequence, and ownership bookkeeping.
class EnterProbe : public IPlatformScreen {
public:
    EnterProbe() : IPlatformScreen(nullptr) {}
    std::vector<std::string> calls;
    bool accepted = true;
    UInt32 sequence = 0;
    SInt32 x = 0, y = 0;
    void enable() override {}
    void disable() override {}
    void enter() override { calls.push_back("enter"); }
    bool tryEnter() override { enter(); return accepted; }
    bool leave() override { return true; }
    bool setClipboard(ClipboardID, const IClipboard*) override { return true; }
    void checkClipboards() override {}
    void openScreensaver(bool) override {}
    void closeScreensaver() override {}
    void screensaver(bool) override {}
    void resetOptions() override {}
    void setOptions(const OptionsList&) override {}
    void setSequenceNumber(UInt32 value) override { sequence = value; }
    bool isPrimary() const override { return true; }
    void* getEventTarget() const override { return const_cast<EnterProbe*>(this); }
    bool getClipboard(ClipboardID, IClipboard*) const override { return false; }
    void getShape(SInt32& a, SInt32& b, SInt32& w, SInt32& h) const override { a=b=0; w=1920; h=1080; }
    void getCursorPos(SInt32& a, SInt32& b) const override { a=b=0; }
    void reconfigure(UInt32) override {}
    void warpCursor(SInt32 a, SInt32 b) override { calls.push_back("warp"); x=a; y=b; }
    UInt32 registerHotKey(KeyID, KeyModifierMask) override { return 0; }
    void unregisterHotKey(UInt32) override {}
    void fakeInputBegin() override {}
    void fakeInputEnd() override {}
    SInt32 getJumpZoneSize() const override { return 0; }
    bool isAnyMouseButtonDown(UInt32&) const override { return false; }
    void getCursorCenter(SInt32& a, SInt32& b) const override { a=960; b=540; }
    void fakeMouseButton(ButtonID, bool) override {}
    void fakeMouseMove(SInt32, SInt32) override {}
    void fakeMouseRelativeMove(SInt32, SInt32) const override {}
    void fakeMouseWheel(SInt32, SInt32) const override {}
    void updateKeyMap() override {}
    void updateKeyState() override {}
    void setHalfDuplexMask(KeyModifierMask) override {}
    void fakeKeyDown(KeyID, KeyModifierMask, KeyButton) override {}
    bool fakeKeyRepeat(KeyID, KeyModifierMask, SInt32, KeyButton) override { return true; }
    bool fakeKeyUp(KeyButton) override { return true; }
    void fakeAllKeysUp() override {}
    bool fakeCtrlAltDel() override { return false; }
    bool isKeyDown(KeyButton) const override { return false; }
    KeyModifierMask getActiveModifiers() const override { return 0; }
    KeyModifierMask pollActiveModifiers() const override { return 0; }
    SInt32 pollActiveGroup() const override { return 0; }
    void pollPressedKeys(KeyButtonSet&) const override {}
    String text;
    void setDraggingStarted(bool) override {}
    String& getDraggingFilename() override { return text; }
    void clearDraggingFilename() override {}
    bool isDraggingStarted() override { return false; }
    bool isFakeDraggingStarted() override { return false; }
    void fakeDraggingFiles(DragFileList) override {}
    const String& getDropTarget() const override { return text; }
    void setDropTarget(const String&) override {}
    void handleSystemEvent(const Event&, void*) override {}
};

class AtomicEnterProbe : public EnterProbe {
public:
    bool tryEnterAt(SInt32 a, SInt32 b) override {
        calls.push_back("enter-at"); x=a; y=b;
        return accepted;
    }
};

template<class Platform>
class PrimaryEnterHarness {
public:
    EventQueue events;
    Platform* platform = new Platform;
    barrier::Screen screen{platform, &events};
    PrimaryClient primary{"primary", &screen};
    PrimaryEnterHarness() { screen.leave(); }
    ~PrimaryEnterHarness() {
        platform->accepted = true;
        if (!screen.isOnScreen()) screen.enter(0);
    }
};
}

TEST(PrimaryClientEnterTests, normalReturnUsesOnePlatformTransactionWithoutSeparateWarp)
{
    PrimaryEnterHarness<AtomicEnterProbe> h;
    h.primary.enter(37, 83, 11, 0, false);
    EXPECT_EQ((std::vector<std::string>{"enter-at"}), h.platform->calls);
    EXPECT_EQ(37, h.platform->x); EXPECT_EQ(83, h.platform->y);
    EXPECT_EQ(11u, h.platform->sequence);
    EXPECT_TRUE(h.screen.isOnScreen());
}

TEST(PrimaryClientEnterTests, rejectedAtomicReturnDoesNotMarkScreenEnteredOrWarpSeparately)
{
    PrimaryEnterHarness<AtomicEnterProbe> h;
    h.platform->accepted = false;
    h.primary.enter(37, 83, 12, 0, false);
    EXPECT_EQ((std::vector<std::string>{"enter-at"}), h.platform->calls);
    EXPECT_FALSE(h.screen.isOnScreen());
}

TEST(PrimaryClientEnterTests, screensaverReturnNeverWarpsOrUsesPositionTransaction)
{
    PrimaryEnterHarness<AtomicEnterProbe> h;
    h.primary.enter(-1, -1, 13, 0, true);
    EXPECT_EQ((std::vector<std::string>{"enter"}), h.platform->calls);
    EXPECT_EQ(13u, h.platform->sequence);
    EXPECT_TRUE(h.screen.isOnScreen());
}

TEST(PrimaryClientEnterTests, legacyPlatformRetainsWarpThenEnterOrdering)
{
    PrimaryEnterHarness<EnterProbe> h;
    h.primary.enter(37, 83, 14, 0, false);
    EXPECT_EQ((std::vector<std::string>{"warp", "enter"}), h.platform->calls);
    EXPECT_EQ(37, h.platform->x); EXPECT_EQ(83, h.platform->y);
    EXPECT_TRUE(h.screen.isOnScreen());
}
