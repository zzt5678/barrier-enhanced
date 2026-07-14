#include "test/global/gtest.h"

#include "platform/XWindowsScreen.h"

#ifdef HAVE_XI2
TEST(XWindowsScreenTests, xInputCookieUsableForTest_rejectsNullEventData)
{
	XGenericEventCookie cookie = {};
	cookie.type = GenericEvent;
	cookie.extension = 23;
	cookie.data = NULL;

	EXPECT_FALSE(XWindowsScreen::xInputCookieUsableForTest(cookie, 23));
}

TEST(XWindowsScreenTests, xInputCookieUsableForTest_rejectsUnrelatedEvents)
{
	int eventData = 0;
	XGenericEventCookie cookie = {};
	cookie.type = ButtonPress;
	cookie.extension = 23;
	cookie.data = &eventData;
	EXPECT_FALSE(XWindowsScreen::xInputCookieUsableForTest(cookie, 23));

	cookie.type = GenericEvent;
	cookie.extension = 24;
	EXPECT_FALSE(XWindowsScreen::xInputCookieUsableForTest(cookie, 23));
}

TEST(XWindowsScreenTests, xInputCookieUsableForTest_acceptsMatchingCookieWithData)
{
	int eventData = 0;
	XGenericEventCookie cookie = {};
	cookie.type = GenericEvent;
	cookie.extension = 23;
	cookie.data = &eventData;

	EXPECT_TRUE(XWindowsScreen::xInputCookieUsableForTest(cookie, 23));
}

TEST(XWindowsScreenTests, xInputEventPayload_rawMotionDoesNotNeedCookieData)
{
	EXPECT_FALSE(
		XWindowsScreen::xInputEventNeedsPayloadForTest(XI_RawMotion));
	EXPECT_TRUE(
		XWindowsScreen::xInputEventNeedsPayloadForTest(XI_RawButtonPress));
	EXPECT_TRUE(
		XWindowsScreen::xInputEventNeedsPayloadForTest(XI_RawButtonRelease));
}
#endif

TEST(XWindowsScreenTests, clampPointToRectForTest_insideRect_keepsPoint)
{
	SInt32 x = 5200;
	SInt32 y = 1200;

	EXPECT_TRUE(XWindowsScreen::clampPointToRectForTest(5120, 0, 5120, 2880, x, y));
	EXPECT_EQ(5200, x);
	EXPECT_EQ(1200, y);
}

TEST(XWindowsScreenTests, clampPointToRectForTest_outsideRect_clampsToVisibleEdge)
{
	SInt32 x = 6;
	SInt32 y = 2558;

	EXPECT_TRUE(XWindowsScreen::clampPointToRectForTest(5120, 0, 5120, 2880, x, y));
	EXPECT_EQ(5120, x);
	EXPECT_EQ(2558, y);
}

TEST(XWindowsScreenTests, clampPointToRectForTest_invalidRect_returnsFalse)
{
	SInt32 x = 6;
	SInt32 y = 2558;

	EXPECT_FALSE(XWindowsScreen::clampPointToRectForTest(5120, 0, 0, 2880, x, y));
	EXPECT_EQ(6, x);
	EXPECT_EQ(2558, y);
}

TEST(XWindowsScreenTests, adjustPointToVisibleAreaForTest_dualOutputsKeepsLeftOutputPoint)
{
	XWindowsScreen::VisibleAreas areas;
	areas.push_back(XWindowsScreen::VisibleArea(0, 0, 5120, 2880, false));
	areas.push_back(XWindowsScreen::VisibleArea(5120, 0, 5120, 2880, true));
	SInt32 x = 40;
	SInt32 y = 2709;

	EXPECT_TRUE(XWindowsScreen::adjustPointToVisibleAreaForTest(areas, x, y));
	EXPECT_EQ(40, x);
	EXPECT_EQ(2709, y);
}

TEST(XWindowsScreenTests, adjustPointToVisibleAreaForTest_singleRightOutputRemapsStaleLeftPoint)
{
	XWindowsScreen::VisibleAreas areas;
	areas.push_back(XWindowsScreen::VisibleArea(5120, 0, 5120, 2880, true));
	SInt32 x = 40;
	SInt32 y = 2709;

	EXPECT_TRUE(XWindowsScreen::adjustPointToVisibleAreaForTest(areas, x, y));
	EXPECT_EQ(5120, x);
	EXPECT_EQ(2709, y);
}

TEST(XWindowsScreenTests, adjustPointToVisibleAreaForTest_outsideOutputsUsesNearestVisibleOutput)
{
	XWindowsScreen::VisibleAreas areas;
	areas.push_back(XWindowsScreen::VisibleArea(0, 0, 5120, 2880, false));
	areas.push_back(XWindowsScreen::VisibleArea(5120, 0, 5120, 2880, true));
	SInt32 x = 12000;
	SInt32 y = 4000;

	EXPECT_TRUE(XWindowsScreen::adjustPointToVisibleAreaForTest(areas, x, y));
	EXPECT_EQ(10239, x);
	EXPECT_EQ(2879, y);
}

TEST(XWindowsScreenTests, adjustPointToVisibleAreaForTest_noValidOutputsReturnsFalse)
{
	XWindowsScreen::VisibleAreas areas;
	areas.push_back(XWindowsScreen::VisibleArea(5120, 0, 0, 2880, true));
	SInt32 x = 40;
	SInt32 y = 2709;

	EXPECT_FALSE(XWindowsScreen::adjustPointToVisibleAreaForTest(areas, x, y));
	EXPECT_EQ(40, x);
	EXPECT_EQ(2709, y);
}

TEST(XWindowsScreenTests, adjustPointToVisibleAreaNearAnchorForTest_usesAnchorOutput)
{
	XWindowsScreen::VisibleAreas areas;
	areas.push_back(XWindowsScreen::VisibleArea(0, 0, 5120, 2880, false));
	areas.push_back(XWindowsScreen::VisibleArea(5120, 0, 5120, 2880, true));
	SInt32 x = 40;
	SInt32 y = 900;

	EXPECT_TRUE(XWindowsScreen::adjustPointToVisibleAreaNearAnchorForTest(
		areas, 8000, 1200, x, y));
	EXPECT_EQ(5120, x);
	EXPECT_EQ(900, y);
}

TEST(XWindowsScreenTests, adjustPointToVisibleAreaNearAnchorForTest_closedAnchorOutputFallsBackToVisible)
{
	XWindowsScreen::VisibleAreas areas;
	areas.push_back(XWindowsScreen::VisibleArea(5120, 0, 5120, 2880, true));
	SInt32 x = 40;
	SInt32 y = 900;

	EXPECT_TRUE(XWindowsScreen::adjustPointToVisibleAreaNearAnchorForTest(
		areas, 100, 1200, x, y));
	EXPECT_EQ(5120, x);
	EXPECT_EQ(900, y);
}

TEST(XWindowsScreenTests, visibleAreaTopologiesEqualForTest_ignoresOutputOrder)
{
	XWindowsScreen::VisibleAreas first;
	first.push_back(XWindowsScreen::VisibleArea(0, 0, 1920, 1080, true));
	first.push_back(XWindowsScreen::VisibleArea(1920, 0, 2560, 1440, false));
	XWindowsScreen::VisibleAreas second;
	second.push_back(XWindowsScreen::VisibleArea(1920, 0, 2560, 1440, false));
	second.push_back(XWindowsScreen::VisibleArea(0, 0, 1920, 1080, true));

	EXPECT_TRUE(XWindowsScreen::visibleAreaTopologiesEqualForTest(first, second));
}

TEST(XWindowsScreenTests, visibleAreaTopologiesEqualForTest_detectsPrimaryOutputChange)
{
	XWindowsScreen::VisibleAreas first;
	first.push_back(XWindowsScreen::VisibleArea(0, 0, 1920, 1080, true));
	first.push_back(XWindowsScreen::VisibleArea(1920, 0, 1920, 1080, false));
	XWindowsScreen::VisibleAreas second;
	second.push_back(XWindowsScreen::VisibleArea(0, 0, 1920, 1080, false));
	second.push_back(XWindowsScreen::VisibleArea(1920, 0, 1920, 1080, true));

	EXPECT_FALSE(XWindowsScreen::visibleAreaTopologiesEqualForTest(first, second));
}

TEST(XWindowsScreenTests, visibleAreaTopologiesEqualForTest_detectsGeometryChange)
{
	XWindowsScreen::VisibleAreas first;
	first.push_back(XWindowsScreen::VisibleArea(0, 0, 1920, 1080, true));
	XWindowsScreen::VisibleAreas second;
	second.push_back(XWindowsScreen::VisibleArea(0, 0, 1920, 1200, true));

	EXPECT_FALSE(XWindowsScreen::visibleAreaTopologiesEqualForTest(first, second));
}
