/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2011 Nick Bolton
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "test/mock/barrier/MockEventQueue.h"
#include "platform/XWindowsScreen.h"

#include "test/global/gtest.h"
#include <cstdlib>

using ::testing::_;

TEST(CXWindowsScreenTests, drmConnectorEnterable_allowsDpmsOffForWake)
{
    EXPECT_TRUE(XWindowsScreen::isDrmConnectorEnterableForTest(
        "connected", "enabled", "Off"));
    EXPECT_TRUE(XWindowsScreen::isDrmConnectorEnterableForTest(
        "connected", "", "Off"));
    EXPECT_FALSE(XWindowsScreen::isDrmConnectorEnterableForTest(
        "connected", "disabled", "On"));
    EXPECT_FALSE(XWindowsScreen::isDrmConnectorEnterableForTest(
        "disconnected", "enabled", "On"));
}

TEST(CXWindowsScreenTests, drmConnectorEnterable_distinguishesDpmsOffFromDisconnected)
{
    EXPECT_TRUE(XWindowsScreen::isDrmConnectorEnterableForTest(
        "connected", "enabled", "Off"));
    EXPECT_TRUE(XWindowsScreen::isDrmConnectorEnterableForTest(
        "connected", "", "Off"));
    EXPECT_FALSE(XWindowsScreen::isDrmConnectorEnterableForTest(
        "disconnected", "enabled", "Off"));
    EXPECT_FALSE(XWindowsScreen::isDrmConnectorEnterableForTest(
        "disconnected", "", "Off"));
	EXPECT_FALSE(XWindowsScreen::isDrmConnectorEnterableForTest(
		"connected", "disabled", "Off"));
}

TEST(CXWindowsScreenTests, primaryDisplayEnterable_rejectsValidXShapeWhenDrmIsUnusable)
{
	EXPECT_FALSE(XWindowsScreen::isPrimaryDisplayEnterableForTest(
		false, 1920, 1080));
	EXPECT_FALSE(XWindowsScreen::isPrimaryDisplayEnterableForTest(
		false, 64, 64));
}

TEST(CXWindowsScreenTests, primaryDisplayEnterable_rejectsInvalidShapeEvenWhenDrmIsUsable)
{
	EXPECT_FALSE(XWindowsScreen::isPrimaryDisplayEnterableForTest(
		false, 0, 1080));
	EXPECT_FALSE(XWindowsScreen::isPrimaryDisplayEnterableForTest(
		false, 1920, 0));
	EXPECT_FALSE(XWindowsScreen::isPrimaryDisplayEnterableForTest(
		false, 63, 1080));
	EXPECT_FALSE(XWindowsScreen::isPrimaryDisplayEnterableForTest(
		true, 0, 0));
	EXPECT_TRUE(XWindowsScreen::isPrimaryDisplayEnterableForTest(
		true, 64, 64));
}

TEST(CXWindowsScreenTests, secondaryDisplayAdvertisable_rejectsValidXShapeWhenDrmIsUnusable)
{
	EXPECT_FALSE(XWindowsScreen::isSecondaryDisplayAdvertisableForTest(
		false, 1920, 1080));
	EXPECT_FALSE(XWindowsScreen::isSecondaryDisplayAdvertisableForTest(
		false, 64, 64));
}

TEST(CXWindowsScreenTests, secondaryDisplayAdvertisable_rejectsInvalidShapeEvenWhenDrmIsUsable)
{
	EXPECT_FALSE(XWindowsScreen::isSecondaryDisplayAdvertisableForTest(
		false, 0, 1080));
	EXPECT_FALSE(XWindowsScreen::isSecondaryDisplayAdvertisableForTest(
		false, 1920, 0));
	EXPECT_FALSE(XWindowsScreen::isSecondaryDisplayAdvertisableForTest(
		false, 63, 1080));
	EXPECT_FALSE(XWindowsScreen::isSecondaryDisplayAdvertisableForTest(
		true, 0, 0));
	EXPECT_TRUE(XWindowsScreen::isSecondaryDisplayAdvertisableForTest(
		true, 64, 64));
}

TEST(CXWindowsScreenTests, fakeMouseMove_nonPrimary_getCursorPosValuesCorrect)
{
    const char* displayName = std::getenv("DISPLAY");
    if (displayName == NULL) {
        displayName = ":0.0";
    }

    MockEventQueue eventQueue;
    EXPECT_CALL(eventQueue, adoptHandler(_, _, _)).Times(2);
    EXPECT_CALL(eventQueue, adoptBuffer(_)).Times(2);
    EXPECT_CALL(eventQueue, removeHandler(_, _)).Times(2);
    XWindowsScreen screen(new XWindowsImpl(), displayName, false, false, 0, &eventQueue);

    screen.fakeMouseMove(10, 20);

    SInt32 x, y;
    screen.getCursorPos(x, y);
    ASSERT_EQ(10, x);
    ASSERT_EQ(20, y);
}
