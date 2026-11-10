/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2004 Chris Schoeneman
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

#include "platform/OSXScreen.h"
#include "barrier/XScreen.h"

#include "base/EventQueue.h"
#include "client/Client.h"
#include "platform/OSXClipboard.h"
#include "platform/OSXEventQueueBuffer.h"
#include "platform/OSXKeyState.h"
#include "platform/OSXScreenSaver.h"
#include "platform/OSXDragSimulator.h"
#include "platform/OSXMediaKeySupport.h"
#include "platform/OSXPasteboardPeeker.h"
#include "barrier/Clipboard.h"
#include "barrier/KeyMap.h"
#include "barrier/ClientApp.h"
#include "mt/CondVar.h"
#include "mt/Lock.h"
#include "mt/Mutex.h"
#include "mt/Thread.h"
#include "mt/ThreadShutdown.h"
#include "arch/XArch.h"
#include "base/Log.h"
#include "base/IEventQueue.h"
#include "base/Stopwatch.h"
#include "base/TMethodEventJob.h"
#include "io/filesystem.h"

#include <math.h>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <mach/mach_time.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <AvailabilityMacros.h>
#include <IOKit/hidsystem/event_status_driver.h>
#include <AppKit/NSEvent.h>

// This isn't in any Apple SDK that I know of as of yet.
enum {
	kBarrierEventMouseScroll = 11,
	kBarrierMouseScrollAxisX = 'saxx',
	kBarrierMouseScrollAxisY = 'saxy'
};

enum {
	kCarbonLoopWaitTimeout = 10
};

// Distinguish remote input posted by Weave from a local trackpad/mouse event,
// including when a downstream event source does not preserve the sender PID.
static constexpr int64_t kWeavePointerEventMarker = 0x5745415645505452;

static CGEventTimestamp nativeInputTimestampTicks()
{
	// Physical events at the primary HID tap use mach ticks on Apple silicon.
	// Converting only this boundary to nanoseconds makes every later physical
	// event look stale when the timebase is not 1:1. Explicitly timestamped
	// synthetic events do not establish the hardware stream's clock domain.
	return mach_absolute_time();
}

static CGEventTimestamp pointerEventTimestampTicks(CGEventRef event)
{
	// On the same Apple-silicon machine, physical HID timestamps are mach
	// ticks whereas IOHIDPostEvent timestamps are uptime nanoseconds. Both
	// sources have HIDSystemState provenance. Select the uptime representation
	// nearest callback arrival, then compare only ticks. This does not discard
	// events or use their PID, direction, or motion magnitude as a filter.
	const uint64_t timestamp = CGEventGetTimestamp(event);
	const uint64_t now = mach_absolute_time();
	static const mach_timebase_info_data_t base = [] {
		mach_timebase_info_data_t value;
		mach_timebase_info(&value);
		return value;
	}();
	const uint64_t nowNS = static_cast<uint64_t>((static_cast<__uint128_t>(now) * base.numer) / base.denom);
	const auto distance = [](uint64_t a, uint64_t b) { return a > b ? a - b : b - a; };
	const __uint128_t ticks = distance(timestamp, now) <= distance(timestamp, nowNS) ? timestamp :
		(static_cast<__uint128_t>(timestamp) * base.denom) / base.numer;
	// An absent/future/unrepresentable timestamp cannot order a native event
	// against a warp. Let the owner handle this explicitly, never wrap it.
	return ticks == 0 || ticks > now ? 0 : static_cast<uint64_t>(ticks);
}

// TODO: upgrade deprecated function usage in these functions.
void setZeroSuppressionInterval();
void avoidSupression();
void logCursorVisibility();
void avoidHesitatingCursor();

//
// OSXScreen
//

bool					OSXScreen::s_testedForGHOM = false;
bool					OSXScreen::s_hasGHOM	    = false;

OSXScreen::OSXScreen(IEventQueue* events, bool isPrimary, bool autoShowHideCursor,
                     CFStringRef pasteboardName) :
	PlatformScreen(events),
	m_isPrimary(isPrimary),
	m_isOnScreen(m_isPrimary),
	m_cursorPosValid(false),
	MouseButtonEventMap(NumButtonIDs),
	m_cursorHidden(false),
	m_dragNumButtonsDown(0),
	m_dragTimer(NULL),
	m_keyState(NULL),
	m_clipboardWorker(new OSXClipboardWorker(pasteboardName)),
	m_sequenceNumber(0),
	m_screensaver(NULL),
	m_screensaverNotify(false),
	m_ownClipboard(false),
	m_clipboardTimer(NULL),
	m_hiddenWindow(NULL),
	m_userInputWindow(NULL),
	m_switchEventHandlerRef(0),
	m_pmMutex(new Mutex),
	m_pmWatchThread(NULL),
	m_pmThreadReady(new CondVar<bool>(m_pmMutex, false)),
	m_pmRunloop(NULL),
	m_pmRootPort(0),
	m_activeModifierHotKey(0),
	m_activeModifierHotKeyMask(0),
	m_eventTapPort(nullptr),
	m_eventTapRLSR(nullptr),
	m_lastClickTime(0),
	m_clickState(1),
	m_lastSingleClickXCursor(0),
	m_lastSingleClickYCursor(0),
	m_autoShowHideCursor(autoShowHideCursor),
	m_events(events),
	m_getDropTargetThread(NULL),
	m_impl(NULL)
{
	try {
		m_displayID   = CGMainDisplayID();
		updateScreenShape();
		m_screensaver = new OSXScreenSaver(m_events, getEventTarget());
		m_keyState	  = new OSXKeyState(m_events);

		// only needed when running as a server.
		if (m_isPrimary) {

#if defined(MAC_OS_X_VERSION_10_9)
			// we can't pass options to show the dialog, this must be done by the gui.
			if (!AXIsProcessTrusted()) {
				throw XInputUnavailable("assistive devices does not trust this process, allow it in system settings.");
			}
#else
			// now deprecated in mavericks.
			if (!AXAPIEnabled()) {
				throw XInputUnavailable("assistive devices is not enabled, enable it in system settings.");
			}
#endif
		}

		// install display manager notification handler
		m_events->registerTypeOnce(m_displayChangeEvent, "OSXScreen::displayChange");
		m_events->adoptHandler(m_displayChangeEvent, this,
			new TMethodEventJob<OSXScreen>(this, &OSXScreen::handleDisplayChange));
		CGDisplayRegisterReconfigurationCallback(displayReconfigurationCallback, this);

		// install fast user switching event handler
		EventTypeSpec switchEventTypes[2];
		switchEventTypes[0].eventClass = kEventClassSystem;
		switchEventTypes[0].eventKind  = kEventSystemUserSessionDeactivated;
		switchEventTypes[1].eventClass = kEventClassSystem;
		switchEventTypes[1].eventKind  = kEventSystemUserSessionActivated;
		EventHandlerUPP switchEventHandler =
			NewEventHandlerUPP(userSwitchCallback);
		InstallApplicationEventHandler(switchEventHandler, 2, switchEventTypes,
									   this, &m_switchEventHandlerRef);
		DisposeEventHandlerUPP(switchEventHandler);

		constructMouseButtonEventMap();

		// watch for requests to sleep
		m_events->adoptHandler(m_events->forOSXScreen().confirmSleep(),
								getEventTarget(),
								new TMethodEventJob<OSXScreen>(this,
									&OSXScreen::handleConfirmSleep));

		// create thread for monitoring system power state.
		*m_pmThreadReady = false;
#if defined(MAC_OS_X_VERSION_10_7)
		m_carbonLoopMutex = new Mutex();
		m_carbonLoopReady = new CondVar<bool>(m_carbonLoopMutex, false);
#endif
		LOG((CLOG_DEBUG "starting watchSystemPowerThread"));
        m_pmWatchThread = new Thread([this](){ watchSystemPowerThread(); });
	}
	catch (...) {
		m_events->removeHandler(m_events->forOSXScreen().confirmSleep(),
								getEventTarget());
		if (m_switchEventHandlerRef != 0) {
			RemoveEventHandler(m_switchEventHandlerRef);
		}

		CGDisplayRemoveReconfigurationCallback(displayReconfigurationCallback, this);
		m_events->removeHandler(m_displayChangeEvent, this);

		delete m_keyState;
		delete m_screensaver;
		delete m_pmThreadReady;
		delete m_pmMutex;
		throw;
	}

	// install event handlers
	m_events->adoptHandler(Event::kSystem, m_events->getSystemTarget(),
							new TMethodEventJob<OSXScreen>(this,
								&OSXScreen::handleSystemEvent));

	// install the platform event queue
	m_events->adoptBuffer(new OSXEventQueueBuffer(m_events));
}

OSXScreen::~OSXScreen()
{
	disable();
	m_clipboardWorker.reset();
	m_events->adoptBuffer(NULL);
	m_events->removeHandler(Event::kSystem, m_events->getSystemTarget());

	if (m_pmWatchThread) {
		Stopwatch shutdownTimer;
		const double shutdownDeadline =
			barrier::kFinalThreadShutdownDeadlineSeconds;
		bool threadReady = false;
		CFRunLoopRef readyRunloop = NULL;

		{
			Lock lock(m_pmMutex);
			while (!(bool)*m_pmThreadReady) {
				if (!m_pmThreadReady->wait(shutdownTimer, shutdownDeadline)) {
					break;
				}
			}
			threadReady = (bool)*m_pmThreadReady;
			if (threadReady) {
				readyRunloop = m_pmRunloop;
			}
		}

		if (threadReady && readyRunloop != NULL) {
			CFRunLoopStop(readyRunloop);
		}

		if (!m_pmWatchThread->wait(0.0)) {
			m_pmWatchThread->cancel();
			m_pmWatchThread->unblockPollSocket();
			const double elapsed = shutdownTimer.getTime();
			const double remaining = elapsed < shutdownDeadline ?
				shutdownDeadline - elapsed : 0.0;
			barrier::waitForFinalThreadShutdown(
				"macOS power-monitor thread",
				remaining,
				[this](double timeout) {
					return m_pmWatchThread->wait(timeout);
				});
		}
		delete m_pmWatchThread;
		m_pmWatchThread = NULL;

		CFRunLoopRef runloop = NULL;
		{
			Lock lock(m_pmMutex);
			runloop = m_pmRunloop;
			m_pmRunloop = NULL;
		}
		if (runloop != NULL) {
			CFRelease(runloop);
		}
	}
	delete m_pmThreadReady;
	delete m_pmMutex;

	m_events->removeHandler(m_events->forOSXScreen().confirmSleep(),
								getEventTarget());

	RemoveEventHandler(m_switchEventHandlerRef);

	CGDisplayRemoveReconfigurationCallback(displayReconfigurationCallback, this);
	m_events->removeHandler(m_displayChangeEvent, this);

	delete m_keyState;
	delete m_screensaver;

#if defined(MAC_OS_X_VERSION_10_7)
	delete m_carbonLoopMutex;
	delete m_carbonLoopReady;
#endif
}

void*
OSXScreen::getEventTarget() const
{
	return const_cast<OSXScreen*>(this);
}

bool
OSXScreen::getClipboard(ClipboardID id, IClipboard* dst) const
{
	std::shared_ptr<const String> snapshot;
	if (!getClipboardSnapshot(id, &snapshot, nullptr)) return false;
	IClipboard::unmarshall(dst, *snapshot, 0);
	return true;
}

bool
OSXScreen::getClipboardSnapshot(ClipboardID id, std::shared_ptr<const String>* data, UInt32* sequence) const
{
	return id < kClipboardEnd && m_clipboardWorker && m_clipboardWorker->snapshot(data, sequence);
}

void
OSXScreen::getShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h) const
{
	std::lock_guard<std::mutex> lock(m_shapeMutex);
	if (m_displayConfiguring) { x = y = w = h = 0; return; }
	x = m_x;
	y = m_y;
	w = m_w;
	h = m_h;
}

void
OSXScreen::getCursorPos(SInt32& x, SInt32& y) const
{
	CGEventRef event = CGEventCreate(NULL);
	CGPoint mouse = CGEventGetLocation(event);
	x                = mouse.x;
	y                = mouse.y;
	m_cursorPosValid = true;
	m_xCursor        = x;
	m_yCursor        = y;
	CFRelease(event);
}

void
OSXScreen::reconfigure(UInt32)
{
	// do nothing
}

void
OSXScreen::warpCursor(SInt32 x, SInt32 y)
{
	if (m_inputLoop && !m_inputLoop->isOwner()) {
		if (!m_inputLoop->invoke([this, x, y] { warpCursor(x, y); })) {
			sendEvent(m_events->forIScreen().error());
		}
		return;
	}
	// move cursor without generating events
	CGPoint pos;
	pos.x = x;
	pos.y = y;
	if (warpPointer(pos) != kCGErrorSuccess) {
		sendEvent(m_events->forIScreen().error());
		return;
	}

	// save new cursor position
	m_xCursor        = x;
	m_yCursor        = y;
	m_cursorPosValid = true;
}

bool
OSXScreen::recordPointerPosition(CGPoint position, CGEventTimestamp ticks, bool observedEvent)
{
	if (!std::isfinite(position.x) || !std::isfinite(position.y)) return false;
	// The single tap delivers native and direct-CG events in order. Once a CG
	// event is observed, older changes can be replaced by its final position;
	// later owner warps must remain for already-captured native callbacks.
	if (observedEvent) {
		while (!m_pointerPositionChanges.empty() && m_pointerPositionChanges.front().ticks <= ticks)
			m_pointerPositionChanges.pop_front();
	}
	if (m_pointerPositionChanges.size() >= 256) return false;
	auto at = std::upper_bound(m_pointerPositionChanges.begin(), m_pointerPositionChanges.end(), ticks,
		[](CGEventTimestamp t, const PointerPositionChange& change) { return t < change.ticks; });
	m_pointerPositionChanges.insert(at, {ticks, position, observedEvent});
	return true;
}

void
OSXScreen::initializePointerMotion()
{
	m_pointerPositionChanges.clear();
	CGEventRef snapshot = CGEventCreate(nullptr);
	m_lastNativePointerValid = false;
	if (snapshot) {
		m_lastNativePointer = CGEventGetLocation(snapshot);
		m_lastNativePointerValid = std::isfinite(m_lastNativePointer.x) && std::isfinite(m_lastNativePointer.y);
		CFRelease(snapshot);
	}
}

CGError
OSXScreen::warpPointer(CGPoint position, bool skipIfAlreadyParked)
{
	if (!m_isPrimary) return CGWarpMouseCursorPosition(position);
	CGEventRef snapshot = CGEventCreate(nullptr);
	if (!snapshot) return kCGErrorFailure;
	const CGPoint before = CGEventGetLocation(snapshot);
	CFRelease(snapshot);
	// A queued event can still describe the old position. Avoid another
	// synchronous WindowServer warp if the live pointer is already parked.
	if (skipIfAlreadyParked && CGPointEqualToPoint(before, position)) return kCGErrorSuccess;
	if (!std::isfinite(before.x) || !std::isfinite(before.y) ||
		!std::isfinite(position.x) || !std::isfinite(position.y) ||
		m_pointerPositionChanges.size() >= 256) return kCGErrorFailure;
	// Snapshot before the native call: old queued callbacks must not consume
	// this position. Failed/no-op warps add no displacement obligation.
	const auto ticks = mach_absolute_time();
	const auto result = CGWarpMouseCursorPosition(position);
	if (result == kCGErrorSuccess && !CGPointEqualToPoint(before, position)) {
		if (!m_lastNativePointerValid) {
			m_lastNativePointer = before;
			m_lastNativePointerValid = true;
		}
		// Preserve unobserved/coalesced native movement between separate
		// warps. A final target minus the last event position would lose it.
		recordPointerPosition(CGPointMake(position.x - before.x, position.y - before.y), ticks, false);
	}
	return result;
}

void
OSXScreen::compensatePointerMotion(CGEventRef event, CGEventTimestamp ticks, double& dx, double& dy)
{
	if (CGEventGetIntegerValueField(event, kCGEventSourceStateID) != kCGEventSourceStateHIDSystemState)
		return;
	if (ticks == 0) {
		m_pointerPositionChanges.clear();
		m_lastNativePointer = CGEventGetLocation(event);
		m_lastNativePointerValid = std::isfinite(m_lastNativePointer.x) && std::isfinite(m_lastNativePointer.y);
		return;
	}
	bool changed = false;
	CGPoint imposed = CGPointZero;
	while (!m_pointerPositionChanges.empty() && m_pointerPositionChanges.front().ticks <= ticks) {
		const auto& change = m_pointerPositionChanges.front();
		if (change.absolute) {
			// Direct CG posting already applied its absolute position before
			// the tap sees it. Reconcile that anchor separately; do not invent
			// a pre-CG position from the current native cursor snapshot.
			imposed = CGPointMake(change.value.x - m_lastNativePointer.x,
				change.value.y - m_lastNativePointer.y);
		}
		else {
			imposed.x += change.value.x;
			imposed.y += change.value.y;
		}
		m_pointerPositionChanges.pop_front();
		changed = true;
	}
	if (changed && m_lastNativePointerValid) {
		// Position is fractional/clipped while these HID delta fields are
		// integer. Remove only the whole-pixel imposed displacement, once;
		// do not accumulate fractional quantization as future motion debt.
		dx -= std::trunc(imposed.x);
		dy -= std::trunc(imposed.y);
	}
	m_lastNativePointer = CGEventGetLocation(event);
	m_lastNativePointerValid = std::isfinite(m_lastNativePointer.x) && std::isfinite(m_lastNativePointer.y);
}

void
OSXScreen::fakeInputBegin()
{
	// FIXME -- not implemented
}

void
OSXScreen::fakeInputEnd()
{
	// FIXME -- not implemented
}

SInt32
OSXScreen::getJumpZoneSize() const
{
	return 1;
}

bool
OSXScreen::isAnyMouseButtonDown(UInt32& buttonID) const
{
	if (needsInputDispatch()) {
		// A failed query must not permit an unsafe screen switch mid-drag.
		bool down = true;
		invokeInputState([this, &buttonID, &down] { down = isAnyMouseButtonDown(buttonID); });
		return down;
	}
	if (m_buttonState.test(0)) {
		buttonID = kButtonLeft;
		return true;
	}

	return (GetCurrentButtonState() != 0);
}

void
OSXScreen::getCursorCenter(SInt32& x, SInt32& y) const
{
	std::lock_guard<std::mutex> lock(m_shapeMutex);
	if (m_displayConfiguring) { x = y = 0; return; }
	x = m_xCenter;
	y = m_yCenter;
}

UInt32
OSXScreen::registerHotKey(KeyID key, KeyModifierMask mask)
{
	if (needsInputDispatch()) {
		UInt32 id = 0;
		invokeInputState([this, key, mask, &id] { id = registerHotKey(key, mask); });
		return id;
	}
	// get mac virtual key and modifier mask matching barrier key and mask
	UInt32 macKey, macMask;
	if (!m_keyState->mapBarrierHotKeyToMac(key, mask, macKey, macMask)) {
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

	// if this hot key has modifiers only then we'll handle it specially
	EventHotKeyRef ref = NULL;
	bool okay;
	if (key == kKeyNone) {
		if (m_modifierHotKeys.count(mask) > 0) {
			// already registered
			okay = false;
		}
		else {
			m_modifierHotKeys[mask] = id;
			okay = true;
		}
	}
	else {
		EventHotKeyID hkid = { 'SNRG', (UInt32)id };
		OSStatus status = RegisterEventHotKey(macKey, macMask, hkid,
								GetApplicationEventTarget(), 0,
								&ref);
		okay = (status == noErr);
		m_hotKeyToIDMap[HotKeyItem(macKey, macMask)] = id;
	}

	if (!okay) {
		m_oldHotKeyIDs.push_back(id);
		m_hotKeyToIDMap.erase(HotKeyItem(macKey, macMask));
		LOG((CLOG_WARN "failed to register hotkey %s (id=%04x mask=%04x)", barrier::KeyMap::formatKey(key, mask).c_str(), key, mask));
		return 0;
	}

	m_hotKeys.insert(std::make_pair(id, HotKeyItem(ref, macKey, macMask)));

	LOG((CLOG_DEBUG "registered hotkey %s (id=%04x mask=%04x) as id=%d", barrier::KeyMap::formatKey(key, mask).c_str(), key, mask, id));
	return id;
}

void
OSXScreen::unregisterHotKey(UInt32 id)
{
	if (needsInputDispatch()) {
		invokeInputState([this, id] { unregisterHotKey(id); });
		return;
	}
	// look up hotkey
	HotKeyMap::iterator i = m_hotKeys.find(id);
	if (i == m_hotKeys.end()) {
		return;
	}

	// unregister with OS
	bool okay;
	if (i->second.getRef() != NULL) {
		okay = (UnregisterEventHotKey(i->second.getRef()) == noErr);
	}
	else {
		okay = false;
		// XXX -- this is inefficient
		for (ModifierHotKeyMap::iterator j = m_modifierHotKeys.begin();
								j != m_modifierHotKeys.end(); ++j) {
			if (j->second == id) {
				m_modifierHotKeys.erase(j);
				okay = true;
				break;
			}
		}
	}
	if (!okay) {
		LOG((CLOG_WARN "failed to unregister hotkey id=%d", id));
	}
	else {
		LOG((CLOG_DEBUG "unregistered hotkey id=%d", id));
	}

	// discard hot key from map and record old id for reuse
	m_hotKeyToIDMap.erase(i->second);
	m_hotKeys.erase(i);
	m_oldHotKeyIDs.push_back(id);
	if (m_activeModifierHotKey == id) {
		m_activeModifierHotKey     = 0;
		m_activeModifierHotKeyMask = 0;
	}
}

void
OSXScreen::constructMouseButtonEventMap()
{
	const CGEventType source[NumButtonIDs][3] = {
		{kCGEventLeftMouseUp, kCGEventLeftMouseDragged, kCGEventLeftMouseDown},
		{kCGEventRightMouseUp, kCGEventRightMouseDragged, kCGEventRightMouseDown},
		{kCGEventOtherMouseUp, kCGEventOtherMouseDragged, kCGEventOtherMouseDown},
		{kCGEventOtherMouseUp, kCGEventOtherMouseDragged, kCGEventOtherMouseDown},
		{kCGEventOtherMouseUp, kCGEventOtherMouseDragged, kCGEventOtherMouseDown},
		{kCGEventOtherMouseUp, kCGEventOtherMouseDragged, kCGEventOtherMouseDown}
	};

	for (UInt16 button = 0; button < NumButtonIDs; button++) {
		MouseButtonEventMapType new_map;
		for (UInt16 state = (UInt32) kMouseButtonUp; state < kMouseButtonStateMax; state++) {
			CGEventType curEvent = source[button][state];
			new_map[state] = curEvent;
		}
		MouseButtonEventMap[button] = new_map;
	}
}

void
OSXScreen::postMouseEvent(CGPoint& pos) const
{
	// check if cursor position is valid on the client display configuration
	// stkamp@users.sourceforge.net
	CGDisplayCount displayCount = 0;
	CGGetDisplaysWithPoint(pos, 0, NULL, &displayCount);
	if (displayCount == 0) {
		// cursor position invalid - clamp to bounds of last valid display.
		// find the last valid display using the last cursor position.
		displayCount = 0;
		CGDirectDisplayID displayID;
		CGGetDisplaysWithPoint(CGPointMake(m_xCursor, m_yCursor), 1,
								&displayID, &displayCount);
		if (displayCount != 0) {
			CGRect displayRect = CGDisplayBounds(displayID);
			if (pos.x < displayRect.origin.x) {
				pos.x = displayRect.origin.x;
			}
			else if (pos.x > displayRect.origin.x +
								displayRect.size.width - 1) {
				pos.x = displayRect.origin.x + displayRect.size.width - 1;
			}
			if (pos.y < displayRect.origin.y) {
				pos.y = displayRect.origin.y;
			}
			else if (pos.y > displayRect.origin.y +
								displayRect.size.height - 1) {
				pos.y = displayRect.origin.y + displayRect.size.height - 1;
			}
		}
	}

	CGEventType type = kCGEventMouseMoved;

	SInt8 button = m_buttonState.getFirstButtonDown();
	if (button != -1) {
		MouseButtonEventMapType thisButtonType = MouseButtonEventMap[button];
		type = thisButtonType[kMouseButtonDragged];
	}

	CGEventRef event = CGEventCreateMouseEvent(NULL, type, pos, static_cast<CGMouseButton>(button));

    // Dragging events also need the click state
    CGEventSetIntegerValueField(event, kCGMouseEventClickState, m_clickState);

    // Fix for sticky keys
    CGEventFlags modifiers = m_keyState->getModifierStateAsOSXFlags();
    CGEventSetFlags(event, modifiers);

    // Set movement deltas to fix issues with certain 3D programs
    SInt64 deltaX = pos.x;
    deltaX -= m_xCursor;

    SInt64 deltaY = pos.y;
    deltaY -= m_yCursor;

    CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, deltaX);
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, deltaY);

    double deltaFX = deltaX;
    double deltaFY = deltaY;

    CGEventSetDoubleValueField(event, kCGMouseEventDeltaX, deltaFX);
    CGEventSetDoubleValueField(event, kCGMouseEventDeltaY, deltaFY);

	CGEventSetIntegerValueField(event, kCGEventSourceUserData, kWeavePointerEventMarker);
	CGEventPost(kCGHIDEventTap, event);

	CFRelease(event);
}

void
OSXScreen::fakeMouseButton(ButtonID id, bool press)
{
	if (needsInputDispatch()) {
		if (press) invokeInputState([this, id] { fakeMouseButton(id, true); });
		else invokeInputRelease([this, id] { fakeMouseButton(id, false); });
		return;
	}
	// Buttons are indexed from one, but the button down array is indexed from zero
	UInt32 index = mapBarrierButtonToMac(id) - kButtonLeft;
	if (index >= NumButtonIDs) {
		return;
	}

	CGPoint pos;
	if (!m_cursorPosValid) {
		SInt32 x, y;
		getCursorPos(x, y);
	}
	pos.x = m_xCursor;
	pos.y = m_yCursor;

	// variable used to detect mouse coordinate differences between
	// old & new mouse clicks. Used in double click detection.
	SInt32 xDiff = m_xCursor - m_lastSingleClickXCursor;
	SInt32 yDiff = m_yCursor - m_lastSingleClickYCursor;
	double diff = sqrt(xDiff * xDiff + yDiff * yDiff);
	// max sqrt(x^2 + y^2) difference allowed to double click
	// since we don't have double click distance in NX APIs
	// we define our own defaults.
	const double maxDiff = sqrt(2) + 0.0001;

    double clickTime = [NSEvent doubleClickInterval];

    // As long as the click is within the time window and distance window
    // increase clickState (double click, triple click, etc)
    // This will allow for higher than triple click but the quartz documentation
    // does not specify that this should be limited to triple click
    if (press) {
        if ((ARCH->time() - m_lastClickTime) <= clickTime && diff <= maxDiff) {
            m_clickState++;
        }
        else {
            m_clickState = 1;
        }

        m_lastClickTime = ARCH->time();
    }

    if (m_clickState == 1) {
        m_lastSingleClickXCursor = m_xCursor;
        m_lastSingleClickYCursor = m_yCursor;
    }

    EMouseButtonState state = press ? kMouseButtonDown : kMouseButtonUp;

    LOG((CLOG_DEBUG1 "faking mouse button id: %d press: %s", index, press ? "pressed" : "released"));

    MouseButtonEventMapType thisButtonMap = MouseButtonEventMap[index];
    CGEventType type = thisButtonMap[state];

    CGEventRef event = CGEventCreateMouseEvent(NULL, type, pos, static_cast<CGMouseButton>(index));

    CGEventSetIntegerValueField(event, kCGMouseEventClickState, m_clickState);

    // Fix for sticky keys
    CGEventFlags modifiers = m_keyState->getModifierStateAsOSXFlags();
    CGEventSetFlags(event, modifiers);

    m_buttonState.set(index, state);
    if (!press) m_handoffButtonGenerations.erase(id);
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, kWeavePointerEventMarker);
    CGEventPost(kCGHIDEventTap, event);

    CFRelease(event);

	if (!press && (id == kButtonLeft)) {
		if (m_fakeDraggingStarted) {
            finishFakeDrag();
		}

		m_draggingStarted = false;
	}
}

const std::string& OSXScreen::getDropTarget() const
{
    // The legacy interface returns a reference. Keep a caller-thread snapshot
    // so Finder's background discovery cannot mutate it while it is copied.
    thread_local std::string target;
    std::lock_guard<std::mutex> lock(m_dropTargetMutex);
    target = m_configuredDropTarget.empty() ? m_dropTarget : m_configuredDropTarget;
    return target;
}

void OSXScreen::setDropTarget(const std::string& target)
{
    std::lock_guard<std::mutex> lock(m_dropTargetMutex);
    m_configuredDropTarget = target;
}

void OSXScreen::finishFakeDrag()
{
    {
        std::lock_guard<std::mutex> lock(m_dropTargetMutex);
        if (!m_configuredDropTarget.empty()) {
            // A configured inbox does not depend on Finder's drop destination.
            m_fakeDraggingStarted = false;
            return;
        }
    }
    m_getDropTargetThread = new Thread([this](){ get_drop_target_thread(); });
}

void OSXScreen::get_drop_target_thread()
{
#if defined(MAC_OS_X_VERSION_10_7)
	char* cstr = NULL;

	// wait for 5 secs for the drop destinaiton string to be filled.
	UInt32 timeout = ARCH->time() + 5;

	while (ARCH->time() < timeout) {
		CFStringRef cfstr = getCocoaDropTarget();
        if (cfstr != NULL) {
            cstr = CFStringRefToUTF8String(cfstr);
            CFRelease(cfstr);
        }

		if (cstr != NULL) {
			break;
		}
		ARCH->sleep(.1f);
	}

	if (cstr != NULL) {
		LOG((CLOG_DEBUG "drop target: %s", cstr));
	}
	else {
		LOG((CLOG_ERR "failed to get drop target"));
	}
    {
        std::lock_guard<std::mutex> lock(m_dropTargetMutex);
        // Discovery remains a fallback; it never writes the explicit inbox.
        m_dropTarget = cstr != NULL ? cstr : "";
    }
    free(cstr);
#else
	LOG((CLOG_WARN "drag drop not supported"));
#endif
	m_fakeDraggingStarted = false;
}

void
OSXScreen::fakeMouseMove(SInt32 x, SInt32 y)
{
	if (needsInputDispatch()) {
		invokeInputState([this, x, y] { fakeMouseMove(x, y); });
		return;
	}
	if (m_fakeDraggingStarted) {
		m_buttonState.set(0, kMouseButtonDown);
	}

	// index 0 means left mouse button
	if (m_buttonState.test(0)) {
		m_draggingStarted = true;
	}

	// synthesize event
	CGPoint pos;
	pos.x = x;
	pos.y = y;
	postMouseEvent(pos);

	// save new cursor position
	m_xCursor        = static_cast<SInt32>(pos.x);
	m_yCursor        = static_cast<SInt32>(pos.y);
	m_cursorPosValid = true;
}

void
OSXScreen::fakeMouseRelativeMove(SInt32 dx, SInt32 dy) const
{
	if (needsInputDispatch()) {
		invokeInputState([this, dx, dy] { fakeMouseRelativeMove(dx, dy); });
		return;
	}
	// OS X does not appear to have a fake relative mouse move function.
	// simulate it by getting the current mouse position and adding to
	// that.  this can yield the wrong answer but there's not much else
	// we can do.

	// get current position
	CGEventRef event = CGEventCreate(NULL);
	CGPoint oldPos = CGEventGetLocation(event);
	CFRelease(event);

	// synthesize event
	CGPoint pos;
	m_xCursor = static_cast<SInt32>(oldPos.x);
	m_yCursor = static_cast<SInt32>(oldPos.y);
	pos.x     = oldPos.x + dx;
	pos.y     = oldPos.y + dy;
	postMouseEvent(pos);

	// we now assume we don't know the current cursor position
	m_cursorPosValid = false;
}

void
OSXScreen::fakeMouseWheel(SInt32 xDelta, SInt32 yDelta) const
{
	if (needsInputDispatch()) {
		invokeInputState([this, xDelta, yDelta] { fakeMouseWheel(xDelta, yDelta); });
		return;
	}
	if (xDelta != 0 || yDelta != 0) {
		// create a scroll event, post it and release it.  not sure if kCGScrollEventUnitLine
		// is the right choice here over kCGScrollEventUnitPixel
		CGEventRef scrollEvent = CGEventCreateScrollWheelEvent(
			NULL, kCGScrollEventUnitLine, 2,
			mapScrollWheelFromBarrier(yDelta),
			-mapScrollWheelFromBarrier(xDelta));

        // Fix for sticky keys
        CGEventFlags modifiers = m_keyState->getModifierStateAsOSXFlags();
        CGEventSetFlags(scrollEvent, modifiers);

		CGEventSetIntegerValueField(scrollEvent, kCGEventSourceUserData, kWeavePointerEventMarker);
		CGEventPost(kCGHIDEventTap, scrollEvent);
		CFRelease(scrollEvent);
	}
}

void
OSXScreen::showCursor()
{
	// Input capture and visibility have independent lifetimes. In particular,
	// a failed hide/show must never skip reconnecting the physical mouse.
	if (m_isPrimary) setPointerDetached(false);
	// CoreGraphics counts hide/show requests. Only release our own hide.
	if (!m_cursorHidden) {
		return;
	}
	LOG((CLOG_DEBUG "showing cursor"));

	CFStringRef propertyString = CFStringCreateWithCString(
		NULL, "SetsCursorInBackground", kCFStringEncodingMacRoman);

	CGSSetConnectionProperty(
		_CGSDefaultConnection(), _CGSDefaultConnection(),
		propertyString, kCFBooleanTrue);

	CFRelease(propertyString);

	CGError error = CGDisplayShowCursor(m_displayID);
	if (error != kCGErrorSuccess) {
		LOG((CLOG_ERR "failed to show cursor, error=%d", error));
		return;
	}

	// appears to fix "mouse randomly not showing" bug
	if (!m_isPrimary) CGAssociateMouseAndMouseCursorPosition(true);

	logCursorVisibility();

	m_cursorHidden = false;
}

void
OSXScreen::hideCursor()
{
	if (m_cursorHidden) {
		return;
	}
	LOG((CLOG_DEBUG "hiding cursor"));

	CFStringRef propertyString = CFStringCreateWithCString(
		NULL, "SetsCursorInBackground", kCFStringEncodingMacRoman);

	CGSSetConnectionProperty(
		_CGSDefaultConnection(), _CGSDefaultConnection(),
		propertyString, kCFBooleanTrue);

	CFRelease(propertyString);

	CGError error = CGDisplayHideCursor(m_displayID);
	if (error != kCGErrorSuccess) {
		LOG((CLOG_ERR "failed to hide cursor, error=%d", error));
		return;
	}

	// appears to fix "mouse randomly not hiding" bug
	if (!m_isPrimary) CGAssociateMouseAndMouseCursorPosition(true);

	logCursorVisibility();

	m_cursorHidden = true;
}

bool
OSXScreen::setPointerDetached(bool detached)
{
	if (m_pointerDetached == detached) return true;
	const CGError error = CGAssociateMouseAndMouseCursorPosition(!detached);
	if (error != kCGErrorSuccess) {
		LOG((CLOG_ERR "failed to %s physical mouse, error=%d",
			detached ? "capture" : "restore", error));
		return false;
	}
	m_pointerDetached = detached;
	LOG((CLOG_INFO "physical mouse %s", detached ? "captured for remote input" : "restored locally"));
	return true;
}

void
OSXScreen::enable()
{
	// Reconnect/resume can occur without a display callback.
	updateScreenShape();
	m_scrollSettings.requestRefresh();
	m_nextScrollSettingsRefresh = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	// watch the clipboard
	// Poll only memory on the input thread. Native polling and all conversion
	// run on the worker; this also drains completions without a cross-thread
	// callback retaining the screen or its event queue during teardown.
	// The worker throttles unchanged native polling to the original 1 second.
	m_clipboardTimer = m_events->newTimer(0.1, NULL);
	m_events->adoptHandler(Event::kTimer, m_clipboardTimer,
							new TMethodEventJob<OSXScreen>(this,
								&OSXScreen::handleClipboardCheck));

	try {
		if (m_isPrimary) {
			try {
				m_inputLoop.reset(new OSXInputLoop);
			}
			catch (const std::exception& e) {
				throw XInputUnavailable(String("cannot start primary input owner: ") + e.what());
			}
			if (!m_inputLoop->invoke([this] { createEventTap(); })) {
				throw XInputUnavailable("primary input owner did not start");
			}
		}
		else createEventTap();
	}
	catch (...) {
		// Screen::enable has not committed m_enabled, so its normal disable
		// path will not run. Roll back the partially created owner/timer here.
		disable();
		throw;
	}
}

void
OSXScreen::createEventTap()
{
	if (m_isPrimary) initializeInputState();

	if (m_isPrimary) {
		// FIXME -- start watching jump zones
		initializePointerMotion();

		// kCGEventTapOptionDefault = 0x00000000 (Missing in 10.4, so specified literally)
		m_eventTapPort = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
										kCGEventMaskForAllEvents,
										handleCapturedInput,
										this);
	}
	else {
		// FIXME -- prevent system from entering power save mode

		if (m_autoShowHideCursor) {
			hideCursor();
		}

		// warp the mouse to the cursor center
		SInt32 x, y, w, h, cx, cy;
		getShape(x, y, w, h);
		getCursorCenter(cx, cy);
		if (w > 0 && h > 0) fakeMouseMove(cx, cy);

                // there may be a better way to do this, but we register an event handler even if we're
                // not on the primary display (acting as a client). This way, if a local event comes in
                // (either keyboard or mouse), we can make sure to show the cursor if we've hidden it.
		m_eventTapPort = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
										kCGEventMaskForAllEvents,
										handleCGInputEventSecondary,
										this);
	}

	if (!m_eventTapPort) {
		throw XInputUnavailable("failed to create quartz event tap");
	}

	m_eventTapRLSR = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, m_eventTapPort, 0);
	if (!m_eventTapRLSR) {
		CFRelease(m_eventTapPort);
		m_eventTapPort = nullptr;
		throw XInputUnavailable("failed to create quartz event tap source");
	}

	CFRunLoopAddSource(CFRunLoopGetCurrent(), m_eventTapRLSR, kCFRunLoopDefaultMode);
	if (m_isPrimary) {
		// Local input must never wait for Weave's event queue or native state
		// maintenance. Only remote ownership requires an active filter.
		CGEventTapEnable(m_eventTapPort, false);
		m_localEventTapPort = CGEventTapCreate(kCGHIDEventTap,
			kCGHeadInsertEventTap, kCGEventTapOptionListenOnly,
			kCGEventMaskForAllEvents, handleLocalInput, this);
		if (!m_localEventTapPort)
			throw XInputUnavailable("failed to create local input observer");
		m_localEventTapSource = CFMachPortCreateRunLoopSource(
			kCFAllocatorDefault, m_localEventTapPort, 0);
		if (!m_localEventTapSource)
			throw XInputUnavailable("failed to create local input observer source");
		CFRunLoopAddSource(CFRunLoopGetCurrent(), m_localEventTapSource, kCFRunLoopDefaultMode);
	}
}

void
OSXScreen::setInputCapture(bool capture)
{
	if (!m_isPrimary || !m_eventTapPort || !m_localEventTapPort) return;
	// Both callbacks run on this owner. Their ownership guards also reject
	// queued observations left over from the previous mode.
	if (capture) {
		CGEventTapEnable(m_eventTapPort, true);
		CGEventTapEnable(m_localEventTapPort, false);
	}
	else {
		CGEventTapEnable(m_localEventTapPort, true);
		CGEventTapEnable(m_eventTapPort, false);
	}
}

CGEventRef
OSXScreen::handleLocalInput(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void* context)
{
	auto* screen = static_cast<OSXScreen*>(context);
	if (!screen->m_isOnScreen.load()) return event;
	if (type == kCGEventTapDisabledByTimeout) {
		if (screen->m_localEventTapPort) CGEventTapEnable(screen->m_localEventTapPort, true);
		return event;
	}
	handleCGInputEvent(proxy, type, event, context);
	return event; // A passive observation cannot consume the native event.
}

CGEventRef
OSXScreen::handleCapturedInput(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void* context)
{
	auto* screen = static_cast<OSXScreen*>(context);
	if (screen->m_isOnScreen.load()) {
		// A queued remote motion must not move the pointer back after return.
		const bool motion = type == kCGEventMouseMoved || type == kCGEventLeftMouseDragged ||
			type == kCGEventRightMouseDragged || type == kCGEventOtherMouseDragged;
		const auto ticks = motion && event ? pointerEventTimestampTicks(event) : 0;
		return ticks && ticks <= screen->m_localMotionAfterTicks ? nullptr : event;
	}
	return handleCGInputEvent(proxy, type, event, context);
}

void
OSXScreen::disable()
{
	if (m_clipboardWorker) m_clipboardWorker->cancel();
	if (m_inputLoop) {
		m_inputLoop->stop([this] { destroyEventTap(); });
		m_inputLoop.reset();
	}
	else destroyEventTap();

	// disable drag handling
	m_dragNumButtonsDown = 0;
	enableDragTimer(false);

	// uninstall clipboard timer
	if (m_clipboardTimer != NULL) {
		m_events->removeHandler(Event::kTimer, m_clipboardTimer);
		m_events->deleteTimer(m_clipboardTimer);
		m_clipboardTimer = NULL;
	}
	m_isOnScreen = m_isPrimary;
}

void
OSXScreen::destroyEventTap()
{
	// stop() has drained accepted release commands. Settle any physical
	// modifier-up suppressed while remote before invalidating its evidence.
	if (m_isPrimary)
		m_keyState->completeSuppressedModifierReleases(m_pendingNativeModifierReleases);
	m_pointerPositionChanges.clear();
	m_lastNativePointerValid = false;
	m_primaryKeysDown.reset();
	m_primaryModifiers = 0;
	m_pendingNativeModifierReleases = 0;
	m_keyState->clearObservedPrimaryModifiers();
	m_handoffKeyGenerations.clear();
	m_handoffButtonGenerations.clear();
	// Called on the loop that created the tap, after its last callback.
	if (m_autoShowHideCursor || m_isPrimary) {
		showCursor();
	}

	// FIXME -- stop watching jump zones, stop capturing input

	if (m_localEventTapSource) {
		CFRunLoopRemoveSource(CFRunLoopGetCurrent(), m_localEventTapSource, kCFRunLoopDefaultMode);
		CFRelease(m_localEventTapSource);
		m_localEventTapSource = nullptr;
	}
	if (m_localEventTapPort) {
		CGEventTapEnable(m_localEventTapPort, false);
		CFRelease(m_localEventTapPort);
		m_localEventTapPort = nullptr;
	}

	if (m_eventTapRLSR) {
		CFRunLoopRemoveSource(CFRunLoopGetCurrent(), m_eventTapRLSR, kCFRunLoopDefaultMode);
		CFRelease(m_eventTapRLSR);
		m_eventTapRLSR = nullptr;
	}

	if (m_eventTapPort) {
		CGEventTapEnable(m_eventTapPort, false);
		CFRelease(m_eventTapPort);
		m_eventTapPort = nullptr;
	}
}

void
OSXScreen::enter()
{
	if (m_inputLoop && !m_inputLoop->isOwner()) {
		if (!m_inputLoop->invoke([this] { enter(); })) {
			sendEvent(m_events->forIScreen().error());
		}
		return;
	}
	showCursor();

	if (m_isPrimary) {
		if (m_pointerDetached) {
			// Restoring the hardware pointer failed. Do not claim local input
			// ownership and leave the user frozen until a manual quit. Normal
			// event-loop shutdown runs disable(), which retries restoration.
			LOG((CLOG_ERR "stopping sharing because physical mouse restoration failed"));
			sendEvent(m_events->forIScreen().error());
			return;
		}
		setZeroSuppressionInterval();
		if (!m_isOnScreen.load()) {
			m_localMotionAfterTicks = nativeInputTimestampTicks();
			if (!m_keyState->hasSyntheticKeyBindings()) {
				m_keyState->completeSuppressedModifierReleases(m_pendingNativeModifierReleases);
				m_pendingNativeModifierReleases = 0;
			}
		}
	}
	else {
		// reset buttons
		m_buttonState.reset();

		// patch by Yutaka Tsutano
		// wakes the client screen
		// http://symless.com/spit/issues/details/3287#c12
		io_registry_entry_t entry = IORegistryEntryFromPath(
			kIOMainPortDefault,
			"IOService:/IOResources/IODisplayWrangler");

		if (entry != MACH_PORT_NULL) {
			IORegistryEntrySetCFProperty(entry, CFSTR("IORequestIdle"), kCFBooleanFalse);
			IOObjectRelease(entry);
		}

		avoidSupression();
	}

	// now on screen
	m_isOnScreen = true;
	setInputCapture(false);
}

bool
OSXScreen::tryEnter()
{
	enter();
	return m_isOnScreen.load();
}

bool
OSXScreen::tryEnterAt(SInt32 x, SInt32 y)
{
	if (m_inputLoop && !m_inputLoop->isOwner()) {
		bool accepted = false;
		if (!m_inputLoop->invoke([this, x, y, &accepted] { accepted = tryEnterAt(x, y); })) {
			sendEvent(m_events->forIScreen().error());
			return false;
		}
		return accepted;
	}
	// One owner operation: no remote callback may park between this warp
	// and the local transition. Keep no-position recovery in tryEnter().
	if (warpPointer(CGPointMake(x, y)) != kCGErrorSuccess) {
		showCursor();
		m_isOnScreen = !m_pointerDetached;
		sendEvent(m_events->forIScreen().error());
		return false;
	}
	m_xCursor = x;
	m_yCursor = y;
	m_cursorPosValid = true;
	return tryEnter();
}

bool
OSXScreen::leave()
{
	if (m_inputLoop && !m_inputLoop->isOwner()) {
		bool accepted = false;
		if (!m_inputLoop->invoke([this, &accepted] { accepted = leave(); })) return false;
		return accepted;
	}
	if (m_isPrimary) {
		if (!m_isOnScreen.load()) return true;
		SInt32 exitX, exitY, cx, cy;
		getCursorPos(exitX, exitY);
		getCursorCenter(cx, cy);
		m_parkPoint = CGPointMake(cx, cy);
		// Native motion can already contain this warp before leave returns.
		// Select the capturing tap first: the passive observer drops callbacks
		// queued across the ownership change, which would otherwise lose the
		// warp-bearing event and apply its compensation to a later real delta.
		setInputCapture(true);
		if (warpPointer(m_parkPoint) != kCGErrorSuccess) {
			setInputCapture(false);
			return false;
		}

		// A warp alone doesn't send a hover event. Give the Dock a leave
		// before hiding, below our HID tap so this is never client motion.
		CGEventRef hover = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved,
			m_parkPoint, kCGMouseButtonLeft);
		if (hover) {
			CGEventSetIntegerValueField(hover, kCGMouseEventDeltaX, 0);
			CGEventSetIntegerValueField(hover, kCGMouseEventDeltaY, 0);
			CGEventPost(kCGSessionEventTap, hover);
			CFRelease(hover);
			hideCursor();
		}
		if (!hover || !m_cursorHidden || !setPointerDetached(true)) {
			showCursor();
			if (warpPointer(CGPointMake(exitX, exitY)) != kCGErrorSuccess)
				sendEvent(m_events->forIScreen().error());
			setInputCapture(false);
			return false;
		}
	}
	else hideCursor();

	if (isDraggingStarted()) {
		if (!m_isPrimary) {
			// Only the secondary sends a file list here. Server-side drag
			// metadata is prepared by Server::sendDragInfo before handoff.
			// Never query a native pasteboard on the primary input owner after
			// detaching the hardware mouse: a slow provider would freeze input.
			String& fileList = getDraggingFilename();
			if (fileList.empty() == false) {
				ClientApp& app = ClientApp::instance();
				Client* client = app.getClientPtr();

				DragInformation di;
				di.setFilename(fileList);
				if (barrier::fs::is_directory(barrier::fs::u8path(fileList))) {
					di.setEntryType(DragInformation::Directory);
				}
				DragFileList dragFileList;
				dragFileList.push_back(di);
				String info;
				UInt32 fileCount = DragInformation::setupDragInfo(
					dragFileList, info);
				client->sendDragInfo(fileCount, info, info.size());
				LOG((CLOG_DEBUG "send dragging file to server"));

				// TODO: what to do with multiple file or even
				// a folder
				client->sendFileToServer(fileList);
			}
		}
		m_draggingStarted = false;
	}

	if (m_isPrimary) avoidHesitatingCursor();

	// now off screen
	m_isOnScreen = false;

	return true;
}

bool
OSXScreen::setClipboard(ClipboardID id, const IClipboard* src)
{
	if (src == nullptr) return true;
	return setClipboardSnapshot(id, std::make_shared<const String>(IClipboard::marshall(src)), 0);
}

bool
OSXScreen::setClipboardSnapshot(ClipboardID id, const std::shared_ptr<const String>& data)
{
	return setClipboardSnapshot(id, data, 0);
}

bool
OSXScreen::setClipboardSnapshot(ClipboardID id, const std::shared_ptr<const String>& data,
                                std::uint64_t publicationId)
{
	return m_clipboardWorker && m_clipboardWorker->publish(id, data, publicationId);
}

void
OSXScreen::checkClipboards()
{
	if (!m_clipboardWorker) return;
	UInt32 sequence;
	if (m_clipboardWorker->takeReady(&sequence)) {
		for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
			for (auto type : {m_events->forClipboard().clipboardGrabbed(), m_events->forClipboard().clipboardChanged()}) {
				auto* info = static_cast<ClipboardInfo*>(malloc(sizeof(ClipboardInfo)));
				if (!info) continue;
				info->m_id = id;
				info->m_sequenceNumber = sequence;
				sendEvent(type, info);
			}
		}
	}
	for (const auto& result : m_clipboardWorker->takePublications()) {
		auto* info = static_cast<ClipboardPublicationInfo*>(malloc(sizeof(ClipboardPublicationInfo)));
		if (!info) continue;
		*info = result;
		sendEvent(m_events->forClipboard().clipboardPublished(), info);
	}
	m_clipboardWorker->poll(m_sequenceNumber);
}

void
OSXScreen::openScreensaver(bool notify)
{
	m_screensaverNotify = notify;
	if (!m_screensaverNotify) {
		m_screensaver->disable();
	}
}

void
OSXScreen::closeScreensaver()
{
	if (!m_screensaverNotify) {
		m_screensaver->enable();
	}
}

void
OSXScreen::screensaver(bool activate)
{
	if (activate) {
		m_screensaver->activate();
	}
	else {
		m_screensaver->deactivate();
	}
}

void
OSXScreen::resetOptions()
{
	// no options
}

void
OSXScreen::setOptions(const OptionsList&)
{
	// no options
}

void
OSXScreen::setSequenceNumber(UInt32 seqNum)
{
	m_sequenceNumber = seqNum;
}

bool
OSXScreen::isPrimary() const
{
	return m_isPrimary;
}

void
OSXScreen::sendEvent(Event::Type type, void* data) const
{
	m_events->addEvent(Event(type, getEventTarget(), data));
}

void
OSXScreen::sendClipboardEvent(Event::Type type, ClipboardID id) const
{
	ClipboardInfo* info   = (ClipboardInfo*)malloc(sizeof(ClipboardInfo));
	info->m_id             = id;
	info->m_sequenceNumber = m_sequenceNumber;
	sendEvent(type, info);
}

void
OSXScreen::handleSystemEvent(const Event& event, void*)
{
	EventRef* carbonEvent = static_cast<EventRef*>(event.getData());
	assert(carbonEvent != NULL);

	UInt32 eventClass = GetEventClass(*carbonEvent);

	switch (eventClass) {
	case kEventClassMouse:
		switch (GetEventKind(*carbonEvent)) {
		case kBarrierEventMouseScroll:
		{
			OSStatus r;
			long xScroll;
			long yScroll;

			// get scroll amount
			r = GetEventParameter(*carbonEvent,
					kBarrierMouseScrollAxisX,
					typeSInt32,
					NULL,
					sizeof(xScroll),
					NULL,
					&xScroll);
			if (r != noErr) {
				xScroll = 0;
			}
			r = GetEventParameter(*carbonEvent,
					kBarrierMouseScrollAxisY,
					typeSInt32,
					NULL,
					sizeof(yScroll),
					NULL,
					&yScroll);
			if (r != noErr) {
				yScroll = 0;
			}

			if (xScroll != 0 || yScroll != 0) {
				onMouseWheel(-mapScrollWheelToBarrier(xScroll),
								mapScrollWheelToBarrier(yScroll));
			}
		}
		}
		break;

	case kEventClassKeyboard:
			switch (GetEventKind(*carbonEvent)) {
				case kEventHotKeyPressed:
				case kEventHotKeyReleased:
					onHotKey(*carbonEvent);
					break;
			}

			break;

	case kEventClassWindow:
		// 2nd param was formerly GetWindowEventTarget(m_userInputWindow) which is 32-bit only,
		// however as m_userInputWindow is never initialized to anything we can take advantage of
		// the fact that GetWindowEventTarget(NULL) == NULL
		SendEventToEventTarget(*carbonEvent, NULL);
		switch (GetEventKind(*carbonEvent)) {
		case kEventWindowActivated:
			LOG((CLOG_DEBUG1 "window activated"));
			break;

		case kEventWindowDeactivated:
			LOG((CLOG_DEBUG1 "window deactivated"));
			break;

		case kEventWindowFocusAcquired:
			LOG((CLOG_DEBUG1 "focus acquired"));
			break;

		case kEventWindowFocusRelinquish:
			LOG((CLOG_DEBUG1 "focus released"));
			break;
		}
		break;

	default:
		SendEventToEventTarget(*carbonEvent, GetEventDispatcherTarget());
		break;
	}
}

bool
OSXScreen::onMouseMove(CGFloat mx, CGFloat my)
{
	// Absolute positions are meaningful only on this screen. Remote motion is
	// supplied by the HID event's delta fields, not a mutable warp baseline.
	if (!m_isOnScreen.load()) return true;
	{
		std::lock_guard<std::mutex> lock(m_shapeMutex);
		if (m_displayConfiguring || m_w <= 0 || m_h <= 0) return true;
	}
	LOG((CLOG_DEBUG2 "mouse move %+f,%+f", mx, my));
	m_xCursor = (SInt32)mx;
	m_yCursor = (SInt32)my;
	sendEvent(m_events->forIPrimaryScreen().motionOnPrimary(),
		MotionInfo::alloc((SInt32)mx, (SInt32)my));
	if (m_buttonState.test(0)) {
		m_draggingStarted = true;
	}

	return true;
}

bool
OSXScreen::onMouseButton(bool pressed, UInt16 macButton)
{
	// Buttons 2 and 3 are inverted on the mac
	ButtonID button = mapMacButtonToBarrier(macButton);

	if (pressed) {
		LOG((CLOG_DEBUG1 "event: button press button=%d", button));
		if (button != kButtonNone) {
			KeyModifierMask mask = m_isPrimary ? m_primaryModifiers.load() : m_keyState->getActiveModifiers();
			sendEvent(m_events->forIPrimaryScreen().buttonDown(), ButtonInfo::alloc(button, mask));
		}
	}
	else {
		LOG((CLOG_DEBUG1 "event: button release button=%d", button));
		if (button != kButtonNone) {
			KeyModifierMask mask = m_isPrimary ? m_primaryModifiers.load() : m_keyState->getActiveModifiers();
			sendEvent(m_events->forIPrimaryScreen().buttonUp(), ButtonInfo::alloc(button, mask));
		}
	}

	// handle drags with any button other than button 1 or 2
	if (macButton > 2) {
		if (pressed) {
			// one more button
			if (m_dragNumButtonsDown++ == 0) {
				enableDragTimer(true);
			}
		}
		else {
			// one less button
			if (--m_dragNumButtonsDown == 0) {
				enableDragTimer(false);
			}
		}
	}

	if (macButton == kButtonLeft) {
		EMouseButtonState state = pressed ? kMouseButtonDown : kMouseButtonUp;
		m_buttonState.set(kButtonLeft - 1, state);
		if (pressed) {
			m_draggingFilename.clear();
			LOG((CLOG_DEBUG2 "dragging file directory is cleared"));
		}
		else {
			if (m_fakeDraggingStarted) {
                finishFakeDrag();
			}

			m_draggingStarted = false;
		}
	}

	return true;
}

bool
OSXScreen::onMouseWheel(SInt32 xDelta, SInt32 yDelta) const
{
	LOG((CLOG_DEBUG1 "event: button wheel delta=%+d,%+d", xDelta, yDelta));
	sendEvent(m_events->forIPrimaryScreen().wheel(), WheelInfo::alloc(xDelta, yDelta));
	return true;
}

void
OSXScreen::handleClipboardCheck(const Event&, void*)
{
	// Reuse maintenance wakeups; never consult preferences in an input callback.
	const auto now = std::chrono::steady_clock::now();
	if (now >= m_nextScrollSettingsRefresh) {
		m_scrollSettings.requestRefresh();
		m_nextScrollSettingsRefresh = now + std::chrono::seconds(5);
	}
	SInt32 x, y, w, h;
	getShape(x, y, w, h);
	// Refresh geometry only while it is unavailable.
	if (w <= 0 || h <= 0) updateScreenShape();
	checkClipboards();
}

void
OSXScreen::displayReconfigurationCallback(CGDirectDisplayID, CGDisplayChangeSummaryFlags flags, void* inUserData)
{
	OSXScreen* screen = (OSXScreen*)inUserData;
	{
		std::lock_guard<std::mutex> lock(screen->m_shapeMutex);
		// Apple guarantees display state only AFTER configuration, not at Begin.
		screen->m_displayConfiguring = (flags & kCGDisplayBeginConfigurationFlag) != 0;
		// getShape may already have returned unavailable before this event drains.
		// Even a same-size completion must then notify the peer of recovery.
		if (screen->m_displayConfiguring) {
			screen->m_shapeNeedsNotify = true;
			screen->m_displayConfigurationStarted = std::chrono::steady_clock::now();
		}
		++screen->m_displayGeneration;
	}
	// The native callback may run off-thread. Sample and notify on our event queue.
	// Completion with flags == 0 (or only SetMain) must also refresh geometry.
	screen->m_events->addEvent(Event(screen->m_displayChangeEvent, screen));
}

void
OSXScreen::onModifierFlags(CGEventFlags flags, bool flagsChanged)
{
	KeyModifierMask oldMask = m_isPrimary ? m_primaryModifiers.load() : getActiveModifiers();
	KeyModifierMask newMask = m_keyState->mapModifiersFromOSX(flags);
	if (!flagsChanged) {
		// On ordinary events NumericPad describes the key's origin, not a
		// NumLock transition. Preserve that logical modifier independently.
		newMask = (newMask & ~KeyModifierNumLock) | (oldMask & KeyModifierNumLock);
	}
	m_keyState->handleModifierKeys(getEventTarget(), oldMask, newMask);
	if (m_isPrimary) {
		if (m_isOnScreen.load()) {
			// This native event is delivered locally, so it already completes
			// the local modifier state. Do not post a second flags-up later.
			m_pendingNativeModifierReleases = 0;
		}
		else {
			const KeyModifierMask momentary = KeyModifierShift | KeyModifierControl |
				KeyModifierAlt | KeyModifierSuper;
			m_pendingNativeModifierReleases |= oldMask & ~newMask & momentary;
			m_pendingNativeModifierReleases &= ~newMask;
		}
		m_primaryModifiers = newMask;
		m_keyState->setObservedPrimaryModifiers(newMask, flags);
		m_keyState->onKey(0, false, newMask);
	}
	if (!flagsChanged && oldMask == newMask) return;

	// Preserve exactly one hotkey release even if the flags callback was
	// missed and a later ordinary event is the first evidence of the change.
	if (m_activeModifierHotKey == 0) {
		if (m_modifierHotKeys.count(newMask) > 0) {
			m_activeModifierHotKey     = m_modifierHotKeys[newMask];
			m_activeModifierHotKeyMask = newMask;
			m_events->addEvent(Event(m_events->forIPrimaryScreen().hotKeyDown(),
							getEventTarget(),
							HotKeyInfo::alloc(m_activeModifierHotKey)));
		}
	}
	else {
		KeyModifierMask mask = (newMask & m_activeModifierHotKeyMask);
		if (mask != m_activeModifierHotKeyMask) {
			m_events->addEvent(Event(m_events->forIPrimaryScreen().hotKeyUp(),
							getEventTarget(),
							HotKeyInfo::alloc(m_activeModifierHotKey)));
			m_activeModifierHotKey     = 0;
			m_activeModifierHotKeyMask = 0;
		}
	}
}

bool
OSXScreen::onKey(CGEventRef event)
{
	CGEventType eventKind = CGEventGetType(event);

	// get the key and active modifiers
	UInt32 virtualKey = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
	CGEventFlags macMask = CGEventGetFlags(event);
	LOG((CLOG_DEBUG1 "event: Key event kind: %d, keycode=%d", eventKind, virtualKey));

	const bool flagsChanged = eventKind == kCGEventFlagsChanged;
	if (flagsChanged || m_isPrimary) {
		// This event's flags belong to its position in the native stream.
		// They repair missed modifier callbacks without allowing a newer
		// physical poll to rewrite the context of a queued event.
		onModifierFlags(macMask, flagsChanged);
	}
	if (flagsChanged) return true;

	HotKeyToIDMap::const_iterator i = m_hotKeyToIDMap.find(HotKeyItem(virtualKey, m_keyState->mapModifiersToCarbon(macMask) & 0xff00u));
	if (i != m_hotKeyToIDMap.end()) {
		UInt32 id = i->second;
		// determine event type
		Event::Type type;
		//UInt32 eventKind = GetEventKind(event);
		if (eventKind == kCGEventKeyDown) {
			type = m_events->forIPrimaryScreen().hotKeyDown();
		}
		else if (eventKind == kCGEventKeyUp) {
			type = m_events->forIPrimaryScreen().hotKeyUp();
		}
		else {
			return false;
		}
		m_events->addEvent(Event(type, getEventTarget(), HotKeyInfo::alloc(id)));
		return true;
	}

	// decode event type
	bool down	  = (eventKind == kCGEventKeyDown);
	bool up		  = (eventKind == kCGEventKeyUp);
	bool isRepeat = (CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) == 1);

	// map event to keys
	KeyModifierMask mask;
	OSXKeyState::KeyIDs keys;
	KeyButton button = m_keyState->mapKeyFromEvent(keys, &mask, event);
	if (button == 0) {
		return false;
	}

	// check for AltGr in mask.  if set we send neither the AltGr nor
	// the super modifiers to clients then remove AltGr before passing
	// the modifiers to onKey.
	KeyModifierMask sendMask = (mask & ~KeyModifierAltGr);
	if ((mask & KeyModifierAltGr) != 0) {
		sendMask &= ~KeyModifierSuper;
	}
	mask &= ~KeyModifierAltGr;

	// update button state
	if (down) {
		m_keyState->onKey(button, true, mask);
		if (m_isPrimary) m_primaryKeysDown.set(button);
	}
	else if (up) {
		if (!m_keyState->isKeyDown(button) &&
			!(m_isPrimary && m_primaryKeysDown.test(button))) {
			// up event for a dead key.  throw it away.
			return false;
		}
		m_keyState->onKey(button, false, mask);
		if (m_isPrimary) m_primaryKeysDown.reset(button);
	}

	// send key events
	for (OSXKeyState::KeyIDs::const_iterator i = keys.begin();
							i != keys.end(); ++i) {
		m_keyState->sendKeyEvent(getEventTarget(), down, isRepeat,
							*i, sendMask, 1, button);
	}

	return true;
}

void
OSXScreen::onMediaKey(CGEventRef event)
{
	KeyID keyID;
	bool down;
	bool isRepeat;

	if (!getMediaKeyEventInfo (event, &keyID, &down, &isRepeat)) {
		LOG ((CLOG_ERR "Failed to decode media key event"));
		return;
	}
	if (m_isPrimary) onModifierFlags(CGEventGetFlags(event), false);

	LOG ((CLOG_DEBUG2 "Media key event: keyID=0x%02x, %s, repeat=%s",
						keyID, (down ? "down": "up"),
						(isRepeat ? "yes" : "no")));

	KeyButton button = 0;
	KeyModifierMask mask = m_isPrimary ? m_primaryModifiers.load() : m_keyState->getActiveModifiers();
	m_keyState->sendKeyEvent(getEventTarget(), down, isRepeat, keyID, mask, 1, button);
}

bool
OSXScreen::onHotKey(EventRef event) const
{
	if (needsInputDispatch()) {
		bool handled = false;
		invokeInputState([this, event, &handled] { handled = onHotKey(event); });
		return handled;
	}
	// get the hotkey id
	EventHotKeyID hkid = {};
	if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
				NULL, sizeof(EventHotKeyID), NULL, &hkid) != noErr ||
		hkid.signature != 'SNRG' || m_hotKeys.count(hkid.id) == 0) {
		return false;
	}
	UInt32 id = hkid.id;

	// determine event type
	Event::Type type;
	UInt32 eventKind = GetEventKind(event);
	if (eventKind == kEventHotKeyPressed) {
		type = m_events->forIPrimaryScreen().hotKeyDown();
	}
	else if (eventKind == kEventHotKeyReleased) {
		type = m_events->forIPrimaryScreen().hotKeyUp();
	}
	else {
		return false;
	}

	m_events->addEvent(Event(type, getEventTarget(),
								HotKeyInfo::alloc(id)));

	return true;
}

ButtonID
OSXScreen::mapBarrierButtonToMac(UInt16 button) const
{
    switch (button) {
    case 1:
        return kButtonLeft;
    case 2:
        return kMacButtonMiddle;
    case 3:
        return kMacButtonRight;
    }

    return static_cast<ButtonID>(button);
}

ButtonID
OSXScreen::mapMacButtonToBarrier(UInt16 macButton) const
{
	switch (macButton) {
	case 1:
		return kButtonLeft;

	case 2:
		return kButtonRight;

	case 3:
		return kButtonMiddle;
	}

	return static_cast<ButtonID>(macButton);
}

SInt32
OSXScreen::mapScrollWheelToBarrier(float x) const
{
	return m_scrollSettings.convert(x);
}

SInt32
OSXScreen::mapScrollWheelFromBarrier(float x) const
{
	// use server's acceleration with a little boost since other platforms
	// take one wheel step as a larger step than the mac does.
	return static_cast<SInt32>(3.0 * x / 120.0);
}

void
OSXScreen::enableDragTimer(bool enable)
{
	if (enable && m_dragTimer == NULL) {
		m_dragTimer = m_events->newTimer(0.01, NULL);
		m_events->adoptHandler(Event::kTimer, m_dragTimer,
							new TMethodEventJob<OSXScreen>(this,
								&OSXScreen::handleDrag));
		CGEventRef event = CGEventCreate(NULL);
		CGPoint mouse = CGEventGetLocation(event);
		m_dragLastPoint.h = (short)mouse.x;
		m_dragLastPoint.v = (short)mouse.y;
		CFRelease(event);
	}
	else if (!enable && m_dragTimer != NULL) {
		m_events->removeHandler(Event::kTimer, m_dragTimer);
		m_events->deleteTimer(m_dragTimer);
		m_dragTimer = NULL;
	}
}

void
OSXScreen::handleDrag(const Event& timerEvent, void* context)
{
	if (needsInputDispatch()) {
		invokeInputState([this, &timerEvent, context] { handleDrag(timerEvent, context); });
		return;
	}
	CGEventRef event = CGEventCreate(NULL);
	CGPoint p = CGEventGetLocation(event);
	CFRelease(event);

	if ((short)p.x != m_dragLastPoint.h || (short)p.y != m_dragLastPoint.v) {
		m_dragLastPoint.h = (short)p.x;
		m_dragLastPoint.v = (short)p.y;
		onMouseMove((SInt32)p.x, (SInt32)p.y);
	}
}

void
OSXScreen::updateButtons()
{
	if (needsInputDispatch()) {
		invokeInputState([this] { updateButtons(); });
		return;
	}
	UInt32 buttons = GetCurrentButtonState();

	m_buttonState.overwrite(buttons);
}

IKeyState*
OSXScreen::getKeyState() const
{
	return m_keyState;
}

bool
OSXScreen::needsInputDispatch() const
{
	return m_isPrimary && m_inputLoop && !m_inputLoop->isOwner();
}

void
OSXScreen::initializeInputState()
{
	m_primaryKeysDown.reset();
	for (KeyButton button = 1; button < IKeyState::kNumButtons; ++button) {
		m_primaryKeysDown.set(button, m_keyState->isKeyDown(button));
	}
	m_primaryModifiers = m_keyState->getActiveModifiers();
	m_pendingNativeModifierReleases = 0;
	m_keyState->setObservedPrimaryModifiers(m_primaryModifiers.load());
}

bool
OSXScreen::invokeInputState(std::function<void()> operation) const
{
	if (m_isPrimary && m_inputLoop) {
		if (!m_inputLoop->invoke(std::move(operation))) {
			LOG((CLOG_WARN "primary input state operation cancelled before execution"));
			return false;
		}
	}
	else operation();
	return true;
}

OSXInputLoop::ReleaseResult
OSXScreen::invokeInputRelease(std::function<void()> operation) const
{
	if (m_isPrimary && m_inputLoop) {
		return m_inputLoop->invokeRelease(std::move(operation));
	}
	operation();
	return OSXInputLoop::ReleaseResult::Completed;
}

void
OSXScreen::updateKeyMap()
{
	// Only explicit map updates rebuild the native layout. Ordinary input
	// and the periodic physical-state refresh never request a map rebuild.
	invokeInputState([this] { PlatformScreen::updateKeyMap(); });
}

void
OSXScreen::updateKeyState()
{
	invokeInputState([this] {
		// A primary refresh (including leavePrimary) must not erase the
		// binding needed to release local replay. Resume polling after release.
		if (m_isPrimary && m_keyState->hasSyntheticKeyBindings()) {
			updateButtons();
			return;
		}
		if (m_isPrimary && m_isOnScreen.load()) {
			m_keyState->completeSuppressedModifierReleases(m_pendingNativeModifierReleases);
			m_pendingNativeModifierReleases = 0;
		}
		PlatformScreen::updateKeyState();
	});
}

void
OSXScreen::setHalfDuplexMask(KeyModifierMask mask)
{
	invokeInputState([this, mask] { PlatformScreen::setHalfDuplexMask(mask); });
}

void
OSXScreen::fakeKeyDown(KeyID id, KeyModifierMask mask, KeyButton button)
{
	invokeInputState([this, id, mask, button] { PlatformScreen::fakeKeyDown(id, mask, button); });
}

bool
OSXScreen::fakeKeyRepeat(KeyID id, KeyModifierMask mask, SInt32 count, KeyButton button)
{
	bool repeated = false;
	invokeInputState([this, id, mask, count, button, &repeated] {
		repeated = PlatformScreen::fakeKeyRepeat(id, mask, count, button);
	});
	return repeated;
}

bool
OSXScreen::fakeKeyUp(KeyButton button)
{
	// A pending release can execute after this method returns. Keep its result
	// owned by the command; the owner remains alive until disable() joins it.
	const auto released = std::make_shared<bool>(false);
	const auto result = invokeInputRelease([this, button, released] {
		*released = PlatformScreen::fakeKeyUp(button);
	});
	return result == OSXInputLoop::ReleaseResult::Pending ||
		(result == OSXInputLoop::ReleaseResult::Completed && *released);
}

void
OSXScreen::fakeAllKeysUp()
{
	invokeInputRelease([this] {
		// A suppressed physical up can zero the shadow count without reaching
		// the local OS. Explicit replay mappings still owe their native up.
		for (const auto& held : m_handoffKeyGenerations)
			PlatformScreen::fakeKeyUp(held.first);
		PlatformScreen::fakeAllKeysUp();
		m_handoffKeyGenerations.clear();
	});
}

void
OSXScreen::fakeKeyDownForHandoff(KeyID id, KeyModifierMask mask, KeyButton button, std::uint64_t generation)
{
	invokeInputState([this, id, mask, button, generation] {
		PlatformScreen::fakeKeyDown(id, mask, button);
		if (m_keyState->syntheticKeyButton(button) != 0)
			m_handoffKeyGenerations[button] = generation;
	});
}

bool
OSXScreen::fakeKeyUpForHandoff(KeyButton button, std::uint64_t generation)
{
	const auto released = std::make_shared<bool>(false);
	const auto result = invokeInputRelease([this, button, generation, released] {
		auto held = m_handoffKeyGenerations.find(button);
		if (held == m_handoffKeyGenerations.end()) {
			*released = true; // Native delivery or an all-up already completed it.
		}
		else if (held->second == generation) {
			m_handoffKeyGenerations.erase(held);
			*released = PlatformScreen::fakeKeyUp(button);
		}
	});
	return result == OSXInputLoop::ReleaseResult::Pending ||
		(result == OSXInputLoop::ReleaseResult::Completed && *released);
}

void
OSXScreen::fakeMouseButtonForHandoff(ButtonID button, bool press, std::uint64_t generation)
{
	if (press) {
		invokeInputState([this, button, generation] {
			fakeMouseButton(button, true);
			m_handoffButtonGenerations[button] = generation;
		});
	}
	else {
		invokeInputRelease([this, button, generation] {
			auto held = m_handoffButtonGenerations.find(button);
			if (held != m_handoffButtonGenerations.end() && held->second == generation)
				fakeMouseButton(button, false);
		});
	}
}

bool
OSXScreen::completeLocalKeyRelease(KeyButton localButton)
{
	bool owned = false;
	for (const auto& held : m_handoffKeyGenerations) {
		if (m_keyState->syntheticKeyButton(held.first) == localButton) {
			owned = true;
			break;
		}
	}
	if (owned) {
		const auto retired = m_keyState->retireSyntheticKeyBindings(localButton);
		for (const auto button : retired) m_handoffKeyGenerations.erase(button);
		m_keyState->completeNativeKeyRelease(localButton);
	}
	return owned;
}

bool
OSXScreen::fakeCtrlAltDel()
{
	bool handled = false;
	invokeInputState([this, &handled] { handled = PlatformScreen::fakeCtrlAltDel(); });
	return handled;
}

bool
OSXScreen::isKeyDown(KeyButton button) const
{
	bool down = false;
	invokeInputState([this, button, &down] { down = PlatformScreen::isKeyDown(button); });
	return down;
}

KeyModifierMask
OSXScreen::getActiveModifiers() const
{
	KeyModifierMask mask = 0;
	invokeInputState([this, &mask] { mask = PlatformScreen::getActiveModifiers(); });
	return mask;
}

KeyModifierMask
OSXScreen::pollActiveModifiers() const
{
	// The input owner already publishes every observed modifier transition.
	// Waiting for it here stalls the server's handoff and socket event queue.
	if (m_isPrimary && m_inputLoop) {
		return m_primaryModifiers.load();
	}
	KeyModifierMask mask = 0;
	invokeInputState([this, &mask] { mask = PlatformScreen::pollActiveModifiers(); });
	return mask;
}

SInt32
OSXScreen::pollActiveGroup() const
{
	SInt32 group = 0;
	invokeInputState([this, &group] { group = PlatformScreen::pollActiveGroup(); });
	return group;
}

void
OSXScreen::pollPressedKeys(KeyButtonSet& pressedKeys) const
{
	invokeInputState([this, &pressedKeys] { PlatformScreen::pollPressedKeys(pressedKeys); });
}

void
OSXScreen::handleDisplayChange(const Event&, void*)
{
	updateScreenShape();
}

void
OSXScreen::updateScreenShape()
{
	UInt64 generation;
	bool configuring;
	{
		std::lock_guard<std::mutex> lock(m_shapeMutex);
		// Completion notifications can be lost. After a short settling interval,
		// let the existing invalid-only timer re-query, never reuse the old shape.
		if (m_displayConfiguring && std::chrono::steady_clock::now() -
			m_displayConfigurationStarted >= std::chrono::seconds(2)) {
			m_displayConfiguring = false;
		}
		generation = m_displayGeneration;
		configuring = m_displayConfiguring;
	}
	constexpr CGDisplayCount maxDisplays = 64;
	CGDirectDisplayID displays[maxDisplays];
	CGDisplayCount count = 0;
	CGRect total = CGRectNull, mainBounds = CGRectNull;
	auto valid = [](CGRect r) {
		// Match Server.cpp's minimum usable screen dimension: smaller transient
		// shapes cannot accept crossing and must not stop the recovery timer.
		constexpr double minUsableDimension = 64;
		const double min = std::numeric_limits<SInt32>::min();
		const double max = std::numeric_limits<SInt32>::max();
		return std::isfinite(r.origin.x) && std::isfinite(r.origin.y) &&
			std::isfinite(r.size.width) && std::isfinite(r.size.height) &&
			r.origin.x >= min && r.origin.y >= min &&
			r.size.width >= minUsableDimension && r.size.height >= minUsableDimension &&
			r.size.width <= max && r.size.height <= max &&
			r.origin.x + r.size.width <= max && r.origin.y + r.size.height <= max;
	};
	bool available = !configuring &&
		CGGetActiveDisplayList(0, nullptr, &count) == kCGErrorSuccess && count > 0 && count <= maxDisplays;
	if (available) {
		available = CGGetActiveDisplayList(maxDisplays, displays, &count) == kCGErrorSuccess &&
			count > 0 && count <= maxDisplays;
	}
	if (available) {
		const auto main = CGMainDisplayID();
		for (CGDisplayCount i = 0; i < count; ++i) {
			const CGRect rect = CGDisplayBounds(displays[i]);
			if (!valid(rect)) { available = false; break; }
			total = CGRectUnion(total, rect);
			if (displays[i] == main) mainBounds = rect;
		}
		available = available && valid(total) && valid(mainBounds);
	}
	bool changed;
	SInt32 x = 0, y = 0, w = 0, h = 0, cx = 0, cy = 0;
	{
		std::lock_guard<std::mutex> lock(m_shapeMutex);
		// Do not publish a sample taken across a native configuration transition.
		if (available && generation == m_displayGeneration && !m_displayConfiguring) {
			x = total.origin.x; y = total.origin.y; w = total.size.width; h = total.size.height;
			cx = mainBounds.origin.x + mainBounds.size.width / 2;
			cy = mainBounds.origin.y + mainBounds.size.height / 2;
		}
		changed = m_shapeNeedsNotify || x != m_x || y != m_y || w != m_w || h != m_h || cx != m_xCenter || cy != m_yCenter;
		m_shapeNeedsNotify = false;
		m_x = x; m_y = y; m_w = w; m_h = h; m_xCenter = cx; m_yCenter = cy;
	}
	if (changed) {
		sendEvent(m_events->forIScreen().shapeChanged());
		LOG((CLOG_DEBUG "screen shape: origin=%d,%d size=%dx%d", x, y, w, h));
	}
}

#pragma mark -

//
// FAST USER SWITCH NOTIFICATION SUPPORT
//
// OSXScreen::userSwitchCallback(void*)
//
// gets called if a fast user switch occurs
//

pascal OSStatus
OSXScreen::userSwitchCallback(EventHandlerCallRef nextHandler,
								EventRef theEvent,
								void* inUserData)
{
	OSXScreen* screen = (OSXScreen*)inUserData;
	UInt32 kind        = GetEventKind(theEvent);
	IEventQueue* events = screen->getEvents();

	if (kind == kEventSystemUserSessionDeactivated) {
		LOG((CLOG_DEBUG "user session deactivated"));
		events->addEvent(Event(events->forIScreen().suspend(),
									screen->getEventTarget()));
	}
	else if (kind == kEventSystemUserSessionActivated) {
		LOG((CLOG_DEBUG "user session activated"));
		events->addEvent(Event(events->forIScreen().resume(),
									screen->getEventTarget()));
	}
	return (CallNextEventHandler(nextHandler, theEvent));
}

#pragma mark -

//
// SLEEP/WAKEUP NOTIFICATION SUPPORT
//
// OSXScreen::watchSystemPowerThread(void*)
//
// main of thread monitoring system power (sleep/wakeup) using a CFRunLoop
//

void OSXScreen::watchSystemPowerThread()
{
	io_object_t				notifier;
	IONotificationPortRef	notificationPortRef;
	CFRunLoopSourceRef		runloopSourceRef = 0;
	CFRunLoopRef				runloop = CFRunLoopGetCurrent();

	// install system power change callback
	m_pmRootPort = IORegisterForSystemPower(this, &notificationPortRef,
											powerChangeCallback, &notifier);
	if (m_pmRootPort == 0) {
		LOG((CLOG_WARN "IORegisterForSystemPower failed"));
	}
	else {
		runloopSourceRef =
			IONotificationPortGetRunLoopSource(notificationPortRef);
		CFRunLoopAddSource(runloop, runloopSourceRef,
								kCFRunLoopCommonModes);
	}

	// thread is ready
	{
		Lock lock(m_pmMutex);
		m_pmRunloop = (CFRunLoopRef)CFRetain(runloop);
		*m_pmThreadReady = true;
		m_pmThreadReady->signal();
	}

	// if we were unable to initialize then exit.  we must do this after
	// setting m_pmThreadReady to true otherwise the parent thread will
	// block waiting for it.
	if (m_pmRootPort == 0) {
		LOG((CLOG_WARN "failed to init watchSystemPowerThread"));
		return;
	}

	LOG((CLOG_DEBUG "started watchSystemPowerThread"));

	LOG((CLOG_DEBUG "waiting for event loop"));
	m_events->waitForReady();

#if defined(MAC_OS_X_VERSION_10_7)
	{
		Lock lockCarbon(m_carbonLoopMutex);
		if (*m_carbonLoopReady == false) {

			// we signalling carbon loop ready before starting
			// unless we know how to do it within the loop
			LOG((CLOG_DEBUG "signalling carbon loop ready"));

			*m_carbonLoopReady = true;
			m_carbonLoopReady->signal();
		}
	}
#endif

	// start the run loop
	LOG((CLOG_DEBUG "starting carbon loop"));
	CFRunLoopRun();
	LOG((CLOG_DEBUG "carbon loop has stopped"));

	// cleanup
	if (notificationPortRef) {
		CFRunLoopRemoveSource(runloop,
								runloopSourceRef, kCFRunLoopDefaultMode);
		CFRunLoopSourceInvalidate(runloopSourceRef);
		CFRelease(runloopSourceRef);
	}

	IODeregisterForSystemPower(&notifier);
	{
		Lock lock(m_pmMutex);
		m_pmRootPort = 0;
	}
	LOG((CLOG_DEBUG "stopped watchSystemPowerThread"));
}

void
OSXScreen::powerChangeCallback(void* refcon, io_service_t service,
						  natural_t messageType, void* messageArg)
{
	((OSXScreen*)refcon)->handlePowerChangeRequest(messageType, messageArg);
}

void
OSXScreen::handlePowerChangeRequest(natural_t messageType, void* messageArg)
{
	// we've received a power change notification
	switch (messageType) {
	case kIOMessageSystemWillSleep:
		// OSXScreen has to handle this in the main thread so we have to
		// queue a confirm sleep event here.  we actually don't allow the
		// system to sleep until the event is handled.
		m_events->addEvent(Event(m_events->forOSXScreen().confirmSleep(),
								getEventTarget(), messageArg,
								Event::kDontFreeData));
		return;

	case kIOMessageSystemHasPoweredOn:
		LOG((CLOG_DEBUG "system wakeup"));
		m_events->addEvent(Event(m_events->forIScreen().resume(),
								getEventTarget()));
		break;

	default:
		break;
	}

	Lock lock(m_pmMutex);
	if (m_pmRootPort != 0) {
		IOAllowPowerChange(m_pmRootPort, (long)messageArg);
	}
}

void
OSXScreen::handleConfirmSleep(const Event& event, void*)
{
	long messageArg = (long)event.getData();
	if (messageArg != 0) {
		Lock lock(m_pmMutex);
		if (m_pmRootPort != 0) {
			// deliver suspend event immediately.
			m_events->addEvent(Event(m_events->forIScreen().suspend(),
									getEventTarget(), NULL,
									Event::kDeliverImmediately));

			LOG((CLOG_DEBUG "system will sleep"));
			IOAllowPowerChange(m_pmRootPort, messageArg);
		}
	}
}

#pragma mark -

//
// GLOBAL HOTKEY OPERATING MODE SUPPORT (10.3)
//
// CoreGraphics private API (OSX 10.3)
// Source: http://ichiro.nnip.org/osx/Cocoa/GlobalHotkey.html
//
// We load the functions dynamically because they're not available in
// older SDKs.  We don't use weak linking because we want users of
// older SDKs to build an app that works on newer systems and older
// SDKs will not provide the symbols.
//
// FIXME: This is hosed as of OS 10.5; patches to repair this are
// a good thing.
//
#if 0

#ifdef	__cplusplus
extern "C" {
#endif

typedef int CGSConnection;
typedef enum {
	CGSGlobalHotKeyEnable = 0,
	CGSGlobalHotKeyDisable = 1,
} CGSGlobalHotKeyOperatingMode;

extern CGSConnection _CGSDefaultConnection(void) WEAK_IMPORT_ATTRIBUTE;
extern CGError CGSGetGlobalHotKeyOperatingMode(CGSConnection connection, CGSGlobalHotKeyOperatingMode *mode) WEAK_IMPORT_ATTRIBUTE;
extern CGError CGSSetGlobalHotKeyOperatingMode(CGSConnection connection, CGSGlobalHotKeyOperatingMode mode) WEAK_IMPORT_ATTRIBUTE;

typedef CGSConnection (*_CGSDefaultConnection_t)(void);
typedef CGError (*CGSGetGlobalHotKeyOperatingMode_t)(CGSConnection connection, CGSGlobalHotKeyOperatingMode *mode);
typedef CGError (*CGSSetGlobalHotKeyOperatingMode_t)(CGSConnection connection, CGSGlobalHotKeyOperatingMode mode);

static _CGSDefaultConnection_t				s__CGSDefaultConnection;
static CGSGetGlobalHotKeyOperatingMode_t	s_CGSGetGlobalHotKeyOperatingMode;
static CGSSetGlobalHotKeyOperatingMode_t	s_CGSSetGlobalHotKeyOperatingMode;

#ifdef	__cplusplus
}
#endif

#define LOOKUP(name_)													\
	s_ ## name_ = NULL;													\
	if (NSIsSymbolNameDefinedWithHint("_" #name_, "CoreGraphics")) {	\
		s_ ## name_ = (name_ ## _t)NSAddressOfSymbol(					\
							NSLookupAndBindSymbolWithHint(				\
								"_" #name_, "CoreGraphics"));			\
	}

bool
OSXScreen::isGlobalHotKeyOperatingModeAvailable()
{
	if (!s_testedForGHOM) {
		s_testedForGHOM = true;
		LOOKUP(_CGSDefaultConnection);
		LOOKUP(CGSGetGlobalHotKeyOperatingMode);
		LOOKUP(CGSSetGlobalHotKeyOperatingMode);
		s_hasGHOM = (s__CGSDefaultConnection != NULL &&
					s_CGSGetGlobalHotKeyOperatingMode != NULL &&
					s_CGSSetGlobalHotKeyOperatingMode != NULL);
	}
	return s_hasGHOM;
}

void
OSXScreen::setGlobalHotKeysEnabled(bool enabled)
{
	if (isGlobalHotKeyOperatingModeAvailable()) {
		CGSConnection conn = s__CGSDefaultConnection();

		CGSGlobalHotKeyOperatingMode mode;
		s_CGSGetGlobalHotKeyOperatingMode(conn, &mode);

		if (enabled && mode == CGSGlobalHotKeyDisable) {
			s_CGSSetGlobalHotKeyOperatingMode(conn, CGSGlobalHotKeyEnable);
		}
		else if (!enabled && mode == CGSGlobalHotKeyEnable) {
			s_CGSSetGlobalHotKeyOperatingMode(conn, CGSGlobalHotKeyDisable);
		}
	}
}

bool
OSXScreen::getGlobalHotKeysEnabled()
{
	CGSGlobalHotKeyOperatingMode mode;
	if (isGlobalHotKeyOperatingModeAvailable()) {
		CGSConnection conn = s__CGSDefaultConnection();
		s_CGSGetGlobalHotKeyOperatingMode(conn, &mode);
	}
	else {
		mode = CGSGlobalHotKeyEnable;
	}
	return (mode == CGSGlobalHotKeyEnable);
}

#endif

//
// OSXScreen::HotKeyItem
//

OSXScreen::HotKeyItem::HotKeyItem(UInt32 keycode, UInt32 mask) :
	m_ref(NULL),
	m_keycode(keycode),
	m_mask(mask)
{
	// do nothing
}

OSXScreen::HotKeyItem::HotKeyItem(EventHotKeyRef ref,
				UInt32 keycode, UInt32 mask) :
	m_ref(ref),
	m_keycode(keycode),
	m_mask(mask)
{
	// do nothing
}

EventHotKeyRef
OSXScreen::HotKeyItem::getRef() const
{
	return m_ref;
}

bool
OSXScreen::HotKeyItem::operator<(const HotKeyItem& x) const
{
	return (m_keycode < x.m_keycode ||
			(m_keycode == x.m_keycode && m_mask < x.m_mask));
}

// Quartz event tap support for the secondary display. This makes sure that we
// will show the cursor if a local event comes in while barrier has the cursor
// off the screen.
CGEventRef
OSXScreen::handleCGInputEventSecondary(
	CGEventTapProxy proxy,
	CGEventType type,
	CGEventRef event,
	void* refcon)
{
	OSXScreen* screen = static_cast<OSXScreen*>(refcon);
	if (screen == nullptr) {
		return event;
	}
	if (type == kCGEventTapDisabledByTimeout) {
		if (screen->m_eventTapPort != nullptr) {
			CGEventTapEnable(screen->m_eventTapPort, true);
			LOG((CLOG_INFO "secondary quartz event tap timed out, re-enabling"));
		}
		return event;
	}
	if (type == kCGEventTapDisabledByUserInput || event == nullptr ||
		!screen->m_cursorHidden) {
		return event;
	}
	// Do not infer origin from position: remote input and local HID input can
	// both occur at the screen center. Nor should we require PID zero, since
	// legitimate local input drivers can have their own process identity.
	if (CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) == getpid() ||
		CGEventGetIntegerValueField(event, kCGEventSourceUserData) == kWeavePointerEventMarker) {
		return event;
	}
	switch (type) {
	case kCGEventMouseMoved:
	case kCGEventLeftMouseDragged:
	case kCGEventRightMouseDragged:
	case kCGEventOtherMouseDragged:
	case kCGEventLeftMouseDown:
	case kCGEventLeftMouseUp:
	case kCGEventRightMouseDown:
	case kCGEventRightMouseUp:
	case kCGEventOtherMouseDown:
	case kCGEventOtherMouseUp:
	case kCGEventScrollWheel:
		LOG((CLOG_DEBUG "restoring secondary cursor for local pointer input, type=%d", type));
		screen->showCursor();
		break;
	default:
		break;
	}
	return event;
}

// Quartz event tap support
CGEventRef
OSXScreen::handleCGInputEvent(CGEventTapProxy proxy,
							   CGEventType type,
							   CGEventRef event,
							   void* refcon)
{
	@autoreleasepool {
	OSXScreen* screen = (OSXScreen*)refcon;
	CGPoint pos;
	const bool local = screen->m_isOnScreen.load();
	const bool pointerEvent = type == kCGEventMouseMoved ||
		type == kCGEventLeftMouseDragged || type == kCGEventRightMouseDragged ||
		type == kCGEventOtherMouseDragged || type == kCGEventLeftMouseDown ||
		type == kCGEventLeftMouseUp || type == kCGEventRightMouseDown ||
		type == kCGEventRightMouseUp || type == kCGEventOtherMouseDown ||
		type == kCGEventOtherMouseUp || type == kCGEventScrollWheel;
	const bool keyboardEvent = type == kCGEventKeyDown ||
		type == kCGEventKeyUp || type == kCGEventFlagsChanged;
	const bool motionEvent = type == kCGEventMouseMoved || type == kCGEventLeftMouseDragged ||
		type == kCGEventRightMouseDragged || type == kCGEventOtherMouseDragged;
	CGEventTimestamp motionTicks = 0;
	double motionX = 0, motionY = 0;
	if (event && pointerEvent && type != kCGEventScrollWheel) {
		motionTicks = pointerEventTimestampTicks(event);
		const auto state = CGEventGetIntegerValueField(event, kCGEventSourceStateID);
		// Direct CG posting changes the next HID position baseline even when
		// it is our replay and the input-routing filter below bypasses it.
		if (state == kCGEventSourceStateCombinedSessionState || state == kCGEventSourceStatePrivate) {
			if (!screen->recordPointerPosition(CGEventGetLocation(event),
				motionTicks ? motionTicks : mach_absolute_time(), true)) {
				screen->showCursor();
				screen->m_isOnScreen = !screen->m_pointerDetached;
				screen->sendEvent(screen->m_events->forIScreen().error());
				return screen->m_isOnScreen ? event : nullptr;
			}
		}
		if (motionEvent) {
			motionX = CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
			motionY = CGEventGetIntegerValueField(event, kCGMouseEventDeltaY);
			const bool unsafeTiming = !local && motionTicks == 0 &&
				state == kCGEventSourceStateHIDSystemState && !screen->m_pointerPositionChanges.empty();
			screen->compensatePointerMotion(event, motionTicks, motionX, motionY);
			if (unsafeTiming) {
				// The raw delta may contain a whole-screen warp. Restore native
				// input instead of forwarding an unorderable delta to the peer.
				screen->showCursor();
				screen->m_isOnScreen = !screen->m_pointerDetached;
				screen->sendEvent(screen->m_events->forIScreen().error());
				return screen->m_isOnScreen ? event : nullptr;
			}
		}
	}
	if (event != nullptr &&
		((pointerEvent && CGEventGetIntegerValueField(event, kCGEventSourceUserData) == kWeavePointerEventMarker) ||
		 ((pointerEvent || keyboardEvent) && CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) == getpid()))) {
		// Our keyboard replay must not rewrite physical key/modifier pairing
		// or retrigger hotkeys. Keep OS delivery even after re-leaving: an own
		// key-up may be the required cleanup of a locally replayed press.
		return event;
	}

	switch(type) {
		case kCGEventLeftMouseDown:
		case kCGEventRightMouseDown:
		case kCGEventOtherMouseDown:
			screen->onModifierFlags(CGEventGetFlags(event), false);
			screen->onMouseButton(true, CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber) + 1);
			break;
		case kCGEventLeftMouseUp:
		case kCGEventRightMouseUp:
		case kCGEventOtherMouseUp: {
			const auto macButton = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber) + 1;
			if (local) {
				auto held = screen->m_handoffButtonGenerations.find(screen->mapMacButtonToBarrier(macButton));
				if (held != screen->m_handoffButtonGenerations.end()) {
					screen->m_handoffButtonGenerations.erase(held);
					// onMouseButton's legacy drag path only updates the left
					// button. The completed native release also retires any
					// replayed right/other button's cached down state.
					if (macButton > 0 && macButton <= NumButtonIDs)
						screen->m_buttonState.set(macButton - 1, kMouseButtonUp);
				}
			}
			screen->onModifierFlags(CGEventGetFlags(event), false);
			screen->onMouseButton(false, macButton);
			break;
		}
		case kCGEventLeftMouseDragged:
		case kCGEventRightMouseDragged:
		case kCGEventOtherMouseDragged:
		case kCGEventMouseMoved:
			if (event == nullptr) return nullptr;
			if (local && motionTicks != 0 && screen->m_localMotionAfterTicks != 0 &&
				motionTicks <= screen->m_localMotionAfterTicks) {
				// Discard queued native motion from the previous remote lease.
				// It must not put the local pointer back on the crossing edge.
				return nullptr;
			}
			if (!local) {
				// Association(false) alone is not durable for background apps.
				// Compensate the captured delta before parking: this new warp
				// belongs to a later native event, not to the current one.
				if (!CGPointEqualToPoint(CGEventGetLocation(event), screen->m_parkPoint) &&
					screen->warpPointer(screen->m_parkPoint, true) != kCGErrorSuccess) {
					screen->showCursor();
					screen->m_isOnScreen = !screen->m_pointerDetached;
					screen->sendEvent(screen->m_events->forIScreen().error());
					return nullptr;
				}
				const double x = motionX, y = motionY;
				if (x > std::numeric_limits<SInt32>::max() || x < std::numeric_limits<SInt32>::min() ||
					y > std::numeric_limits<SInt32>::max() || y < std::numeric_limits<SInt32>::min()) {
					return nullptr;
				}
				const SInt32 ix = static_cast<SInt32>(x), iy = static_cast<SInt32>(y);
				if (ix != 0 || iy != 0) {
					screen->sendEvent(screen->m_events->forIPrimaryScreen().motionOnSecondary(),
						MotionInfo::alloc(ix, iy));
				}
				// Suppress native movement while the remote screen owns input.
				return nullptr;
			}
			pos = CGEventGetLocation(event);
			screen->onMouseMove(pos.x, pos.y);
			return event;
			break;
		case kCGEventScrollWheel:
			screen->onMouseWheel(screen->mapScrollWheelToBarrier(
								 CGEventGetIntegerValueField(event, kCGScrollWheelEventFixedPtDeltaAxis2) / 65536.0f),
								 screen->mapScrollWheelToBarrier(
								 CGEventGetIntegerValueField(event, kCGScrollWheelEventFixedPtDeltaAxis1) / 65536.0f));
			break;
		case kCGEventKeyDown:
		case kCGEventKeyUp:
		case kCGEventFlagsChanged: {
			const auto code = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
			bool released = type == kCGEventKeyUp;
			if (type == kCGEventFlagsChanged) {
				CGEventFlags modifier = 0;
				switch (code) {
				case kVK_Shift: case kVK_RightShift: modifier = kCGEventFlagMaskShift; break;
				case kVK_Control: case kVK_RightControl: modifier = kCGEventFlagMaskControl; break;
				case kVK_Option: case kVK_RightOption: modifier = kCGEventFlagMaskAlternate; break;
				case kVK_Command: case kVK_RightCommand: modifier = kCGEventFlagMaskCommand; break;
				default: break;
				}
				released = modifier != 0 && (CGEventGetFlags(event) & modifier) == 0;
			}
			const KeyButton button = code >= 0 && code + 1 < IKeyState::kNumButtons ? code + 1 : 0;
			const bool completed = local && released && button != 0 &&
				screen->completeLocalKeyRelease(button);
			screen->onKey(event);
			if (completed)
				screen->m_keyState->onKey(button, false, screen->m_primaryModifiers.load());
			break;
		}
		case kCGEventTapDisabledByTimeout:
			// Re-enable our event-tap
			CGEventTapEnable(screen->m_eventTapPort, true);
			LOG((CLOG_INFO "quartz event tap was disabled by timeout, re-enabling"));
			break;
		case kCGEventTapDisabledByUserInput:
			LOG((CLOG_ERR "quartz event tap was disabled by user input"));
			// A permanently disabled primary tap cannot capture remote input or
			// the emergency-return shortcut. Restore local control now, then let
			// the business loop end the peer lease and retry any failed restore.
			screen->showCursor();
			screen->m_isOnScreen = !screen->m_pointerDetached;
			screen->sendEvent(screen->m_events->forIScreen().error());
			break;
		case NX_NULLEVENT:
			break;
		default:
			if (type == NX_SYSDEFINED) {
			if (isMediaKeyEvent (event)) {
				LOG((CLOG_DEBUG2 "detected media key event"));
				screen->onMediaKey (event);
			} else {
				LOG((CLOG_DEBUG2 "ignoring unknown system defined event"));
				return event;
			}
			break;
			}

			LOG((CLOG_DEBUG3 "unknown quartz event type: 0x%02x", type));
	}

	if (screen->m_isOnScreen) {
		return event;
	} else {
		return NULL;
	}
	}
}

void
OSXScreen::MouseButtonState::set(UInt32 button, EMouseButtonState state)
{
	bool newState = (state == kMouseButtonDown);
	m_buttons.set(button, newState);
}

bool
OSXScreen::MouseButtonState::any()
{
	return m_buttons.any();
}

void
OSXScreen::MouseButtonState::reset()
{
	m_buttons.reset();
}

void
OSXScreen::MouseButtonState::overwrite(UInt32 buttons)
{
	m_buttons = std::bitset<NumButtonIDs>(buttons);
}

bool
OSXScreen::MouseButtonState::test(UInt32 button) const
{
	return m_buttons.test(button);
}

SInt8
OSXScreen::MouseButtonState::getFirstButtonDown() const
{
	if (m_buttons.any()) {
		for (unsigned short button = 0; button < m_buttons.size(); button++) {
			if (m_buttons.test(button)) {
				return button;
			}
		}
	}
	return -1;
}

char*
OSXScreen::CFStringRefToUTF8String(CFStringRef aString)
{
	if (aString == NULL) {
		return NULL;
	}

	CFIndex length = CFStringGetLength(aString);
	CFIndex maxSize = CFStringGetMaximumSizeForEncoding(
		length,
		kCFStringEncodingUTF8);
	char* buffer = (char*)malloc(maxSize);
	if (CFStringGetCString(aString, buffer, maxSize, kCFStringEncodingUTF8)) {
		return buffer;
	}
	return NULL;
}

void
OSXScreen::fakeDraggingFiles(DragFileList fileList)
{
	m_fakeDraggingStarted = true;
	String fileExt;
	if (fileList.size() == 1) {
		fileExt = DragInformation::getDragFileExtension(
			fileList.at(0).getFilename());
	}

#if defined(MAC_OS_X_VERSION_10_7)
	fakeDragging(fileExt.c_str(), m_xCursor, m_yCursor);
#else
	LOG((CLOG_WARN "drag drop not supported"));
#endif
}

String&
OSXScreen::getDraggingFilename()
{
	if (m_draggingStarted) {
		CFStringRef dragInfo = getDraggedFileURL();
		char* info = NULL;
		info = CFStringRefToUTF8String(dragInfo);
		if (info == NULL) {
			m_draggingFilename.clear();
		}
		else {
			LOG((CLOG_DEBUG "drag info: %s", info));
			CFRelease(dragInfo);
			String fileList(info);
			m_draggingFilename = fileList;
		}

		// fake a escape key down and up then left mouse button up
		fakeKeyDown(kKeyEscape, 8192, 1);
		fakeKeyUp(1);
		fakeMouseButton(kButtonLeft, false);
	}
	return m_draggingFilename;
}

void
OSXScreen::waitForCarbonLoop() const
{
#if defined(MAC_OS_X_VERSION_10_7)
	if (*m_carbonLoopReady) {
		LOG((CLOG_DEBUG "carbon loop already ready"));
		return;
	}

	Lock lock(m_carbonLoopMutex);

	LOG((CLOG_DEBUG "waiting for carbon loop"));

	double timeout = ARCH->time() + kCarbonLoopWaitTimeout;
	while (!m_carbonLoopReady->wait()) {
		if (ARCH->time() > timeout) {
			LOG((CLOG_DEBUG "carbon loop not ready, waiting again"));
			timeout = ARCH->time() + kCarbonLoopWaitTimeout;
		}
	}

	LOG((CLOG_DEBUG "carbon loop ready"));
#endif

}

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

void
setZeroSuppressionInterval()
{
	CGSetLocalEventsSuppressionInterval(0.0);
}

void
avoidSupression()
{
	// avoid suppression of local hardware events
	// stkamp@users.sourceforge.net
	CGSetLocalEventsFilterDuringSupressionState(
							kCGEventFilterMaskPermitAllEvents,
							kCGEventSupressionStateSupressionInterval);
	CGSetLocalEventsFilterDuringSupressionState(
							(kCGEventFilterMaskPermitLocalKeyboardEvents |
							kCGEventFilterMaskPermitSystemDefinedEvents),
							kCGEventSupressionStateRemoteMouseDrag);
}

void
logCursorVisibility()
{
	// CGCursorIsVisible is probably deprecated because its unreliable.
	if (!CGCursorIsVisible()) {
		LOG((CLOG_WARN "cursor may not be visible"));
	}
}

void
avoidHesitatingCursor()
{
	// This used to be necessary to get smooth mouse motion on other screens,
	// but now is just to avoid a hesitating cursor when transitioning to
	// the primary (this) screen.
	CGSetLocalEventsSuppressionInterval(0.0001);
}

#pragma GCC diagnostic error "-Wdeprecated-declarations"
