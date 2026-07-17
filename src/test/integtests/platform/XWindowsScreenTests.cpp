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
#include "base/EventTypes.h"
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

#ifdef HAVE_XI2
class AsyncXISelectFailureXWindowsImpl : public XWindowsImpl {
public:
	explicit AsyncXISelectFailureXWindowsImpl(bool* requestQueued) :
		m_requestQueued(requestQueued)
	{
	}

	int XISelectEvents(Display* display, Window, XIEventMask* masks,
		int numMasks) override
	{
		// Queue a harmless BadWindow response after reporting synchronous
		// success. ErrorLock must observe it during XSync and keep Core motion
		// enabled. Sending a malformed XI2 request can terminate older Xvfb.
		XSelectInput(display, None, PointerMotionMask);
		const int status = Success;
		*m_requestQueued = (status == Success);
		return status;
	}

private:
	bool* m_requestQueued;
};

struct XIQueryVersionCapture {
	explicit XIQueryVersionCapture(int negotiatedMinor) :
		calls(0),
		requestedMajor(0),
		requestedMinor(0),
		negotiatedMinor(negotiatedMinor),
		selectCalls(0)
	{
	}

	int calls;
	int requestedMajor;
	int requestedMinor;
	int negotiatedMinor;
	int selectCalls;
};

class XIQueryVersionTrackingXWindowsImpl : public XWindowsImpl {
public:
	explicit XIQueryVersionTrackingXWindowsImpl(
		XIQueryVersionCapture* capture) :
		m_capture(capture)
	{
	}

	int XIQueryVersion(Display*, int* major, int* minor) override
	{
		++m_capture->calls;
		m_capture->requestedMajor = *major;
		m_capture->requestedMinor = *minor;
		*major = 2;
		*minor = m_capture->negotiatedMinor;
		return Success;
	}

	int XISelectEvents(Display*, Window, XIEventMask*, int) override
	{
		++m_capture->selectCalls;
		return Success;
	}

private:
	XIQueryVersionCapture* m_capture;
};
#endif

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

#ifdef HAVE_XI2
TEST(CXWindowsScreenTests, primaryScreenRequiresXi21BeforeSelectingRawMotion)
{
	const char* displayName = std::getenv("DISPLAY");
	if (displayName == NULL) {
		displayName = ":0.0";
	}

	std::unique_ptr<Display, int (*)(Display*)> probe(
		XOpenDisplay(displayName), &XCloseDisplay);
	ASSERT_NE(static_cast<Display*>(NULL), probe.get());
	int xiOpcode = 0;
	int xiEvent = 0;
	int xiError = 0;
	if (!XQueryExtension(probe.get(), "XInputExtension", &xiOpcode,
			&xiEvent, &xiError)) {
		GTEST_SKIP() << "XInputExtension is unavailable";
	}

	auto constructPrimary = [&](XIQueryVersionCapture& capture) {
		MockEventQueue eventQueue;
		std::vector<std::unique_ptr<IEventJob> > handlers;
		std::unique_ptr<IEventQueueBuffer> buffer;
		EXPECT_CALL(eventQueue, adoptHandler(_, _, _))
			.Times(2)
			.WillRepeatedly(Invoke(
				[&](Event::Type, void*, IEventJob* handler) {
					handlers.emplace_back(handler);
				}));
		EXPECT_CALL(eventQueue, adoptBuffer(_))
			.Times(2)
			.WillRepeatedly(Invoke(
				[&](IEventQueueBuffer* adoptedBuffer) {
					buffer.reset(adoptedBuffer);
				}));
		EXPECT_CALL(eventQueue, removeHandler(_, _)).Times(2);

		XWindowsScreen screen(
			new XIQueryVersionTrackingXWindowsImpl(&capture), displayName,
			true, false, 0, &eventQueue);
		return screen.xi2DetectedForTest();
	};

	XIQueryVersionCapture xi20(0);
	EXPECT_FALSE(constructPrimary(xi20));
	EXPECT_EQ(1, xi20.calls);
	EXPECT_EQ(2, xi20.requestedMajor);
	EXPECT_EQ(1, xi20.requestedMinor);
	EXPECT_EQ(0, xi20.selectCalls);

	XIQueryVersionCapture xi21(1);
	EXPECT_TRUE(constructPrimary(xi21));
	EXPECT_EQ(1, xi21.calls);
	EXPECT_EQ(2, xi21.requestedMajor);
	EXPECT_EQ(1, xi21.requestedMinor);
	EXPECT_EQ(1, xi21.selectCalls);
}

TEST(CXWindowsScreenTests, xi2AsyncSelectionFailureKeepsCoreMotionFallback)
{
    const char* displayName = std::getenv("DISPLAY");
    if (displayName == NULL) {
        displayName = ":0.0";
    }

    Display* probeDisplay = XOpenDisplay(displayName);
    ASSERT_NE(static_cast<Display*>(NULL), probeDisplay);
    int xiOpcode = 0;
    int xiEvent = 0;
    int xiError = 0;
    if (!XQueryExtension(probeDisplay, "XInputExtension", &xiOpcode,
            &xiEvent, &xiError)) {
        XCloseDisplay(probeDisplay);
        GTEST_SKIP() << "XInputExtension is unavailable";
    }

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

    bool requestQueued = false;
    XWindowsScreen screen(new AsyncXISelectFailureXWindowsImpl(&requestQueued),
        displayName, true, false, 0, &eventQueue);
    ASSERT_TRUE(requestQueued);
    ASSERT_FALSE(screen.xi2DetectedForTest());
    ASSERT_NE(static_cast<IEventJob*>(NULL), systemHandler.get());

    const Event::Type motionType = primaryScreenEvents.motionOnPrimary();
    EXPECT_CALL(eventQueue, addEvent(_))
        .Times(1)
        .WillOnce(Invoke([&](const Event& event) {
            EXPECT_EQ(motionType, event.getType());
            Event::deleteData(event);
        }));

    XEvent coreMotion = {};
    coreMotion.type = MotionNotify;
    coreMotion.xmotion.type = MotionNotify;
    coreMotion.xmotion.display = probeDisplay;
    coreMotion.xmotion.window = DefaultRootWindow(probeDisplay);
    coreMotion.xmotion.root = DefaultRootWindow(probeDisplay);
    coreMotion.xmotion.x_root = 100;
    coreMotion.xmotion.y_root = 100;
    coreMotion.xmotion.same_screen = True;
    Event event(Event::kSystem, NULL, &coreMotion, Event::kDontFreeData);
    systemHandler->run(event);

    XCloseDisplay(probeDisplay);
}
TEST(CXWindowsScreenTests, xi2OffscreenIgnoresQueuedCoreMotionAcrossRecenter)
{
    const char* displayName = std::getenv("DISPLAY");
    if (displayName == NULL) {
        displayName = ":0.0";
    }

    Display* probeDisplay = XOpenDisplay(displayName);
    ASSERT_NE(static_cast<Display*>(NULL), probeDisplay);

    int xiOpcode = 0;
    int xiEvent = 0;
    int xiError = 0;
    if (!XQueryExtension(probeDisplay, "XInputExtension", &xiOpcode,
            &xiEvent, &xiError)) {
        XCloseDisplay(probeDisplay);
        GTEST_SKIP() << "XInputExtension is unavailable";
    }

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
    if (!screen.xi2DetectedForTest()) {
        XCloseDisplay(probeDisplay);
        GTEST_SKIP() << "XI2 RawMotion was not selected; Core motion fallback remains active";
    }
    if (!screen.leave()) {
        XCloseDisplay(probeDisplay);
        GTEST_SKIP() << "primary pointer/keyboard grab is unavailable";
    }

    SInt32 centerX = 0;
    SInt32 centerY = 0;
    screen.getCursorCenter(centerX, centerY);

    const SInt32 rawDeltaX = 40;
    const Event::Type motionType = primaryScreenEvents.motionOnSecondary();
    EXPECT_CALL(eventQueue, addEvent(_))
        .Times(1)
        .WillOnce(Invoke([&](const Event& event) {
            EXPECT_EQ(motionType, event.getType());
            const IPrimaryScreen::MotionInfo* motion =
                static_cast<const IPrimaryScreen::MotionInfo*>(event.getData());
            ASSERT_NE(static_cast<const IPrimaryScreen::MotionInfo*>(NULL), motion);
            EXPECT_EQ(rawDeltaX, motion->m_x);
            EXPECT_EQ(0, motion->m_y);
            Event::deleteData(event);
        }));

    XEvent xiRawMotion = {};
    unsigned char valuatorMask[XIMaskLen(1)] = {};
    XISetMask(valuatorMask, 0);
    double valuatorValues[] = { 0.5 };
    XIRawEvent rawEvent = {};
    rawEvent.type = GenericEvent;
    rawEvent.extension = xiOpcode;
    rawEvent.evtype = XI_RawMotion;
    rawEvent.valuators.mask_len = sizeof(valuatorMask);
    rawEvent.valuators.mask = valuatorMask;
    rawEvent.valuators.values = valuatorValues;

    xiRawMotion.type = GenericEvent;
    xiRawMotion.xcookie.type = GenericEvent;
    xiRawMotion.xcookie.send_event = False;
    xiRawMotion.xcookie.display = probeDisplay;
    xiRawMotion.xcookie.extension = xiOpcode;
    xiRawMotion.xcookie.evtype = XI_RawMotion;
    xiRawMotion.xcookie.data = &rawEvent;
    Event rawMotion(Event::kSystem, NULL, &xiRawMotion, Event::kDontFreeData);
    systemHandler->run(rawMotion);
    valuatorValues[0] = static_cast<double>(rawDeltaX) - valuatorValues[0];
    systemHandler->run(rawMotion);

    // Once a usable RawMotion event takes ownership for this off-screen
    // epoch, a later malformed/scroll-only raw event must not re-enable Core
    // motion and replay the absolute distance accumulated since recentering.
    memset(valuatorMask, 0, sizeof(valuatorMask));
    XISetMask(valuatorMask, 2);
    valuatorValues[0] = 1.0;
    systemHandler->run(rawMotion);

    XEvent xevent = {};
    xevent.type = MotionNotify;
    xevent.xmotion.type = MotionNotify;
    xevent.xmotion.display = probeDisplay;
    xevent.xmotion.window = DefaultRootWindow(probeDisplay);
    xevent.xmotion.root = DefaultRootWindow(probeDisplay);
    xevent.xmotion.x_root = centerX + rawDeltaX;
    xevent.xmotion.y_root = centerY;
    xevent.xmotion.same_screen = True;

    // A Core sample queued before the XI2 recenter can arrive later with its
    // old coordinate. XI2 owns motion delivery, so it must not be replayed as
    // a second user move.
    xevent.xmotion.x_root = centerX - 22;
    Event delayedCoreMotion(Event::kSystem, NULL, &xevent, Event::kDontFreeData);
    systemHandler->run(delayedCoreMotion);

    XCloseDisplay(probeDisplay);
}
#endif
