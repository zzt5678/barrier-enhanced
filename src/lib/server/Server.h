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

#pragma once

#include "server/Config.h"
#include "barrier/clipboard_types.h"
#include "barrier/Clipboard.h"
#include "barrier/key_types.h"
#include "barrier/mouse_types.h"
#include "barrier/INode.h"
#include "barrier/DragInformation.h"
#include "barrier/ServerArgs.h"
#include "base/Event.h"
#include "base/Stopwatch.h"
#include "base/EventTypes.h"
#include "common/stdmap.h"
#include "common/stdset.h"
#include "common/stdvector.h"
#include "io/filesystem.h"

#include <deque>
#include <memory>
#include <vector>

class BaseClientProxy;
class EventQueueTimer;
class PrimaryClient;
class InputFilter;
class StreamChunker;
namespace barrier {
class IStream;
class Screen;
}
class IEventQueue;
class Thread;
class ClientListener;

//! Barrier server
/*!
This class implements the top-level server algorithms for barrier.
*/
class Server : public INode {
public:
	class FileClipboardReadyInfo : public EventData {
	public:
		FileClipboardReadyInfo() : m_publishClipboard(false) { }

		std::string m_sessionId;
		std::vector<std::string> m_paths;
		bool m_publishClipboard;
	};

    //! Lock cursor to screen data
    class LockCursorToScreenInfo {
    public:
        enum State { kOff, kOn, kToggle };

        static LockCursorToScreenInfo* alloc(State state = kToggle);

    public:
        State            m_state;
    };

    //! Switch to screen data
    class SwitchToScreenInfo {
    public:
        static SwitchToScreenInfo* alloc(const std::string& screen);

    public:
        // this is a C-string;  this type is a variable size structure
        char            m_screen[1];
    };

    //! Switch in direction data
    class SwitchInDirectionInfo {
    public:
        static SwitchInDirectionInfo* alloc(EDirection direction);

    public:
        EDirection        m_direction;
    };

    //! Screen connected data
    class ScreenConnectedInfo : public EventData {
    public:
        ScreenConnectedInfo(std::string screen) : m_screen(screen) { }

    public:
        std::string m_screen;
    };

    //! Keyboard broadcast data
    class KeyboardBroadcastInfo {
    public:
        enum State { kOff, kOn, kToggle };

        static KeyboardBroadcastInfo* alloc(State state = kToggle);
        static KeyboardBroadcastInfo* alloc(State state,
                                            const std::string& screens);

    public:
        State            m_state;
        char            m_screens[1];
    };

    /*!
    Start the server with the configuration \p config and the primary
    client (local screen) \p primaryClient.  The client retains
    ownership of \p primaryClient.
    */
    Server(Config& config, PrimaryClient* primaryClient,
        barrier::Screen* screen, IEventQueue* events, ServerArgs const& args);
    ~Server();

#ifdef BARRIER_TEST_ENV
    Server() :
        m_mock(true),
        m_primaryClient(NULL),
        m_active(NULL),
        m_seqNum(0),
        m_x(0),
        m_y(0),
        m_xDelta(0),
        m_yDelta(0),
        m_xDelta2(0),
        m_yDelta2(0),
        m_config(NULL),
        m_inputFilter(NULL),
        m_activeSaver(NULL),
        m_xSaver(0),
        m_ySaver(0),
        m_switchDir(kNoDirection),
        m_switchScreen(NULL),
        m_recentSwitchGuardActive(false),
        m_recentSwitchGuardLogged(false),
        m_recentSwitchReverseDir(kNoDirection),
        m_recentSwitchEntryX(0),
        m_recentSwitchEntryY(0),
        m_primaryReturnAnchorActive(false),
        m_primaryReturnAnchorX(0),
        m_primaryReturnAnchorY(0),
        m_switchWaitDelay(0.0),
        m_switchWaitTimer(NULL),
        m_primaryKeyStateTimer(NULL),
        m_mouseMoveTimer(NULL),
        m_pendingMouseMoveTarget(NULL),
        m_pendingMouseMove(false),
        m_mouseMoveSent(false),
        m_pendingMouseX(0),
        m_pendingMouseY(0),
        m_switchWaitX(0),
        m_switchWaitY(0),
        m_switchTwoTapDelay(0.0),
        m_switchTwoTapEngaged(false),
        m_switchTwoTapArmed(false),
        m_switchTwoTapZone(3),
        m_switchNeedsShift(false),
        m_switchNeedsControl(false),
        m_switchNeedsAlt(false),
        m_relativeMoves(false),
        m_keyboardBroadcasting(false),
        m_lockedToScreen(false),
        m_screen(NULL),
        m_events(NULL),
        m_expectedFileSize(0),
        m_sendFileThread(NULL),
        m_sendFileTarget(NULL),
        m_sendFileTransferId(0),
        m_sendFileCompletionPending(false),
        m_sendFileIsClipboardPrefetch(false),
        m_writeToDropDirThread(NULL),
        m_ignoreFileTransfer(false),
        m_enableClipboard(false),
        m_localShortcutMode(false),
        m_lowLatencyMode(false),
        m_nestedRemoteMode(false),
        m_primaryLeaveFailedRecently(false),
        m_primaryLeaveFailureTimer(true),
        m_clientListener(NULL)
    { }
    void setActive(BaseClientProxy* active) {    m_active = active; }
    void setFileTransferForTest(BaseClientProxy* target, UInt32 transferId)
    {
        m_sendFileTarget = target;
        m_sendFileTransferId = transferId;
        m_sendFileCompletionPending = false;
    }
    static void addDefaultConnectionOptionsForTest(OptionsList& optionsList)
    {
        addDefaultConnectionOptions(optionsList);
    }
#endif

    //! @name manipulators
    //@{

    //! Set configuration
    /*!
    Change the server's configuration.  Returns true iff the new
    configuration was accepted (it must include the server's name).
    This will disconnect any clients no longer in the configuration.
    */
    bool                setConfig(const Config&);

    //! Add a client
    /*!
    Adds \p client to the server.  The client is adopted and will be
    destroyed when the client disconnects or is disconnected.
    */
    void                adoptClient(BaseClientProxy* client);

    //! Disconnect clients
    /*!
    Disconnect clients.  This tells them to disconnect but does not wait
    for them to actually do so.  The server sends the disconnected event
    when they're all disconnected (or immediately if none are connected).
    The caller can also just destroy this object to force the disconnection.
    */
    void                disconnect();

    //! Create a new thread and use it to send file to client
    void                sendFileToClient(const std::string& filename);

    //! Received dragging information from client
    void dragInfoReceived(UInt32 fileNum, std::string content);

    //! Store ClientListener pointer
    void                setListener(ClientListener* p) { m_clientListener = p; }

    //@}
    //! @name accessors
    //@{

    //! Get number of connected clients
    /*!
    Returns the number of connected clients, including the server itself.
    */
    UInt32                getNumClients() const;

    //! Get the list of connected clients
    /*!
    Set the \c list to the names of the currently connected clients.
    */
    void getClients(std::vector<std::string>& list) const;

    //! Return true if received file size is valid
    bool                isReceivedFileSizeValid();

    //! Return expected file data size
    size_t&                getExpectedFileSize() { return m_expectedFileSize; }

    //! Return received file data
    std::string& getReceivedFileData() { return m_receivedFileData; }

    //! Return received file spool path when large payload is kept on disk
    barrier::fs::path& getReceivedFileSpoolPath() { return m_receivedFileSpoolPath; }

    //! Return fake drag file list
    DragFileList        getFakeDragFileList() { return m_fakeDragFileList; }

    //@}

private:
    struct CompletedFileTransfer {
        size_t expectedSize;
		std::string data;
		barrier::fs::path spoolPath;
		std::string dropTarget;
		DragFileList dragFileList;
		std::string remoteFileClipboardSession;

        CompletedFileTransfer() : expectedSize(0) { }
    };

    // get canonical name of client
    std::string getName(const BaseClientProxy*) const;

    // get the sides of the primary screen that have neighbors
    UInt32                getActivePrimarySides() const;

    // returns true iff mouse should be locked to the current screen
    // according to this object only, ignoring what the primary client
    // says.
    bool                isLockedToScreenServer() const;

    // returns true iff mouse should be locked to the current screen
    // according to this object or the primary client.
    bool                isLockedToScreen() const;

    // returns true iff the destination screen may be entered now.
    bool                canEnterScreen(BaseClientProxy*) const;

    // returns the jump zone of the client
    SInt32                getJumpZoneSize(BaseClientProxy*) const;

    // change the active screen
    bool                switchScreen(BaseClientProxy*,
	                            SInt32 x, SInt32 y, bool forScreenSaver,
                            EDirection guardDir = kNoDirection);

    // jump to screen
    void                jumpToScreen(BaseClientProxy*);

    // convert pixel position to fraction, using x or y depending on the
    // direction.
    float                mapToFraction(BaseClientProxy*, EDirection,
                            SInt32 x, SInt32 y) const;

    // convert fraction to pixel position, writing only x or y depending
    // on the direction.
    void                mapToPixel(BaseClientProxy*, EDirection, float f,
                            SInt32& x, SInt32& y) const;

    // returns true if the client has a neighbor anywhere along the edge
    // indicated by the direction.
    bool                hasAnyNeighbor(BaseClientProxy*, EDirection) const;

    // lookup neighboring screen, mapping the coordinate independent of
    // the direction to the neighbor's coordinate space.
    BaseClientProxy*    getNeighbor(BaseClientProxy*, EDirection,
                            SInt32& x, SInt32& y) const;

    // lookup neighboring screen.  given a position relative to the
    // source screen, find the screen we should move onto and where.
    // if the position is sufficiently far from the source then we
    // cross multiple screens.  if there is no suitable screen then
    // return NULL and x,y are not modified.
    BaseClientProxy*    mapToNeighbor(BaseClientProxy*, EDirection,
                            SInt32& x, SInt32& y) const;

    // adjusts x and y or neither to avoid ending up in a jump zone
    // after entering the client in the given direction.
    void                avoidJumpZone(BaseClientProxy*, EDirection,
                            SInt32& x, SInt32& y) const;

    // test if a switch is permitted.  this includes testing user
    // options like switch delay and tracking any state required to
    // implement them.  returns true iff a switch is permitted.
    bool                isSwitchOkay(BaseClientProxy* dst, EDirection,
                            SInt32 x, SInt32 y, SInt32 xActive, SInt32 yActive);
    void                armRecentSwitchGuard(BaseClientProxy* from,
                            BaseClientProxy* to, EDirection dir);
    void                clearRecentSwitchGuardIfMovedAway();
    bool                isRecentReverseSwitch(BaseClientProxy* dst,
                            EDirection dir);
    void                rememberPrimaryReturnAnchor(BaseClientProxy* dst,
                            SInt32 x, SInt32 y);
    void                adjustPrimaryReturnPoint(BaseClientProxy* src,
                            SInt32& x, SInt32& y);

    // update switch state due to a mouse move at \p x, \p y that
    // doesn't switch screens.
    void                noSwitch(SInt32 x, SInt32 y);

    // stop switch timers
    void                stopSwitch();

    // start two tap switch timer
    void                startSwitchTwoTap();

    // arm the two tap switch timer if \p x, \p y is outside the tap zone
    void                armSwitchTwoTap(SInt32 x, SInt32 y);

    // stop the two tap switch timer
    void                stopSwitchTwoTap();

    // returns true iff the two tap switch timer is started
    bool                isSwitchTwoTapStarted() const;

    // returns true iff should switch because of two tap
    bool                shouldSwitchTwoTap() const;

    // start delay switch timer
    void                startSwitchWait(SInt32 x, SInt32 y);

    // stop delay switch timer
    void                stopSwitchWait();

    // returns true iff the delay switch timer is started
    bool                isSwitchWaitStarted() const;

    // returns the corner (EScreenSwitchCornerMasks) where x,y is on the
    // given client.  corners have the given size.
    UInt32                getCorner(BaseClientProxy*,
                            SInt32 x, SInt32 y, SInt32 size) const;

    // stop relative mouse moves
    void                stopRelativeMoves();

    // send screen options to \c client
    void                sendOptions(BaseClientProxy* client) const;
    static void         addDefaultConnectionOptions(OptionsList& optionsList);

    // process options from configuration
    void                processOptions();

    // event handlers
    void                handleShapeChanged(const Event&, void*);
    void                handleClipboardGrabbed(const Event&, void*);
    void                handleClipboardChanged(const Event&, void*);
    void                handleKeyDownEvent(const Event&, void*);
    void                handleKeyUpEvent(const Event&, void*);
    void                handleKeyRepeatEvent(const Event&, void*);
    void                handleButtonDownEvent(const Event&, void*);
    void                handleButtonUpEvent(const Event&, void*);
    void                handleMotionPrimaryEvent(const Event&, void*);
    void                handleMotionSecondaryEvent(const Event&, void*);
    void                handleWheelEvent(const Event&, void*);
    void                handleScreensaverActivatedEvent(const Event&, void*);
    void                handleScreensaverDeactivatedEvent(const Event&, void*);
    void                handleSwitchWaitTimeout(const Event&, void*);
    void                handlePrimaryKeyStateSync(const Event&, void*);
    void                handleMouseMoveFlush(const Event&, void*);
    void                handleClientDisconnected(const Event&, void*);
    void                handleClientCloseTimeout(const Event&, void*);
    void                handleSwitchToScreenEvent(const Event&, void*);
    void                handleToggleScreenEvent(const Event&, void*);
    void                handleSwitchInDirectionEvent(const Event&, void*);
    void                handleKeyboardBroadcastEvent(const Event&,void*);
    void                handleLockCursorToScreenEvent(const Event&, void*);
    void                handleFakeInputBeginEvent(const Event&, void*);
    void                handleFakeInputEndEvent(const Event&, void*);
    void                handleFileChunkSendingEvent(const Event&, void*);
    void                handleFileRecieveCompletedEvent(const Event&, void*);
    void                handleFileClipboardReadyEvent(const Event&, void*);
    void                handleDropDirWriteFinishedEvent(const Event&, void*);
    void                handleFileKeepAliveEvent(const Event&, void*);

    // event processing
    bool                canLeavePrimaryNow(const char* reason);
    void                recoverToPrimaryFromActive(const char* reason);
    void                recoverPrimaryAfterSwitchFailure(SInt32 x, SInt32 y);
    void                reanchorActiveAfterFailedSwitch(BaseClientProxy* dst);
    void                fetchPendingPrimaryClipboards();
    void                replayClipboardsToActive();
    bool                onClipboardChanged(BaseClientProxy* sender,
                            ClipboardID id, UInt32 seqNum);
    void                onScreensaver(bool activated);
    void                onKeyDown(KeyID, KeyModifierMask, KeyButton,
                            const char* screens);
    void                onKeyUp(KeyID, KeyModifierMask, KeyButton,
                            const char* screens);
    void                onKeyRepeat(KeyID, KeyModifierMask, SInt32, KeyButton);
    void                onMouseDown(ButtonID);
    void                onMouseUp(ButtonID);
    bool                onMouseMovePrimary(SInt32 x, SInt32 y);
    void                onMouseMoveSecondary(SInt32 dx, SInt32 dy);
    void                onMouseWheel(SInt32 xDelta, SInt32 yDelta);
    void                queueMouseMove(BaseClientProxy*, SInt32 x, SInt32 y);
    void                flushPendingMouseMove();
    void                discardPendingMouseMove(BaseClientProxy* target = NULL);
    void                onFileChunkSending(const void* data);
    void                onFileRecieveCompleted();
    void                publishMaterializedFileClipboard(const std::vector<std::string>& paths,
                                                         const std::string& sessionId);
    void                sendClipboardSelectionToClient(BaseClientProxy* target,
                                                       const std::vector<barrier::fs::path>& sourcePaths);

    // add client to list and attach event handlers for client
    bool                addClient(BaseClientProxy*);

    // remove client from list and detach event handlers for client
    bool                removeClient(BaseClientProxy*);

    // close a client
    void                closeClient(BaseClientProxy*, const char* msg,
                            bool forceLeave = true);

    // close clients not in \p config
    void                closeClients(const Config& config);

    // close all clients whether they've completed the handshake or not,
    // except the primary client
    void                closeAllClients();

    // remove clients from internal state
    void                removeActiveClient(BaseClientProxy*);
    void                removeOldClient(BaseClientProxy*);

    // force the cursor off of \p client
    void                forceLeaveClient(BaseClientProxy* client);

    // thread function for sending file
    void                send_file_thread(barrier::IStream* stream,
                                         const std::string& filename,
                                         const std::shared_ptr<StreamChunker>& chunker,
                                         UInt32 transferId);
	void                send_clipboard_file_thread(barrier::IStream* stream,
                                                   const std::vector<barrier::fs::path>& sourcePaths,
                                                   const std::shared_ptr<StreamChunker>& chunker,
                                                   UInt32 transferId);
	bool                cleanupSendFileThread(bool cancel);
	bool                reapSendFileThreadIfReady();
	bool                finishCompletedSendFileIfReady();
	bool                cleanupWriteToDropDirThread();
	bool                reapWriteToDropDirThreadIfReady();
	bool                deferDeleteIfSendingToClient(BaseClientProxy* client);
	bool                deleteClientIfReady(BaseClientProxy* client);
	bool                clientReadyForDelete(BaseClientProxy* client);
	void                deleteDeferredClient(BaseClientProxy* client);
	void                deleteDeferredClients();
	std::shared_ptr<CompletedFileTransfer> takeCompletedFileTransfer();
	void                startDropDirTransfer(std::shared_ptr<CompletedFileTransfer> transfer);
	void                queueDropDirTransfer(std::shared_ptr<CompletedFileTransfer> transfer);
	void                drainDropDirTransferQueue();
	void                releasePendingDropDirTransfers();

	// thread function for writing file to drop directory
	void write_to_drop_dir_thread(std::shared_ptr<CompletedFileTransfer> transfer);

    // send drag info to new client screen
    void                sendDragInfo(BaseClientProxy* newScreen);

public:
	bool                m_mock;
#ifdef BARRIER_TEST_ENV
	std::shared_ptr<CompletedFileTransfer> testTakeCompletedFileTransfer()
	{
		return takeCompletedFileTransfer();
	}
	void testWriteRemoteClipboardTransfer(const std::string& data,
	                                      const std::string& sessionId)
	{
		std::shared_ptr<CompletedFileTransfer> transfer(new CompletedFileTransfer());
		transfer->expectedSize = data.size();
		transfer->data = data;
		transfer->remoteFileClipboardSession = sessionId;
		write_to_drop_dir_thread(transfer);
	}
	void testQueueDropDirTransfer(const std::string& data)
	{
		std::shared_ptr<CompletedFileTransfer> transfer(new CompletedFileTransfer());
		transfer->expectedSize = data.size();
		transfer->data = data;
		transfer->dropTarget = barrier::fs::temp_directory_path().u8string();
		queueDropDirTransfer(transfer);
	}
	bool testCleanupWriteToDropDirThread() { return cleanupWriteToDropDirThread(); }
	bool testHasWriteToDropDirThread() const { return m_writeToDropDirThread != NULL; }
	void testSetWriteToDropDirThread(Thread* thread) { m_writeToDropDirThread = thread; }
	void testDrainDropDirTransferQueue() { drainDropDirTransferQueue(); }
	std::size_t testPendingDropDirTransferCount() const { return m_pendingDropDirTransfers.size(); }
	bool testCleanupSendFileThread(bool cancel) { return cleanupSendFileThread(cancel); }
#endif
private:
    class ClipboardInfo {
    public:
        ClipboardInfo();

    public:
        Clipboard        m_clipboard;
        ClipboardDataSnapshot m_clipboardData;
        std::string m_clipboardOwner;
        UInt32            m_clipboardSeqNum;
        bool              m_pendingPrimaryFetch;
    };

    // the primary screen client
    PrimaryClient*        m_primaryClient;

    // all clients (including the primary client) indexed by name
    typedef std::map<std::string, BaseClientProxy*> ClientList;
    typedef std::set<BaseClientProxy*> ClientSet;
    ClientList            m_clients;
    ClientSet            m_clientSet;

    // all old connections that we're waiting to hangup
    typedef std::map<BaseClientProxy*, EventQueueTimer*> OldClients;
    OldClients            m_oldClients;

    // the client with focus
    BaseClientProxy*    m_active;

    // the sequence number of enter messages
    UInt32                m_seqNum;

    // current mouse position (in absolute screen coordinates) on
    // whichever screen is active
    SInt32                m_x, m_y;

    // last mouse deltas.  this is needed to smooth out double tap
    // on win32 which reports bogus mouse motion at the edge of
    // the screen when using low level hooks, synthesizing motion
    // in the opposite direction the mouse actually moved.
    SInt32                m_xDelta, m_yDelta;
    SInt32                m_xDelta2, m_yDelta2;

    // current configuration
    Config*                m_config;

    // input filter (from m_config);
    InputFilter*        m_inputFilter;

    // clipboard cache
    ClipboardInfo        m_clipboards[kClipboardEnd];

    // state saved when screen saver activates
    BaseClientProxy*    m_activeSaver;
    SInt32                m_xSaver, m_ySaver;

    // common state for screen switch tests.  all tests are always
    // trying to reach the same screen in the same direction.
    EDirection            m_switchDir;
    BaseClientProxy*    m_switchScreen;
    bool                m_recentSwitchGuardActive;
    bool                m_recentSwitchGuardLogged;
    Stopwatch           m_recentSwitchGuardTimer;
    std::string         m_recentSwitchFromName;
    std::string         m_recentSwitchToName;
    EDirection          m_recentSwitchReverseDir;
    SInt32              m_recentSwitchEntryX;
    SInt32              m_recentSwitchEntryY;
    bool                m_primaryReturnAnchorActive;
    std::string         m_primaryReturnAnchorClientName;
    SInt32              m_primaryReturnAnchorX;
    SInt32              m_primaryReturnAnchorY;

    // state for delayed screen switching
    double                m_switchWaitDelay;
    EventQueueTimer*    m_switchWaitTimer;
    EventQueueTimer*    m_primaryKeyStateTimer;
    EventQueueTimer*    m_mouseMoveTimer;
    BaseClientProxy*    m_pendingMouseMoveTarget;
    bool                m_pendingMouseMove;
    bool                m_mouseMoveSent;
    SInt32              m_pendingMouseX;
    SInt32              m_pendingMouseY;
    Stopwatch           m_mouseMoveRateTimer;
    SInt32                m_switchWaitX, m_switchWaitY;

    // state for double-tap screen switching
    double                m_switchTwoTapDelay;
    Stopwatch            m_switchTwoTapTimer;
    bool                m_switchTwoTapEngaged;
    bool                m_switchTwoTapArmed;
    SInt32                m_switchTwoTapZone;

    // modifiers needed before switching
    bool                m_switchNeedsShift;
    bool                m_switchNeedsControl;
    bool                m_switchNeedsAlt;

    // relative mouse move option
    bool                m_relativeMoves;

    // flag whether or not we have broadcasting enabled and the screens to
    // which we should send broadcasted keys.
    bool                m_keyboardBroadcasting;
    std::string m_keyboardBroadcastingScreens;

    // screen locking (former scroll lock)
    bool                m_lockedToScreen;

    // server screen
    barrier::Screen*    m_screen;

    IEventQueue*        m_events;

    // file transfer
    size_t                m_expectedFileSize;
    std::string m_receivedFileData;
    barrier::fs::path m_receivedFileSpoolPath;
    DragFileList        m_dragFileList;
    DragFileList        m_fakeDragFileList;
    Thread*                m_sendFileThread;
    std::shared_ptr<StreamChunker> m_sendFileChunker;
    BaseClientProxy*       m_sendFileTarget;
    UInt32                 m_sendFileTransferId;
    bool                   m_sendFileCompletionPending;
    bool                   m_sendFileIsClipboardPrefetch;
    ClientSet              m_deferredDeleteClients;
    Thread*                m_writeToDropDirThread;
	std::deque<std::shared_ptr<CompletedFileTransfer> > m_pendingDropDirTransfers;
    std::string         m_remoteFileClipboardSession;
    std::string         m_readyFileClipboardSession;
    std::vector<std::string> m_readyFileClipboardPaths;
    std::string m_dragFileExt;
    bool                m_ignoreFileTransfer;
    bool                m_enableClipboard;

    // local shortcut mode - when enabled, shortcut keys are handled locally
    // when mouse is on primary screen, preventing conflicts with remote
    bool                m_localShortcutMode;

    // low latency mode - when enabled, reduces latency at cost of higher CPU usage
    // by disabling Nagle algorithm, larger buffers, and faster timeouts
    bool                m_lowLatencyMode;
    bool                m_nestedRemoteMode;
    bool                m_primaryLeaveFailedRecently;
    Stopwatch           m_primaryLeaveFailureTimer;

    ClientListener*        m_clientListener;
    ServerArgs            m_args;
};
