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

#include "server/Server.h"

#include "server/ClientProxy.h"
#include "server/ClientProxy1_6.h"
#include "server/ClientProxyUnknown.h"
#include "server/PrimaryClient.h"
#include "server/ClientListener.h"
#include "barrier/FileChunk.h"
#include "barrier/IPlatformScreen.h"
#include "barrier/DropHelper.h"
#include "barrier/option_types.h"
#include "barrier/protocol_types.h"
#include "barrier/XScreen.h"
#include "barrier/XBarrier.h"
#include "barrier/StreamChunker.h"
#include "barrier/TransferArchive.h"
#include "barrier/KeyState.h"
#include "barrier/Screen.h"
#include "barrier/PacketStreamFilter.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/IClipboard.h"
#include "barrier/RemoteFileClipboard.h"
#include "net/TCPSocket.h"
#include "net/IDataSocket.h"
#include "net/IListenSocket.h"
#include "net/XSocket.h"
#include "mt/Thread.h"
#include "mt/XThread.h"
#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/TMethodEventJob.h"
#include "common/DataDirectories.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <memory>
#include <sstream>
#include <fstream>
#include <ctime>
#include <stdexcept>
#include <vector>

namespace {

const SInt32 kMinUsableScreenDimension = 64;
const SInt32 kSwitchEdgeHysteresisInset = 16;
const SInt32 kSwitchReverseClearDistance = 96;
const double kSwitchReverseGuardMaxSeconds = 2.0;
const int kClipboardReadAttempts = 8;
const double kClipboardReadRetrySeconds = 0.025;
const UInt32 kDefaultHeartbeatMilliseconds = 10000;
const double kMouseMoveIntervalSeconds = 1.0 / 240.0;
const size_t kMaxPendingDropDirTransfers = 4;
const size_t kMaxPendingDropDirTransferMemoryBytes = 32 * 1024 * 1024;

EDirection
oppositeDirection(EDirection dir)
{
	switch (dir) {
	case kLeft:
		return kRight;
	case kRight:
		return kLeft;
	case kTop:
		return kBottom;
	case kBottom:
		return kTop;
	case kNoDirection:
		break;
	}
	return kNoDirection;
}

bool prepareTransferSource(const char* filename,
                           barrier::fs::path& sourcePath,
                           barrier::fs::path& tempPackagePath,
                           std::string& error);

std::vector<barrier::fs::path> utf8PathsToFsPaths(const std::vector<std::string>& paths)
{
    std::vector<barrier::fs::path> result;
    result.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        result.push_back(barrier::fs::u8path(paths[i]));
    }
    return result;
}

float clampUnitFraction(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

bool clampToClientShape(BaseClientProxy* client, SInt32& x, SInt32& y)
{
    SInt32 sx, sy, sw, sh;
    client->getShape(sx, sy, sw, sh);

    if (sw < kMinUsableScreenDimension || sh < kMinUsableScreenDimension) {
        LOG((CLOG_WARN "ignoring unusable screen shape for \"%s\": %d,%d %dx%d",
            client->getName().c_str(), sx, sy, sw, sh));
        return false;
    }

    if (x < sx) {
        x = sx;
    }
    else if (x >= sx + sw) {
        x = sx + sw - 1;
    }

    if (y < sy) {
        y = sy;
    }
    else if (y >= sy + sh) {
        y = sy + sh - 1;
    }

    return true;
}

bool optionsListContains(const OptionsList& optionsList, OptionID option)
{
    for (UInt32 i = 0; i + 1 < optionsList.size(); i += 2) {
        if (optionsList[i] == option) {
            return true;
        }
    }
    return false;
}

bool hasValidClientShape(BaseClientProxy* client)
{
    SInt32 sx, sy, sw, sh;
    client->getShape(sx, sy, sw, sh);
    if (sw < kMinUsableScreenDimension || sh < kMinUsableScreenDimension) {
        LOG((CLOG_WARN "unusable screen shape for \"%s\": %d,%d %dx%d",
            client->getName().c_str(), sx, sy, sw, sh));
        return false;
    }
    return true;
}

bool readClipboardWithRetry(BaseClientProxy* sender, ClipboardID id, Clipboard& clipboard)
{
    for (int attempt = 0; attempt < kClipboardReadAttempts; ++attempt) {
        if (sender->getClipboard(id, &clipboard)) {
            if (attempt > 0) {
                LOG((CLOG_DEBUG "clipboard %d read from \"%s\" succeeded after %d retry attempt(s)",
                    id, sender->getName().c_str(), attempt));
            }
            return true;
        }

        ARCH->sleep(kClipboardReadRetrySeconds);
    }

    LOG((CLOG_WARN "clipboard %d could not be read from \"%s\" after %d attempts; preserving previous data",
        id, sender->getName().c_str(), kClipboardReadAttempts));
    return false;
}

bool isReturnToPrimaryHotKey(KeyID id, KeyModifierMask mask)
{
    const KeyModifierMask required =
        KeyModifierShift | KeyModifierControl | KeyModifierAlt;
    return id == kKeyPause && (mask & required) == required;
}

DragFileList parseDraggedPaths(const std::string& pathList)
{
    DragFileList dragFileList;
    std::istringstream input(pathList);
    std::string path;
    while (std::getline(input, path)) {
        if (path.empty()) {
            continue;
        }
        DragInformation di;
        di.setFilename(path);
        if (barrier::fs::is_directory(barrier::fs::u8path(path))) {
            di.setEntryType(DragInformation::Directory);
        }
        dragFileList.push_back(di);
    }

    if (dragFileList.empty() && !pathList.empty()) {
        DragInformation di;
        std::string singlePath = pathList;
        di.setFilename(singlePath);
        if (barrier::fs::is_directory(barrier::fs::u8path(pathList))) {
            di.setEntryType(DragInformation::Directory);
        }
        dragFileList.push_back(di);
    }

    return dragFileList;
}

}
//
// Server
//

Server::Server(
		Config& config,
		PrimaryClient* primaryClient,
		barrier::Screen* screen,
		IEventQueue* events,
		ServerArgs const& args) :
	m_mock(false),
		m_primaryClient(primaryClient),
		m_active(primaryClient),
		m_seqNum(0),
		m_x(0),
		m_y(0),
		m_xDelta(0),
		m_yDelta(0),
	m_xDelta2(0),
	m_yDelta2(0),
	m_config(&config),
	m_inputFilter(config.getInputFilter()),
	m_activeSaver(NULL),
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
		m_screen(screen),
		m_events(events),
		m_expectedFileSize(0),
		m_receivedFileData(),
		m_receivedFileSpoolPath(),
	m_sendFileThread(NULL),
	m_sendFileTarget(NULL),
	m_sendFileTransferId(0),
	m_sendFileCompletionPending(false),
	m_sendFileIsClipboardPrefetch(false),
	m_writeToDropDirThread(NULL),
	m_pendingDropDirTransfers(),
	m_remoteFileClipboardSession(),
	m_readyFileClipboardSession(),
	m_readyFileClipboardPaths(),
	m_ignoreFileTransfer(false),
	m_enableClipboard(true),
	m_localShortcutMode(false),
	m_lowLatencyMode(false),
	m_nestedRemoteMode(false),
	m_primaryLeaveFailedRecently(false),
	m_primaryLeaveFailureTimer(true),
	m_args(args)
{
	// must have a primary client and it must have a canonical name
	assert(m_primaryClient != NULL);
	assert(config.isScreen(primaryClient->getName()));
	assert(m_screen != NULL);

    std::string primaryName = getName(primaryClient);

	// clear clipboards
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		ClipboardInfo& clipboard   = m_clipboards[id];
		clipboard.m_clipboardOwner  = primaryName;
		clipboard.m_clipboardSeqNum = m_seqNum;
		if (clipboard.m_clipboard.open(0)) {
			clipboard.m_clipboard.empty();
			clipboard.m_clipboard.close();
		}
		clipboard.m_clipboardData.set(clipboard.m_clipboard.marshall());
	}

	// install event handlers
	m_events->adoptHandler(Event::kTimer, this,
							new TMethodEventJob<Server>(this,
								&Server::handleSwitchWaitTimeout));
	m_primaryKeyStateTimer = m_events->newTimer(2.0, NULL);
	m_events->adoptHandler(Event::kTimer, m_primaryKeyStateTimer,
							new TMethodEventJob<Server>(this,
								&Server::handlePrimaryKeyStateSync));
	m_events->adoptHandler(m_events->forIKeyState().keyDown(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyDownEvent));
	m_events->adoptHandler(m_events->forIKeyState().keyUp(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyUpEvent));
	m_events->adoptHandler(m_events->forIKeyState().keyRepeat(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyRepeatEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().buttonDown(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleButtonDownEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().buttonUp(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleButtonUpEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().motionOnPrimary(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleMotionPrimaryEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().motionOnSecondary(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleMotionSecondaryEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().wheel(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleWheelEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().screensaverActivated(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleScreensaverActivatedEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().screensaverDeactivated(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleScreensaverDeactivatedEvent));
	m_events->adoptHandler(m_events->forServer().switchToScreen(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleSwitchToScreenEvent));
  m_events->adoptHandler(m_events->forServer().toggleScreen(),
              m_inputFilter,
              new TMethodEventJob<Server>(this,
                &Server::handleToggleScreenEvent));
	m_events->adoptHandler(m_events->forServer().switchInDirection(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleSwitchInDirectionEvent));
	m_events->adoptHandler(m_events->forServer().keyboardBroadcast(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyboardBroadcastEvent));
	m_events->adoptHandler(m_events->forServer().lockCursorToScreen(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleLockCursorToScreenEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().fakeInputBegin(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleFakeInputBeginEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().fakeInputEnd(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleFakeInputEndEvent));

	m_events->adoptHandler(m_events->forFile().fileChunkSending(),
							this,
							new TMethodEventJob<Server>(this,
								&Server::handleFileChunkSendingEvent));
	m_events->adoptHandler(m_events->forFile().fileRecieveCompleted(),
							this,
							new TMethodEventJob<Server>(this,
								&Server::handleFileRecieveCompletedEvent));
	m_events->adoptHandler(m_events->forFile().fileClipboardReady(),
							this,
							new TMethodEventJob<Server>(this,
								&Server::handleFileClipboardReadyEvent));
	m_events->adoptHandler(m_events->forFile().dropDirWriteFinished(),
							this,
							new TMethodEventJob<Server>(this,
								&Server::handleDropDirWriteFinishedEvent));
	m_events->adoptHandler(m_events->forFile().keepAlive(),
							this,
							new TMethodEventJob<Server>(this,
								&Server::handleFileKeepAliveEvent));

	// add connection
	addClient(m_primaryClient);

	// set initial configuration
	setConfig(config);

	// enable primary client
	m_primaryClient->enable();
	m_inputFilter->setPrimaryClient(m_primaryClient);

	// Determine if scroll lock is already set. If so, lock the cursor to the primary screen
	if (m_primaryClient->getToggleMask() & KeyModifierScrollLock) {
		LOG((CLOG_NOTE "Scroll Lock is on, locking cursor to screen"));
		m_lockedToScreen = true;
	}
	if (m_args.m_gameMode) {
		m_lockedToScreen = true;
		LOG((CLOG_NOTE "game mode enabled - cursor starts locked to the primary screen"));
	}

}

Server::~Server()
{
	if (m_mock) {
		return;
	}

	discardPendingMouseMove();

	if (!cleanupSendFileThread(true) && m_sendFileThread != NULL) {
		LOG((CLOG_ERR "waiting for file sender before destroying server state"));
		m_sendFileThread->wait();
		delete m_sendFileThread;
		m_sendFileThread = NULL;
		m_sendFileChunker.reset();
		m_sendFileTarget = NULL;
	}
	if (!cleanupWriteToDropDirThread() && m_writeToDropDirThread != NULL) {
		LOG((CLOG_ERR "waiting for drop-dir writer before destroying server state"));
		m_writeToDropDirThread->wait();
		delete m_writeToDropDirThread;
		m_writeToDropDirThread = NULL;
	}
	deleteDeferredClients();
	FileChunk::releaseReceiveBuffer(m_receivedFileData, m_expectedFileSize, &m_receivedFileSpoolPath);
	m_events->removeHandler(m_events->forFile().fileChunkSending(), this);
	m_events->removeHandler(m_events->forFile().fileRecieveCompleted(), this);
	m_events->removeHandler(m_events->forFile().fileClipboardReady(), this);
	m_events->removeHandler(m_events->forFile().dropDirWriteFinished(), this);
	m_events->removeHandler(m_events->forFile().keepAlive(), this);
	releasePendingDropDirTransfers();

	// remove event handlers and timers
	m_events->removeHandler(m_events->forIKeyState().keyDown(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIKeyState().keyUp(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIKeyState().keyRepeat(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().buttonDown(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().buttonUp(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().motionOnPrimary(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().motionOnSecondary(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().wheel(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().screensaverActivated(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().screensaverDeactivated(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().fakeInputBegin(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().fakeInputEnd(),
							m_inputFilter);
	if (m_primaryKeyStateTimer != NULL) {
		m_events->removeHandler(Event::kTimer, m_primaryKeyStateTimer);
		m_events->deleteTimer(m_primaryKeyStateTimer);
		m_primaryKeyStateTimer = NULL;
	}
	m_events->removeHandler(Event::kTimer, this);
	stopSwitch();

	// force immediate disconnection of secondary clients
	disconnect();
	for (OldClients::iterator index = m_oldClients.begin();
							index != m_oldClients.end(); ++index) {
		BaseClientProxy* client = index->first;
		m_events->deleteTimer(index->second);
		m_events->removeHandler(Event::kTimer, client);
		m_events->removeHandler(m_events->forClientProxy().disconnected(), client);
		deleteClientIfReady(client);
	}
	deleteDeferredClients();

	// remove input filter
	m_inputFilter->setPrimaryClient(NULL);

	// disable and disconnect primary client
	m_primaryClient->disable();
	removeClient(m_primaryClient);
}

bool
Server::setConfig(const Config& config)
{
	// refuse configuration if it doesn't include the primary screen
	if (!config.isScreen(m_primaryClient->getName())) {
		return false;
	}

	// close clients that are connected but being dropped from the
	// configuration.
	closeClients(config);

	// cut over
	processOptions();

	// add ScrollLock as a hotkey to lock to the screen.  this was a
	// built-in feature in earlier releases and is now supported via
	// the user configurable hotkey mechanism.  if the user has already
	// registered ScrollLock for something else then that will win but
	// we will unfortunately generate a warning.  if the user has
	// configured a LockCursorToScreenAction then we don't add
	// ScrollLock as a hotkey.
	if (!m_config->hasLockToScreenAction()) {
		IPlatformScreen::KeyInfo* key =
			IPlatformScreen::KeyInfo::alloc(kKeyScrollLock, 0, 0, 0);
		InputFilter::Rule rule(new InputFilter::KeystrokeCondition(m_events, key));
		rule.adoptAction(new InputFilter::LockCursorToScreenAction(m_events), true);
		m_inputFilter->addFilterRule(rule);
	}

	// tell primary screen about reconfiguration
	m_primaryClient->reconfigure(getActivePrimarySides());

	// tell all (connected) clients about current options
	for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		BaseClientProxy* client = index->second;
		sendOptions(client);
	}

	return true;
}

void
Server::adoptClient(BaseClientProxy* client)
{
	assert(client != NULL);
	deleteDeferredClients();

	// watch for client disconnection
	m_events->adoptHandler(m_events->forClientProxy().disconnected(), client,
							new TMethodEventJob<Server>(this,
								&Server::handleClientDisconnected, client));

	// name must be in our configuration
	if (!m_config->isScreen(client->getName())) {
		LOG((CLOG_WARN "unrecognised client name \"%s\", check server config", client->getName().c_str()));
		closeClient(client, kMsgEUnknown);
		return;
	}

	// add client to client list
	ClientList::const_iterator existing = m_clients.find(getName(client));
	bool replaceActive = false;
	bool replaceActiveSaver = false;
	if (existing != m_clients.end()) {
		BaseClientProxy* oldClient = existing->second;
		replaceActive = (m_active == oldClient);
		replaceActiveSaver = (m_activeSaver == oldClient);
		if (m_switchScreen == oldClient) {
			stopSwitch();
		}
		LOG((CLOG_WARN "replacing existing client \"%s\" with a new connection",
			getName(client).c_str()));
		closeClient(oldClient, kMsgEBusy, false);
	}

	if (!addClient(client)) {
		// can only have one screen with a given name at any given time
		LOG((CLOG_WARN "a client with name \"%s\" is already connected", getName(client).c_str()));
		closeClient(client, kMsgEBusy);
		return;
	}
	if (replaceActive) {
		m_active = client;
	}
	if (replaceActiveSaver) {
		m_activeSaver = client;
	}
	LOG((CLOG_NOTE "client \"%s\" has connected", getName(client).c_str()));

	// send configuration options to client
	sendOptions(client);

	// activate screen saver on new client if active on the primary screen
	if (m_activeSaver != NULL) {
		client->screensaver(true);
	}

	if (replaceActive && m_activeSaver == NULL) {
		++m_seqNum;
		client->enter(m_x, m_y, m_seqNum,
			m_primaryClient->getToggleMask(), false);
		replayClipboardsToActive();

		Server::SwitchToScreenInfo* switchInfo =
			Server::SwitchToScreenInfo::alloc(client->getName());
		m_events->addEvent(Event(m_events->forServer().screenSwitched(),
			this, switchInfo));
	}

	// send notification
	Server::ScreenConnectedInfo* info =
		new Server::ScreenConnectedInfo(getName(client));
	Event connectedEvent(m_events->forServer().connected(),
					 m_primaryClient->getEventTarget(), info);
	connectedEvent.setDataObject(info);
	m_events->addEvent(connectedEvent);
}

void
Server::disconnect()
{
	// close all secondary clients
	if (m_clients.size() > 1 || !m_oldClients.empty()) {
		Config emptyConfig(m_events);
		closeClients(emptyConfig);
	}
	else {
		m_events->addEvent(Event(m_events->forServer().disconnected(), this));
	}
}

UInt32
Server::getNumClients() const
{
	return (SInt32)m_clients.size();
}

void
Server::getClients(std::vector<std::string>& list) const
{
	list.clear();
	for (ClientList::const_iterator index = m_clients.begin();
							index != m_clients.end(); ++index) {
		list.push_back(index->first);
	}
}

std::string Server::getName(const BaseClientProxy* client) const
{
    std::string name = m_config->getCanonicalName(client->getName());
	if (name.empty()) {
		name = client->getName();
	}
	return name;
}

UInt32
Server::getActivePrimarySides() const
{
	UInt32 sides = 0;
	if (!isLockedToScreenServer()) {
		if (hasAnyNeighbor(m_primaryClient, kLeft)) {
			sides |= kLeftMask;
		}
		if (hasAnyNeighbor(m_primaryClient, kRight)) {
			sides |= kRightMask;
		}
		if (hasAnyNeighbor(m_primaryClient, kTop)) {
			sides |= kTopMask;
		}
		if (hasAnyNeighbor(m_primaryClient, kBottom)) {
			sides |= kBottomMask;
		}
	}
	return sides;
}

bool
Server::isLockedToScreenServer() const
{
	// locked if scroll-lock is toggled on
	return m_lockedToScreen;
}

bool
Server::isLockedToScreen() const
{
	// locked if we say we're locked
	if (isLockedToScreenServer()) {
		return true;
	}

	// locked if primary says we're locked
	if (m_primaryClient->isLockedToScreen()) {
		return true;
	}

	// not locked
	return false;
}

bool
Server::canEnterScreen(BaseClientProxy* client) const
{
	if (client == m_primaryClient && m_active != m_primaryClient &&
		!m_primaryClient->canEnter()) {
		LOG((CLOG_WARN "refusing to enter primary screen \"%s\" because the local display is not available",
			getName(m_primaryClient).c_str()));
		return false;
	}

	return true;
}

SInt32
Server::getJumpZoneSize(BaseClientProxy* client) const
{
	if (client == m_primaryClient) {
		return m_primaryClient->getJumpZoneSize();
	}
	else {
		return 0;
	}
}

bool
Server::switchScreen(BaseClientProxy* dst,
					SInt32 x, SInt32 y, bool forScreensaver,
					EDirection guardDir)
{
	assert(dst != NULL);

	if (!canEnterScreen(dst)) {
		stopSwitch();
		return false;
	}

	if (dst == m_primaryClient && m_active != NULL &&
		m_active != m_primaryClient) {
		adjustPrimaryReturnPoint(m_active, x, y);
	}

	if (!clampToClientShape(dst, x, y)) {
		LOG((CLOG_WARN "refusing to switch to \"%s\" with unusable destination shape",
			getName(dst).c_str()));
		stopSwitch();
		return false;
	}

	if (m_active == m_primaryClient && dst != m_primaryClient &&
		!canLeavePrimaryNow("screen switch")) {
		return false;
	}

#ifndef NDEBUG
	{
		SInt32 dx, dy, dw, dh;
		dst->getShape(dx, dy, dw, dh);
		assert(x >= dx && y >= dy && x < dx + dw && y < dy + dh);
	}
#endif
	assert(m_active != NULL);

	LOG((CLOG_INFO "switch from \"%s\" to \"%s\" at %d,%d", getName(m_active).c_str(), getName(dst).c_str(), x, y));

	// stop waiting to switch
	stopSwitch();

	// wrapping means leaving the active screen and entering it again.
	// since that's a waste of time we skip that and just warp the
	// mouse.
	if (m_active != dst) {
		flushPendingMouseMove();
		BaseClientProxy* oldActive = m_active;
		const SInt32 oldX = m_x;
		const SInt32 oldY = m_y;
		if (oldActive == m_primaryClient && dst != m_primaryClient) {
			rememberPrimaryReturnAnchor(dst, oldX, oldY);
		}

			// leave active screen
			if (!m_active->leave()) {
				// cannot leave screen
				LOG((CLOG_WARN "can't leave screen"));
				m_x = oldX;
				m_y = oldY;
				m_xDelta = 0;
				m_yDelta = 0;
				m_xDelta2 = 0;
				m_yDelta2 = 0;
				if (oldActive == m_primaryClient) {
					recoverPrimaryAfterSwitchFailure(oldX, oldY);
				}
				return false;
			}

		m_primaryLeaveFailedRecently = false;
		if (oldActive == m_primaryClient) {
			m_screen->fakeAllKeysUp();
		}

		// record new position
		m_x       = x;
		m_y       = y;
		m_xDelta  = 0;
		m_yDelta  = 0;
		m_xDelta2 = 0;
		m_yDelta2 = 0;

		if (m_active == m_primaryClient && m_enableClipboard) {
			fetchPendingPrimaryClipboards();
		}

		// cut over
		m_active = dst;

		// increment enter sequence number
		++m_seqNum;

		// enter new screen
		m_active->enter(x, y, m_seqNum,
								m_primaryClient->getToggleMask(),
								forScreensaver);
		if (m_active == m_primaryClient) {
			m_primaryClient->refreshKeyState();
		}

		replayClipboardsToActive();

		Server::SwitchToScreenInfo* info =
			Server::SwitchToScreenInfo::alloc(m_active->getName());
		m_events->addEvent(Event(m_events->forServer().screenSwitched(), this, info));
		if (!forScreensaver && guardDir != kNoDirection) {
			armRecentSwitchGuard(oldActive, m_active, guardDir);
		}
		}
		else {
			m_x       = x;
			m_y       = y;
			m_xDelta  = 0;
			m_yDelta  = 0;
			m_xDelta2 = 0;
			m_yDelta2 = 0;
			flushPendingMouseMove();
			m_active->mouseMove(x, y);
		}

	return true;
}

bool
Server::canLeavePrimaryNow(const char* reason)
{
	if (m_active != m_primaryClient || !m_primaryLeaveFailedRecently) {
		return true;
	}

	if (m_primaryLeaveFailureTimer.getTime() < 2.0) {
		LOG((CLOG_WARN "suppressing %s while primary input recovery settles", reason));
		stopSwitch();
		return false;
	}

	m_primaryLeaveFailedRecently = false;
	return true;
}

void
Server::recoverPrimaryAfterSwitchFailure(SInt32 x, SInt32 y)
{
	SInt32 ax, ay, aw, ah;
	m_primaryClient->getShape(ax, ay, aw, ah);
	if (aw < kMinUsableScreenDimension || ah < kMinUsableScreenDimension) {
		LOG((CLOG_WARN "cannot reanchor primary after failed leave; unusable primary shape %d,%d %dx%d",
			ax, ay, aw, ah));
		m_primaryLeaveFailedRecently = true;
		m_primaryLeaveFailureTimer.reset();
		return;
	}

	const SInt32 zone = getJumpZoneSize(m_primaryClient);
	const SInt32 margin = std::max<SInt32>(zone + 8, 16);
	SInt32 safeX = x;
	SInt32 safeY = y;
	if (safeX < ax + margin || safeX >= ax + aw - margin) {
		safeX = ax + aw / 2;
	}
	if (safeY < ay + margin || safeY >= ay + ah - margin) {
		safeY = ay + ah / 2;
	}

	LOG((CLOG_WARN "reanchoring primary at %d,%d after failed leave", safeX, safeY));
	m_x = safeX;
	m_y = safeY;
	discardPendingMouseMove();
	m_primaryClient->mouseMove(m_x, m_y);
	m_primaryClient->refreshKeyState();
	noSwitch(m_x, m_y);
	m_primaryLeaveFailedRecently = true;
	m_primaryLeaveFailureTimer.reset();
}

void
Server::reanchorActiveAfterFailedSwitch(BaseClientProxy* dst)
{
	if (m_active == NULL) {
		return;
	}

	if (clampToClientShape(m_active, m_x, m_y)) {
		LOG((CLOG_WARN "reanchoring \"%s\" at %d,%d after failed switch to \"%s\"",
			getName(m_active).c_str(),
			m_x,
			m_y,
			dst != NULL ? getName(dst).c_str() : "unknown"));
		m_xDelta = 0;
		m_yDelta = 0;
		m_xDelta2 = 0;
		m_yDelta2 = 0;
		noSwitch(m_x, m_y);
		discardPendingMouseMove();
		m_active->mouseMove(m_x, m_y);
	}
}

void
Server::fetchPendingPrimaryClipboards()
{
    if (!m_enableClipboard) {
        return;
    }

    const std::string primaryName = getName(m_primaryClient);
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        ClipboardInfo& clipboard = m_clipboards[id];
        const bool pendingFetch = clipboard.m_clipboardOwner == primaryName &&
            clipboard.m_pendingPrimaryFetch;
        if (pendingFetch) {
            onClipboardChanged(m_primaryClient, id, clipboard.m_clipboardSeqNum);
            continue;
        }

        // X11 only reports SelectionClear after Weave has owned a selection.
        // Snapshot the regular clipboard on every primary leave so the first
        // local copy after startup is not missed. PRIMARY selection remains
        // event-driven to avoid unnecessary reads and switch latency.
        if (id == kClipboardClipboard) {
            const std::string previousOwner = clipboard.m_clipboardOwner;
            const bool previousPendingFetch = clipboard.m_pendingPrimaryFetch;

            Clipboard observedClipboard;
            if (!readClipboardWithRetry(m_primaryClient, id, observedClipboard)) {
                continue;
            }

            RemoteFileClipboard::Data observedFileClipboard;
            const bool materializedFileEcho =
                !m_readyFileClipboardSession.empty() &&
                RemoteFileClipboard::normalizeClipboard(
                    observedClipboard, &observedFileClipboard) &&
                observedFileClipboard.mode ==
                    RemoteFileClipboard::Mode::SourcePaths &&
                RemoteFileClipboard::pathsMatch(
                    observedFileClipboard, m_readyFileClipboardPaths);
            if (materializedFileEcho) {
                LOG((CLOG_INFO
                    "suppressed remote file clipboard echo: session=%s items=%lu",
                    m_readyFileClipboardSession.c_str(),
                    static_cast<unsigned long>(m_readyFileClipboardPaths.size())));
                clipboard.m_clipboard = observedClipboard;
                clipboard.m_clipboardData.set(observedClipboard.marshall());
                clipboard.m_pendingPrimaryFetch = false;
                m_readyFileClipboardSession.clear();
                m_readyFileClipboardPaths.clear();
                continue;
            }

            const std::string observedData = observedClipboard.marshall();
            const bool observedEmpty = observedData.size() == sizeof(UInt32);
            if (clipboard.m_clipboardData.matches(observedData) ||
                (previousOwner != primaryName && observedEmpty)) {
                continue;
            }

            clipboard.m_clipboardOwner = primaryName;
            clipboard.m_pendingPrimaryFetch = true;
            if (!onClipboardChanged(m_primaryClient, id, clipboard.m_clipboardSeqNum)) {
                clipboard.m_clipboardOwner = previousOwner;
                clipboard.m_pendingPrimaryFetch = previousPendingFetch;
            }
        }
    }
}

void
Server::replayClipboardsToActive()
{
    if (!m_enableClipboard || m_active == NULL) {
        return;
    }

    const std::string activeName = getName(m_active);
    const std::string primaryName = getName(m_primaryClient);
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        ClipboardInfo& clipboard = m_clipboards[id];
        if (id == kClipboardClipboard) {
            RemoteFileClipboard::Data remoteFileClipboard;
            if (RemoteFileClipboard::readFromClipboard(clipboard.m_clipboard, remoteFileClipboard) &&
                remoteFileClipboard.mode == RemoteFileClipboard::Mode::SourcePaths) {
                if (clipboard.m_clipboardOwner == primaryName &&
                    m_active != m_primaryClient) {
                    m_active->setClipboard(id, &clipboard.m_clipboard);
                    sendClipboardSelectionToClient(
                        m_active, remoteFileClipboard.paths);
                }
                else {
                    LOG((CLOG_INFO
                        "not replaying source-path file clipboard from \"%s\" directly to \"%s\"",
                        clipboard.m_clipboardOwner.c_str(),
                        activeName.c_str()));
                }
                continue;
            }
        }

        m_active->setClipboard(id, &clipboard.m_clipboard);
    }
}

void
Server::recoverToPrimaryFromActive(const char* reason)
{
	if (m_active == m_primaryClient) {
		return;
	}

	LOG((CLOG_WARN "recovering to primary after %s while \"%s\" was active",
		reason, getName(m_active).c_str()));
	stopSwitch();

	SInt32 x, y;
	m_primaryClient->getCursorCenter(x, y);
	if (clampToClientShape(m_primaryClient, x, y)) {
		if (!switchScreen(m_primaryClient, x, y, false)) {
			reanchorActiveAfterFailedSwitch(m_primaryClient);
		}
	}
	else {
		LOG((CLOG_WARN "primary shape is not valid; notifying active client to leave before local fallback"));
		m_active->leave();
		forceLeaveClient(m_active);
	}

	m_primaryLeaveFailedRecently = true;
	m_primaryLeaveFailureTimer.reset();
}

void
Server::jumpToScreen(BaseClientProxy* newScreen)
{
	assert(newScreen != NULL);

	// record the current cursor position on the active screen
	m_active->setJumpCursorPos(m_x, m_y);

	// get the last cursor position on the target screen
	SInt32 x, y;
	newScreen->getJumpCursorPos(x, y);

	if (!switchScreen(newScreen, x, y, false)) {
		reanchorActiveAfterFailedSwitch(newScreen);
	}
}

float
Server::mapToFraction(BaseClientProxy* client,
				EDirection dir, SInt32 x, SInt32 y) const
{
	SInt32 sx, sy, sw, sh;
	client->getShape(sx, sy, sw, sh);
	switch (dir) {
	case kLeft:
	case kRight:
		return clampUnitFraction(static_cast<float>(y - sy + 0.5f) /
			static_cast<float>(sh));

	case kTop:
	case kBottom:
		return clampUnitFraction(static_cast<float>(x - sx + 0.5f) /
			static_cast<float>(sw));

	case kNoDirection:
		assert(0 && "bad direction");
		break;
	}
	return 0.0f;
}

void
Server::mapToPixel(BaseClientProxy* client,
				EDirection dir, float f, SInt32& x, SInt32& y) const
{
	SInt32 sx, sy, sw, sh;
	client->getShape(sx, sy, sw, sh);
	switch (dir) {
	case kLeft:
	case kRight:
		y = static_cast<SInt32>(f * sh) + sy;
		if (y < sy) {
			y = sy;
		}
		else if (y >= sy + sh) {
			y = sy + sh - 1;
		}
		break;

	case kTop:
	case kBottom:
		x = static_cast<SInt32>(f * sw) + sx;
		if (x < sx) {
			x = sx;
		}
		else if (x >= sx + sw) {
			x = sx + sw - 1;
		}
		break;

	case kNoDirection:
		assert(0 && "bad direction");
		break;
	}
}

bool
Server::hasAnyNeighbor(BaseClientProxy* client, EDirection dir) const
{
	assert(client != NULL);

	return m_config->hasNeighbor(getName(client), dir);
}

BaseClientProxy*
Server::getNeighbor(BaseClientProxy* src,
				EDirection dir, SInt32& x, SInt32& y) const
{
	// note -- must be locked on entry

	assert(src != NULL);

	// get source screen name
    std::string srcName = getName(src);
	assert(!srcName.empty());
	LOG((CLOG_DEBUG2 "find neighbor on %s of \"%s\"", Config::dirName(dir), srcName.c_str()));

	// convert position to fraction
	float t = mapToFraction(src, dir, x, y);

	// search for the closest neighbor that exists in direction dir
	float tTmp;
	for (;;) {
        std::string dstName(m_config->getNeighbor(srcName, dir, t, &tTmp));

		// if nothing in that direction then return NULL. if the
		// destination is the source then we can make no more
		// progress in this direction.  since we haven't found a
		// connected neighbor we return NULL.
		if (dstName.empty()) {
			LOG((CLOG_DEBUG2 "no neighbor on %s of \"%s\"", Config::dirName(dir), srcName.c_str()));
			return NULL;
		}

		// look up neighbor cell.  if the screen is connected and
		// ready then we can stop.
		ClientList::const_iterator index = m_clients.find(dstName);
		if (index != m_clients.end()) {
			if (!hasValidClientShape(index->second)) {
				LOG((CLOG_WARN "ignoring neighbor \"%s\" with unusable screen shape",
					dstName.c_str()));
				return NULL;
			}
			LOG((CLOG_DEBUG2 "\"%s\" is on %s of \"%s\" at %f", dstName.c_str(), Config::dirName(dir), srcName.c_str(), t));
			mapToPixel(index->second, dir, tTmp, x, y);
			return index->second;
		}

		// skip over unconnected screen
		LOG((CLOG_DEBUG2 "ignored \"%s\" on %s of \"%s\"", dstName.c_str(), Config::dirName(dir), srcName.c_str()));
		srcName = dstName;

		// use position on skipped screen
		t = tTmp;
	}
}

BaseClientProxy*
Server::mapToNeighbor(BaseClientProxy* src,
				EDirection srcSide, SInt32& x, SInt32& y) const
{
	// note -- must be locked on entry

	assert(src != NULL);

	// get the first neighbor
	BaseClientProxy* dst = getNeighbor(src, srcSide, x, y);
	if (dst == NULL) {
		return NULL;
	}

	// get the source screen's size
	SInt32 dx, dy, dw, dh;
	BaseClientProxy* lastGoodScreen = src;
	lastGoodScreen->getShape(dx, dy, dw, dh);

	// find destination screen, adjusting x or y (but not both).  the
	// searches are done in a sort of canonical screen space where
	// the upper-left corner is 0,0 for each screen.  we adjust from
	// actual to canonical position on entry to and from canonical to
	// actual on exit from the search.
	switch (srcSide) {
	case kLeft:
		x -= dx;
		while (dst != NULL) {
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			x += dw;
			if (x >= 0) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide, x, y);
		}
		assert(lastGoodScreen != NULL);
		x += dx;
		break;

	case kRight:
		x -= dx;
		while (dst != NULL) {
			x -= dw;
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			if (x < dw) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide, x, y);
		}
		assert(lastGoodScreen != NULL);
		x += dx;
		break;

	case kTop:
		y -= dy;
		while (dst != NULL) {
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			y += dh;
			if (y >= 0) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide, x, y);
		}
		assert(lastGoodScreen != NULL);
		y += dy;
		break;

	case kBottom:
		y -= dy;
		while (dst != NULL) {
			y -= dh;
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			if (y < dh) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide, x, y);
		}
		assert(lastGoodScreen != NULL);
		y += dy;
		break;

	case kNoDirection:
		assert(0 && "bad direction");
		return NULL;
	}

	// save destination screen
	assert(lastGoodScreen != NULL);
	dst = lastGoodScreen;

	// Move in far enough to avoid the jump zone.  If entering a side
	// that doesn't have a neighbor (i.e. an asymmetrical side) then we
	// don't need to move inwards because that side can't provoke a jump.
	avoidJumpZone(dst, srcSide, x, y);

	return dst;
}

void
Server::avoidJumpZone(BaseClientProxy* dst,
				EDirection dir, SInt32& x, SInt32& y) const
{
	SInt32 dx, dy, dw, dh;
	dst->getShape(dx, dy, dw, dh);
	if (dw < kMinUsableScreenDimension || dh < kMinUsableScreenDimension) {
		return;
	}
	const SInt32 maxInset =
		(std::max)(static_cast<SInt32>(1),
				   static_cast<SInt32>((std::min)(dw, dh) / 8));
	SInt32 z = (std::min)(kSwitchEdgeHysteresisInset, maxInset);
	z = (std::max)(z, getJumpZoneSize(dst));

	// move in far enough to avoid the jump zone.  if entering a side
	// that doesn't have a neighbor (i.e. an asymmetrical side) then we
	// don't need to move inwards because that side can't provoke a jump.
	SInt32 neighborX = x;
	SInt32 neighborY = y;
	switch (dir) {
	case kLeft:
		if (getNeighbor(dst, kRight, neighborX, neighborY) != NULL &&
			x > dx + dw - 1 - z) {
			x = dx + dw - 1 - z;
		}
		break;

	case kRight:
		if (getNeighbor(dst, kLeft, neighborX, neighborY) != NULL &&
			x < dx + z) {
			x = dx + z;
		}
		break;

	case kTop:
		if (getNeighbor(dst, kBottom, neighborX, neighborY) != NULL &&
			y > dy + dh - 1 - z) {
			y = dy + dh - 1 - z;
		}
		break;

	case kBottom:
		if (getNeighbor(dst, kTop, neighborX, neighborY) != NULL &&
			y < dy + z) {
			y = dy + z;
		}
		break;

	case kNoDirection:
		assert(0 && "bad direction");
	}
}

void
Server::armRecentSwitchGuard(BaseClientProxy* from, BaseClientProxy* to,
				EDirection dir)
{
	if (from == NULL || to == NULL || from == to) {
		m_recentSwitchGuardActive = false;
		return;
	}

	m_recentSwitchGuardActive = true;
	m_recentSwitchGuardLogged = false;
	m_recentSwitchFromName = getName(from);
	m_recentSwitchToName = getName(to);
	m_recentSwitchReverseDir = oppositeDirection(dir);
	m_recentSwitchEntryX = m_x;
	m_recentSwitchEntryY = m_y;
	m_recentSwitchGuardTimer.reset();
	LOG((CLOG_INFO "armed reverse switch guard from \"%s\" to \"%s\" reverse=%s entry=%d,%d",
		m_recentSwitchFromName.c_str(),
		m_recentSwitchToName.c_str(),
		Config::dirName(m_recentSwitchReverseDir),
		m_recentSwitchEntryX,
		m_recentSwitchEntryY));
}

void
Server::clearRecentSwitchGuardIfMovedAway()
{
	if (!m_recentSwitchGuardActive || m_active == NULL ||
		getName(m_active) != m_recentSwitchToName) {
		return;
	}

	switch (m_recentSwitchReverseDir) {
	case kLeft:
		if (m_x >= m_recentSwitchEntryX + kSwitchReverseClearDistance) {
			m_recentSwitchGuardActive = false;
		}
		break;

	case kRight:
		if (m_x <= m_recentSwitchEntryX - kSwitchReverseClearDistance) {
			m_recentSwitchGuardActive = false;
		}
		break;

	case kTop:
		if (m_y >= m_recentSwitchEntryY + kSwitchReverseClearDistance) {
			m_recentSwitchGuardActive = false;
		}
		break;

	case kBottom:
		if (m_y <= m_recentSwitchEntryY - kSwitchReverseClearDistance) {
			m_recentSwitchGuardActive = false;
		}
		break;

	case kNoDirection:
		m_recentSwitchGuardActive = false;
		break;
	}
}

bool
Server::isRecentReverseSwitch(BaseClientProxy* dst, EDirection dir)
{
	if (!m_recentSwitchGuardActive || m_active == NULL || dst == NULL) {
		return false;
	}
	if (m_recentSwitchGuardTimer.getTime() > kSwitchReverseGuardMaxSeconds) {
		m_recentSwitchGuardActive = false;
		return false;
	}
	if (getName(m_active) != m_recentSwitchToName ||
		getName(dst) != m_recentSwitchFromName) {
		return false;
	}

	return dir == m_recentSwitchReverseDir;
}

void
Server::rememberPrimaryReturnAnchor(BaseClientProxy* dst, SInt32 x, SInt32 y)
{
	if (dst == NULL) {
		m_primaryReturnAnchorActive = false;
		m_primaryReturnAnchorClientName.clear();
		return;
	}

	m_primaryReturnAnchorActive = true;
	m_primaryReturnAnchorClientName = getName(dst);
	m_primaryReturnAnchorX = x;
	m_primaryReturnAnchorY = y;
	LOG((CLOG_INFO "remembered primary return anchor for \"%s\" at %d,%d",
		m_primaryReturnAnchorClientName.c_str(), x, y));
}

void
Server::adjustPrimaryReturnPoint(BaseClientProxy* src, SInt32& x, SInt32& y)
{
	if (!m_primaryReturnAnchorActive || src == NULL ||
		getName(src) != m_primaryReturnAnchorClientName ||
		m_screen == NULL || m_screen->getPlatformScreen() == NULL) {
		return;
	}

	const SInt32 originalX = x;
	const SInt32 originalY = y;
	if (m_screen->getPlatformScreen()->adjustPointToVisibleAreaNearAnchor(
			m_primaryReturnAnchorX, m_primaryReturnAnchorY, x, y)) {
		if (x != originalX || y != originalY) {
			LOG((CLOG_INFO "anchored primary return from \"%s\" at %d,%d to %d,%d using primary exit %d,%d",
				getName(src).c_str(), originalX, originalY, x, y,
				m_primaryReturnAnchorX, m_primaryReturnAnchorY));
		}
	}
}

bool
Server::isSwitchOkay(BaseClientProxy* newScreen,
				EDirection dir, SInt32 x, SInt32 y,
				SInt32 xActive, SInt32 yActive)
{
	LOG((CLOG_DEBUG1 "try to leave \"%s\" on %s", getName(m_active).c_str(), Config::dirName(dir)));

	if (!canLeavePrimaryNow("edge switch")) {
		return false;
	}

	// is there a neighbor?
	if (newScreen == NULL) {
		// there's no neighbor.  we don't want to switch and we don't
		// want to try to switch later.
		LOG((CLOG_DEBUG1 "no neighbor %s", Config::dirName(dir)));
		stopSwitch();
		return false;
	}

	if (isRecentReverseSwitch(newScreen, dir)) {
		if (!m_recentSwitchGuardLogged) {
			LOG((CLOG_INFO "suppressing immediate reverse switch from \"%s\" to \"%s\" on %s briefly after screen entry",
				getName(m_active).c_str(),
				getName(newScreen).c_str(),
				Config::dirName(dir)));
			m_recentSwitchGuardLogged = true;
		}
		stopSwitch();
		return false;
	}

	// should we switch or not?
	bool preventSwitch = false;
	bool allowSwitch   = false;

	// note if the switch direction has changed.  save the new
	// direction and screen if so.
	bool isNewDirection  = (dir != m_switchDir);
	if (isNewDirection || m_switchScreen == NULL) {
		m_switchDir    = dir;
		m_switchScreen = newScreen;
	}

	// is this a double tap and do we care?
	if (!allowSwitch && m_switchTwoTapDelay > 0.0) {
		if (isNewDirection ||
			!isSwitchTwoTapStarted() || !shouldSwitchTwoTap()) {
			// tapping a different or new edge or second tap not
			// fast enough.  prepare for second tap.
			preventSwitch = true;
			startSwitchTwoTap();
		}
		else {
			// got second tap
			allowSwitch = true;
		}
	}

	// if waiting before a switch then prepare to switch later
	if (!allowSwitch && m_switchWaitDelay > 0.0) {
		if (isNewDirection || !isSwitchWaitStarted()) {
			startSwitchWait(x, y);
		}
		preventSwitch = true;
	}

	// are we in a locked corner?  first check if screen has the option set
	// and, if not, check the global options.
	const Config::ScreenOptions* options =
						m_config->getOptions(getName(m_active));
	if (options == NULL || options->count(kOptionScreenSwitchCorners) == 0) {
		options = m_config->getOptions("");
	}
	if (options != NULL && options->count(kOptionScreenSwitchCorners) > 0) {
		// get corner mask and size
		Config::ScreenOptions::const_iterator i =
			options->find(kOptionScreenSwitchCorners);
		UInt32 corners = static_cast<UInt32>(i->second);
		i = options->find(kOptionScreenSwitchCornerSize);
		SInt32 size = 0;
		if (i != options->end()) {
			size = i->second;
		}

		// see if we're in a locked corner
		if ((getCorner(m_active, xActive, yActive, size) & corners) != 0) {
			// yep, no switching
			LOG((CLOG_DEBUG1 "locked in corner"));
			preventSwitch = true;
			stopSwitch();
		}
	}

	// ignore if mouse is locked to screen and don't try to switch later
	if (!preventSwitch && isLockedToScreen()) {
		LOG((CLOG_DEBUG1 "locked to screen"));
		preventSwitch = true;
		stopSwitch();
	}

	// check for optional needed modifiers
	KeyModifierMask mods = this->m_primaryClient->getToggleMask();

	if (!preventSwitch && (
			(this->m_switchNeedsShift && ((mods & KeyModifierShift) != KeyModifierShift)) ||
			(this->m_switchNeedsControl && ((mods & KeyModifierControl) != KeyModifierControl)) ||
			(this->m_switchNeedsAlt && ((mods & KeyModifierAlt) != KeyModifierAlt))
		)) {
		LOG((CLOG_DEBUG1 "need modifiers to switch"));
		preventSwitch = true;
		stopSwitch();
	}

	return !preventSwitch;
}

void
Server::noSwitch(SInt32 x, SInt32 y)
{
	armSwitchTwoTap(x, y);
	stopSwitchWait();
}

void
Server::stopSwitch()
{
	if (m_switchScreen != NULL) {
		m_switchScreen = NULL;
		m_switchDir    = kNoDirection;
		stopSwitchTwoTap();
		stopSwitchWait();
	}
}

void
Server::startSwitchTwoTap()
{
	m_switchTwoTapEngaged = true;
	m_switchTwoTapArmed   = false;
	m_switchTwoTapTimer.reset();
	LOG((CLOG_DEBUG1 "waiting for second tap"));
}

void
Server::armSwitchTwoTap(SInt32 x, SInt32 y)
{
	if (m_switchTwoTapEngaged) {
		if (m_switchTwoTapTimer.getTime() > m_switchTwoTapDelay) {
			// second tap took too long.  disengage.
			stopSwitchTwoTap();
		}
		else if (!m_switchTwoTapArmed) {
			// still time for a double tap.  see if we left the tap
			// zone and, if so, arm the two tap.
			SInt32 ax, ay, aw, ah;
			m_active->getShape(ax, ay, aw, ah);
			SInt32 tapZone = m_primaryClient->getJumpZoneSize();
			if (tapZone < m_switchTwoTapZone) {
				tapZone = m_switchTwoTapZone;
			}
			if (x >= ax + tapZone && x < ax + aw - tapZone &&
				y >= ay + tapZone && y < ay + ah - tapZone) {
				// win32 can generate bogus mouse events that appear to
				// move in the opposite direction that the mouse actually
				// moved.  try to ignore that crap here.
				switch (m_switchDir) {
				case kLeft:
					m_switchTwoTapArmed = (m_xDelta > 0 && m_xDelta2 > 0);
					break;

				case kRight:
					m_switchTwoTapArmed = (m_xDelta < 0 && m_xDelta2 < 0);
					break;

				case kTop:
					m_switchTwoTapArmed = (m_yDelta > 0 && m_yDelta2 > 0);
					break;

				case kBottom:
					m_switchTwoTapArmed = (m_yDelta < 0 && m_yDelta2 < 0);
					break;

				default:
					break;
				}
			}
		}
	}
}

void
Server::stopSwitchTwoTap()
{
	m_switchTwoTapEngaged = false;
	m_switchTwoTapArmed   = false;
}

bool
Server::isSwitchTwoTapStarted() const
{
	return m_switchTwoTapEngaged;
}

bool
Server::shouldSwitchTwoTap() const
{
	// this is the second tap if two-tap is armed and this tap
	// came fast enough
	return (m_switchTwoTapArmed &&
			m_switchTwoTapTimer.getTime() <= m_switchTwoTapDelay);
}

void
Server::startSwitchWait(SInt32 x, SInt32 y)
{
	stopSwitchWait();
	m_switchWaitX     = x;
	m_switchWaitY     = y;
	m_switchWaitTimer = m_events->newOneShotTimer(m_switchWaitDelay, this);
	LOG((CLOG_DEBUG1 "waiting to switch"));
}

void
Server::stopSwitchWait()
{
	if (m_switchWaitTimer != NULL) {
		m_events->deleteTimer(m_switchWaitTimer);
		m_switchWaitTimer = NULL;
	}
}

bool
Server::isSwitchWaitStarted() const
{
	return (m_switchWaitTimer != NULL);
}

UInt32
Server::getCorner(BaseClientProxy* client,
				SInt32 x, SInt32 y, SInt32 size) const
{
	assert(client != NULL);

	// get client screen shape
	SInt32 ax, ay, aw, ah;
	client->getShape(ax, ay, aw, ah);

	// check for x,y on the left or right
	SInt32 xSide;
	if (x <= ax) {
		xSide = -1;
	}
	else if (x >= ax + aw - 1) {
		xSide = 1;
	}
	else {
		xSide = 0;
	}

	// check for x,y on the top or bottom
	SInt32 ySide;
	if (y <= ay) {
		ySide = -1;
	}
	else if (y >= ay + ah - 1) {
		ySide = 1;
	}
	else {
		ySide = 0;
	}

	// if against the left or right then check if y is within size
	if (xSide != 0) {
		if (y < ay + size) {
			return (xSide < 0) ? kTopLeftMask : kTopRightMask;
		}
		else if (y >= ay + ah - size) {
			return (xSide < 0) ? kBottomLeftMask : kBottomRightMask;
		}
	}

	// if against the left or right then check if y is within size
	if (ySide != 0) {
		if (x < ax + size) {
			return (ySide < 0) ? kTopLeftMask : kBottomLeftMask;
		}
		else if (x >= ax + aw - size) {
			return (ySide < 0) ? kTopRightMask : kBottomRightMask;
		}
	}

	return kNoCornerMask;
}

void
Server::stopRelativeMoves()
{
	if (m_relativeMoves && m_active != m_primaryClient) {
		// warp to the center of the active client so we know where we are
		SInt32 ax, ay, aw, ah;
		m_active->getShape(ax, ay, aw, ah);
		m_x       = ax + (aw >> 1);
		m_y       = ay + (ah >> 1);
		m_xDelta  = 0;
		m_yDelta  = 0;
		m_xDelta2 = 0;
		m_yDelta2 = 0;
		LOG((CLOG_DEBUG2 "synchronize move on %s by %d,%d", getName(m_active).c_str(), m_x, m_y));
		discardPendingMouseMove();
		m_active->mouseMove(m_x, m_y);
	}
}

void
Server::addDefaultConnectionOptions(OptionsList& optionsList)
{
	if (!optionsListContains(optionsList, kOptionHeartbeat)) {
		optionsList.push_back(kOptionHeartbeat);
		optionsList.push_back(kDefaultHeartbeatMilliseconds);
	}
}

void
Server::sendOptions(BaseClientProxy* client) const
{
	OptionsList optionsList;

	// look up options for client
	const Config::ScreenOptions* options =
						m_config->getOptions(getName(client));
	if (options != NULL) {
		// convert options to a more convenient form for sending
		optionsList.reserve(2 * options->size());
		for (Config::ScreenOptions::const_iterator index = options->begin();
									index != options->end(); ++index) {
			optionsList.push_back(index->first);
			optionsList.push_back(static_cast<UInt32>(index->second));
		}
	}

	// look up global options
	options = m_config->getOptions("");
	if (options != NULL) {
		// convert options to a more convenient form for sending
		optionsList.reserve(optionsList.size() + 2 * options->size());
		for (Config::ScreenOptions::const_iterator index = options->begin();
									index != options->end(); ++index) {
			optionsList.push_back(index->first);
			optionsList.push_back(static_cast<UInt32>(index->second));
		}
	}

	addDefaultConnectionOptions(optionsList);

	if (m_args.m_gameMode) {
		optionsList.push_back(kOptionRelativeMouseMoves);
		optionsList.push_back(1);
		optionsList.push_back(kOptionLocalShortcutMode);
		optionsList.push_back(1);
		optionsList.push_back(kOptionLowLatencyMode);
		optionsList.push_back(1);
	}
	if (m_args.m_lowLatencyMode || m_args.m_nestedRemoteMode) {
		optionsList.push_back(kOptionLowLatencyMode);
		optionsList.push_back(1);
	}
	if (m_args.m_nestedRemoteMode) {
		optionsList.push_back(kOptionRelativeMouseMoves);
		optionsList.push_back(1);
		optionsList.push_back(kOptionNestedRemoteMode);
		optionsList.push_back(1);
	}

	// send the options
	client->resetOptions();
	client->setOptions(optionsList);
}

void
Server::processOptions()
{
	const Config::ScreenOptions* options = m_config->getOptions("");

	m_switchNeedsShift = false;		// it seems if I don't add these
	m_switchNeedsControl = false;	// lines, the 'reload config' option
	m_switchNeedsAlt = false;		// doesn't work correct.
	m_enableClipboard = true;
	m_localShortcutMode = false;
	m_lowLatencyMode = false;
	m_nestedRemoteMode = false;

	bool newRelativeMoves = m_relativeMoves;
	if (options != NULL) {
		for (Config::ScreenOptions::const_iterator index = options->begin();
									index != options->end(); ++index) {
			const OptionID id       = index->first;
			const OptionValue value = index->second;
			if (id == kOptionScreenSwitchDelay) {
				m_switchWaitDelay = 1.0e-3 * static_cast<double>(value);
				if (m_switchWaitDelay < 0.0) {
					m_switchWaitDelay = 0.0;
				}
				stopSwitchWait();
			}
			else if (id == kOptionScreenSwitchTwoTap) {
				m_switchTwoTapDelay = 1.0e-3 * static_cast<double>(value);
				if (m_switchTwoTapDelay < 0.0) {
					m_switchTwoTapDelay = 0.0;
				}
				stopSwitchTwoTap();
			}
			else if (id == kOptionScreenSwitchNeedsControl) {
				m_switchNeedsControl = (value != 0);
			}
			else if (id == kOptionScreenSwitchNeedsShift) {
				m_switchNeedsShift = (value != 0);
			}
			else if (id == kOptionScreenSwitchNeedsAlt) {
				m_switchNeedsAlt = (value != 0);
			}
			else if (id == kOptionRelativeMouseMoves) {
				newRelativeMoves = (value != 0);
			}
			else if (id == kOptionClipboardSharing) {
				m_enableClipboard = (value != 0);

				if (m_enableClipboard == false) {
					LOG((CLOG_NOTE "clipboard sharing is disabled"));
				}
			}
			else if (id == kOptionLocalShortcutMode) {
				m_localShortcutMode = (value != 0);

				if (m_localShortcutMode) {
					LOG((CLOG_NOTE "local shortcut mode enabled - shortcuts will be handled locally when mouse is on primary screen"));
				}
			}
			else if (id == kOptionLowLatencyMode) {
				m_lowLatencyMode = (value != 0);

				if (m_lowLatencyMode) {
					LOG((CLOG_NOTE "low latency mode enabled - reduced latency at cost of higher CPU usage"));
				}
			}
			else if (id == kOptionNestedRemoteMode) {
				m_nestedRemoteMode = (value != 0);

				if (m_nestedRemoteMode) {
					LOG((CLOG_NOTE "nested remote mode enabled - favoring relative mouse delivery and remote-control compatibility"));
				}
			}
		}
	}
	if (m_relativeMoves && !newRelativeMoves) {
		stopRelativeMoves();
	}
	m_relativeMoves = newRelativeMoves;

	if (m_args.m_gameMode) {
		m_relativeMoves = true;
		m_localShortcutMode = true;
		m_lowLatencyMode = true;
		LOG((CLOG_NOTE "game mode enabled - low latency, local shortcuts, and relative mouse moves are forced on"));
	}
	if (m_args.m_lowLatencyMode) {
		m_lowLatencyMode = true;
		LOG((CLOG_NOTE "low latency mode enabled from command line"));
	}
	if (m_args.m_nestedRemoteMode) {
		m_nestedRemoteMode = true;
		m_relativeMoves = true;
		m_lowLatencyMode = true;
		LOG((CLOG_NOTE "nested remote mode enabled - relative mouse moves and low latency are forced on"));
	}
}

void
Server::handleShapeChanged(const Event&, void* vclient)
{
	// ignore events from unknown clients
	BaseClientProxy* client = static_cast<BaseClientProxy*>(vclient);
	if (m_clientSet.count(client) == 0) {
		return;
	}

	LOG((CLOG_DEBUG "screen \"%s\" shape changed", getName(client).c_str()));

	if (!hasValidClientShape(client)) {
		if (client == m_primaryClient && m_active != m_primaryClient) {
			recoverToPrimaryFromActive("invalid primary screen shape");
		}
		else if (client == m_active && client != m_primaryClient) {
			recoverToPrimaryFromActive("invalid active client screen shape");
		}
		return;
	}

	// update jump coordinate
	SInt32 x, y;
	if (client == m_active) {
		x = m_x;
		y = m_y;
	}
	else {
		client->getCursorPos(x, y);
	}
	if (!clampToClientShape(client, x, y)) {
		return;
	}
	client->setJumpCursorPos(x, y);

	// update the mouse coordinates
	if (client == m_active) {
		m_x = x;
		m_y = y;
		m_xDelta  = 0;
		m_yDelta  = 0;
		m_xDelta2 = 0;
		m_yDelta2 = 0;
		stopSwitch();
		if (client != m_primaryClient) {
			LOG((CLOG_DEBUG "reanchoring active screen \"%s\" at %d,%d after shape change",
				getName(client).c_str(), m_x, m_y));
			discardPendingMouseMove();
			client->mouseMove(m_x, m_y);
		}
	}

	// handle resolution change to primary screen
	if (client == m_primaryClient) {
		if (client == m_active) {
			onMouseMovePrimary(m_x, m_y);
		}
		else {
			LOG((CLOG_WARN "returning to primary after primary screen shape changed while \"%s\" was active",
				getName(m_active).c_str()));
			recoverToPrimaryFromActive("primary screen shape changed");
		}
	}
}

void
Server::handleClipboardGrabbed(const Event& event, void* vclient)
{
	if (!m_enableClipboard) {
		return;
	}

	// ignore events from unknown clients
	BaseClientProxy* grabber = static_cast<BaseClientProxy*>(vclient);
	if (m_clientSet.count(grabber) == 0) {
		return;
	}
	const IScreen::ClipboardInfo* info =
		static_cast<const IScreen::ClipboardInfo*>(event.getData());

	// ignore grab if sequence number is old.  always allow primary
	// screen to grab.
	ClipboardInfo& clipboard = m_clipboards[info->m_id];
	if (grabber != m_primaryClient &&
		info->m_sequenceNumber < clipboard.m_clipboardSeqNum) {
		LOG((CLOG_INFO "ignored screen \"%s\" grab of clipboard %d", getName(grabber).c_str(), info->m_id));
		return;
	}

	// mark screen as owning clipboard
	LOG((CLOG_INFO "screen \"%s\" grabbed clipboard %d from \"%s\"", getName(grabber).c_str(), info->m_id, clipboard.m_clipboardOwner.c_str()));
	clipboard.m_clipboardOwner  = getName(grabber);
	clipboard.m_clipboardSeqNum = info->m_sequenceNumber;
	clipboard.m_pendingPrimaryFetch = (grabber == m_primaryClient);

	LOG((CLOG_DEBUG "deferred clipboard %d fetch from \"%s\" until screen leave",
		info->m_id, getName(grabber).c_str()));
}

void
Server::handleClipboardChanged(const Event& event, void* vclient)
{
	// ignore events from unknown clients
	BaseClientProxy* sender = static_cast<BaseClientProxy*>(vclient);
	if (m_clientSet.count(sender) == 0) {
		return;
	}
	const IScreen::ClipboardInfo* info =
		static_cast<const IScreen::ClipboardInfo*>(event.getData());
	onClipboardChanged(sender, info->m_id, info->m_sequenceNumber);
}

void
Server::handleKeyDownEvent(const Event& event, void*)
{
	IPlatformScreen::KeyInfo* info =
		static_cast<IPlatformScreen::KeyInfo*>(event.getData());
	onKeyDown(info->m_key, info->m_mask, info->m_button, info->m_screens);
}

void
Server::handleKeyUpEvent(const Event& event, void*)
{
	IPlatformScreen::KeyInfo* info =
		 static_cast<IPlatformScreen::KeyInfo*>(event.getData());
	onKeyUp(info->m_key, info->m_mask, info->m_button, info->m_screens);
}

void
Server::handleKeyRepeatEvent(const Event& event, void*)
{
	IPlatformScreen::KeyInfo* info =
		static_cast<IPlatformScreen::KeyInfo*>(event.getData());
	onKeyRepeat(info->m_key, info->m_mask, info->m_count, info->m_button);
}

void
Server::handleButtonDownEvent(const Event& event, void*)
{
	IPlatformScreen::ButtonInfo* info =
		static_cast<IPlatformScreen::ButtonInfo*>(event.getData());
	onMouseDown(info->m_button);
}

void
Server::handleButtonUpEvent(const Event& event, void*)
{
	IPlatformScreen::ButtonInfo* info =
		static_cast<IPlatformScreen::ButtonInfo*>(event.getData());
	onMouseUp(info->m_button);
}

void
Server::handleMotionPrimaryEvent(const Event& event, void*)
{
	IPlatformScreen::MotionInfo* info =
		static_cast<IPlatformScreen::MotionInfo*>(event.getData());
	onMouseMovePrimary(info->m_x, info->m_y);
}

void
Server::handleMotionSecondaryEvent(const Event& event, void*)
{
	IPlatformScreen::MotionInfo* info =
		static_cast<IPlatformScreen::MotionInfo*>(event.getData());
	onMouseMoveSecondary(info->m_x, info->m_y);
}

void
Server::handleWheelEvent(const Event& event, void*)
{
	IPlatformScreen::WheelInfo* info =
		static_cast<IPlatformScreen::WheelInfo*>(event.getData());
	onMouseWheel(info->m_xDelta, info->m_yDelta);
}

void
Server::handleScreensaverActivatedEvent(const Event&, void*)
{
	onScreensaver(true);
}

void
Server::handleScreensaverDeactivatedEvent(const Event&, void*)
{
	onScreensaver(false);
}

void
Server::handleSwitchWaitTimeout(const Event&, void*)
{
	if (m_switchScreen == NULL || m_clientSet.count(m_switchScreen) == 0) {
		LOG((CLOG_WARN "canceling delayed switch to unavailable screen"));
		stopSwitch();
		return;
	}

	// ignore if mouse is locked to screen
	if (isLockedToScreen()) {
		LOG((CLOG_DEBUG1 "locked to screen"));
		stopSwitch();
		return;
	}

	// switch screen
	BaseClientProxy* dst = m_switchScreen;
	if (!switchScreen(dst, m_switchWaitX, m_switchWaitY, false, m_switchDir)) {
		reanchorActiveAfterFailedSwitch(dst);
	}
}

void
Server::handlePrimaryKeyStateSync(const Event&, void*)
{
	if (m_active == m_primaryClient) {
		m_primaryClient->refreshKeyState();
	}
}

void
Server::handleMouseMoveFlush(const Event&, void*)
{
	EventQueueTimer* timer = m_mouseMoveTimer;
	m_mouseMoveTimer = NULL;
	if (timer != NULL) {
		m_events->removeHandler(Event::kTimer, timer);
		m_events->deleteTimer(timer);
	}
	flushPendingMouseMove();
}

void
Server::handleClientDisconnected(const Event&, void* vclient)
{
	// client has disconnected.  it might be an old client or an
	// active client.  we don't care so just handle it both ways.
	BaseClientProxy* client = static_cast<BaseClientProxy*>(vclient);
	FileChunk::releaseReceiveBuffer(m_receivedFileData, m_expectedFileSize, &m_receivedFileSpoolPath);
	removeActiveClient(client);
	removeOldClient(client);

	if (deferDeleteIfSendingToClient(client)) {
		return;
	}
	deleteClientIfReady(client);
}

void
Server::handleClientCloseTimeout(const Event&, void* vclient)
{
	// client took too long to disconnect.  just dump it.
	BaseClientProxy* client = static_cast<BaseClientProxy*>(vclient);
	LOG((CLOG_NOTE "forced disconnection of client \"%s\"", getName(client).c_str()));
	FileChunk::releaseReceiveBuffer(m_receivedFileData, m_expectedFileSize, &m_receivedFileSpoolPath);
	removeOldClient(client);

	if (deferDeleteIfSendingToClient(client)) {
		return;
	}
	deleteClientIfReady(client);
}

void
Server::handleSwitchToScreenEvent(const Event& event, void*)
{
	SwitchToScreenInfo* info =
		static_cast<SwitchToScreenInfo*>(event.getData());

	ClientList::const_iterator index = m_clients.find(info->m_screen);
	if (index == m_clients.end()) {
		LOG((CLOG_DEBUG1 "screen \"%s\" not active", info->m_screen));
	}
	else {
		jumpToScreen(index->second);
	}
}

void
Server::handleToggleScreenEvent(const Event& event, void*)
{
  std::string current = getName(m_active);
  ClientList::const_iterator index = m_clients.find(current);
  if (index == m_clients.end()) {
    LOG((CLOG_DEBUG1 "screen \"%s\" not active", current.c_str()));
  }
  else {
    ++index;
    if (index == m_clients.end()) {
      index = m_clients.begin();
    }
    jumpToScreen(index->second);
  }
}


void
Server::handleSwitchInDirectionEvent(const Event& event, void*)
{
	SwitchInDirectionInfo* info =
		static_cast<SwitchInDirectionInfo*>(event.getData());

	// jump to screen in chosen direction from center of this screen
	SInt32 x = m_x, y = m_y;
	BaseClientProxy* newScreen =
		getNeighbor(m_active, info->m_direction, x, y);
	if (newScreen == NULL) {
		LOG((CLOG_DEBUG1 "no neighbor %s", Config::dirName(info->m_direction)));
	}
	else {
		jumpToScreen(newScreen);
	}
}

void
Server::handleKeyboardBroadcastEvent(const Event& event, void*)
{
	KeyboardBroadcastInfo* info = (KeyboardBroadcastInfo*)event.getData();

	// choose new state
	bool newState;
	switch (info->m_state) {
	case KeyboardBroadcastInfo::kOff:
		newState = false;
		break;

	default:
	case KeyboardBroadcastInfo::kOn:
		newState = true;
		break;

	case KeyboardBroadcastInfo::kToggle:
		newState = !m_keyboardBroadcasting;
		break;
	}

	// enter new state
	if (newState != m_keyboardBroadcasting ||
		info->m_screens != m_keyboardBroadcastingScreens) {
		m_keyboardBroadcasting        = newState;
		m_keyboardBroadcastingScreens = info->m_screens;
		LOG((CLOG_DEBUG "keyboard broadcasting %s: %s", m_keyboardBroadcasting ? "on" : "off", m_keyboardBroadcastingScreens.c_str()));
	}
}

void
Server::handleLockCursorToScreenEvent(const Event& event, void*)
{
	LockCursorToScreenInfo* info = (LockCursorToScreenInfo*)event.getData();

	// choose new state
	bool newState;
	switch (info->m_state) {
	case LockCursorToScreenInfo::kOff:
		newState = false;
		break;

	default:
	case LockCursorToScreenInfo::kOn:
		newState = true;
		break;

	case LockCursorToScreenInfo::kToggle:
		newState = !m_lockedToScreen;
		break;
	}

	// enter new state
	if (newState != m_lockedToScreen) {
		m_lockedToScreen = newState;
		LOG((CLOG_NOTE "cursor %s current screen", m_lockedToScreen ? "locked to" : "unlocked from"));

		m_primaryClient->reconfigure(getActivePrimarySides());
		if (!isLockedToScreenServer()) {
			stopRelativeMoves();
		}
	}
}

void
Server::handleFakeInputBeginEvent(const Event&, void*)
{
	m_primaryClient->fakeInputBegin();
}

void
Server::handleFakeInputEndEvent(const Event&, void*)
{
	m_primaryClient->fakeInputEnd();
}

void
Server::handleFileChunkSendingEvent(const Event& event, void*)
{
	onFileChunkSending(event.getData());
}

void
Server::handleFileRecieveCompletedEvent(const Event& event, void*)
{
	onFileRecieveCompleted();
}

void
Server::handleDropDirWriteFinishedEvent(const Event&, void*)
{
	drainDropDirTransferQueue();
}

void
Server::handleFileClipboardReadyEvent(const Event& event, void*)
{
	FileClipboardReadyInfo* info =
		static_cast<FileClipboardReadyInfo*>(event.getDataObject());
	if (info == NULL) {
		return;
	}

	if (!info->m_publishClipboard &&
		(m_remoteFileClipboardSession.empty() ||
		 m_remoteFileClipboardSession != info->m_sessionId)) {
		LOG((CLOG_WARN "ignoring stale remote clipboard ready event: session=%s current=%s",
			info->m_sessionId.c_str(), m_remoteFileClipboardSession.c_str()));
		return;
	}

	m_readyFileClipboardSession = info->m_sessionId;
	m_readyFileClipboardPaths = info->m_paths;
	if (info->m_publishClipboard) {
		publishMaterializedFileClipboard(m_readyFileClipboardPaths,
			m_readyFileClipboardSession);
	}
	else if (!m_remoteFileClipboardSession.empty() &&
		m_remoteFileClipboardSession == m_readyFileClipboardSession) {
		publishMaterializedFileClipboard(m_readyFileClipboardPaths,
			m_readyFileClipboardSession);
	}
}

void
Server::handleFileKeepAliveEvent(const Event&, void*)
{
	if (reapSendFileThreadIfReady()) {
		finishCompletedSendFileIfReady();
		deleteDeferredClients();
	}

	BaseClientProxy* target = m_sendFileTarget;
	if (target == NULL || m_clientSet.count(target) == 0) {
		return;
	}
	ProtocolUtil::writef(target->getStream(), kMsgCKeepAlive);
}

bool
Server::onClipboardChanged(BaseClientProxy* sender,
					ClipboardID id, UInt32 seqNum)
{
	ClipboardInfo& clipboard = m_clipboards[id];

	// ignore update if sequence number is old
	if (seqNum < clipboard.m_clipboardSeqNum) {
		LOG((CLOG_INFO "ignored screen \"%s\" update of clipboard %d (missequenced)", getName(sender).c_str(), id));
		return false;
	}

	// should be the expected client
	assert(sender == m_clients.find(clipboard.m_clipboardOwner)->second);

	// get data
	if (!readClipboardWithRetry(sender, id, clipboard.m_clipboard)) {
		return false;
	}

	RemoteFileClipboard::AutomaticSharingStatus clipboardSharingStatus =
		RemoteFileClipboard::AutomaticSharingStatus::Safe;
	if (id == kClipboardClipboard) {
		clipboardSharingStatus =
			RemoteFileClipboard::prepareForAutomaticClipboardSharing(clipboard.m_clipboard);
		if (clipboardSharingStatus ==
			RemoteFileClipboard::AutomaticSharingStatus::SafeAfterRemovingImageFileMetadata) {
			LOG((CLOG_INFO "stripped image file-transfer metadata before forwarding clipboard"));
		}
		else if (clipboardSharingStatus ==
			RemoteFileClipboard::AutomaticSharingStatus::Safe) {
			m_remoteFileClipboardSession.clear();
			m_readyFileClipboardSession.clear();
			m_readyFileClipboardPaths.clear();
		}
	}

	// ignore if data hasn't changed
    std::string data = clipboard.m_clipboard.marshall();
	if (clipboard.m_clipboardData.matches(data)) {
		LOG((CLOG_DEBUG "ignored screen \"%s\" update of clipboard %d (unchanged)", clipboard.m_clipboardOwner.c_str(), id));
		if (sender == m_primaryClient) {
			clipboard.m_pendingPrimaryFetch = false;
		}
		return true;
	}

	// got new data
	LOG((CLOG_INFO "screen \"%s\" updated clipboard %d", clipboard.m_clipboardOwner.c_str(), id));
	if (clipboardSharingStatus ==
		RemoteFileClipboard::AutomaticSharingStatus::ContainsFileList) {
		RemoteFileClipboard::Data sourceFileClipboard;
		std::string error;
		if (!RemoteFileClipboard::normalizeClipboard(
				clipboard.m_clipboard, &sourceFileClipboard, &error)) {
			LOG((CLOG_WARN "file clipboard metadata could not be normalized: %s",
				error.c_str()));
			if (sender == m_primaryClient) {
				clipboard.m_pendingPrimaryFetch = false;
			}
			return false;
		}

		data = clipboard.m_clipboard.marshall();
		m_readyFileClipboardSession.clear();
		m_readyFileClipboardPaths.clear();
		if (sender == m_primaryClient) {
			m_remoteFileClipboardSession.clear();
			LOG((CLOG_INFO
				"cached local file clipboard for transfer on the active remote screen: session=%s items=%lu",
				sourceFileClipboard.sessionId.c_str(),
				static_cast<unsigned long>(sourceFileClipboard.paths.size())));
		}
		else {
			m_remoteFileClipboardSession = sourceFileClipboard.sessionId;
			LOG((CLOG_INFO
				"awaiting remote file clipboard package: session=%s items=%lu source=%s",
				m_remoteFileClipboardSession.c_str(),
				static_cast<unsigned long>(sourceFileClipboard.paths.size()),
				getName(sender).c_str()));
		}
		clipboard.m_clipboardData.set(data);
		for (ClientList::const_iterator index = m_clients.begin();
									index != m_clients.end(); ++index) {
			BaseClientProxy* client = index->second;
			client->setClipboardDirty(id, client != sender);
		}
		if (sender == m_primaryClient) {
			clipboard.m_pendingPrimaryFetch = false;
		}
		return true;
	}

	clipboard.m_clipboardData.set(data);
	if (sender == m_primaryClient) {
		clipboard.m_pendingPrimaryFetch = false;
	}

	// tell all clients except the sender that the clipboard is dirty
	for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		BaseClientProxy* client = index->second;
		client->setClipboardDirty(id, client != sender);
	}

	// send the new clipboard to the active screen
	m_active->setClipboard(id, &clipboard.m_clipboard);
	return true;
}

void
Server::onScreensaver(bool activated)
{
	LOG((CLOG_DEBUG "onScreenSaver %s", activated ? "activated" : "deactivated"));

	if (activated) {
		// save current screen and position
		m_activeSaver = m_active;
		m_xSaver      = m_x;
		m_ySaver      = m_y;

		// jump to primary screen
		if (m_active != m_primaryClient) {
			if (!switchScreen(m_primaryClient, 0, 0, true)) {
				reanchorActiveAfterFailedSwitch(m_primaryClient);
			}
		}
	}
	else {
		bool restoredSaver = true;
		// jump back to previous screen and position.  we must check
		// that the position is still valid since the screen may have
		// changed resolutions while the screen saver was running.
		if (m_activeSaver != NULL && m_activeSaver != m_primaryClient) {
			// check position
			BaseClientProxy* screen = m_activeSaver;
			SInt32 x, y, w, h;
			screen->getShape(x, y, w, h);
			SInt32 zoneSize = getJumpZoneSize(screen);
			if (m_xSaver < x + zoneSize) {
				m_xSaver = x + zoneSize;
			}
			else if (m_xSaver >= x + w - zoneSize) {
				m_xSaver = x + w - zoneSize - 1;
			}
			if (m_ySaver < y + zoneSize) {
				m_ySaver = y + zoneSize;
			}
			else if (m_ySaver >= y + h - zoneSize) {
				m_ySaver = y + h - zoneSize - 1;
			}

			// jump
			restoredSaver = switchScreen(screen, m_xSaver, m_ySaver, false);
			if (!restoredSaver) {
				reanchorActiveAfterFailedSwitch(screen);
			}
		}

		// reset state
		if (restoredSaver) {
			m_activeSaver = NULL;
		}
	}

	// send message to all clients
	for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		BaseClientProxy* client = index->second;
		client->screensaver(activated);
	}
}

void
Server::onKeyDown(KeyID id, KeyModifierMask mask, KeyButton button,
				const char* screens)
{
	LOG((CLOG_DEBUG1 "onKeyDown id=%d mask=0x%04x button=0x%04x", id, mask, button));
	assert(m_active != NULL);
	flushPendingMouseMove();

	if (m_active != m_primaryClient && isReturnToPrimaryHotKey(id, mask)) {
		LOG((CLOG_WARN "emergency return to primary from \"%s\"", getName(m_active).c_str()));
		forceLeaveClient(m_active);
		return;
	}

	// relay
	if (!m_keyboardBroadcasting && IKeyState::KeyInfo::isDefault(screens)) {
		// If localShortcutMode is enabled and we're on primary screen,
		// intercept modifier keys locally to prevent conflicts with remote
		// screen shortcuts that use the same key combinations
		if (m_localShortcutMode && m_active == m_primaryClient) {
			// Only intercept keys that commonly cause conflicts:
			// - Meta/Win key (system-level shortcuts)
			// - Alt key alone (menu accelerators)
			// - Ctrl+Alt combinations (often keyboard layout switches)
			// Note: We do NOT intercept Ctrl+key or Shift+key alone
			// as these are less likely to conflict
			bool shouldHandleLocally = false;

			// Win/Super/Meta key is almost always a local shortcut
			if ((mask & KeyModifierMeta) != 0) {
				shouldHandleLocally = true;
			}
			// Alt key with other modifiers (not just Alt alone)
			else if ((mask & KeyModifierAlt) != 0 &&
					 ((mask & KeyModifierControl) != 0 ||
					  (mask & KeyModifierShift) != 0 ||
					  (mask & KeyModifierMeta) != 0)) {
				shouldHandleLocally = true;
			}
			// Alt key alone - only if we have a recent Alt+Tab or similar
			// This is a trade-off between compatibility and preventing conflicts
			// For now, we let Alt alone pass through to allow Alt+Tab to work

			if (shouldHandleLocally) {
				// Handle locally - don't forward to remote
				m_primaryClient->keyDown(id, mask, button);
				return;
			}
		}
		m_active->keyDown(id, mask, button);
	}
	else {
		if (!screens && m_keyboardBroadcasting) {
			screens = m_keyboardBroadcastingScreens.c_str();
			if (IKeyState::KeyInfo::isDefault(screens)) {
				screens = "*";
			}
		}
		for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
			if (IKeyState::KeyInfo::contains(screens, index->first)) {
				index->second->keyDown(id, mask, button);
			}
		}
	}
}

void
Server::onKeyUp(KeyID id, KeyModifierMask mask, KeyButton button,
				const char* screens)
{
	LOG((CLOG_DEBUG1 "onKeyUp id=%d mask=0x%04x button=0x%04x", id, mask, button));
	assert(m_active != NULL);
	flushPendingMouseMove();

	// relay
	if (!m_keyboardBroadcasting && IKeyState::KeyInfo::isDefault(screens)) {
		// Match the local shortcut mode logic from onKeyDown
		if (m_localShortcutMode && m_active == m_primaryClient) {
			bool shouldHandleLocally = false;

			if ((mask & KeyModifierMeta) != 0) {
				shouldHandleLocally = true;
			}
			else if ((mask & KeyModifierAlt) != 0 &&
					 ((mask & KeyModifierControl) != 0 ||
					  (mask & KeyModifierShift) != 0 ||
					  (mask & KeyModifierMeta) != 0)) {
				shouldHandleLocally = true;
			}

			if (shouldHandleLocally) {
				m_primaryClient->keyUp(id, mask, button);
				return;
			}
		}
		m_active->keyUp(id, mask, button);
	}
	else {
		if (!screens && m_keyboardBroadcasting) {
			screens = m_keyboardBroadcastingScreens.c_str();
			if (IKeyState::KeyInfo::isDefault(screens)) {
				screens = "*";
			}
		}
		for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
			if (IKeyState::KeyInfo::contains(screens, index->first)) {
				index->second->keyUp(id, mask, button);
			}
		}
	}
}

void
Server::onKeyRepeat(KeyID id, KeyModifierMask mask,
				SInt32 count, KeyButton button)
{
	LOG((CLOG_DEBUG1 "onKeyRepeat id=%d mask=0x%04x count=%d button=0x%04x", id, mask, count, button));
	assert(m_active != NULL);
	flushPendingMouseMove();

	// relay
	m_active->keyRepeat(id, mask, count, button);
}

void
Server::onMouseDown(ButtonID id)
{
	LOG((CLOG_DEBUG1 "onMouseDown id=%d", id));
	assert(m_active != NULL);
	flushPendingMouseMove();

	// relay
	m_active->mouseDown(id);
}

void
Server::onMouseUp(ButtonID id)
{
	LOG((CLOG_DEBUG1 "onMouseUp id=%d", id));
	assert(m_active != NULL);
	flushPendingMouseMove();

	// relay
	m_active->mouseUp(id);

	if (m_ignoreFileTransfer) {
		m_ignoreFileTransfer = false;
		return;
	}

	if (m_args.m_enableDragDrop) {
		if (!m_screen->isOnScreen()) {
            std::string& file = m_screen->getDraggingFilename();
			if (!file.empty()) {
				sendFileToClient(file.c_str());
			}
		}

		// always clear dragging filename
		m_screen->clearDraggingFilename();
	}
}

bool
Server::onMouseMovePrimary(SInt32 x, SInt32 y)
{
	// In low latency mode, reduce logging verbosity
	if (m_lowLatencyMode) {
		LOG((CLOG_DEBUG2 "onMouseMovePrimary %d,%d", x, y));
	} else {
		LOG((CLOG_DEBUG4 "onMouseMovePrimary %d,%d", x, y));
	}

	// mouse move on primary (server's) screen
	if (m_active != m_primaryClient) {
		// stale event -- we're actually on a secondary screen
		return false;
	}

	// save last delta
	m_xDelta2 = m_xDelta;
	m_yDelta2 = m_yDelta;

	// save current delta
	m_xDelta  = x - m_x;
	m_yDelta  = y - m_y;

	// save position
	m_x       = x;
	m_y       = y;
	clearRecentSwitchGuardIfMovedAway();

	// get screen shape
	SInt32 ax, ay, aw, ah;
	m_active->getShape(ax, ay, aw, ah);
	if (aw < kMinUsableScreenDimension || ah < kMinUsableScreenDimension) {
		LOG((CLOG_WARN "primary screen has unusable shape during local motion: %d,%d %dx%d",
			ax, ay, aw, ah));
		noSwitch(m_x, m_y);
		return false;
	}
	SInt32 zoneSize = getJumpZoneSize(m_active);

	// clamp position to screen
	SInt32 xc = x, yc = y;
	if (xc < ax + zoneSize) {
		xc = ax;
	}
	else if (xc >= ax + aw - zoneSize) {
		xc = ax + aw - 1;
	}
	if (yc < ay + zoneSize) {
		yc = ay;
	}
	else if (yc >= ay + ah - zoneSize) {
		yc = ay + ah - 1;
	}

	// see if we should change screens
	// when the cursor is in a corner, there may be a screen either
	// horizontally or vertically.  check both directions.
	EDirection dirh = kNoDirection, dirv = kNoDirection;
	SInt32 xh = x, yv = y;
	if (x < ax + zoneSize) {
		xh  -= zoneSize;
		dirh = kLeft;
	}
	else if (x >= ax + aw - zoneSize) {
		xh  += zoneSize;
		dirh = kRight;
	}
	if (y < ay + zoneSize) {
		yv  -= zoneSize;
		dirv = kTop;
	}
	else if (y >= ay + ah - zoneSize) {
		yv  += zoneSize;
		dirv = kBottom;
	}
	if (dirh == kNoDirection && dirv == kNoDirection) {
		// still on local screen
		noSwitch(x, y);
		return false;
	}

	// check both horizontally and vertically
	EDirection dirs[] = {dirh, dirv};
	SInt32 xs[] = {xh, x}, ys[] = {y, yv};
	for (int i = 0; i < 2; ++i) {
		EDirection dir = dirs[i];
		if (dir == kNoDirection) {
			continue;
		}
		x = xs[i], y = ys[i];

		// get jump destination
		BaseClientProxy* newScreen = mapToNeighbor(m_active, dir, x, y);

		// should we switch or not?
		if (isSwitchOkay(newScreen, dir, x, y, xc, yc)) {
			if (m_args.m_enableDragDrop
				&& m_screen->isDraggingStarted()
				&& m_active != newScreen) {
				sendDragInfo(newScreen);
			}

			// switch screen
			if (!switchScreen(newScreen, x, y, false, dir)) {
				reanchorActiveAfterFailedSwitch(newScreen);
				return false;
			}
			return true;
		}
	}

	return false;
}

void
Server::sendDragInfo(BaseClientProxy* newScreen)
{
	if (newScreen == NULL || m_screen == NULL) {
		return;
	}

	m_dragFileList.clear();
	std::string& dragFileList = m_screen->getDraggingFilename();
	if (!dragFileList.empty()) {
		m_dragFileList = parseDraggedPaths(dragFileList);
	}

#if defined(__APPLE__)
	// On macOS, faking a left-button release can produce a second mouse-up.
	m_ignoreFileTransfer = true;
#endif

	if (m_dragFileList.empty()) {
		return;
	}

    std::string infoString;
	UInt32 fileCount = DragInformation::setupDragInfo(m_dragFileList, infoString);
	m_dragFileList.clear();

	if (fileCount > 0) {
		size_t size = infoString.size();

		LOG((CLOG_DEBUG2 "sending drag information to client"));
		LOG((CLOG_DEBUG3 "dragging file list: %s", infoString.c_str()));
		LOG((CLOG_DEBUG3 "dragging file list string size: %i", size));
		newScreen->sendDragInfo(fileCount, infoString.c_str(), size);
	}
}

void
Server::onMouseMoveSecondary(SInt32 dx, SInt32 dy)
{
	LOG((CLOG_DEBUG2 "onMouseMoveSecondary %+d,%+d", dx, dy));

	// mouse move on secondary (client's) screen
	assert(m_active != NULL);
	if (m_active == m_primaryClient) {
		// stale event -- we're actually on the primary screen
		return;
	}

	// if doing relative motion on secondary screens and we're locked
	// to the screen (which activates relative moves) then send a
	// relative mouse motion.  when we're doing this we pretend as if
	// the mouse isn't actually moving because we're expecting some
	// program on the secondary screen to warp the mouse on us, so we
	// have no idea where it really is.
	if (m_relativeMoves && isLockedToScreenServer()) {
		LOG((CLOG_DEBUG2 "relative move on %s by %d,%d", getName(m_active).c_str(), dx, dy));
		flushPendingMouseMove();
		m_active->mouseRelativeMove(dx, dy);
		return;
	}

	// save old position
	const SInt32 xOld = m_x;
	const SInt32 yOld = m_y;

	// save last delta
	m_xDelta2 = m_xDelta;
	m_yDelta2 = m_yDelta;

	// save current delta
	m_xDelta  = dx;
	m_yDelta  = dy;

	// accumulate motion
	m_x      += dx;
	m_y      += dy;
	clearRecentSwitchGuardIfMovedAway();

	// get screen shape
	SInt32 ax, ay, aw, ah;
	m_active->getShape(ax, ay, aw, ah);
	if (aw < kMinUsableScreenDimension || ah < kMinUsableScreenDimension) {
		LOG((CLOG_WARN "active screen \"%s\" has unusable shape during secondary motion: %d,%d %dx%d",
			getName(m_active).c_str(), ax, ay, aw, ah));
		recoverToPrimaryFromActive("invalid active client shape during motion");
		return;
	}

	// find direction of neighbor and get the neighbor
	bool jump = true;
	BaseClientProxy* newScreen;
	EDirection switchDir = kNoDirection;
	do {
		// clamp position to screen
		SInt32 xc = m_x, yc = m_y;
		if (xc < ax) {
			xc = ax;
		}
		else if (xc >= ax + aw) {
			xc = ax + aw - 1;
		}
		if (yc < ay) {
			yc = ay;
		}
		else if (yc >= ay + ah) {
			yc = ay + ah - 1;
		}

		EDirection dir;
		if (m_x < ax) {
			dir = kLeft;
		}
		else if (m_x > ax + aw - 1) {
			dir = kRight;
		}
		else if (m_y < ay) {
			dir = kTop;
		}
		else if (m_y > ay + ah - 1) {
			dir = kBottom;
		}
		else {
			// we haven't left the screen
			newScreen = m_active;
			jump      = false;

			// if waiting and mouse is not on the border we're waiting
			// on then stop waiting.  also if it's not on the border
			// then arm the double tap.
			if (m_switchScreen != NULL) {
				bool clearWait;
				SInt32 zoneSize = m_primaryClient->getJumpZoneSize();
				switch (m_switchDir) {
				case kLeft:
					clearWait = (m_x >= ax + zoneSize);
					break;

				case kRight:
					clearWait = (m_x <= ax + aw - 1 - zoneSize);
					break;

				case kTop:
					clearWait = (m_y >= ay + zoneSize);
					break;

				case kBottom:
					clearWait = (m_y <= ay + ah - 1 - zoneSize);
					break;

				default:
					clearWait = false;
					break;
				}
				if (clearWait) {
					// still on local screen
					noSwitch(m_x, m_y);
				}
			}

			// skip rest of block
			break;
		}

		// try to switch screen.  get the neighbor.
		newScreen = mapToNeighbor(m_active, dir, m_x, m_y);
		switchDir = dir;

		// see if we should switch
		if (!isSwitchOkay(newScreen, dir, m_x, m_y, xc, yc)) {
			newScreen = m_active;
			jump      = false;
		}
	} while (false);

	if (jump) {
		if (m_sendFileChunker && !m_sendFileIsClipboardPrefetch) {
			m_sendFileChunker->interruptFile();
		}

		SInt32 newX = m_x;
		SInt32 newY = m_y;

			// switch screens
			if (!switchScreen(newScreen, newX, newY, false, switchDir) && m_active != newScreen &&
				clampToClientShape(m_active, m_x, m_y)) {
				LOG((CLOG_WARN "reanchoring \"%s\" at %d,%d after failed switch to \"%s\"",
					getName(m_active).c_str(), m_x, m_y, getName(newScreen).c_str()));
				m_xDelta = 0;
				m_yDelta = 0;
				m_xDelta2 = 0;
				m_yDelta2 = 0;
				noSwitch(m_x, m_y);
				discardPendingMouseMove();
				m_active->mouseMove(m_x, m_y);
			}
		}
	else {
		// same screen.  clamp mouse to edge.
		m_x = xOld + dx;
		m_y = yOld + dy;
		if (m_x < ax) {
			m_x = ax;
			LOG((CLOG_DEBUG2 "clamp to left of \"%s\"", getName(m_active).c_str()));
		}
		else if (m_x > ax + aw - 1) {
			m_x = ax + aw - 1;
			LOG((CLOG_DEBUG2 "clamp to right of \"%s\"", getName(m_active).c_str()));
		}
		if (m_y < ay) {
			m_y = ay;
			LOG((CLOG_DEBUG2 "clamp to top of \"%s\"", getName(m_active).c_str()));
		}
		else if (m_y > ay + ah - 1) {
			m_y = ay + ah - 1;
			LOG((CLOG_DEBUG2 "clamp to bottom of \"%s\"", getName(m_active).c_str()));
		}

		// warp cursor if it moved.
		if (m_x != xOld || m_y != yOld) {
			LOG((CLOG_DEBUG2 "move on %s to %d,%d", getName(m_active).c_str(), m_x, m_y));
			queueMouseMove(m_active, m_x, m_y);
		}
	}
}

void
Server::queueMouseMove(BaseClientProxy* target, SInt32 x, SInt32 y)
{
	if (target == NULL) {
		return;
	}

	if (m_pendingMouseMove && m_pendingMouseMoveTarget != target) {
		flushPendingMouseMove();
	}

	const double elapsed = m_mouseMoveRateTimer.getTime();
	if (!m_mouseMoveSent || elapsed >= kMouseMoveIntervalSeconds) {
		discardPendingMouseMove();
		target->mouseMove(x, y);
		m_mouseMoveSent = true;
		m_mouseMoveRateTimer.reset();
		return;
	}

	m_pendingMouseMove = true;
	m_pendingMouseMoveTarget = target;
	m_pendingMouseX = x;
	m_pendingMouseY = y;
	if (m_mouseMoveTimer == NULL && m_events != NULL) {
		const double delay = kMouseMoveIntervalSeconds - elapsed;
		m_mouseMoveTimer = m_events->newOneShotTimer(delay, NULL);
		m_events->adoptHandler(Event::kTimer, m_mouseMoveTimer,
							new TMethodEventJob<Server>(this,
								&Server::handleMouseMoveFlush));
	}
}

void
Server::flushPendingMouseMove()
{
	if (!m_pendingMouseMove) {
		return;
	}

	if (m_mouseMoveTimer != NULL) {
		m_events->removeHandler(Event::kTimer, m_mouseMoveTimer);
		m_events->deleteTimer(m_mouseMoveTimer);
		m_mouseMoveTimer = NULL;
	}

	BaseClientProxy* target = m_pendingMouseMoveTarget;
	const SInt32 x = m_pendingMouseX;
	const SInt32 y = m_pendingMouseY;
	m_pendingMouseMove = false;
	m_pendingMouseMoveTarget = NULL;
	if (target != NULL && target == m_active && m_clientSet.count(target) != 0) {
		target->mouseMove(x, y);
		m_mouseMoveSent = true;
		m_mouseMoveRateTimer.reset();
	}
}

void
Server::discardPendingMouseMove(BaseClientProxy* target)
{
	if (target != NULL && m_pendingMouseMoveTarget != target) {
		return;
	}

	if (m_mouseMoveTimer != NULL) {
		m_events->removeHandler(Event::kTimer, m_mouseMoveTimer);
		m_events->deleteTimer(m_mouseMoveTimer);
		m_mouseMoveTimer = NULL;
	}
	m_pendingMouseMove = false;
	m_pendingMouseMoveTarget = NULL;
}

void
Server::onMouseWheel(SInt32 xDelta, SInt32 yDelta)
{
	LOG((CLOG_DEBUG1 "onMouseWheel %+d,%+d", xDelta, yDelta));
	assert(m_active != NULL);
	flushPendingMouseMove();

	// relay
	m_active->mouseWheel(xDelta, yDelta);
}

void
Server::onFileChunkSending(const void* data)
{
	FileChunk* chunk = static_cast<FileChunk*>(const_cast<void*>(data));
	const bool completesTransfer =
		(chunk->m_chunk[0] == kDataEnd || chunk->m_chunk[0] == kDataCancel);

	LOG((CLOG_DEBUG1 "sending file chunk"));
	BaseClientProxy* target = m_sendFileTarget;
	if (target == NULL) {
		LOG((CLOG_DEBUG "dropping file chunk because transfer target is gone, transfer=%u current=%u",
			chunk->m_transferId, m_sendFileTransferId));
		return;
	}
	const bool hasTransferGeneration =
		(chunk->m_transferId != 0 || m_sendFileTransferId != 0);
	if (hasTransferGeneration && chunk->m_transferId != m_sendFileTransferId) {
		LOG((CLOG_DEBUG "dropping stale file chunk, transfer=%u current=%u",
			chunk->m_transferId, m_sendFileTransferId));
		return;
	}
	if (m_clientSet.count(target) == 0) {
		LOG((CLOG_DEBUG "dropping file chunk because target client is disconnected, transfer=%u current=%u",
			chunk->m_transferId, m_sendFileTransferId));
		if (completesTransfer) {
			m_sendFileCompletionPending = true;
			finishCompletedSendFileIfReady();
		}
		return;
	}

	// relay
	target->fileChunkSending(chunk->m_chunk[0], &chunk->m_chunk[1], chunk->m_dataSize);
	if (completesTransfer) {
		m_sendFileCompletionPending = true;
		finishCompletedSendFileIfReady();
	}
}

void
Server::onFileRecieveCompleted()
{
	if (isReceivedFileSizeValid()) {
		std::shared_ptr<CompletedFileTransfer> transfer = takeCompletedFileTransfer();
		startDropDirTransfer(transfer);
		return;
	}

	LOG((CLOG_ERR "received file completion with invalid size, expected=%d actual=%d",
		m_expectedFileSize,
		m_receivedFileSpoolPath.empty()
			? m_receivedFileData.size()
			: (barrier::fs::exists(m_receivedFileSpoolPath)
				? static_cast<size_t>(barrier::fs::file_size(m_receivedFileSpoolPath))
				: 0)));
	FileChunk::releaseReceiveBuffer(m_receivedFileData, m_expectedFileSize, &m_receivedFileSpoolPath);
}

void
Server::startDropDirTransfer(std::shared_ptr<CompletedFileTransfer> transfer)
{
	if (!transfer) {
		return;
	}

	if (!reapWriteToDropDirThreadIfReady()) {
		queueDropDirTransfer(transfer);
		return;
	}

	m_writeToDropDirThread = new Thread([this, transfer]() {
		write_to_drop_dir_thread(transfer);
		m_events->addEvent(Event(m_events->forFile().dropDirWriteFinished(), this));
	});
}

void
Server::queueDropDirTransfer(std::shared_ptr<CompletedFileTransfer> transfer)
{
	if (!transfer) {
		return;
	}
	size_t pendingMemoryBytes = 0;
	for (std::deque<std::shared_ptr<CompletedFileTransfer> >::const_iterator i =
			 m_pendingDropDirTransfers.begin();
		 i != m_pendingDropDirTransfers.end(); ++i) {
		if (*i && (*i)->spoolPath.empty()) {
			pendingMemoryBytes += (*i)->data.size();
		}
	}
	const size_t transferMemoryBytes =
		transfer->spoolPath.empty() ? transfer->data.size() : 0;
	if (transferMemoryBytes > 0 &&
		(transferMemoryBytes > kMaxPendingDropDirTransferMemoryBytes ||
		 pendingMemoryBytes > kMaxPendingDropDirTransferMemoryBytes - transferMemoryBytes)) {
		LOG((CLOG_ERR "drop-dir writer memory queue full; dropping completed file transfer, queued=%lu incoming=%lu limit=%lu",
			static_cast<unsigned long>(pendingMemoryBytes),
			static_cast<unsigned long>(transferMemoryBytes),
			static_cast<unsigned long>(kMaxPendingDropDirTransferMemoryBytes)));
		FileChunk::releaseReceiveBuffer(transfer->data, transfer->expectedSize, &transfer->spoolPath);
		return;
	}
	if (m_pendingDropDirTransfers.size() >= kMaxPendingDropDirTransfers) {
		LOG((CLOG_ERR "drop-dir writer queue full; dropping completed file transfer"));
		FileChunk::releaseReceiveBuffer(transfer->data, transfer->expectedSize, &transfer->spoolPath);
		return;
	}
	LOG((CLOG_WARN "queueing completed file transfer while drop-dir writer is still running (%lu/%lu)",
		static_cast<unsigned long>(m_pendingDropDirTransfers.size() + 1),
		static_cast<unsigned long>(kMaxPendingDropDirTransfers)));
	m_pendingDropDirTransfers.push_back(transfer);
}

void
Server::drainDropDirTransferQueue()
{
	if (!reapWriteToDropDirThreadIfReady() || m_pendingDropDirTransfers.empty()) {
		return;
	}

	std::shared_ptr<CompletedFileTransfer> transfer = m_pendingDropDirTransfers.front();
	m_pendingDropDirTransfers.pop_front();
	startDropDirTransfer(transfer);
}

void
Server::releasePendingDropDirTransfers()
{
	for (std::deque<std::shared_ptr<CompletedFileTransfer> >::iterator i = m_pendingDropDirTransfers.begin();
		 i != m_pendingDropDirTransfers.end(); ++i) {
		if (*i) {
			FileChunk::releaseReceiveBuffer((*i)->data, (*i)->expectedSize, &(*i)->spoolPath);
		}
	}
	m_pendingDropDirTransfers.clear();
}

void Server::write_to_drop_dir_thread(std::shared_ptr<CompletedFileTransfer> transfer)
{
	LOG((CLOG_DEBUG "starting write to drop dir thread"));

	if (!transfer) {
		return;
	}

	bool released = false;
	const auto releaseReceiveBuffer = [&]() {
		if (!released) {
			FileChunk::releaseReceiveBuffer(transfer->data, transfer->expectedSize, &transfer->spoolPath);
			released = true;
		}
	};

	try {
	const bool hasSpooledReceive = !transfer->spoolPath.empty();
	if (!transfer->remoteFileClipboardSession.empty() &&
		(hasSpooledReceive
			? TransferArchive::isPackageFile(transfer->spoolPath)
			: TransferArchive::isPackageData(transfer->data))) {
		LOG((CLOG_INFO "remote clipboard package received: session=%s", transfer->remoteFileClipboardSession.c_str()));
		const barrier::fs::path cacheRoot =
			barrier::DataDirectories::profile() / "clipboard-cache" / "server";
		const barrier::fs::path spoolDir =
			RemoteFileClipboard::materializedSessionRoot(cacheRoot, transfer->remoteFileClipboardSession);
		std::vector<barrier::fs::path> roots;
		std::string error;
		const bool extracted = hasSpooledReceive
			? RemoteFileClipboard::extractPackageFile(transfer->spoolPath, spoolDir, roots, error)
			: RemoteFileClipboard::extractPackage(transfer->data, spoolDir, roots, error);
		if (extracted) {
			RemoteFileClipboard::pruneMaterializedCache(
				cacheRoot, transfer->remoteFileClipboardSession, 8, 512u * 1024u * 1024u);
			LOG((CLOG_INFO "remote clipboard package materialized: session=%s items=%lu",
				transfer->remoteFileClipboardSession.c_str(),
				static_cast<unsigned long>(roots.size())));
			FileClipboardReadyInfo* info = new FileClipboardReadyInfo();
			info->m_sessionId = transfer->remoteFileClipboardSession;
			for (size_t i = 0; i < roots.size(); ++i) {
				info->m_paths.push_back(roots[i].u8string());
			}

			Event ready(m_events->forFile().fileClipboardReady(), this);
			ready.setDataObject(info);
			m_events->addEvent(ready);
		}
		else {
			LOG((CLOG_ERR "failed to materialize remote clipboard package: %s", error.c_str()));
		}

		releaseReceiveBuffer();
		return;
	}

	std::vector<String> droppedPaths = hasSpooledReceive
		? DropHelper::writeToDirFromFile(transfer->dropTarget, transfer->dragFileList, transfer->spoolPath)
		: DropHelper::writeToDir(transfer->dropTarget, transfer->dragFileList, transfer->data);

	if (!droppedPaths.empty()) {
		const std::string sessionId = RemoteFileClipboard::createSessionId();
		FileClipboardReadyInfo* info = new FileClipboardReadyInfo();
		info->m_sessionId = sessionId;
		info->m_publishClipboard = true;
		for (size_t i = 0; i < droppedPaths.size(); ++i) {
			info->m_paths.push_back(droppedPaths[i]);
		}

		Event ready(m_events->forFile().fileClipboardReady(), this);
		ready.setDataObject(info);
		m_events->addEvent(ready);
	}

	releaseReceiveBuffer();
	}
	catch (XThread&) {
		releaseReceiveBuffer();
		throw;
	}
	catch (std::exception& error) {
		LOG((CLOG_ERR "drop-dir writer failed: %s", error.what()));
		releaseReceiveBuffer();
	}
}

void
Server::publishMaterializedFileClipboard(const std::vector<std::string>& paths,
										 const std::string& sessionId)
{
	Clipboard clipboard;
	if (!RemoteFileClipboard::buildMaterializedClipboard(
			utf8PathsToFsPaths(paths), sessionId, clipboard)) {
		LOG((CLOG_ERR "failed to publish remote clipboard locally: session=%s", sessionId.c_str()));
		return;
	}

	if (m_screen != NULL) {
		m_screen->setClipboard(kClipboardClipboard, &clipboard);
	}
	if (m_primaryClient != NULL) {
		m_primaryClient->setClipboard(kClipboardClipboard, &clipboard);
	}
	m_clipboards[kClipboardClipboard].m_clipboardData.set(clipboard.marshall());
	LOG((CLOG_INFO "remote clipboard published locally: session=%s items=%lu",
		sessionId.c_str(),
		static_cast<unsigned long>(paths.size())));
	m_remoteFileClipboardSession.clear();
}

void
Server::sendClipboardSelectionToClient(BaseClientProxy* target,
									   const std::vector<barrier::fs::path>& sourcePaths)
{
	if (target == NULL || sourcePaths.empty()) {
		return;
	}

	if (!reapSendFileThreadIfReady()) {
		LOG((CLOG_DEBUG "remote clipboard prefetch already active; keeping the current sender"));
		return;
	}
	finishCompletedSendFileIfReady();
	if (m_sendFileTarget != NULL) {
		LOG((CLOG_DEBUG "remote clipboard prefetch deferred until queued file output is flushed"));
		return;
	}

	auto chunker = std::make_shared<StreamChunker>();
	m_sendFileChunker = chunker;
	m_sendFileTarget = target;
	m_sendFileCompletionPending = false;
	m_sendFileIsClipboardPrefetch = true;
	m_sendFileTransferId++;
	if (m_sendFileTransferId == 0) {
		m_sendFileTransferId++;
	}
	const UInt32 transferId = m_sendFileTransferId;
	barrier::IStream* stream = target->getStream();
	LOG((CLOG_INFO "remote clipboard prefetch started: direction=server-to-client items=%lu target=%s",
		static_cast<unsigned long>(sourcePaths.size()),
		target->getName().c_str()));
	m_sendFileThread = new Thread([this, stream, sourcePaths, chunker, transferId]() {
		send_clipboard_file_thread(stream, sourcePaths, chunker, transferId);
	});
}

void
Server::send_clipboard_file_thread(barrier::IStream* stream,
                                   const std::vector<barrier::fs::path>& sourcePaths,
                                   const std::shared_ptr<StreamChunker>& chunker,
                                   UInt32 transferId)
{
    barrier::fs::path packagePath;
    try {
        Thread::testCancel();
        RemoteFileClipboard::Data payload;
        payload.mode = RemoteFileClipboard::Mode::SourcePaths;
        payload.paths = sourcePaths;

        std::string error;
        if (!RemoteFileClipboard::createPackage(payload, packagePath, error)) {
            throw std::runtime_error(error);
        }

	        Thread::testCancel();
	        chunker->sendFile(packagePath.u8string().c_str(), m_events, this,
	            stream, transferId);
    }
    catch (XThread&) {
        if (!packagePath.empty()) {
            barrier::fs::remove(packagePath);
        }
        throw;
    }
    catch (std::runtime_error& error) {
        LOG((CLOG_ERR "failed sending remote clipboard file package: %s", error.what()));
    }

	if (!packagePath.empty()) {
		barrier::fs::remove(packagePath);
	}
}

bool
Server::addClient(BaseClientProxy* client)
{
    std::string name = getName(client);
	if (m_clients.count(name) != 0) {
		return false;
	}

	// add event handlers
	m_events->adoptHandler(m_events->forIScreen().shapeChanged(),
							client->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleShapeChanged, client));
	m_events->adoptHandler(m_events->forClipboard().clipboardGrabbed(),
							client->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleClipboardGrabbed, client));
	m_events->adoptHandler(m_events->forClipboard().clipboardChanged(),
							client->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleClipboardChanged, client));

	// add to list
	m_clientSet.insert(client);
	m_clients.insert(std::make_pair(name, client));

	// initialize client data
	SInt32 x, y;
	client->getCursorPos(x, y);
	if (client == m_primaryClient || clampToClientShape(client, x, y)) {
		client->setJumpCursorPos(x, y);
	}

	// tell primary client about the active sides
	m_primaryClient->reconfigure(getActivePrimarySides());

	return true;
}

bool
Server::removeClient(BaseClientProxy* client)
{
	// return false if not in list
	ClientSet::iterator i = m_clientSet.find(client);
	if (i == m_clientSet.end()) {
		return false;
	}
	discardPendingMouseMove(client);

	// remove event handlers
	m_events->removeHandler(m_events->forIScreen().shapeChanged(),
							client->getEventTarget());
	m_events->removeHandler(m_events->forClipboard().clipboardGrabbed(),
							client->getEventTarget());
	m_events->removeHandler(m_events->forClipboard().clipboardChanged(),
							client->getEventTarget());

	// remove from list
	m_clients.erase(getName(client));
	m_clientSet.erase(i);

	return true;
}

void
Server::closeClient(BaseClientProxy* client, const char* msg, bool forceLeave)
{
	assert(client != m_primaryClient);
	assert(msg != NULL);

	// send message to client.  this message should cause the client
	// to disconnect.  we add this client to the closed client list
	// and install a timer to remove the client if it doesn't respond
	// quickly enough.  we also remove the client from the active
	// client list since we're not going to listen to it anymore.
	// note that this method also works on clients that are not in
	// the m_clients list.  adoptClient() may call us with such a
	// client.
	LOG((CLOG_NOTE "disconnecting client \"%s\"", getName(client).c_str()));

	// send message
	// FIXME -- avoid type cast (kinda hard, though)
	((ClientProxy*)client)->close(msg);
	if (client == m_sendFileTarget && !cleanupSendFileThread(true)) {
		LOG((CLOG_WARN "client \"%s\" is closing while file sender is still stopping",
			getName(client).c_str()));
	}

	// install timer.  wait timeout seconds for client to close.
	double timeout = 5.0;
	EventQueueTimer* timer = m_events->newOneShotTimer(timeout, NULL);
	m_events->adoptHandler(Event::kTimer, timer,
							new TMethodEventJob<Server>(this,
								&Server::handleClientCloseTimeout, client));

	// move client to closing list
	removeClient(client);
	m_oldClients.insert(std::make_pair(client, timer));

	if (forceLeave) {
		// if this client is the active screen then we have to
		// jump off of it
		forceLeaveClient(client);
	}
}

void
Server::closeClients(const Config& config)
{
	// collect the clients that are connected but are being dropped
	// from the configuration (or who's canonical name is changing).
	typedef std::set<BaseClientProxy*> RemovedClients;
	RemovedClients removed;
	for (ClientList::iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		if (!config.isCanonicalName(index->first)) {
			removed.insert(index->second);
		}
	}

	// don't close the primary client
	removed.erase(m_primaryClient);

	// now close them.  we collect the list then close in two steps
	// because closeClient() modifies the collection we iterate over.
	for (RemovedClients::iterator index = removed.begin();
								index != removed.end(); ++index) {
		closeClient(*index, kMsgCClose);
	}
}

void
Server::removeActiveClient(BaseClientProxy* client)
{
	if (removeClient(client)) {
		forceLeaveClient(client);
		m_events->removeHandler(m_events->forClientProxy().disconnected(), client);
		if (m_clients.size() == 1 && m_oldClients.empty()) {
			m_events->addEvent(Event(m_events->forServer().disconnected(), this));
		}
	}
}

void
Server::removeOldClient(BaseClientProxy* client)
{
	OldClients::iterator i = m_oldClients.find(client);
	if (i != m_oldClients.end()) {
		m_events->removeHandler(m_events->forClientProxy().disconnected(), client);
		m_events->removeHandler(Event::kTimer, i->second);
		m_events->deleteTimer(i->second);
		m_oldClients.erase(i);
		if (m_clients.size() == 1 && m_oldClients.empty()) {
			m_events->addEvent(Event(m_events->forServer().disconnected(), this));
		}
	}
}

void
Server::forceLeaveClient(BaseClientProxy* client)
{
	if (m_switchScreen == client) {
		stopSwitch();
	}

	BaseClientProxy* active =
		(m_activeSaver != NULL) ? m_activeSaver : m_active;
	if (active == client) {
		// record new position (center of primary screen)
		m_primaryClient->getCursorCenter(m_x, m_y);
		const bool primaryUsable = clampToClientShape(m_primaryClient, m_x, m_y);
		const bool primaryEnterable = canEnterScreen(m_primaryClient);

		// stop waiting to switch to this client
		if (active == m_switchScreen) {
			stopSwitch();
		}

		if (!primaryEnterable || !primaryUsable) {
			LOG((CLOG_WARN "holding local input on primary after \"%s\" left; primaryEnterable=%d primaryUsable=%d",
				getName(active).c_str(), primaryEnterable ? 1 : 0, primaryUsable ? 1 : 0));
			m_active = m_primaryClient;
			m_xDelta = 0;
			m_yDelta = 0;
			m_xDelta2 = 0;
			m_yDelta2 = 0;
			replayClipboardsToActive();
			m_primaryLeaveFailedRecently = true;
			m_primaryLeaveFailureTimer.reset();
		}
		else {
			// don't notify active screen since it has probably already
			// disconnected.
			LOG((CLOG_INFO "jump from \"%s\" to \"%s\" at %d,%d", getName(active).c_str(), getName(m_primaryClient).c_str(), m_x, m_y));

			// cut over
			m_active = m_primaryClient;

			// enter new screen (unless we already have because of the
			// screen saver)
			if (m_activeSaver == NULL) {
				m_primaryClient->enter(m_x, m_y, m_seqNum,
									m_primaryClient->getToggleMask(), false);
				m_primaryClient->refreshKeyState();
			}
			replayClipboardsToActive();

			Server::SwitchToScreenInfo* info =
				Server::SwitchToScreenInfo::alloc(m_active->getName());
			m_events->addEvent(Event(m_events->forServer().screenSwitched(), this, info));
		}
	}

	// if this screen had the cursor when the screen saver activated
	// then we can't switch back to it when the screen saver
	// deactivates.
	if (m_activeSaver == client) {
		m_activeSaver = NULL;
	}

	// tell primary client about the active sides
	m_primaryClient->reconfigure(getActivePrimarySides());
}


//
// Server::ClipboardInfo
//

Server::ClipboardInfo::ClipboardInfo() :
	m_clipboard(),
	m_clipboardData(),
	m_clipboardOwner(),
	m_clipboardSeqNum(0),
	m_pendingPrimaryFetch(false)
{
	// do nothing
}


//
// Server::LockCursorToScreenInfo
//

Server::LockCursorToScreenInfo*
Server::LockCursorToScreenInfo::alloc(State state)
{
	LockCursorToScreenInfo* info =
		(LockCursorToScreenInfo*)malloc(sizeof(LockCursorToScreenInfo));
	info->m_state = state;
	return info;
}


//
// Server::SwitchToScreenInfo
//

Server::SwitchToScreenInfo*
Server::SwitchToScreenInfo::alloc(const std::string& screen)
{
	SwitchToScreenInfo* info =
		(SwitchToScreenInfo*)malloc(sizeof(SwitchToScreenInfo) +
								screen.size());
	strcpy(info->m_screen, screen.c_str());
	return info;
}


//
// Server::SwitchInDirectionInfo
//

Server::SwitchInDirectionInfo*
Server::SwitchInDirectionInfo::alloc(EDirection direction)
{
	SwitchInDirectionInfo* info =
		(SwitchInDirectionInfo*)malloc(sizeof(SwitchInDirectionInfo));
	info->m_direction = direction;
	return info;
}

//
// Server::KeyboardBroadcastInfo
//

Server::KeyboardBroadcastInfo*
Server::KeyboardBroadcastInfo::alloc(State state)
{
	KeyboardBroadcastInfo* info =
		(KeyboardBroadcastInfo*)malloc(sizeof(KeyboardBroadcastInfo));
	info->m_state      = state;
	info->m_screens[0] = '\0';
	return info;
}

Server::KeyboardBroadcastInfo*
Server::KeyboardBroadcastInfo::alloc(State state, const std::string& screens)
{
	KeyboardBroadcastInfo* info =
		(KeyboardBroadcastInfo*)malloc(sizeof(KeyboardBroadcastInfo) +
								screens.size());
	info->m_state = state;
	strcpy(info->m_screens, screens.c_str());
	return info;
}

bool
Server::isReceivedFileSizeValid()
{
	if (!m_receivedFileSpoolPath.empty()) {
		return barrier::fs::exists(m_receivedFileSpoolPath) &&
			static_cast<size_t>(barrier::fs::file_size(m_receivedFileSpoolPath)) == m_expectedFileSize;
	}
	return m_expectedFileSize == m_receivedFileData.size();
}

void
Server::sendFileToClient(const std::string& filename)
{
	if (!cleanupSendFileThread(true)) {
		LOG((CLOG_WARN "file send skipped because previous file sender is still stopping"));
		return;
	}
	BaseClientProxy* target = m_active;
	if (target == NULL || m_clientSet.count(target) == 0 || target == m_primaryClient) {
		LOG((CLOG_WARN "cannot send file without an active secondary target"));
		return;
	}

    auto chunker = std::make_shared<StreamChunker>();
	m_sendFileChunker = chunker;
	m_sendFileTarget = target;
	m_sendFileCompletionPending = false;
	m_sendFileIsClipboardPrefetch = false;
	m_sendFileTransferId++;
	if (m_sendFileTransferId == 0) {
		m_sendFileTransferId++;
	}
	const UInt32 transferId = m_sendFileTransferId;
	barrier::IStream* stream = target->getStream();
    m_sendFileThread = new Thread([this, stream, filename, chunker, transferId]() {
		send_file_thread(stream, filename, chunker, transferId);
	});
}

void Server::send_file_thread(barrier::IStream* stream,
                              const std::string& filename,
                              const std::shared_ptr<StreamChunker>& chunker,
                              UInt32 transferId)
{
	barrier::fs::path sourcePath;
	barrier::fs::path tempPackagePath;
	try {
		Thread::testCancel();
		LOG((CLOG_DEBUG "sending file to client, filename=%s", filename.c_str()));
		std::string error;
		if (!prepareTransferSource(filename.c_str(), sourcePath, tempPackagePath, error)) {
			throw std::runtime_error(error);
		}

			Thread::testCancel();
			const barrier::fs::path& transferPath =
				tempPackagePath.empty() ? sourcePath : tempPackagePath;
			chunker->sendFile(transferPath.u8string().c_str(), m_events, this,
				stream, transferId);
	}
	catch (XThread&) {
		if (!tempPackagePath.empty()) {
			barrier::fs::remove(tempPackagePath);
		}
		throw;
	}
	catch (std::runtime_error &error) {
		LOG((CLOG_ERR "failed sending file chunks, error: %s", error.what()));
	}

	if (!tempPackagePath.empty()) {
		barrier::fs::remove(tempPackagePath);
	}
}

bool
Server::cleanupSendFileThread(bool cancel)
{
	BaseClientProxy* target = m_sendFileTarget;
	if (cancel && m_sendFileChunker) {
		m_sendFileChunker->interruptFile();
	}

	if (m_sendFileThread != NULL) {
		if (cancel && !m_sendFileThread->wait(2.0)) {
			LOG((CLOG_WARN "file send thread did not stop after interrupt; cancelling"));
			m_sendFileThread->cancel();
			m_sendFileThread->unblockPollSocket();
			if (!m_sendFileThread->wait(5.0)) {
				LOG((CLOG_ERR "file send thread still running after cancellation; cleanup deferred"));
				return false;
			}
		}
		else if (!cancel && !m_sendFileThread->wait(5.0)) {
			LOG((CLOG_ERR "file send thread still running; cleanup deferred"));
			return false;
		}
		delete m_sendFileThread;
		m_sendFileThread = NULL;
	}

	m_sendFileChunker.reset();
	m_sendFileTarget = NULL;
	m_sendFileCompletionPending = false;
	m_sendFileIsClipboardPrefetch = false;
	deleteDeferredClient(target);
	deleteDeferredClients();
	return true;
}

bool
Server::reapSendFileThreadIfReady()
{
	if (m_sendFileThread == NULL) {
		return true;
	}
	if (!m_sendFileThread->wait(0.0)) {
		return false;
	}

	delete m_sendFileThread;
	m_sendFileThread = NULL;
	m_sendFileChunker.reset();
	return true;
}

bool
Server::finishCompletedSendFileIfReady()
{
	if (!m_sendFileCompletionPending || m_sendFileThread != NULL) {
		return false;
	}

	BaseClientProxy* target = m_sendFileTarget;
	if (target != NULL && m_clientSet.count(target) != 0) {
		barrier::IStream* stream = target->getStream();
		if (stream != NULL && stream->getBufferedOutputSize() > 0) {
			return false;
		}
	}

	m_sendFileTarget = NULL;
	m_sendFileCompletionPending = false;
	m_sendFileIsClipboardPrefetch = false;
	deleteDeferredClient(target);
	deleteDeferredClients();
	return true;
}

bool
Server::cleanupWriteToDropDirThread()
{
	if (m_writeToDropDirThread != NULL) {
		if (!m_writeToDropDirThread->wait(2.0)) {
			LOG((CLOG_WARN "drop-dir writer thread did not stop; cancelling"));
			m_writeToDropDirThread->cancel();
			m_writeToDropDirThread->unblockPollSocket();
			if (!m_writeToDropDirThread->wait(5.0)) {
				LOG((CLOG_ERR "drop-dir writer thread still running after cancellation; cleanup deferred"));
				return false;
			}
		}
		delete m_writeToDropDirThread;
			m_writeToDropDirThread = NULL;
	}
	return true;
}

bool
Server::reapWriteToDropDirThreadIfReady()
{
	if (m_writeToDropDirThread == NULL) {
		return true;
	}
	if (!m_writeToDropDirThread->wait(0.0)) {
		return false;
	}
	delete m_writeToDropDirThread;
	m_writeToDropDirThread = NULL;
	return true;
}

bool
Server::deferDeleteIfSendingToClient(BaseClientProxy* client)
{
	if (client == NULL || client != m_sendFileTarget) {
		return false;
	}

	if (cleanupSendFileThread(true)) {
		return false;
	}

	LOG((CLOG_WARN "deferring deleted client cleanup until file sender stops"));
	m_deferredDeleteClients.insert(client);
	return true;
}

bool
Server::deleteClientIfReady(BaseClientProxy* client)
{
	if (client == NULL) {
		return true;
	}

	if (!clientReadyForDelete(client)) {
		LOG((CLOG_WARN "deferring deleted client cleanup until async clipboard sender stops"));
		m_deferredDeleteClients.insert(client);
		return false;
	}

	m_deferredDeleteClients.erase(client);
	delete client;
	return true;
}

bool
Server::clientReadyForDelete(BaseClientProxy* client)
{
	ClientProxy1_6* client16 = dynamic_cast<ClientProxy1_6*>(client);
	if (client16 != NULL && !client16->cleanupClipboardSendThread(true)) {
		return false;
	}
	return true;
}

void
Server::deleteDeferredClient(BaseClientProxy* client)
{
	if (client == NULL) {
		return;
	}

	ClientSet::iterator it = m_deferredDeleteClients.find(client);
	if (it == m_deferredDeleteClients.end()) {
		return;
	}

	m_deferredDeleteClients.erase(it);
	deleteClientIfReady(client);
}

void
Server::deleteDeferredClients()
{
	ClientSet clients;
	clients.swap(m_deferredDeleteClients);
	for (ClientSet::iterator it = clients.begin(); it != clients.end(); ++it) {
		if (*it == m_sendFileTarget) {
			m_deferredDeleteClients.insert(*it);
			continue;
		}
		deleteClientIfReady(*it);
	}
}

std::shared_ptr<Server::CompletedFileTransfer>
Server::takeCompletedFileTransfer()
{
	std::shared_ptr<CompletedFileTransfer> transfer(new CompletedFileTransfer());
	transfer->expectedSize = m_expectedFileSize;
	transfer->data.swap(m_receivedFileData);
	transfer->spoolPath = m_receivedFileSpoolPath;
	m_receivedFileSpoolPath.clear();
	if (m_screen != NULL) {
		transfer->dropTarget = m_screen->getDropTarget();
	}
	transfer->dragFileList.swap(m_fakeDragFileList);
	transfer->remoteFileClipboardSession = m_remoteFileClipboardSession;
	m_expectedFileSize = 0;
	return transfer;
}

void
Server::dragInfoReceived(UInt32 fileNum, std::string content)
{
	if (!m_args.m_enableDragDrop) {
		LOG((CLOG_DEBUG "drag drop not enabled, ignoring drag info."));
		return;
	}

	DragInformation::parseDragInfo(m_fakeDragFileList, fileNum, content);

	m_screen->startDraggingFiles(m_fakeDragFileList);
}
namespace {

bool prepareTransferSource(const char* filename,
                           barrier::fs::path& sourcePath,
                           barrier::fs::path& tempPackagePath,
                           std::string& error)
{
    sourcePath.clear();
    tempPackagePath.clear();
    error.clear();

    std::vector<barrier::fs::path> sourcePaths;
    std::istringstream input(filename);
    std::string entry;
    while (std::getline(input, entry)) {
        if (!entry.empty()) {
            sourcePaths.push_back(barrier::fs::u8path(entry));
        }
    }
    if (sourcePaths.empty()) {
        sourcePaths.push_back(barrier::fs::u8path(filename));
    }

    for (const auto& path : sourcePaths) {
        if (!barrier::fs::exists(path)) {
            error = "transfer source does not exist";
            return false;
        }
    }

    if (sourcePaths.size() > 1) {
        return TransferArchive::createSelectionPackageFile(sourcePaths, tempPackagePath, error);
    }

    sourcePath = sourcePaths.front();
    if (!barrier::fs::exists(sourcePath)) {
        error = "transfer source does not exist";
        return false;
    }

    if (barrier::fs::is_directory(sourcePath)) {
        return TransferArchive::createSelectionPackageFile(sourcePaths, tempPackagePath, error);
    }

    if (barrier::fs::is_symlink(barrier::fs::symlink_status(sourcePath)) ||
        !barrier::fs::is_regular_file(sourcePath)) {
        error = "transfer source must be a regular file";
        return false;
    }

    return true;
}

}

bool
testServerPrepareTransferSource(const char* filename,
                                barrier::fs::path& sourcePath,
                                barrier::fs::path& tempPackagePath,
                                std::string& error)
{
	return prepareTransferSource(filename, sourcePath, tempPackagePath, error);
}
