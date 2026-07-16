/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
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

#include "platform/XWindowsScreen.h"

#include "platform/XWindowsClipboard.h"
#include "platform/XWindowsClipboardSnapshotState.h"
#include "platform/XWindowsEventQueueBuffer.h"
#include "platform/XWindowsKeyState.h"
#include "platform/XWindowsScreenSaver.h"
#include "platform/XWindowsUtil.h"
#include "barrier/Clipboard.h"
#include "barrier/ClipboardChunk.h"
#include "barrier/KeyMap.h"
#include "barrier/XScreen.h"
#include "arch/XArch.h"
#include "arch/Arch.h"
#include "base/Log.h"
#include "base/Stopwatch.h"
#include "base/IEventQueue.h"
#include "base/TMethodEventJob.h"
#include "mt/Thread.h"

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

static int xi_opcode;

struct XWindowsClipboardSnapshotWorkerContext {
    XWindowsClipboardSnapshotWorkerContext(
        const String& displayName_, IEventQueue* events_, void* eventTarget_) :
        displayName(displayName_),
        events(events_),
        eventTarget(eventTarget_),
        clipboardChangedEvent(Event::kUnknown),
        stopping(false)
    {
    }

    std::mutex mutex;
    std::condition_variable wake;
    XWindowsClipboardSnapshotState snapshots;
    String displayName;
    IEventQueue* events;
    void* eventTarget;
    Event::Type clipboardChangedEvent;
    bool stopping;
};

namespace {

const double kClipboardSnapshotReadDeadlineSeconds = 2.0;
const double kClipboardSnapshotShutdownWaitSeconds = 0.5;
const double kClipboardSnapshotRetryBackoffSeconds = 0.05;
const UInt32 kClipboardSnapshotMaxRetryAttempts = 2;
const double kPrimaryLeaveGrabTimeoutSeconds = 1.0;
const double kLowLatencyPrimaryLeaveGrabTimeoutSeconds = 0.025;
const double kPrimaryLeaveGrabRetrySleepSeconds = 0.05;
const double kLowLatencyPrimaryLeaveGrabRetrySleepSeconds = 0.005;

class SnapshotDisplay {
public:
    explicit SnapshotDisplay(const String& displayName) :
        display(NULL),
        window(None)
    {
        display = impl.XOpenDisplay(
            displayName.empty() ? NULL : displayName.c_str());
        if (display == NULL) {
            return;
        }

        XSetWindowAttributes attributes = {};
        attributes.event_mask = PropertyChangeMask;
        window = impl.XCreateWindow(
            display, impl.do_DefaultRootWindow(display), 0, 0, 1, 1, 0, 0,
            InputOnly, CopyFromParent, CWEventMask, &attributes);
    }

    ~SnapshotDisplay()
    {
        if (display != NULL) {
            if (window != None) {
                impl.XDestroyWindow(display, window);
            }
            impl.XCloseDisplay(display);
        }
    }

    XWindowsImpl impl;
    Display* display;
    Window window;
};

bool
readClipboardSnapshot(
    const std::shared_ptr<XWindowsClipboardSnapshotWorkerContext>& context,
    XWindowsClipboardSnapshotState::Request* request,
    std::shared_ptr<const String>* snapshot, Window* currentOwner,
    bool* ownerObserved)
{
    *snapshot = std::shared_ptr<const String>();
    *currentOwner = None;
    *ownerObserved = false;

    SnapshotDisplay workerDisplay(context->displayName);
    if (workerDisplay.display == NULL || workerDisplay.window == None) {
        LOG((CLOG_WARN "could not open an independent X11 clipboard display"));
        return false;
    }

    XWindowsClipboard source(
        &workerDisplay.impl, workerDisplay.display, workerDisplay.window,
        request->id, ARCH->time() + kClipboardSnapshotReadDeadlineSeconds);
    *currentOwner = workerDisplay.impl.XGetSelectionOwner(
        workerDisplay.display, source.getSelection());
    *ownerObserved = true;
    if (*currentOwner != request->owner) {
        return false;
    }

    if (request->timestamp == CurrentTime) {
        request->timestamp = XWindowsUtil::getCurrentTime(
            workerDisplay.display, workerDisplay.window);
    }
    Clipboard clipboard;
    const bool copiedToMemory = Clipboard::copy(
        &clipboard, &source, request->timestamp);
    const bool copied = copiedToMemory && source.wasLastReadValid();
    if (copiedToMemory && !copied) {
        LOG((CLOG_DEBUG
            "rejecting X11 clipboard snapshot without provider-read evidence"));
    }
    *currentOwner = workerDisplay.impl.XGetSelectionOwner(
        workerDisplay.display, source.getSelection());
    if (!copied || *currentOwner != request->owner) {
        return false;
    }

    std::shared_ptr<String> marshalled(
        new String(clipboard.marshall()));
    if (marshalled->size() > ClipboardChunk::kMaxReceiveSize) {
        LOG((CLOG_WARN
            "refusing oversized local clipboard snapshot: size=%lu limit=%lu",
            static_cast<unsigned long>(marshalled->size()),
            static_cast<unsigned long>(ClipboardChunk::kMaxReceiveSize)));
        return false;
    }

    *snapshot = marshalled;
    return true;
}

void
runClipboardSnapshotWorker(
    const std::shared_ptr<XWindowsClipboardSnapshotWorkerContext>& context)
{
    for (;;) {
        Thread::testCancel();

        XWindowsClipboardSnapshotState::Request request;
        {
            std::unique_lock<std::mutex> lock(context->mutex);
            context->wake.wait(lock, [&context]() {
                return context->stopping || context->snapshots.hasPending();
            });
            if (context->stopping) {
                return;
            }
            if (!context->snapshots.takeNext(&request)) {
                continue;
            }
        }

        std::shared_ptr<const String> snapshot;
        Window currentOwner = None;
        bool ownerObserved = false;
        const bool copied = readClipboardSnapshot(
            context, &request, &snapshot, &currentOwner, &ownerObserved);

        bool retryQueued = false;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->stopping) {
                return;
            }
            if (!copied || !context->snapshots.complete(
                    request, currentOwner, snapshot)) {
                retryQueued = ownerObserved ?
                    context->snapshots.retry(
                        request, currentOwner,
                        kClipboardSnapshotMaxRetryAttempts) :
                    context->snapshots.retryWithoutOwnerObservation(
                        request, kClipboardSnapshotMaxRetryAttempts);
                if (!retryQueued) {
                    if (ownerObserved) {
                        context->snapshots.fail(request, currentOwner);
                    }
                    else {
                        context->snapshots.failWithoutOwnerObservation(
                            request);
                    }
                }
                LOG((CLOG_DEBUG
                    "%s unavailable or stale X11 clipboard snapshot: id=%d generation=%llu attempt=%u owner=0x%lx current=%s0x%lx",
                    retryQueued ? "retrying" : "discarded",
                    request.id,
                    static_cast<unsigned long long>(request.generation),
                    request.attempt,
                    static_cast<unsigned long>(request.owner),
                    ownerObserved ? "" : "unobserved/",
                    static_cast<unsigned long>(currentOwner)));
            }
            else {
                IScreen::ClipboardInfo* info =
                    static_cast<IScreen::ClipboardInfo*>(
                        std::malloc(sizeof(IScreen::ClipboardInfo)));
                if (info != NULL) {
                    info->m_id = request.id;
                    info->m_sequenceNumber = request.sequenceNumber;
                    context->events->addEvent(Event(
                        context->clipboardChangedEvent,
                        context->eventTarget, info));
                }
            }
        }

        if (retryQueued) {
            ARCH->sleep(kClipboardSnapshotRetryBackoffSeconds);
            context->wake.notify_one();
        }
    }
}

bool
normalizeToScreenShape(const char* operation,
	SInt32 sx, SInt32 sy, SInt32 sw, SInt32 sh,
	SInt32& x, SInt32& y)
{
	if (sw <= 0 || sh <= 0) {
		LOG((CLOG_WARN "ignoring %s for invalid screen shape %+d,%+d %dx%d",
			operation, sx, sy, sw, sh));
		return false;
	}

	const SInt32 minX = sx;
	const SInt32 minY = sy;
	const SInt32 maxX = sx + sw - 1;
	const SInt32 maxY = sy + sh - 1;
	const SInt32 originalX = x;
	const SInt32 originalY = y;

	if (x < minX) {
		x = minX;
	}
	else if (x > maxX) {
		x = maxX;
	}
	if (y < minY) {
		y = minY;
	}
	else if (y > maxY) {
		y = maxY;
	}

	if (x != originalX || y != originalY) {
		LOG((CLOG_WARN "normalized out-of-bounds %s from %+d,%+d to %+d,%+d within %+d,%+d %dx%d",
			operation, originalX, originalY, x, y, sx, sy, sw, sh));
	}

	return true;
}

bool
clampPointToRect(const char* operation,
	SInt32 rx, SInt32 ry, SInt32 rw, SInt32 rh,
	SInt32& x, SInt32& y)
{
	return normalizeToScreenShape(operation, rx, ry, rw, rh, x, y);
}

bool
visibleAreaContains(const XWindowsScreen::VisibleArea& area,
	SInt32 x, SInt32 y)
{
	return area.width > 0 && area.height > 0 &&
		x >= area.x && x < area.x + area.width &&
		y >= area.y && y < area.y + area.height;
}

bool
visibleAreaEquals(const XWindowsScreen::VisibleArea& a,
	const XWindowsScreen::VisibleArea& b)
{
	return a.x == b.x && a.y == b.y &&
			a.width == b.width && a.height == b.height;
}

bool
visibleAreaTopologyEquals(const XWindowsScreen::VisibleArea& a,
	const XWindowsScreen::VisibleArea& b)
{
	return visibleAreaEquals(a, b) && a.primary == b.primary;
}

bool
visibleAreaTopologiesEqual(const XWindowsScreen::VisibleAreas& first,
	const XWindowsScreen::VisibleAreas& second)
{
	if (first.size() != second.size()) {
		return false;
	}

	std::vector<bool> matched(second.size(), false);
	for (XWindowsScreen::VisibleAreas::const_iterator i = first.begin();
		i != first.end(); ++i) {
		bool found = false;
		for (std::size_t j = 0; j < second.size(); ++j) {
			if (!matched[j] && visibleAreaTopologyEquals(*i, second[j])) {
				matched[j] = true;
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
	}

	return true;
}

long long
squaredDistance(SInt32 x1, SInt32 y1, SInt32 x2, SInt32 y2)
{
	const long long dx = static_cast<long long>(x1) - static_cast<long long>(x2);
	const long long dy = static_cast<long long>(y1) - static_cast<long long>(y2);
	return dx * dx + dy * dy;
}

void
clampPointToAreaSilently(const XWindowsScreen::VisibleArea& area,
	SInt32& x, SInt32& y)
{
	const SInt32 maxX = area.x + area.width - 1;
	const SInt32 maxY = area.y + area.height - 1;

	if (x < area.x) {
		x = area.x;
	}
	else if (x > maxX) {
		x = maxX;
	}
	if (y < area.y) {
		y = area.y;
	}
	else if (y > maxY) {
		y = maxY;
	}
}

bool
adjustPointToVisibleAreas(const char* operation,
	const XWindowsScreen::VisibleAreas& areas,
	SInt32& x, SInt32& y)
{
	for (XWindowsScreen::VisibleAreas::const_iterator i = areas.begin();
		i != areas.end(); ++i) {
		if (visibleAreaContains(*i, x, y)) {
			return true;
		}
	}

	bool found = false;
	long long bestDistance = 0;
	SInt32 bestX = x;
	SInt32 bestY = y;
	XWindowsScreen::VisibleArea bestArea;

	for (XWindowsScreen::VisibleAreas::const_iterator i = areas.begin();
		i != areas.end(); ++i) {
		if (i->width <= 0 || i->height <= 0) {
			continue;
		}

		SInt32 candidateX = x;
		SInt32 candidateY = y;
		clampPointToAreaSilently(*i, candidateX, candidateY);
		const long long distance =
			squaredDistance(x, y, candidateX, candidateY);
		if (!found || distance < bestDistance ||
			(distance == bestDistance && i->primary && !bestArea.primary)) {
			found = true;
			bestDistance = distance;
			bestX = candidateX;
			bestY = candidateY;
			bestArea = *i;
		}
	}

	if (!found) {
		LOG((CLOG_WARN "ignoring %s for no visible output areas",
			operation));
		return false;
	}

	if (x != bestX || y != bestY) {
		LOG((CLOG_WARN "normalized unavailable %s from %+d,%+d to %+d,%+d within visible output %+d,%+d %dx%d",
			operation, x, y, bestX, bestY, bestArea.x, bestArea.y,
			bestArea.width, bestArea.height));
	}

	x = bestX;
	y = bestY;
	return true;
}

bool
adjustPointToVisibleAreasNearAnchor(const char* operation,
	const XWindowsScreen::VisibleAreas& areas,
	SInt32 anchorX, SInt32 anchorY,
	SInt32& x, SInt32& y)
{
	bool found = false;
	long long bestDistance = 0;
	XWindowsScreen::VisibleArea bestArea;

	for (XWindowsScreen::VisibleAreas::const_iterator i = areas.begin();
		i != areas.end(); ++i) {
		if (i->width <= 0 || i->height <= 0) {
			continue;
		}
		if (visibleAreaContains(*i, anchorX, anchorY)) {
			bestArea = *i;
			found = true;
			break;
		}

		SInt32 candidateX = anchorX;
		SInt32 candidateY = anchorY;
		clampPointToAreaSilently(*i, candidateX, candidateY);
		const long long distance =
			squaredDistance(anchorX, anchorY, candidateX, candidateY);
		if (!found || distance < bestDistance ||
			(distance == bestDistance && i->primary && !bestArea.primary)) {
			found = true;
			bestDistance = distance;
			bestArea = *i;
		}
	}

	if (!found) {
		LOG((CLOG_WARN "ignoring %s for no visible output areas",
			operation));
		return false;
	}

	const SInt32 originalX = x;
	const SInt32 originalY = y;
	clampPointToAreaSilently(bestArea, x, y);
	if (x != originalX || y != originalY) {
		LOG((CLOG_WARN "normalized anchored %s from %+d,%+d to %+d,%+d using visible output %+d,%+d %dx%d near anchor %+d,%+d",
			operation, originalX, originalY, x, y, bestArea.x, bestArea.y,
			bestArea.width, bestArea.height, anchorX, anchorY));
	}

	return true;
}

std::string
readTrimmedFile(const std::string& path)
{
	std::ifstream stream(path.c_str());
	std::string value;
	if (!std::getline(stream, value)) {
		return "";
	}

	while (!value.empty() && (value[value.size() - 1] == '\n' ||
		value[value.size() - 1] == '\r' ||
		value[value.size() - 1] == ' ' ||
		value[value.size() - 1] == '\t')) {
		value.erase(value.size() - 1);
	}

	return value;
}

bool
isDrmConnectorName(const char* name)
{
	return std::strncmp(name, "card", 4) == 0 &&
		std::strchr(name, '-') != NULL;
}

bool
isDrmConnectorEnterable(const std::string& status,
						const std::string& enabled,
						const std::string& dpms)
{
	if (status != "connected") {
		return false;
	}
	if (enabled == "disabled") {
		return false;
	}
	if (dpms == "Off") {
		LOG((CLOG_DEBUG "DRM connector is DPMS Off but still enterable; enter path will attempt wake"));
	}
	return true;
}

bool
hasUsableDrmDisplay()
{
	DIR* dir = opendir("/sys/class/drm");
	if (dir == NULL) {
		return true;
	}

	bool sawConnector = false;
	bool sawConnectedConnector = false;
	bool hasUsableConnector = false;
	while (!hasUsableConnector) {
		dirent* entry = readdir(dir);
		if (entry == NULL) {
			break;
		}
		if (!isDrmConnectorName(entry->d_name)) {
			continue;
		}

		sawConnector = true;
		const std::string base = std::string("/sys/class/drm/") + entry->d_name;
		if (readTrimmedFile(base + "/status") != "connected") {
			continue;
		}

		sawConnectedConnector = true;
		const std::string enabled = readTrimmedFile(base + "/enabled");
		const std::string dpms = readTrimmedFile(base + "/dpms");
		if (isDrmConnectorEnterable("connected", enabled, dpms)) {
			hasUsableConnector = true;
		}
	}
	closedir(dir);

	return !sawConnector || (sawConnectedConnector && hasUsableConnector);
}

bool
isPrimaryDisplayEnterable(bool hasUsableDrmDisplay, SInt32 width, SInt32 height)
{
	return hasUsableDrmDisplay && width >= 64 && height >= 64;
}

bool
isSecondaryDisplayAdvertisable(bool hasUsableDrmDisplay, SInt32 width, SInt32 height)
{
	return hasUsableDrmDisplay && width >= 64 && height >= 64;
}

}

//
// XWindowsScreen
//

// NOTE -- the X display is shared among several objects but is owned
// by the XWindowsScreen.  Xlib is not reentrant so we must ensure
// that no two objects can simultaneously call Xlib with the display.
// this is easy since we only make X11 calls from the main thread.
// we must also ensure that these objects do not use the display in
// their destructors or, if they do, we can tell them not to.  This
// is to handle unexpected disconnection of the X display, when any
// call on the display is invalid.  In that situation we discard the
// display and the X11 event queue buffer, ignore any calls that try
// to use the display, and wait to be destroyed.

XWindowsScreen*		XWindowsScreen::s_screen = NULL;

XWindowsScreen::VisibleArea::VisibleArea() :
	x(0),
	y(0),
	width(0),
	height(0),
	primary(false)
{
}

XWindowsScreen::VisibleArea::VisibleArea(SInt32 x_, SInt32 y_,
	SInt32 width_, SInt32 height_, bool primary_) :
	x(x_),
	y(y_),
	width(width_),
	height(height_),
	primary(primary_)
{
}

XWindowsScreen::XWindowsScreen(
        IXWindowsImpl* impl,
		const char* displayName,
		bool isPrimary,
		bool disableXInitThreads,
		int mouseScrollDelta,
		IEventQueue* events) :
		PlatformScreen(events),
		m_impl(impl),
		m_isPrimary(isPrimary),
	m_mouseScrollDelta(mouseScrollDelta),
    m_x_accumulatedScroll(0),
    m_y_accumulatedScroll(0),
	m_display(NULL),
	m_root(None),
	m_window(None),
	m_isOnScreen(m_isPrimary),
	m_x(0), m_y(0),
	m_w(0), m_h(0),
	m_xCenter(0), m_yCenter(0),
	m_primaryVisibleX(0), m_primaryVisibleY(0),
	m_primaryVisibleW(0), m_primaryVisibleH(0),
	m_xCursor(0), m_yCursor(0),
	m_keyState(NULL),
	m_lastFocus(None),
	m_lastFocusRevert(RevertToNone),
	m_im(NULL),
	m_ic(NULL),
	m_lastKeycode(0),
	m_sequenceNumber(0),
	m_clipboardSnapshotContext(),
	m_clipboardSnapshotThread(NULL),
	m_clipboardSnapshotsBootstrapped(false),
	m_clipboardSnapshotThreadingEnabled(!disableXInitThreads),
    m_dropTargetPath(),
	m_screensaver(NULL),
	m_screensaverNotify(false),
	m_autoRepeat(false),
	m_xtestIsXineramaUnaware(true),
	m_xinerama(false),
	m_preserveFocus(false),
	m_xkb(false),
	m_xkbEventBase(0),
	m_xi2detected(false),
	m_lowLatencyMode(false),
	m_xrandr(false),
	m_xrandrEventBase(0),
	m_xfixes(false),
	m_xfixesEventBase(0),
	m_events(events)
{
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		m_clipboard[id] = NULL;
	}

	assert(s_screen == NULL);

	if (mouseScrollDelta==0) m_mouseScrollDelta=120;
	XWindowsScreen* const previousScreen = s_screen;
	XIOErrorHandler previousIOErrorHandler = NULL;
	bool ioErrorHandlerInstalled = false;
	bool systemHandlerInstalled = false;
	bool eventBufferInstalled = false;
	s_screen = this;

	try {
		if (!disableXInitThreads) {
			// initializes Xlib support for concurrent threads.
			if (m_impl->XInitThreads() == 0) {
				throw XArch("XInitThreads() returned zero");
			}
		}
		else {
			LOG((CLOG_DEBUG "skipping XInitThreads()"));
		}

		// set the X I/O error handler so we catch the display disconnecting
		previousIOErrorHandler =
			m_impl->XSetIOErrorHandler(&XWindowsScreen::ioErrorHandler);
		ioErrorHandlerInstalled = true;

		m_display     = openDisplay(displayName);
        m_root        = m_impl->do_DefaultRootWindow(m_display);
		saveShape();
		m_window      = openWindow();
        m_screensaver = new XWindowsScreenSaver(m_impl, m_display,
								m_window, getEventTarget(), events);
        m_keyState    = new XWindowsKeyState(m_impl, m_display, m_xkb, events,
                                             m_keyMap);
		LOG((CLOG_DEBUG "screen shape: %d,%d %dx%d %s", m_x, m_y, m_w, m_h, m_xinerama ? "(xinerama)" : ""));
		LOG((CLOG_DEBUG "window is 0x%08x", m_window));

		// primary/secondary screen only initialization
		if (m_isPrimary) {
#ifdef HAVE_XI2
			m_xi2detected = detectXI2();
			if (m_xi2detected) {
				selectXIRawMotion();
			} else
#endif
			{
				// start watching for events on other windows
				selectEvents(m_root);
			}

			// prepare to use input methods
			openIM();
		}
		else {
			// become impervious to server grabs
			m_impl->XTestGrabControl(m_display, True);
		}

		// initialize the clipboards
		for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
			m_clipboard[id] = new XWindowsClipboard(m_impl, m_display,
				m_window, id);
		}
		if (m_clipboardSnapshotThreadingEnabled) {
			m_clipboardSnapshotContext.reset(
				new XWindowsClipboardSnapshotWorkerContext(
					DisplayString(m_display), m_events, getEventTarget()));
		}
		else {
			LOG((CLOG_WARN
				"asynchronous X11 clipboard snapshots are disabled by --no-xinitthreads; provider isolation is unavailable"));
		}
		m_xfixes = detectXFixesSelectionNotifications();

		// install event handlers
		m_events->adoptHandler(Event::kSystem, m_events->getSystemTarget(),
			new TMethodEventJob<XWindowsScreen>(this,
				&XWindowsScreen::handleSystemEvent));
		systemHandlerInstalled = true;

		// install the platform event queue
		m_events->adoptBuffer(new XWindowsEventQueueBuffer(m_impl,
			m_display, m_window, m_events));
		eventBufferInstalled = true;
	}
	catch (...) {
		releaseResources(eventBufferInstalled, systemHandlerInstalled);
		if (ioErrorHandlerInstalled) {
			m_impl->XSetIOErrorHandler(previousIOErrorHandler);
		}
		s_screen = previousScreen;
		delete m_impl;
		m_impl = NULL;
		throw;
	}
}

XWindowsScreen::~XWindowsScreen()
{
	assert(s_screen  != NULL);
	assert(m_display != NULL);

	releaseResources(true, true);
    m_impl->XSetIOErrorHandler(NULL);

	s_screen = NULL;
    delete m_impl;
}

void
XWindowsScreen::releaseResources(bool eventBufferInstalled,
	bool systemHandlerInstalled)
{
	stopClipboardSnapshotWorker();
	if (eventBufferInstalled) {
		m_events->adoptBuffer(NULL);
	}
	if (systemHandlerInstalled) {
		m_events->removeHandler(Event::kSystem,
			m_events->getSystemTarget());
	}
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		delete m_clipboard[id];
		m_clipboard[id] = NULL;
	}
    delete m_keyState;
	delete m_screensaver;
	m_keyState    = NULL;
	m_screensaver = NULL;
	if (m_display != NULL) {
		// FIXME -- is it safe to clean up the IC and IM without a display?
		if (m_ic != NULL) {
            m_impl->XDestroyIC(m_ic);
		}
		if (m_im != NULL) {
            m_impl->XCloseIM(m_im);
		}
		if (m_window != None) {
			m_impl->XDestroyWindow(m_display, m_window);
		}
        m_impl->XCloseDisplay(m_display);
		m_display = NULL;
		m_window = None;
	}
}

void
XWindowsScreen::enable()
{
	if (!m_isPrimary) {
		// get the keyboard control state
		XKeyboardState keyControl;
        m_impl->XGetKeyboardControl(m_display, &keyControl);
		m_autoRepeat = (keyControl.global_auto_repeat == AutoRepeatModeOn);
		m_keyState->setAutoRepeat(keyControl);

		// move hider window under the cursor center
        m_impl->XMoveWindow(m_display, m_window, m_xCenter, m_yCenter);

		// raise and show the window
		// FIXME -- take focus?
        m_impl->XMapRaised(m_display, m_window);

		// warp the mouse to the cursor center
		fakeMouseMove(m_xCenter, m_yCenter);
	}

	bootstrapClipboardSnapshots();
}

void
XWindowsScreen::disable()
{
	// release input context focus
	if (m_ic != NULL) {
        m_impl->XUnsetICFocus(m_ic);
	}

	// Release grabs explicitly before hiding the grab window. Confirm the
	// server processed the requests before reporting that local input is
	// active again; otherwise a disconnect during a transition can leave the
	// pointer or keyboard captured by the now-local server.
	m_impl->XUngrabPointer(m_display, CurrentTime);
	m_impl->XUngrabKeyboard(m_display, CurrentTime);
    m_impl->XUnmapWindow(m_display, m_window);
	m_impl->XSync(m_display, False);

	// restore auto-repeat state
	if (!m_isPrimary && m_autoRepeat) {
		//XAutoRepeatOn(m_display);
	}
}

void
XWindowsScreen::enter()
{
	screensaver(false);

	// release input context focus
	if (m_ic != NULL) {
        m_impl->XUnsetICFocus(m_ic);
	}

	// set the input focus to what it had been when we took it
	if (m_lastFocus != None) {
		// the window may not exist anymore so ignore errors
		XWindowsUtil::ErrorLock lock(m_display);
        m_impl->XSetInputFocus(m_display, m_lastFocus, m_lastFocusRevert, CurrentTime);
	}

	wakeDisplayFromPowerSave();

	// Release grabs explicitly before hiding the grab window. Confirm the
	// server processed the requests before reporting that local input is
	// active again; otherwise a disconnect during a transition can leave the
	// pointer or keyboard captured by the now-local server.
	m_impl->XUngrabPointer(m_display, CurrentTime);
	m_impl->XUngrabKeyboard(m_display, CurrentTime);
    m_impl->XUnmapWindow(m_display, m_window);
	m_impl->XSync(m_display, False);

/* maybe call this if entering for the screensaver
	// set keyboard focus to root window.  the screensaver should then
	// pick up key events for when the user enters a password to unlock.
	XSetInputFocus(m_display, PointerRoot, PointerRoot, CurrentTime);
*/

	if (!m_isPrimary) {
		// get the keyboard control state
		XKeyboardState keyControl;
        m_impl->XGetKeyboardControl(m_display, &keyControl);
		m_autoRepeat = (keyControl.global_auto_repeat == AutoRepeatModeOn);
		m_keyState->setAutoRepeat(keyControl);

		// turn off auto-repeat.  we do this so fake key press events don't
		// cause the local server to generate their own auto-repeats of
		// those keys.
		//XAutoRepeatOff(m_display);
	}

	// now on screen
	m_isOnScreen = true;
}

bool
XWindowsScreen::leave()
{
	wakeDisplayFromPowerSave();

	if (!m_isPrimary) {
		// restore the previous keyboard auto-repeat state.  if the user
		// changed the auto-repeat configuration while on the client then
		// that state is lost.  that's because we can't get notified by
		// the X server when the auto-repeat configuration is changed so
		// we can't track the desired configuration.
		if (m_autoRepeat) {
			//XAutoRepeatOn(m_display);
		}

		// move hider window under the cursor center
        m_impl->XMoveWindow(m_display, m_window, m_xCenter, m_yCenter);
	}

	// raise and show the window
    m_impl->XMapRaised(m_display, m_window);

	// grab the mouse and keyboard, if primary and possible
	if (m_isPrimary && !grabMouseAndKeyboard()) {
		m_impl->XUngrabPointer(m_display, CurrentTime);
		m_impl->XUngrabKeyboard(m_display, CurrentTime);
        m_impl->XUnmapWindow(m_display, m_window);
		m_impl->XSync(m_display, False);
		return false;
	}

	// save current focus
    m_impl->XGetInputFocus(m_display, &m_lastFocus, &m_lastFocusRevert);

	// take focus
	if (!m_preserveFocus) {
        m_impl->XSetInputFocus(m_display, m_window, RevertToPointerRoot, CurrentTime);
	}

	// now warp the mouse.  we warp after showing the window so we're
	// guaranteed to get the mouse leave event and to prevent the
	// keyboard focus from changing under point-to-focus policies.
	if (m_isPrimary) {
		warpCursor(m_xCenter, m_yCenter);
	}
	else {
		fakeMouseMove(m_xCenter, m_yCenter);
	}

	// set input context focus to our window
	if (m_ic != NULL) {
		XmbResetIC(m_ic);
        m_impl->XSetICFocus(m_ic);
		m_filtered.clear();
	}

	// now off screen
	m_isOnScreen = false;

	return true;
}

void
XWindowsScreen::wakeDisplayFromPowerSave()
{
#if HAVE_X11_EXTENSIONS_DPMS_H
	// Synthetic input does not reliably wake DPMS before we try to grab or warp.
	int dummy;
	CARD16 powerlevel;
	BOOL enabled;
	if (m_impl->DPMSQueryExtension(m_display, &dummy, &dummy) &&
		m_impl->DPMSCapable(m_display) &&
		m_impl->DPMSInfo(m_display, &powerlevel, &enabled)) {
		if (enabled && powerlevel != DPMSModeOn) {
			LOG((CLOG_INFO "waking X11 display from DPMS mode before input transition"));
			m_impl->DPMSForceLevel(m_display, DPMSModeOn);
			m_impl->XSync(m_display, False);
			ARCH->sleep(0.05);
		}
	}
#endif
}

bool
XWindowsScreen::setClipboard(ClipboardID id, const IClipboard* clipboard)
{
	// fail if we don't have the requested clipboard
	if (m_clipboard[id] == NULL) {
		return false;
	}
	if (m_clipboardSnapshotContext) {
		std::lock_guard<std::mutex> lock(m_clipboardSnapshotContext->mutex);
		// A remote clipboard is about to become our selection. Revoke any
		// external-owner snapshot before its worker can publish stale data.
		m_clipboardSnapshotContext->snapshots.invalidate(id, m_window);
	}

	// get the actual time.  ICCCM does not allow CurrentTime.
	Time timestamp = XWindowsUtil::getCurrentTime(
								m_display, m_clipboard[id]->getWindow());

	bool copied = false;
	if (clipboard != NULL) {
		// save clipboard data
		copied = Clipboard::copy(m_clipboard[id], clipboard, timestamp);
	}
	else {
		// assert clipboard ownership
		if (!m_clipboard[id]->open(timestamp)) {
			return false;
		}
		copied = m_clipboard[id]->empty();
		m_clipboard[id]->close();
	}
	return copied;
}

void
XWindowsScreen::checkClipboards()
{
	if (!m_clipboardSnapshotContext) {
		return;
	}

	// This is only an owner query, never a provider data read. It supplies a
	// safe leave-time fallback when XFixes is unavailable.
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		const Window owner = m_impl->XGetSelectionOwner(
			m_display, m_clipboard[id]->getSelection());
		Window knownOwner = None;
		{
			std::lock_guard<std::mutex> lock(
				m_clipboardSnapshotContext->mutex);
			knownOwner =
				m_clipboardSnapshotContext->snapshots.currentOwner(id);
		}
		const bool external = owner != None && owner != m_window;
		if (shouldObserveClipboardOnCheckForTest(
				m_xfixes, owner, knownOwner, m_window)) {
			// Without XFixes, a selection owner can update its payload while
			// keeping the same owner window. Re-snapshot at this safe leave-time
			// check, but only re-announce ownership when the owner changed.
			observeClipboardOwner(
				id, owner, CurrentTime, !m_xfixes && external,
				external && owner != knownOwner);
		}
	}
}

bool
XWindowsScreen::hasAsyncClipboardSnapshots() const
{
	return m_clipboardSnapshotContext != NULL;
}

void
XWindowsScreen::fakeDraggingFiles(DragFileList)
{
    // File-copy transfers use the shared inbox on X11. Native fake drag
    // synthesis is intentionally not required for copy/paste file support.
    m_fakeDraggingStarted = false;
}

const String&
XWindowsScreen::getDropTarget() const
{
    if (m_dropTargetPath.empty()) {
        const char* home = std::getenv("HOME");
        if (home != NULL && home[0] != '\0') {
            const std::string downloads = std::string(home) + "/Downloads";
            struct stat info;
            if (stat(downloads.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
                m_dropTargetPath = downloads + "/Weave Inbox";
            }
            else {
                m_dropTargetPath = std::string(home) + "/Weave Inbox";
            }
        }
        else {
            m_dropTargetPath = "/tmp/Weave Inbox";
        }
        LOG((CLOG_INFO "using X11 drop target: %s", m_dropTargetPath.c_str()));
    }

    return m_dropTargetPath;
}

void
XWindowsScreen::setDropTarget(const String& target)
{
    m_dropTargetPath = target;
}

void
XWindowsScreen::openScreensaver(bool notify)
{
	m_screensaverNotify = notify;
	if (!m_screensaverNotify) {
		m_screensaver->disable();
	}
}

void
XWindowsScreen::closeScreensaver()
{
	if (!m_screensaverNotify) {
		m_screensaver->enable();
	}
}

void
XWindowsScreen::screensaver(bool activate)
{
	if (activate) {
		m_screensaver->activate();
	}
	else {
		m_screensaver->deactivate();
	}
}

void
XWindowsScreen::resetOptions()
{
	m_xtestIsXineramaUnaware = true;
	m_preserveFocus = false;
}

void
XWindowsScreen::setOptions(const OptionsList& options)
{
	for (UInt32 i = 0, n = options.size(); i < n; i += 2) {
		if (options[i] == kOptionXTestXineramaUnaware) {
			m_xtestIsXineramaUnaware = (options[i + 1] != 0);
			LOG((CLOG_DEBUG1 "XTest is Xinerama unaware %s", m_xtestIsXineramaUnaware ? "true" : "false"));
		}
		else if (options[i] == kOptionScreenPreserveFocus) {
			m_preserveFocus = (options[i + 1] != 0);
			LOG((CLOG_DEBUG1 "Preserve Focus = %s", m_preserveFocus ? "true" : "false"));
		}
		else if (options[i] == kOptionLowLatencyMode) {
			m_lowLatencyMode = (options[i + 1] != 0);
			LOG((CLOG_INFO "Low Latency Mode = %s", m_lowLatencyMode ? "true" : "false"));
		}
	}
}

void
XWindowsScreen::setSequenceNumber(UInt32 seqNum)
{
	m_sequenceNumber = seqNum;
}

bool
XWindowsScreen::isPrimary() const
{
	return m_isPrimary;
}

bool
XWindowsScreen::canEnter() const
{
	if (!m_isPrimary) {
		return true;
	}

	if (m_screensaver != NULL && m_screensaver->isActive()) {
		LOG((CLOG_DEBUG "primary X11 display is in screensaver/DPMS state; enter path will attempt wake"));
	}

	const bool drmUsable = hasUsableDrmDisplay();
	if (!isPrimaryDisplayEnterable(drmUsable, m_w, m_h)) {
		LOG((CLOG_WARN "primary X11 display is not enterable; drmUsable=%d shape=%d,%d %dx%d",
			drmUsable ? 1 : 0, m_x, m_y, m_w, m_h));
		return false;
	}

	return true;
}

bool
XWindowsScreen::isPrimaryDisplayEnterableForTest(bool hasUsableDrmDisplay,
												 SInt32 width,
												 SInt32 height)
{
	return isPrimaryDisplayEnterable(hasUsableDrmDisplay, width, height);
}

bool
XWindowsScreen::isSecondaryDisplayAdvertisableForTest(bool hasUsableDrmDisplay,
													  SInt32 width,
													  SInt32 height)
{
	return isSecondaryDisplayAdvertisable(hasUsableDrmDisplay, width, height);
}

bool
XWindowsScreen::isDrmConnectorEnterableForTest(const std::string& status,
											   const std::string& enabled,
											   const std::string& dpms)
{
	return isDrmConnectorEnterable(status, enabled, dpms);
}

bool
XWindowsScreen::clampPointToRectForTest(SInt32 rx, SInt32 ry,
										SInt32 rw, SInt32 rh,
										SInt32& x, SInt32& y)
{
	return clampPointToRect("test point", rx, ry, rw, rh, x, y);
}

bool
XWindowsScreen::adjustPointToVisibleAreaForTest(const VisibleAreas& areas,
												SInt32& x, SInt32& y)
{
	return adjustPointToVisibleAreas("test point", areas, x, y);
}

bool
XWindowsScreen::adjustPointToVisibleAreaNearAnchorForTest(
	const VisibleAreas& areas, SInt32 anchorX, SInt32 anchorY,
	SInt32& x, SInt32& y)
{
	return adjustPointToVisibleAreasNearAnchor("test point", areas,
		anchorX, anchorY, x, y);
}

bool
XWindowsScreen::visibleAreaTopologiesEqualForTest(
	const VisibleAreas& first, const VisibleAreas& second)
{
	return visibleAreaTopologiesEqual(first, second);
}

bool
XWindowsScreen::isExternalSelectionOwnerChangeForTest(
	int eventType, int selectionEventType, int subtype, int setOwnerSubtype,
	Window owner, Window ownWindow)
{
	return eventType == selectionEventType && subtype == setOwnerSubtype &&
		owner != None && owner != ownWindow;
}

bool
XWindowsScreen::shouldObserveClipboardOnCheckForTest(
	bool hasXFixes, Window owner, Window knownOwner, Window ownWindow)
{
	const bool external = owner != None && owner != ownWindow;
	return owner != knownOwner || (!hasXFixes && external);
}

double
XWindowsScreen::primaryLeaveGrabTimeoutForTest(bool lowLatencyMode)
{
	return lowLatencyMode ?
		kLowLatencyPrimaryLeaveGrabTimeoutSeconds :
		kPrimaryLeaveGrabTimeoutSeconds;
}

double
XWindowsScreen::primaryLeaveGrabRetrySleepForTest(bool lowLatencyMode)
{
	return lowLatencyMode ?
		kLowLatencyPrimaryLeaveGrabRetrySleepSeconds :
		kPrimaryLeaveGrabRetrySleepSeconds;
}

#ifdef HAVE_XI2
bool
XWindowsScreen::xInputCookieUsableForTest(
	const XGenericEventCookie& cookie, int expectedExtension)
{
	return cookie.type == GenericEvent &&
		cookie.extension == expectedExtension && cookie.data != NULL;
}

bool
XWindowsScreen::xInputEventNeedsPayloadForTest(int eventType)
{
	return eventType == XI_RawButtonPress ||
		eventType == XI_RawButtonRelease;
}
#endif

void*
XWindowsScreen::getEventTarget() const
{
	return const_cast<XWindowsScreen*>(this);
}

bool
XWindowsScreen::queueClipboardSnapshot(ClipboardID id, Window owner,
	Time timestamp, bool announceGrab)
{
	if (!m_clipboardSnapshotContext || owner == None || owner == m_window) {
		return false;
	}

	if (m_clipboardSnapshotThread == NULL) {
		const std::shared_ptr<XWindowsClipboardSnapshotWorkerContext> context =
			m_clipboardSnapshotContext;
		m_clipboardSnapshotThread = new Thread([context]() {
			runClipboardSnapshotWorker(context);
		});
	}

	std::lock_guard<std::mutex> lock(m_clipboardSnapshotContext->mutex);
	if (m_clipboardSnapshotContext->stopping) {
		return false;
	}
	if (m_clipboardSnapshotContext->clipboardChangedEvent == Event::kUnknown) {
		m_clipboardSnapshotContext->clipboardChangedEvent =
			m_events->forClipboard().clipboardChanged();
	}
	// Enqueue ownership before making the snapshot visible. The worker takes
	// this same mutex, so even a spurious condition-variable wake cannot post
	// clipboardChanged ahead of clipboardGrabbed.
	if (announceGrab) {
		sendClipboardEvent(m_events->forClipboard().clipboardGrabbed(), id);
	}
	m_clipboardSnapshotContext->snapshots.queue(
		id, owner, timestamp, m_sequenceNumber);
	return true;
}

void
XWindowsScreen::wakeClipboardSnapshotWorker()
{
	if (m_clipboardSnapshotContext) {
		m_clipboardSnapshotContext->wake.notify_one();
	}
}

void
XWindowsScreen::observeClipboardOwner(ClipboardID id, Window owner,
	Time timestamp, bool contentChanged, bool announceGrab)
{
	if (id >= kClipboardEnd) {
		return;
	}

	if (!m_clipboardSnapshotContext) {
		// --no-xinitthreads cannot safely run Xlib on a worker. Suppress the
		// notification instead of causing Server to read an arbitrary provider
		// on its input EventQueue.
		return;
	}

	Window knownOwner = None;
	{
		std::lock_guard<std::mutex> lock(
			m_clipboardSnapshotContext->mutex);
		knownOwner = m_clipboardSnapshotContext->snapshots.currentOwner(id);
	}
	if (!contentChanged && owner == knownOwner) {
		return;
	}

	if (owner == None || owner == m_window) {
		std::lock_guard<std::mutex> lock(
			m_clipboardSnapshotContext->mutex);
		m_clipboardSnapshotContext->snapshots.invalidate(id, owner);
		return;
	}

	m_clipboard[id]->lost(timestamp);
	const bool queued = queueClipboardSnapshot(
		id, owner, timestamp, announceGrab);
	if (queued) {
		wakeClipboardSnapshotWorker();
	}
}

void
XWindowsScreen::bootstrapClipboardSnapshots()
{
	if (m_clipboardSnapshotsBootstrapped) {
		return;
	}
	m_clipboardSnapshotsBootstrapped = true;

	if (!m_clipboardSnapshotContext) {
		return;
	}
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		const Window owner = m_impl->XGetSelectionOwner(
			m_display, m_clipboard[id]->getSelection());
		observeClipboardOwner(
			id, owner, CurrentTime, true,
			owner != None && owner != m_window);
	}
}

void
XWindowsScreen::stopClipboardSnapshotWorker()
{
	const std::shared_ptr<XWindowsClipboardSnapshotWorkerContext> context =
		m_clipboardSnapshotContext;
	if (context) {
		{
			std::lock_guard<std::mutex> lock(context->mutex);
			context->stopping = true;
			for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
				context->snapshots.invalidate(id, None);
			}
		}
		context->wake.notify_all();
	}

	if (m_clipboardSnapshotThread != NULL) {
		m_clipboardSnapshotThread->cancel();
		if (!m_clipboardSnapshotThread->wait(
				kClipboardSnapshotShutdownWaitSeconds)) {
			LOG((CLOG_WARN
				"X11 clipboard snapshot worker did not stop within %.3fs; detaching its isolated context",
				kClipboardSnapshotShutdownWaitSeconds));
		}
		delete m_clipboardSnapshotThread;
		m_clipboardSnapshotThread = NULL;
	}
	m_clipboardSnapshotContext.reset();
}

bool
XWindowsScreen::getClipboard(ClipboardID id, IClipboard* clipboard) const
{
	assert(clipboard != NULL);

	// fail if we don't have the requested clipboard
	if (m_clipboard[id] == NULL) {
		return false;
	}

	if (m_clipboardSnapshotContext) {
		std::shared_ptr<const String> snapshot;
		Window snapshotOwner = None;
		Time snapshotTime = CurrentTime;
		bool blocksSynchronousRead = false;
		{
			std::lock_guard<std::mutex> lock(
				m_clipboardSnapshotContext->mutex);
			m_clipboardSnapshotContext->snapshots.copyReady(
				id, &snapshot, &snapshotOwner, &snapshotTime);
			blocksSynchronousRead =
				m_clipboardSnapshotContext->snapshots.blocksSynchronousRead(id);
		}

		if (snapshot) {
			// A worker can finish just before the main queue receives the next
			// XFixes event. Verify the owner with the X server before exposing
			// the cached bytes; this query never calls the clipboard provider.
			const Window currentOwner = m_impl->XGetSelectionOwner(
				m_display, m_clipboard[id]->getSelection());
			if (currentOwner != snapshotOwner) {
				std::lock_guard<std::mutex> lock(
					m_clipboardSnapshotContext->mutex);
				m_clipboardSnapshotContext->snapshots.invalidate(
					id, currentOwner);
				return false;
			}

			IClipboard::unmarshall(clipboard, *snapshot, snapshotTime);
			return true;
		}
		if (blocksSynchronousRead) {
			// Once an external-owner read enters the isolated path, failure and
			// retries remain isolated too. Falling back here would put the same
			// provider stall back onto the input event queue.
			return false;
		}

		const Window currentOwner = m_impl->XGetSelectionOwner(
			m_display, m_clipboard[id]->getSelection());
		if (currentOwner != m_window) {
			// A caller reached us before bootstrap/XFixes. Start the same isolated
			// path and ask it to retry later; never discover an external provider
			// by synchronously reading it here.
			const_cast<XWindowsScreen*>(this)->observeClipboardOwner(
				id, currentOwner, CurrentTime, true, false);
			return false;
		}
	}
	else if (!m_clipboardSnapshotThreadingEnabled) {
		return false;
	}

	// get the actual time.  ICCCM does not allow CurrentTime.
	Time timestamp = XWindowsUtil::getCurrentTime(
								m_display, m_clipboard[id]->getWindow());

	// copy the clipboard
	return Clipboard::copy(clipboard, m_clipboard[id], timestamp);
}

void
XWindowsScreen::getShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h) const
{
	x = m_x;
	y = m_y;
	const bool drmUsable = m_isPrimary ? true : hasUsableDrmDisplay();
	if (!m_isPrimary && !isSecondaryDisplayAdvertisable(drmUsable, m_w, m_h)) {
		LOG((CLOG_WARN "secondary X11 display is not usable; advertising unavailable shape drmUsable=%d shape=%d,%d %dx%d",
			drmUsable ? 1 : 0, m_x, m_y, m_w, m_h));
		w = 0;
		h = 0;
		return;
	}

	w = m_w;
	h = m_h;
}

void
XWindowsScreen::getCursorPos(SInt32& x, SInt32& y) const
{
	Window root, window;
	int mx, my, xWindow, yWindow;
	unsigned int mask;
    if (m_impl->XQueryPointer(m_display, m_root, &root, &window,
								&mx, &my, &xWindow, &yWindow, &mask)) {
		x = mx;
		y = my;
	}
	else {
		x = m_xCenter;
		y = m_yCenter;
	}
}

bool
XWindowsScreen::adjustPointToVisibleAreaNearAnchor(SInt32 anchorX,
												   SInt32 anchorY,
												   SInt32& x,
												   SInt32& y)
{
	updateVisibleAreasFromRandR();
	return adjustPointToVisibleAreasNearAnchor("primary return",
		m_visibleAreas, anchorX, anchorY, x, y);
}

void
XWindowsScreen::reconfigure(UInt32)
{
	// do nothing
}

void
XWindowsScreen::warpCursor(SInt32 x, SInt32 y)
{
	if (!normalizeToScreenShape("cursor warp", m_x, m_y, m_w, m_h, x, y)) {
		return;
	}
	if (m_isPrimary) {
		updateVisibleAreasFromRandR();
		if (!adjustPointToVisibleArea("primary cursor warp", x, y)) {
			return;
		}
	}

	// warp mouse
	warpCursorNoFlush(x, y);

	// remove all input events before and including warp
	XEvent event;
    while (m_impl->XCheckMaskEvent(m_display, PointerMotionMask |
								ButtonPressMask | ButtonReleaseMask |
								KeyPressMask | KeyReleaseMask |
								KeymapStateMask,
								&event)) {
		// do nothing
	}

	// save position as last position
	m_xCursor = x;
	m_yCursor = y;
}

UInt32
XWindowsScreen::registerHotKey(KeyID key, KeyModifierMask mask)
{
	// only allow certain modifiers
	if ((mask & ~(KeyModifierShift | KeyModifierControl |
				  KeyModifierAlt   | KeyModifierSuper)) != 0) {
		LOG((CLOG_DEBUG "could not map hotkey id=%04x mask=%04x", key, mask));
		return 0;
	}

	// fail if no keys
	if (key == kKeyNone && mask == 0) {
		return 0;
	}

	// convert to X
	unsigned int modifiers;
	if (!m_keyState->mapModifiersToX(mask, modifiers)) {
		// can't map all modifiers
		LOG((CLOG_DEBUG "could not map hotkey id=%04x mask=%04x", key, mask));
		return 0;
	}
	XWindowsKeyState::KeycodeList keycodes;
	m_keyState->mapKeyToKeycodes(key, keycodes);
	if (key != kKeyNone && keycodes.empty()) {
		// can't map key
		LOG((CLOG_DEBUG "could not map hotkey id=%04x mask=%04x", key, mask));
		return 0;
	}

	// choose hotkey id
	UInt32 id;
	if (!m_oldHotKeyIDs.empty()) {
		id = m_oldHotKeyIDs.back();
		m_oldHotKeyIDs.pop_back();
	}
	else {
		id = m_hotKeys.size() + 1;
	}
	HotKeyList& hotKeys = m_hotKeys[id];

	// all modifier hotkey must be treated specially.  for each modifier
	// we need to grab the modifier key in combination with all the other
	// requested modifiers.
	bool err = false;
	{
		XWindowsUtil::ErrorLock lock(m_display, &err);
		if (key == kKeyNone) {
			static const KeyModifierMask s_hotKeyModifiers[] = {
				KeyModifierShift,
				KeyModifierControl,
				KeyModifierAlt,
				KeyModifierMeta,
				KeyModifierSuper
			};

            XModifierKeymap* modKeymap = m_impl->XGetModifierMapping(m_display);
			for (size_t j = 0; j < sizeof(s_hotKeyModifiers) /
									sizeof(s_hotKeyModifiers[0]) && !err; ++j) {
				// skip modifier if not in mask
				if ((mask & s_hotKeyModifiers[j]) == 0) {
					continue;
				}

				// skip with error if we can't map remaining modifiers
				unsigned int modifiers2;
				KeyModifierMask mask2 = (mask & ~s_hotKeyModifiers[j]);
				if (!m_keyState->mapModifiersToX(mask2, modifiers2)) {
					err = true;
					continue;
				}

				// compute modifier index for modifier.  there should be
				// exactly one X modifier missing
				int index;
				switch (modifiers ^ modifiers2) {
				case ShiftMask:
					index = ShiftMapIndex;
					break;

				case LockMask:
					index = LockMapIndex;
					break;

				case ControlMask:
					index = ControlMapIndex;
					break;

				case Mod1Mask:
					index = Mod1MapIndex;
					break;

				case Mod2Mask:
					index = Mod2MapIndex;
					break;

				case Mod3Mask:
					index = Mod3MapIndex;
					break;

				case Mod4Mask:
					index = Mod4MapIndex;
					break;

				case Mod5Mask:
					index = Mod5MapIndex;
					break;

				default:
					err = true;
					continue;
				}

				// grab each key for the modifier
				const KeyCode* modifiermap =
					modKeymap->modifiermap + index * modKeymap->max_keypermod;
				for (int k = 0; k < modKeymap->max_keypermod && !err; ++k) {
					KeyCode code = modifiermap[k];
					if (modifiermap[k] != 0) {
                        m_impl->XGrabKey(m_display, code, modifiers2, m_root,
									False, GrabModeAsync, GrabModeAsync);
						if (!err) {
							hotKeys.push_back(std::make_pair(code, modifiers2));
							m_hotKeyToIDMap[HotKeyItem(code, modifiers2)] = id;
						}
					}
				}
			}
            m_impl->XFreeModifiermap(modKeymap);
		}

		// a non-modifier key must be insensitive to CapsLock, NumLock and
		// ScrollLock, so we have to grab the key with every combination of
		// those.
		else {
			// collect available toggle modifiers
			unsigned int modifier;
			unsigned int toggleModifiers[3];
			size_t numToggleModifiers = 0;
			if (m_keyState->mapModifiersToX(KeyModifierCapsLock, modifier)) {
				toggleModifiers[numToggleModifiers++] = modifier;
			}
			if (m_keyState->mapModifiersToX(KeyModifierNumLock, modifier)) {
				toggleModifiers[numToggleModifiers++] = modifier;
			}
			if (m_keyState->mapModifiersToX(KeyModifierScrollLock, modifier)) {
				toggleModifiers[numToggleModifiers++] = modifier;
			}


			for (XWindowsKeyState::KeycodeList::iterator j = keycodes.begin();
									j != keycodes.end() && !err; ++j) {
				for (size_t i = 0; i < (1u << numToggleModifiers); ++i) {
					// add toggle modifiers for index i
					unsigned int tmpModifiers = modifiers;
					if ((i & 1) != 0) {
						tmpModifiers |= toggleModifiers[0];
					}
					if ((i & 2) != 0) {
						tmpModifiers |= toggleModifiers[1];
					}
					if ((i & 4) != 0) {
						tmpModifiers |= toggleModifiers[2];
					}

					// add grab
                    m_impl->XGrabKey(m_display, *j, tmpModifiers, m_root,
										False, GrabModeAsync, GrabModeAsync);
					if (!err) {
						hotKeys.push_back(std::make_pair(*j, tmpModifiers));
						m_hotKeyToIDMap[HotKeyItem(*j, tmpModifiers)] = id;
					}
				}
			}
		}
	}

	if (err) {
		// if any failed then unregister any we did get
		for (HotKeyList::iterator j = hotKeys.begin();
								j != hotKeys.end(); ++j) {
            m_impl->XUngrabKey(m_display, j->first, j->second, m_root);
			m_hotKeyToIDMap.erase(HotKeyItem(j->first, j->second));
		}

		m_oldHotKeyIDs.push_back(id);
		m_hotKeys.erase(id);
		LOG((CLOG_WARN "failed to register hotkey %s (id=%04x mask=%04x)", barrier::KeyMap::formatKey(key, mask).c_str(), key, mask));
		return 0;
	}

	LOG((CLOG_DEBUG "registered hotkey %s (id=%04x mask=%04x) as id=%d", barrier::KeyMap::formatKey(key, mask).c_str(), key, mask, id));
	return id;
}

void
XWindowsScreen::unregisterHotKey(UInt32 id)
{
	// look up hotkey
	HotKeyMap::iterator i = m_hotKeys.find(id);
	if (i == m_hotKeys.end()) {
		return;
	}

	// unregister with OS
	bool err = false;
	{
		XWindowsUtil::ErrorLock lock(m_display, &err);
		HotKeyList& hotKeys = i->second;
		for (HotKeyList::iterator j = hotKeys.begin();
								j != hotKeys.end(); ++j) {
            m_impl->XUngrabKey(m_display, j->first, j->second, m_root);
			m_hotKeyToIDMap.erase(HotKeyItem(j->first, j->second));
		}
	}
	if (err) {
		LOG((CLOG_WARN "failed to unregister hotkey id=%d", id));
	}
	else {
		LOG((CLOG_DEBUG "unregistered hotkey id=%d", id));
	}

	// discard hot key from map and record old id for reuse
	m_hotKeys.erase(i);
	m_oldHotKeyIDs.push_back(id);
}

void
XWindowsScreen::fakeInputBegin()
{
	// FIXME -- not implemented
}

void
XWindowsScreen::fakeInputEnd()
{
	// FIXME -- not implemented
}

SInt32
XWindowsScreen::getJumpZoneSize() const
{
	return 1;
}

bool
XWindowsScreen::isAnyMouseButtonDown(UInt32& buttonID) const
{
	// query the pointer to get the button state
	Window root, window;
	int xRoot, yRoot, xWindow, yWindow;
	unsigned int state;
    if (m_impl->XQueryPointer(m_display, m_root, &root, &window,
								&xRoot, &yRoot, &xWindow, &yWindow, &state)) {
		return ((state & (Button1Mask | Button2Mask | Button3Mask |
							Button4Mask | Button5Mask)) != 0);
	}

	return false;
}

void
XWindowsScreen::getCursorCenter(SInt32& x, SInt32& y) const
{
	x = m_xCenter;
	y = m_yCenter;
}

void
XWindowsScreen::fakeMouseButton(ButtonID button, bool press)
{
	const unsigned int xButton = mapButtonToX(button);
	if (xButton > 0 && xButton < 11) {
        m_impl->XTestFakeButtonEvent(m_display, xButton,
							press ? True : False, CurrentTime);
        m_impl->XFlush(m_display);
	}
}

void
XWindowsScreen::fakeMouseMove(SInt32 x, SInt32 y)
{
	if (!normalizeToScreenShape("remote mouse move", m_x, m_y, m_w, m_h, x, y)) {
		return;
	}

	if (m_xinerama && m_xtestIsXineramaUnaware) {
        m_impl->XWarpPointer(m_display, None, m_root, 0, 0, 0, 0, x, y);
	}
	else {
		XTestFakeMotionEvent(m_display, DefaultScreen(m_display),
							x, y, CurrentTime);
	}
    m_impl->XFlush(m_display);
}

void
XWindowsScreen::fakeMouseRelativeMove(SInt32 dx, SInt32 dy) const
{
	// FIXME -- ignore xinerama for now
	if (false && m_xinerama && m_xtestIsXineramaUnaware) {
//		m_impl->XWarpPointer(m_display, None, m_root, 0, 0, 0, 0, x, y);
	}
	else {
        m_impl->XTestFakeRelativeMotionEvent(m_display, dx, dy, CurrentTime);
	}
    m_impl->XFlush(m_display);
}

void
XWindowsScreen::fakeMouseWheel(SInt32 xDelta, SInt32 yDelta) const
{
    int numEvents;

    if ((!xDelta && !yDelta) || (xDelta && yDelta)) {
        // Invalid scrolling inputs
        return;
    }

    // 4,  5,    6,    7
    // up, down, left, right
    unsigned int xButton;

    if (yDelta) { // vertical scroll
        numEvents = y_accumulateMouseScroll(yDelta);
        if (numEvents >= 0) {
            xButton = 4; // up
        }
        else {
            xButton = 5; // down
        }
    }
    else { // horizontal scroll
        numEvents = x_accumulateMouseScroll(xDelta);
        if (numEvents >= 0) {
            xButton = 7; // right
        }
        else {
            xButton = 6; // left
        }
    }

    numEvents = std::abs(numEvents);

	// send as many clicks as necessary
    for (; numEvents > 0; numEvents--) {
        m_impl->XTestFakeButtonEvent(m_display, xButton, True, CurrentTime);
        m_impl->XTestFakeButtonEvent(m_display, xButton, False, CurrentTime);
	}

    m_impl->XFlush(m_display);
}

Display*
XWindowsScreen::openDisplay(const char* displayName)
{
	// get the DISPLAY
	if (displayName == NULL) {
		displayName = std::getenv("DISPLAY");
		if (displayName == NULL) {
			displayName = ":0.0";
		}
	}

	// open the display
	LOG((CLOG_DEBUG "XOpenDisplay(\"%s\")", displayName));
    Display* display = m_impl->XOpenDisplay(displayName);
	if (display == NULL) {
		throw XScreenUnavailable(60.0);
	}

	// verify the availability of the XTest extension
	if (!m_isPrimary) {
		int majorOpcode, firstEvent, firstError;
        if (!m_impl->XQueryExtension(display, XTestExtensionName,
							&majorOpcode, &firstEvent, &firstError)) {
			LOG((CLOG_ERR "XTEST extension not available"));
            m_impl->XCloseDisplay(display);
			throw XScreenOpenFailure();
		}
	}

#if HAVE_XKB_EXTENSION
	{
		m_xkb = false;
		int major = XkbMajorVersion, minor = XkbMinorVersion;
        if (m_impl->XkbLibraryVersion(&major, &minor)) {
			int opcode, firstError;
            if (m_impl->XkbQueryExtension(display, &opcode, &m_xkbEventBase,
								&firstError, &major, &minor)) {
				m_xkb = true;
                m_impl->XkbSelectEvents(display, XkbUseCoreKbd,
								XkbMapNotifyMask, XkbMapNotifyMask);
                m_impl->XkbSelectEventDetails(display, XkbUseCoreKbd,
								XkbStateNotifyMask,
								XkbGroupStateMask, XkbGroupStateMask);
			}
		}
	}
#endif

#if HAVE_X11_EXTENSIONS_XRANDR_H
	// query for XRandR extension
	int dummyError;
	m_xrandr = m_impl->XRRQueryExtension(display, &m_xrandrEventBase, &dummyError);
	if (m_xrandr) {
		// Enable screen, CRTC, and output notifications. Monitor unplug/replug
		// can arrive as an output change without a separate CRTC event.
		m_impl->XRRSelectInput(display, DefaultRootWindow(display),
			RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
			RROutputChangeNotifyMask | RRProviderChangeNotifyMask |
			RRResourceChangeNotifyMask);
	}
#endif

	return display;
}

void
XWindowsScreen::saveShape()
{
	// get shape of default screen
	m_x = 0;
	m_y = 0;

	m_w = WidthOfScreen(DefaultScreenOfDisplay(m_display));
	m_h = HeightOfScreen(DefaultScreenOfDisplay(m_display));

	// get center of default screen
	m_xCenter = m_x + (m_w >> 1);
	m_yCenter = m_y + (m_h >> 1);
	VisibleAreas areas;
	areas.push_back(VisibleArea(m_x, m_y, m_w, m_h, true));
	setVisibleAreas(areas);

	if (updateVisibleAreasFromRandR()) {
		return;
	}

	// check if xinerama is enabled and there is more than one screen.
	// get center of first Xinerama screen.  Xinerama appears to have
	// a bug when XWarpPointer() is used in combination with
	// XGrabPointer().  in that case, the warp is successful but the
	// next pointer motion warps the pointer again, apparently to
	// constrain it to some unknown region, possibly the region from
	// 0,0 to Wm,Hm where Wm (Hm) is the minimum width (height) over
	// all physical screens.  this warp only seems to happen if the
	// pointer wasn't in that region before the XWarpPointer().  the
	// second (unexpected) warp causes barrier to think the pointer
	// has been moved when it hasn't.  to work around the problem,
	// we warp the pointer to the center of the first physical
	// screen instead of the logical screen.
	m_xinerama = false;
#if HAVE_X11_EXTENSIONS_XINERAMA_H
	int eventBase, errorBase;
    if (m_impl->XineramaQueryExtension(m_display, &eventBase, &errorBase) &&
        m_impl->XineramaIsActive(m_display)) {
		int numScreens;
		XineramaScreenInfo* screens;
        screens = reinterpret_cast<XineramaScreenInfo*>(
            XineramaQueryScreens(m_display, &numScreens));

		if (screens != NULL) {
			VisibleAreas xineramaAreas;
			if (numScreens > 1) {
				m_xinerama = true;
			}
			for (int i = 0; i < numScreens; ++i) {
				xineramaAreas.push_back(VisibleArea(screens[i].x_org,
					screens[i].y_org, screens[i].width, screens[i].height,
					i == 0));
			}
			if (!xineramaAreas.empty()) {
				setVisibleAreas(xineramaAreas);
			}
			XFree(screens);
		}
	}
#endif
}

void
XWindowsScreen::setPrimaryVisibleArea(SInt32 x, SInt32 y,
									  SInt32 width, SInt32 height)
{
	if (width <= 0 || height <= 0) {
		return;
	}

	m_primaryVisibleX = x;
	m_primaryVisibleY = y;
	m_primaryVisibleW = width;
	m_primaryVisibleH = height;
	m_xCenter = x + (width >> 1);
	m_yCenter = y + (height >> 1);
}

void
XWindowsScreen::setVisibleAreas(const VisibleAreas& areas)
{
	VisibleAreas validAreas;
	VisibleAreas::const_iterator fallback = areas.end();
	VisibleAreas::const_iterator primary = areas.end();
	for (VisibleAreas::const_iterator i = areas.begin(); i != areas.end();
		++i) {
		if (i->width <= 0 || i->height <= 0) {
			continue;
		}
		if (fallback == areas.end()) {
			fallback = i;
		}
		if (i->primary && primary == areas.end()) {
			primary = i;
		}
		validAreas.push_back(*i);
	}

	if (validAreas.empty()) {
		return;
	}

	m_visibleAreas = validAreas;
	const VisibleArea& centerArea =
		(primary != areas.end()) ? *primary : *fallback;
	setPrimaryVisibleArea(centerArea.x, centerArea.y, centerArea.width,
		centerArea.height);
}

bool
XWindowsScreen::updateVisibleAreasFromRandR()
{
#if HAVE_X11_EXTENSIONS_XRANDR_H
	if (!m_xrandr) {
		return false;
	}

	Window root = DefaultRootWindow(m_display);
	RROutput primary = XRRGetOutputPrimary(m_display, root);

	XRRScreenResources* resources =
		XRRGetScreenResourcesCurrent(m_display, root);
	if (resources == NULL) {
		return false;
	}

	VisibleAreas areas;
	for (int i = 0; i < resources->noutput; ++i) {
		RROutput output = resources->outputs[i];
		XRROutputInfo* outputInfo =
			XRRGetOutputInfo(m_display, resources, output);
		if (outputInfo == NULL) {
			continue;
		}

		if (outputInfo->connection == RR_Connected &&
			outputInfo->crtc != None) {
			XRRCrtcInfo* crtcInfo =
				XRRGetCrtcInfo(m_display, resources, outputInfo->crtc);
			if (crtcInfo != NULL) {
				if (crtcInfo->width > 0 && crtcInfo->height > 0) {
					VisibleArea area(crtcInfo->x, crtcInfo->y,
						crtcInfo->width, crtcInfo->height,
						output == primary);
					bool duplicate = false;
					for (VisibleAreas::iterator j = areas.begin();
						j != areas.end(); ++j) {
						if (visibleAreaEquals(*j, area)) {
							j->primary = j->primary || area.primary;
							duplicate = true;
							break;
						}
					}
					if (!duplicate) {
						areas.push_back(area);
					}
				}
				XRRFreeCrtcInfo(crtcInfo);
			}
		}
		XRRFreeOutputInfo(outputInfo);
	}
	XRRFreeScreenResources(resources);
	if (areas.empty()) {
		return false;
	}
	setVisibleAreas(areas);
	return true;
#else
	return false;
#endif
}

bool
XWindowsScreen::adjustPointToVisibleArea(const char* operation,
										 SInt32& x, SInt32& y) const
{
	return adjustPointToVisibleAreas(operation, m_visibleAreas, x, y);
}

Window
XWindowsScreen::openWindow() const
{
	// default window attributes.  we don't want the window manager
	// messing with our window and we don't want the cursor to be
	// visible inside the window.
	XSetWindowAttributes attr;
	attr.do_not_propagate_mask = 0;
	attr.override_redirect     = True;
	attr.cursor                = createBlankCursor();

	// adjust attributes and get size and shape
	SInt32 x, y, w, h;
	if (m_isPrimary) {
		// grab window attributes.  this window is used to capture user
		// input when the user is focused on another client.  it covers
		// the whole screen.
		attr.event_mask = PointerMotionMask |
							 ButtonPressMask | ButtonReleaseMask |
							 KeyPressMask | KeyReleaseMask |
							 KeymapStateMask | PropertyChangeMask;
		x = m_x;
		y = m_y;
		w = m_w;
		h = m_h;
	}
	else {
		// cursor hider window attributes.  this window is used to hide the
		// cursor when it's not on the screen.  the window is hidden as soon
		// as the cursor enters the screen or the display's real mouse is
		// moved.  we'll reposition the window as necessary so its
		// position here doesn't matter.  it only needs to be 1x1 because
		// it only needs to contain the cursor's hotspot.
		attr.event_mask = LeaveWindowMask;
		x = 0;
		y = 0;
		w = 1;
		h = 1;
	}

	// create and return the window
    Window window = m_impl->XCreateWindow(m_display, m_root, x, y, w, h, 0, 0,
							InputOnly, CopyFromParent,
							CWDontPropagate | CWEventMask |
							CWOverrideRedirect | CWCursor,
							&attr);
	if (window == None) {
		throw XScreenOpenFailure();
	}
	return window;
}

void
XWindowsScreen::openIM()
{
	// open the input methods
    XIM im = m_impl->XOpenIM(m_display, NULL, NULL, NULL);
	if (im == NULL) {
		LOG((CLOG_INFO "no support for IM"));
		return;
	}

	// find the appropriate style.  barrier supports XIMPreeditNothing
	// only at the moment.
	XIMStyles* styles;
    if (m_impl->XGetIMValues(im, XNQueryInputStyle, &styles) != nullptr ||
		styles == NULL) {
		LOG((CLOG_WARN "cannot get IM styles"));
        m_impl->XCloseIM(im);
		return;
	}
	XIMStyle style = 0;
	for (unsigned short i = 0; i < styles->count_styles; ++i) {
		style = styles->supported_styles[i];
		if ((style & XIMPreeditNothing) != 0) {
			if ((style & (XIMStatusNothing | XIMStatusNone)) != 0) {
				break;
			}
		}
	}
	XFree(styles);
	if (style == 0) {
		LOG((CLOG_INFO "no supported IM styles"));
        m_impl->XCloseIM(im);
		return;
	}

	// create an input context for the style and tell it about our window
    XIC ic = m_impl->XCreateIC(im, XNInputStyle, style, XNClientWindow, m_window);
	if (ic == NULL) {
		LOG((CLOG_WARN "cannot create IC"));
        m_impl->XCloseIM(im);
		return;
	}

	// find out the events we must select for and do so
	unsigned long mask;
    if (m_impl->XGetICValues(ic, XNFilterEvents, &mask) != NULL) {
		LOG((CLOG_WARN "cannot get IC filter events"));
        m_impl->XDestroyIC(ic);
        m_impl->XCloseIM(im);
		return;
	}

	// we have IM
	m_im          = im;
	m_ic          = ic;
	m_lastKeycode = 0;

	// select events on our window that IM requires
	XWindowAttributes attr;
    m_impl->XGetWindowAttributes(m_display, m_window, &attr);
    m_impl->XSelectInput(m_display, m_window, attr.your_event_mask | mask);
}

void
XWindowsScreen::sendEvent(Event::Type type, void* data)
{
	m_events->addEvent(Event(type, getEventTarget(), data));
}

void
XWindowsScreen::sendClipboardEvent(Event::Type type, ClipboardID id)
{
	ClipboardInfo* info   = (ClipboardInfo*)malloc(sizeof(ClipboardInfo));
	info->m_id             = id;
	info->m_sequenceNumber = m_sequenceNumber;
	sendEvent(type, info);
}

IKeyState*
XWindowsScreen::getKeyState() const
{
	return m_keyState;
}

Bool
XWindowsScreen::findKeyEvent(Display*, XEvent* xevent, XPointer arg)
{
	KeyEventFilter* filter = reinterpret_cast<KeyEventFilter*>(arg);
	return (xevent->type         == filter->m_event &&
			xevent->xkey.window  == filter->m_window &&
			xevent->xkey.time    == filter->m_time &&
			xevent->xkey.keycode == filter->m_keycode) ? True : False;
}

void
XWindowsScreen::handleSystemEvent(const Event& event, void*)
{
	XEvent* xevent = static_cast<XEvent*>(event.getData());
	assert(xevent != NULL);

#ifdef HAVE_XFIXES
	if (m_xfixes &&
		xevent->type == m_xfixesEventBase + XFixesSelectionNotify) {
		const XFixesSelectionNotifyEvent* selectionEvent =
			reinterpret_cast<const XFixesSelectionNotifyEvent*>(xevent);
		const ClipboardID id = getClipboardID(selectionEvent->selection);
		if (id != kClipboardEnd &&
			selectionEvent->subtype == XFixesSetSelectionOwnerNotify) {
			const bool external = isExternalSelectionOwnerChangeForTest(
				selectionEvent->type,
				m_xfixesEventBase + XFixesSelectionNotify,
				selectionEvent->subtype,
				XFixesSetSelectionOwnerNotify,
				selectionEvent->owner,
				m_window);
			// Self and None ownership changes still advance the generation so
			// an old external snapshot can never overwrite a remote clipboard.
			observeClipboardOwner(
				id, selectionEvent->owner, selectionEvent->timestamp, true,
				external);
		}
		return;
	}
#endif

	// update key state
	bool isRepeat = false;
	if (m_isPrimary) {
		if (xevent->type == KeyRelease) {
			// check if this is a key repeat by getting the next
			// KeyPress event that has the same key and time as
			// this release event, if any.  first prepare the
			// filter info.
			KeyEventFilter filter;
			filter.m_event   = KeyPress;
			filter.m_window  = xevent->xkey.window;
			filter.m_time    = xevent->xkey.time;
			filter.m_keycode = xevent->xkey.keycode;
			XEvent xevent2;
            isRepeat = (m_impl->XCheckIfEvent(m_display, &xevent2,
							&XWindowsScreen::findKeyEvent,
							(XPointer)&filter) == True);
		}

		if (xevent->type == KeyPress || xevent->type == KeyRelease) {
			if (xevent->xkey.window == m_root) {
				// this is a hot key
				onHotKey(xevent->xkey, isRepeat);
				return;
			}
			else if (!m_isOnScreen) {
				// this might be a hot key
				if (onHotKey(xevent->xkey, isRepeat)) {
					return;
				}
			}

			bool down             = (isRepeat || xevent->type == KeyPress);
			KeyModifierMask state =
				m_keyState->mapModifiersFromX(xevent->xkey.state);
			m_keyState->onKey(xevent->xkey.keycode, down, state);
		}
	}

	// let input methods try to handle event first
	if (m_ic != NULL) {
		// XFilterEvent() may eat the event and generate a new KeyPress
		// event with a keycode of 0 because there isn't an actual key
		// associated with the keysym.  but the KeyRelease may pass
		// through XFilterEvent() and keep its keycode.  this means
		// there's a mismatch between KeyPress and KeyRelease keycodes.
		// since we use the keycode on the client to detect when a key
		// is released this won't do.  so we remember the keycode on
		// the most recent KeyPress (and clear it on a matching
		// KeyRelease) so we have a keycode for a synthesized KeyPress.
		if (xevent->type == KeyPress && xevent->xkey.keycode != 0) {
			m_lastKeycode = xevent->xkey.keycode;
		}
		else if (xevent->type == KeyRelease &&
			xevent->xkey.keycode == m_lastKeycode) {
			m_lastKeycode = 0;
		}

		// now filter the event
        if (m_impl->XFilterEvent(xevent, DefaultRootWindow(m_display))) {
			if (xevent->type == KeyPress) {
				// add filtered presses to the filtered list
				m_filtered.insert(m_lastKeycode);
			}
			return;
		}

		// discard matching key releases for key presses that were
		// filtered and remove them from our filtered list.
		else if (xevent->type == KeyRelease &&
			m_filtered.count(xevent->xkey.keycode) > 0) {
			m_filtered.erase(xevent->xkey.keycode);
			return;
		}
	}

	// let screen saver have a go
	if (m_screensaver->handleXEvent(xevent)) {
		// screen saver handled it
		return;
	}

#ifdef HAVE_XI2
	if (m_xi2detected && xevent->type == GenericEvent &&
		xevent->xcookie.extension == xi_opcode) {
		XGenericEventCookie* cookie = &xevent->xcookie;
		if (cookie->evtype == XI_RawMotion) {
			// Relative movement only needs the current pointer position. Avoid
			// depending on cookie payloads that some X servers omit for
			// synthetic or coalesced raw-motion notifications.
			XMotionEvent xmotion = {};
			xmotion.type = MotionNotify;
			xmotion.send_event = False;
			xmotion.display = m_display;
			xmotion.window = m_window;
			unsigned int msk;
			xmotion.same_screen = m_impl->XQueryPointer(
				m_display, m_root, &xmotion.root, &xmotion.subwindow,
				&xmotion.x_root, &xmotion.y_root, &xmotion.x, &xmotion.y,
				&msk);
			if (!xmotion.same_screen) {
				LOG((CLOG_WARN "resynchronizing XI2 raw motion after XQueryPointer failed"));
				m_xCursor = m_xCenter;
				m_yCursor = m_yCenter;
				if (!m_isOnScreen) {
					m_impl->XMoveWindow(m_display, m_window, m_xCenter, m_yCenter);
					fakeMouseMove(m_xCenter, m_yCenter);
				}
			}
			else if (normalizeToScreenShape("XI2 raw motion",
				m_x, m_y, m_w, m_h, xmotion.x_root, xmotion.y_root)) {
				onMouseMove(xmotion);
			}
			return;
		}

		if (!xInputEventNeedsPayloadForTest(cookie->evtype)) {
			return;
		}

		if (!m_impl->XGetEventData(m_display, cookie)) {
			LOG((CLOG_WARN "failed to acquire XI2 event data (type=%d)",
				cookie->evtype));
			return;
		}
		if (!xInputCookieUsableForTest(*cookie, xi_opcode)) {
			LOG((CLOG_WARN "ignoring XI2 event with invalid data (type=%d)",
				cookie->evtype));
			m_impl->XFreeEventData(m_display, cookie);
			return;
		}

		XIRawButtonEvent* btn = static_cast<XIRawButtonEvent*>(cookie->data);
		// Core ButtonPress/Release events handle buttons 1-7. XI2 is used
		// only for side buttons that core events do not reliably expose.
		if (btn->detail == 8 || btn->detail == 9) {
			handleXIRawButtonEvent(btn);
		}

		m_impl->XFreeEventData(m_display, cookie);
		return;
	}
#endif

	// handle the event ourself
	switch (xevent->type) {
	case CreateNotify:
		if (m_isPrimary && !m_xi2detected) {
			// select events on new window
			selectEvents(xevent->xcreatewindow.window);
		}
		break;

	case MappingNotify:
		refreshKeyboard(xevent);
		break;

	case LeaveNotify:
		if (!m_isPrimary) {
			// mouse moved out of hider window somehow.  hide the window.
            m_impl->XUnmapWindow(m_display, m_window);
		}
		break;

	case SelectionClear:
		{
			// we just lost the selection.  that means someone else
			// grabbed the selection so this screen is now the
			// selection owner.  report that to the receiver.
			ClipboardID id = getClipboardID(xevent->xselectionclear.selection);
			if (id != kClipboardEnd) {
				if (!m_xfixes) {
					const Window owner = m_impl->XGetSelectionOwner(
						m_display, m_clipboard[id]->getSelection());
					observeClipboardOwner(
						id, owner, xevent->xselectionclear.time, true,
						owner != None && owner != m_window);
				}
				else {
					m_clipboard[id]->lost(xevent->xselectionclear.time);
				}
				return;
			}
		}
		break;

	case SelectionNotify:
		// notification of selection transferred.  we shouldn't
		// get this here because we handle them in the selection
		// retrieval methods.  we'll just delete the property
		// with the data (satisfying the usual ICCCM protocol).
		if (xevent->xselection.property != None) {
            m_impl->XDeleteProperty(m_display,
								xevent->xselection.requestor,
								xevent->xselection.property);
		}
		break;

	case SelectionRequest:
		{
			// somebody is asking for clipboard data
			ClipboardID id = getClipboardID(
								xevent->xselectionrequest.selection);
			if (id != kClipboardEnd) {
				m_clipboard[id]->addRequest(
								xevent->xselectionrequest.owner,
								xevent->xselectionrequest.requestor,
								xevent->xselectionrequest.target,
								xevent->xselectionrequest.time,
								xevent->xselectionrequest.property);
				return;
			}
		}
		break;

	case PropertyNotify:
		// property delete may be part of a selection conversion
		if (xevent->xproperty.state == PropertyDelete) {
			processClipboardRequest(xevent->xproperty.window,
								xevent->xproperty.time,
								xevent->xproperty.atom);
		}
		break;

	case DestroyNotify:
		// looks like one of the windows that requested a clipboard
		// transfer has gone bye-bye.
		destroyClipboardRequest(xevent->xdestroywindow.window);
		break;

	case KeyPress:
		if (m_isPrimary) {
			onKeyPress(xevent->xkey);
		}
		return;

	case KeyRelease:
		if (m_isPrimary) {
			onKeyRelease(xevent->xkey, isRepeat);
		}
		return;

	case ButtonPress:
		if (m_isPrimary) {
			onMousePress(xevent->xbutton);
		}
		return;

	case ButtonRelease:
		if (m_isPrimary) {
			onMouseRelease(xevent->xbutton);
		}
		return;

	case MotionNotify:
		if (m_isPrimary) {
			onMouseMove(xevent->xmotion);
		}
		return;

	default:
#if HAVE_XKB_EXTENSION
		if (m_xkb && xevent->type == m_xkbEventBase) {
			XkbEvent* xkbEvent = reinterpret_cast<XkbEvent*>(xevent);
			switch (xkbEvent->any.xkb_type) {
			case XkbMapNotify:
				refreshKeyboard(xevent);
				return;

			case XkbStateNotify:
				LOG((CLOG_INFO "group change: %d", xkbEvent->state.group));
				m_keyState->setActiveGroup((SInt32)xkbEvent->state.group);
				return;
			}
		}
#endif

#if HAVE_X11_EXTENSIONS_XRANDR_H
		if (m_xrandr) {
			bool topologyChanged = (xevent->type == m_xrandrEventBase + RRScreenChangeNotify);
			if (!topologyChanged && xevent->type == m_xrandrEventBase + RRNotify) {
				const int subtype = reinterpret_cast<XRRNotifyEvent *>(xevent)->subtype;
				topologyChanged = (subtype == RRNotify_CrtcChange ||
					subtype == RRNotify_OutputChange ||
					subtype == RRNotify_ProviderChange ||
					subtype == RRNotify_ResourceChange);
			}
			if (topologyChanged) {
				// we're required to call back into XLib so XLib can update its internal state
				XRRUpdateConfiguration(xevent);

				const SInt32 oldX = m_x;
				const SInt32 oldY = m_y;
				const SInt32 oldWidth = m_w;
				const SInt32 oldHeight = m_h;
				const VisibleAreas oldVisibleAreas = m_visibleAreas;

				// requery/recalculate the screen shape
				saveShape();
				const bool shapeChanged = oldX != m_x || oldY != m_y ||
					oldWidth != m_w || oldHeight != m_h ||
					!visibleAreaTopologiesEqual(oldVisibleAreas, m_visibleAreas);
				if (!shapeChanged) {
					LOG((CLOG_DEBUG2 "ignoring duplicate XRandR topology notification"));
					return;
				}

				LOG((CLOG_INFO "XRandR screen topology changed from %d,%d %dx%d to %d,%d %dx%d",
					oldX, oldY, oldWidth, oldHeight, m_x, m_y, m_w, m_h));

				// we need to resize m_window, otherwise we'll get a weird problem where moving
				// off the server onto the client causes the pointer to warp to the
				// center of the server (so you can't move the pointer off the server)
				if (m_isPrimary) {
					m_impl->XMoveWindow(m_display, m_window, m_x, m_y);
					m_impl->XResizeWindow(m_display, m_window, m_w, m_h);
					if (!m_isOnScreen) {
						LOG((CLOG_DEBUG "reparking hidden cursor at %d,%d after screen shape change",
							m_xCenter, m_yCenter));
						warpCursor(m_xCenter, m_yCenter);
					}
				}
				else if (!m_isOnScreen) {
					LOG((CLOG_DEBUG "reparking secondary hidden cursor at %d,%d after screen shape change",
						m_xCenter, m_yCenter));
					m_impl->XMoveWindow(m_display, m_window, m_xCenter, m_yCenter);
					fakeMouseMove(m_xCenter, m_yCenter);
				}

				sendEvent(m_events->forIScreen().shapeChanged());
			}
		}
#endif

		break;
	}
}

void
XWindowsScreen::onKeyPress(XKeyEvent& xkey)
{
	LOG((CLOG_DEBUG1 "event: KeyPress code=%d, state=0x%04x", xkey.keycode, xkey.state));
	const KeyModifierMask mask = m_keyState->mapModifiersFromX(xkey.state);
	KeyID key                  = mapKeyFromX(&xkey);
	if (key != kKeyNone) {
		// check for ctrl+alt+del emulation
		if ((key == kKeyPause || key == kKeyBreak) &&
			(mask & (KeyModifierControl | KeyModifierAlt)) ==
					(KeyModifierControl | KeyModifierAlt)) {
			// pretend it's ctrl+alt+del
			LOG((CLOG_DEBUG "emulate ctrl+alt+del"));
			key = kKeyDelete;
		}

		// get which button.  see call to XFilterEvent() in onEvent()
		// for more info.
		bool isFake = false;
		KeyButton keycode = static_cast<KeyButton>(xkey.keycode);
		if (keycode == 0) {
			isFake  = true;
			keycode = static_cast<KeyButton>(m_lastKeycode);
			if (keycode == 0) {
				// no keycode
				LOG((CLOG_DEBUG1 "event: KeyPress no keycode"));
				return;
			}
		}

		// handle key
		m_keyState->sendKeyEvent(getEventTarget(),
							true, false, key, mask, 1, keycode);

		// do fake release if this is a fake press
		if (isFake) {
			m_keyState->sendKeyEvent(getEventTarget(),
							false, false, key, mask, 1, keycode);
		}
	}
    else {
		LOG((CLOG_DEBUG1 "can't map keycode to key id"));
    }
}

void
XWindowsScreen::onKeyRelease(XKeyEvent& xkey, bool isRepeat)
{
	const KeyModifierMask mask = m_keyState->mapModifiersFromX(xkey.state);
	KeyID key                  = mapKeyFromX(&xkey);
	if (key != kKeyNone) {
		// check for ctrl+alt+del emulation
		if ((key == kKeyPause || key == kKeyBreak) &&
			(mask & (KeyModifierControl | KeyModifierAlt)) ==
					(KeyModifierControl | KeyModifierAlt)) {
			// pretend it's ctrl+alt+del and ignore autorepeat
			LOG((CLOG_DEBUG "emulate ctrl+alt+del"));
			key      = kKeyDelete;
			isRepeat = false;
		}

		KeyButton keycode = static_cast<KeyButton>(xkey.keycode);
		if (!isRepeat) {
			// no press event follows so it's a plain release
			LOG((CLOG_DEBUG1 "event: KeyRelease code=%d, state=0x%04x", keycode, xkey.state));
			m_keyState->sendKeyEvent(getEventTarget(),
							false, false, key, mask, 1, keycode);
		}
		else {
			// found a press event following so it's a repeat.
			// we could attempt to count the already queued
			// repeats but we'll just send a repeat of 1.
			// note that we discard the press event.
			LOG((CLOG_DEBUG1 "event: repeat code=%d, state=0x%04x", keycode, xkey.state));
			m_keyState->sendKeyEvent(getEventTarget(),
							false, true, key, mask, 1, keycode);
		}
	}
}

bool
XWindowsScreen::onHotKey(XKeyEvent& xkey, bool isRepeat)
{
	// find the hot key id
	HotKeyToIDMap::const_iterator i =
		m_hotKeyToIDMap.find(HotKeyItem(xkey.keycode, xkey.state));
	if (i == m_hotKeyToIDMap.end()) {
		return false;
	}

	// find what kind of event
	Event::Type type;
	if (xkey.type == KeyPress) {
		type = m_events->forIPrimaryScreen().hotKeyDown();
	}
	else if (xkey.type == KeyRelease) {
		type = m_events->forIPrimaryScreen().hotKeyUp();
	}
	else {
		return false;
	}

	// generate event (ignore key repeats)
	if (!isRepeat) {
		m_events->addEvent(Event(type, getEventTarget(),
								HotKeyInfo::alloc(i->second)));
	}
	return true;
}

void
XWindowsScreen::onMousePress(const XButtonEvent& xbutton)
{
	LOG((CLOG_DEBUG1 "event: ButtonPress button=%d", xbutton.button));
	ButtonID button      = mapButtonFromX(&xbutton);
	KeyModifierMask mask = m_keyState->mapModifiersFromX(xbutton.state);
	if (button != kButtonNone) {
		sendEvent(m_events->forIPrimaryScreen().buttonDown(), ButtonInfo::alloc(button, mask));
	}
}

void
XWindowsScreen::onMouseRelease(const XButtonEvent& xbutton)
{
	LOG((CLOG_DEBUG1 "event: ButtonRelease button=%d", xbutton.button));
	ButtonID button      = mapButtonFromX(&xbutton);
	KeyModifierMask mask = m_keyState->mapModifiersFromX(xbutton.state);
	if (button != kButtonNone) {
		sendEvent(m_events->forIPrimaryScreen().buttonUp(), ButtonInfo::alloc(button, mask));
	}
	else if (xbutton.button == 4) {
		// wheel forward (away from user)
		sendEvent(m_events->forIPrimaryScreen().wheel(), WheelInfo::alloc(0, 120));
	}
	else if (xbutton.button == 5) {
		// wheel backward (toward user)
		sendEvent(m_events->forIPrimaryScreen().wheel(), WheelInfo::alloc(0, -120));
	}
	else if (xbutton.button == 6) {
		// wheel left
		sendEvent(m_events->forIPrimaryScreen().wheel(), WheelInfo::alloc(-120, 0));
	}
	else if (xbutton.button == 7) {
		// wheel right
		sendEvent(m_events->forIPrimaryScreen().wheel(), WheelInfo::alloc(120, 0));
	}
}

void
XWindowsScreen::onMouseMove(const XMotionEvent& xmotion)
{
	LOG((CLOG_DEBUG2 "event: MotionNotify %d,%d", xmotion.x_root, xmotion.y_root));
	if (!xmotion.same_screen) {
		LOG((CLOG_WARN "ignoring motion event outside the current X11 screen"));
		return;
	}

	// compute motion delta (relative to the last known
	// mouse position)
	SInt32 x = xmotion.x_root - m_xCursor;
	SInt32 y = xmotion.y_root - m_yCursor;

	// save position to compute delta of next motion
	m_xCursor = xmotion.x_root;
	m_yCursor = xmotion.y_root;

	if (xmotion.send_event) {
		// we warped the mouse.  discard events until we
		// find the matching sent event.  see
		// warpCursorNoFlush() for where the events are
		// sent.  we discard the matching sent event and
		// can be sure we've skipped the warp event.
		XEvent xevent;
		char cntr = 0;
		do {
            m_impl->XMaskEvent(m_display, PointerMotionMask, &xevent);
			if (cntr++ > 10) {
				LOG((CLOG_WARN "too many discarded events! %d", cntr));
				break;
			}
		} while (!xevent.xany.send_event);
		cntr = 0;
	}
	else if (m_isOnScreen) {
		// motion on primary screen
		sendEvent(m_events->forIPrimaryScreen().motionOnPrimary(),
							MotionInfo::alloc(m_xCursor, m_yCursor));
	}
	else {
		// motion on secondary screen.  warp mouse back to
		// center.
		//
		// my lombard (powerbook g3) running linux and
		// using the adbmouse driver has two problems:
		// first, the driver only sends motions of +/-2
		// pixels and, second, it seems to discard some
		// physical input after a warp.  the former isn't a
		// big deal (we're just limited to every other
		// pixel) but the latter is a PITA.  to work around
		// it we only warp when the mouse has moved more
		// than s_size pixels from the center.
		static const SInt32 s_size = 32;
		if (xmotion.x_root - m_xCenter < -s_size ||
			xmotion.x_root - m_xCenter >  s_size ||
			xmotion.y_root - m_yCenter < -s_size ||
			xmotion.y_root - m_yCenter >  s_size) {
			warpCursorNoFlush(m_xCenter, m_yCenter);
		}

		// send event if mouse moved.  do this after warping
		// back to center in case the motion takes us onto
		// the primary screen.  if we sent the event first
		// in that case then the warp would happen after
		// warping to the primary screen's enter position,
		// effectively overriding it.
		if (x != 0 || y != 0) {
			sendEvent(m_events->forIPrimaryScreen().motionOnSecondary(), MotionInfo::alloc(x, y));
		}
	}
}

int
XWindowsScreen::x_accumulateMouseScroll(SInt32 xDelta) const
{
    m_x_accumulatedScroll += xDelta;
    int numEvents = m_x_accumulatedScroll / m_mouseScrollDelta;
    m_x_accumulatedScroll -= numEvents * m_mouseScrollDelta;
    return numEvents;
}

int
XWindowsScreen::y_accumulateMouseScroll(SInt32 yDelta) const
{
    m_y_accumulatedScroll += yDelta;
    int numEvents = m_y_accumulatedScroll / m_mouseScrollDelta;
    m_y_accumulatedScroll -= numEvents * m_mouseScrollDelta;
    return numEvents;
}

Cursor
XWindowsScreen::createBlankCursor() const
{
	// this seems just a bit more complicated than really necessary

	// get the closet cursor size to 1x1
	unsigned int w = 0, h = 0;
    m_impl->XQueryBestCursor(m_display, m_root, 1, 1, &w, &h);
	w = std::max(1u, w);
	h = std::max(1u, h);

	// make bitmap data for cursor of closet size.  since the cursor
	// is blank we can use the same bitmap for shape and mask:  all
	// zeros.
	const int size = ((w + 7) >> 3) * h;
	char* data = new char[size];
	memset(data, 0, size);

	// make bitmap
    Pixmap bitmap = m_impl->XCreateBitmapFromData(m_display, m_root, data, w, h);

	// need an arbitrary color for the cursor
	XColor color;
	color.pixel = 0;
	color.red   = color.green = color.blue = 0;
	color.flags = DoRed | DoGreen | DoBlue;

	// make cursor from bitmap
    Cursor cursor = m_impl->XCreatePixmapCursor(m_display, bitmap, bitmap,
								&color, &color, 0, 0);

	// don't need bitmap or the data anymore
	delete[] data;
    m_impl->XFreePixmap(m_display, bitmap);

	return cursor;
}

ClipboardID
XWindowsScreen::getClipboardID(Atom selection) const
{
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		if (m_clipboard[id] != NULL &&
			m_clipboard[id]->getSelection() == selection) {
			return id;
		}
	}
	return kClipboardEnd;
}

void
XWindowsScreen::processClipboardRequest(Window requestor,
				Time time, Atom property)
{
	// check every clipboard until one returns success
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		if (m_clipboard[id] != NULL &&
			m_clipboard[id]->processRequest(requestor, time, property)) {
			break;
		}
	}
}

void
XWindowsScreen::destroyClipboardRequest(Window requestor)
{
	// check every clipboard until one returns success
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		if (m_clipboard[id] != NULL &&
			m_clipboard[id]->destroyRequest(requestor)) {
			break;
		}
	}
}

void
XWindowsScreen::onError()
{
	// prevent further access to the X display
	m_events->adoptBuffer(NULL);
	m_screensaver->destroy();
	m_screensaver = NULL;
	m_display     = NULL;

	// notify of failure
	sendEvent(m_events->forIScreen().error(), NULL);

	// FIXME -- should ensure that we ignore operations that involve
	// m_display from now on.  however, Xlib will simply exit the
	// application in response to the X I/O error so there's no
	// point in trying to really handle the error.  if we did want
	// to handle the error, it'd probably be easiest to delegate to
	// one of two objects.  one object would take the implementation
	// from this class.  the other object would be stub methods that
	// don't use X11.  on error, we'd switch to the latter.
}

int
XWindowsScreen::ioErrorHandler(Display*)
{
	// the display has disconnected, probably because X is shutting
	// down.  X forces us to exit at this point which is annoying.
	// we'll pretend as if we won't exit so we try to make sure we
	// don't access the display anymore.
	LOG((CLOG_CRIT "X display has unexpectedly disconnected"));
	s_screen->onError();
	return 0;
}

void
XWindowsScreen::selectEvents(Window w) const
{
	// ignore errors while we adjust event masks.  windows could be
	// destroyed at any time after the XQueryTree() in doSelectEvents()
	// so we must ignore BadWindow errors.
	XWindowsUtil::ErrorLock lock(m_display);

	// adjust event masks
	doSelectEvents(w);
}

void
XWindowsScreen::doSelectEvents(Window w) const
{
	// we want to track the mouse everywhere on the display.  to achieve
	// that we select PointerMotionMask on every window.  we also select
	// SubstructureNotifyMask in order to get CreateNotify events so we
	// select events on new windows too.

	// we don't want to adjust our grab window
	if (w == m_window) {
		return;
	}

       // X11 has a design flaw. If *no* client selected PointerMotionMask for
       // a window, motion events will be delivered to that window's parent.
       // If *any* client, not necessarily the owner, selects PointerMotionMask
       // on such a window, X will stop propagating motion events to its
       // parent. This breaks applications that rely on event propagation
       // behavior.
       //
       // Avoid selecting PointerMotionMask unless some other client selected
       // it already.
       long mask = SubstructureNotifyMask;
       XWindowAttributes attr;
       m_impl->XGetWindowAttributes(m_display, w, &attr);
       if ((attr.all_event_masks & PointerMotionMask) == PointerMotionMask) {
               mask |= PointerMotionMask;
       }

	// select events of interest.  do this before querying the tree so
	// we'll get notifications of children created after the XQueryTree()
	// so we won't miss them.
       m_impl->XSelectInput(m_display, w, mask);

	// recurse on child windows
	Window rw, pw, *cw;
	unsigned int nc;
    if (m_impl->XQueryTree(m_display, w, &rw, &pw, &cw, &nc)) {
		for (unsigned int i = 0; i < nc; ++i) {
			doSelectEvents(cw[i]);
		}
		XFree(cw);
	}
}

KeyID
XWindowsScreen::mapKeyFromX(XKeyEvent* event) const
{
	// convert to a keysym
	KeySym keysym;
	if (event->type == KeyPress && m_ic != NULL) {
		// do multibyte lookup.  can only call XmbLookupString with a
		// key press event and a valid XIC so we checked those above.
		char scratch[32];
		int n        = sizeof(scratch) / sizeof(scratch[0]);
		char* buffer = scratch;
		int status;
        n = m_impl->XmbLookupString(m_ic, event, buffer, n, &keysym, &status);
		if (status == XBufferOverflow) {
			// not enough space.  grow buffer and try again.
			buffer = new char[n];
            n = m_impl->XmbLookupString(m_ic, event, buffer, n, &keysym, &status);
			delete[] buffer;
		}

		// see what we got.  since we don't care about the string
		// we'll just look for a keysym.
		switch (status) {
		default:
		case XLookupNone:
		case XLookupChars:
			keysym = 0;
			break;

		case XLookupKeySym:
		case XLookupBoth:
			break;
		}
	}
	else {
		// plain old lookup
		char dummy[1];
        m_impl->XLookupString(event, dummy, 0, &keysym, NULL);
	}

	LOG((CLOG_DEBUG2 "mapped code=%d to keysym=0x%04x", event->keycode, keysym));

	// convert key
	KeyID result = XWindowsUtil::mapKeySymToKeyID(keysym);
	LOG((CLOG_DEBUG2 "mapped keysym=0x%04x to keyID=%d", keysym, result));
	return result;
}

ButtonID
XWindowsScreen::mapButtonFromX(const XButtonEvent* event) const
{
	unsigned int button = event->button;

	// http://xahlee.info/linux/linux_x11_mouse_button_number.html
	// and the program `xev`
	switch (button)
	{
	case 1: case 2: case 3: // kButtonLeft, Middle, Right
		return static_cast<ButtonID>(button);
	case 4: case 5: case 6: case 7: // scroll up, down, left, right -- ignored here
		return kButtonNone;
	case 8: // mouse button 4
		return kButtonExtra0;
	case 9: // mouse button 5
		return kButtonExtra1;
	default: // unknown button
		return kButtonNone;
	}
}

unsigned int
XWindowsScreen::mapButtonToX(ButtonID id) const
{
	switch (id)
	{
	case kButtonLeft: case kButtonMiddle: case kButtonRight:
		return id;
	case kButtonExtra0:
		return 8;
	case kButtonExtra1:
		return 9;
	default:
		return 0;
	}
}

void
XWindowsScreen::warpCursorNoFlush(SInt32 x, SInt32 y)
{
	assert(m_window != None);

	// send an event that we can recognize before the mouse warp
	XEvent eventBefore;
	eventBefore.type                = MotionNotify;
	eventBefore.xmotion.display     = m_display;
	eventBefore.xmotion.window      = m_window;
	eventBefore.xmotion.root        = m_root;
	eventBefore.xmotion.subwindow   = m_window;
	eventBefore.xmotion.time        = CurrentTime;
	eventBefore.xmotion.x           = x;
	eventBefore.xmotion.y           = y;
	eventBefore.xmotion.x_root      = x;
	eventBefore.xmotion.y_root      = y;
	eventBefore.xmotion.state       = 0;
	eventBefore.xmotion.is_hint     = NotifyNormal;
	eventBefore.xmotion.same_screen = True;
	XEvent eventAfter               = eventBefore;
    m_impl->XSendEvent(m_display, m_window, False, 0, &eventBefore);

	// warp mouse
    m_impl->XWarpPointer(m_display, None, m_root, 0, 0, 0, 0, x, y);

	// send an event that we can recognize after the mouse warp
    m_impl->XSendEvent(m_display, m_window, False, 0, &eventAfter);
    m_impl->XSync(m_display, False);

	LOG((CLOG_DEBUG2 "warped to %d,%d", x, y));
}

void
XWindowsScreen::updateButtons()
{
	// query the button mapping
    UInt32 numButtons = m_impl->XGetPointerMapping(m_display, NULL, 0);
	unsigned char* tmpButtons = new unsigned char[numButtons];
    m_impl->XGetPointerMapping(m_display, tmpButtons, numButtons);

	// find the largest logical button id
	unsigned char maxButton = 0;
	for (UInt32 i = 0; i < numButtons; ++i) {
		if (tmpButtons[i] > maxButton) {
			maxButton = tmpButtons[i];
		}
	}

	// allocate button array. add 1 because button IDs are 1-based
	// (index 0 is unused, indices 1..maxButton store button mapping).
	m_buttons.resize(maxButton + 1);

	// fill in button array values: m_buttons[physical-1] = logical button ID
	for (UInt32 i = 0; i < numButtons; ++i) {
		m_buttons[i] = 0;
	}
	for (UInt32 i = 0; i < numButtons; ++i) {
		m_buttons[tmpButtons[i] - 1] = i + 1;
	}

	// clean up
	delete[] tmpButtons;
}

bool
XWindowsScreen::grabMouseAndKeyboard()
{
	unsigned int event_mask = ButtonPressMask | ButtonReleaseMask | EnterWindowMask | LeaveWindowMask | PointerMotionMask;

	// grab the mouse and keyboard.  keep trying until we get them.
	// if we can't grab one after grabbing the other then ungrab
	// and wait before retrying.  Give up after the grab retry timeout.
	// Display wake-up, when needed, is a separate step before this budget.
	const double timeout = primaryLeaveGrabTimeoutForTest(m_lowLatencyMode);
	const double sleepTime = primaryLeaveGrabRetrySleepForTest(m_lowLatencyMode);

	int result;
	Stopwatch timer;
	do {
		// keyboard first
		do {
            result = m_impl->XGrabKeyboard(m_display, m_window, True,
								GrabModeAsync, GrabModeAsync, CurrentTime);
			assert(result != GrabNotViewable);
			if (result != GrabSuccess) {
				LOG((CLOG_DEBUG2 "waiting to grab keyboard"));
				ARCH->sleep(sleepTime);
				if (timer.getTime() >= timeout) {
					LOG((CLOG_DEBUG2 "grab keyboard timed out"));
					return false;
				}
			}
		} while (result != GrabSuccess);
		LOG((CLOG_DEBUG2 "grabbed keyboard"));

		// now the mouse --- use event_mask to get EnterNotify, LeaveNotify events
        result = m_impl->XGrabPointer(m_display, m_window, False, event_mask,
								GrabModeAsync, GrabModeAsync,
								m_window, None, CurrentTime);
		assert(result != GrabNotViewable);
		if (result != GrabSuccess) {
			// back off to avoid grab deadlock
            m_impl->XUngrabKeyboard(m_display, CurrentTime);
			LOG((CLOG_DEBUG2 "ungrabbed keyboard, waiting to grab pointer"));
			ARCH->sleep(sleepTime);
			if (timer.getTime() >= timeout) {
				LOG((CLOG_DEBUG2 "grab pointer timed out"));
				return false;
			}
		}
	} while (result != GrabSuccess);

	LOG((CLOG_DEBUG1 "grabbed pointer and keyboard"));
	return true;
}

void
XWindowsScreen::refreshKeyboard(XEvent* event)
{
    if (m_impl->XPending(m_display) > 0) {
		XEvent tmpEvent;
        m_impl->XPeekEvent(m_display, &tmpEvent);
		if (tmpEvent.type == MappingNotify) {
			// discard this event since another follows.
			// we tend to get a bunch of these in a row.
			return;
		}
	}

	// keyboard mapping changed
#if HAVE_XKB_EXTENSION
	if (m_xkb && event->type == m_xkbEventBase) {
        m_impl->XkbRefreshKeyboardMapping((XkbMapNotifyEvent*)event);
	}
	else
#else
	{
        m_impl->XRefreshKeyboardMapping(&event->xmapping);
	}
#endif
	m_keyState->updateKeyMap();
	m_keyState->updateKeyState();
}


//
// XWindowsScreen::HotKeyItem
//

XWindowsScreen::HotKeyItem::HotKeyItem(int keycode, unsigned int mask) :
	m_keycode(keycode),
	m_mask(mask)
{
	// do nothing
}

bool
XWindowsScreen::HotKeyItem::operator<(const HotKeyItem& x) const
{
	return (m_keycode < x.m_keycode ||
			(m_keycode == x.m_keycode && m_mask < x.m_mask));
}

bool
XWindowsScreen::detectXI2()
{
	int event, error;
    return m_impl->XQueryExtension(m_display,
			"XInputExtension", &xi_opcode, &event, &error);
}

bool
XWindowsScreen::detectXFixesSelectionNotifications()
{
#ifdef HAVE_XFIXES
	int errorBase = 0;
	if (!m_impl->XFixesQueryExtension(
		m_display, &m_xfixesEventBase, &errorBase)) {
		LOG((CLOG_DEBUG "XFixes selection notifications are unavailable"));
		return false;
	}

	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		m_impl->XFixesSelectSelectionInput(
			m_display, m_window, m_clipboard[id]->getSelection(),
			XFixesSetSelectionOwnerNotifyMask);
	}
	LOG((CLOG_DEBUG "using XFixes selection owner notifications"));
	return true;
#else
	return false;
#endif
}

#ifdef HAVE_XI2
void
XWindowsScreen::selectXIRawMotion()
{
	// Allocate mask large enough for all XI event types (up to XI_GenericEvent = 35).
	// XIMaskLen(XI_GenericEvent + 1) ensures coverage of all raw button and motion events.
	XIEventMask mask;
	mask.deviceid = XIAllMasterDevices;
	mask.mask_len = XIMaskLen(XI_GenericEvent + 1);
	mask.mask = (unsigned char*)calloc(mask.mask_len, sizeof(char));
	if (mask.mask == NULL) {
		LOG((CLOG_WARN "failed to allocate XI event mask"));
		return;
	}

	XISetMask(mask.mask, XI_RawKeyRelease);
	XISetMask(mask.mask, XI_RawMotion);
	XISetMask(mask.mask, XI_RawButtonPress);
	XISetMask(mask.mask, XI_RawButtonRelease);
	m_impl->XISelectEvents(m_display, DefaultRootWindow(m_display), &mask, 1);
	free(mask.mask);
}

void
XWindowsScreen::handleXIRawButtonEvent(const XIRawButtonEvent* const event)
{
	// Convert XI_RawButton event to XButtonEvent-like structure for reuse.
	// Use XQueryPointer for current modifier state, matching the established
	// pattern used in the XI_RawMotion handler in this file.
	XButtonEvent xbutton;
	memset(&xbutton, 0, sizeof(xbutton));
	xbutton.type       = (event->evtype == XI_RawButtonPress) ? ButtonPress : ButtonRelease;
	xbutton.send_event  = False;
	xbutton.display     = m_display;
	xbutton.window      = m_window;
	xbutton.root        = DefaultRootWindow(m_display);
	xbutton.time        = event->time;
	xbutton.button      = event->detail;

	Window root, child;
	unsigned int msk;
	Status st = m_impl->XQueryPointer(
		m_display, m_root,
		&xbutton.root, &child,
		&xbutton.x_root, &xbutton.y_root,
		&xbutton.x, &xbutton.y,
		&msk);
	if (st) {
		xbutton.state = msk;
	}

	if (event->evtype == XI_RawButtonPress) {
		onMousePress(xbutton);
	}
	else {
		onMouseRelease(xbutton);
	}
}
#endif
