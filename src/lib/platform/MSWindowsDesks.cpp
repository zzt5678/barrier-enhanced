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
#include "arch/win32/ArchMiscWindows.h"
#include "base/Log.h"
#include "base/IEventQueue.h"
#include "base/Stopwatch.h"
#include "base/TMethodEventJob.h"

#include <malloc.h>
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
const double kDeskStartupTimeout = 2.0;
const ULONGLONG kDeskRecoveryProbeInterval = 1000;

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

namespace {

struct DeskCommand {
    DeskCommand(UINT commandMessage, WPARAM commandWParam,
                LPARAM commandLParam, std::uint64_t commandSequence) :
        message(commandMessage),
        wParam(commandWParam),
        lParam(commandLParam),
        sequence(commandSequence)
    {
    }

    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    std::uint64_t sequence;
};

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

void sendKeyboardInput(UINT virtualKey, UINT scanCode, DWORD flags)
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

    SendInput(1, &input, sizeof(input));
}

void sendMouseInput(LONG dx, LONG dy, DWORD flags, DWORD mouseData)
{
    INPUT input;
    ZeroMemory(&input, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = flags;
    input.mi.mouseData = mouseData;
    SendInput(1, &input, sizeof(input));
}

}

//
// MSWindowsDesks
//

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
    m_mutex(),
    m_sendMutex(),
    m_deskReady(&m_mutex, false),
    m_inputDesktopGeneration(0),
    m_nextDeskCommandSequence(0),
    m_cursorPos{0, 0},
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

void
MSWindowsDesks::leave(HKL keyLayout)
{
    sendMessage(BARRIER_MSG_LEAVE, (WPARAM)keyLayout, 0);
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
    sendMessage(BARRIER_MSG_FAKE_KEY, flags,
                            MAKEWORD(static_cast<BYTE>(button & 0xffu),
                                static_cast<BYTE>(virtualKey & 0xffu)));
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
    sendMessage(BARRIER_MSG_FAKE_BUTTON, flags, data);
}

bool
MSWindowsDesks::fakeMouseMove(SInt32 x, SInt32 y) const
{
    return sendMessage(BARRIER_MSG_FAKE_MOVE,
                       static_cast<WPARAM>(x),
                       static_cast<LPARAM>(y));
}

void
MSWindowsDesks::fakeMouseRelativeMove(SInt32 dx, SInt32 dy) const
{
    sendMessage(BARRIER_MSG_FAKE_REL_MOVE,
                            static_cast<WPARAM>(dx),
                            static_cast<LPARAM>(dy));
}

void
MSWindowsDesks::fakeMouseWheel(SInt32 xDelta, SInt32 yDelta) const
{
    sendMessage(BARRIER_MSG_FAKE_WHEEL, xDelta, yDelta);
}

bool
MSWindowsDesks::canEnter() const
{
    Lock lock(&m_mutex);
    return isDeskReadyLocked(m_activeDesk);
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
    std::uint64_t sequence, std::uint64_t executingSequence)
{
    return sequence != executingSequence;
}

bool
MSWindowsDesks::commandCompletionProvesResponsiveForTest(
    bool commandExecuted, std::uint64_t sequence,
    std::uint64_t poisonedThroughSequence)
{
    return commandExecuted && sequence > poisonedThroughSequence;
}

bool
MSWindowsDesks::sendMessage(UINT msg, WPARAM wParam, LPARAM lParam) const
{
    Lock sendLock(&m_sendMutex);

    Desk* desk = NULL;
    std::uint64_t sequence = 0;
    {
        Lock lock(&m_mutex);
        desk = m_activeDesk;
        if (desk == NULL || !desk->m_threadRunning ||
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
            return false;
        }
        sequence = ++m_nextDeskCommandSequence;
    }

    DeskCommand* command = new DeskCommand(msg, wParam, lParam, sequence);
    if (PostThreadMessage(desk->m_threadID, BARRIER_MSG_DESK_COMMAND,
                          reinterpret_cast<WPARAM>(command), 0) == 0) {
        const DWORD error = GetLastError();
        delete command;
        {
            Lock lock(&m_mutex);
            if (desk == m_activeDesk && desk->m_commandResponsive) {
                ++m_inputDesktopGeneration;
            }
            desk->m_commandResponsive = false;
            if (sequence > desk->m_cancelledCommandSequence) {
                desk->m_cancelledCommandSequence = sequence;
            }
        }
        LOG((CLOG_WARN
            "cannot post Windows input command message=%u sequence=%llu error=%lu",
            static_cast<unsigned int>(msg),
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long>(error)));
        return false;
    }

    if (!waitForDeskCommand(desk, sequence, kDeskCommandTimeout)) {
        bool commandIsExecuting = false;
        {
            Lock lock(&m_mutex);
            if (isDeskCommandCompleteForTest(
                    sequence, desk->m_completedCommandSequence,
                    desk->m_threadRunning)) {
                return true;
            }
            commandIsExecuting = !canCancelTimedOutDeskCommandForTest(
                sequence, desk->m_executingCommandSequence);
            if (!commandIsExecuting) {
                if (desk == m_activeDesk && desk->m_commandResponsive) {
                    ++m_inputDesktopGeneration;
                }
                desk->m_commandResponsive = false;
                if (sequence > desk->m_cancelledCommandSequence) {
                    desk->m_cancelledCommandSequence = sequence;
                }
            }
        }

        if (commandIsExecuting) {
            LOG((CLOG_WARN
                "Windows input command exceeded %.3fs after execution began; allowing %.3fs recovery grace message=%u sequence=%llu desktop=%s",
                kDeskCommandTimeout, kDeskCommandExecutionGrace,
                static_cast<unsigned int>(msg),
                static_cast<unsigned long long>(sequence),
                desk->m_name.c_str()));
            if (waitForDeskCommand(
                    desk, sequence, kDeskCommandExecutionGrace)) {
                return true;
            }

            {
                Lock lock(&m_mutex);
                if (isDeskCommandCompleteForTest(
                        sequence, desk->m_completedCommandSequence,
                        desk->m_threadRunning)) {
                    return true;
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
            m_events->addEvent(Event(Event::kQuit));
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

void
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

    sendMouseInput(
        normalizeMouseCoordinate(x, originX, width),
        normalizeMouseCoordinate(y, originY, height),
        flags,
        0);
}

void
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
    sendMouseInput(dx, dy, flags, 0);

    if (manageAccelerationPerMove && accelChanged) {
        SystemParametersInfo(SPI_SETMOUSE, 0, oldSpeed, 0);
        SystemParametersInfo(SPI_SETMOUSESPEED, 0, oldSpeed + 3, 0);
    }
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

void
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
        deskMouseMove(m_xCenter, m_yCenter);
    }
}

void MSWindowsDesks::desk_thread(Desk* desk)
{
    MSG msg;

    // use given desktop for this thread
    desk->m_threadID         = GetCurrentThreadId();
    desk->m_window           = NULL;
    desk->m_foregroundWindow = NULL;
    const bool threadAttached =
        desk->m_desk != NULL && SetThreadDesktop(desk->m_desk) != 0;
    if (threadAttached) {
        // create a message queue
        PeekMessage(&msg, NULL, 0,0, PM_NOREMOVE);

        // create a window.  we use this window to hide the cursor.
        try {
            desk->m_window = createWindow(m_deskClass, "BarrierDesk");
            LOG((CLOG_DEBUG "desk %s window is 0x%08x", desk->m_name.c_str(), desk->m_window));
        }
        catch (...) {
            // ignore
            LOG((CLOG_DEBUG "can't create desk window for %s", desk->m_name.c_str()));
        }
    }

    // Report capability, not just thread startup. A desktop without a
    // successful attachment and message window cannot accept input safely.
    {
        Lock lock(&m_mutex);
        desk->m_threadAttached = threadAttached;
        desk->m_windowReady = desk->m_window != NULL;
        desk->m_startupComplete = true;
        desk->m_threadRunning = true;
        m_deskReady = true;
        m_deskReady.broadcast();
    }

    BOOL messageResult = 0;
    while ((messageResult = GetMessage(&msg, NULL, 0, 0)) > 0) {
        DeskCommand* command = NULL;
        if (msg.message == BARRIER_MSG_DESK_COMMAND) {
            command = reinterpret_cast<DeskCommand*>(msg.wParam);
            if (command == NULL) {
                continue;
            }
            msg.message = command->message;
            msg.wParam = command->wParam;
            msg.lParam = command->lParam;
        }

        bool processCommand = true;
        if (command != NULL) {
            Lock lock(&m_mutex);
            processCommand = shouldProcessDeskCommandForTest(
                command->sequence, desk->m_cancelledCommandSequence);
            if (processCommand) {
                desk->m_executingCommandSequence = command->sequence;
            }
        }

        if (!processCommand) {
            LOG((CLOG_DEBUG1
                "dropping expired Windows input command sequence=%llu",
                static_cast<unsigned long long>(command->sequence)));
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
            deskLeave(desk, keyLayout);
            break;
        }

        case BARRIER_MSG_FAKE_KEY:
            sendKeyboardInput(HIBYTE(msg.lParam), LOBYTE(msg.lParam), (DWORD)msg.wParam);
            break;

        case BARRIER_MSG_FAKE_BUTTON:
            if (msg.wParam != 0) {
                sendMouseInput(0, 0, static_cast<DWORD>(msg.wParam),
                                static_cast<DWORD>(msg.lParam));
            }
            break;

        case BARRIER_MSG_FAKE_MOVE:
            deskMouseMove(static_cast<SInt32>(msg.wParam),
                            static_cast<SInt32>(msg.lParam));
            break;

        case BARRIER_MSG_FAKE_REL_MOVE:
            deskMouseRelativeMove(static_cast<SInt32>(msg.wParam),
                            static_cast<SInt32>(msg.lParam));
            break;

        case BARRIER_MSG_FAKE_WHEEL:
            if (msg.lParam != 0) {
                sendMouseInput(0, 0, MOUSEEVENTF_WHEEL,
                                static_cast<DWORD>(msg.lParam));
            }
            else if (IsWindowsVistaOrGreater() && msg.wParam != 0) {
                sendMouseInput(0, 0, MOUSEEVENTF_HWHEEL,
                                static_cast<DWORD>(msg.wParam));
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
            sendKeyboardInput(BARRIER_HOOK_FAKE_INPUT_VIRTUAL_KEY,
                                BARRIER_HOOK_FAKE_INPUT_SCANCODE,
                                msg.wParam ? 0 : KEYEVENTF_KEYUP);
            break;
        }

        if (command != NULL) {
            {
                Lock lock(&m_mutex);
                desk->m_completedCommandSequence = command->sequence;
                if (desk->m_executingCommandSequence == command->sequence) {
                    desk->m_executingCommandSequence = 0;
                }
                if (commandCompletionProvesResponsiveForTest(
                        processCommand, command->sequence,
                        desk->m_poisonedThroughCommandSequence)) {
                    desk->m_commandResponsive = true;
                }
                m_deskReady = true;
                m_deskReady.broadcast();
            }
            delete command;
        }
    }

    if (messageResult == -1) {
        LOG((CLOG_ERR "Windows input desktop message loop failed: %lu",
            static_cast<unsigned long>(GetLastError())));
    }

    while (PeekMessage(&msg, NULL, BARRIER_MSG_DESK_COMMAND,
                       BARRIER_MSG_DESK_COMMAND, PM_REMOVE)) {
        delete reinterpret_cast<DeskCommand*>(msg.wParam);
    }

    // clean up
    {
        Lock lock(&m_mutex);
        desk->m_hookInstalled = false;
        desk->m_windowReady = false;
        desk->m_threadAttached = false;
        desk->m_commandResponsive = false;
        desk->m_executingCommandSequence = 0;
        desk->m_threadRunning = false;
        m_deskReady.broadcast();
    }
    deskEnter(desk);
    if (desk->m_window != NULL) {
        DestroyWindow(desk->m_window);
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
    desk->m_threadRunning = false;
    desk->m_commandResponsive = false;
    desk->m_completedCommandSequence = 0;
    desk->m_cancelledCommandSequence = 0;
    desk->m_executingCommandSequence = 0;
    desk->m_poisonedThroughCommandSequence = 0;
    desk->m_thread   = new Thread([this, desk]() { desk_thread(desk); });
    if (!waitForDeskStartup(desk, kDeskStartupTimeout)) {
        LOG((CLOG_WARN "Windows input desktop thread startup timed out: %s",
            name.empty() ? "<unavailable>" : name.c_str()));
    }
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
    }

    for (Desks::iterator index = m_desks.begin();
                            index != m_desks.end(); ++index) {
        Desk* desk = index->second;
        PostThreadMessage(desk->m_threadID, WM_QUIT, 0, 0);
        desk->m_thread->wait();
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

    // if active desktop changed then tell the old and new desk threads
    // about the change.  don't switch desktops when the screensaver is
    // active because we'd most likely switch to the screensaver desktop
    // which would have the side effect of forcing the screensaver to
    // stop.
    if (name != activeDeskName && !m_screensaver->isActive()) {
        // show cursor on previous desk
        if (!wasOnScreen) {
            sendMessage(BARRIER_MSG_ENTER, 0, 0);
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
        sendMessage(BARRIER_MSG_SWITCH, 0, 0);

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
            sendMessage(BARRIER_MSG_LEAVE, reinterpret_cast<WPARAM>(keyLayout), 0);
        }

        // update keys if necessary
        if (syncKeys) {
            updateKeys();
        }
    }
    else if (name != activeDeskName) {
        // screen saver might have started
        PostThreadMessage(m_threadID, BARRIER_MSG_SCREEN_SAVER, TRUE, 0);
    }
    else if (!m_screensaver->isActive() && !isDeskReady(desk) &&
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

bool
MSWindowsDesks::waitForDeskStartup(const Desk* desk, double timeout) const
{
    Stopwatch timer;
    Lock lock(&m_mutex);
    while (!desk->m_startupComplete) {
        if (!m_deskReady.wait(timer, timeout)) {
            break;
        }
    }
    return desk->m_startupComplete;
}

bool
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
        const bool signalled = timeout < 0.0 ?
            m_deskReady.wait() : m_deskReady.wait(timer, timeout);
        if (!signalled) {
            break;
        }
    }
    return isDeskCommandCompleteForTest(
        sequence, desk->m_completedCommandSequence, desk->m_threadRunning);
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
MSWindowsDesks::openInputDesktop()
{
    return OpenInputDesktop(
        DF_ALLOWOTHERACCOUNTHOOK, TRUE,
        DESKTOP_CREATEWINDOW | DESKTOP_HOOKCONTROL | GENERIC_WRITE);
}

void
MSWindowsDesks::closeDesktop(HDESK desk)
{
    if (desk != NULL) {
        CloseDesktop(desk);
    }
}

std::string MSWindowsDesks::getDesktopName(HDESK desk)
{
    if (desk == NULL) {
        return {};
    }
    else {
        DWORD size;
        GetUserObjectInformation(desk, UOI_NAME, NULL, 0, &size);
        TCHAR* name = (TCHAR*)alloca(size + sizeof(TCHAR));
        GetUserObjectInformation(desk, UOI_NAME, name, size, &size);
        std::string result(name);
        return result;
    }
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
