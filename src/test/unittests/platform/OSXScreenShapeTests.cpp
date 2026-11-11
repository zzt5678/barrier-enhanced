#include "platform/OSXScreen.h"
#include "barrier/XScreen.h"
#include "base/EventQueue.h"
#include "test/global/gtest.h"
#include <dlfcn.h>
#include <limits>
#include <thread>
#include <vector>
#include <functional>
#include <mach/mach_time.h>

void configureCursorModelForTapTest(CGPoint, CGError, std::function<void()>);
void resetCursorModelForTapTest();

namespace {
bool scripted = false;
int queries = 0;
CGError queryResult = kCGErrorSuccess;
CGDisplayCount count = 1, returnedCount = 1;
CGRect bounds = CGRectMake(0, 0, 1710, 1107);
CGDisplayReconfigurationCallBack callback = nullptr;
void* callbackContext = nullptr;
bool beginDuringBounds = false;
bool denyAccessibility = false, failTap = false, failTapSource = false;
int tapCalls = 0, sourceCalls = 0, failTapAt = 0, failSourceAt = 0;
struct TapState {
    CFMachPortRef port;
    CGEventTapOptions options;
    bool enabled;
    CGEventTapCallBack callback;
    void* context;
};
std::vector<TapState> taps;
template<class T> T native(const char* name) { return reinterpret_cast<T>(dlsym(RTLD_NEXT, name)); }
}
extern "C" Boolean AXIsProcessTrusted()
{
    return scripted ? !denyAccessibility : native<decltype(&AXIsProcessTrusted)>("AXIsProcessTrusted")();
}

extern "C" CGError CGGetActiveDisplayList(uint32_t max, CGDirectDisplayID* ids, uint32_t* out)
{
    if (!scripted) return native<decltype(&CGGetActiveDisplayList)>("CGGetActiveDisplayList")(max, ids, out);
    ++queries;
    *out = max ? returnedCount : count;
    if (max && returnedCount) ids[0] = 1;
    return queryResult;
}
extern "C" CGRect CGDisplayBounds(CGDirectDisplayID id)
{
    if (scripted && beginDuringBounds) {
        beginDuringBounds = false;
        std::thread transition([] { callback(1, kCGDisplayBeginConfigurationFlag, callbackContext); });
        transition.join();
    }
    return scripted ? bounds : native<decltype(&CGDisplayBounds)>("CGDisplayBounds")(id);
}
extern "C" CGDirectDisplayID CGMainDisplayID()
{
    return scripted ? 1 : native<decltype(&CGMainDisplayID)>("CGMainDisplayID")();
}
extern "C" CGError CGDisplayRegisterReconfigurationCallback(CGDisplayReconfigurationCallBack cb, void* context)
{
    if (!scripted) return native<decltype(&CGDisplayRegisterReconfigurationCallback)>("CGDisplayRegisterReconfigurationCallback")(cb, context);
    callback = cb; callbackContext = context;
    return kCGErrorSuccess;
}
extern "C" CGError CGDisplayRemoveReconfigurationCallback(CGDisplayReconfigurationCallBack cb, void* context)
{
    if (!scripted) return native<decltype(&CGDisplayRemoveReconfigurationCallback)>("CGDisplayRemoveReconfigurationCallback")(cb, context);
    callback = nullptr; callbackContext = nullptr;
    return kCGErrorSuccess;
}
extern "C" CFMachPortRef CGEventTapCreate(CGEventTapLocation location, CGEventTapPlacement placement,
    CGEventTapOptions options, CGEventMask mask, CGEventTapCallBack cb, void* context)
{
    if (!scripted) return native<decltype(&CGEventTapCreate)>("CGEventTapCreate")(location, placement, options, mask, cb, context);
    if (failTap || ++tapCalls == failTapAt) return nullptr;
    // A private inert port exercises enable/disable without installing an input tap.
    auto port = CFMachPortCreate(nullptr, [](CFMachPortRef, void*, CFIndex, void*) {}, nullptr, nullptr);
    taps.push_back({port, options, true, cb, context});
    return port;
}
extern "C" CFRunLoopSourceRef CFMachPortCreateRunLoopSource(CFAllocatorRef allocator, CFMachPortRef port, CFIndex order)
{
    if (scripted && (failTapSource || ++sourceCalls == failSourceAt)) return nullptr;
    return native<decltype(&CFMachPortCreateRunLoopSource)>("CFMachPortCreateRunLoopSource")(allocator, port, order);
}
extern "C" void CGEventTapEnable(CFMachPortRef port, bool enabled)
{
    if (!scripted) native<decltype(&CGEventTapEnable)>("CGEventTapEnable")(port, enabled);
    else for (auto& tap : taps) if (tap.port == port) tap.enabled = enabled;
}

class OSXScreenShapeTestAccess {
public:
    static void primary(OSXScreen& screen) { screen.m_isPrimary = true; screen.m_isOnScreen = true; }
    static void onOwner(OSXScreen& screen, std::function<void()> operation) {
        ASSERT_TRUE(screen.m_inputLoop->invoke(std::move(operation)));
    }
    static void capture(OSXScreen& screen, bool remote) {
        screen.m_inputLoop->invoke([&screen, remote] {
            screen.m_isOnScreen = !remote;
            screen.setInputCapture(remote);
        });
    }
    static bool hasInputResources(const OSXScreen& screen) {
        return screen.m_inputLoop || screen.m_eventTapPort ||
            screen.m_eventTapRLSR || screen.m_localEventTapPort || screen.m_localEventTapSource || screen.m_clipboardTimer;
    }
    static void tick(OSXScreen& screen) { screen.handleClipboardCheck(Event(), nullptr); }
    static void refresh(OSXScreen& screen) { screen.updateScreenShape(); }
    static void expireConfiguration(OSXScreen& screen) {
        std::lock_guard<std::mutex> lock(screen.m_shapeMutex);
        screen.m_displayConfigurationStarted -= std::chrono::seconds(3);
    }
};
class ShapeScreen : public OSXScreen {
public:
    explicit ShapeScreen(EventQueue& events) : OSXScreen(&events, false, false) {
        // Initialize the actual native queue buffer and flush constructor events.
        events.addEvent(Event(Event::kQuit));
        events.loop();
    }
    void checkClipboards() override {} // Never touch the native test host clipboard.
    bool isDraggingStarted() override { return false; }
    void fakeMouseMove(SInt32, SInt32) override { ++warps; }
    int warps = 0;
};
class OSXScreenShapeTests : public testing::Test {
protected:
    void SetUp() override {
        scripted = true; queries = 0; queryResult = kCGErrorSuccess;
        count = returnedCount = 1; bounds = CGRectMake(0, 0, 1710, 1107);
        beginDuringBounds = false;
        denyAccessibility = failTap = failTapSource = false;
        tapCalls = sourceCalls = failTapAt = failSourceAt = 0; taps.clear();
    }
    void TearDown() override { resetCursorModelForTapTest(); scripted = false; }
    static void shape(OSXScreen& screen, int width, int height) {
        SInt32 x, y, w, h; screen.getShape(x, y, w, h);
        EXPECT_EQ(width, w); EXPECT_EQ(height, h);
    }
    static int drain(EventQueue& events) {
        int changed = 0; Event event;
        while (events.getEvent(event, 0)) {
            if (event.getType() == events.forIScreen().shapeChanged()) ++changed;
            events.dispatchEvent(event); Event::deleteData(event);
        }
        return changed;
    }
};

TEST_F(OSXScreenShapeTests, deniedAccessibilityIsPermanentInputFailureWithReason)
{
    EventQueue events; denyAccessibility = true;
    try {
        OSXScreen screen(&events, true, false);
        FAIL() << "permission denial must reject primary startup";
    }
    catch (const XInputUnavailable& error) {
        EXPECT_NE(std::string::npos, std::string(error.what()).find("system settings"));
    }
    catch (const std::exception& error) {
        ADD_FAILURE() << "wrong startup classification: " << error.what();
    }
}

TEST_F(OSXScreenShapeTests, failedPrimaryTapCreationReleasesOwnerAndTimerBeforeThrow)
{
    EventQueue events; ShapeScreen screen(events);
    OSXScreenShapeTestAccess::primary(screen); failTap = true;
    EXPECT_THROW(screen.enable(), XInputUnavailable);
    EXPECT_FALSE(OSXScreenShapeTestAccess::hasInputResources(screen));
}

TEST_F(OSXScreenShapeTests, failedPrimaryTapSourceReleasesOwnerPortAndTimerBeforeThrow)
{
    EventQueue events; ShapeScreen screen(events);
    OSXScreenShapeTestAccess::primary(screen); failTapSource = true;
    EXPECT_THROW(screen.enable(), XInputUnavailable);
    EXPECT_FALSE(OSXScreenShapeTestAccess::hasInputResources(screen));
}

TEST_F(OSXScreenShapeTests, localInputBypassesFilterAndRemoteOwnershipEnablesIt)
{
    EventQueue events; ShapeScreen screen(events);
    OSXScreenShapeTestAccess::primary(screen);
    screen.enable();
    ASSERT_EQ(2u, taps.size());
    EXPECT_EQ(kCGEventTapOptionDefault, taps[0].options);
    EXPECT_FALSE(taps[0].enabled);
    EXPECT_EQ(kCGEventTapOptionListenOnly, taps[1].options);
    EXPECT_TRUE(taps[1].enabled);
    OSXScreenShapeTestAccess::capture(screen, true);
    EXPECT_TRUE(taps[0].enabled);
    EXPECT_FALSE(taps[1].enabled);
    OSXScreenShapeTestAccess::capture(screen, false);
    EXPECT_FALSE(taps[0].enabled);
    EXPECT_TRUE(taps[1].enabled);
    screen.disable();
    EXPECT_FALSE(OSXScreenShapeTestAccess::hasInputResources(screen));
}

TEST_F(OSXScreenShapeTests, firstWarpBearingMotionSurvivesLocalToRemoteTapCutover)
{
    // Physical capture, 2026-11-10: the first event contains the -1279,+32
    // parking offset. Losing it turns the following +1 into a +1280 jump.
    bounds = CGRectMake(0, 0, 2560, 1440);
    configureCursorModelForTapTest(CGPointMake(2559, 688), kCGErrorSuccess, {});
    EventQueue events;
    ShapeScreen screen(events);
    OSXScreenShapeTestAccess::primary(screen);
    screen.enable();
    ASSERT_EQ(2u, taps.size());
    CGEventRef parkedMotion = nullptr;
    std::vector<TapState> recipients;
    configureCursorModelForTapTest(CGPointMake(2559, 688), kCGErrorSuccess, [&] {
        if (parkedMotion) return;
        parkedMotion = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved,
            CGPointMake(1280, 720), kCGMouseButtonLeft);
        CGEventSetIntegerValueField(parkedMotion, kCGEventSourceStateID, kCGEventSourceStateHIDSystemState);
        CGEventSetIntegerValueField(parkedMotion, kCGEventSourceUnixProcessID, 0);
        CGEventSetIntegerValueField(parkedMotion, kCGMouseEventDeltaX, -1278);
        CGEventSetIntegerValueField(parkedMotion, kCGMouseEventDeltaY, 32);
        CGEventSetTimestamp(parkedMotion, mach_absolute_time());
        for (const auto& tap : taps) if (tap.enabled) recipients.push_back(tap);
    });
    const bool left = screen.leave();
    configureCursorModelForTapTest(CGPointMake(1280, 720), kCGErrorSuccess, {});
    ASSERT_TRUE(left);
    ASSERT_NE(nullptr, parkedMotion);
    OSXScreenShapeTestAccess::onOwner(screen, [&] {
        // Native delivery selected its recipients before leave returned.
        for (const auto& tap : recipients)
            tap.callback(nullptr, kCGEventMouseMoved, parkedMotion, tap.context);
    });
    CFRelease(parkedMotion);
    CGEventRef next = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved,
        CGPointMake(1280, 720), kCGMouseButtonLeft);
    CGEventSetIntegerValueField(next, kCGEventSourceStateID, kCGEventSourceStateHIDSystemState);
    CGEventSetIntegerValueField(next, kCGEventSourceUnixProcessID, 0);
    CGEventSetIntegerValueField(next, kCGMouseEventDeltaX, 1);
    CGEventSetTimestamp(next, mach_absolute_time());
    OSXScreenShapeTestAccess::onOwner(screen, [&] {
        for (const auto& tap : taps) if (tap.enabled)
            tap.callback(nullptr, kCGEventMouseMoved, next, tap.context);
    });
    CFRelease(next);
    std::vector<std::pair<SInt32, SInt32>> motion;
    Event event;
    while (events.getEvent(event, 0)) {
        if (event.getType() == events.forIPrimaryScreen().motionOnSecondary()) {
            const auto* point = static_cast<const IPrimaryScreen::MotionInfo*>(event.getData());
            motion.emplace_back(point->m_x, point->m_y);
        }
        Event::deleteData(event);
    }
    EXPECT_EQ((std::vector<std::pair<SInt32, SInt32>>{{1, 0}, {1, 0}}), motion);
    screen.disable();
}

TEST_F(OSXScreenShapeTests, failedInitialParkRestoresPassiveLocalTap)
{
    configureCursorModelForTapTest(CGPointMake(2559, 688), kCGErrorFailure, {});
    EventQueue events;
    ShapeScreen screen(events);
    OSXScreenShapeTestAccess::primary(screen);
    screen.enable();
    ASSERT_FALSE(screen.leave());
    ASSERT_EQ(2u, taps.size());
    EXPECT_FALSE(taps[0].enabled);
    EXPECT_TRUE(taps[1].enabled);
    screen.disable();
}

TEST_F(OSXScreenShapeTests, failedLocalObserverCreationReleasesActiveTapAndOwner)
{
    EventQueue events; ShapeScreen screen(events);
    OSXScreenShapeTestAccess::primary(screen); failTapAt = 2;
    EXPECT_THROW(screen.enable(), XInputUnavailable);
    EXPECT_FALSE(OSXScreenShapeTestAccess::hasInputResources(screen));
}

TEST_F(OSXScreenShapeTests, failedLocalObserverSourceReleasesBothTapsAndOwner)
{
    EventQueue events; ShapeScreen screen(events);
    OSXScreenShapeTestAccess::primary(screen); failSourceAt = 2;
    EXPECT_THROW(screen.enable(), XInputUnavailable);
    EXPECT_FALSE(OSXScreenShapeTestAccess::hasInputResources(screen));
}

TEST_F(OSXScreenShapeTests, secondaryCursorObservationDoesNotBlockNativeInputDelivery)
{
    EventQueue events;
    ShapeScreen screen(events);
    screen.enable();
    ASSERT_EQ(1u, taps.size());
    // The client only restores cursor visibility for local activity. It
    // never consumes an event, and injected motion must not wait for this
    // helper's Cocoa run loop before reaching the rest of the system.
    EXPECT_EQ(kCGEventTapOptionListenOnly, taps[0].options);
    EXPECT_TRUE(taps[0].enabled);
    screen.disable();
    EXPECT_FALSE(OSXScreenShapeTestAccess::hasInputResources(screen));
}

TEST_F(OSXScreenShapeTests, failedSecondaryTapCreationReleasesTimerBeforeThrow)
{
    EventQueue events; ShapeScreen screen(events); failTap = true;
    EXPECT_THROW(screen.enable(), XInputUnavailable);
    EXPECT_FALSE(OSXScreenShapeTestAccess::hasInputResources(screen));
}

TEST_F(OSXScreenShapeTests, failedInitialQueryIsUnavailableAndExistingTickRecovers)
{
    EventQueue events; queryResult = kCGErrorFailure; ShapeScreen screen(events);
    shape(screen, 0, 0); drain(events);
    queryResult = kCGErrorSuccess;
    OSXScreenShapeTestAccess::tick(screen);
    shape(screen, 1710, 1107); EXPECT_EQ(1, drain(events));
    const int validQueries = queries;
    OSXScreenShapeTestAccess::tick(screen);
    EXPECT_EQ(validQueries, queries); EXPECT_EQ(0, drain(events));
}
TEST_F(OSXScreenShapeTests, enumerationRaceAndMissingDisplaysClearPreviouslyValidShape)
{
    EventQueue events; ShapeScreen screen(events); drain(events);
    returnedCount = 0; OSXScreenShapeTestAccess::refresh(screen);
    shape(screen, 0, 0); EXPECT_EQ(1, drain(events));
    count = 0; OSXScreenShapeTestAccess::tick(screen);
    shape(screen, 0, 0); EXPECT_EQ(0, drain(events));
    count = returnedCount = 1; OSXScreenShapeTestAccess::tick(screen);
    shape(screen, 1710, 1107); EXPECT_EQ(1, drain(events));
}
TEST_F(OSXScreenShapeTests, beginDoesNotQueryAndCompletionWithoutFlagsRefreshesOnEventThread)
{
    EventQueue events; ShapeScreen screen(events); drain(events);
    const int before = queries;
    callback(1, kCGDisplayBeginConfigurationFlag, callbackContext);
    EXPECT_EQ(before, queries);
    shape(screen, 0, 0);
    drain(events); OSXScreenShapeTestAccess::tick(screen);
    EXPECT_EQ(before, queries);
    bounds = CGRectMake(0, 0, 1920, 1080);
    callback(1, 0, callbackContext);
    EXPECT_EQ(before, queries); // Native callback must not mutate the cached geometry.
    drain(events); shape(screen, 1920, 1080);
}
TEST_F(OSXScreenShapeTests, enableRefreshesAndNeverWarpsWhenGeometryIsUnavailable)
{
    EventQueue events; ShapeScreen screen(events); drain(events);
    queryResult = kCGErrorFailure;
    screen.enable(); shape(screen, 0, 0); EXPECT_EQ(0, screen.warps); screen.disable();
    queryResult = kCGErrorSuccess; bounds = CGRectMake(0, 0, 1920, 1080);
    screen.enable(); shape(screen, 1920, 1080); EXPECT_EQ(1, screen.warps); screen.disable();
}
TEST_F(OSXScreenShapeTests, sameSizeCompletionNotifiesAfterTransientUnavailableQuery)
{
    EventQueue events; ShapeScreen screen(events);
    callback(1, kCGDisplayBeginConfigurationFlag, callbackContext);
    shape(screen, 0, 0);
    callback(1, 0, callbackContext);
    EXPECT_EQ(1, drain(events)); shape(screen, 1710, 1107);
}
TEST_F(OSXScreenShapeTests, zeroAndNonfiniteBoundsAreUnavailableAndRetryable)
{
    EventQueue events; ShapeScreen screen(events); drain(events);
    bounds = CGRectZero; OSXScreenShapeTestAccess::refresh(screen); shape(screen, 0, 0);
    bounds = CGRectMake(0, 0, 1, 1);
    OSXScreenShapeTestAccess::tick(screen); shape(screen, 0, 0);
    bounds = CGRectMake(0, 0, std::numeric_limits<double>::infinity(), 100);
    OSXScreenShapeTestAccess::tick(screen); shape(screen, 0, 0);
    bounds = CGRectMake(0, 0, 1710, 1107);
    OSXScreenShapeTestAccess::tick(screen); shape(screen, 1710, 1107);
}
TEST_F(OSXScreenShapeTests, missingCompletionRecoversAfterBoundedSettlingInterval)
{
    EventQueue events; ShapeScreen screen(events);
    const int before = queries;
    callback(1, kCGDisplayBeginConfigurationFlag, callbackContext);
    drain(events); OSXScreenShapeTestAccess::tick(screen);
    EXPECT_EQ(before, queries); shape(screen, 0, 0);
    OSXScreenShapeTestAccess::expireConfiguration(screen);
    OSXScreenShapeTestAccess::tick(screen);
    shape(screen, 1710, 1107); EXPECT_EQ(1, drain(events));
}
TEST_F(OSXScreenShapeTests, dimensionsBelowServerUsableThresholdRemainRetryable)
{
    EventQueue events; ShapeScreen screen(events);
    bounds = CGRectMake(0, 0, 63, 63);
    OSXScreenShapeTestAccess::refresh(screen); shape(screen, 0, 0);
    bounds = CGRectMake(0, 0, 64, 64);
    OSXScreenShapeTestAccess::tick(screen); shape(screen, 64, 64);
}
TEST_F(OSXScreenShapeTests, nativeCallbackDuringSamplingDiscardsCrossGenerationShape)
{
    EventQueue events; ShapeScreen screen(events);
    bounds = CGRectMake(0, 0, 1920, 1080); beginDuringBounds = true;
    OSXScreenShapeTestAccess::refresh(screen); shape(screen, 0, 0);
    callback(1, 0, callbackContext);
    drain(events); shape(screen, 1920, 1080);
}
