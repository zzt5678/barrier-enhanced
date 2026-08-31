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

#pragma once

#include "platform/synwinhk.h"
#include "barrier/key_types.h"
#include "barrier/mouse_types.h"
#include "barrier/option_types.h"
#include "mt/CondVar.h"
#include "mt/Mutex.h"
#include "common/stdmap.h"
#include <cstdint>
#include <functional>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>


class Event;
class EventQueueTimer;
class Thread;
class IScreenSaver;
class IEventQueue;

//! Microsoft Windows desk handling
/*!
Desks in Microsoft Windows are only remotely like desktops on X11
systems.  A desk is another virtual surface for windows but desks
impose serious restrictions:  a thread can interact with only one
desk at a time, you can't switch desks if the thread has any hooks
installed or owns any windows, windows cannot exist on multiple
desks at once, etc.  Basically, they're useless except for running
the login window or the screensaver, which is what they're used
for.  Barrier must deal with them mainly because of the login
window and screensaver but users can create their own desks and
barrier should work on those too.

This class encapsulates all the desk nastiness.  Clients of this
object don't have to know anything about desks.
*/
class MSWindowsDesks {
public:
    enum DeskInputCommand {
        kDeskInputKey,
        kDeskInputButton,
        kDeskInputAbsoluteMove,
        kDeskInputRelativeMove,
        kDeskInputWheel,
        kDeskControlEnter,
        kDeskControlLeave,
        kDeskControlSwitch,
        kDeskControlOther
    };

    enum PendingMotionBoundary {
        kNoPendingMotionBoundary,
        kFlushPendingMotion,
        kSupersedePendingMotion
    };

    enum DesktopTransitionAction {
        kKeepActiveDesktop,
        kWaitForObservedDesktop,
        kActivateObservedDesktop,
        kRecoverInputProcess
    };

    //! Constructor
    /*!
    \p isPrimary is true iff the desk is for a primary screen.
    \p screensaver points to a screensaver object and it's used
    only to check if the screensaver is active.  The \p updateKeys
    job is adopted and is called when the key state should be
    updated in a thread attached to the current desk.
    \p hookLibrary must be a handle to the hook library.
    */
    MSWindowsDesks(
        bool isPrimary, bool noHooks,
        const IScreenSaver* screensaver, IEventQueue* events,
        const std::function<void()>& updateKeys, bool stopOnDeskSwitch);
    ~MSWindowsDesks();

    //! @name manipulators
    //@{

    //! Enable desk tracking
    /*!
    Enables desk tracking.  While enabled, this object checks to see
    if the desk has changed and ensures that the hooks are installed
    on the new desk.  \c setShape should be called at least once
    before calling \c enable.
    */
    void                enable();

    //! Disable desk tracking
    /*!
    Disables desk tracking.  \sa enable.
    */
    void                disable();

    //! Notify of entering a desk
    /*!
    Prepares a desk for when the cursor enters it.
    */
    bool                enter();

    //! Notify of leaving a desk
    /*!
    Prepares a desk for when the cursor leaves it.
    */
    bool                leave(HKL keyLayout);

    //! Notify of options changes
    /*!
    Resets all options to their default values.
    */
    void                resetOptions();

    //! Notify of options changes
    /*!
    Set options to given values.  Ignores unknown options and doesn't
    modify options that aren't given in \c options.
    */
    void                setOptions(const OptionsList& options);

    //! Update the key state
    /*!
    Causes the key state to get updated to reflect the physical keyboard
    state and current keyboard mapping.
    */
    void                updateKeys();

    //! Tell desk about new size
    /*!
    This tells the desks that the display size has changed.
    */
    void                setShape(SInt32 x, SInt32 y,
                            SInt32 width, SInt32 height,
                            SInt32 xCenter, SInt32 yCenter, bool isMultimon);

    //! Install/uninstall screensaver hooks
    /*!
    If \p install is true then the screensaver hooks are installed and,
    if desk tracking is enabled, updated whenever the desk changes.  If
    \p install is false then the screensaver hooks are uninstalled.
    */
    void                installScreensaverHooks(bool install);

    //! Start ignoring user input
    /*!
    Starts ignoring user input so we don't pick up our own synthesized events.
    */
    void                fakeInputBegin();

    //! Stop ignoring user input
    /*!
    Undoes whatever \c fakeInputBegin() did.
    */
    void                fakeInputEnd();

    //@}
    //! @name accessors
    //@{

    //! Get cursor position
    /*!
    Return the current position of the cursor in \c x and \c y.
    */
    void                getCursorPos(SInt32& x, SInt32& y) const;

    //! Fake key press/release
    /*!
    Synthesize a press or release of key \c button.
    */
    void                fakeKeyEvent(KeyButton button, UINT virtualKey,
                            bool press, bool isAutoRepeat) const;

    //! Fake mouse press/release
    /*!
    Synthesize a press or release of mouse button \c id.
    */
    void                fakeMouseButton(ButtonID id, bool press);

    //! Fake mouse move
    /*!
    Synthesize a mouse move to the absolute coordinates \c x,y.
    */
    bool                fakeMouseMove(SInt32 x, SInt32 y,
                            bool waitForCompletion = true) const;

    //! Fake mouse move
    /*!
    Synthesize a mouse move to the relative coordinates \c dx,dy.
    */
    void                fakeMouseRelativeMove(SInt32 dx, SInt32 dy) const;

    //! Fake mouse wheel
    /*!
    Synthesize a mouse wheel event of amount \c delta in direction \c axis.
    */
    void                fakeMouseWheel(SInt32 xDelta, SInt32 yDelta) const;

    //! Return true when the active Windows desktop can process input.
    bool                canEnter() const;

    //! Probe access to the input desktop without starting a desk thread/hook.
    bool                probeInputDesktop(std::string& desktopName) const;

    //! Monotonically increasing identity for the active desktop backend.
    std::uint64_t       inputDesktopGeneration() const;

    //! Name of the active Windows input desktop.
    std::string         inputDesktopName() const;

    static bool         isDesktopReadyForTest(bool isPrimary, bool noHooks,
                            bool threadAttached, bool windowReady,
                            bool hookInstalled,
                            bool commandResponsive = true,
                            bool injectionProbeSucceeded = true);

    static bool         isDeskCommandCompleteForTest(
                            std::uint64_t expectedSequence,
                            std::uint64_t completedSequence,
                            bool threadRunning);

    static bool         shouldProcessDeskCommandForTest(
                            std::uint64_t sequence,
                            std::uint64_t cancelledThroughSequence);

    static bool         canCancelTimedOutDeskCommandForTest(
                            std::uint64_t sequence,
                            std::uint64_t executingSequence,
                            bool orderedInputCommand = false);

    static bool         commandCompletionProvesResponsiveForTest(
                            bool commandExecuted,
                            bool commandSucceeded,
                            std::uint64_t sequence,
                            std::uint64_t poisonedThroughSequence);

    static bool         commandInjectionSucceededForTest(
                            std::uint64_t sequence,
                            std::uint64_t failedSequence);

    static bool         canQueueDeskCommandForTest(
                            std::uint64_t nextSequence,
                            std::uint64_t completedSequence,
                            std::uint64_t maxPendingCommands);

    static std::uint64_t maxPendingDeskCommandsForTest();

    static bool         deskCommandWaitsForCompletionForTest(
                            DeskInputCommand command,
                            bool forceCompletion = false);

    static double        deskCommandAckTimeoutForTest(
                            bool lowLatencyMode,
                            bool nestedRemoteMode);

    static double        deskCommandExecutionGraceForTest(
                            bool lowLatencyMode,
                            bool nestedRemoteMode);

    static double        boundedDeskCommandWaitTimeoutForTest(
                            double timeout);

    static bool         canActivateDesktopForTest(
                            bool startupComplete,
                            bool threadRunning,
                            bool threadAttached,
                            bool windowReady);

    static bool         canAcceptInputForActiveDesktopForTest(
                            bool activeDesktopReady,
                            bool observedDesktopMatchesActive);

    static bool         hasDesktopStartupTimedOutForTest(
                            bool startupComplete,
                            std::uint64_t now,
                            std::uint64_t deadline);

    static bool         shouldPostDeskQuitForTest(
                            bool startupComplete,
                            DWORD threadID);

    static bool         coalesceMouseMotionForTest(
                            DeskInputCommand pendingCommand,
                            SInt32& pendingFirst,
                            SInt32& pendingSecond,
                            DeskInputCommand nextCommand,
                            SInt32 nextFirst,
                            SInt32 nextSecond);

    static PendingMotionBoundary pendingMotionBoundaryForTest(
                            DeskInputCommand command,
                            bool forceCompletion = false);

    static DesktopTransitionAction desktopTransitionActionForTest(
                            bool observedDesktopMatchesActive,
                            bool observedDesktopReady,
                            bool inputLeaseActive,
                            bool startupComplete,
                            std::uint64_t now,
                            std::uint64_t deadline);

    static bool         beginInputRecoveryForTest(bool& recoveryRequested);
    static DWORD        inputRecoveryHardExitDelayForTest();

    //@}

private:
    enum DeskCommandDispatch {
        kWaitForDeskCommand,
        kPostDeskCommand
    };

    enum DeskCommandWaitResult {
        kDeskCommandTimedOut,
        kDeskCommandSucceeded,
        kDeskCommandFailed
    };

    class DeskCommand {
    public:
        DeskCommand(UINT message, WPARAM wParam, LPARAM lParam,
                    std::uint64_t sequence);

        UINT            message;
        WPARAM          wParam;
        LPARAM          lParam;
        std::uint64_t   sequence;
    };

    class Desk {
    public:
        std::string m_name;
        Thread*        m_thread;
        DWORD            m_threadID;
        DWORD            m_targetID;
        HDESK            m_desk;
        HWND            m_window;
        HWND            m_foregroundWindow;
        bool            m_lowLevel;
        bool            m_threadAttached;
        bool            m_windowReady;
        bool            m_hookInstalled;
        bool            m_injectionProbeSucceeded;
        bool            m_startupComplete;
        std::uint64_t   m_startupDeadline;
        bool            m_shutdownRequested;
        bool            m_threadRunning;
        bool            m_commandResponsive;
        std::uint64_t   m_completedCommandSequence;
        std::uint64_t   m_cancelledCommandSequence;
        std::uint64_t   m_executingCommandSequence;
        std::uint64_t   m_poisonedThroughCommandSequence;
        std::uint64_t   m_failedCommandSequence;
        std::uint64_t   m_lastMotionCommandSequence;
        DeskCommand*    m_pendingMotionCommand;
    };

    struct DeskReadinessSnapshot {
        DeskReadinessSnapshot() :
            threadAttached(false),
            windowReady(false),
            hookInstalled(false),
            injectionProbeSucceeded(false),
            commandResponsive(false),
            ready(false)
        {
        }

        bool threadAttached;
        bool windowReady;
        bool hookInstalled;
        bool injectionProbeSucceeded;
        bool commandResponsive;
        bool ready;
    };
    typedef std::map<std::string, Desk*> Desks;

    // initialization and shutdown operations
    HCURSOR                createBlankCursor() const;
    void                destroyCursor(HCURSOR cursor) const;
    ATOM                createDeskWindowClass(bool isPrimary) const;
    void                destroyClass(ATOM windowClass) const;
    HWND                createWindow(ATOM windowClass, const char* name) const;
    void                destroyWindow(HWND) const;

    // message handlers
    bool                deskMouseMove(SInt32 x, SInt32 y) const;
    bool                deskMouseRelativeMove(SInt32 dx, SInt32 dy) const;
    void                deskEnter(Desk* desk);
    bool                deskLeave(Desk* desk, HKL keyLayout);
    void                updateDeskTimer(double interval);
    void                beginLowLatencyRelativeMoves();
    void                endLowLatencyRelativeMoves();
    void desk_thread(Desk* desk);

    // desk switch checking and handling
    Desk* addDesk(const std::string& name, HDESK hdesk);
    void                removeDesks();
    void                checkDesk();
    bool                isDeskAccessible(const Desk* desk) const;
    bool                isDeskReady(const Desk* desk) const;
    bool                isDeskReadyLocked(const Desk* desk) const;
    DeskReadinessSnapshot getDeskReadiness(const Desk* desk) const;
    void                handleCheckDesk(const Event& event, void*);

    // communication with desk threads
    DeskCommandWaitResult waitForDeskCommand(const Desk* desk,
                            std::uint64_t sequence, double timeout) const;
    bool                sendMessage(UINT, WPARAM, LPARAM,
                            DeskCommandDispatch dispatch =
                                kWaitForDeskCommand) const;
    static DeskInputCommand classifyDeskCommand(UINT msg);
    void                requestInputRecovery(UINT msg,
                            const char* reason, bool orderedCommand) const;

    // work around for messed up keyboard events from low-level hooks
    HWND                getForegroundWindow() const;

    // desk API wrappers
    HDESK                openInputDesktop() const;
    HDESK                openInputDesktopForProbe() const;
    void                 closeDesktop(HDESK) const;
    std::string getDesktopName(HDESK) const;

    // our desk window procs
    static LRESULT CALLBACK primaryDeskProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK secondaryDeskProc(HWND, UINT, WPARAM, LPARAM);

private:
    // true if screen is being used as a primary screen, false otherwise
    bool                m_isPrimary;

    // true if hooks are not to be installed (useful for debugging)
    bool                m_noHooks;

    // true if mouse has entered the screen
    bool                m_isOnScreen;

    // our resources
    ATOM                m_deskClass;
    HCURSOR                m_cursor;

    // screen shape stuff
    SInt32                m_x, m_y;
    SInt32                m_w, m_h;
    SInt32                m_xCenter, m_yCenter;

    // true if system appears to have multiple monitors
    bool                m_multimon;

    // the timer used to check for desktop switching
    EventQueueTimer*    m_timer;

    // screen saver stuff
    DWORD                m_threadID;
    const IScreenSaver*    m_screensaver;
    bool                m_screensaverNotify;

    // the current desk and it's name
    Desk*                m_activeDesk;
    std::string m_activeDeskName;
    std::string m_observedDeskName;

    // one desk per desktop and a cond var to communicate with it
    mutable Mutex        m_mutex;
    mutable Mutex        m_sendMutex;
    CondVar<bool>        m_deskReady;
    Desks                m_desks;
    mutable std::uint64_t m_inputDesktopGeneration;
    mutable std::uint64_t m_nextDeskCommandSequence;
    mutable POINT        m_cursorPos;
    mutable bool         m_inputRecoveryRequested;
    ULONGLONG            m_nextDeskRecoveryProbe;

    // keyboard stuff
    std::function<void()> m_updateKeys;
    HKL                    m_keyLayout;

    // options
    bool                m_leaveForegroundOption;
    bool                m_lowLatencyMode;
    bool                m_nestedRemoteMode;
    bool                m_relativeMoveAccelerationDisabled;
    int                 m_oldMouseAcceleration[4];
    double              m_deskPollInterval;

    IEventQueue*        m_events;

    // true if program should stop on desk switch.
    bool                m_stopOnDeskSwitch;
};
