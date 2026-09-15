#include "platform/OSXScreen.h"
#include "base/EventQueue.h"
#include "test/global/gtest.h"
#include <dlfcn.h>
#include <limits>
#include <thread>

namespace {
bool scripted = false;
int queries = 0;
CGError queryResult = kCGErrorSuccess;
CGDisplayCount count = 1, returnedCount = 1;
CGRect bounds = CGRectMake(0, 0, 1710, 1107);
CGDisplayReconfigurationCallBack callback = nullptr;
void* callbackContext = nullptr;
bool beginDuringBounds = false;
template<class T> T native(const char* name) { return reinterpret_cast<T>(dlsym(RTLD_NEXT, name)); }
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
    // A private inert port exercises enable/disable without installing an input tap.
    return CFMachPortCreate(nullptr, [](CFMachPortRef, void*, CFIndex, void*) {}, nullptr, nullptr);
}
extern "C" void CGEventTapEnable(CFMachPortRef port, bool enabled)
{
    if (!scripted) native<decltype(&CGEventTapEnable)>("CGEventTapEnable")(port, enabled);
}

class OSXScreenShapeTestAccess {
public:
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
    void fakeMouseMove(SInt32, SInt32) override { ++warps; }
    int warps = 0;
};
class OSXScreenShapeTests : public testing::Test {
protected:
    void SetUp() override {
        scripted = true; queries = 0; queryResult = kCGErrorSuccess;
        count = returnedCount = 1; bounds = CGRectMake(0, 0, 1710, 1107);
        beginDuringBounds = false;
    }
    void TearDown() override { scripted = false; }
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
