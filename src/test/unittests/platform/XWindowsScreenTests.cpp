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
