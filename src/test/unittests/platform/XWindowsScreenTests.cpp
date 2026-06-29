#include "test/global/gtest.h"

#include "platform/XWindowsScreen.h"

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
