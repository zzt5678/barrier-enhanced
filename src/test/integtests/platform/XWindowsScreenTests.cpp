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
#include "barrier/XScreen.h"
#include "base/IEventJob.h"
#include "base/IEventQueueBuffer.h"
#include "platform/XWindowsScreen.h"

#include "test/global/gtest.h"
#include <cstdlib>
#include <memory>
#include <vector>

using ::testing::_;
using ::testing::Invoke;
using ::testing::ReturnRef;

namespace {

int
testIOErrorHandler(Display*)
{
	return 0;
}

}

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

TEST(CXWindowsScreenTests, unavailableDisplay_canBeConstructedTwiceAfterFailure)
{
	const char* const invalidDisplay = ":65535";
	MockEventQueue eventQueue;
	XIOErrorHandler const previousIOErrorHandler =
		XSetIOErrorHandler(&testIOErrorHandler);

	EXPECT_THROW({
		XWindowsScreen screen(new XWindowsImpl(), invalidDisplay, true, true,
			0, &eventQueue);
	}, XScreenUnavailable);
	EXPECT_THROW({
		XWindowsScreen screen(new XWindowsImpl(), invalidDisplay, true, true,
			0, &eventQueue);
	}, XScreenUnavailable);

	XIOErrorHandler const restoredIOErrorHandler =
		XSetIOErrorHandler(previousIOErrorHandler);
	EXPECT_EQ(&testIOErrorHandler, restoredIOErrorHandler);
}

TEST(CXWindowsScreenTests, fakeMouseMove_nonPrimary_getCursorPosValuesCorrect)
{
    const char* displayName = std::getenv("DISPLAY");
    if (displayName == NULL) {
        displayName = ":0.0";
    }

    MockEventQueue eventQueue;
    std::vector<std::unique_ptr<IEventJob> > handlers;
    std::unique_ptr<IEventQueueBuffer> buffer;
    EXPECT_CALL(eventQueue, adoptHandler(_, _, _))
        .Times(2)
        .WillRepeatedly(Invoke([&](Event::Type, void*, IEventJob* handler) {
            handlers.emplace_back(handler);
        }));
    EXPECT_CALL(eventQueue, adoptBuffer(_))
        .Times(2)
        .WillRepeatedly(Invoke([&](IEventQueueBuffer* adoptedBuffer) {
            buffer.reset(adoptedBuffer);
        }));
    EXPECT_CALL(eventQueue, removeHandler(_, _)).Times(2);
    XWindowsScreen screen(new XWindowsImpl(), displayName, false, false, 0, &eventQueue);

    screen.fakeMouseMove(10, 20);

    SInt32 x, y;
    screen.getCursorPos(x, y);
    ASSERT_EQ(10, x);
    ASSERT_EQ(20, y);
}

TEST(CXWindowsScreenTests, primaryEnter_releasesPointerGrabBeforeReturning)
{
    const char* displayName = std::getenv("DISPLAY");
    if (displayName == NULL) {
        displayName = ":0.0";
    }

    Display* probeDisplay = XOpenDisplay(displayName);
    ASSERT_NE(static_cast<Display*>(NULL), probeDisplay);
    const int baselineGrabResult = XGrabPointer(probeDisplay,
        DefaultRootWindow(probeDisplay), False, PointerMotionMask,
        GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    if (baselineGrabResult == GrabSuccess) {
        XUngrabPointer(probeDisplay, CurrentTime);
        XSync(probeDisplay, False);
    }
    ASSERT_EQ(GrabSuccess, baselineGrabResult);

    MockEventQueue eventQueue;
    std::vector<std::unique_ptr<IEventJob> > handlers;
    std::unique_ptr<IEventQueueBuffer> buffer;
    EXPECT_CALL(eventQueue, adoptHandler(_, _, _))
        .Times(2)
        .WillRepeatedly(Invoke([&](Event::Type, void*, IEventJob* handler) {
            handlers.emplace_back(handler);
        }));
    EXPECT_CALL(eventQueue, adoptBuffer(_))
        .Times(2)
        .WillRepeatedly(Invoke([&](IEventQueueBuffer* adoptedBuffer) {
            buffer.reset(adoptedBuffer);
        }));
    EXPECT_CALL(eventQueue, removeHandler(_, _)).Times(2);
    XWindowsScreen screen(new XWindowsImpl(), displayName, true, false, 0,
        &eventQueue);

    ASSERT_TRUE(screen.leave());
    screen.enter();

    const int grabResult = XGrabPointer(probeDisplay,
        DefaultRootWindow(probeDisplay), False, PointerMotionMask,
        GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    if (grabResult == GrabSuccess) {
        XUngrabPointer(probeDisplay, CurrentTime);
    }
    XCloseDisplay(probeDisplay);

    EXPECT_EQ(GrabSuccess, grabResult);
}

TEST(CXWindowsScreenTests, offscreenRecenter_doesNotEmitReverseMotionAtCenter)
{
    const char* displayName = std::getenv("DISPLAY");
    if (displayName == NULL) {
        displayName = ":0.0";
    }

    Display* probeDisplay = XOpenDisplay(displayName);
    ASSERT_NE(static_cast<Display*>(NULL), probeDisplay);

    MockEventQueue eventQueue;
    IPrimaryScreenEvents primaryScreenEvents;
    primaryScreenEvents.setEvents(&eventQueue);
    ON_CALL(eventQueue, forIPrimaryScreen())
        .WillByDefault(ReturnRef(primaryScreenEvents));

    std::unique_ptr<IEventJob> systemHandler;
    std::vector<std::unique_ptr<IEventJob> > otherHandlers;
    std::unique_ptr<IEventQueueBuffer> buffer;
    EXPECT_CALL(eventQueue, adoptHandler(_, _, _))
        .Times(2)
        .WillRepeatedly(Invoke([&](Event::Type type, void*, IEventJob* handler) {
            if (type == Event::kSystem) {
                systemHandler.reset(handler);
            }
            else {
                otherHandlers.emplace_back(handler);
            }
        }));
    EXPECT_CALL(eventQueue, adoptBuffer(_))
        .Times(2)
        .WillRepeatedly(Invoke([&](IEventQueueBuffer* adoptedBuffer) {
            buffer.reset(adoptedBuffer);
        }));
    EXPECT_CALL(eventQueue, removeHandler(_, _)).Times(2);

    XWindowsScreen screen(new XWindowsImpl(), displayName, true, false, 0,
        &eventQueue);
    ASSERT_NE(static_cast<IEventJob*>(NULL), systemHandler.get());
    ASSERT_TRUE(screen.leave());

    SInt32 centerX = 0;
    SInt32 centerY = 0;
    screen.getCursorCenter(centerX, centerY);

    const Event::Type motionType = primaryScreenEvents.motionOnSecondary();
    EXPECT_CALL(eventQueue, addEvent(_))
        .Times(1)
        .WillOnce(Invoke([&](const Event& event) {
            EXPECT_EQ(motionType, event.getType());
            const IPrimaryScreen::MotionInfo* motion =
                static_cast<const IPrimaryScreen::MotionInfo*>(event.getData());
            ASSERT_NE(static_cast<const IPrimaryScreen::MotionInfo*>(NULL), motion);
            EXPECT_EQ(40, motion->m_x);
            EXPECT_EQ(0, motion->m_y);
            Event::deleteData(event);
        }));

    XEvent xevent = {};
    xevent.type = MotionNotify;
    xevent.xmotion.type = MotionNotify;
    xevent.xmotion.display = probeDisplay;
    xevent.xmotion.window = DefaultRootWindow(probeDisplay);
    xevent.xmotion.root = DefaultRootWindow(probeDisplay);
    xevent.xmotion.x_root = centerX + 40;
    xevent.xmotion.y_root = centerY;
    xevent.xmotion.same_screen = True;
    Event firstMotion(Event::kSystem, NULL, &xevent, Event::kDontFreeData);
    systemHandler->run(firstMotion);

    // XI2 can report another raw-motion notification before the synthetic
    // marker events are consumed. Its pointer query then observes the warp
    // destination and must not turn the recenter into reverse user motion.
    xevent.xmotion.x_root = centerX;
    Event recenterSample(Event::kSystem, NULL, &xevent, Event::kDontFreeData);
    systemHandler->run(recenterSample);

    XCloseDisplay(probeDisplay);
}
