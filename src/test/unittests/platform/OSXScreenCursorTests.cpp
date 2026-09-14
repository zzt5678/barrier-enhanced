#include "platform/OSXScreen.h"
#include "base/EventQueue.h"
#include "test/global/gtest.h"
#include <unistd.h>

// Keep unit tests from hiding the build host's actual pointer. These two
// framework boundaries model Apple's counted cursor visibility API.
namespace {
int hideCalls = 0;
int showCalls = 0;
CGError hideResult = kCGErrorSuccess;
CGError showResult = kCGErrorSuccess;
}
extern "C" CGError CGDisplayHideCursor(CGDirectDisplayID)
{
    ++hideCalls;
    return hideResult;
}
extern "C" CGError CGDisplayShowCursor(CGDirectDisplayID)
{
    ++showCalls;
    return showResult;
}

class OSXScreenCursorTestAccess {
public:
    static void hide(OSXScreen& screen) { screen.hideCursor(); }
    static void show(OSXScreen& screen) { screen.showCursor(); }
    static bool hidden(const OSXScreen& screen) { return screen.m_cursorHidden; }
    static bool onScreen(const OSXScreen& screen) { return screen.m_isOnScreen; }
    static CGPoint center(const OSXScreen& screen)
    {
        return CGPointMake(screen.m_xCenter, screen.m_yCenter);
    }
    static CGEventRef dispatch(OSXScreen& screen, CGEventType type, CGEventRef event)
    {
        return OSXScreen::handleCGInputEventSecondary(nullptr, type, event, &screen);
    }
};

class OSXScreenCursorTests : public testing::Test {
protected:
    void SetUp() override
    {
        hideCalls = showCalls = 0;
        hideResult = showResult = kCGErrorSuccess;
    }
};

TEST_F(OSXScreenCursorTests, repeatedHideAndShowBalanceOneOwnedHide)
{
    EventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenCursorTestAccess::show(screen);
    OSXScreenCursorTestAccess::hide(screen);
    OSXScreenCursorTestAccess::hide(screen);
    OSXScreenCursorTestAccess::show(screen);
    OSXScreenCursorTestAccess::show(screen);
    EXPECT_EQ(1, hideCalls);
    EXPECT_EQ(1, showCalls);
    EXPECT_FALSE(OSXScreenCursorTestAccess::hidden(screen));
}

TEST_F(OSXScreenCursorTests, failedHideDoesNotAcquireHideOwnership)
{
    EventQueue events;
    OSXScreen screen(&events, false, false);
    hideResult = kCGErrorFailure;
    OSXScreenCursorTestAccess::hide(screen);
    EXPECT_FALSE(OSXScreenCursorTestAccess::hidden(screen));
    OSXScreenCursorTestAccess::show(screen);
    EXPECT_EQ(0, showCalls);
}

TEST_F(OSXScreenCursorTests, failedShowRetainsOwnershipForRetry)
{
    EventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenCursorTestAccess::hide(screen);
    showResult = kCGErrorFailure;
    OSXScreenCursorTestAccess::show(screen);
    EXPECT_TRUE(OSXScreenCursorTestAccess::hidden(screen));
    showResult = kCGErrorSuccess;
    OSXScreenCursorTestAccess::show(screen);
    EXPECT_FALSE(OSXScreenCursorTestAccess::hidden(screen));
    EXPECT_EQ(2, showCalls);
}

TEST_F(OSXScreenCursorTests, physicalPointerActivityRestoresCursorWithoutTakingRemoteOwnership)
{
    EventQueue events;
    OSXScreen screen(&events, false, false);
    const CGEventType types[] = {kCGEventMouseMoved, kCGEventLeftMouseDragged,
        kCGEventRightMouseDragged, kCGEventOtherMouseDragged, kCGEventLeftMouseDown,
        kCGEventLeftMouseUp, kCGEventRightMouseDown, kCGEventRightMouseUp,
        kCGEventOtherMouseDown, kCGEventOtherMouseUp, kCGEventScrollWheel};
    CGEventRef event = CGEventCreate(nullptr);
    ASSERT_NE(nullptr, event);
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, 0);
    CGEventSetLocation(event, OSXScreenCursorTestAccess::center(screen));
    for (const auto type : types) {
        OSXScreenCursorTestAccess::hide(screen);
        EXPECT_EQ(event, OSXScreenCursorTestAccess::dispatch(screen, type, event));
        EXPECT_FALSE(OSXScreenCursorTestAccess::hidden(screen)) << type;
        EXPECT_FALSE(OSXScreenCursorTestAccess::onScreen(screen));
    }
    CFRelease(event);
    EXPECT_EQ(11, hideCalls);
    EXPECT_EQ(11, showCalls);
}

TEST_F(OSXScreenCursorTests, localDriverProcessCanRestoreCursor)
{
    EventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenCursorTestAccess::hide(screen);
    CGEventRef event = CGEventCreate(nullptr);
    ASSERT_NE(nullptr, event);
    // A local input driver need not report the kernel's PID zero.
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, getpid() + 1);
    OSXScreenCursorTestAccess::dispatch(screen, kCGEventMouseMoved, event);
    EXPECT_FALSE(OSXScreenCursorTestAccess::hidden(screen));
    EXPECT_EQ(1, showCalls);
    CFRelease(event);
}

TEST_F(OSXScreenCursorTests, ownInjectedPointerAndKeyboardEventsDoNotRestoreCursor)
{
    EventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenCursorTestAccess::hide(screen);
    CGEventRef event = CGEventCreate(nullptr);
    ASSERT_NE(nullptr, event);
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, getpid());
    EXPECT_EQ(event, OSXScreenCursorTestAccess::dispatch(screen, kCGEventMouseMoved, event));
    EXPECT_TRUE(OSXScreenCursorTestAccess::hidden(screen));
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, 0);
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, 0x5745415645505452);
    OSXScreenCursorTestAccess::dispatch(screen, kCGEventMouseMoved, event);
    EXPECT_TRUE(OSXScreenCursorTestAccess::hidden(screen));
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, 0);
    OSXScreenCursorTestAccess::dispatch(screen, kCGEventKeyDown, event);
    EXPECT_TRUE(OSXScreenCursorTestAccess::hidden(screen));
    EXPECT_EQ(0, showCalls);
    CFRelease(event);
}

TEST_F(OSXScreenCursorTests, nullAndDisabledTapEventsAreSafeWithoutShowingCursor)
{
    EventQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenCursorTestAccess::hide(screen);
    EXPECT_EQ(nullptr, OSXScreenCursorTestAccess::dispatch(screen, kCGEventMouseMoved, nullptr));
    EXPECT_EQ(nullptr, OSXScreenCursorTestAccess::dispatch(screen, kCGEventTapDisabledByTimeout, nullptr));
    EXPECT_EQ(nullptr, OSXScreenCursorTestAccess::dispatch(screen, kCGEventTapDisabledByUserInput, nullptr));
    EXPECT_TRUE(OSXScreenCursorTestAccess::hidden(screen));
    EXPECT_EQ(0, showCalls);
}
