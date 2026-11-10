#include "platform/OSXScreen.h"
#include "platform/OSXKeyState.h"
#include "platform/OSXMediaKeySupport.h"
#include "barrier/Screen.h"
#include "base/EventQueue.h"
#include "test/global/gtest.h"

#include <atomic>
#include <future>
#include <mutex>
#include <vector>
#include <unistd.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <IOKit/hidsystem/ev_keymap.h>

// Native output boundaries only: these tests must never post input or reserve
// a real hotkey on the build host. The screen, key state and input loop are real.
namespace {
std::atomic<unsigned> postedEvents{0};
std::atomic<unsigned> postedMouseUps{0};
std::atomic<unsigned> nativeButtons{0};
std::atomic<CGEventFlags> postedMouseFlags{0};
std::vector<CGEventType> deliveredButtons;
std::vector<std::pair<KeyButton, bool>> deliveredKeys;
}
extern "C" void CGEventPost(CGEventTapLocation, CGEventRef event) {
    ++postedEvents;
    postedMouseFlags = CGEventGetFlags(event);
    if (CGEventGetType(event) == kCGEventLeftMouseUp) ++postedMouseUps;
    if (CGEventGetType(event) == kCGEventLeftMouseDown || CGEventGetType(event) == kCGEventLeftMouseUp ||
        CGEventGetType(event) == kCGEventRightMouseDown || CGEventGetType(event) == kCGEventRightMouseUp)
        deliveredButtons.push_back(CGEventGetType(event));
}
extern "C" UInt32 GetCurrentButtonState() { return nativeButtons.load(); }
extern "C" OSStatus RegisterEventHotKey(UInt32, UInt32, EventHotKeyID,
                                       EventTargetRef, OptionBits, EventHotKeyRef* ref)
{
    *ref = reinterpret_cast<EventHotKeyRef>(1);
    return noErr;
}
extern "C" OSStatus UnregisterEventHotKey(EventHotKeyRef) { return noErr; }

class SnapshotKeyState : public OSXKeyState {
public:
    explicit SnapshotKeyState(IEventQueue* events) : OSXKeyState(events) {}
    SInt32 pollActiveGroup() const override { return 0; }
    void pollPressedKeys(KeyButtonSet& keys) const override
    {
        ++polls;
        keys.insert(physicalKeys.begin(), physicalKeys.end());
    }
    KeyButtonSet physicalKeys;
    KeyModifierMask physicalMask = 0;
    mutable std::atomic<unsigned> polls{0};
    std::vector<bool> synthesized;
    std::vector<KeyButton> synthesizedButtons;
    std::vector<UInt32> nativeModifierFlags;
    UInt32 nativeGlobalFlags = 0;
    std::vector<std::pair<UInt8, UInt32>> nativeOrdinaryFlags;
protected:
    KeyModifierMask pollNativeModifiers() const override { return physicalMask; }
    void getKeyMap(barrier::KeyMap& map) override
    {
        barrier::KeyMap::KeyItem tab = {};
        tab.m_id = kKeyTab;
        tab.m_button = kVK_Tab + 1;
        map.addKeyEntry(tab);
        barrier::KeyMap::KeyItem shift = {};
        shift.m_id = kKeyShift_L;
        shift.m_button = kVK_Shift + 1;
        shift.m_generates = KeyModifierShift;
        map.addKeyEntry(shift);
        barrier::KeyMap::KeyItem control = {};
        control.m_id = kKeyControl_L;
        control.m_button = kVK_Control + 1;
        control.m_generates = KeyModifierControl;
        map.addKeyEntry(control);
        for (const auto& key : {std::make_pair(KeyID('l'), KeyButton(kVK_ANSI_L + 1)),
                                std::make_pair(KeyID('k'), KeyButton(kVK_ANSI_K + 1))}) {
            barrier::KeyMap::KeyItem letter = {};
            letter.m_id = key.first;
            letter.m_button = key.second;
            letter.m_sensitive = KeyModifierShift;
            map.addKeyEntry(letter);
        }
    }
    void fakeKey(const Keystroke& stroke) override
    {
        if (stroke.m_type == Keystroke::kButton) {
            synthesized.push_back(stroke.m_data.m_button.m_press);
            synthesizedButtons.push_back(stroke.m_data.m_button.m_button);
            deliveredKeys.emplace_back(stroke.m_data.m_button.m_button, stroke.m_data.m_button.m_press);
            OSXKeyState::fakeKey(stroke);
        }
    }
    void postNativeKeyEvent(UInt32, UInt8 code, UInt32 flags, bool updateModifiers) override
    {
        if (updateModifiers) {
            nativeModifierFlags.push_back(flags);
            nativeGlobalFlags = flags;
        }
        else nativeOrdinaryFlags.emplace_back(code, nativeGlobalFlags);
        // Deliberately no IOHID service lookup or post.
    }
};

class OSXScreenInputStateTestAccess {
public:
    static SnapshotKeyState& prepare(OSXScreen& screen, bool primary, bool owner,
                                     KeyModifierMask initialMask = 0)
    {
        screen.m_isPrimary = primary;
        screen.m_isOnScreen = true;
        screen.m_cursorPosValid = true;
        delete screen.m_keyState;
        auto* keys = new SnapshotKeyState(screen.getEvents());
        screen.m_keyState = keys;
        keys->updateKeyMap();
        if (initialMask != 0) {
            keys->physicalMask = initialMask;
            if (initialMask & KeyModifierShift) keys->physicalKeys.insert(kVK_Shift + 1);
            if (initialMask & KeyModifierControl) keys->physicalKeys.insert(kVK_Control + 1);
            keys->physicalKeys.insert(kVK_ANSI_L + 1);
            keys->updateKeyState(); // The real pre-tap startup poll.
        }
        if (primary && owner) screen.initializeInputState();
        if (owner) screen.m_inputLoop.reset(new OSXInputLoop);
        return *keys;
    }
    static OSXInputLoop& loop(OSXScreen& screen) { return *screen.m_inputLoop; }
    static void restartOwner(OSXScreen& screen)
    {
        screen.initializeInputState();
        screen.m_inputLoop.reset(new OSXInputLoop);
    }
    static bool key(OSXScreen& screen, CGEventType type, CGKeyCode code, CGEventFlags flags = 0)
    {
        CGEventRef event = CGEventCreateKeyboardEvent(nullptr, code, type == kCGEventKeyDown);
        CGEventSetType(event, type);
        CGEventSetFlags(event, flags);
        const bool handled = screen.onKey(event);
        CFRelease(event);
        return handled;
    }
    // Exercise the native callback boundary, including origin filtering and
    // local-versus-remote OS delivery. Nothing is posted to the host desktop.
    static bool callbackKey(OSXScreen& screen, CGEventType type, CGKeyCode code,
                            pid_t source, CGEventFlags flags = 0)
    {
        CGEventRef event = CGEventCreateKeyboardEvent(nullptr, code, type == kCGEventKeyDown);
        EXPECT_NE(nullptr, event);
        if (!event) return false;
        CGEventSetType(event, type);
        CGEventSetFlags(event, flags);
        CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, source);
        const bool delivered = OSXScreen::handleCGInputEvent(nullptr, type, event, &screen) == event;
        if (delivered && (type == kCGEventKeyDown || type == kCGEventKeyUp))
            deliveredKeys.emplace_back(code + 1, type == kCGEventKeyDown);
        if (delivered && type == kCGEventFlagsChanged && code == kVK_Shift)
            deliveredKeys.emplace_back(code + 1, (flags & kCGEventFlagMaskShift) != 0);
        CFRelease(event);
        return delivered;
    }
    static bool callbackButton(OSXScreen& screen, bool down, CGMouseButton button = kCGMouseButtonLeft)
    {
        const auto type = button == kCGMouseButtonRight ?
            (down ? kCGEventRightMouseDown : kCGEventRightMouseUp) :
            (down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp);
        CGEventRef event = CGEventCreateMouseEvent(nullptr, type, CGPointZero, button);
        CGEventSetFlags(event, 0);
        CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, 0);
        const bool delivered = OSXScreen::handleCGInputEvent(nullptr, type, event, &screen) == event;
        if (delivered) deliveredButtons.push_back(type);
        CFRelease(event);
        return delivered;
    }
    static void local(OSXScreen& screen, bool value) { screen.m_isOnScreen = value; }
    static bool observedDown(const OSXScreen& screen, KeyButton button) {
        return screen.m_primaryKeysDown.test(button);
    }
    static KeyModifierMask observedModifiers(const OSXScreen& screen) { return screen.m_primaryModifiers; }
    static bool cachedLeftButton(const OSXScreen& screen) { return screen.m_buttonState.test(0); }
    static bool cachedRightButton(const OSXScreen& screen) { return screen.m_buttonState.test(1); }
    static bool dragging(const OSXScreen& screen) { return screen.m_draggingStarted; }
    static void button(OSXScreen& screen, bool down)
    {
        screen.m_buttonState.set(0, down ? OSXScreen::kMouseButtonDown : OSXScreen::kMouseButtonUp);
    }
    static void mouseButton(OSXScreen& screen, bool down, CGEventFlags flags = 0)
    {
        if (!screen.m_isPrimary) {
            screen.onMouseButton(down, kButtonLeft);
            return;
        }
        const CGEventType type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
        CGEventRef event = CGEventCreateMouseEvent(nullptr, type, CGPointZero, kCGMouseButtonLeft);
        CGEventSetFlags(event, flags);
        CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, 0);
        OSXScreen::handleCGInputEvent(nullptr, type, event, &screen);
        CFRelease(event);
    }
    static void mediaKey(OSXScreen& screen, bool down, CGEventFlags flags = 0)
    {
        // Build an in-memory NSEvent using the runtime because this test file
        // is C++. The real Cocoa decoder runs; the event is never posted.
        using Message = id (*)(id, SEL);
        auto message = reinterpret_cast<Message>(objc_msgSend);
        id pool = message(reinterpret_cast<id>(objc_getClass("NSAutoreleasePool")), sel_registerName("new"));
        using OtherEvent = id (*)(id, SEL, unsigned long, CGPoint, unsigned long,
                                  double, long, id, short, long, long);
        auto otherEvent = reinterpret_cast<OtherEvent>(objc_msgSend);
        const long data = (NX_KEYTYPE_SOUND_UP << 16) | ((down ? 0xa : 0xb) << 8);
        id nativeEvent = otherEvent(reinterpret_cast<id>(objc_getClass("NSEvent")),
            sel_registerName("otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:subtype:data1:data2:"),
            14 /* NSEventTypeSystemDefined */, CGPointZero, flags, 0.0, 0, nullptr, 8, data, -1);
        auto toCGEvent = reinterpret_cast<CGEventRef (*)(id, SEL)>(objc_msgSend);
        CGEventRef event = toCGEvent(nativeEvent, sel_registerName("CGEvent"));
        EXPECT_TRUE(isMediaKeyEvent(event));
        screen.onMediaKey(event);
        message(pool, sel_registerName("drain"));
    }
    static bool hotKey(OSXScreen& screen, EventRef event) { return screen.onHotKey(event); }
};

class StateEventQueue : public EventQueue {
public:
    struct Key { bool down; KeyButton button; KeyModifierMask mask; KeyID id; };
    struct Button { bool down; ButtonID button; KeyModifierMask mask; };
    std::vector<Key> keys;
    std::vector<Button> buttons;
    std::vector<UInt32> hotkeys;
    std::mutex mutex;
    void addEvent(const Event& event) override
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (event.getType() == forIKeyState().keyDown() || event.getType() == forIKeyState().keyUp()) {
            const auto* key = static_cast<IKeyState::KeyInfo*>(event.getData());
            keys.push_back({event.getType() == forIKeyState().keyDown(), key->m_button, key->m_mask, key->m_key});
        }
        else if (event.getType() == forIPrimaryScreen().buttonDown() ||
                 event.getType() == forIPrimaryScreen().buttonUp()) {
            const auto* button = static_cast<IPrimaryScreen::ButtonInfo*>(event.getData());
            buttons.push_back({event.getType() == forIPrimaryScreen().buttonDown(), button->m_button, button->m_mask});
        }
        else if (event.getType() == forIPrimaryScreen().hotKeyDown() ||
                 event.getType() == forIPrimaryScreen().hotKeyUp()) {
            hotkeys.push_back(static_cast<IPrimaryScreen::HotKeyInfo*>(event.getData())->m_id);
        }
        Event::deleteData(event);
    }
};

// Every blocked-owner test catches a caller-thread access that would overlap
// a tap callback. A barrier fixes the ordering; the timeout only detects that
// the public call has not completed before the callback releases ownership.
class OwnerBlock {
public:
    explicit OwnerBlock(OSXScreen& screen) : m_gate(m_release.get_future().share())
    {
        m_running = std::async(std::launch::async, [&screen, this] {
            return OSXScreenInputStateTestAccess::loop(screen).invoke([this] {
                m_entered.set_value();
                m_gate.wait();
            });
        });
        m_entered.get_future().wait();
    }
    ~OwnerBlock() { release(); }
    void release()
    {
        if (!m_released) {
            m_released = true;
            m_release.set_value();
            EXPECT_TRUE(m_running.get());
        }
    }
private:
    std::promise<void> m_entered, m_release;
    std::shared_future<void> m_gate;
    std::future<bool> m_running;
    bool m_released = false;
};

class OSXScreenInputStateTests : public testing::Test {
protected:
    enum class Release { Key, Button, AllKeys };
    void SetUp() override {
        nativeButtons = 0; postedEvents = postedMouseUps = 0;
        deliveredButtons.clear(); deliveredKeys.clear();
    }
    void expectTimedOutReleaseCompletes(Release release, bool duringStop = false)
    {
        StateEventQueue events;
        OSXScreen screen(&events, false, false);
        auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
        if (release == Release::Button) screen.fakeMouseButton(kButtonLeft, true);
        else screen.fakeKeyDown(kKeyTab, 0, 7);

        // Leave no synchronous invoke caller on the loop when disable destroys
        // it. This models a native callback owning the loop, not a second
        // business thread racing destruction of an API call's receiver.
        auto entered = std::make_shared<std::promise<void>>();
        std::promise<void> resume;
        const auto gate = resume.get_future().share();
        auto blocking = std::async(std::launch::async, [&] {
            return OSXScreenInputStateTestAccess::loop(screen).invokeRelease(
                [entered, gate] { entered->set_value(); gate.wait(); },
                std::chrono::milliseconds(20));
        });
        entered->get_future().wait();
        EXPECT_EQ(OSXInputLoop::ReleaseResult::Pending, blocking.get());
        auto releasing = std::async(std::launch::async, [&] {
            if (release == Release::Key) return screen.fakeKeyUp(7);
            if (release == Release::Button) screen.fakeMouseButton(kButtonLeft, false);
            else screen.fakeAllKeysUp();
            return true;
        });
        // The caller may return after its deadline, but the release obligation
        // must outlive its stack frame and complete when the owner recovers.
        const auto completion = releasing.wait_for(std::chrono::milliseconds(1500));
        EXPECT_EQ(std::future_status::ready, completion);
        if (completion != std::future_status::ready) {
            resume.set_value();
            releasing.get();
            return;
        }
        EXPECT_TRUE(releasing.get());
        if (duringStop) {
            auto& loop = OSXScreenInputStateTestAccess::loop(screen);
            auto stopping = std::async(std::launch::async, [&] { screen.disable(); });
            auto admission = OSXInputLoop::ReleaseResult::Pending;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (admission != OSXInputLoop::ReleaseResult::Rejected &&
                   std::chrono::steady_clock::now() < deadline) {
                admission = loop.invokeRelease([] {}, std::chrono::milliseconds(1));
            }
            EXPECT_EQ(OSXInputLoop::ReleaseResult::Rejected, admission);
            resume.set_value();
            stopping.get();
        }
        else {
            resume.set_value();
            ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([] {}));
        }
        if (release == Release::Button) {
            EXPECT_EQ(1u, postedMouseUps.load());
            UInt32 button = 0;
            EXPECT_FALSE(screen.isAnyMouseButtonDown(button));
        }
        else {
            EXPECT_EQ(std::vector<bool>({true, false}), keys.synthesized);
            EXPECT_FALSE(screen.isKeyDown(kVK_Tab + 1));
        }
    }
};

TEST_F(OSXScreenInputStateTests, timedOutPrimaryKeyReleaseStillExecutesAfterOwnerRecovers)
{
    expectTimedOutReleaseCompletes(Release::Key);
}

TEST_F(OSXScreenInputStateTests, timedOutPrimaryButtonReleaseStillExecutesAfterOwnerRecovers)
{
    expectTimedOutReleaseCompletes(Release::Button);
}

TEST_F(OSXScreenInputStateTests, timedOutPrimaryAllKeysReleaseStillExecutesAfterOwnerRecovers)
{
    expectTimedOutReleaseCompletes(Release::AllKeys);
}

TEST_F(OSXScreenInputStateTests, timedOutPrimaryKeyReleaseDrainsBeforeDisable)
{
    expectTimedOutReleaseCompletes(Release::Key, true);
}

TEST_F(OSXScreenInputStateTests, timedOutPrimaryButtonReleaseDrainsBeforeDisable)
{
    expectTimedOutReleaseCompletes(Release::Button, true);
}

TEST_F(OSXScreenInputStateTests, timedOutPrimaryAllKeysReleaseDrainsBeforeDisable)
{
    expectTimedOutReleaseCompletes(Release::AllKeys, true);
}

TEST_F(OSXScreenInputStateTests, sameProcessReplayKeepsPhysicalReleasePairedAcrossReturnAndReleave)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        // A physical press is held remotely, then replayed on return. Model
        // ownership transitions here; exercise real fake-key state and the tap.
        OSXScreenInputStateTestAccess::local(screen, false);
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyDown, kVK_Tab, 0));
        OSXScreenInputStateTestAccess::local(screen, true);
        screen.fakeKeyDown(kKeyTab, 0, 7);
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyDown, kVK_Tab, getpid()));
        // Re-leave cleans the synthetic local press while the physical key is
        // still held. Its own-PID echo must reach the OS, not consume the ledger.
        OSXScreenInputStateTestAccess::local(screen, false);
        EXPECT_TRUE(screen.fakeKeyUp(7));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, getpid()));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::observedDown(screen, kVK_Tab + 1));
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
        EXPECT_FALSE(OSXScreenInputStateTestAccess::observedDown(screen, kVK_Tab + 1));
    }));
    EXPECT_EQ(std::vector<bool>({true, false}), keys.synthesized);
    ASSERT_EQ(2u, events.keys.size());
    EXPECT_TRUE(events.keys[0].down);
    EXPECT_FALSE(events.keys[1].down);
    EXPECT_EQ(kVK_Tab + 1, events.keys[1].button);
}

TEST_F(OSXScreenInputStateTests, physicalRefreshDoesNotEraseSyntheticReleaseObligation)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeKeyDown(kKeyTab, 0, 7);
    // The refresh is independent of tap echo: real KeyState must retain the
    // synthetic server-button binding until its mandatory release completes.
    screen.updateKeyState();
    EXPECT_TRUE(screen.fakeKeyUp(7));
    EXPECT_EQ(std::vector<bool>({true, false}), keys.synthesized);
}

TEST_F(OSXScreenInputStateTests, returnRefreshDoesNotRestoreARemotelyReleasedShift)
{
    StateEventQueue events;
    auto* native = new OSXScreen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(*native, true, true, KeyModifierShift);
    barrier::Screen screen(native, &events);
    // Native DEBUG evidence: Carbon stays Shift after the tap suppresses the
    // physical Up. Ordinary IOHID keys inherit that native global flag.
    keys.nativeGlobalFlags = NX_SHIFTMASK | NX_DEVICELSHIFTKEYMASK;
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(*native).invoke([&] {
        OSXScreenInputStateTestAccess::local(*native, false);
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(*native,
            kCGEventFlagsChanged, kVK_Shift, 0, 0));
        EXPECT_EQ(0, OSXScreenInputStateTestAccess::observedModifiers(*native));
        EXPECT_FALSE(keys.hasSyntheticKeyBindings());
    }));
    native->enter();
    screen.refreshKeyState(); // Actual Server return boundary, before replay.
    EXPECT_EQ(0, native->getActiveModifiers());
    native->fakeKeyDownForHandoff('l', 0, kVK_ANSI_L + 1, 1);
    ASSERT_EQ(1u, keys.nativeOrdinaryFlags.size());
    EXPECT_EQ(0u, keys.nativeOrdinaryFlags[0].second);
    EXPECT_EQ(0u, keys.nativeGlobalFlags);
    EXPECT_EQ(std::vector<UInt32>({0}), keys.nativeModifierFlags);
    EXPECT_TRUE(native->fakeKeyUpForHandoff(kVK_ANSI_L + 1, 1));
    screen.refreshKeyState(); // Completion must not be posted again.
    native->fakeKeyDown('k', 0, kVK_ANSI_K + 1);
    ASSERT_FALSE(keys.nativeOrdinaryFlags.empty());
    EXPECT_EQ(0u, keys.nativeOrdinaryFlags.back().second);
    EXPECT_EQ(std::vector<UInt32>({0}), keys.nativeModifierFlags);
    EXPECT_TRUE(native->fakeKeyUp(kVK_ANSI_K + 1));
}

TEST_F(OSXScreenInputStateTests, returnReleasedShiftPreservesAnotherObservedHeldModifier)
{
    StateEventQueue events;
    OSXScreen native(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(native, true, true,
        KeyModifierShift | KeyModifierControl);
    keys.nativeGlobalFlags = NX_SHIFTMASK | NX_DEVICELSHIFTKEYMASK |
        NX_CONTROLMASK | NX_DEVICELCTLKEYMASK;
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(native).invoke([&] {
        OSXScreenInputStateTestAccess::local(native, false);
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(native,
            kCGEventFlagsChanged, kVK_Shift, 0, NX_CONTROLMASK | NX_DEVICELCTLKEYMASK));
    }));
    native.enter();
    native.updateKeyState();
    EXPECT_EQ(KeyModifierControl, native.getActiveModifiers());
    native.fakeKeyDownForHandoff('l', KeyModifierControl, kVK_ANSI_L + 1, 1);
    ASSERT_EQ(1u, keys.nativeOrdinaryFlags.size());
    EXPECT_EQ(UInt32(NX_CONTROLMASK | NX_DEVICELCTLKEYMASK), keys.nativeOrdinaryFlags[0].second);
    EXPECT_EQ(std::vector<UInt32>({NX_CONTROLMASK | NX_DEVICELCTLKEYMASK}), keys.nativeModifierFlags);
    EXPECT_TRUE(native.fakeKeyUpForHandoff(kVK_ANSI_L + 1, 1));
}

TEST_F(OSXScreenInputStateTests, remoteModifierRepressDoesNotReleaseTheHeldModifierOnReturn)
{
    StateEventQueue events;
    OSXScreen native(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(native, true, true, KeyModifierShift);
    keys.nativeGlobalFlags = NX_SHIFTMASK | NX_DEVICELSHIFTKEYMASK;
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(native).invoke([&] {
        OSXScreenInputStateTestAccess::local(native, false);
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(native,
            kCGEventFlagsChanged, kVK_Shift, 0, 0));
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(native,
            kCGEventFlagsChanged, kVK_Shift, 0, kCGEventFlagMaskShift));
    }));
    native.enter();
    native.updateKeyState();
    EXPECT_EQ(KeyModifierShift, native.getActiveModifiers());
    EXPECT_TRUE(keys.nativeModifierFlags.empty());
}

TEST_F(OSXScreenInputStateTests, stoppingWhileRemoteCompletesTheSuppressedNativeModifierRelease)
{
    StateEventQueue events;
    OSXScreen native(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(native, true, true, KeyModifierShift);
    keys.nativeGlobalFlags = NX_SHIFTMASK | NX_DEVICELSHIFTKEYMASK;
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(native).invoke([&] {
        OSXScreenInputStateTestAccess::local(native, false);
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(native,
            kCGEventFlagsChanged, kVK_Shift, 0, 0));
    }));
    native.disable();
    EXPECT_EQ(std::vector<UInt32>({0}), keys.nativeModifierFlags);
    EXPECT_EQ(0u, keys.nativeGlobalFlags);
    native.disable();
    EXPECT_EQ(std::vector<UInt32>({0}), keys.nativeModifierFlags);
    // A later enable must use the new native snapshot, not the retired tap's.
    keys.physicalMask = KeyModifierControl;
    native.updateKeyState();
    EXPECT_EQ(KeyModifierControl, native.getActiveModifiers());
}

TEST_F(OSXScreenInputStateTests, returnModifierCompletionPreservesObservedRightControlFlags)
{
    StateEventQueue events;
    OSXScreen native(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(native, true, true,
        KeyModifierShift | KeyModifierControl);
    keys.nativeGlobalFlags = NX_SHIFTMASK | NX_DEVICELSHIFTKEYMASK |
        NX_CONTROLMASK | NX_DEVICERCTLKEYMASK;
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(native).invoke([&] {
        OSXScreenInputStateTestAccess::local(native, false);
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(native,
            kCGEventFlagsChanged, kVK_Shift, 0, NX_CONTROLMASK | NX_DEVICERCTLKEYMASK));
    }));
    native.enter();
    EXPECT_EQ(std::vector<UInt32>({NX_CONTROLMASK | NX_DEVICERCTLKEYMASK}), keys.nativeModifierFlags);
}

TEST_F(OSXScreenInputStateTests, locallyDeliveredFlagsCompleteAPendingRemoteReleaseOnce)
{
    StateEventQueue events;
    OSXScreen native(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(native, true, true, KeyModifierShift);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(native).invoke([&] {
        OSXScreenInputStateTestAccess::local(native, false);
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(native,
            kCGEventFlagsChanged, kVK_Shift, 0, 0));
        OSXScreenInputStateTestAccess::local(native, true);
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(native,
            kCGEventFlagsChanged, kVK_Shift, 0, 0));
    }));
    native.enter();
    native.updateKeyState();
    EXPECT_EQ(0, native.getActiveModifiers());
    EXPECT_TRUE(keys.nativeModifierFlags.empty());
}

TEST_F(OSXScreenInputStateTests, completedReturnModifierCacheTracksLaterPhysicalRelease)
{
    StateEventQueue events;
    OSXScreen native(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(native, true, true,
        KeyModifierShift | KeyModifierControl);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(native).invoke([&] {
        OSXScreenInputStateTestAccess::local(native, false);
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(native,
            kCGEventFlagsChanged, kVK_Shift, 0, NX_CONTROLMASK | NX_DEVICERCTLKEYMASK));
    }));
    native.enter();
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(native).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(native,
            kCGEventFlagsChanged, kVK_RightControl, 0, 0));
    }));
    native.fakeKeyDown(kKeyShift_L, 0, 8);
    ASSERT_FALSE(keys.nativeModifierFlags.empty());
    EXPECT_EQ(UInt32(NX_SHIFTMASK | NX_DEVICELSHIFTKEYMASK), keys.nativeModifierFlags.back());
    EXPECT_TRUE(native.fakeKeyUp(8));
}

TEST_F(OSXScreenInputStateTests, primaryRefreshDefersOwnedReplayButStillRefreshesButtons)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeKeyDown(kKeyTab, 0, 7);
    const unsigned before = keys.polls.load();
    keys.physicalMask = KeyModifierControl;
    for (bool down : {true, false, true}) {
        nativeButtons = down ? 1 : 0;
        screen.updateKeyState();
        EXPECT_EQ(before, keys.polls.load());
        EXPECT_EQ(0, screen.getActiveModifiers());
        ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
            EXPECT_EQ(down, OSXScreenInputStateTestAccess::cachedLeftButton(screen));
        }));
    }
    EXPECT_TRUE(screen.fakeKeyUp(7));
    EXPECT_EQ(std::vector<bool>({true, false}), keys.synthesized);
    screen.updateKeyState();
    EXPECT_EQ(before + 1, keys.polls.load());
    // The native poll resumes, but cannot replace the owner's modifier stream.
    EXPECT_EQ(0, screen.getActiveModifiers());
}

TEST_F(OSXScreenInputStateTests, primaryRefreshKeepsExplicitShiftTabReleasesUntilAllKeysUp)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeKeyDown(kKeyShift_L, 0, 8);
    screen.fakeKeyDown(kKeyTab, KeyModifierShift, 7);
    const unsigned before = keys.polls.load();
    screen.updateKeyState();
    screen.updateKeyState();
    EXPECT_EQ(before, keys.polls.load());
    EXPECT_EQ(KeyModifierShift, screen.getActiveModifiers());
    EXPECT_TRUE(screen.fakeKeyUp(7));
    screen.fakeAllKeysUp();
    EXPECT_EQ(std::vector<bool>({true, true, false, false}), keys.synthesized);
    EXPECT_EQ((std::vector<KeyButton>{kVK_Shift + 1, kVK_Tab + 1, kVK_Tab + 1, kVK_Shift + 1}),
              keys.synthesizedButtons);
    EXPECT_FALSE(screen.isKeyDown(kVK_Tab + 1));
    EXPECT_FALSE(screen.isKeyDown(kVK_Shift + 1));
    screen.updateKeyState();
    EXPECT_EQ(before + 1, keys.polls.load());
}

TEST_F(OSXScreenInputStateTests, secondaryRefreshStillReseedsWithSyntheticBinding)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, false, false);
    screen.fakeKeyDown(kKeyTab, 0, 7);
    const unsigned before = keys.polls.load();
    keys.physicalMask = KeyModifierControl;
    screen.updateKeyState();
    EXPECT_EQ(before + 1, keys.polls.load());
    EXPECT_EQ(KeyModifierControl, screen.getActiveModifiers());
    EXPECT_FALSE(keys.hasSyntheticKeyBindings());
    EXPECT_FALSE(screen.isKeyDown(kVK_Tab + 1));
}

TEST_F(OSXScreenInputStateTests, primaryPhysicalOnlyPressDoesNotDeferRefresh)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyDown, kVK_Tab, 0));
    }));
    const unsigned before = keys.polls.load();
    screen.updateKeyState();
    EXPECT_EQ(before + 1, keys.polls.load());
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
    }));
    ASSERT_EQ(2u, events.keys.size());
    EXPECT_TRUE(events.keys[0].down);
    EXPECT_FALSE(events.keys[1].down);
}

TEST_F(OSXScreenInputStateTests, physicalKeyUpBeforeReplayCleanupDoesNotCorruptNextPress)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyDown, kVK_Tab, 0));
        screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 1);
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
    }));
    EXPECT_TRUE(screen.fakeKeyUpForHandoff(7, 1));
    EXPECT_FALSE(screen.isKeyDown(kVK_Tab + 1));
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 9, 2);
    EXPECT_TRUE(screen.isKeyDown(kVK_Tab + 1));
    EXPECT_TRUE(screen.fakeKeyUpForHandoff(9, 2));
    EXPECT_FALSE(screen.isKeyDown(kVK_Tab + 1));
    screen.fakeAllKeysUp();
    EXPECT_EQ(std::vector<bool>({true, true, false}), keys.synthesized);
}

TEST_F(OSXScreenInputStateTests, locallyDeliveredButtonUpCompletesReplayWithoutDuplicateNativeUp)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeMouseButtonForHandoff(kButtonLeft, true, 1);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, false));
    }));
    ASSERT_EQ(1u, events.buttons.size());
    screen.fakeMouseButtonForHandoff(kButtonLeft, false, 1);
    EXPECT_EQ((std::vector<CGEventType>{kCGEventLeftMouseDown, kCGEventLeftMouseUp}), deliveredButtons);
    EXPECT_EQ(0u, postedMouseUps.load());
}

TEST_F(OSXScreenInputStateTests, locallyDeliveredKeyUpRetiresBindingWithoutDuplicateNativeUp)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 1);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
        EXPECT_FALSE(keys.hasSyntheticKeyBindings());
    }));
    ASSERT_EQ(1u, events.keys.size());
    EXPECT_TRUE(screen.fakeKeyUpForHandoff(7, 1));
    EXPECT_EQ((std::vector<std::pair<KeyButton, bool>>{{kVK_Tab + 1, true}, {kVK_Tab + 1, false}}), deliveredKeys);
    EXPECT_EQ(std::vector<bool>({true}), keys.synthesized);
    const auto polls = keys.polls.load();
    screen.updateKeyState();
    EXPECT_EQ(polls + 1, keys.polls.load());
}

TEST_F(OSXScreenInputStateTests, locallyDeliveredRightButtonUpRetiresCachedDragState)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeMouseButtonForHandoff(kButtonRight, true, 1);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        ASSERT_TRUE(OSXScreenInputStateTestAccess::cachedRightButton(screen));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, false, kCGMouseButtonRight));
        EXPECT_FALSE(OSXScreenInputStateTestAccess::cachedRightButton(screen));
    }));
    screen.fakeMouseButtonForHandoff(kButtonRight, false, 1);
    EXPECT_EQ((std::vector<CGEventType>{kCGEventRightMouseDown, kCGEventRightMouseUp}), deliveredButtons);
}

TEST_F(OSXScreenInputStateTests, completedOldButtonCleanupCannotReleaseNewPhysicalPressOrDrag)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeMouseButtonForHandoff(kButtonLeft, true, 1);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, false));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, true));
        screen.fakeMouseMove(100, 100);
    }));
    screen.fakeMouseButtonForHandoff(kButtonLeft, false, 1); // delayed cleanup of the older replay
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::cachedLeftButton(screen));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::dragging(screen));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, false));
    }));
    EXPECT_EQ((std::vector<CGEventType>{kCGEventLeftMouseDown, kCGEventLeftMouseUp,
                                     kCGEventLeftMouseDown, kCGEventLeftMouseUp}), deliveredButtons);
    EXPECT_EQ(0u, postedMouseUps.load());
}

TEST_F(OSXScreenInputStateTests, completedOldKeyCleanupCannotReleaseNewPhysicalPress)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 1);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyDown, kVK_Tab, 0));
    }));
    EXPECT_TRUE(screen.fakeKeyUpForHandoff(7, 1));
    EXPECT_TRUE(screen.isKeyDown(kVK_Tab + 1));
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::observedDown(screen, kVK_Tab + 1));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
    }));
    EXPECT_EQ((std::vector<std::pair<KeyButton, bool>>{{kVK_Tab + 1, true}, {kVK_Tab + 1, false},
                                                    {kVK_Tab + 1, true}, {kVK_Tab + 1, false}}), deliveredKeys);
}

TEST_F(OSXScreenInputStateTests, completedOldReleasesCannotConsumeNewReplayGeneration)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 1);
    screen.fakeMouseButtonForHandoff(kButtonLeft, true, 1);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, false));
    }));
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 2);
    screen.fakeMouseButtonForHandoff(kButtonLeft, true, 2);
    EXPECT_FALSE(screen.fakeKeyUpForHandoff(7, 1)); // stale completion cannot release generation 2
    screen.fakeMouseButtonForHandoff(kButtonLeft, false, 1);
    EXPECT_TRUE(screen.isKeyDown(kVK_Tab + 1));
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::cachedLeftButton(screen));
    }));
    EXPECT_TRUE(screen.fakeKeyUpForHandoff(7, 2));
    screen.fakeMouseButtonForHandoff(kButtonLeft, false, 2);
    EXPECT_EQ((std::vector<std::pair<KeyButton, bool>>{{kVK_Tab + 1, true}, {kVK_Tab + 1, false},
                                                    {kVK_Tab + 1, true}, {kVK_Tab + 1, false}}), deliveredKeys);
    EXPECT_EQ((std::vector<CGEventType>{kCGEventLeftMouseDown, kCGEventLeftMouseUp,
                                     kCGEventLeftMouseDown, kCGEventLeftMouseUp}), deliveredButtons);
    EXPECT_EQ(1u, postedMouseUps.load());
}

TEST_F(OSXScreenInputStateTests, remotelySuppressedUpsStillRequireNativeReplayCleanup)
{
    for (bool replayBeforeUp : {false, true}) {
        SCOPED_TRACE(replayBeforeUp);
        deliveredButtons.clear(); deliveredKeys.clear(); postedMouseUps = 0;
        StateEventQueue events; OSXScreen screen(&events, false, false);
        OSXScreenInputStateTestAccess::prepare(screen, true, true);
        if (replayBeforeUp) {
            screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 1); screen.fakeMouseButtonForHandoff(kButtonLeft, true, 1);
        }
        ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
            OSXScreenInputStateTestAccess::local(screen, false);
            EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
            EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackButton(screen, false));
            OSXScreenInputStateTestAccess::local(screen, true);
        }));
        if (!replayBeforeUp) {
            screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 1); screen.fakeMouseButtonForHandoff(kButtonLeft, true, 1);
        }
        EXPECT_TRUE(screen.fakeKeyUpForHandoff(7, 1)); screen.fakeMouseButtonForHandoff(kButtonLeft, false, 1);
        EXPECT_EQ((std::vector<std::pair<KeyButton, bool>>{{kVK_Tab + 1, true}, {kVK_Tab + 1, false}}), deliveredKeys);
        EXPECT_EQ((std::vector<CGEventType>{kCGEventLeftMouseDown, kCGEventLeftMouseUp}), deliveredButtons);
        EXPECT_EQ(1u, postedMouseUps.load());
    }
}

TEST_F(OSXScreenInputStateTests, lifecycleCleanupReleasesNewReplayDespiteOlderLocalCompletion)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 1); screen.fakeMouseButtonForHandoff(kButtonLeft, true, 1);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, false));
    }));
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 2); screen.fakeMouseButtonForHandoff(kButtonLeft, true, 2);
    // One lease-wide cleanup, as on disconnect: no later fakeAllKeysUp or
    // second fakeMouseButton is available to rescue a live newer generation.
    screen.fakeKeyUpForHandoff(7, 2); screen.fakeMouseButtonForHandoff(kButtonLeft, false, 2);
    EXPECT_FALSE(keys.hasSyntheticKeyBindings());
    EXPECT_FALSE(screen.isKeyDown(kVK_Tab + 1));
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_FALSE(OSXScreenInputStateTestAccess::cachedLeftButton(screen));
    }));
    EXPECT_EQ((std::vector<std::pair<KeyButton, bool>>{{kVK_Tab + 1, true}, {kVK_Tab + 1, false},
                                                    {kVK_Tab + 1, true}, {kVK_Tab + 1, false}}), deliveredKeys);
    EXPECT_EQ((std::vector<CGEventType>{kCGEventLeftMouseDown, kCGEventLeftMouseUp,
                                     kCGEventLeftMouseDown, kCGEventLeftMouseUp}), deliveredButtons);
}

TEST_F(OSXScreenInputStateTests, locallyDeliveredModifierUpCompletesExplicitReplay)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::local(screen, false);
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventFlagsChanged, kVK_Shift, 0, kCGEventFlagMaskShift));
        OSXScreenInputStateTestAccess::local(screen, true);
    }));
    screen.fakeKeyDownForHandoff(kKeyShift_L, KeyModifierShift, 8, 1);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventFlagsChanged, kVK_Shift, 0));
        EXPECT_FALSE(keys.hasSyntheticKeyBindings());
    }));
    ASSERT_EQ(2u, events.keys.size());
    EXPECT_TRUE(screen.fakeKeyUpForHandoff(8, 1));
    EXPECT_EQ((std::vector<std::pair<KeyButton, bool>>{{kVK_Shift + 1, true}, {kVK_Shift + 1, false}}), deliveredKeys);
    EXPECT_FALSE(screen.isKeyDown(kVK_Shift + 1));
    EXPECT_EQ(0, screen.getActiveModifiers());
}

TEST_F(OSXScreenInputStateTests, ordinaryFlagsRecoveryDoesNotProveLocalModifierDelivery)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeKeyDownForHandoff(kKeyShift_L, KeyModifierShift, 8, 1);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyDown, kVK_Tab, 0));
        EXPECT_TRUE(keys.hasSyntheticKeyBindings());
    }));
    EXPECT_TRUE(screen.fakeKeyUpForHandoff(8, 1));
    EXPECT_EQ((std::vector<bool>{true, false}), keys.synthesized);
}

TEST_F(OSXScreenInputStateTests, nativeModifierCacheClearsWithoutPostingADuplicateRelease)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeKeyDownForHandoff(kKeyShift_L, KeyModifierShift, 8, 1);
    ASSERT_NE(0u, keys.getModifierStateAsOSXFlags() & kCGEventFlagMaskShift);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventFlagsChanged, kVK_Shift, 0));
    }));
    EXPECT_TRUE(screen.fakeKeyUpForHandoff(8, 1));
    EXPECT_EQ((std::vector<bool>{true}), keys.synthesized);
    EXPECT_EQ(0u, keys.getModifierStateAsOSXFlags() & kCGEventFlagMaskShift);
    screen.fakeMouseButton(kButtonLeft, true);
    EXPECT_EQ(0u, postedMouseFlags.load() & kCGEventFlagMaskShift);
    screen.fakeKeyDownForHandoff(kKeyControl_L, KeyModifierControl, 9, 2);
    ASSERT_EQ(2u, keys.nativeModifierFlags.size());
    EXPECT_EQ(0u, keys.nativeModifierFlags.back() & NX_SHIFTMASK);
    EXPECT_NE(0u, keys.nativeModifierFlags.back() & NX_CONTROLMASK);
    screen.fakeKeyUpForHandoff(9, 2);
    screen.fakeMouseButton(kButtonLeft, false);
    // Also reset the old process-static cache when running this RED test.
    screen.fakeKeyDownForHandoff(kKeyShift_L, KeyModifierShift, 8, 3);
    screen.fakeKeyUpForHandoff(8, 3);
}

TEST_F(OSXScreenInputStateTests, ordinaryLocalClickCannotSuppressLaterFirstLeaveCleanup)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, true));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, false));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, true));
        OSXScreenInputStateTestAccess::local(screen, false);
    }));
    screen.fakeMouseButton(kButtonLeft, false); // no replay Down; first-leave cleanup is mandatory
    EXPECT_EQ((std::vector<CGEventType>{kCGEventLeftMouseDown, kCGEventLeftMouseUp,
                                     kCGEventLeftMouseDown, kCGEventLeftMouseUp}), deliveredButtons);
    EXPECT_EQ(1u, postedMouseUps.load());
}

TEST_F(OSXScreenInputStateTests, localUpBeforeHeldReplayStillReleasesLateReconstruction)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::local(screen, false);
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyDown, kVK_Tab, 0));
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackButton(screen, true));
        OSXScreenInputStateTestAccess::local(screen, true);
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, false));
    }));
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 1);
    screen.fakeMouseButtonForHandoff(kButtonLeft, true, 1);
    EXPECT_TRUE(screen.fakeKeyUpForHandoff(7, 1));
    screen.fakeMouseButtonForHandoff(kButtonLeft, false, 1);
    EXPECT_EQ((std::vector<std::pair<KeyButton, bool>>{{kVK_Tab + 1, false},
                                                    {kVK_Tab + 1, true}, {kVK_Tab + 1, false}}), deliveredKeys);
    EXPECT_EQ((std::vector<CGEventType>{kCGEventLeftMouseUp, kCGEventLeftMouseDown,
                                     kCGEventLeftMouseUp}), deliveredButtons);
}

TEST_F(OSXScreenInputStateTests, allUpThenNewPressRejectsLateOldCompletionWithoutTokens)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 1);
    screen.fakeMouseButtonForHandoff(kButtonLeft, true, 1);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, false));
    }));
    screen.fakeAllKeysUp();
    screen.fakeMouseButtonForHandoff(kButtonLeft, false, 1);
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 3);
    screen.fakeMouseButtonForHandoff(kButtonLeft, true, 3);
    EXPECT_FALSE(screen.fakeKeyUpForHandoff(7, 1));
    screen.fakeMouseButtonForHandoff(kButtonLeft, false, 1);
    EXPECT_TRUE(screen.isKeyDown(kVK_Tab + 1));
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::cachedLeftButton(screen));
        OSXScreenInputStateTestAccess::local(screen, false);
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackButton(screen, false));
    }));
    EXPECT_TRUE(screen.fakeKeyUpForHandoff(7, 3));
    screen.fakeMouseButtonForHandoff(kButtonLeft, false, 3);
    EXPECT_EQ((std::vector<std::pair<KeyButton, bool>>{{kVK_Tab + 1, true}, {kVK_Tab + 1, false},
                                                    {kVK_Tab + 1, true}, {kVK_Tab + 1, false}}), deliveredKeys);
    EXPECT_EQ((std::vector<CGEventType>{kCGEventLeftMouseDown, kCGEventLeftMouseUp,
                                     kCGEventLeftMouseDown, kCGEventLeftMouseUp}), deliveredButtons);
}

TEST_F(OSXScreenInputStateTests, samePressReconstructionAfterNativeCompletionStillOwesRelease)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 1);
    screen.fakeMouseButtonForHandoff(kButtonLeft, true, 1);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, false));
    }));
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 1);
    screen.fakeMouseButtonForHandoff(kButtonLeft, true, 1);
    EXPECT_TRUE(screen.fakeKeyUpForHandoff(7, 1));
    screen.fakeMouseButtonForHandoff(kButtonLeft, false, 1);
    EXPECT_EQ((std::vector<std::pair<KeyButton, bool>>{{kVK_Tab + 1, true}, {kVK_Tab + 1, false},
                                                    {kVK_Tab + 1, true}, {kVK_Tab + 1, false}}), deliveredKeys);
    EXPECT_EQ((std::vector<CGEventType>{kCGEventLeftMouseDown, kCGEventLeftMouseUp,
                                     kCGEventLeftMouseDown, kCGEventLeftMouseUp}), deliveredButtons);
}

TEST_F(OSXScreenInputStateTests, allUpReleasesOwnedReplayAfterSuppressedPhysicalUpClearsShadowCount)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 1);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::local(screen, false);
        EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
        EXPECT_TRUE(keys.hasSyntheticKeyBindings());
    }));
    screen.fakeAllKeysUp();
    EXPECT_EQ((std::vector<std::pair<KeyButton, bool>>{{kVK_Tab + 1, true}, {kVK_Tab + 1, false}}), deliveredKeys);
    EXPECT_FALSE(keys.hasSyntheticKeyBindings());
}

TEST_F(OSXScreenInputStateTests, nativeUpDuringOldNewReplayBatchDoesNotReplaceBusinessFifo)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::local(screen, false);
        // The business buffer already contains D_A, U_A, D_B.
        for (bool down : {true, false, true}) {
            EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackKey(screen,
                down ? kCGEventKeyDown : kCGEventKeyUp, kVK_Tab, 0));
            EXPECT_FALSE(OSXScreenInputStateTestAccess::callbackButton(screen, down));
        }
        OSXScreenInputStateTestAccess::local(screen, true);
    }));
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 1);
    screen.fakeMouseButtonForHandoff(kButtonLeft, true, 1);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        // A real callback can run between per-event replay commands. This is
        // physical U_B, even though the native obligation it completes is A.
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, false));
    }));
    screen.fakeKeyUpForHandoff(7, 1); screen.fakeMouseButtonForHandoff(kButtonLeft, false, 1);
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 2);
    screen.fakeMouseButtonForHandoff(kButtonLeft, true, 2);
    // FIFO business processing of U_B releases the now-current ledger B.
    EXPECT_TRUE(screen.fakeKeyUpForHandoff(7, 2));
    screen.fakeMouseButtonForHandoff(kButtonLeft, false, 2);
    EXPECT_EQ((std::vector<bool>{true, true, false}), keys.synthesized);
    EXPECT_EQ((std::vector<std::pair<KeyButton, bool>>{{kVK_Tab + 1, true}, {kVK_Tab + 1, false},
                                                    {kVK_Tab + 1, true}, {kVK_Tab + 1, false}}), deliveredKeys);
    EXPECT_EQ((std::vector<CGEventType>{kCGEventLeftMouseDown, kCGEventLeftMouseUp,
                                     kCGEventLeftMouseDown, kCGEventLeftMouseUp}), deliveredButtons);
    EXPECT_FALSE(keys.hasSyntheticKeyBindings());
}

TEST_F(OSXScreenInputStateTests, timedOutOldCleanupCannotReleaseNewOwnerGeneration)
{
    StateEventQueue events; OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 1);
    screen.fakeMouseButtonForHandoff(kButtonLeft, true, 1);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
        EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackButton(screen, false));
    }));
    const auto gate = std::make_shared<std::promise<void>>();
    const auto entered = std::make_shared<std::promise<void>>();
    const auto ready = gate->get_future().share();
    const auto pending = OSXScreenInputStateTestAccess::loop(screen).invokeRelease([&, ready, entered] {
        entered->set_value();
        ready.wait();
        screen.fakeKeyDownForHandoff(kKeyTab, 0, 7, 2);
        screen.fakeMouseButtonForHandoff(kButtonLeft, true, 2);
    }, std::chrono::milliseconds(5));
    ASSERT_EQ(OSXInputLoop::ReleaseResult::Pending, pending);
    entered->get_future().wait();
    EXPECT_TRUE(screen.fakeKeyUpForHandoff(7, 1)); // accepted, one-second deadline
    screen.fakeMouseButtonForHandoff(kButtonLeft, false, 1);
    gate->set_value();
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([] {}));
    EXPECT_TRUE(screen.isKeyDown(kVK_Tab + 1));
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::cachedLeftButton(screen));
    }));
    EXPECT_TRUE(screen.fakeKeyUpForHandoff(7, 2));
    screen.fakeMouseButtonForHandoff(kButtonLeft, false, 2);
    EXPECT_EQ((std::vector<std::pair<KeyButton, bool>>{{kVK_Tab + 1, true}, {kVK_Tab + 1, false},
                                                    {kVK_Tab + 1, true}, {kVK_Tab + 1, false}}), deliveredKeys);
    EXPECT_EQ((std::vector<CGEventType>{kCGEventLeftMouseDown, kCGEventLeftMouseUp,
                                     kCGEventLeftMouseDown, kCGEventLeftMouseUp}), deliveredButtons);
}

TEST_F(OSXScreenInputStateTests, realPrimaryScreenLeaveKeepsReplayedKeyReleaseBinding)
{
    class NativeBoundaryScreen : public OSXScreen {
    public:
        explicit NativeBoundaryScreen(IEventQueue* events) : OSXScreen(events, false, false) {}
        bool isDraggingStarted() override { return false; }
        void checkClipboards() override {} // No test-host pasteboard access.
    };
    StateEventQueue events;
    auto* native = new NativeBoundaryScreen(&events);
    auto& keys = OSXScreenInputStateTestAccess::prepare(*native, true, true);
    barrier::Screen screen(native, &events);
    native->fakeKeyDown(kKeyTab, 0, 7);
    const unsigned before = keys.polls.load();
    const bool left = screen.leave(); // real leavePrimary invokes updateKeyState
    EXPECT_TRUE(left);
    EXPECT_EQ(before, keys.polls.load());
    EXPECT_TRUE(native->fakeKeyUp(7));
    EXPECT_EQ(std::vector<bool>({true, false}), keys.synthesized);
    if (left) EXPECT_TRUE(screen.enter(0)); // restore normal wrapper ownership
}

TEST_F(OSXScreenInputStateTests, sameProcessKeyboardReplayNeverStartsAPhysicalPair)
{
    for (bool local : {true, false}) {
        SCOPED_TRACE(local);
        StateEventQueue events;
        OSXScreen screen(&events, false, false);
        OSXScreenInputStateTestAccess::prepare(screen, true, true);
        ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
            OSXScreenInputStateTestAccess::local(screen, local);
            EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyDown, kVK_Tab, getpid()));
            EXPECT_FALSE(OSXScreenInputStateTestAccess::observedDown(screen, kVK_Tab + 1));
            EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, getpid()));
            EXPECT_EQ(local, OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, 0));
        }));
        EXPECT_TRUE(events.keys.empty());
        EXPECT_TRUE(events.hotkeys.empty());
    }
}

TEST_F(OSXScreenInputStateTests, sameProcessReplayCannotReleaseObservedModifierOrHotkey)
{
    for (bool local : {true, false}) {
        SCOPED_TRACE(local);
        StateEventQueue events;
        OSXScreen screen(&events, false, false);
        OSXScreenInputStateTestAccess::prepare(screen, true, true);
        const UInt32 hotkey = screen.registerHotKey(kKeyNone, KeyModifierShift);
        ASSERT_NE(0u, hotkey);
        ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
            OSXScreenInputStateTestAccess::local(screen, local);
            EXPECT_EQ(local, OSXScreenInputStateTestAccess::callbackKey(
                screen, kCGEventFlagsChanged, kVK_Shift, 0, kCGEventFlagMaskShift));
            EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventFlagsChanged, kVK_Shift, getpid()));
            EXPECT_EQ(KeyModifierShift, OSXScreenInputStateTestAccess::observedModifiers(screen));
            EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyDown, kVK_Tab, getpid()));
            EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, getpid()));
            EXPECT_EQ(KeyModifierShift, OSXScreenInputStateTestAccess::observedModifiers(screen));
            EXPECT_EQ(1u, events.keys.size());
            EXPECT_EQ(std::vector<UInt32>({hotkey}), events.hotkeys);
            EXPECT_EQ(local, OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventFlagsChanged, kVK_Shift, 0));
            EXPECT_EQ(0, OSXScreenInputStateTestAccess::observedModifiers(screen));
        }));
        ASSERT_EQ(2u, events.keys.size());
        EXPECT_TRUE(events.keys[0].down);
        EXPECT_FALSE(events.keys[1].down);
        EXPECT_EQ(std::vector<UInt32>({hotkey, hotkey}), events.hotkeys);
    }
}

TEST_F(OSXScreenInputStateTests, sameProcessKeyboardReplayCannotTriggerRegisteredHotkey)
{
    for (bool local : {true, false}) {
        StateEventQueue events;
        OSXScreen screen(&events, false, false);
        OSXScreenInputStateTestAccess::prepare(screen, true, true);
        ASSERT_NE(0u, screen.registerHotKey(kKeyTab, 0));
        ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
            OSXScreenInputStateTestAccess::local(screen, local);
            EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyDown, kVK_Tab, getpid()));
            EXPECT_TRUE(OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventKeyUp, kVK_Tab, getpid()));
        }));
        EXPECT_TRUE(events.keys.empty());
        EXPECT_TRUE(events.hotkeys.empty());
    }
}

TEST_F(OSXScreenInputStateTests, physicalAndOtherProducerKeysKeepLocalDeliveryAndRemoteSuppression)
{
    for (pid_t source : {pid_t(0), pid_t(getpid() + 1)}) {
        for (bool local : {true, false}) {
            SCOPED_TRACE(source);
            SCOPED_TRACE(local);
            StateEventQueue events;
            OSXScreen screen(&events, false, false);
            OSXScreenInputStateTestAccess::prepare(screen, true, true);
            ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
                OSXScreenInputStateTestAccess::local(screen, local);
                EXPECT_EQ(local, OSXScreenInputStateTestAccess::callbackKey(
                    screen, kCGEventFlagsChanged, kVK_Shift, source, kCGEventFlagMaskShift));
                EXPECT_EQ(local, OSXScreenInputStateTestAccess::callbackKey(
                    screen, kCGEventKeyDown, kVK_Tab, source, kCGEventFlagMaskShift));
                EXPECT_TRUE(OSXScreenInputStateTestAccess::observedDown(screen, kVK_Tab + 1));
                EXPECT_EQ(local, OSXScreenInputStateTestAccess::callbackKey(
                    screen, kCGEventKeyUp, kVK_Tab, source, kCGEventFlagMaskShift));
                EXPECT_EQ(local, OSXScreenInputStateTestAccess::callbackKey(screen, kCGEventFlagsChanged, kVK_Shift, source));
                EXPECT_FALSE(OSXScreenInputStateTestAccess::observedDown(screen, kVK_Tab + 1));
                EXPECT_EQ(0, OSXScreenInputStateTestAccess::observedModifiers(screen));
            }));
            ASSERT_EQ(4u, events.keys.size());
            EXPECT_EQ(kKeyShift_L, events.keys[0].id); EXPECT_TRUE(events.keys[0].down);
            EXPECT_EQ(kKeyTab, events.keys[1].id); EXPECT_TRUE(events.keys[1].down);
            EXPECT_EQ(kVK_Tab + 1, events.keys[2].button); EXPECT_FALSE(events.keys[2].down);
            EXPECT_EQ(KeyModifierShift, events.keys[2].mask);
            EXPECT_EQ(kKeyShift_L, events.keys[3].id); EXPECT_FALSE(events.keys[3].down);
        }
    }
}

TEST_F(OSXScreenInputStateTests, physicalRefreshCannotSplitModifierAndKeyReleaseFromTheirPresses)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    std::promise<void> pressed, release;
    auto gate = release.get_future().share();
    auto callback = std::async(std::launch::async, [&] {
        return OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
            OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Shift, kCGEventFlagMaskShift);
            OSXScreenInputStateTestAccess::key(screen, kCGEventKeyDown, kVK_Tab, kCGEventFlagMaskShift);
            pressed.set_value();
            gate.wait();
            OSXScreenInputStateTestAccess::key(screen, kCGEventKeyUp, kVK_Tab, kCGEventFlagMaskShift);
            OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Shift);
        });
    });
    pressed.get_future().wait();
    auto refresh = std::async(std::launch::async, [&] { screen.updateKeyState(); });
    const auto pending = refresh.wait_for(std::chrono::milliseconds(100));
    release.set_value();
    EXPECT_TRUE(callback.get());
    refresh.get();
    EXPECT_EQ(std::future_status::timeout, pending);
    ASSERT_EQ(4u, events.keys.size());
    EXPECT_TRUE(events.keys[0].down);
    EXPECT_EQ(kVK_Shift + 1, events.keys[0].button);
    EXPECT_TRUE(events.keys[1].down);
    EXPECT_FALSE(events.keys[2].down);
    EXPECT_EQ(kVK_Tab + 1, events.keys[2].button);
    EXPECT_EQ(KeyModifierShift, events.keys[2].mask);
    EXPECT_FALSE(events.keys[3].down);
    EXPECT_EQ(kVK_Shift + 1, events.keys[3].button);
    EXPECT_EQ(0, screen.getActiveModifiers());
    EXPECT_EQ(1u, keys.polls.load());
}

TEST_F(OSXScreenInputStateTests, modifierSnapshotDoesNotWaitForBusyInputOwner)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true, KeyModifierControl);
    OwnerBlock blocked(screen);
    auto result = std::async(std::launch::async, [&] { return screen.pollActiveModifiers(); });
    const auto ready = result.wait_for(std::chrono::milliseconds(30));
    blocked.release();
    EXPECT_EQ(std::future_status::ready, ready);
    EXPECT_EQ(KeyModifierControl, result.get());
}

TEST_F(OSXScreenInputStateTests, allKeyStateEntryPointsWaitForThePrimaryOwner)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    const std::vector<std::pair<const char*, std::function<void()>>> operations = {
        {"updateKeyMap", [&] { screen.updateKeyMap(); }},
        {"updateKeyState", [&] { screen.updateKeyState(); }},
        {"setHalfDuplexMask", [&] { screen.setHalfDuplexMask(KeyModifierCapsLock); }},
        {"fakeKeyDown", [&] { screen.fakeKeyDown(kKeyTab, 0, 7); }},
        {"fakeKeyRepeat", [&] { EXPECT_TRUE(screen.fakeKeyRepeat(kKeyTab, 0, 1, 7)); }},
        {"fakeKeyUp", [&] { EXPECT_TRUE(screen.fakeKeyUp(7)); }},
        {"fakeAllKeysUp", [&] { screen.fakeAllKeysUp(); }},
        {"fakeCtrlAltDel", [&] { EXPECT_FALSE(screen.fakeCtrlAltDel()); }},
        {"isKeyDown", [&] { EXPECT_FALSE(screen.isKeyDown(kVK_Tab + 1)); }},
        {"getActiveModifiers", [&] { EXPECT_EQ(0, screen.getActiveModifiers()); }},
        {"pollActiveGroup", [&] { EXPECT_EQ(0, screen.pollActiveGroup()); }},
        {"pollPressedKeys", [&] { IKeyState::KeyButtonSet pressed; screen.pollPressedKeys(pressed); EXPECT_TRUE(pressed.empty()); }}
    };
    for (const auto& operation : operations) {
        SCOPED_TRACE(operation.first);
        OwnerBlock blocked(screen);
        auto result = std::async(std::launch::async, operation.second);
        const auto pending = result.wait_for(std::chrono::milliseconds(50));
        blocked.release();
        result.get();
        EXPECT_EQ(std::future_status::timeout, pending);
    }
    ASSERT_GE(keys.synthesized.size(), 2u);
    EXPECT_TRUE(keys.synthesized.front());
    EXPECT_FALSE(keys.synthesized.back());
}

TEST_F(OSXScreenInputStateTests, newerPhysicalSnapshotCannotConsumeQueuedKeyAndModifierReleases)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Shift, kCGEventFlagMaskShift);
        OSXScreenInputStateTestAccess::key(screen, kCGEventKeyDown, kVK_Tab, kCGEventFlagMaskShift);
    }));
    // The physical device is already up, but its Quartz releases have not yet
    // reached the tap. Refresh must not erase the obligation to forward them.
    screen.updateKeyState();
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::key(screen, kCGEventKeyUp, kVK_Tab, kCGEventFlagMaskShift);
        OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Shift);
        // A paired release consumes its observed-down entry exactly once.
        OSXScreenInputStateTestAccess::key(screen, kCGEventKeyUp, kVK_Tab);
    }));
    ASSERT_EQ(4u, events.keys.size());
    EXPECT_EQ(kVK_Tab + 1, events.keys[2].button);
    EXPECT_FALSE(events.keys[2].down);
    EXPECT_EQ(KeyModifierShift, events.keys[2].mask);
    EXPECT_EQ(kVK_Shift + 1, events.keys[3].button);
    EXPECT_FALSE(events.keys[3].down);
    EXPECT_EQ(0, screen.getActiveModifiers());
}

// Both regressions catch using the physically polled mask for an event that
// precedes the queued Shift release in the tap's observed event stream.
TEST_F(OSXScreenInputStateTests, primaryMouseButtonsKeepObservedModifiersUntilQueuedRelease)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Shift, kCGEventFlagMaskShift);
    }));
    screen.updateKeyState(); // physical Shift is up; its tap event is queued
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::mouseButton(screen, true, kCGEventFlagMaskShift);
        OSXScreenInputStateTestAccess::mouseButton(screen, false, kCGEventFlagMaskShift);
        OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Shift);
        OSXScreenInputStateTestAccess::mouseButton(screen, true);
        OSXScreenInputStateTestAccess::mouseButton(screen, false);
    }));
    ASSERT_EQ(4u, events.buttons.size());
    for (size_t i = 0; i < events.buttons.size(); ++i) {
        EXPECT_EQ(i % 2 == 0, events.buttons[i].down);
        EXPECT_EQ(kButtonLeft, events.buttons[i].button);
        EXPECT_EQ(i < 2 ? KeyModifierShift : 0, events.buttons[i].mask);
    }
    ASSERT_EQ(2u, events.keys.size());
    EXPECT_TRUE(events.keys.front().down);
    EXPECT_FALSE(events.keys.back().down);
    EXPECT_EQ(kKeyShift_L, events.keys.back().id);
}

TEST_F(OSXScreenInputStateTests, primaryMediaKeysKeepObservedModifiersUntilQueuedRelease)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Shift, kCGEventFlagMaskShift);
    }));
    screen.updateKeyState();
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::mediaKey(screen, true, kCGEventFlagMaskShift);
        OSXScreenInputStateTestAccess::mediaKey(screen, false, kCGEventFlagMaskShift);
        OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Shift);
        OSXScreenInputStateTestAccess::mediaKey(screen, true);
        OSXScreenInputStateTestAccess::mediaKey(screen, false);
    }));
    ASSERT_EQ(6u, events.keys.size());
    EXPECT_EQ(kKeyShift_L, events.keys[0].id);
    EXPECT_TRUE(events.keys[0].down);
    EXPECT_EQ(kKeyShift_L, events.keys[3].id);
    EXPECT_FALSE(events.keys[3].down);
    for (size_t i : {1u, 2u, 4u, 5u}) {
        EXPECT_EQ(kKeyAudioUp, events.keys[i].id);
        EXPECT_EQ(i == 1 || i == 4, events.keys[i].down);
        EXPECT_EQ(i < 3 ? KeyModifierShift : 0, events.keys[i].mask);
    }
}

TEST_F(OSXScreenInputStateTests, secondaryMouseAndMediaEventsKeepShadowModifiers)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, false, false);
    keys.physicalMask = KeyModifierControl;
    screen.updateKeyState();
    OSXScreenInputStateTestAccess::mouseButton(screen, true);
    OSXScreenInputStateTestAccess::mouseButton(screen, false);
    OSXScreenInputStateTestAccess::mediaKey(screen, true);
    OSXScreenInputStateTestAccess::mediaKey(screen, false);
    ASSERT_EQ(2u, events.buttons.size());
    ASSERT_EQ(2u, events.keys.size());
    for (const auto& button : events.buttons) EXPECT_EQ(KeyModifierControl, button.mask);
    for (const auto& key : events.keys) {
        EXPECT_EQ(kKeyAudioUp, key.id);
        EXPECT_EQ(KeyModifierControl, key.mask);
    }
}

TEST_F(OSXScreenInputStateTests, ordinaryKeyRecoversMissedModifierReleaseAndHotkeyPairExactlyOnce)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    const UInt32 hotkey = screen.registerHotKey(kKeyNone, KeyModifierShift);
    ASSERT_NE(0u, hotkey);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Shift, kCGEventFlagMaskShift);
    }));
    screen.updateKeyState();
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        // The Shift-up callback was lost, but the next key carries no Shift.
        OSXScreenInputStateTestAccess::key(screen, kCGEventKeyDown, kVK_Tab);
        OSXScreenInputStateTestAccess::key(screen, kCGEventKeyUp, kVK_Tab);
        // A late flags callback must not release the modifier/hotkey again.
        OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Shift);
    }));
    ASSERT_EQ(4u, events.keys.size());
    EXPECT_EQ(kKeyShift_L, events.keys[1].id);
    EXPECT_FALSE(events.keys[1].down);
    EXPECT_EQ(0, events.keys[1].mask);
    EXPECT_EQ(kKeyTab, events.keys[2].id);
    EXPECT_TRUE(events.keys[2].down);
    EXPECT_EQ(0, events.keys[2].mask);
    EXPECT_EQ(kVK_Tab + 1, events.keys[3].button);
    EXPECT_FALSE(events.keys[3].down);
    EXPECT_EQ(0, events.keys[3].mask);
    EXPECT_EQ(std::vector<UInt32>({hotkey, hotkey}), events.hotkeys);
    EXPECT_EQ(0, screen.getActiveModifiers());
}

TEST_F(OSXScreenInputStateTests, mouseAndMediaEventsRecoverMissedModifierReleaseFromNativeFlags)
{
    for (bool media : {false, true}) {
        StateEventQueue events;
        OSXScreen screen(&events, false, false);
        OSXScreenInputStateTestAccess::prepare(screen, true, true);
        const UInt32 hotkey = screen.registerHotKey(kKeyNone, KeyModifierShift);
        ASSERT_NE(0u, hotkey);
        ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
            OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Shift, kCGEventFlagMaskShift);
        }));
        screen.updateKeyState();
        ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
            if (media) {
                OSXScreenInputStateTestAccess::mediaKey(screen, true);
                OSXScreenInputStateTestAccess::mediaKey(screen, false);
            }
            else {
                OSXScreenInputStateTestAccess::mouseButton(screen, true);
                OSXScreenInputStateTestAccess::mouseButton(screen, false);
            }
            OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Shift);
        }));
        ASSERT_EQ(media ? 4u : 2u, events.keys.size());
        EXPECT_EQ(kKeyShift_L, events.keys[1].id);
        EXPECT_FALSE(events.keys[1].down);
        EXPECT_EQ(0, events.keys[1].mask);
        if (media) {
            EXPECT_EQ(kKeyAudioUp, events.keys[2].id);
            EXPECT_EQ(0, events.keys[2].mask);
            EXPECT_EQ(0, events.keys[3].mask);
        }
        else {
            ASSERT_EQ(2u, events.buttons.size());
            EXPECT_EQ(0, events.buttons[0].mask);
            EXPECT_EQ(0, events.buttons[1].mask);
        }
        EXPECT_EQ(std::vector<UInt32>({hotkey, hotkey}), events.hotkeys);
    }
}

TEST_F(OSXScreenInputStateTests, numericKeypadOriginDoesNotSynthesizeNumLockTransitions)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::key(screen, kCGEventKeyDown, kVK_Tab, kCGEventFlagMaskNumericPad);
        OSXScreenInputStateTestAccess::key(screen, kCGEventKeyUp, kVK_Tab, kCGEventFlagMaskNumericPad);
    }));
    ASSERT_EQ(2u, events.keys.size());
    EXPECT_TRUE(events.keys[0].down);
    EXPECT_FALSE(events.keys[1].down);
    EXPECT_EQ(0, events.keys[0].mask);
    EXPECT_EQ(0, events.keys[1].mask);
}

TEST_F(OSXScreenInputStateTests, restartingInputReseedsPairingFromCurrentState)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Shift, kCGEventFlagMaskShift);
        OSXScreenInputStateTestAccess::key(screen, kCGEventKeyDown, kVK_Tab, kCGEventFlagMaskShift);
    }));
    screen.disable();
    keys.physicalMask = KeyModifierControl;
    screen.updateKeyState();
    OSXScreenInputStateTestAccess::restartOwner(screen);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_FALSE(OSXScreenInputStateTestAccess::key(screen, kCGEventKeyUp, kVK_Tab));
        OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Control);
    }));
    ASSERT_EQ(3u, events.keys.size());
    EXPECT_EQ(kVK_Control + 1, events.keys.back().button);
    EXPECT_FALSE(events.keys.back().down);
    EXPECT_EQ(0, screen.getActiveModifiers());
}

TEST_F(OSXScreenInputStateTests, queuedRefreshTimeoutNeverClearsLaterKeyOrButtonState)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    auto& keys = OSXScreenInputStateTestAccess::prepare(screen, true, true);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        keys.onKey(kVK_Tab + 1, true, KeyModifierShift);
        OSXScreenInputStateTestAccess::button(screen, true);
    }));
    OwnerBlock blocked(screen);
    screen.updateKeyState(); // the one-second queue deadline must cancel it
    blocked.release();
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([] {}));
    EXPECT_TRUE(screen.isKeyDown(kVK_Tab + 1));
    EXPECT_EQ(KeyModifierShift, screen.getActiveModifiers());
    UInt32 button = 0;
    EXPECT_TRUE(screen.isAnyMouseButtonDown(button));
    EXPECT_EQ(kButtonLeft, button);
    EXPECT_EQ(0u, keys.polls.load());
}

TEST_F(OSXScreenInputStateTests, primaryButtonQueriesAndSynthesisWaitForTapState)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    const std::vector<std::function<void()>> operations = {
        [&] { UInt32 button = 0; EXPECT_FALSE(screen.isAnyMouseButtonDown(button)); },
        [&] { screen.fakeMouseButton(kButtonLeft, true); },
        [&] { screen.fakeMouseMove(100, 200); },
        [&] { screen.fakeMouseRelativeMove(1, 2); },
        [&] { screen.fakeMouseWheel(0, 120); },
        [&] { screen.fakeMouseButton(kButtonLeft, false); }
    };
    for (const auto& operation : operations) {
        OwnerBlock blocked(screen);
        auto result = std::async(std::launch::async, operation);
        const auto pending = result.wait_for(std::chrono::milliseconds(50));
        blocked.release();
        result.get();
        EXPECT_EQ(std::future_status::timeout, pending);
    }
    UInt32 button = 0;
    EXPECT_FALSE(screen.isAnyMouseButtonDown(button));
    EXPECT_EQ(5u, postedEvents.load());
}

TEST_F(OSXScreenInputStateTests, hotkeyRegistrationAndRemovalWaitForTapLookup)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    OwnerBlock blocked(screen);
    auto registration = std::async(std::launch::async, [&] { return screen.registerHotKey(kKeyTab, 0); });
    const auto pending = registration.wait_for(std::chrono::milliseconds(50));
    blocked.release();
    const UInt32 id = registration.get();
    EXPECT_EQ(std::future_status::timeout, pending);
    ASSERT_NE(0u, id);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        EXPECT_TRUE(OSXScreenInputStateTestAccess::key(screen, kCGEventKeyDown, kVK_Tab));
    }));
    OwnerBlock blockedAgain(screen);
    auto removal = std::async(std::launch::async, [&] { screen.unregisterHotKey(id); });
    const auto removalPending = removal.wait_for(std::chrono::milliseconds(50));
    blockedAgain.release();
    removal.get();
    EXPECT_EQ(std::future_status::timeout, removalPending);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::key(screen, kCGEventKeyDown, kVK_Tab);
    }));
    EXPECT_EQ(std::vector<UInt32>({id}), events.hotkeys);
    ASSERT_EQ(1u, events.keys.size());
    EXPECT_TRUE(events.keys.front().down);
}

TEST_F(OSXScreenInputStateTests, timedOutRegistrationDoesNotInstallAModifierHotkeyLater)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    OwnerBlock blocked(screen);
    const UInt32 id = screen.registerHotKey(kKeyNone, KeyModifierShift);
    blocked.release();
    EXPECT_EQ(0u, id);
    ASSERT_TRUE(OSXScreenInputStateTestAccess::loop(screen).invoke([&] {
        OSXScreenInputStateTestAccess::key(screen, kCGEventFlagsChanged, kVK_Shift, kCGEventFlagMaskShift);
    }));
    EXPECT_TRUE(events.hotkeys.empty());
    ASSERT_EQ(1u, events.keys.size());
    EXPECT_TRUE(events.keys.front().down);
}

TEST_F(OSXScreenInputStateTests, carbonHotkeyDeliveryWaitsForOwnerAndIgnoresRemovedRegistration)
{
    StateEventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenInputStateTestAccess::prepare(screen, true, true);
    const UInt32 id = screen.registerHotKey(kKeyTab, 0);
    ASSERT_NE(0u, id);
    EventRef event = nullptr;
    ASSERT_EQ(noErr, CreateEvent(nullptr, kEventClassKeyboard, kEventHotKeyPressed, 0, 0, &event));
    EventHotKeyID nativeID = {'SNRG', id};
    ASSERT_EQ(noErr, SetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                                      sizeof(nativeID), &nativeID));
    OwnerBlock blocked(screen);
    auto delivery = std::async(std::launch::async, [&] { return OSXScreenInputStateTestAccess::hotKey(screen, event); });
    const auto pending = delivery.wait_for(std::chrono::milliseconds(50));
    blocked.release();
    EXPECT_TRUE(delivery.get());
    EXPECT_EQ(std::future_status::timeout, pending);
    screen.unregisterHotKey(id);
    EXPECT_FALSE(OSXScreenInputStateTestAccess::hotKey(screen, event));
    ReleaseEvent(event);
    EXPECT_EQ(std::vector<UInt32>({id}), events.hotkeys);
}

TEST_F(OSXScreenInputStateTests, secondaryAndPrimaryWithoutLoopKeepSynchronousStateUpdates)
{
    for (bool primary : {false, true}) {
        StateEventQueue events;
        OSXScreen screen(&events, false, false);
        auto& keys = OSXScreenInputStateTestAccess::prepare(screen, primary, false);
        keys.physicalKeys.insert(kVK_Tab + 1);
        keys.physicalMask = KeyModifierControl;
        nativeButtons = 1;
        screen.updateKeyState();
        EXPECT_TRUE(screen.isKeyDown(kVK_Tab + 1));
        EXPECT_EQ(KeyModifierControl, screen.getActiveModifiers());
        UInt32 button = 0;
        EXPECT_TRUE(screen.isAnyMouseButtonDown(button));
        EXPECT_EQ(kButtonLeft, button);
        EXPECT_EQ(1u, keys.polls.load());
    }
}
