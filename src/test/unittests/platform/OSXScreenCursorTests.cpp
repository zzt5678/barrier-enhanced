#include "platform/OSXScreen.h"
#include "base/EventQueue.h"
#include "test/global/gtest.h"
#include <unistd.h>
#include <vector>
#include <future>
#include <functional>
#include <time.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <limits>

// Keep unit tests from hiding the build host's actual pointer. These two
// framework boundaries model Apple's counted cursor visibility API.
namespace {
int hideCalls = 0;
int showCalls = 0;
CGError hideResult = kCGErrorSuccess;
CGError showResult = kCGErrorSuccess;
int warpCalls = 0;
int warpsAtHide = 0;
std::vector<CGPoint> warps;
CGError warpResult = kCGErrorSuccess;
std::function<void()> beforeWarp;
std::vector<bool> associations;
CGError detachResult = kCGErrorSuccess;
CGError attachResult = kCGErrorSuccess;
bool modelPointerPosition = false;
CGPoint modelPointer = CGPointZero;
thread_local bool trackScrollPreferenceReads = false;
thread_local int scrollPreferenceReads = 0;
}
// The tap lifecycle tests share these native cursor boundaries so queued
// events can be captured during a warp without touching the desktop.
void configureCursorModelForTapTest(CGPoint position, CGError result,
                                   std::function<void()> hook)
{
    modelPointerPosition = true;
    modelPointer = position;
    warpResult = result;
    hideResult = showResult = detachResult = attachResult = kCGErrorSuccess;
    beforeWarp = std::move(hook);
}
void resetCursorModelForTapTest()
{
    beforeWarp = {};
    modelPointerPosition = false;
    warpResult = kCGErrorSuccess;
}
extern "C" CGEventRef CGEventCreate(CGEventSourceRef source)
{
    using CreateEvent = CGEventRef (*)(CGEventSourceRef);
    static const auto original = reinterpret_cast<CreateEvent>(dlsym(RTLD_NEXT, "CGEventCreate"));
    CGEventRef event = original(source);
    if (event && modelPointerPosition) CGEventSetLocation(event, modelPointer);
    return event;
}
extern "C" CFPropertyListRef CFPreferencesCopyValue(CFStringRef key,
    CFStringRef app, CFStringRef user, CFStringRef host)
{
    if (trackScrollPreferenceReads && CFEqual(key, CFSTR("com.apple.scrollwheel.scaling"))) {
        ++scrollPreferenceReads;
        const double scale = 0;
        return CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &scale);
    }
    using CopyPreference = CFPropertyListRef (*)(CFStringRef, CFStringRef, CFStringRef, CFStringRef);
    static const auto original = reinterpret_cast<CopyPreference>(dlsym(RTLD_NEXT, "CFPreferencesCopyValue"));
    return original ? original(key, app, user, host) : nullptr;
}
extern "C" CGError CGWarpMouseCursorPosition(CGPoint point)
{
    if (beforeWarp) beforeWarp();
    ++warpCalls;
    warps.push_back(point);
    if (modelPointerPosition && warpResult == kCGErrorSuccess) modelPointer = point;
    return warpResult;
}
extern "C" CGError CGAssociateMouseAndMouseCursorPosition(boolean_t connected)
{
    associations.push_back(connected);
    return connected ? attachResult : detachResult;
}
extern "C" CGError CGSetLocalEventsSuppressionInterval(CFTimeInterval) { return kCGErrorSuccess; }
extern "C" CGError CGDisplayHideCursor(CGDirectDisplayID)
{
    warpsAtHide = warpCalls;
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
    static void primary(OSXScreen& screen, bool local)
    {
        screen.m_isPrimary = true;
        screen.m_isOnScreen = local;
        screen.m_x = screen.m_y = 0;
        screen.m_w = 2560; screen.m_h = 1440;
        screen.m_xCenter = 1280; screen.m_yCenter = 720;
        screen.m_xCursor = 1280; screen.m_yCursor = 720;
        screen.m_parkPoint = CGPointMake(1280, 720);
    }
    static CGEventRef primaryEvent(OSXScreen& screen, CGEventType type, CGEventRef event)
    {
        if (screen.m_inputLoop && !screen.m_inputLoop->isOwner()) {
            CGEventRef result = nullptr;
            screen.m_inputLoop->invoke([&] { result = primaryEvent(screen, type, event); });
            return result;
        }
        return OSXScreen::handleCGInputEvent(nullptr, type, event, &screen);
    }
    static void startOwner(OSXScreen& screen) { screen.m_inputLoop.reset(new OSXInputLoop); }
    static OSXInputLoop& owner(OSXScreen& screen) { return *screen.m_inputLoop; }
    static bool ownerResponsive(OSXScreen& screen)
    {
        return screen.m_inputLoop->invoke([] {}, std::chrono::milliseconds(100));
    }
    static void hide(OSXScreen& screen) { screen.hideCursor(); }
    static void show(OSXScreen& screen) { screen.showCursor(); }
    static bool hidden(const OSXScreen& screen) { return screen.m_cursorHidden; }
    static bool onScreen(const OSXScreen& screen) { return screen.m_isOnScreen; }
    static void parkAt(OSXScreen& screen, CGPoint point) { screen.m_parkPoint = point; }
    static void initializePointer(OSXScreen& screen) { screen.initializePointerMotion(); }
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
        warpCalls = warpsAtHide = 0;
        warps.clear();
        warpResult = kCGErrorSuccess;
        beforeWarp = {};
        hideResult = showResult = kCGErrorSuccess;
        detachResult = attachResult = kCGErrorSuccess;
        associations.clear();
        modelPointerPosition = false;
    }
    void TearDown() override { modelPointerPosition = false; }
};

// These tests have no App singleton or file drag. Keep real enter/leave and
// replace only the unrelated global drag-configuration query.
class CursorOwnershipScreen : public OSXScreen {
public:
    explicit CursorOwnershipScreen(EventQueue* events) : OSXScreen(events, false, false) {}
    bool isDraggingStarted() override { return false; }
};

class CursorRecoveryQueue : public EventQueue {
public:
    int quits = 0;
    int errors = 0;
    void* errorTarget = nullptr;
    void addEvent(const Event& event) override {
        if (event.getType() == Event::kQuit) {
            ++quits;
            Event::deleteData(event);
        }
        else if (event.getType() == forIScreen().error()) {
            ++errors;
            errorTarget = event.getTarget();
            Event::deleteData(event);
        }
        else EventQueue::addEvent(event);
    }
};

static void expectOwnerTimeoutReportsInputFailure(
    const std::function<void(CursorOwnershipScreen&)>& operation)
{
    CursorRecoveryQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    OSXScreenCursorTestAccess::startOwner(screen);
    ASSERT_TRUE(screen.leave());
    auto resume = std::make_shared<std::promise<void>>();
    auto gate = resume->get_future().share();
    auto entered = std::make_shared<std::promise<void>>();
    auto started = entered->get_future();
    auto& owner = OSXScreenCursorTestAccess::owner(screen);
    EXPECT_EQ(OSXInputLoop::ReleaseResult::Pending,
        owner.invokeRelease([entered, gate] { entered->set_value(); gate.wait(); },
                            std::chrono::milliseconds(20)));
    started.wait();
    const int before = warpCalls;
    operation(screen);
    EXPECT_EQ(1, events.errors);
    EXPECT_EQ(screen.getEventTarget(), events.errorTarget);
    EXPECT_EQ(0, events.quits);
    EXPECT_FALSE(OSXScreenCursorTestAccess::onScreen(screen));
    resume->set_value();
    EXPECT_TRUE(owner.invoke([] {}));
    EXPECT_EQ(before, warpCalls); // expired return/warp cannot execute later
    screen.disable();
    EXPECT_EQ(std::vector<bool>({false, true}), associations);
}

TEST_F(OSXScreenCursorTests, timedOutEnterReportsInputFailureWithoutLateTransition)
{
    expectOwnerTimeoutReportsInputFailure([](CursorOwnershipScreen& screen) {
        EXPECT_FALSE(screen.tryEnter());
    });
}

TEST_F(OSXScreenCursorTests, timedOutPositionedEnterReportsInputFailureWithoutLateWarp)
{
    expectOwnerTimeoutReportsInputFailure([](CursorOwnershipScreen& screen) {
        EXPECT_FALSE(screen.tryEnterAt(2543, 600));
    });
}

TEST_F(OSXScreenCursorTests, timedOutWarpReportsInputFailureWithoutLateWarp)
{
    expectOwnerTimeoutReportsInputFailure([](CursorOwnershipScreen& screen) {
        screen.warpCursor(2543, 600);
    });
}

// Replace the native pasteboard boundary, not leave()/input ownership. A
// delayed provider must not block the primary tap after hardware capture.
class DragProviderProbeScreen : public CursorOwnershipScreen {
public:
    explicit DragProviderProbeScreen(EventQueue* events) : CursorOwnershipScreen(events) {}
    bool isDraggingStarted() override { return m_draggingStarted; }
    String& getDraggingFilename() override
    {
        ++reads;
        if (read) read();
        return files;
    }
    int reads = 0;
    String files;
    std::function<void()> read;
};

TEST_F(OSXScreenCursorTests, primaryDragLeaveDoesNotWaitForPasteboardAfterCapturingMouse)
{
    EventQueue events;
    DragProviderProbeScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    OSXScreenCursorTestAccess::startOwner(screen);
    screen.setDraggingStarted(true);
    std::promise<void> release;
    auto gate = release.get_future().share();
    screen.read = [&] { gate.wait(); };
    auto leaving = std::async(std::launch::async, [&] { return screen.leave(); });
    const auto completion = leaving.wait_for(std::chrono::milliseconds(500));
    const bool responsive = OSXScreenCursorTestAccess::ownerResponsive(screen);
    release.set_value(); // always unblock old code, even when assertions fail
    EXPECT_TRUE(leaving.get());
    EXPECT_EQ(std::future_status::ready, completion);
    EXPECT_TRUE(responsive);
    EXPECT_EQ(0, screen.reads);
    EXPECT_FALSE(screen.isDraggingStarted());
    EXPECT_FALSE(OSXScreenCursorTestAccess::onScreen(screen));
    EXPECT_TRUE(screen.tryEnterAt(2543, 600));
}

TEST_F(OSXScreenCursorTests, secondaryDragLeaveStillCollectsItsOutgoingFileList)
{
    EventQueue events;
    DragProviderProbeScreen screen(&events);
    screen.setDraggingStarted(true);
    EXPECT_TRUE(screen.leave());
    EXPECT_EQ(1, screen.reads);
    EXPECT_FALSE(screen.isDraggingStarted());
}

TEST_F(OSXScreenCursorTests, primaryLeaveDetachesPointerAndEnterRestoresItOnce)
{
    EventQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    ASSERT_TRUE(screen.leave());
    EXPECT_FALSE(OSXScreenCursorTestAccess::onScreen(screen));
    ASSERT_EQ(std::vector<bool>({false}), associations);
    EXPECT_TRUE(screen.leave());
    EXPECT_EQ(std::vector<bool>({false}), associations);
    screen.enter();
    EXPECT_TRUE(OSXScreenCursorTestAccess::onScreen(screen));
    EXPECT_EQ(std::vector<bool>({false, true}), associations);
    screen.enter();
    EXPECT_EQ(std::vector<bool>({false, true}), associations);
}

TEST_F(OSXScreenCursorTests, failedPointerDetachRejectsHandoffAndKeepsLocalCursor)
{
    EventQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    detachResult = kCGErrorFailure;
    EXPECT_FALSE(screen.leave());
    EXPECT_TRUE(OSXScreenCursorTestAccess::onScreen(screen));
    EXPECT_FALSE(OSXScreenCursorTestAccess::hidden(screen));
    EXPECT_EQ(std::vector<bool>({false}), associations);
}

TEST_F(OSXScreenCursorTests, failedPrimaryHideRejectsHandoffBeforeDetaching)
{
    EventQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    hideResult = kCGErrorFailure;
    EXPECT_FALSE(screen.leave());
    EXPECT_TRUE(OSXScreenCursorTestAccess::onScreen(screen));
    EXPECT_TRUE(associations.empty());
}

TEST_F(OSXScreenCursorTests, disableRestoresPointerEvenWithoutAutomaticCursorVisibility)
{
    EventQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    ASSERT_TRUE(screen.leave());
    screen.disable();
    EXPECT_EQ(std::vector<bool>({false, true}), associations);
    EXPECT_FALSE(OSXScreenCursorTestAccess::hidden(screen));
}

TEST_F(OSXScreenCursorTests, failedCursorShowDoesNotPreventPointerRestoration)
{
    EventQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    ASSERT_TRUE(screen.leave());
    showResult = kCGErrorFailure;
    screen.enter();
    EXPECT_EQ(std::vector<bool>({false, true}), associations);
    EXPECT_TRUE(OSXScreenCursorTestAccess::hidden(screen));
    showResult = kCGErrorSuccess;
    screen.disable();
    EXPECT_FALSE(OSXScreenCursorTestAccess::hidden(screen));
}

TEST_F(OSXScreenCursorTests, failedPointerRestoreIsRetriedDuringDisable)
{
    CursorRecoveryQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    ASSERT_TRUE(screen.leave());
    attachResult = kCGErrorFailure;
    EXPECT_FALSE(screen.tryEnter());
    // Stop sharing automatically; do not leave a frozen local cursor until
    // the user manages to quit manually. Cleanup retries the native restore.
    EXPECT_EQ(1, events.errors);
    EXPECT_EQ(screen.getEventTarget(), events.errorTarget);
    EXPECT_EQ(0, events.quits);
    EXPECT_FALSE(OSXScreenCursorTestAccess::onScreen(screen));
    attachResult = kCGErrorSuccess;
    screen.disable();
    EXPECT_EQ(std::vector<bool>({false, true, true}), associations);
}

class CursorMotionQueue : public EventQueue {
public:
    int quits = 0;
    int errors = 0;
    void* errorTarget = nullptr;
    std::vector<std::pair<SInt32, SInt32>> remote;
    std::vector<std::pair<SInt32, SInt32>> local;
    std::vector<std::pair<SInt32, SInt32>> wheels;
    void addEvent(const Event& event) override
    {
        if (event.getType() == Event::kQuit) {
            ++quits;
            Event::deleteData(event);
        }
        else if (event.getType() == forIScreen().error()) {
            ++errors;
            errorTarget = event.getTarget();
            Event::deleteData(event);
        }
        else if (event.getType() == forIPrimaryScreen().motionOnSecondary()) {
            const auto* motion = static_cast<const IPrimaryScreen::MotionInfo*>(event.getData());
            remote.emplace_back(motion->m_x, motion->m_y);
            Event::deleteData(event);
        }
        else if (event.getType() == forIPrimaryScreen().motionOnPrimary()) {
            const auto* motion = static_cast<const IPrimaryScreen::MotionInfo*>(event.getData());
            local.emplace_back(motion->m_x, motion->m_y);
            Event::deleteData(event);
        }
        else if (event.getType() == forIPrimaryScreen().wheel()) {
            const auto* wheel = static_cast<const IPrimaryScreen::WheelInfo*>(event.getData());
            wheels.emplace_back(wheel->m_xDelta, wheel->m_yDelta);
            Event::deleteData(event);
        }
        else EventQueue::addEvent(event);
    }
};

TEST_F(OSXScreenCursorTests, wheelCallbackDoesNotSynchronouslyReadSystemPreferences)
{
    CursorMotionQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenCursorTestAccess::primary(screen, true);
    CGEventRef event = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 2, 1, 0);
    ASSERT_NE(nullptr, event);
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, 0);
    CGEventSetIntegerValueField(event, kCGScrollWheelEventFixedPtDeltaAxis1, 65536);
    CGEventSetIntegerValueField(event, kCGScrollWheelEventFixedPtDeltaAxis2, 0);
    scrollPreferenceReads = 0;
    trackScrollPreferenceReads = true;
    const auto result = OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventScrollWheel, event);
    trackScrollPreferenceReads = false;
    EXPECT_EQ(event, result);
    EXPECT_EQ(0, scrollPreferenceReads);
    ASSERT_EQ(1u, events.wheels.size());
    EXPECT_EQ(std::make_pair(SInt32(0), SInt32(120)), events.wheels[0]);
    CFRelease(event);
}

TEST_F(OSXScreenCursorTests, remoteMotionParksBackgroundCursorWithoutChangingEventDelta)
{
    CursorMotionQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenCursorTestAccess::primary(screen, false);
    modelPointerPosition = true;
    modelPointer = CGPointMake(1283, 718);
    CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointMake(0, 0), kCGMouseButtonLeft);
    ASSERT_NE(nullptr, event);
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, 0);
    // Deliberately stale pre-warp absolute location, unrelated to real delta.
    CGEventSetLocation(event, CGPointMake(2500, 1400));
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, 3);
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, -2);
    EXPECT_EQ(nullptr, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, event));
    ASSERT_EQ(1u, warps.size());
    EXPECT_EQ(1280, warps[0].x);
    EXPECT_EQ(720, warps[0].y);
    ASSERT_EQ(1u, events.remote.size());
    EXPECT_EQ(std::make_pair(SInt32(3), SInt32(-2)), events.remote[0]);
    CFRelease(event);
}

TEST_F(OSXScreenCursorTests, remoteZeroDeltaDoesNotReplayOldAbsolutePosition)
{
    CursorMotionQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenCursorTestAccess::primary(screen, false);
    modelPointerPosition = true;
    modelPointer = CGPointMake(1400, 800);
    CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointMake(0, 0), kCGMouseButtonLeft);
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, 0);
    CGEventSetLocation(event, CGPointMake(1400, 800));
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, 0);
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, 0);
    EXPECT_EQ(nullptr, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, event));
    EXPECT_TRUE(events.remote.empty());
    ASSERT_EQ(1u, warps.size());
    EXPECT_EQ(1280, warps[0].x);
    EXPECT_EQ(720, warps[0].y);
    CFRelease(event);
}

TEST_F(OSXScreenCursorTests, primaryIgnoresOwnSyntheticPointerEvents)
{
    CursorMotionQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenCursorTestAccess::primary(screen, false);
    CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointMake(0, 0), kCGMouseButtonLeft);
    CGEventSetLocation(event, CGPointMake(1400, 800));
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, 120);
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, 0);
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, 0x5745415645505452);
    EXPECT_EQ(event, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, event));
    EXPECT_TRUE(events.remote.empty());
    EXPECT_EQ(0, warpCalls);
    CFRelease(event);
}

TEST_F(OSXScreenCursorTests, remoteDragKeepsSmallDeltasWhileParkingBackgroundCursor)
{
    CursorMotionQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenCursorTestAccess::primary(screen, false);
    modelPointerPosition = true;
    CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseDragged, CGPointMake(0, 0), kCGMouseButtonLeft);
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, 0);
    CGEventSetLocation(event, CGPointMake(2559, 0));
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, 1);
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, -1);
    for (const auto type : {kCGEventLeftMouseDragged, kCGEventRightMouseDragged, kCGEventOtherMouseDragged}) {
        modelPointer = CGPointMake(1281, 719);
        EXPECT_EQ(nullptr, OSXScreenCursorTestAccess::primaryEvent(screen, type, event));
    }
    ASSERT_EQ(3u, events.remote.size());
    for (const auto& motion : events.remote)
        EXPECT_EQ(std::make_pair(SInt32(1), SInt32(-1)), motion);
    EXPECT_EQ(3, warpCalls);
    CFRelease(event);
}

namespace {
CGEventTimestamp pointerTimestamp(bool nanoseconds)
{
    const auto ticks = mach_absolute_time();
    mach_timebase_info_data_t base;
    mach_timebase_info(&base);
    return nanoseconds ? static_cast<uint64_t>((static_cast<__uint128_t>(ticks) * base.numer) / base.denom) : ticks;
}
CGEventRef pointerFixture(CGPoint position, SInt32 dx, SInt32 dy, bool ns = true,
    int sourceState = kCGEventSourceStateHIDSystemState, pid_t pid = 74972)
{
    CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, position, kCGMouseButtonLeft);
    CGEventSetIntegerValueField(event, kCGEventSourceStateID, sourceState);
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, pid);
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, dx);
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, dy);
    CGEventSetTimestamp(event, pointerTimestamp(ns));
    return event;
}
void deliverPointer(OSXScreen& screen, CGPoint position, SInt32 dx, SInt32 dy,
    bool ns = true, int sourceState = kCGEventSourceStateHIDSystemState, pid_t pid = 74972)
{
    modelPointer = position;
    CGEventRef event = pointerFixture(position, dx, dy, ns, sourceState, pid);
    OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, event);
    CFRelease(event);
}
void seedPointer(OSXScreen& screen, CGPoint position, bool ns = true)
{
    modelPointerPosition = true;
    OSXScreenCursorTestAccess::primary(screen, true);
    deliverPointer(screen, position, 0, 0, ns);
}
}

TEST_F(OSXScreenCursorTests, queuedRemoteMotionDoesNotReparkAnAlreadyParkedPointer)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, false);
    modelPointerPosition = true;
    modelPointer = CGPointMake(1280, 720);

    // Multiple queued motion events can carry an old absolute position after
    // parking. Their HID deltas must still reach the client exactly once.
    for (int i = 0; i < 16; ++i) {
        auto event = pointerFixture(CGPointMake(1285, 720), 5, 0);
        ASSERT_NE(nullptr, event);
        EXPECT_EQ(nullptr, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, event));
        CFRelease(event);
    }
    EXPECT_EQ(0, warpCalls);
    ASSERT_EQ(16u, events.remote.size());
    for (const auto& delta : events.remote) {
        EXPECT_EQ(std::make_pair(SInt32(5), SInt32(0)), delta);
    }
}

TEST_F(OSXScreenCursorTests, nativeWarpDeltaRetainsRealPositiveMotionAndRepeatedParking)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    seedPointer(screen, CGPointMake(2559, 720));
    ASSERT_TRUE(screen.leave());
    // Native reproduction: the first +8 is combined with the -1279 warp.
    // Association may keep the event location exactly at the park point.
    deliverPointer(screen, CGPointMake(1280, 720), -1271, 0);
    deliverPointer(screen, CGPointMake(1288, 720), 8, 0);
    // Each following repark contributes -8 to the next actual +8.
    for (int i = 0; i < 5; ++i) deliverPointer(screen, CGPointMake(1288, 720), 0, 0);
    ASSERT_EQ(7u, events.remote.size());
    for (const auto& motion : events.remote) EXPECT_EQ(std::make_pair(SInt32(8), SInt32(0)), motion);
}

TEST_F(OSXScreenCursorTests, nativeWarpDeltaPreservesImmediateRealReverse)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    seedPointer(screen, CGPointMake(2559, 720));
    ASSERT_TRUE(screen.leave());
    deliverPointer(screen, CGPointMake(1280, 720), -1283, -2);
    ASSERT_EQ(1u, events.remote.size());
    EXPECT_EQ(std::make_pair(SInt32(-4), SInt32(-2)), events.remote[0]);
}

TEST_F(OSXScreenCursorTests, queuedPreWarpMotionDoesNotConsumeLaterPositionChangeInEitherClockDomain)
{
    for (bool ns : {false, true}) {
        CursorMotionQueue events;
        CursorOwnershipScreen screen(&events);
        seedPointer(screen, CGPointMake(2559, 720), ns);
        CGEventRef queued = pointerFixture(CGPointMake(2558, 720), -1, 0, ns);
        modelPointer = CGPointMake(2558, 720); // Already captured, callback still queued.
        ASSERT_TRUE(screen.leave());
        OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, queued);
        CFRelease(queued);
        deliverPointer(screen, CGPointMake(1280, 720), -1270, 0, ns);
        EXPECT_EQ((std::vector<std::pair<SInt32, SInt32>>{{-1, 0}, {8, 0}}), events.remote);
    }
}

TEST_F(OSXScreenCursorTests, multipleWarpsAndReturnReenterUseTheirNetPositionOnce)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    seedPointer(screen, CGPointMake(2559, 720));
    ASSERT_TRUE(screen.leave());
    screen.warpCursor(1200, 700);
    ASSERT_TRUE(screen.tryEnterAt(2543, 720));
    ASSERT_TRUE(screen.leave());
    deliverPointer(screen, CGPointMake(1280, 720), -1271, 0);
    deliverPointer(screen, CGPointMake(1280, 720), 4, -2);
    EXPECT_EQ((std::vector<std::pair<SInt32, SInt32>>{{8, 0}, {4, -2}}), events.remote);
}

TEST_F(OSXScreenCursorTests, localNativeMotionConsumesReturnWarpBeforeNextLeave)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    seedPointer(screen, CGPointMake(2559, 720));
    ASSERT_TRUE(screen.leave());
    ASSERT_TRUE(screen.tryEnterAt(2543, 720));
    deliverPointer(screen, CGPointMake(2540, 720), -19, 0);
    EXPECT_EQ(std::make_pair(SInt32(2540), SInt32(720)), events.local.back());
    EXPECT_TRUE(events.remote.empty());
    ASSERT_TRUE(screen.leave());
    deliverPointer(screen, CGPointMake(1280, 720), -1252, 0);
    ASSERT_EQ(1u, events.remote.size());
    EXPECT_EQ(std::make_pair(SInt32(8), SInt32(0)), events.remote[0]);
}

TEST_F(OSXScreenCursorTests, failedWarpDoesNotCreateCompensation)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    seedPointer(screen, CGPointMake(2559, 720));
    ASSERT_TRUE(screen.leave());
    warpResult = kCGErrorFailure;
    screen.warpCursor(900, 700);
    warpResult = kCGErrorSuccess;
    deliverPointer(screen, CGPointMake(1280, 720), -1271, 0);
    ASSERT_EQ(1u, events.remote.size());
    EXPECT_EQ(std::make_pair(SInt32(8), SInt32(0)), events.remote[0]);
}

TEST_F(OSXScreenCursorTests, directCGPositionChangesRemainDirectAndDoNotReplayIntoNextNativeMotion)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    seedPointer(screen, CGPointMake(1280, 720));
    // Local direct CG input is returned immediately, including our own PID.
    deliverPointer(screen, CGPointMake(1283, 718), 3, -2, false, kCGEventSourceStateCombinedSessionState, getpid());
    deliverPointer(screen, CGPointMake(1286, 716), 3, -2, false, kCGEventSourceStateCombinedSessionState, getpid());
    deliverPointer(screen, CGPointMake(1289, 714), 3, -2, false, kCGEventSourceStateCombinedSessionState, getpid());
    // Keep the same test geometry remote without adding another real warp.
    OSXScreenCursorTestAccess::primary(screen, false);
    deliverPointer(screen, CGPointMake(1286, 716), 6, -4);
    ASSERT_EQ(1u, events.remote.size());
    EXPECT_EQ(std::make_pair(SInt32(-3), SInt32(2)), events.remote[0]);
}

TEST_F(OSXScreenCursorTests, directCGMotionDoesNotConsumeNativeWarpCompensation)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    seedPointer(screen, CGPointMake(2559, 720));
    ASSERT_TRUE(screen.leave());
    deliverPointer(screen, CGPointMake(1288, 720), 8, 0, false, kCGEventSourceStateCombinedSessionState, 44001);
    deliverPointer(screen, CGPointMake(1280, 720), -1271, 0);
    EXPECT_EQ((std::vector<std::pair<SInt32, SInt32>>{{8, 0}, {8, 0}}), events.remote);
}

TEST_F(OSXScreenCursorTests, fractionalEdgeWarpKeepsResidualSmallAndDoesNotAccumulateDrift)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    seedPointer(screen, CGPointMake(2559.98, 720));
    ASSERT_TRUE(screen.leave());
    deliverPointer(screen, CGPointMake(1280, 720), -1271, 0);
    deliverPointer(screen, CGPointMake(1280, 720), 8, 0);
    ASSERT_EQ(2u, events.remote.size());
    EXPECT_GE(events.remote[0].first, 7);
    EXPECT_LE(events.remote[0].first, 9);
    EXPECT_EQ(std::make_pair(SInt32(8), SInt32(0)), events.remote[1]);
}

TEST_F(OSXScreenCursorTests, capturedMotionBetweenTwoWarpsConsumesOnlyItsEarlierTarget)
{
    for (bool ns : {false, true}) {
        CursorMotionQueue events;
        CursorOwnershipScreen screen(&events);
        seedPointer(screen, CGPointMake(100, 720), ns);
        screen.warpCursor(200, 720);
        CGEventRef queued = pointerFixture(CGPointMake(205, 720), 105, 0, ns);
        modelPointer = CGPointMake(205, 720);
        screen.warpCursor(300, 720);
        OSXScreenCursorTestAccess::primary(screen, false);
        // Park at the actual position so processing the queued event cannot
        // introduce another displacement into this ordering control.
        OSXScreenCursorTestAccess::parkAt(screen, CGPointMake(300, 720));
        OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, queued);
        CFRelease(queued);
        deliverPointer(screen, CGPointMake(305, 720), 100, 0, ns);
        EXPECT_EQ((std::vector<std::pair<SInt32, SInt32>>{{5, 0}, {5, 0}}), events.remote);
    }
}

TEST_F(OSXScreenCursorTests, coalescedNativeMotionBetweenWarpsRetainsEveryRealDelta)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    seedPointer(screen, CGPointMake(100, 720));
    screen.warpCursor(200, 720); // imposed +100
    modelPointer = CGPointMake(205, 720); // unseen real +5, not yet a callback
    screen.warpCursor(300, 720); // imposed +95, not +100
    OSXScreenCursorTestAccess::primary(screen, false);
    OSXScreenCursorTestAccess::parkAt(screen, CGPointMake(300, 720));
    // One coalesced callback includes +100 +5 +95 +5 = +205.
    deliverPointer(screen, CGPointMake(305, 720), 205, 0);
    EXPECT_EQ((std::vector<std::pair<SInt32, SInt32>>{{10, 0}}), events.remote);
}

TEST_F(OSXScreenCursorTests, zeroNativeDeltaCanContainRealMotionOppositeTheWarp)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    seedPointer(screen, CGPointMake(2559, 720));
    ASSERT_TRUE(screen.leave());
    deliverPointer(screen, CGPointMake(1280, 720), 0, 0);
    EXPECT_EQ((std::vector<std::pair<SInt32, SInt32>>{{1279, 0}}), events.remote);
}

TEST_F(OSXScreenCursorTests, warpOnlyDeltaIsConsumedOnceWithoutChangingNextRealReverse)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    seedPointer(screen, CGPointMake(2559, 720));
    ASSERT_TRUE(screen.leave());
    deliverPointer(screen, CGPointMake(1280, 720), -1279, 0);
    EXPECT_TRUE(events.remote.empty());
    deliverPointer(screen, CGPointMake(1280, 720), -17, 0);
    EXPECT_EQ((std::vector<std::pair<SInt32, SInt32>>{{-17, 0}}), events.remote);
}

TEST_F(OSXScreenCursorTests, unknownSourceStateRetainsInputWithoutConsumingNativePosition)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    seedPointer(screen, CGPointMake(2559, 720));
    ASSERT_TRUE(screen.leave());
    deliverPointer(screen, CGPointMake(1280, 720), 5, -3, true, 999, 44001);
    deliverPointer(screen, CGPointMake(1280, 720), -1271, 0);
    EXPECT_EQ((std::vector<std::pair<SInt32, SInt32>>{{5, -3}, {8, 0}}), events.remote);
}

TEST_F(OSXScreenCursorTests, unclassifiableNativeTimeWithRemoteWarpRestoresLocalAndStopsSharing)
{
    for (uint64_t timestamp : {uint64_t(0), std::numeric_limits<uint64_t>::max(),
             pointerTimestamp(true) + uint64_t(3600000000000)}) {
        CursorMotionQueue events;
        CursorOwnershipScreen screen(&events);
        seedPointer(screen, CGPointMake(2559, 720));
        ASSERT_TRUE(screen.leave());
        CGEventRef event = pointerFixture(CGPointMake(1280, 720), 8, 0);
        CGEventSetTimestamp(event, timestamp);
        EXPECT_EQ(event, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, event));
        CFRelease(event);
        EXPECT_TRUE(events.remote.empty());
        EXPECT_TRUE(OSXScreenCursorTestAccess::onScreen(screen));
        EXPECT_EQ(1, events.errors);
        deliverPointer(screen, CGPointMake(1263, 720), -17, 0);
        EXPECT_TRUE(events.remote.empty());
        EXPECT_EQ(std::make_pair(SInt32(1263), SInt32(720)), events.local.back());
    }
}

TEST_F(OSXScreenCursorTests, invalidNativeTimeWithoutPendingPositionKeepsRemoteInput)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    seedPointer(screen, CGPointMake(1280, 720));
    OSXScreenCursorTestAccess::primary(screen, false);
    CGEventRef event = pointerFixture(CGPointMake(1280, 720), 8, -2);
    CGEventSetTimestamp(event, 0);
    EXPECT_EQ(nullptr, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, event));
    CFRelease(event);
    deliverPointer(screen, CGPointMake(1280, 720), -17, 0);
    EXPECT_EQ((std::vector<std::pair<SInt32, SInt32>>{{8, -2}, {-17, 0}}), events.remote);
    EXPECT_EQ(0, events.errors);
}

TEST_F(OSXScreenCursorTests, invalidLocalNativeTimeDeliversOriginalEventAndRebasesBeforeLeave)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    seedPointer(screen, CGPointMake(2559, 720));
    ASSERT_TRUE(screen.leave());
    ASSERT_TRUE(screen.tryEnterAt(2543, 720));
    modelPointer = CGPointMake(2540, 720);
    CGEventRef event = pointerFixture(modelPointer, -19, 0);
    CGEventSetTimestamp(event, 0);
    EXPECT_EQ(event, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, event));
    CFRelease(event);
    EXPECT_EQ(0, events.errors);
    EXPECT_EQ(std::make_pair(SInt32(2540), SInt32(720)), events.local.back());
    ASSERT_TRUE(screen.leave());
    deliverPointer(screen, CGPointMake(1280, 720), -1252, 0);
    EXPECT_EQ((std::vector<std::pair<SInt32, SInt32>>{{8, 0}}), events.remote);
}

TEST_F(OSXScreenCursorTests, coldSeedIncludesOwnCGButtonPositionBeforeTheFirstNativeMotion)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    modelPointerPosition = true;
    modelPointer = CGPointMake(1000, 720);
    OSXScreenCursorTestAccess::primary(screen, true);
    // Same initialization used before the production tap is installed; no
    // physical/native fixture is sent to establish the baseline beforehand.
    OSXScreenCursorTestAccess::initializePointer(screen);
    CGEventRef button = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseUp,
        CGPointMake(1100, 720), kCGMouseButtonLeft);
    CGEventSetIntegerValueField(button, kCGEventSourceUnixProcessID, getpid());
    EXPECT_EQ(button, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventLeftMouseUp, button));
    CFRelease(button);
    modelPointer = CGPointMake(1100, 720);
    OSXScreenCursorTestAccess::primary(screen, false);
    OSXScreenCursorTestAccess::parkAt(screen, modelPointer);
    deliverPointer(screen, CGPointMake(1105, 720), 105, 0);
    EXPECT_EQ((std::vector<std::pair<SInt32, SInt32>>{{5, 0}}), events.remote);
}

TEST_F(OSXScreenCursorTests, oldRemoteMotionCannotUndoAtomicReturn)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    ASSERT_TRUE(screen.leave());
    CGEventRef stale = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointMake(2559, 600), kCGMouseButtonLeft);
    CGEventSetIntegerValueField(stale, kCGEventSourceUnixProcessID, 0);
    CGEventSetTimestamp(stale, 1);
    ASSERT_TRUE(screen.tryEnterAt(2543, 600));
    const int afterReturn = warpCalls;
    EXPECT_EQ(nullptr, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, stale));
    EXPECT_TRUE(events.local.empty());
    EXPECT_TRUE(events.remote.empty());
    EXPECT_EQ(afterReturn, warpCalls);
    ASSERT_FALSE(warps.empty());
    EXPECT_EQ(2543, warps.back().x);
    CGEventRef fresh = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointMake(2530, 600), kCGMouseButtonLeft);
    // Explicitly timestamped synthetic events may use nanoseconds. This
    // control is not evidence for the physical HID timestamp domain below.
    CGEventSetTimestamp(fresh, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
    CGEventSetIntegerValueField(fresh, kCGEventSourceUnixProcessID, 0);
    EXPECT_EQ(fresh, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, fresh));
    ASSERT_EQ(1u, events.local.size());
    EXPECT_EQ(std::make_pair(SInt32(2530), SInt32(600)), events.local.back());
    CFRelease(fresh);
    CFRelease(stale);
}

TEST_F(OSXScreenCursorTests, freshHardwareTickMotionSurvivesTheReturnFence)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    OSXScreenCursorTestAccess::startOwner(screen);
    ASSERT_TRUE(screen.leave());

    // 2026-11-05 physical trace: PID 0 event timestamp 59888423050527;
    // later mach_absolute_time() was 59897776031866 with timebase 125/3.
    // Thus this HID stream uses mach ticks, not converted nanoseconds.
    // Use current raw ticks to preserve that clock relationship, rather than
    // comparing a frozen capture timestamp to today's return boundary.
    CGEventRef stale = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved,
        CGPointMake(2559, 600), kCGMouseButtonLeft);
    ASSERT_NE(nullptr, stale);
    CGEventSetIntegerValueField(stale, kCGEventSourceUnixProcessID, 0);
    CGEventSetTimestamp(stale, mach_absolute_time());
    ASSERT_TRUE(screen.tryEnterAt(2543, 600));
    const int afterReturn = warpCalls;
    EXPECT_EQ(nullptr, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, stale));
    EXPECT_TRUE(events.local.empty());

    const std::vector<CGPoint> capturedPoints = {
        CGPointMake(2327.94, 761.12), CGPointMake(2325.33, 761.59), CGPointMake(1392.02, 589.38)
    };
    for (const auto& point : capturedPoints) {
        CGEventRef fresh = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved,
            point, kCGMouseButtonLeft);
        ASSERT_NE(nullptr, fresh);
        CGEventSetIntegerValueField(fresh, kCGEventSourceUnixProcessID, 0);
        CGEventSetTimestamp(fresh, mach_absolute_time());
        EXPECT_EQ(fresh, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, fresh));
        CFRelease(fresh);
    }
    EXPECT_EQ((std::vector<std::pair<SInt32, SInt32>>{{2327, 761}, {2325, 761}, {1392, 589}}), events.local);
    EXPECT_TRUE(events.remote.empty());
    EXPECT_EQ(afterReturn, warpCalls);
    CFRelease(stale);
}

TEST_F(OSXScreenCursorTests, failedRemoteParkingRestoresPointerAndStopsSharing)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    ASSERT_TRUE(screen.leave());
    // Require parking in the model, independently of the host's live cursor.
    modelPointerPosition = true;
    modelPointer = CGPointMake(1500, 800);
    CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointMake(1500, 800), kCGMouseButtonLeft);
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, 0);
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, 10);
    warpResult = kCGErrorFailure;
    EXPECT_EQ(nullptr, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, event));
    EXPECT_EQ(1, events.errors);
    EXPECT_EQ(screen.getEventTarget(), events.errorTarget);
    EXPECT_EQ(0, events.quits);
    EXPECT_TRUE(events.remote.empty());
    EXPECT_EQ(std::vector<bool>({false, true}), associations);
    EXPECT_FALSE(OSXScreenCursorTestAccess::hidden(screen));
    CFRelease(event);
}

TEST_F(OSXScreenCursorTests, returningWhileRemoteCallbackParksLeavesReturnPointLast)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    OSXScreenCursorTestAccess::startOwner(screen);
    ASSERT_TRUE(screen.leave());
    modelPointerPosition = true;
    modelPointer = CGPointMake(1500, 800);
    std::promise<void> parked, release;
    auto gate = release.get_future().share();
    std::atomic<bool> paused{false};
    beforeWarp = [&] {
        if (!paused.exchange(true)) { parked.set_value(); gate.wait(); }
    };
    CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointMake(1500, 800), kCGMouseButtonLeft);
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, 0);
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, 10);
    auto callback = std::async(std::launch::async, [&] {
        return OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, event);
    });
    if (parked.get_future().wait_for(std::chrono::seconds(1)) != std::future_status::ready) {
        release.set_value();
        callback.get();
        CFRelease(event);
        FAIL() << "remote callback did not reach the modeled parking operation";
    }
    auto returning = std::async(std::launch::async, [&] { return screen.tryEnterAt(2543, 600); });
    EXPECT_EQ(std::future_status::timeout, returning.wait_for(std::chrono::milliseconds(20)));
    // Release the native boundary; the serialized return must be last.
    release.set_value();
    EXPECT_EQ(nullptr, callback.get());
    EXPECT_TRUE(returning.get());
    beforeWarp = {};
    EXPECT_TRUE(OSXScreenCursorTestAccess::onScreen(screen));
    ASSERT_FALSE(warps.empty());
    EXPECT_EQ(2543, warps.back().x);
    EXPECT_EQ(600, warps.back().y);
    CFRelease(event);
}

TEST_F(OSXScreenCursorTests, failedReturnWarpRestoresCaptureButDoesNotAcceptPositionedReturn)
{
    CursorRecoveryQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    ASSERT_TRUE(screen.leave());
    warpResult = kCGErrorFailure;
    EXPECT_FALSE(screen.tryEnterAt(2543, 600));
    EXPECT_EQ(std::vector<bool>({false, true}), associations);
    EXPECT_EQ(1, events.errors);
    EXPECT_EQ(screen.getEventTarget(), events.errorTarget);
    EXPECT_EQ(0, events.quits);
    EXPECT_FALSE(OSXScreenCursorTestAccess::hidden(screen));
}

TEST_F(OSXScreenCursorTests, primaryParksBeforeHidingAtTheSwitchEdge)
{
    CursorRecoveryQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    ASSERT_TRUE(screen.leave());
    EXPECT_EQ(1, warpsAtHide);
    EXPECT_EQ(1, hideCalls);
    screen.enter();
    EXPECT_EQ(1, showCalls);
}

TEST_F(OSXScreenCursorTests, failedInitialParkingKeepsLocalControl)
{
    CursorRecoveryQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    warpResult = kCGErrorFailure;
    EXPECT_FALSE(screen.leave());
    EXPECT_TRUE(OSXScreenCursorTestAccess::onScreen(screen));
    EXPECT_TRUE(associations.empty());
    EXPECT_EQ(0, hideCalls);
}

TEST_F(OSXScreenCursorTests, failedRemoteParkingAndRestoreDoesNotClaimLocalOwnership)
{
    CursorMotionQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    ASSERT_TRUE(screen.leave());
    modelPointerPosition = true;
    modelPointer = CGPointMake(1500, 800);
    CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointMake(1500, 800), kCGMouseButtonLeft);
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, 0);
    warpResult = attachResult = kCGErrorFailure;
    OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, event);
    EXPECT_EQ(1, events.errors);
    EXPECT_EQ(screen.getEventTarget(), events.errorTarget);
    EXPECT_EQ(0, events.quits);
    EXPECT_FALSE(OSXScreenCursorTestAccess::onScreen(screen));
    CFRelease(event);
}

TEST_F(OSXScreenCursorTests, localPointerRetainsAbsoluteMotionAndOwnReplayIsNotCaptured)
{
    CursorMotionQueue events;
    OSXScreen screen(&events, false, false);
    OSXScreenCursorTestAccess::primary(screen, true);
    CGEventRef event = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, CGPointMake(0, 0), kCGMouseButtonLeft);
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, 0);
    CGEventSetLocation(event, CGPointMake(1280, 720));
    EXPECT_EQ(event, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, event));
    ASSERT_EQ(1u, events.local.size());
    EXPECT_EQ(std::make_pair(SInt32(1280), SInt32(720)), events.local[0]);
    CGEventSetIntegerValueField(event, kCGEventSourceUnixProcessID, getpid());
    CGEventSetLocation(event, CGPointMake(2559, 720));
    EXPECT_EQ(event, OSXScreenCursorTestAccess::primaryEvent(screen, kCGEventMouseMoved, event));
    EXPECT_EQ(1u, events.local.size());
    EXPECT_TRUE(events.remote.empty());
    EXPECT_EQ(0, warpCalls);
    CFRelease(event);
}

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

TEST_F(OSXScreenCursorTests, permanentlyDisabledPrimaryTapRestoresLocalPointerAndStopsSharing)
{
    CursorRecoveryQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    OSXScreenCursorTestAccess::startOwner(screen);
    ASSERT_TRUE(screen.leave());
    ASSERT_FALSE(OSXScreenCursorTestAccess::onScreen(screen));
    ASSERT_TRUE(OSXScreenCursorTestAccess::hidden(screen));

    EXPECT_EQ(nullptr, OSXScreenCursorTestAccess::primaryEvent(
        screen, kCGEventTapDisabledByUserInput, nullptr));

    EXPECT_EQ(std::vector<bool>({false, true}), associations);
    EXPECT_FALSE(OSXScreenCursorTestAccess::hidden(screen));
    EXPECT_TRUE(OSXScreenCursorTestAccess::onScreen(screen));
    EXPECT_EQ(1, events.errors);
    EXPECT_EQ(screen.getEventTarget(), events.errorTarget);
    EXPECT_EQ(0, events.quits);
}

TEST_F(OSXScreenCursorTests, permanentlyDisabledPrimaryTapDoesNotClaimFailedPointerRestoration)
{
    CursorRecoveryQueue events;
    CursorOwnershipScreen screen(&events);
    OSXScreenCursorTestAccess::primary(screen, true);
    OSXScreenCursorTestAccess::startOwner(screen);
    ASSERT_TRUE(screen.leave());
    attachResult = kCGErrorFailure;

    EXPECT_EQ(nullptr, OSXScreenCursorTestAccess::primaryEvent(
        screen, kCGEventTapDisabledByUserInput, nullptr));

    EXPECT_EQ(std::vector<bool>({false, true}), associations);
    EXPECT_FALSE(OSXScreenCursorTestAccess::onScreen(screen));
    EXPECT_EQ(1, events.errors);
    EXPECT_EQ(screen.getEventTarget(), events.errorTarget);
    EXPECT_EQ(0, events.quits);
    // The business-loop fatal-error path disables input and retries restoration.
    attachResult = kCGErrorSuccess;
    screen.disable();
    EXPECT_EQ(std::vector<bool>({false, true, true}), associations);
    EXPECT_FALSE(OSXScreenCursorTestAccess::hidden(screen));
    EXPECT_TRUE(OSXScreenCursorTestAccess::onScreen(screen));
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
