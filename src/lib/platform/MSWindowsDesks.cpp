/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2018 Debauchee Open Source Group
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

#include "platform/MSWindowsDesks.h"

#include "platform/MSWindowsScreen.h"
#include "barrier/IScreenSaver.h"
#include "barrier/XScreen.h"
#include "mt/Lock.h"
#include "mt/Thread.h"
#include "mt/ThreadShutdown.h"
#include "arch/win32/ArchMiscWindows.h"
#include "base/Log.h"
#include "base/IEventQueue.h"
#include "base/Stopwatch.h"
#include "base/TMethodEventJob.h"

#include <malloc.h>
#include <limits>
#include <vector>
#include <VersionHelpers.h>

// these are only defined when WINVER >= 0x0500
#if !defined(SPI_GETMOUSESPEED)
#define SPI_GETMOUSESPEED 112
#endif
#if !defined(SPI_SETMOUSESPEED)
#define SPI_SETMOUSESPEED 113
#endif
#if !defined(SPI_GETSCREENSAVERRUNNING)
#define SPI_GETSCREENSAVERRUNNING 114
#endif

#if !defined(MOUSEEVENTF_HWHEEL)
#define MOUSEEVENTF_HWHEEL 0x1000
#endif
#if !defined(MOUSEEVENTF_MOVE_NOCOALESCE)
#define MOUSEEVENTF_MOVE_NOCOALESCE 0x2000
#endif

// X button stuff
#if !defined(WM_XBUTTONDOWN)
#define WM_XBUTTONDOWN        0x020B
#define WM_XBUTTONUP        0x020C
#define WM_XBUTTONDBLCLK    0x020D
#define WM_NCXBUTTONDOWN    0x00AB
#define WM_NCXBUTTONUP        0x00AC
#define WM_NCXBUTTONDBLCLK    0x00AD
#define MOUSEEVENTF_XDOWN    0x0080
#define MOUSEEVENTF_XUP        0x0100
#define XBUTTON1            0x0001
#define XBUTTON2            0x0002
#endif
#if !defined(VK_XBUTTON1)
#define VK_XBUTTON1            0x05
#define VK_XBUTTON2            0x06
#endif

namespace {

const double kNormalDeskPollInterval = 0.2;
const double kLowLatencyDeskPollInterval = 0.05;
const double kDeskCommandTimeout = 0.25;
const double kDeskCommandExecutionGrace = 0.75;
const double kLowLatencyDeskCommandTimeout = 0.05;
const double kLowLatencyDeskCommandExecutionGrace = 0.20;
const ULONGLONG kDeskRecoveryProbeInterval = 1000;
const ULONGLONG kDeskStartupDeadline = 2000;
const std::uint64_t kMaxPendingDeskCommands = 128;
const int kDeskStopPostRetries = 3;
const DWORD kInputRecoveryHardExitDelay = 5000;

DWORD WINAPI terminateInputProcessAfterRecoveryDeadline(LPVOID)
{
    Sleep(kInputRecoveryHardExitDelay);
    TerminateProcess(GetCurrentProcess(), ERROR_PROCESS_ABORTED);
    return 0;
}

}

// <unused>; <unused>
#define BARRIER_MSG_SWITCH            BARRIER_HOOK_LAST_MSG + 1
// <unused>; <unused>
#define BARRIER_MSG_ENTER            BARRIER_HOOK_LAST_MSG + 2
// <unused>; <unused>
#define BARRIER_MSG_LEAVE            BARRIER_HOOK_LAST_MSG + 3
// wParam = flags, HIBYTE(lParam) = virtual key, LOBYTE(lParam) = scan code
#define BARRIER_MSG_FAKE_KEY        BARRIER_HOOK_LAST_MSG + 4
 // flags, XBUTTON id
#define BARRIER_MSG_FAKE_BUTTON        BARRIER_HOOK_LAST_MSG + 5
// x; y
#define BARRIER_MSG_FAKE_MOVE        BARRIER_HOOK_LAST_MSG + 6
// xDelta; yDelta
#define BARRIER_MSG_FAKE_WHEEL        BARRIER_HOOK_LAST_MSG + 7
// <unused>; <unused>
#define BARRIER_MSG_CURSOR_POS        BARRIER_HOOK_LAST_MSG + 8
// IKeyState*; <unused>
#define BARRIER_MSG_SYNC_KEYS        BARRIER_HOOK_LAST_MSG + 9
// install; <unused>
#define BARRIER_MSG_SCREENSAVER        BARRIER_HOOK_LAST_MSG + 10
// dx; dy
#define BARRIER_MSG_FAKE_REL_MOVE    BARRIER_HOOK_LAST_MSG + 11
// enable; <unused>
#define BARRIER_MSG_FAKE_INPUT        BARRIER_HOOK_LAST_MSG + 12
// DeskCommand*; <unused>
#define BARRIER_MSG_DESK_COMMAND      BARRIER_HOOK_LAST_MSG + 13
// <unused>; <unused>
#define BARRIER_MSG_DESK_STOP         BARRIER_HOOK_LAST_MSG + 14

namespace {

LONG normalizeMouseCoordinate(SInt32 value, SInt32 origin, SInt32 length)
{
    if (length <= 1) {
        return 0;
    }

    const double scaled =
        (static_cast<double>(value - origin) * 65535.0) /
        static_cast<double>(length - 1);
    if (scaled <= 0.0) {
        return 0;
    }
    if (scaled >= 65535.0) {
        return 65535;
    }
    return static_cast<LONG>(scaled + 0.5);
}

bool sendKeyboardInput(UINT virtualKey, UINT scanCode, DWORD flags)
{
    INPUT input;
    ZeroMemory(&input, sizeof(input));
    input.type = INPUT_KEYBOARD;

    const UINT resolvedScan =
        (scanCode != 0) ? scanCode : MapVirtualKey(virtualKey, MAPVK_VK_TO_VSC);
    if (resolvedScan != 0) {
        input.ki.wVk = 0;
        input.ki.wScan = static_cast<WORD>(resolvedScan);
        input.ki.dwFlags = flags | KEYEVENTF_SCANCODE;
    }
    else {
        input.ki.wVk = static_cast<WORD>(virtualKey);
        input.ki.wScan = 0;
        input.ki.dwFlags = flags;
    }

    return SendInput(1, &input, sizeof(input)) == 1;
}

bool sendMouseInput(LONG dx, LONG dy, DWORD flags, DWORD mouseData)
{
    INPUT input;
    ZeroMemory(&input, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = flags;
    input.mi.mouseData = mouseData;
    return SendInput(1, &input, sizeof(input)) == 1;
}

bool isOrderedInputCommand(UINT message)
{
    return message == BARRIER_MSG_FAKE_KEY ||
        message == BARRIER_MSG_FAKE_BUTTON ||
        message == BARRIER_MSG_FAKE_WHEEL;
}

bool isMouseMotionCommand(UINT message)
{
    return message == BARRIER_MSG_FAKE_MOVE ||
        message == BARRIER_MSG_FAKE_REL_MOVE;
}

bool isOrdinaryInputCommand(UINT message)
{
    return isMouseMotionCommand(message) || isOrderedInputCommand(message);
}

}

//
// MSWindowsDesks
//

MSWindowsDesks::DeskCommand::DeskCommand(
    UINT commandMessage, WPARAM commandWParam,
    LPARAM commandLParam, std::uint64_t commandSequence) :
    message(commandMessage),
    wParam(commandWParam),
    lParam(commandLParam),
    sequence(commandSequence)
{
}

MSWindowsDesks::MSWindowsDesks(bool isPrimary, bool noHooks,
        const IScreenSaver* screensaver, IEventQueue* events,
        const std::function<void()>& updateKeys, bool stopOnDeskSwitch) :
    m_isPrimary(isPrimary),
    m_noHooks(noHooks),
    m_isOnScreen(m_isPrimary),
    m_x(0), m_y(0),
    m_w(0), m_h(0),
    m_xCenter(0), m_yCenter(0),
    m_multimon(false),
    m_timer(NULL),
    m_threadID(0),
    m_screensaver(screensaver),
    m_screensaverNotify(false),
    m_activeDesk(NULL),
    m_activeDeskName(),
    m_observedDeskName(),
    m_mutex(),
    m_sendMutex(),
    m_deskReady(&m_mutex, false),
    m_inputDesktopGeneration(0),
    m_nextDeskCommandSequence(0),
    m_cursorPos{0, 0},
    m_inputRecoveryRequested(false),
    m_nextDeskRecoveryProbe(0),
    m_updateKeys(updateKeys),
    m_leaveForegroundOption(false),
    m_lowLatencyMode(false),
    m_nestedRemoteMode(false),
    m_relativeMoveAccelerationDisabled(false),
    m_oldMouseAcceleration{0, 0, 0, 0},
    m_deskPollInterval(0.2),
    m_events(events),
    m_stopOnDeskSwitch(stopOnDeskSwitch)
{
    m_cursor    = createBlankCursor();
    m_deskClass = createDeskWindowClass(m_isPrimary);
    m_keyLayout = GetKeyboardLayout(GetCurrentThreadId());
    resetOptions();
}

MSWindowsDesks::~MSWindowsDesks()
{
    disable();
    destroyClass(m_deskClass);
    destroyCursor(m_cursor);
}

void
MSWindowsDesks::enable()
{
    if (m_threadID != 0) {
        return;
    }

    m_threadID = GetCurrentThreadId();

    // set the active desk and (re)install the hooks
    checkDesk();

    // install the desk timer.  this timer periodically checks
    // which desk is active and reinstalls the hooks as necessary.
    // we wouldn't need this if windows notified us of a desktop
    // change but as far as i can tell it doesn't.
    updateDeskTimer(m_deskPollInterval);

    updateKeys();
}

void
MSWindowsDesks::disable()
{
    // remove timer
    if (m_timer != NULL) {
        m_events->removeHandler(Event::kTimer, m_timer);
        m_events->deleteTimer(m_timer);
        m_timer = NULL;
    }
    m_threadID = 0;
    endLowLatencyRelativeMoves();

    // destroy desks
    removeDesks();

    m_isOnScreen = m_isPrimary;
}

bool
MSWindowsDesks::enter()
{
    return sendMessage(BARRIER_MSG_ENTER, 0, 0);
}

bool
MSWindowsDesks::leave(HKL keyLayout)
{
    if (sendMessage(BARRIER_MSG_LEAVE, (WPARAM)keyLayout, 0)) {
        return true;
    }

    requestInputRecovery(
        BARRIER_MSG_LEAVE, "desktop leave command failed", true);
    return false;
}

void
MSWindowsDesks::resetOptions()
{
    m_leaveForegroundOption = false;
    m_lowLatencyMode = false;
    m_nestedRemoteMode = false;
    endLowLatencyRelativeMoves();
    updateDeskTimer(kNormalDeskPollInterval);
}

void
MSWindowsDesks::setOptions(const OptionsList& options)
{
    bool lowLatencyMode = false;
    bool nestedRemoteMode = false;
    for (UInt32 i = 0, n = (UInt32)options.size(); i < n; i += 2) {
        if (options[i] == kOptionWin32KeepForeground) {
            m_leaveForegroundOption = (options[i + 1] != 0);
            LOG((CLOG_DEBUG1 "%s the foreground window", m_leaveForegroundOption ? "don\'t grab" : "grab"));
        }
        else if (options[i] == kOptionLowLatencyMode) {
            lowLatencyMode = (options[i + 1] != 0);
        }
        else if (options[i] == kOptionNestedRemoteMode) {
            nestedRemoteMode = (options[i + 1] != 0);
        }
    }

    m_lowLatencyMode = lowLatencyMode;
    m_nestedRemoteMode = nestedRemoteMode;
    if (!m_lowLatencyMode && !m_nestedRemoteMode) {
        endLowLatencyRelativeMoves();
    }
    updateDeskTimer((m_lowLatencyMode || m_nestedRemoteMode) ?
        kLowLatencyDeskPollInterval : kNormalDeskPollInterval);
}

void
MSWindowsDesks::updateKeys()
{
    sendMessage(BARRIER_MSG_SYNC_KEYS, 0, 0);
}

void
MSWindowsDesks::updateDeskTimer(double interval)
{
    m_deskPollInterval = interval;

    if (m_timer != NULL) {
        m_events->removeHandler(Event::kTimer, m_timer);
        m_events->deleteTimer(m_timer);
        m_timer = NULL;
    }

    if (m_threadID != 0) {
        m_timer = m_events->newTimer(m_deskPollInterval, NULL);
        m_events->adoptHandler(Event::kTimer, m_timer,
                                new TMethodEventJob<MSWindowsDesks>(
                                    this, &MSWindowsDesks::handleCheckDesk));
    }
}

void
MSWindowsDesks::beginLowLatencyRelativeMoves()
{
    if (m_relativeMoveAccelerationDisabled ||
        GetSystemMetrics(SM_MOUSEPRESENT) == 0 ||
        (!m_lowLatencyMode && !m_nestedRemoteMode)) {
        return;
    }

    if (!SystemParametersInfo(SPI_GETMOUSE, 0, m_oldMouseAcceleration, 0) ||
        !SystemParametersInfo(SPI_GETMOUSESPEED, 0, m_oldMouseAcceleration + 3, 0)) {
        return;
    }

    int newSpeed[4] = { 0, 0, 0, 1 };
    if (SystemParametersInfo(SPI_SETMOUSE, 0, newSpeed, 0) &&
        SystemParametersInfo(SPI_SETMOUSESPEED, 0, newSpeed + 3, 0)) {
        m_relativeMoveAccelerationDisabled = true;
    }
}

void
MSWindowsDesks::endLowLatencyRelativeMoves()
{
    if (!m_relativeMoveAccelerationDisabled) {
        return;
    }

    SystemParametersInfo(SPI_SETMOUSE, 0, m_oldMouseAcceleration, 0);
    SystemParametersInfo(SPI_SETMOUSESPEED, 0, m_oldMouseAcceleration + 3, 0);
    m_relativeMoveAccelerationDisabled = false;
}

void
MSWindowsDesks::setShape(SInt32 x, SInt32 y,
                SInt32 width, SInt32 height,
                SInt32 xCenter, SInt32 yCenter, bool isMultimon)
{
    m_x        = x;
    m_y        = y;
    m_w        = width;
    m_h        = height;
    m_xCenter  = xCenter;
    m_yCenter  = yCenter;
    m_multimon = isMultimon;
}

void
MSWindowsDesks::installScreensaverHooks(bool install)
{
    if (m_isPrimary && m_screensaverNotify != install) {
        m_screensaverNotify = install;
        sendMessage(BARRIER_MSG_SCREENSAVER, install, 0);
    }
}

void
MSWindowsDesks::fakeInputBegin()
{
    sendMessage(BARRIER_MSG_FAKE_INPUT, 1, 0);
}

void
MSWindowsDesks::fakeInputEnd()
{
    sendMessage(BARRIER_MSG_FAKE_INPUT, 0, 0);
}

void
MSWindowsDesks::getCursorPos(SInt32& x, SInt32& y) const
{
    const bool updated = sendMessage(BARRIER_MSG_CURSOR_POS, 0, 0);
    Lock lock(&m_mutex);
    x = updated ? m_cursorPos.x : m_xCenter;
    y = updated ? m_cursorPos.y : m_yCenter;
}

void
MSWindowsDesks::fakeKeyEvent(
                KeyButton button, UINT virtualKey,
                bool press, bool /*isAutoRepeat*/) const
{
    // synthesize event
    DWORD flags = 0;
    if (((button & 0x100u) != 0)) {
        flags |= KEYEVENTF_EXTENDEDKEY;
    }
    if (!press) {
        flags |= KEYEVENTF_KEYUP;
    }
    const DeskCommandDispatch dispatch =
        deskCommandWaitsForCompletionForTest(kDeskInputKey) ?
            kWaitForDeskCommand : kPostDeskCommand;
    if (!sendMessage(BARRIER_MSG_FAKE_KEY, flags,
            MAKEWORD(static_cast<BYTE>(button & 0xffu),
                static_cast<BYTE>(virtualKey & 0xffu)), dispatch)) {
        requestInputRecovery(
            BARRIER_MSG_FAKE_KEY, "ordered key command failed", true);
    }
}

void
MSWindowsDesks::fakeMouseButton(ButtonID button, bool press)
{
    // the system will swap the meaning of left/right for us if
    // the user has configured a left-handed mouse but we don't
    // want it to swap since we want the handedness of the
    // server's mouse.  so pre-swap for a left-handed mouse.
    if (GetSystemMetrics(SM_SWAPBUTTON)) {
        switch (button) {
        case kButtonLeft:
            button = kButtonRight;
            break;

        case kButtonRight:
            button = kButtonLeft;
            break;
        }
    }

    // map button id to button flag and button data
    DWORD data = 0;
    DWORD flags;
    switch (button) {
    case kButtonLeft:
        flags = press ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;

    case kButtonMiddle:
        flags = press ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;

    case kButtonRight:
        flags = press ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;

    case kButtonExtra0:
        data = XBUTTON1;
        flags = press ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        break;

    case kButtonExtra1:
        data = XBUTTON2;
        flags = press ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        break;

    default:
        return;
    }

    // do it
    const DeskCommandDispatch dispatch =
        deskCommandWaitsForCompletionForTest(kDeskInputButton) ?
            kWaitForDeskCommand : kPostDeskCommand;
    if (!sendMessage(BARRIER_MSG_FAKE_BUTTON, flags, data, dispatch)) {
        requestInputRecovery(
            BARRIER_MSG_FAKE_BUTTON, "ordered button command failed", true);
    }
}

bool
MSWindowsDesks::fakeMouseMove(SInt32 x, SInt32 y,
                              bool waitForCompletion) const
{
    const DeskCommandDispatch dispatch =
        deskCommandWaitsForCompletionForTest(
            kDeskInputAbsoluteMove, waitForCompletion) ?
                kWaitForDeskCommand : kPostDeskCommand;
    return sendMessage(BARRIER_MSG_FAKE_MOVE,
                       static_cast<WPARAM>(x),
                       static_cast<LPARAM>(y),
                       dispatch);
}

void
MSWindowsDesks::fakeMouseRelativeMove(SInt32 dx, SInt32 dy) const
{
    const DeskCommandDispatch dispatch =
        deskCommandWaitsForCompletionForTest(kDeskInputRelativeMove) ?
            kWaitForDeskCommand : kPostDeskCommand;
    sendMessage(BARRIER_MSG_FAKE_REL_MOVE,
                            static_cast<WPARAM>(dx),
                            static_cast<LPARAM>(dy),
                            dispatch);
}

void
MSWindowsDesks::fakeMouseWheel(SInt32 xDelta, SInt32 yDelta) const
{
    const DeskCommandDispatch dispatch =
        deskCommandWaitsForCompletionForTest(kDeskInputWheel) ?
            kWaitForDeskCommand : kPostDeskCommand;
    if (!sendMessage(BARRIER_MSG_FAKE_WHEEL, xDelta, yDelta, dispatch)) {
        requestInputRecovery(
            BARRIER_MSG_FAKE_WHEEL, "ordered wheel command failed", true);
    }
}

bool
MSWindowsDesks::canEnter() const
{
    Lock lock(&m_mutex);
    return canAcceptInputForActiveDesktopForTest(
        isDeskReadyLocked(m_activeDesk),
        m_observedDeskName == m_activeDeskName);
}

bool
MSWindowsDesks::probeInputDesktop(std::string& desktopName) const
{
    desktopName.clear();
    HDESK desktop = openInputDesktopForProbe();
    if (desktop == NULL) {
        return false;
    }

    desktopName = getDesktopName(desktop);
    closeDesktop(desktop);
    return !desktopName.empty();
}

std::uint64_t
MSWindowsDesks::inputDesktopGeneration() const
{
    Lock lock(&m_mutex);
    return m_inputDesktopGeneration;
}

std::string
MSWindowsDesks::inputDesktopName() const
{
    Lock lock(&m_mutex);
    return m_activeDeskName;
}

bool
MSWindowsDesks::isDesktopReadyForTest(bool isPrimary, bool noHooks,
                                      bool threadAttached, bool windowReady,
                                      bool hookInstalled,
                                      bool commandResponsive)
{
    return threadAttached && windowReady && commandResponsive &&
        (!isPrimary || noHooks || hookInstalled);
}

bool
MSWindowsDesks::isDeskCommandCompleteForTest(
    std::uint64_t expectedSequence, std::uint64_t completedSequence,
    bool threadRunning)
{
    return threadRunning && completedSequence >= expectedSequence;
}

bool
MSWindowsDesks::shouldProcessDeskCommandForTest(
    std::uint64_t sequence, std::uint64_t cancelledThroughSequence)
{
    return sequence > cancelledThroughSequence;
}

bool
MSWindowsDesks::canCancelTimedOutDeskCommandForTest(
    std::uint64_t sequence, std::uint64_t executingSequence,
    bool orderedInputCommand)
{
    return !orderedInputCommand && sequence != executingSequence;
}

bool
MSWindowsDesks::commandCompletionProvesResponsiveForTest(
    bool commandExecuted, bool commandSucceeded, std::uint64_t sequence,
    std::uint64_t poisonedThroughSequence)
{
    return commandExecuted && commandSucceeded &&
        sequence > poisonedThroughSequence;
}

bool
MSWindowsDesks::commandInjectionSucceededForTest(
    std::uint64_t sequence, std::uint64_t failedSequence)
{
    return sequence != failedSequence;
}

bool
MSWindowsDesks::canQueueDeskCommandForTest(
    std::uint64_t nextSequence, std::uint64_t completedSequence,
    std::uint64_t maxPendingCommands)
{
    return maxPendingCommands > 0 && nextSequence >= completedSequence &&
        nextSequence - completedSequence < maxPendingCommands;
}

std::uint64_t
MSWindowsDesks::maxPendingDeskCommandsForTest()
{
    return kMaxPendingDeskCommands;
}

bool
MSWindowsDesks::deskCommandWaitsForCompletionForTest(
    DeskInputCommand command, bool forceCompletion)
{
    if (forceCompletion) {
        return true;
    }
    return command != kDeskInputAbsoluteMove &&
        command != kDeskInputRelativeMove;
}

double
MSWindowsDesks::deskCommandAckTimeoutForTest(
    bool lowLatencyMode, bool nestedRemoteMode)
{
    return (lowLatencyMode || nestedRemoteMode) ?
        kLowLatencyDeskCommandTimeout : kDeskCommandTimeout;
}

double
MSWindowsDesks::deskCommandExecutionGraceForTest(
    bool lowLatencyMode, bool nestedRemoteMode)
{
    return (lowLatencyMode || nestedRemoteMode) ?
        kLowLatencyDeskCommandExecutionGrace : kDeskCommandExecutionGrace;
}

double
MSWindowsDesks::boundedDeskCommandWaitTimeoutForTest(double timeout)
{
    return timeout >= 0.0 ? timeout : 0.0;
}

bool
MSWindowsDesks::canActivateDesktopForTest(
    bool startupComplete, bool threadRunning,
    bool threadAttached, bool windowReady)
{
    return startupComplete && threadRunning && threadAttached && windowReady;
}

bool
MSWindowsDesks::canAcceptInputForActiveDesktopForTest(
    bool activeDesktopReady, bool observedDesktopMatchesActive)
{
    return activeDesktopReady && observedDesktopMatchesActive;
}

bool
MSWindowsDesks::hasDesktopStartupTimedOutForTest(
    bool startupComplete, std::uint64_t now, std::uint64_t deadline)
{
    return !startupComplete && now >= deadline;
}

bool
MSWindowsDesks::shouldPostDeskQuitForTest(
    bool startupComplete, DWORD threadID)
{
    return startupComplete && threadID != 0;
}

bool
MSWindowsDesks::coalesceMouseMotionForTest(
    DeskInputCommand pendingCommand,
    SInt32& pendingFirst, SInt32& pendingSecond,
    DeskInputCommand nextCommand,
    SInt32 nextFirst, SInt32 nextSecond)
{
    if (pendingCommand != nextCommand) {
        return false;
    }

    if (nextCommand == kDeskInputAbsoluteMove) {
        // The server has already rate-limited absolute motion. Replacing the
        // pending desk command here creates a second latest-value stage and
        // turns short desktop-thread stalls into visibly large cursor jumps.
        // Keep each sample in the existing bounded desk command queue.
        return false;
    }

    if (nextCommand != kDeskInputRelativeMove) {
        return false;
    }

    const std::int64_t first =
        static_cast<std::int64_t>(pendingFirst) + nextFirst;
    const std::int64_t second =
        static_cast<std::int64_t>(pendingSecond) + nextSecond;
    const std::int64_t minimum =
        (std::numeric_limits<SInt32>::min)();
    const std::int64_t maximum =
        (std::numeric_limits<SInt32>::max)();
    pendingFirst = static_cast<SInt32>(
        first < minimum ? minimum : (first > maximum ? maximum : first));
    pendingSecond = static_cast<SInt32>(
        second < minimum ? minimum :
            (second > maximum ? maximum : second));
    return true;
}

MSWindowsDesks::PendingMotionBoundary
MSWindowsDesks::pendingMotionBoundaryForTest(
    DeskInputCommand command, bool forceCompletion)
{
    if (command == kDeskInputAbsoluteMove ||
        command == kDeskInputRelativeMove) {
        return forceCompletion ?
            kSupersedePendingMotion : kNoPendingMotionBoundary;
    }

    if (command == kDeskControlEnter ||
        command == kDeskControlLeave ||
        command == kDeskControlSwitch) {
        return kSupersedePendingMotion;
    }

    return kFlushPendingMotion;
}

MSWindowsDesks::DesktopTransitionAction
MSWindowsDesks::desktopTransitionActionForTest(
    bool observedDesktopMatchesActive,
    bool observedDesktopReady,
    bool inputLeaseActive,
    bool startupComplete,
    std::uint64_t now,
    std::uint64_t deadline)
{
    if (observedDesktopMatchesActive) {
        return kKeepActiveDesktop;
    }
    if (observedDesktopReady) {
        return kActivateObservedDesktop;
    }
    if (inputLeaseActive || startupComplete || now >= deadline) {
        return kRecoverInputProcess;
    }
    return kWaitForObservedDesktop;
}

bool
MSWindowsDesks::beginInputRecoveryForTest(bool& recoveryRequested)
{
    if (recoveryRequested) {
        return false;
    }
    recoveryRequested = true;
    return true;
}

DWORD
MSWindowsDesks::inputRecoveryHardExitDelayForTest()
{
    return kInputRecoveryHardExitDelay;
}

MSWindowsDesks::DeskInputCommand
MSWindowsDesks::classifyDeskCommand(UINT msg)
{
    switch (msg) {
    case BARRIER_MSG_FAKE_KEY:
        return kDeskInputKey;
    case BARRIER_MSG_FAKE_BUTTON:
        return kDeskInputButton;
    case BARRIER_MSG_FAKE_MOVE:
        return kDeskInputAbsoluteMove;
    case BARRIER_MSG_FAKE_REL_MOVE:
        return kDeskInputRelativeMove;
    case BARRIER_MSG_FAKE_WHEEL:
        return kDeskInputWheel;
    case BARRIER_MSG_ENTER:
        return kDeskControlEnter;
    case BARRIER_MSG_LEAVE:
        return kDeskControlLeave;
    case BARRIER_MSG_SWITCH:
        return kDeskControlSwitch;
    default:
        return kDeskControlOther;
    }
}

bool
MSWindowsDesks::sendMessage(UINT msg, WPARAM wParam, LPARAM lParam,
                            DeskCommandDispatch dispatch) const
{
    Lock sendLock(&m_sendMutex);

    const DeskInputCommand commandType = classifyDeskCommand(msg);
    const bool motionCommand = isMouseMotionCommand(msg);
    const bool asyncMotionCommand =
        motionCommand && dispatch == kPostDeskCommand;
    const PendingMotionBoundary motionBoundary =
        pendingMotionBoundaryForTest(
            commandType, motionCommand && dispatch == kWaitForDeskCommand);

    Desk* desk = NULL;
    std::uint64_t sequence = 0;
    std::uint64_t motionBoundarySequence = 0;
    bool rejected = false;
    bool requestRecovery = false;
    const char* recoveryReason = NULL;
    {
        Lock lock(&m_mutex);
        desk = m_activeDesk;
        const bool observedDesktopMatches =
            m_observedDeskName == m_activeDeskName;
        if (isOrdinaryInputCommand(msg) &&
            !observedDesktopMatches) {
            LOG((CLOG_WARN
                "Windows input command rejected because observed desktop changed message=%u active=%s observed=%s lease=%d",
                static_cast<unsigned int>(msg),
                m_activeDeskName.empty() ?
                    "<none>" : m_activeDeskName.c_str(),
                m_observedDeskName.empty() ?
                    "<none>" : m_observedDeskName.c_str(),
                m_isOnScreen ? 1 : 0));
            requestRecovery = m_isOnScreen;
            recoveryReason = "observed desktop changed during input lease";
            rejected = true;
        }
        else if (desk == NULL || !desk->m_threadRunning ||
            !desk->m_threadAttached || !desk->m_windowReady ||
            (!desk->m_commandResponsive && msg != BARRIER_MSG_SWITCH)) {
            if (msg == BARRIER_MSG_ENTER) {
                LOG((CLOG_WARN
                    "Windows input enter rejected before dispatch desktop=%s running=%d attached=%d windowReady=%d responsive=%d",
                    desk == NULL ? "<none>" : desk->m_name.c_str(),
                    desk != NULL && desk->m_threadRunning ? 1 : 0,
                    desk != NULL && desk->m_threadAttached ? 1 : 0,
                    desk != NULL && desk->m_windowReady ? 1 : 0,
                    desk != NULL && desk->m_commandResponsive ? 1 : 0));
            }
            else if (dispatch == kPostDeskCommand) {
                if (isOrderedInputCommand(msg)) {
                    LOG((CLOG_WARN
                        "Windows ordered input command rejected before dispatch message=%u desktop=%s running=%d attached=%d windowReady=%d responsive=%d",
                        static_cast<unsigned int>(msg),
                        desk == NULL ? "<none>" : desk->m_name.c_str(),
                        desk != NULL && desk->m_threadRunning ? 1 : 0,
                        desk != NULL && desk->m_threadAttached ? 1 : 0,
                        desk != NULL && desk->m_windowReady ? 1 : 0,
                        desk != NULL && desk->m_commandResponsive ? 1 : 0));
                }
                else {
                    LOG((CLOG_WARN
                        "Windows async input command rejected before dispatch message=%u desktop=%s running=%d attached=%d windowReady=%d responsive=%d",
                        static_cast<unsigned int>(msg),
                        desk == NULL ? "<none>" : desk->m_name.c_str(),
                        desk != NULL && desk->m_threadRunning ? 1 : 0,
                        desk != NULL && desk->m_threadAttached ? 1 : 0,
                        desk != NULL && desk->m_windowReady ? 1 : 0,
                        desk != NULL && desk->m_commandResponsive ? 1 : 0));
                }
                requestRecovery = true;
                recoveryReason = "pre-dispatch rejection";
            }
            rejected = true;
        }
        else if (asyncMotionCommand &&
            desk->m_pendingMotionCommand != NULL) {
            DeskCommand* pending = desk->m_pendingMotionCommand;
            SInt32 pendingFirst = static_cast<SInt32>(pending->wParam);
            SInt32 pendingSecond = static_cast<SInt32>(pending->lParam);
            if (coalesceMouseMotionForTest(
                    classifyDeskCommand(pending->message),
                    pendingFirst, pendingSecond,
                    commandType,
                    static_cast<SInt32>(wParam),
                    static_cast<SInt32>(lParam))) {
                pending->wParam = static_cast<WPARAM>(pendingFirst);
                pending->lParam = static_cast<LPARAM>(pendingSecond);
                return true;
            }

            // Absolute samples and changes between absolute/relative mode
            // remain queued with immutable data. Relative samples may still
            // accumulate so their total movement is never lost.
            desk->m_pendingMotionCommand = NULL;
        }

        if (!rejected && motionBoundary != kNoPendingMotionBoundary &&
            desk->m_lastMotionCommandSequence >
                desk->m_completedCommandSequence) {
            motionBoundarySequence = desk->m_lastMotionCommandSequence;
            if (motionBoundary == kSupersedePendingMotion &&
                motionBoundarySequence >
                    desk->m_cancelledCommandSequence) {
                desk->m_cancelledCommandSequence = motionBoundarySequence;
            }
            desk->m_pendingMotionCommand = NULL;
        }

        if (!rejected && asyncMotionCommand &&
            !canQueueDeskCommandForTest(
                m_nextDeskCommandSequence,
                desk->m_completedCommandSequence,
                kMaxPendingDeskCommands)) {
            const std::uint64_t pending =
                m_nextDeskCommandSequence >= desk->m_completedCommandSequence ?
                    m_nextDeskCommandSequence -
                        desk->m_completedCommandSequence :
                    kMaxPendingDeskCommands;
            LOG((CLOG_WARN
                "Windows input command queue is full; rejecting async command message=%u pending=%llu desktop=%s",
                static_cast<unsigned int>(msg),
                static_cast<unsigned long long>(pending),
                desk->m_name.c_str()));
            requestRecovery = true;
            recoveryReason = "queue full";
            rejected = true;
        }
        else if (!rejected) {
            sequence = ++m_nextDeskCommandSequence;
        }
    }

    if (rejected) {
        if (requestRecovery) {
            requestInputRecovery(
                msg, recoveryReason, isOrderedInputCommand(msg));
        }
        return false;
    }

    if (motionBoundarySequence != 0) {
        const double boundaryTimeout =
            deskCommandAckTimeoutForTest(
                m_lowLatencyMode, m_nestedRemoteMode) +
            deskCommandExecutionGraceForTest(
                m_lowLatencyMode, m_nestedRemoteMode);
        const DeskCommandWaitResult boundaryResult = waitForDeskCommand(
            desk, motionBoundarySequence, boundaryTimeout);
        if (boundaryResult == kDeskCommandFailed) {
            LOG((CLOG_ERR
                "Windows mouse motion injection failed before control boundary message=%u sequence=%llu desktop=%s",
                static_cast<unsigned int>(msg),
                static_cast<unsigned long long>(motionBoundarySequence),
                desk->m_name.c_str()));
            return false;
        }
        if (boundaryResult == kDeskCommandTimedOut) {
            {
                Lock lock(&m_mutex);
                if (desk == m_activeDesk && desk->m_commandResponsive) {
                    ++m_inputDesktopGeneration;
                }
                desk->m_commandResponsive = false;
                if (motionBoundarySequence >
                    desk->m_poisonedThroughCommandSequence) {
                    desk->m_poisonedThroughCommandSequence =
                        motionBoundarySequence;
                }
            }
            LOG((CLOG_ERR
                "Windows mouse motion boundary failed to drain within %.3fs before message=%u sequence=%llu desktop=%s",
                boundaryTimeout,
                static_cast<unsigned int>(msg),
                static_cast<unsigned long long>(motionBoundarySequence),
                desk->m_name.c_str()));
            requestInputRecovery(
                msg, "mouse motion boundary timeout",
                isOrderedInputCommand(msg));
            return false;
        }
    }

    DeskCommand* command = new DeskCommand(msg, wParam, lParam, sequence);
    if (motionCommand) {
        Lock lock(&m_mutex);
        desk->m_lastMotionCommandSequence = sequence;
        if (asyncMotionCommand) {
            desk->m_pendingMotionCommand = command;
        }
    }
    if (PostThreadMessage(desk->m_threadID, BARRIER_MSG_DESK_COMMAND,
                          reinterpret_cast<WPARAM>(command), 0) == 0) {
        const DWORD error = GetLastError();
        {
            Lock lock(&m_mutex);
            if (desk->m_pendingMotionCommand == command) {
                desk->m_pendingMotionCommand = NULL;
            }
            if (desk->m_lastMotionCommandSequence == sequence) {
                desk->m_lastMotionCommandSequence =
                    desk->m_completedCommandSequence;
            }
            if (desk == m_activeDesk && desk->m_commandResponsive) {
                ++m_inputDesktopGeneration;
            }
            desk->m_commandResponsive = false;
            if (sequence > desk->m_cancelledCommandSequence) {
                desk->m_cancelledCommandSequence = sequence;
            }
        }
        delete command;
        LOG((CLOG_WARN
            "cannot post Windows input command message=%u sequence=%llu error=%lu",
            static_cast<unsigned int>(msg),
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long>(error)));
        if (dispatch == kPostDeskCommand) {
            requestInputRecovery(
                msg, "PostThreadMessage failure", isOrderedInputCommand(msg));
        }
        return false;
    }

    if (dispatch == kPostDeskCommand) {
        return true;
    }

    const double commandTimeout =
        deskCommandAckTimeoutForTest(m_lowLatencyMode, m_nestedRemoteMode);
    const double commandExecutionGrace =
        deskCommandExecutionGraceForTest(
            m_lowLatencyMode, m_nestedRemoteMode);

    const DeskCommandWaitResult initialResult =
        waitForDeskCommand(desk, sequence, commandTimeout);
    if (initialResult == kDeskCommandFailed) {
        return false;
    }
    if (initialResult == kDeskCommandTimedOut) {
        bool commandRequiresGrace = false;
        {
            Lock lock(&m_mutex);
            if (isDeskCommandCompleteForTest(
                    sequence, desk->m_completedCommandSequence,
                    desk->m_threadRunning)) {
                return desk->m_failedCommandSequence != sequence;
            }
            commandRequiresGrace = !canCancelTimedOutDeskCommandForTest(
                sequence, desk->m_executingCommandSequence,
                isOrderedInputCommand(msg));
            if (!commandRequiresGrace) {
                if (desk == m_activeDesk && desk->m_commandResponsive) {
                    ++m_inputDesktopGeneration;
                }
                desk->m_commandResponsive = false;
                if (sequence > desk->m_cancelledCommandSequence) {
                    desk->m_cancelledCommandSequence = sequence;
                }
            }
        }

        if (commandRequiresGrace) {
            LOG((CLOG_WARN
                "Windows input command exceeded %.3fs; allowing %.3fs completion grace message=%u sequence=%llu desktop=%s",
                commandTimeout, commandExecutionGrace,
                static_cast<unsigned int>(msg),
                static_cast<unsigned long long>(sequence),
                desk->m_name.c_str()));
            const DeskCommandWaitResult graceResult = waitForDeskCommand(
                desk, sequence, commandExecutionGrace);
            if (graceResult == kDeskCommandSucceeded) {
                return true;
            }
            if (graceResult == kDeskCommandFailed) {
                return false;
            }

            {
                Lock lock(&m_mutex);
                if (isDeskCommandCompleteForTest(
                        sequence, desk->m_completedCommandSequence,
                        desk->m_threadRunning)) {
                    return desk->m_failedCommandSequence != sequence;
                }
                if (desk == m_activeDesk && desk->m_commandResponsive) {
                    ++m_inputDesktopGeneration;
                }
                desk->m_commandResponsive = false;
                if (sequence > desk->m_poisonedThroughCommandSequence) {
                    desk->m_poisonedThroughCommandSequence = sequence;
                }
            }
            LOG((CLOG_ERR
                "Windows input desktop is poisoned by a stuck command; requesting supervised process recovery message=%u sequence=%llu desktop=%s",
                static_cast<unsigned int>(msg),
                static_cast<unsigned long long>(sequence),
                desk->m_name.c_str()));
            requestInputRecovery(
                msg, "command completion timeout", isOrderedInputCommand(msg));
            return false;
        }

        LOG((CLOG_WARN
            "Windows input command timed out message=%u sequence=%llu desktop=%s",
            static_cast<unsigned int>(msg),
            static_cast<unsigned long long>(sequence), desk->m_name.c_str()));
        return false;
    }

    return true;
}

void
MSWindowsDesks::requestInputRecovery(UINT msg, const char* reason,
                                     bool orderedCommand) const
{
    bool startRecovery = false;
    {
        Lock lock(&m_mutex);
        startRecovery =
            beginInputRecoveryForTest(m_inputRecoveryRequested);
    }
    if (!startRecovery) {
        return;
    }

    LOG((CLOG_ERR
        "Windows %sinput command failed; requesting supervised process recovery message=%u reason=%s",
        orderedCommand ? "ordered " : "async ",
        static_cast<unsigned int>(msg),
        reason != NULL ? reason : "<unknown>"));

    // The normal path is Event::kQuit followed by watchdog restart. If a desk
    // thread is stuck inside Windows and removeDesks() cannot join it, force
    // only this process down so the external supervisor can replace it. The
    // callback owns no object state and disappears with a normal process exit.
    HANDLE hardExitThread = CreateThread(
        NULL, 0, terminateInputProcessAfterRecoveryDeadline,
        NULL, 0, NULL);
    if (hardExitThread != NULL) {
        CloseHandle(hardExitThread);
    }
    else {
        LOG((CLOG_ERR
            "could not arm Windows input recovery hard-exit deadline error=%lu",
            static_cast<unsigned long>(GetLastError())));
        TerminateProcess(GetCurrentProcess(), ERROR_PROCESS_ABORTED);
        ExitProcess(ERROR_PROCESS_ABORTED);
    }

    m_events->addEvent(Event(Event::kQuit));
}

HCURSOR
MSWindowsDesks::createBlankCursor() const
{
    // create a transparent cursor
    int cw = GetSystemMetrics(SM_CXCURSOR);
    int ch = GetSystemMetrics(SM_CYCURSOR);
    UInt8* cursorAND = new UInt8[ch * ((cw + 31) >> 2)];
    UInt8* cursorXOR = new UInt8[ch * ((cw + 31) >> 2)];
    memset(cursorAND, 0xff, ch * ((cw + 31) >> 2));
    memset(cursorXOR, 0x00, ch * ((cw + 31) >> 2));
    HCURSOR c = CreateCursor(MSWindowsScreen::getWindowInstance(),
                            0, 0, cw, ch, cursorAND, cursorXOR);
    delete[] cursorXOR;
    delete[] cursorAND;
    return c;
}

void
MSWindowsDesks::destroyCursor(HCURSOR cursor) const
{
    if (cursor != NULL) {
        DestroyCursor(cursor);
    }
}

ATOM
MSWindowsDesks::createDeskWindowClass(bool isPrimary) const
{
    WNDCLASSEX classInfo;
    classInfo.cbSize        = sizeof(classInfo);
    classInfo.style         = CS_DBLCLKS | CS_NOCLOSE;
    classInfo.lpfnWndProc   = isPrimary ?
                                &MSWindowsDesks::primaryDeskProc :
                                &MSWindowsDesks::secondaryDeskProc;
    classInfo.cbClsExtra    = 0;
    classInfo.cbWndExtra    = 0;
    classInfo.hInstance     = MSWindowsScreen::getWindowInstance();
    classInfo.hIcon         = NULL;
    classInfo.hCursor       = m_cursor;
    classInfo.hbrBackground = NULL;
    classInfo.lpszMenuName  = NULL;
    classInfo.lpszClassName = "BarrierDesk";
    classInfo.hIconSm       = NULL;
    return RegisterClassEx(&classInfo);
}

void
MSWindowsDesks::destroyClass(ATOM windowClass) const
{
    if (windowClass != 0) {
        UnregisterClass(MAKEINTATOM(windowClass),
                            MSWindowsScreen::getWindowInstance());
    }
}

HWND
MSWindowsDesks::createWindow(ATOM windowClass, const char* name) const
{
    HWND window = CreateWindowEx(WS_EX_TRANSPARENT |
                                    WS_EX_TOOLWINDOW,
                                MAKEINTATOM(windowClass),
                                name,
                                WS_POPUP,
                                0, 0, 1, 1,
                                NULL, NULL,
                                MSWindowsScreen::getWindowInstance(),
                                NULL);
    if (window == NULL) {
        LOG((CLOG_ERR "failed to create window: %d", GetLastError()));
        throw XScreenOpenFailure();
    }
    return window;
}

void
MSWindowsDesks::destroyWindow(HWND hwnd) const
{
    if (hwnd != NULL) {
        DestroyWindow(hwnd);
    }
}

LRESULT CALLBACK
MSWindowsDesks::primaryDeskProc(
                HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK
MSWindowsDesks::secondaryDeskProc(
                HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // would like to detect any local user input and hide the hider
    // window but for now we just detect mouse motion.
    bool hide = false;
    switch (msg) {
    case WM_MOUSEMOVE:
        if (LOWORD(lParam) != 0 || HIWORD(lParam) != 0) {
            hide = true;
        }
        break;
    }

    if (hide && IsWindowVisible(hwnd)) {
        ReleaseCapture();
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE |
                            SWP_NOACTIVATE | SWP_HIDEWINDOW);
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool
MSWindowsDesks::deskMouseMove(SInt32 x, SInt32 y) const
{
    SInt32 originX = 0;
    SInt32 originY = 0;
    SInt32 width = GetSystemMetrics(SM_CXSCREEN);
    SInt32 height = GetSystemMetrics(SM_CYSCREEN);
    DWORD flags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;

    if (m_multimon) {
        originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        flags |= MOUSEEVENTF_VIRTUALDESK;
    }
    if (m_lowLatencyMode || m_nestedRemoteMode) {
        flags |= MOUSEEVENTF_MOVE_NOCOALESCE;
    }

    return sendMouseInput(
        normalizeMouseCoordinate(x, originX, width),
        normalizeMouseCoordinate(y, originY, height),
        flags,
        0);
}

bool
MSWindowsDesks::deskMouseRelativeMove(SInt32 dx, SInt32 dy) const
{
    // relative moves are subject to cursor acceleration which we don't
    // want.so we disable acceleration, do the relative move, then
    // restore acceleration.  there's a slight chance we'll end up in
    // the wrong place if the user moves the cursor using this system's
    // mouse while simultaneously moving the mouse on the server
    // system.  that defeats the purpose of barrier so we'll assume
    // that won't happen.  even if it does, the next mouse move will
    // correct the position.

    // save mouse speed & acceleration
    int oldSpeed[4];
    const bool manageAccelerationPerMove =
        !m_relativeMoveAccelerationDisabled &&
        !m_lowLatencyMode &&
        !m_nestedRemoteMode;
    bool accelChanged = false;

    if (manageAccelerationPerMove) {
        accelChanged =
                    SystemParametersInfo(SPI_GETMOUSE,0, oldSpeed, 0) &&
                    SystemParametersInfo(SPI_GETMOUSESPEED, 0, oldSpeed + 3, 0);

        if (accelChanged) {
            int newSpeed[4] = { 0, 0, 0, 1 };
            accelChanged =
                    SystemParametersInfo(SPI_SETMOUSE, 0, newSpeed, 0) &&
                    SystemParametersInfo(SPI_SETMOUSESPEED, 0, newSpeed + 3, 0);
        }
    }

    DWORD flags = MOUSEEVENTF_MOVE;
    if (m_lowLatencyMode || m_nestedRemoteMode) {
        flags |= MOUSEEVENTF_MOVE_NOCOALESCE;
    }
    const bool injected = sendMouseInput(dx, dy, flags, 0);

    if (manageAccelerationPerMove && accelChanged) {
        SystemParametersInfo(SPI_SETMOUSE, 0, oldSpeed, 0);
        SystemParametersInfo(SPI_SETMOUSESPEED, 0, oldSpeed + 3, 0);
    }
    return injected;
}

void
MSWindowsDesks::deskEnter(Desk* desk)
{
    endLowLatencyRelativeMoves();

    if (!m_isPrimary) {
        ReleaseCapture();
    }
    ShowCursor(TRUE);
    SetWindowPos(desk->m_window, HWND_BOTTOM, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE |
                            SWP_NOACTIVATE | SWP_HIDEWINDOW);

    // restore the foreground window
    // XXX -- this raises the window to the top of the Z-order.  we
    // want it to stay wherever it was to properly support X-mouse
    // (mouse over activation) but i've no idea how to do that.
    // the obvious workaround of using SetWindowPos() to move it back
    // after being raised doesn't work.
    DWORD thisThread =
        GetWindowThreadProcessId(desk->m_window, NULL);
    DWORD thatThread =
        GetWindowThreadProcessId(desk->m_foregroundWindow, NULL);
    AttachThreadInput(thatThread, thisThread, TRUE);
    SetForegroundWindow(desk->m_foregroundWindow);
    AttachThreadInput(thatThread, thisThread, FALSE);
    EnableWindow(desk->m_window, FALSE);
    desk->m_foregroundWindow = NULL;
}

bool
MSWindowsDesks::deskLeave(Desk* desk, HKL keyLayout)
{
    ShowCursor(FALSE);
    if (m_isPrimary) {
        // map a window to hide the cursor and to use whatever keyboard
        // layout we choose rather than the keyboard layout of the last
        // active window.
        int x, y, w, h;
        // with a low level hook the cursor will never budge so
        // just a 1x1 window is sufficient.
        x = m_xCenter;
        y = m_yCenter;
        w = 1;
        h = 1;
        SetWindowPos(desk->m_window, HWND_TOP, x, y, w, h,
                            SWP_NOACTIVATE | SWP_SHOWWINDOW);

        // since we're using low-level hooks, disable the foreground window
        // so it can't mess up any of our keyboard events.  the console
        // program, for example, will cause characters to be reported as
        // unshifted, regardless of the shift key state.  interestingly
        // we do see the shift key go down and up.
        //
        // note that we must enable the window to activate it and we
        // need to disable the window on deskEnter.
        desk->m_foregroundWindow = getForegroundWindow();
        if (desk->m_foregroundWindow != NULL) {
            EnableWindow(desk->m_window, TRUE);
            SetActiveWindow(desk->m_window);
            DWORD thisThread =
                GetWindowThreadProcessId(desk->m_window, NULL);
            DWORD thatThread =
                GetWindowThreadProcessId(desk->m_foregroundWindow, NULL);
            AttachThreadInput(thatThread, thisThread, TRUE);
            SetForegroundWindow(desk->m_window);
            AttachThreadInput(thatThread, thisThread, FALSE);
        }

        // switch to requested keyboard layout
        ActivateKeyboardLayout(keyLayout, 0);
        return true;
    }
    else {
        beginLowLatencyRelativeMoves();

        // move hider window under the cursor center, raise, and show it
        SetWindowPos(desk->m_window, HWND_TOP,
                            m_xCenter, m_yCenter, 1, 1,
                            SWP_NOACTIVATE | SWP_SHOWWINDOW);

        // watch for mouse motion.  if we see any then we hide the
        // hider window so the user can use the physically attached
        // mouse if desired.  we'd rather not capture the mouse but
        // we aren't notified when the mouse leaves our window.
        SetCapture(desk->m_window);

        // warp the mouse to the cursor center
        LOG((CLOG_DEBUG2 "warping cursor to center: %+d,%+d", m_xCenter, m_yCenter));
        return deskMouseMove(m_xCenter, m_yCenter);
    }
}

void MSWindowsDesks::desk_thread(Desk* desk)
{
    MSG msg;

    // use given desktop for this thread
    {
        Lock lock(&m_mutex);
        desk->m_threadID = GetCurrentThreadId();
    }
    desk->m_foregroundWindow = NULL;
    const bool threadAttached =
        desk->m_desk != NULL && SetThreadDesktop(desk->m_desk) != 0;

    // Create the thread message queue before advertising startup completion.
    // removeDesks() can then safely post WM_QUIT once startup is complete.
    PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE);
    HWND window = NULL;
    if (threadAttached) {
        // create a window.  we use this window to hide the cursor.
        try {
            window = createWindow(m_deskClass, "BarrierDesk");
            LOG((CLOG_DEBUG "desk %s window is 0x%08x", desk->m_name.c_str(), window));
        }
        catch (...) {
            // ignore
            LOG((CLOG_DEBUG "can't create desk window for %s", desk->m_name.c_str()));
        }
    }

    // Report capability, not just thread startup. A desktop without a
    // successful attachment and message window cannot accept input safely.
    bool shutdownRequested = false;
    {
        Lock lock(&m_mutex);
        desk->m_window = window;
        desk->m_threadAttached = threadAttached;
        desk->m_windowReady = window != NULL;
        desk->m_startupComplete = true;
        desk->m_threadRunning = true;
        shutdownRequested = desk->m_shutdownRequested;
        m_deskReady = true;
        m_deskReady.broadcast();
    }

    BOOL messageResult = 0;
    while (!shutdownRequested &&
           (messageResult = GetMessage(&msg, NULL, 0, 0)) > 0) {
        if (msg.message == BARRIER_MSG_DESK_STOP) {
            break;
        }

        DeskCommand* command = NULL;
        if (msg.message == BARRIER_MSG_DESK_COMMAND) {
            command = reinterpret_cast<DeskCommand*>(msg.wParam);
            if (command == NULL) {
                continue;
            }
        }

        bool processCommand = true;
        bool commandSucceeded = true;
        DWORD commandError = ERROR_SUCCESS;
        if (command != NULL) {
            Lock lock(&m_mutex);
            if (desk->m_pendingMotionCommand == command) {
                desk->m_pendingMotionCommand = NULL;
            }
            msg.message = command->message;
            msg.wParam = command->wParam;
            msg.lParam = command->lParam;
            processCommand = shouldProcessDeskCommandForTest(
                command->sequence, desk->m_cancelledCommandSequence);
            if (processCommand) {
                desk->m_executingCommandSequence = command->sequence;
            }
        }

        if (!processCommand) {
            if (isOrderedInputCommand(command->message)) {
                LOG((CLOG_WARN
                    "dropping expired ordered Windows input command message=%u sequence=%llu",
                    static_cast<unsigned int>(command->message),
                    static_cast<unsigned long long>(command->sequence)));
            }
            else {
                LOG((CLOG_DEBUG1
                    "dropping expired Windows input command sequence=%llu",
                    static_cast<unsigned long long>(command->sequence)));
            }
        }
        else switch (msg.message) {
        default:
            if (command == NULL) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                continue;
            }
            LOG((CLOG_WARN "ignoring unknown Windows input command %u",
                static_cast<unsigned int>(msg.message)));
            break;

        case BARRIER_MSG_SWITCH:
        {
            bool hookInstalled = false;
            if (m_isPrimary && !m_noHooks) {
                MSWindowsHook::uninstall();
                if (m_screensaverNotify) {
                    MSWindowsHook::uninstallScreenSaver();
                    MSWindowsHook::installScreenSaver();
                }
                hookInstalled = MSWindowsHook::install();
                if (!hookInstalled) {
                    // we won't work on this desk
                    LOG((CLOG_WARN "cannot install input hook on desktop %s",
                        desk->m_name.c_str()));
                }
                // a window on the primary screen with low-level hooks
                // should never activate.
                if (desk->m_window)
                    EnableWindow(desk->m_window, FALSE);
            }
            {
                Lock lock(&m_mutex);
                desk->m_hookInstalled = hookInstalled;
            }
            break;
        }

        case BARRIER_MSG_ENTER:
            {
                Lock lock(&m_mutex);
                m_isOnScreen = true;
            }
            deskEnter(desk);
            break;

        case BARRIER_MSG_LEAVE:
        {
            const HKL keyLayout = reinterpret_cast<HKL>(msg.wParam);
            {
                Lock lock(&m_mutex);
                m_isOnScreen = false;
                m_keyLayout = keyLayout;
            }
            commandSucceeded = deskLeave(desk, keyLayout);
            if (!commandSucceeded) {
                commandError = GetLastError();
            }
            break;
        }

        case BARRIER_MSG_FAKE_KEY:
            commandSucceeded = sendKeyboardInput(
                HIBYTE(msg.lParam), LOBYTE(msg.lParam), (DWORD)msg.wParam);
            if (!commandSucceeded) {
                commandError = GetLastError();
            }
            break;

        case BARRIER_MSG_FAKE_BUTTON:
            if (msg.wParam != 0) {
                commandSucceeded = sendMouseInput(
                    0, 0, static_cast<DWORD>(msg.wParam),
                    static_cast<DWORD>(msg.lParam));
                if (!commandSucceeded) {
                    commandError = GetLastError();
                }
            }
            break;

        case BARRIER_MSG_FAKE_MOVE:
            commandSucceeded = deskMouseMove(
                static_cast<SInt32>(msg.wParam),
                static_cast<SInt32>(msg.lParam));
            if (!commandSucceeded) {
                commandError = GetLastError();
            }
            break;

        case BARRIER_MSG_FAKE_REL_MOVE:
            commandSucceeded = deskMouseRelativeMove(
                static_cast<SInt32>(msg.wParam),
                static_cast<SInt32>(msg.lParam));
            if (!commandSucceeded) {
                commandError = GetLastError();
            }
            break;

        case BARRIER_MSG_FAKE_WHEEL:
            if (msg.lParam != 0) {
                commandSucceeded = sendMouseInput(
                    0, 0, MOUSEEVENTF_WHEEL,
                    static_cast<DWORD>(msg.lParam));
            }
            else if (IsWindowsVistaOrGreater() && msg.wParam != 0) {
                commandSucceeded = sendMouseInput(
                    0, 0, MOUSEEVENTF_HWHEEL,
                    static_cast<DWORD>(msg.wParam));
            }
            if (!commandSucceeded) {
                commandError = GetLastError();
            }
            break;

        case BARRIER_MSG_CURSOR_POS: {
            POINT pos;
            if (!GetCursorPos(&pos)) {
                pos.x = m_xCenter;
                pos.y = m_yCenter;
            }
            {
                Lock lock(&m_mutex);
                m_cursorPos = pos;
            }
            break;
        }

        case BARRIER_MSG_SYNC_KEYS:
            m_updateKeys();
            break;

        case BARRIER_MSG_SCREENSAVER:
            if (!m_noHooks) {
                if (msg.wParam != 0) {
                    MSWindowsHook::installScreenSaver();
                }
                else {
                    MSWindowsHook::uninstallScreenSaver();
                }
            }
            break;

        case BARRIER_MSG_FAKE_INPUT:
            commandSucceeded = sendKeyboardInput(
                BARRIER_HOOK_FAKE_INPUT_VIRTUAL_KEY,
                BARRIER_HOOK_FAKE_INPUT_SCANCODE,
                msg.wParam ? 0 : KEYEVENTF_KEYUP);
            if (!commandSucceeded) {
                commandError = GetLastError();
            }
            break;
        }

        if (command != NULL) {
            const bool commandInjectionFailed =
                processCommand && !commandSucceeded;
            {
                Lock lock(&m_mutex);
                desk->m_completedCommandSequence = command->sequence;
                if (desk->m_executingCommandSequence == command->sequence) {
                    desk->m_executingCommandSequence = 0;
                }
                if (commandCompletionProvesResponsiveForTest(
                        processCommand, commandSucceeded, command->sequence,
                        desk->m_poisonedThroughCommandSequence)) {
                    desk->m_commandResponsive = true;
                }
                else if (commandInjectionFailed) {
                    if (desk == m_activeDesk && desk->m_commandResponsive) {
                        ++m_inputDesktopGeneration;
                    }
                    desk->m_commandResponsive = false;
                    desk->m_failedCommandSequence = command->sequence;
                }
                m_deskReady = true;
                m_deskReady.broadcast();
            }
            const std::uint64_t completedSequence = command->sequence;
            delete command;
            if (commandInjectionFailed) {
                LOG((CLOG_ERR
                    "Windows SendInput failed message=%u sequence=%llu error=%lu; rejecting input lease",
                    static_cast<unsigned int>(msg.message),
                    static_cast<unsigned long long>(completedSequence),
                    static_cast<unsigned long>(commandError)));
                requestInputRecovery(
                    msg.message, "SendInput rejected input injection",
                    isOrderedInputCommand(msg.message));
            }
        }
    }

    if (messageResult == -1) {
        LOG((CLOG_ERR "Windows input desktop message loop failed: %lu",
            static_cast<unsigned long>(GetLastError())));
    }

    while (PeekMessage(&msg, NULL, BARRIER_MSG_DESK_COMMAND,
                       BARRIER_MSG_DESK_COMMAND, PM_REMOVE)) {
        DeskCommand* command = reinterpret_cast<DeskCommand*>(msg.wParam);
        {
            Lock lock(&m_mutex);
            if (desk->m_pendingMotionCommand == command) {
                desk->m_pendingMotionCommand = NULL;
            }
        }
        delete command;
    }

    // clean up
    {
        Lock lock(&m_mutex);
        desk->m_hookInstalled = false;
        desk->m_windowReady = false;
        desk->m_threadAttached = false;
        desk->m_commandResponsive = false;
        desk->m_executingCommandSequence = 0;
        desk->m_pendingMotionCommand = NULL;
        desk->m_threadRunning = false;
        m_deskReady.broadcast();
    }
    deskEnter(desk);
    if (window != NULL) {
        DestroyWindow(window);
    }
    {
        Lock lock(&m_mutex);
        if (desk->m_window == window) {
            desk->m_window = NULL;
        }
    }
    if (desk->m_desk != NULL) {
        closeDesktop(desk->m_desk);
    }
}

MSWindowsDesks::Desk* MSWindowsDesks::addDesk(const std::string& name, HDESK hdesk)
{
    Desk* desk      = new Desk;
    desk->m_name     = name;
    desk->m_desk     = hdesk;
    desk->m_threadID = 0;
    desk->m_targetID = GetCurrentThreadId();
    desk->m_window = NULL;
    desk->m_foregroundWindow = NULL;
    desk->m_lowLevel = false;
    desk->m_threadAttached = false;
    desk->m_windowReady = false;
    desk->m_hookInstalled = false;
    desk->m_startupComplete = false;
    desk->m_startupDeadline =
        static_cast<std::uint64_t>(GetTickCount64() + kDeskStartupDeadline);
    desk->m_shutdownRequested = false;
    desk->m_threadRunning = false;
    desk->m_commandResponsive = false;
    desk->m_completedCommandSequence = 0;
    desk->m_cancelledCommandSequence = 0;
    desk->m_executingCommandSequence = 0;
    desk->m_poisonedThroughCommandSequence = 0;
    desk->m_failedCommandSequence = 0;
    desk->m_lastMotionCommandSequence = 0;
    desk->m_pendingMotionCommand = NULL;
    desk->m_thread   = new Thread([this, desk]() { desk_thread(desk); });
    m_desks.insert(std::make_pair(name, desk));
    return desk;
}

void
MSWindowsDesks::removeDesks()
{
    Lock sendLock(&m_sendMutex);
    {
        Lock lock(&m_mutex);
        m_activeDesk = NULL;
        m_activeDeskName = "";
        m_observedDeskName = "";
    }

    for (Desks::iterator index = m_desks.begin();
                            index != m_desks.end(); ++index) {
        Desk* desk = index->second;
        DWORD threadID = 0;
        HWND window = NULL;
        bool postQuit = false;
        {
            Lock lock(&m_mutex);
            desk->m_shutdownRequested = true;
            postQuit = shouldPostDeskQuitForTest(
                desk->m_startupComplete, desk->m_threadID);
            threadID = desk->m_threadID;
            window = desk->m_threadRunning && desk->m_windowReady ?
                desk->m_window : NULL;
        }
        bool stopPosted = !postQuit;
        DWORD postError = ERROR_SUCCESS;
        for (int attempt = 0;
             postQuit && !stopPosted && attempt < kDeskStopPostRetries;
             ++attempt) {
            if (PostThreadMessage(threadID, WM_QUIT, 0, 0) != 0) {
                stopPosted = true;
                break;
            }
            postError = GetLastError();
            if (window != NULL &&
                PostMessage(window, BARRIER_MSG_DESK_STOP, 0, 0) != 0) {
                stopPosted = true;
                break;
            }
            if (desk->m_thread->wait(0.0)) {
                stopPosted = true;
                break;
            }
            Sleep(1);
        }
        if (!stopPosted) {
            LOG((CLOG_ERR
                "could not signal Windows input desktop thread shutdown id=%lu error=%lu; waiting for safe teardown",
                static_cast<unsigned long>(threadID),
                static_cast<unsigned long>(postError)));
        }
        if (!desk->m_thread->wait(0.0)) {
            desk->m_thread->cancel();
            desk->m_thread->unblockPollSocket();
            barrier::waitForFinalThreadShutdown(
                "Windows input desktop thread",
                barrier::kFinalThreadShutdownDeadlineSeconds,
                [desk](double timeout) {
                    return desk->m_thread->wait(timeout);
                });
        }
        delete desk->m_thread;
        delete desk;
    }
    m_desks.clear();
}

void
MSWindowsDesks::checkDesk()
{
    Desk* activeDesk = NULL;
    std::string activeDeskName;
    bool wasOnScreen = false;
    HKL keyLayout = NULL;
    {
        Lock lock(&m_mutex);
        activeDesk = m_activeDesk;
        activeDeskName = m_activeDeskName;
        wasOnScreen = m_isOnScreen;
        keyLayout = m_keyLayout;
    }

    // get current desktop.  if we already know about it then return.
    Desk* desk;
    HDESK hdesk  = openInputDesktop();
    std::string name = getDesktopName(hdesk);
    {
        Lock lock(&m_mutex);
        m_observedDeskName = name;
    }
    Desks::const_iterator index = m_desks.find(name);
    if (index == m_desks.end()) {
        desk = addDesk(name, hdesk);
        // hold on to hdesk until thread exits so the desk can't
        // be removed by the system
    }
    else {
        closeDesktop(hdesk);
        desk = index->second;
    }

    // if we are told to shut down on desk switch, and this is not the
    // first switch, then shut down.
    if (m_stopOnDeskSwitch && activeDesk != NULL && name != activeDeskName) {
        LOG((CLOG_DEBUG "shutting down because of desk switch to \"%s\"", name.c_str()));
        m_events->addEvent(Event(Event::kQuit));
        return;
    }

    const bool inputLeaseActive = activeDesk != NULL && wasOnScreen;
    const bool screensaverActive = m_screensaver->isActive();
    if (name != activeDeskName && screensaverActive) {
        if (inputLeaseActive) {
            requestInputRecovery(
                BARRIER_MSG_SWITCH,
                "desktop changed during input lease while switching is blocked",
                false);
        }
        else {
            // screen saver might have started
            PostThreadMessage(
                m_threadID, BARRIER_MSG_SCREEN_SAVER, TRUE, 0);
        }
        return;
    }

    // if active desktop changed then tell the old and new desk threads
    // about the change.  don't switch desktops when the screensaver is
    // active because we'd most likely switch to the screensaver desktop
    // which would have the side effect of forcing the screensaver to
    // stop.
    if (name != activeDeskName) {
        bool startupComplete = false;
        std::uint64_t startupDeadline = 0;
        bool canActivate = false;
        {
            Lock lock(&m_mutex);
            startupComplete = desk->m_startupComplete;
            startupDeadline = desk->m_startupDeadline;
            canActivate = canActivateDesktopForTest(
                desk->m_startupComplete,
                desk->m_threadRunning,
                desk->m_threadAttached,
                desk->m_windowReady);
        }
        const std::uint64_t now =
            static_cast<std::uint64_t>(GetTickCount64());
        const DesktopTransitionAction transition =
            desktopTransitionActionForTest(
                false, canActivate, inputLeaseActive,
                startupComplete, now, startupDeadline);
        if (transition == kRecoverInputProcess) {
            const char* reason = inputLeaseActive ?
                "desktop changed before replacement helper was ready" :
                (startupComplete ?
                    "desktop helper startup capability failure" :
                    "desktop helper startup timed out");
            LOG((CLOG_ERR
                "Windows input desktop cannot be activated safely; requesting recovery current=%s observed=%s lease=%d startupComplete=%d deadlineExpired=%d",
                activeDeskName.empty() ?
                    "<none>" : activeDeskName.c_str(),
                name.empty() ? "<unavailable>" : name.c_str(),
                inputLeaseActive ? 1 : 0,
                startupComplete ? 1 : 0,
                now >= startupDeadline ? 1 : 0));
            requestInputRecovery(BARRIER_MSG_SWITCH, reason, false);
            return;
        }
        if (transition == kWaitForObservedDesktop) {
            LOG((CLOG_DEBUG1
                "Windows input desktop is still starting without an active input lease current=%s pending=%s",
                activeDeskName.empty() ?
                    "<none>" : activeDeskName.c_str(),
                name.empty() ? "<unavailable>" : name.c_str()));
            return;
        }

        // Stop the old helper at an acknowledged control boundary before
        // publishing the replacement. ENTER safely supersedes any queued
        // motion and restores the old desktop's local cursor state.
        if (activeDesk != NULL &&
            !sendMessage(BARRIER_MSG_ENTER, 0, 0)) {
            requestInputRecovery(
                BARRIER_MSG_ENTER,
                "could not stop old desktop input before handoff", false);
            return;
        }

        // check for desk accessibility change.  we don't get events
        // from an inaccessible desktop so when we switch from an
        // inaccessible desktop to an accessible one we have to
        // update the keyboard state.
        LOG((CLOG_DEBUG "switched to desk \"%s\"", name.c_str()));
        bool syncKeys = false;
        bool isAccessible = isDeskAccessible(desk);
        if (isDeskAccessible(activeDesk) != isAccessible) {
            if (isAccessible) {
                LOG((CLOG_DEBUG "desktop is now accessible"));
                syncKeys = true;
            }
            else {
                LOG((CLOG_DEBUG "desktop is now inaccessible"));
            }
        }

        // switch desk
        std::uint64_t generation = 0;
        {
            Lock sendLock(&m_sendMutex);
            {
                Lock lock(&m_mutex);
                m_activeDesk = desk;
                m_activeDeskName = name;
                // A cached desktop is not ready for a new activation until
                // its switch command has reinstalled the active hook state.
                desk->m_commandResponsive = false;
                generation = ++m_inputDesktopGeneration;
            }
        }
        m_nextDeskRecoveryProbe =
            GetTickCount64() + kDeskRecoveryProbeInterval;
        if (!sendMessage(BARRIER_MSG_SWITCH, 0, 0)) {
            {
                Lock sendLock(&m_sendMutex);
                Lock lock(&m_mutex);
                if (m_activeDesk == desk) {
                    m_activeDesk = activeDesk;
                    m_activeDeskName = activeDeskName;
                    ++m_inputDesktopGeneration;
                }
            }
            LOG((CLOG_WARN
                "Windows input desktop activation failed; restored desktop %s",
                activeDeskName.empty() ? "<none>" : activeDeskName.c_str()));
            if (!wasOnScreen && activeDesk != NULL) {
                leave(keyLayout);
            }
            return;
        }

        const DeskReadinessSnapshot readiness = getDeskReadiness(desk);
        if (readiness.ready) {
            LOG((CLOG_INFO
                "Windows input desktop generation=%llu name=%s attached=yes window=yes hook=%s ready=yes",
                static_cast<unsigned long long>(generation),
                name.empty() ? "<unavailable>" : name.c_str(),
                (m_isPrimary && !m_noHooks) ? "yes" : "not-required"));
        }
        else {
            LOG((CLOG_WARN
                "Windows input desktop generation=%llu name=%s attached=%s window=%s hook=%s responsive=%s ready=no",
                static_cast<unsigned long long>(generation),
                name.empty() ? "<unavailable>" : name.c_str(),
                readiness.threadAttached ? "yes" : "no",
                readiness.windowReady ? "yes" : "no",
                readiness.hookInstalled ? "yes" : "no",
                readiness.commandResponsive ? "yes" : "no"));
        }

        // hide cursor on new desk
        if (!wasOnScreen) {
            leave(keyLayout);
        }

        // update keys if necessary
        if (syncKeys) {
            updateKeys();
        }
    }
    else if (!screensaverActive && !isDeskReady(desk) &&
             GetTickCount64() >= m_nextDeskRecoveryProbe) {
        m_nextDeskRecoveryProbe =
            GetTickCount64() + kDeskRecoveryProbeInterval;
        std::uint64_t generation = 0;
        {
            Lock lock(&m_mutex);
            generation = ++m_inputDesktopGeneration;
        }
        LOG((CLOG_INFO
            "retrying Windows input desktop generation=%llu name=%s",
            static_cast<unsigned long long>(generation),
            name.empty() ? "<unavailable>" : name.c_str()));
        sendMessage(BARRIER_MSG_SWITCH, 0, 0);
    }
}

bool
MSWindowsDesks::isDeskAccessible(const Desk* desk) const
{
    Lock lock(&m_mutex);
    return desk != NULL && desk->m_threadAttached;
}

bool
MSWindowsDesks::isDeskReady(const Desk* desk) const
{
    Lock lock(&m_mutex);
    return isDeskReadyLocked(desk);
}

bool
MSWindowsDesks::isDeskReadyLocked(const Desk* desk) const
{
    return desk != NULL && isDesktopReadyForTest(
        m_isPrimary, m_noHooks, desk->m_threadAttached,
        desk->m_windowReady, desk->m_hookInstalled,
        desk->m_commandResponsive);
}

MSWindowsDesks::DeskReadinessSnapshot
MSWindowsDesks::getDeskReadiness(const Desk* desk) const
{
    DeskReadinessSnapshot snapshot;
    Lock lock(&m_mutex);
    if (desk != NULL) {
        snapshot.threadAttached = desk->m_threadAttached;
        snapshot.windowReady = desk->m_windowReady;
        snapshot.hookInstalled = desk->m_hookInstalled;
        snapshot.commandResponsive = desk->m_commandResponsive;
        snapshot.ready = isDeskReadyLocked(desk);
    }
    return snapshot;
}

MSWindowsDesks::DeskCommandWaitResult
MSWindowsDesks::waitForDeskCommand(const Desk* desk,
                                   std::uint64_t sequence,
                                   double timeout) const
{
    Stopwatch timer;
    Lock lock(&m_mutex);
    while (!isDeskCommandCompleteForTest(
               sequence, desk->m_completedCommandSequence,
               desk->m_threadRunning)) {
        if (!desk->m_threadRunning) {
            break;
        }
        const bool signalled = m_deskReady.wait(
            timer, boundedDeskCommandWaitTimeoutForTest(timeout));
        if (!signalled) {
            break;
        }
    }
    if (!isDeskCommandCompleteForTest(
            sequence, desk->m_completedCommandSequence,
            desk->m_threadRunning)) {
        return kDeskCommandTimedOut;
    }
    return commandInjectionSucceededForTest(
        sequence, desk->m_failedCommandSequence) ?
            kDeskCommandSucceeded : kDeskCommandFailed;
}

void
MSWindowsDesks::handleCheckDesk(const Event&, void*)
{
    checkDesk();

    // also check if screen saver is running if on a modern OS and
    // this is the primary screen.
    if (m_isPrimary) {
        BOOL running;
        SystemParametersInfo(SPI_GETSCREENSAVERRUNNING, 0, &running, FALSE);
        PostThreadMessage(m_threadID, BARRIER_MSG_SCREEN_SAVER, running, 0);
    }
}

HDESK
MSWindowsDesks::openInputDesktop() const
{
    return OpenInputDesktop(
        DF_ALLOWOTHERACCOUNTHOOK, FALSE,
        DESKTOP_CREATEWINDOW | DESKTOP_HOOKCONTROL |
        DESKTOP_READOBJECTS | GENERIC_WRITE);
}

HDESK
MSWindowsDesks::openInputDesktopForProbe() const
{
    // Standby readiness only identifies the current desktop. It must not ask
    // Windows for hook or write capabilities before the previous owner exits.
    return OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
}

void
MSWindowsDesks::closeDesktop(HDESK desk) const
{
    if (desk != NULL) {
        CloseDesktop(desk);
    }
}

std::string MSWindowsDesks::getDesktopName(HDESK desk) const
{
    if (desk == NULL) {
        return {};
    }

    DWORD size = 0;
    GetUserObjectInformationA(desk, UOI_NAME, NULL, 0, &size);
    if (size == 0) {
        return {};
    }

    std::vector<char> name(size, '\0');
    if (GetUserObjectInformationA(
            desk, UOI_NAME, name.data(), size, &size) == FALSE) {
        return {};
    }
    return std::string(name.data());
}

HWND
MSWindowsDesks::getForegroundWindow() const
{
    // Ideally we'd return NULL as much as possible, only returning
    // the actual foreground window when we know it's going to mess
    // up our keyboard input.  For now we'll just let the user
    // decide.
    if (m_leaveForegroundOption) {
        return NULL;
    }
    return GetForegroundWindow();
}
