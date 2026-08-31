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
#include "server/ClientProxy1_12.h"

#include "server/ClientProxy.h"
#include "server/ClientProxy1_6.h"
#include "server/ClientProxyUnknown.h"
#include "server/PrimaryClient.h"
#include "server/ClientListener.h"
#include "barrier/FileChunk.h"
#include "barrier/FileTransferProtocol.h"
#include "barrier/FileTransferReceiver.h"
#include "barrier/FileTransferSendState.h"
#include "barrier/BulkChannel.h"
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
#include "barrier/SecureRandom.h"
#include "net/TCPSocket.h"
#include "net/IDataSocket.h"
#include "net/IListenSocket.h"
#include "net/XSocket.h"
#include "mt/Thread.h"
#include "mt/ThreadShutdown.h"
#include "mt/XThread.h"
#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/TMethodEventJob.h"
#include "common/DataDirectories.h"

#include <cstring>
#include <charconv>
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

const double kBulkBindingLifetimeSeconds = 30.0;
const int kMaxBulkTokenGenerationAttempts = 8;

const SInt32 kMinUsableScreenDimension = 64;
const SInt32 kSwitchEdgeHysteresisInset = 16;
const double kInputHandoffTimeoutSeconds = 0.5;
const double kInputHandoffCommitAckTimeoutSeconds = 1.5;
const double kInputHandoffSourceRevokeTimeoutSeconds = 1.5;
const int kClipboardReadAttempts = 8;
const double kClipboardReadRetrySeconds = 0.025;
const double kClipboardSyncDelaySeconds = 0.01;
const double kFileReceiveCompletionPollSeconds = 0.01;
const double kFileTransferCancelAckTimeoutSeconds = 15.0;
const double kFileSenderReapPollSeconds = 0.01;
const UInt32 kDefaultHeartbeatMilliseconds = 10000;
const double kMouseMoveIntervalSeconds = 1.0 / 240.0;
const size_t kMaxPendingDropDirTransfers = 4;
const size_t kMaxPendingDropDirTransferMemoryBytes = 32 * 1024 * 1024;

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
		m_inputHandoffPending(false),
		m_inputHandoffCommitReady(false),
		m_inputHandoffCommitted(false),
		m_inputHandoffCommitAckPending(false),
		m_inputHandoffSourceRevokePending(false),
		m_inputHandoffSourceRevoked(false),
		m_inputHandoffSource(NULL),
		m_inputHandoffTarget(NULL),
		m_inputHandoffSeqNum(0),
		m_inputHandoffSourceLeaseSeqNum(0),
		m_inputHandoffX(0),
		m_inputHandoffY(0),
		m_inputHandoffSourceX(0),
		m_inputHandoffSourceY(0),
		m_inputHandoffMask(0),
		m_inputHandoffForScreensaver(false),
		m_inputHandoffGuardDir(kNoDirection),
		m_inputHandoffTimer(NULL),
		m_activeReplacementSource(NULL),
		m_activeReplacementTarget(NULL),
		m_activeReplacementSeqNum(0),
		m_activeReplacementTimer(NULL),
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
	m_primaryReturnAnchorActive(false),
	m_primaryReturnAnchorDir(kNoDirection),
	m_primaryReturnAnchorX(0),
	m_primaryReturnAnchorY(0),
	m_switchWaitDelay(0.0),
	m_switchWaitTimer(NULL),
	m_primaryKeyStateTimer(NULL),
	m_mouseMoveTimer(NULL),
	m_clipboardSyncTimer(NULL),
	m_fileReceiveCompletionTimer(NULL),
	m_fileReceiveCompletionGeneration(0),
	m_clipboardFetchPending(false),
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
		m_fileReceiveSession(),
	m_fileReceiveSource(NULL),
	m_fileReceiveBulkChannel(NULL),
	m_sendFileThread(NULL),
	m_sendFileBulkChannel(),
	m_sendFileTransactionState(),
	m_sendFileTarget(NULL),
	m_sendFileTransferId(0),
	m_sendFileCompletionPending(false),
	m_sendFileStarted(false),
	m_sendFilePreflightFailed(false),
	m_sendFileCleanupPending(false),
	m_sendFileIsClipboardPrefetch(false),
	m_sendFileCancelAckPending(false),
	m_sendFileCancelAckTimeoutTimer(NULL),
	m_sendFileCancelAckTimeoutTransferId(0),
	m_sendFileReapTimer(NULL),
	m_sendFileReapTransferId(0),
	m_sendFileOutputDrainTransferId(0),
	m_sendFileOutputDrainDeadline(0.0),
	m_pendingManualFileSendTarget(NULL),
	m_pendingManualFileSendPath(),
	m_pendingFileClipboardPrefetchTarget(NULL),
	m_pendingFileClipboardPrefetchPaths(),
	m_pendingFileClipboardPrefetchSession(),
	m_pendingFileClipboardPrefetchRevision(),
	m_writeToDropDirThread(NULL),
	m_pendingDropDirTransfers(),
	m_transactionalDragFileLists(),
	m_clipboardRevision(),
	m_remoteFileClipboardSession(),
	m_remoteFileClipboardRevision(),
	m_remoteFileClipboardOriginBinding(),
	m_readyFileClipboardSession(),
	m_readyFileClipboardPaths(),
	m_readyFileClipboardRevision(),
	m_nextClipboardPublicationId(0),
	m_pendingMaterializedClipboardPublicationId(0),
	m_lastCommittedClipboardPublicationId(0),
	m_pendingMaterializedClipboardData(),
	m_pendingMaterializedClipboardSession(),
	m_pendingMaterializedClipboardRevision(),
	m_fileReceiveClipboardGeneration(0),
	m_fileReceiveRemoteFileClipboardSession(),
	m_fileReceiveClipboardOrigin(),
	m_fileReceiveClipboardRevision(),
	m_ignoreFileTransfer(false),
	m_enableClipboard(true),
	m_localShortcutMode(false),
	m_lowLatencyMode(false),
	m_nestedRemoteMode(false),
	m_primaryLeaveFailedRecently(false),
	m_primaryLeaveFailureDir(kNoDirection),
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

	cancelActiveClientReplacement("server is shutting down", true);
	cancelInputHandoff("server is shutting down", false);
	cleanupInputHandoffTimer();
	discardPendingMouseMove();
	cleanupSendFileCancelAckTimeout();
	cleanupSendFileReap();

	if (!cleanupSendFileThread(true) && m_sendFileThread != NULL) {
		barrier::waitForFinalThreadShutdown(
			"server file sender",
			barrier::kFinalThreadShutdownDeadlineSeconds,
			[this](double timeout) { return m_sendFileThread->wait(timeout); });
		delete m_sendFileThread;
		m_sendFileThread = NULL;
		m_sendFileChunker.reset();
		m_sendFileTarget = NULL;
		m_sendFilePreflightFailed.store(false);
		resetSendFileOutputDrainDeadline();
	}
	if (!cleanupWriteToDropDirThread() && m_writeToDropDirThread != NULL) {
		barrier::waitForFinalThreadShutdown(
			"server drop-dir writer",
			barrier::kFinalThreadShutdownDeadlineSeconds,
			[this](double timeout) { return m_writeToDropDirThread->wait(timeout); });
		delete m_writeToDropDirThread;
		m_writeToDropDirThread = NULL;
	}
	deleteDeferredClients();
	cleanupFileReceiveCompletionPoll();
	FileChunk::releaseReceiveBuffer(m_fileReceiveSession);
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
	if (m_clipboardSyncTimer != NULL) {
		m_events->removeHandler(Event::kTimer, m_clipboardSyncTimer);
		m_events->deleteTimer(m_clipboardSyncTimer);
		m_clipboardSyncTimer = NULL;
	}
	m_events->removeHandler(Event::kTimer, this);
	stopSwitch();

	// Teardown is not an input handoff.  Clear both runtime leases before
	// closing secondary clients so closeClient() cannot re-enter the primary
	// input backend while the server object graph is being destroyed.
	m_activeSaver = NULL;
	m_active = m_primaryClient;

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
	m_events->adoptHandler(m_events->forClientProxy().inputHandoffReady(), client,
								new TMethodEventJob<Server>(this,
									&Server::handleInputHandoffReady, client));

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
		if (replaceActive && m_activeSaver == NULL &&
			oldClient->supportsInputLeaseRevokeAck()) {
			if (m_activeReplacementTarget != NULL ||
				m_inputHandoffPending || m_inputHandoffCommitted) {
				LOG((CLOG_WARN
					"deferring replacement of active client \"%s\" while another input transaction is pending",
					getName(client).c_str()));
				closeClient(client, kMsgEBusy, false);
				return;
			}
			if (beginActiveClientReplacement(oldClient, client)) {
				return;
			}
			closeClient(client, kMsgEBusy, false);
			return;
		}
		closeClient(oldClient, kMsgEBusy, false);
	}

	finishAdoptingClient(client, replaceActive, replaceActiveSaver);
}

void
Server::finishAdoptingClient(BaseClientProxy* client, bool replaceActive,
								 bool replaceActiveSaver)
{
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
	renewBulkChannel(client);

	// activate screen saver on new client if active on the primary screen
	if (m_activeSaver != NULL) {
		client->screensaver(true);
	}

	if (replaceActive && m_activeSaver == NULL) {
		++m_seqNum;
		const bool trackReentry = client->supportsInputHandoff();
		SInt32 sourceX = 0;
		SInt32 sourceY = 0;
		if (trackReentry) {
			getPrimaryRecoveryPoint(client, sourceX, sourceY);
		}
		client->enter(m_x, m_y, m_seqNum,
			m_primaryClient->getToggleMask(), false);
		if (trackReentry) {
			trackCommittedInputHandoff(m_primaryClient, client, m_seqNum,
				sourceX, sourceY, kNoDirection);
		}
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

bool
Server::beginActiveClientReplacement(BaseClientProxy* source,
								  BaseClientProxy* target)
{
	if (source == NULL || target == NULL ||
		m_activeReplacementTarget != NULL ||
		m_active != source || !source->supportsInputLeaseRevokeAck()) {
		return false;
	}

	m_activeReplacementSource = source;
	m_activeReplacementTarget = target;
	m_activeReplacementSeqNum = ++m_seqNum;
	LOG((CLOG_WARN
		"waiting for active client \"%s\" to revoke epoch=%u before replacement seq=%u",
		getName(source).c_str(), source->getInputEpoch(),
		m_activeReplacementSeqNum));
	source->requestInputLeaseRevoke(
		m_activeReplacementSeqNum, source->getInputEpoch());
	m_activeReplacementTimer = m_events->newOneShotTimer(
		kInputHandoffSourceRevokeTimeoutSeconds, NULL);
	m_events->adoptHandler(Event::kTimer, m_activeReplacementTimer,
		new TMethodEventJob<Server>(this,
			&Server::handleActiveClientReplacementTimeout, NULL));
	return true;
}

void
Server::cleanupActiveClientReplacementTimer()
{
	if (m_activeReplacementTimer != NULL) {
		m_events->removeHandler(Event::kTimer, m_activeReplacementTimer);
		m_events->deleteTimer(m_activeReplacementTimer);
		m_activeReplacementTimer = NULL;
	}
}

void
Server::completeActiveClientReplacement()
{
	BaseClientProxy* source = m_activeReplacementSource;
	BaseClientProxy* target = m_activeReplacementTarget;
	if (target == NULL) {
		return;
	}

	cleanupActiveClientReplacementTimer();
	m_activeReplacementSource = NULL;
	m_activeReplacementTarget = NULL;
	m_activeReplacementSeqNum = 0;

	if (source != NULL && m_clientSet.count(source) != 0) {
		// A positive revoke ACK proves the old input backend is inactive.
		// Retire its transport without forcing another local lease transition.
		closeClient(source, kMsgEBusy, false);
	}
	finishAdoptingClient(target, true, false);
}

void
Server::cancelActiveClientReplacement(const char* reason, bool closeTarget)
{
	BaseClientProxy* target = m_activeReplacementTarget;
	if (target == NULL) {
		return;
	}
	LOG((CLOG_WARN "canceling active client replacement for \"%s\": %s",
		getName(target).c_str(), reason));
	cleanupActiveClientReplacementTimer();
	m_activeReplacementSource = NULL;
	m_activeReplacementTarget = NULL;
	m_activeReplacementSeqNum = 0;
	if (closeTarget) {
		closeClient(target, kMsgEBusy, false);
	}
}

std::string
Server::generateBulkToken() const
{
	std::string token;
	if (!barrier::SecureRandom::generateHex(32, token)) {
		return std::string();
	}
	return token;
}

void
Server::eraseBulkBindings(BaseClientProxy* client)
{
	for (std::map<std::string, PendingBulkBinding>::iterator i =
			m_pendingBulkBindings.begin(); i != m_pendingBulkBindings.end();) {
		if (i->second.client == client) {
			i = m_pendingBulkBindings.erase(i);
		}
		else {
			++i;
		}
	}
}

void
Server::renewBulkChannel(BaseClientProxy* client)
{
	if (client == NULL || !client->supportsBulkChannel() ||
		m_clientSet.count(client) == 0) {
		return;
	}
	eraseBulkBindings(client);

	const std::string connectionBinding = client->getConnectionBinding();
	if (client->supportsTransactionalFileTransfer() &&
		!isValidConnectionBinding(connectionBinding)) {
		LOG((CLOG_ERR
			"refusing bulk offer for \"%s\": invalid control connection binding",
			getName(client).c_str()));
		return;
	}

	std::string token;
	for (int attempt = 0;
		 attempt < kMaxBulkTokenGenerationAttempts && token.empty();
		 ++attempt) {
		const std::string candidate = generateBulkToken();
		if (candidate.empty()) {
			LOG((CLOG_ERR
				"refusing bulk offer for \"%s\": secure token generation failed",
				getName(client).c_str()));
			return;
		}
		if (m_pendingBulkBindings.count(candidate) == 0) {
			token = candidate;
		}
	}
	if (token.empty()) {
		LOG((CLOG_ERR
			"refusing bulk offer for \"%s\": secure token collision budget exhausted",
			getName(client).c_str()));
		return;
	}

	m_pendingBulkBindings.insert(std::make_pair(
		token, PendingBulkBinding(getName(client), client, token,
			client->supportsTransactionalFileTransfer() ? connectionBinding :
			std::string())));
	client->offerBulkChannel(token);
}

void
Server::handleBulkDisconnected(BaseClientProxy* client,
                               barrier::BulkChannel* channel,
                               std::uint64_t pausedGeneration)
{
	if (pausedGeneration != 0) {
		handleBulkInputPauseFailed(client, channel, pausedGeneration);
	}
	else {
		abortFileReceiveRoute(client, channel);
	}
	if (m_sendFileTarget == client && m_sendFileBulkChannel &&
		m_sendFileBulkChannel.get() == channel) {
		if (m_sendFileTransactionState) {
			m_sendFileTransactionState->connectionLost();
		}
		cleanupSendFileCancelAckTimeout();
		m_sendFileCancelAckPending = false;
		if (m_sendFileChunker) {
			m_sendFileChunker->interruptFile();
		}
	}
}

barrier::FileTransferReason
Server::prepareTransactionalFileReceive(
	BaseClientProxy* source, const barrier::FileTransferFrame& start,
	TransactionalFileReceiveContext& context)
{
	context = TransactionalFileReceiveContext();
	if (source == NULL || !source->supportsTransactionalFileTransfer() ||
		start.type != barrier::FileTransferFrameType::kStart ||
		start.connectionBinding.empty() ||
		start.connectionBinding != source->getConnectionBinding() ||
		!barrier::FileTransferProtocol::validate(
			start, barrier::FileTransferRole::kSecondary)) {
		return barrier::FileTransferReason::kProtocolError;
	}
	if (!m_mock && m_clientSet.count(source) == 0) {
		return barrier::FileTransferReason::kConnectionLost;
	}

	context.source = source;
	context.transferId = start.transferId;
	context.connectionBinding = start.connectionBinding;
	context.kind = start.kind;
	context.senderClipboardRevision = start.clipboardRevision;
	context.clipboardSessionId = start.clipboardSessionId;
	if (m_screen != NULL && m_screen->getPlatformScreen() != NULL) {
		context.dropTarget = m_screen->getDropTarget();
	}
	if (start.kind == barrier::FileTransferKind::kDrag) {
		std::map<BaseClientProxy*, DragFileList>::iterator drag =
			m_transactionalDragFileLists.find(source);
		if (drag != m_transactionalDragFileLists.end()) {
			context.dragFileList.swap(drag->second);
			m_transactionalDragFileLists.erase(drag);
		}
	}
	return barrier::FileTransferReason::kNone;
}

barrier::FileTransferReason
Server::acceptTransactionalFileReceive(
	const TransactionalFileReceiveContext& context,
	barrier::CompletedFilePayload&& payload)
{
	const auto releasePayload = [&payload]() {
		FileChunk::releaseReceiveBuffer(
			payload.data, payload.expectedSize, &payload.spoolPath);
	};
	if (context.source == NULL || context.transferId == 0 ||
		context.connectionBinding.empty() ||
		context.source->getConnectionBinding() != context.connectionBinding ||
		(!m_mock && m_clientSet.count(context.source) == 0)) {
		releasePayload();
		return barrier::FileTransferReason::kConnectionLost;
	}

	std::string remoteFileClipboardSession;
	std::string remoteFileClipboardOrigin;
	barrier::ClipboardRevision clipboardRevision;
	if (context.kind == barrier::FileTransferKind::kClipboard) {
		const bool currentClipboardMatches =
			context.senderClipboardRevision != 0 &&
			!context.clipboardSessionId.empty() &&
			m_clipboardRevision.valid() &&
			m_remoteFileClipboardSession == context.clipboardSessionId &&
			m_remoteFileClipboardRevision == m_clipboardRevision &&
			m_remoteFileClipboardOriginBinding == context.connectionBinding;
		if (!currentClipboardMatches) {
			LOG((CLOG_INFO
				"rejecting superseded or unbound transactional clipboard package: transfer=%u session=%s",
				context.transferId, context.clipboardSessionId.c_str()));
			releasePayload();
			return barrier::FileTransferReason::kRejected;
		}
		remoteFileClipboardSession = context.clipboardSessionId;
		remoteFileClipboardOrigin = context.source->getName();
		clipboardRevision = m_clipboardRevision;
	}

	std::shared_ptr<CompletedFileTransfer> transfer;
	try {
		transfer = std::make_shared<CompletedFileTransfer>();
		transfer->expectedSize = payload.expectedSize;
		transfer->data.swap(payload.data);
		transfer->spoolPath.swap(payload.spoolPath);
		transfer->dropTarget = context.dropTarget;
		transfer->dragFileList = context.dragFileList;
		transfer->remoteFileClipboardSession =
			remoteFileClipboardSession;
		transfer->remoteFileClipboardOrigin =
			remoteFileClipboardOrigin;
		transfer->clipboardRevision = clipboardRevision;
		transfer->kind = context.kind;
	}
	catch (...) {
		releasePayload();
		return barrier::FileTransferReason::kIoError;
	}

	return startDropDirTransfer(transfer) ?
		barrier::FileTransferReason::kNone :
		barrier::FileTransferReason::kBusy;
}

bool
Server::handleTransactionalFileAck(
	BaseClientProxy* source, const barrier::FileTransferFrame& frame)
{
	const bool isAck =
		frame.type == barrier::FileTransferFrameType::kStartAck ||
		frame.type == barrier::FileTransferFrameType::kCancelAck ||
		frame.type == barrier::FileTransferFrameType::kCommitAck;
	if (source == NULL || !isAck ||
		!barrier::FileTransferProtocol::validate(
			frame, barrier::FileTransferRole::kPrimary) ||
		frame.connectionBinding != source->getConnectionBinding()) {
		LOG((CLOG_WARN "rejecting invalid transactional file ACK"));
		return false;
	}
	if (source != m_sendFileTarget || !m_sendFileTransactionState ||
		frame.transferId != m_sendFileTransactionState->transferId()) {
		LOG((CLOG_DEBUG1
			"ignoring stale transactional file ACK, transfer=%u",
			frame.transferId));
		return true;
	}

	bool accepted = false;
	switch (frame.type) {
	case barrier::FileTransferFrameType::kStartAck:
		accepted = m_sendFileTransactionState->signalStartAck(
			frame.transferId, frame.reason);
		break;
	case barrier::FileTransferFrameType::kCancelAck:
		accepted = m_sendFileTransactionState->signalCancelAck(
			frame.transferId, frame.reason);
		if (accepted) {
			cleanupSendFileCancelAckTimeout();
			m_sendFileCancelAckPending = false;
		}
		break;
	case barrier::FileTransferFrameType::kCommitAck:
		accepted = m_sendFileTransactionState->signalCommitAck(
			frame.transferId, frame.reason);
		break;
	default:
		return false;
	}
	if (!accepted) {
		LOG((CLOG_DEBUG1
			"ignoring out-of-phase transactional file ACK, transfer=%u",
			frame.transferId));
	}
	else if (frame.type != barrier::FileTransferFrameType::kStartAck ||
		frame.reason != barrier::FileTransferReason::kNone) {
		serviceSendFileCompletion();
	}
	return true;
}

void
Server::transactionalDragInfoReceived(
	BaseClientProxy* source, UInt32 fileNum, const std::string& content)
{
	if (source == NULL || !m_args.m_enableDragDrop) {
		return;
	}
	DragFileList parsed;
	DragInformation::parseDragInfo(parsed, fileNum, content);
	m_transactionalDragFileLists[source].swap(parsed);
	if (m_screen != NULL) {
		m_screen->startDraggingFiles(m_transactionalDragFileLists[source]);
	}
}

void
Server::handleBulkInputPauseFailed(BaseClientProxy* client,
	barrier::BulkChannel* channel, std::uint64_t generation)
{
	if (!m_fileReceiveSession.matchesGeneration(generation)) {
		return;
	}
	if ((m_fileReceiveSource != NULL || m_fileReceiveBulkChannel != NULL) &&
		(m_fileReceiveSource != client || m_fileReceiveBulkChannel != channel)) {
		return;
	}

	const FileReceiveSession::State state = m_fileReceiveSession.state();
	if (state != FileReceiveSession::kReceiving &&
		state != FileReceiveSession::kFinalizing &&
		state != FileReceiveSession::kComplete &&
		state != FileReceiveSession::kFailed) {
		return;
	}

	LOG((CLOG_WARN
		"server bulk input pause failed; cancelling receive generation=%llu state=%d",
		static_cast<unsigned long long>(generation), static_cast<int>(state)));
	cleanupFileReceiveCompletionPoll();
	FileChunk::releaseReceiveBuffer(m_fileReceiveSession);
	m_fileReceiveSource = NULL;
	m_fileReceiveBulkChannel = NULL;
	m_fileReceiveClipboardGeneration = 0;
	m_fileReceiveRemoteFileClipboardSession.clear();
	m_fileReceiveClipboardOrigin.clear();
	m_fileReceiveClipboardRevision.reset();
}

bool
Server::attachBulkStream(const std::string& name, const std::string& token,
						 barrier::IStream* stream)
{
	return attachBulkStream(name, token, std::string(), stream);
}

bool
Server::attachBulkStream(const std::string& name, const std::string& token,
						 const std::string& connectionBinding,
						 barrier::IStream* stream)
{
	std::map<std::string, PendingBulkBinding>::iterator binding =
		m_pendingBulkBindings.find(token);
	if (binding == m_pendingBulkBindings.end()) {
		LOG((CLOG_WARN "rejected bulk connection with unknown or reused token"));
		return false;
	}

	PendingBulkBinding pending = binding->second;
	if (pending.issued.getTime() > kBulkBindingLifetimeSeconds) {
		m_pendingBulkBindings.erase(binding);
		LOG((CLOG_WARN "rejected expired bulk connection token for \"%s\"",
			 pending.name.c_str()));
		if (pending.client != NULL &&
			m_clientSet.count(pending.client) != 0) {
			renewBulkChannel(pending.client);
		}
		return false;
	}
	if (pending.token != token || pending.name != name ||
		pending.client == NULL ||
		m_clientSet.count(pending.client) == 0 ||
		getName(pending.client) != name) {
		LOG((CLOG_WARN "rejected bulk connection with mismatched client binding"));
		return false;
	}
	if (pending.client->supportsTransactionalFileTransfer()) {
		if (!isValidConnectionBinding(connectionBinding) ||
			connectionBinding != pending.connectionBinding ||
			connectionBinding != pending.client->getConnectionBinding()) {
			LOG((CLOG_WARN
				"rejected bulk connection with mismatched control binding"));
			return false;
		}
	}
	else if (!connectionBinding.empty() || !pending.connectionBinding.empty()) {
		LOG((CLOG_WARN "rejected unexpected binding on legacy bulk connection"));
		return false;
	}

	// Exact-match attempts consume the bearer token whether attachment succeeds
	// or fails, so a captured hello can never be replayed.
	m_pendingBulkBindings.erase(binding);
	if (!pending.client->attachBulkChannel(stream)) {
		renewBulkChannel(pending.client);
		return false;
	}

	startPendingManualFileSend();
	startPendingFileClipboardPrefetch();
	return true;
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
					EDirection guardDir, bool trackDirectCommit)
{
	assert(dst != NULL);
	if (m_inputHandoffPending &&
		m_inputHandoffSource == m_active &&
		m_inputHandoffTarget == dst) {
		stopSwitch();
		return true;
	}
	if (!m_inputHandoffCommitReady &&
		m_inputHandoffCommitAckPending && m_active != dst) {
		BaseClientProxy* confirmedSource = m_inputHandoffSource;
		LOG((CLOG_WARN
			"rolling back unacknowledged input lease before switching from \"%s\" to \"%s\"",
			getName(m_active).c_str(), getName(dst).c_str()));
		rollbackCommittedInputHandoff(
			"superseded before commit acknowledgment");
		if (dst == confirmedSource && m_active == dst) {
			return true;
		}
		stopSwitch();
		return false;
	}
	if (m_inputHandoffPending &&
		(dst != m_inputHandoffTarget || m_active != m_inputHandoffSource)) {
		if (m_inputHandoffSourceRevokePending) {
			LOG((CLOG_WARN
				"rejecting superseding switch while source lease revoke is pending"));
			stopSwitch();
			return false;
		}
		cancelInputHandoff("superseded by another switch", false);
	}

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
		!canLeavePrimaryNow("screen switch", guardDir)) {
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

	const bool targetNeedsPrepare = guardDir != kNoDirection &&
		dst != m_primaryClient && dst->supportsInputHandoff();
	const bool sourceNeedsRevoke = m_active != m_primaryClient &&
		m_active->supportsInputLeaseRevokeAck();
	if (!m_inputHandoffCommitReady && m_active != dst &&
		(targetNeedsPrepare || sourceNeedsRevoke)) {
		// Legacy peers have no commit acknowledgment. Retire their rollback
		// record before the currently active target becomes the new source.
		if (m_inputHandoffCommitted) {
			finishCommittedInputHandoff();
		}
		return beginInputHandoff(dst, x, y, guardDir, forScreensaver);
	}

	// wrapping means leaving the active screen and entering it again.
	// since that's a waste of time we skip that and just warp the
	// mouse.
	if (m_active != dst) {
		flushPendingMouseMove();
		BaseClientProxy* oldActive = m_active;
		const SInt32 oldX = m_x;
		const SInt32 oldY = m_y;
		const bool trackDirectHandoff =
			trackDirectCommit && !m_inputHandoffCommitReady &&
			dst != m_primaryClient &&
			dst->supportsInputHandoff();
		if (oldActive == m_primaryClient && dst != m_primaryClient) {
			rememberPrimaryReturnAnchor(dst, oldX, oldY, guardDir);
		}

			const bool sourceAlreadyRevoked =
				m_inputHandoffCommitReady && m_inputHandoffSourceRevoked &&
				oldActive == m_inputHandoffSource;
			// Leave is synchronous for the primary and legacy peers. Protocol
			// 1.11 sources have already acknowledged the matching lease revoke.
			if (!sourceAlreadyRevoked && !m_active->leave()) {
				// cannot leave screen
				LOG((CLOG_WARN "can't leave screen"));
				m_x = oldX;
				m_y = oldY;
				m_xDelta = 0;
				m_yDelta = 0;
				m_xDelta2 = 0;
				m_yDelta2 = 0;
				if (oldActive == m_primaryClient) {
					recoverPrimaryAfterSwitchFailure(oldX, oldY, guardDir);
				}
				return false;
			}

			if (!m_inputHandoffCommitReady && m_inputHandoffCommitted) {
				finishCommittedInputHandoff();
			}

		m_primaryLeaveFailedRecently = false;
		m_primaryLeaveFailureDir = kNoDirection;
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

		// cut over
		m_active = dst;

		if (m_inputHandoffCommitReady) {
			m_seqNum = m_inputHandoffSeqNum;
		}
		else {
			++m_seqNum;
		}

		// enter new screen
		m_active->enter(x, y, m_seqNum,
								m_inputHandoffCommitReady ? m_inputHandoffMask :
									m_primaryClient->getToggleMask(),
								forScreensaver);
		if (trackDirectHandoff) {
			trackCommittedInputHandoff(oldActive, m_active, m_seqNum,
				oldX, oldY, guardDir);
		}
		if (m_active == m_primaryClient) {
			m_primaryClient->refreshKeyState();
		}

		if (m_enableClipboard) {
			// Only replay the last committed revision after a switch. Reading the
			// OS clipboard here can block the input event queue for seconds.
			scheduleClipboardSync(false);
		}

		Server::SwitchToScreenInfo* info =
			Server::SwitchToScreenInfo::alloc(m_active->getName());
		m_events->addEvent(Event(m_events->forServer().screenSwitched(), this, info));
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
Server::beginInputHandoff(BaseClientProxy* dst, SInt32 x, SInt32 y,
						  EDirection guardDir, bool forScreensaver)
{
	if (m_inputHandoffPending) {
		if (m_inputHandoffSource == m_active &&
			m_inputHandoffTarget == dst) {
			return true;
		}
		cancelInputHandoff("superseded before readiness", false);
	}

	m_inputHandoffPending = true;
	m_inputHandoffCommitAckPending = false;
	m_inputHandoffSourceRevokePending = false;
	m_inputHandoffSourceRevoked = false;
	m_inputHandoffSource = m_active;
	m_inputHandoffTarget = dst;
	// Prepare sequence numbers are speculative and may advance without
	// changing the source lease.  Revoke the epoch actually installed in the
	// source proxy, not the global allocator's latest value.
	m_inputHandoffSourceLeaseSeqNum =
		m_inputHandoffSource->getInputEpoch();
	m_inputHandoffSeqNum = ++m_seqNum;
	m_inputHandoffX = x;
	m_inputHandoffY = y;
	m_inputHandoffSourceX = m_x;
	m_inputHandoffSourceY = m_y;
	m_inputHandoffMask = m_primaryClient->getToggleMask();
	m_inputHandoffForScreensaver = forScreensaver;
	m_inputHandoffGuardDir = guardDir;
	m_inputHandoffPhaseTimer.reset();

	if (dst->supportsInputHandoff()) {
		LOG((CLOG_DEBUG1
			"preparing input handoff from \"%s\" to \"%s\", seq=%u",
			getName(m_inputHandoffSource).c_str(), getName(dst).c_str(),
			m_inputHandoffSeqNum));
		dst->prepareEnter(x, y, m_inputHandoffSeqNum, m_inputHandoffMask);

		m_inputHandoffTimer =
			m_events->newOneShotTimer(kInputHandoffTimeoutSeconds, NULL);
		m_events->adoptHandler(Event::kTimer, m_inputHandoffTimer,
			new TMethodEventJob<Server>(this,
				&Server::handleInputHandoffTimeout, NULL));
		return true;
	}

	if (requestInputHandoffSourceRevoke()) {
		return true;
	}
	cancelInputHandoff("handoff has neither a prepared target nor revocable source",
		false, false);
	return false;
}

bool
Server::requestInputHandoffSourceRevoke()
{
	BaseClientProxy* source = m_inputHandoffSource;
	if (!m_inputHandoffPending || source == NULL ||
		source == m_primaryClient ||
		!source->supportsInputLeaseRevokeAck()) {
		return false;
	}

	cleanupInputHandoffTimer();
	m_inputHandoffSourceRevokePending = true;
	m_inputHandoffSourceRevoked = false;
	m_inputHandoffPhaseTimer.reset();
	LOG((CLOG_DEBUG1
		"revoking source input lease from \"%s\", handoff=%u epoch=%u",
		getName(source).c_str(), m_inputHandoffSeqNum,
		m_inputHandoffSourceLeaseSeqNum));
	source->requestInputLeaseRevoke(
		m_inputHandoffSeqNum, m_inputHandoffSourceLeaseSeqNum);
	m_inputHandoffTimer = m_events->newOneShotTimer(
		kInputHandoffSourceRevokeTimeoutSeconds, NULL);
	m_events->adoptHandler(Event::kTimer, m_inputHandoffTimer,
		new TMethodEventJob<Server>(this,
			&Server::handleInputHandoffTimeout, NULL));
	return true;
}

void
Server::commitPreparedInputHandoff()
{
	if (!m_inputHandoffPending) {
		return;
	}

	BaseClientProxy* source = m_inputHandoffSource;
	BaseClientProxy* target = m_inputHandoffTarget;
	if (source == NULL || target == NULL || m_active != source ||
		m_clientSet.count(target) == 0) {
		cancelInputHandoff("source or target changed before commit", false);
		return;
	}

	const SInt32 x = m_inputHandoffX;
	const SInt32 y = m_inputHandoffY;
	const SInt32 sourceX = m_inputHandoffSourceX;
	const SInt32 sourceY = m_inputHandoffSourceY;
	const UInt32 seqNum = m_inputHandoffSeqNum;
	const EDirection guardDir = m_inputHandoffGuardDir;
	const bool forScreensaver = m_inputHandoffForScreensaver;
	const bool sourceWasRevoked = m_inputHandoffSourceRevoked;
	cleanupInputHandoffTimer();
	m_inputHandoffPending = false;
	m_inputHandoffSourceRevokePending = false;
	m_inputHandoffCommitReady = true;

	const bool committed =
		switchScreen(target, x, y, forScreensaver, guardDir);
	m_inputHandoffCommitReady = false;
	m_inputHandoffSourceRevoked = false;
	m_inputHandoffForScreensaver = false;
	m_inputHandoffGuardDir = kNoDirection;
	if (!committed) {
		m_inputHandoffCommitted = false;
		m_inputHandoffCommitAckPending = false;
		m_inputHandoffSource = NULL;
		m_inputHandoffTarget = NULL;
		target->abortEnter(seqNum);
		if (sourceWasRevoked && source != m_primaryClient &&
			m_clientSet.count(source) != 0 && m_active == source) {
			const UInt32 restoreSeqNum = ++m_seqNum;
			source->enter(sourceX, sourceY, restoreSeqNum,
				m_primaryClient->getToggleMask(), false);
			trackCommittedInputHandoff(m_primaryClient, source,
				restoreSeqNum, sourceX, sourceY, kNoDirection);
		}
		else {
			reanchorActiveAfterFailedSwitch(target);
		}
		return;
	}

	if (target == m_primaryClient) {
		finishCommittedInputHandoff();
	}
	else {
		trackCommittedInputHandoff(source, target, seqNum,
			sourceX, sourceY, guardDir);
	}
}

void
Server::cleanupInputHandoffTimer()
{
	if (m_inputHandoffTimer != NULL) {
		m_events->removeHandler(Event::kTimer, m_inputHandoffTimer);
		m_events->deleteTimer(m_inputHandoffTimer);
		m_inputHandoffTimer = NULL;
	}
}

void
Server::cancelInputHandoff(const char* reason, bool reanchor, bool notifyTarget)
{
	if (!m_inputHandoffPending) {
		return;
	}

	BaseClientProxy* source = m_inputHandoffSource;
	BaseClientProxy* target = m_inputHandoffTarget;
	const UInt32 seqNum = m_inputHandoffSeqNum;
	const EDirection guardDir = m_inputHandoffGuardDir;
	LOG((CLOG_WARN "canceling input handoff to \"%s\", seq=%u: %s",
		target != NULL ? getName(target).c_str() : "unknown", seqNum, reason));

	cleanupInputHandoffTimer();
	m_inputHandoffPending = false;
	m_inputHandoffCommitReady = false;
	m_inputHandoffCommitted = false;
	m_inputHandoffCommitAckPending = false;
	m_inputHandoffSourceRevokePending = false;
	m_inputHandoffSourceRevoked = false;
	m_inputHandoffSource = NULL;
	m_inputHandoffTarget = NULL;
	m_inputHandoffSourceLeaseSeqNum = 0;
	m_inputHandoffForScreensaver = false;
	m_inputHandoffGuardDir = kNoDirection;

	if (notifyTarget && target != NULL && m_clientSet.count(target) != 0) {
		target->abortEnter(seqNum);
	}

	if (reanchor && source != NULL && m_active == source) {
		if (source == m_primaryClient) {
			recoverPrimaryAfterSwitchFailure(m_x, m_y, guardDir);
		}
		else {
			reanchorActiveAfterFailedSwitch(target);
		}
	}
}

void
Server::trackCommittedInputHandoff(BaseClientProxy* source,
									BaseClientProxy* target, UInt32 seqNum,
									SInt32 sourceX, SInt32 sourceY,
									EDirection guardDir)
{
	assert(target != NULL);
	m_inputHandoffCommitted = true;
	m_inputHandoffCommitAckPending = false;
	m_inputHandoffSourceRevokePending = false;
	m_inputHandoffSourceRevoked = false;
	m_inputHandoffSource = source;
	m_inputHandoffTarget = target;
	m_inputHandoffSeqNum = seqNum;
	m_inputHandoffSourceLeaseSeqNum = 0;
	m_inputHandoffForScreensaver = false;
	m_inputHandoffSourceX = sourceX;
	m_inputHandoffSourceY = sourceY;
	m_inputHandoffGuardDir = guardDir;

	LOG((CLOG_INFO
		"tracking committed input handoff from \"%s\" to \"%s\", seq=%u; rollback=%d,%d",
		source != NULL ? getName(source).c_str() : "local fallback",
		getName(target).c_str(), seqNum, sourceX, sourceY));
	if (target->supportsInputHandoffCommitAck()) {
		startInputHandoffCommitAckTimer();
	}
}

void
Server::startInputHandoffCommitAckTimer()
{
	cleanupInputHandoffTimer();
	m_inputHandoffCommitAckPending = true;
	m_inputHandoffPhaseTimer.reset();
	m_inputHandoffTimer =
		m_events->newOneShotTimer(kInputHandoffCommitAckTimeoutSeconds, NULL);
	m_events->adoptHandler(Event::kTimer, m_inputHandoffTimer,
		new TMethodEventJob<Server>(this,
			&Server::handleInputHandoffTimeout, NULL));
}

void
Server::finishCommittedInputHandoff()
{
	cleanupInputHandoffTimer();
	m_inputHandoffCommitted = false;
	m_inputHandoffCommitAckPending = false;
	m_inputHandoffSourceRevokePending = false;
	m_inputHandoffSourceRevoked = false;
	m_inputHandoffSource = NULL;
	m_inputHandoffTarget = NULL;
	m_inputHandoffSourceLeaseSeqNum = 0;
	m_inputHandoffForScreensaver = false;
	m_inputHandoffGuardDir = kNoDirection;
}

void
Server::rollbackCommittedInputHandoff(const char* reason)
{
	if (!m_inputHandoffCommitted) {
		return;
	}

	BaseClientProxy* source = m_inputHandoffSource;
	BaseClientProxy* target = m_inputHandoffTarget;
	const UInt32 seqNum = m_inputHandoffSeqNum;
	const SInt32 sourceX = m_inputHandoffSourceX;
	const SInt32 sourceY = m_inputHandoffSourceY;
	LOG((CLOG_WARN
		"rolling back committed input handoff to \"%s\", seq=%u: %s",
		target != NULL ? getName(target).c_str() : "unknown", seqNum, reason));

	finishCommittedInputHandoff();

	bool restoreAccepted = false;
	bool restored = false;
	if (source != NULL && m_clientSet.count(source) != 0) {
		// The rejected target is not a confirmed rollback source. Restore the
		// previous lease first, then track that enter against the local primary.
		restoreAccepted = switchScreen(source, sourceX, sourceY, false,
			kNoDirection, false);
		restored = restoreAccepted && m_active == source;
		if (restoreAccepted && !restored && m_inputHandoffPending &&
			m_inputHandoffTarget == source) {
			return;
		}
		if (restored && source != m_primaryClient &&
			source->supportsInputHandoff()) {
			SInt32 fallbackX = 0;
			SInt32 fallbackY = 0;
			BaseClientProxy* fallback = NULL;
			if (getPrimaryRecoveryPoint(source, fallbackX, fallbackY)) {
				fallback = m_primaryClient;
			}
			trackCommittedInputHandoff(fallback, source, m_seqNum,
				fallbackX, fallbackY, kNoDirection);
		}
	}
	if (!restored && target != NULL && m_active == target) {
		LOG((CLOG_WARN
			"committed handoff rollback could not switch to its source; forcing local recovery"));
		if (m_clientSet.count(target) != 0) {
			target->leave();
		}
		forceLeaveClient(target);
	}
}

void
Server::handleInputHandoffReady(const Event& event, void* vclient)
{
	BaseClientProxy* client = static_cast<BaseClientProxy*>(vclient);
	BaseClientProxy::InputHandoffReadyInfo* info =
		static_cast<BaseClientProxy::InputHandoffReadyInfo*>(
			event.getDataObject() != NULL ? event.getDataObject() :
			static_cast<EventData*>(event.getData()));
	if (info != NULL && m_activeReplacementTarget != NULL &&
		client == m_activeReplacementSource &&
		info->m_seqNum == m_activeReplacementSeqNum) {
		if (!info->m_ready) {
			cancelActiveClientReplacement(
				"source rejected lease revoke", true);
			return;
		}
		LOG((CLOG_INFO
			"active client source acknowledged replacement revoke, seq=%u",
			info->m_seqNum));
		completeActiveClientReplacement();
		return;
	}
	if (info != NULL && m_inputHandoffCommitted &&
		client == m_inputHandoffTarget &&
		info->m_seqNum == m_inputHandoffSeqNum &&
		m_active == m_inputHandoffTarget) {
		if (!info->m_ready) {
			rollbackCommittedInputHandoff("target rejected committed lease");
			return;
		}
		if (m_inputHandoffCommitAckPending) {
			LOG((CLOG_INFO
				"target acknowledged committed input handoff, seq=%u",
				info->m_seqNum));
			finishCommittedInputHandoff();
			return;
		}
	}

	if (info != NULL && m_inputHandoffPending &&
		m_inputHandoffSourceRevokePending &&
		client == m_inputHandoffSource &&
		info->m_seqNum == m_inputHandoffSeqNum) {
		cleanupInputHandoffTimer();
		m_inputHandoffSourceRevokePending = false;
		if (!info->m_ready) {
			cancelInputHandoff("source rejected lease revoke", true);
			return;
		}
		LOG((CLOG_INFO
			"source acknowledged input lease revoke, seq=%u",
			info->m_seqNum));
		m_inputHandoffSourceRevoked = true;
		commitPreparedInputHandoff();
		return;
	}
	if (m_inputHandoffSourceRevokePending) {
		LOG((CLOG_DEBUG1
			"ignoring readiness while source lease revoke is pending"));
		return;
	}

	if (info == NULL || !m_inputHandoffPending ||
		client != m_inputHandoffTarget ||
		info->m_seqNum != m_inputHandoffSeqNum) {
		LOG((CLOG_DEBUG1 "ignoring stale input handoff readiness"));
		return;
	}

	if (!info->m_ready) {
		cancelInputHandoff("target rejected readiness", true);
		return;
	}
	if (m_active != m_inputHandoffSource ||
		m_clientSet.count(client) == 0) {
		cancelInputHandoff("source or target changed before commit", false);
		return;
	}

	if (requestInputHandoffSourceRevoke()) {
		return;
	}
	commitPreparedInputHandoff();
}

void
Server::handleInputHandoffTimeout(const Event&, void*)
{
	const bool waitingForSourceRevoke = m_inputHandoffSourceRevokePending;
	const bool waitingForCommit = m_inputHandoffCommitAckPending;
	const double deadline = waitingForSourceRevoke
		? kInputHandoffSourceRevokeTimeoutSeconds
		: (waitingForCommit ? kInputHandoffCommitAckTimeoutSeconds
			: kInputHandoffTimeoutSeconds);
	const char* phase = waitingForSourceRevoke ? "source-revoke" :
		(waitingForCommit ? "commit" : "prepare");
	LOG((CLOG_WARN
		"input handoff timeout: phase=%s seq=%u elapsed=%.3fs deadline=%.3fs",
		phase, m_inputHandoffSeqNum,
		m_inputHandoffPhaseTimer.getTime(), deadline));
	if (waitingForSourceRevoke) {
		BaseClientProxy* source = m_inputHandoffSource;
		cancelInputHandoff("source lease revoke timed out", false);
		if (source != NULL && source != m_primaryClient &&
			m_clientSet.count(source) != 0) {
			closeClient(source, kMsgCClose, true);
		}
	}
	else if (m_inputHandoffCommitAckPending) {
		rollbackCommittedInputHandoff("target commit acknowledgment timed out");
	}
	else {
		cancelInputHandoff("target readiness timed out", true);
	}
}

void
Server::handleActiveClientReplacementTimeout(const Event&, void*)
{
	BaseClientProxy* source = m_activeReplacementSource;
	if (source == NULL || m_activeReplacementTarget == NULL) {
		cleanupActiveClientReplacementTimer();
		return;
	}

	cleanupActiveClientReplacementTimer();
	LOG((CLOG_WARN
		"active client replacement revoke timed out for \"%s\"; fencing old transport before new enter",
		getName(source).c_str()));
	if (m_clientSet.count(source) != 0) {
		closeClient(source, kMsgCClose, true);
		try {
			source->getStream()->close();
		}
		catch (...) {
			LOG((CLOG_WARN
				"old active client stream close raised while fencing replacement"));
		}
		return;
	}

	// The source was already removed, so no remote input path remains.
	completeActiveClientReplacement();
}

bool
Server::canLeavePrimaryNow(const char* reason, EDirection dir)
{
	if (m_active != m_primaryClient || !m_primaryLeaveFailedRecently) {
		return true;
	}

	// Only suppress a repeated push against the edge whose handoff just
	// failed.  Explicit switches and a different edge express a new intent and
	// must not inherit an arbitrary time blackout.
	if (dir == kNoDirection || m_primaryLeaveFailureDir == kNoDirection ||
		dir != m_primaryLeaveFailureDir) {
		return true;
	}

	LOG((CLOG_WARN "suppressing one synthetic %s event on failed %s edge",
		reason, Config::dirName(dir)));
	// The platform warp used for recovery is drained before the server can
	// observe an inward motion. Consume exactly one same-edge event instead of
	// requiring the user to move away from the edge before retrying.
	m_primaryLeaveFailedRecently = false;
	m_primaryLeaveFailureDir = kNoDirection;
	stopSwitch();
	return false;
}

void
Server::clearPrimaryLeaveFailureIfMovedAway(SInt32 x, SInt32 y)
{
	if (!m_primaryLeaveFailedRecently ||
		m_primaryLeaveFailureDir == kNoDirection) {
		return;
	}

	SInt32 ax, ay, aw, ah;
	m_primaryClient->getShape(ax, ay, aw, ah);
	if (aw < kMinUsableScreenDimension || ah < kMinUsableScreenDimension) {
		return;
	}

	const SInt32 margin = (std::max)(
		static_cast<SInt32>(getJumpZoneSize(m_primaryClient) + 8),
		kSwitchEdgeHysteresisInset);
	bool movedAway = false;
	switch (m_primaryLeaveFailureDir) {
	case kLeft:
		movedAway = x >= ax + margin;
		break;
	case kRight:
		movedAway = x <= ax + aw - margin - 1;
		break;
	case kTop:
		movedAway = y >= ay + margin;
		break;
	case kBottom:
		movedAway = y <= ay + ah - margin - 1;
		break;
	case kNoDirection:
		break;
	}

	if (movedAway) {
		LOG((CLOG_DEBUG1 "clearing failed %s edge gate after pointer moved to %d,%d",
			Config::dirName(m_primaryLeaveFailureDir), x, y));
		m_primaryLeaveFailedRecently = false;
		m_primaryLeaveFailureDir = kNoDirection;
	}
}

void
Server::recoverPrimaryAfterSwitchFailure(SInt32 x, SInt32 y,
										  EDirection dir)
{
	SInt32 ax, ay, aw, ah;
	m_primaryClient->getShape(ax, ay, aw, ah);
	if (aw < kMinUsableScreenDimension || ah < kMinUsableScreenDimension) {
		LOG((CLOG_WARN "cannot reanchor primary after failed leave; unusable primary shape %d,%d %dx%d",
			ax, ay, aw, ah));
		m_primaryLeaveFailedRecently = false;
		m_primaryLeaveFailureDir = kNoDirection;
		return;
	}

	const SInt32 zone = getJumpZoneSize(m_primaryClient);
	const SInt32 margin = std::max<SInt32>(zone + 8, 16);
	const SInt32 insetX = std::min<SInt32>(margin, (aw - 1) / 2);
	const SInt32 insetY = std::min<SInt32>(margin, (ah - 1) / 2);
	SInt32 safeX = x;
	SInt32 safeY = y;
	if (safeX < ax + insetX) {
		safeX = ax + insetX;
	}
	else if (safeX >= ax + aw - insetX) {
		safeX = ax + aw - insetX - 1;
	}
	if (safeY < ay + insetY) {
		safeY = ay + insetY;
	}
	else if (safeY >= ay + ah - insetY) {
		safeY = ay + ah - insetY - 1;
	}

	LOG((CLOG_WARN "reanchoring primary at %d,%d after failed leave", safeX, safeY));
	m_x = safeX;
	m_y = safeY;
	discardPendingMouseMove();
	m_primaryClient->mouseMove(m_x, m_y);
	m_primaryClient->refreshKeyState();
	noSwitch(m_x, m_y);

	if (dir == kNoDirection) {
		const SInt32 leftDistance = (std::max)(x - ax, static_cast<SInt32>(0));
		const SInt32 rightDistance = (std::max)(
			ax + aw - 1 - x, static_cast<SInt32>(0));
		const SInt32 topDistance = (std::max)(y - ay, static_cast<SInt32>(0));
		const SInt32 bottomDistance = (std::max)(
			ay + ah - 1 - y, static_cast<SInt32>(0));
		SInt32 nearestDistance = margin + 1;
		if (leftDistance <= margin && leftDistance < nearestDistance) {
			dir = kLeft;
			nearestDistance = leftDistance;
		}
		if (rightDistance <= margin && rightDistance < nearestDistance) {
			dir = kRight;
			nearestDistance = rightDistance;
		}
		if (topDistance <= margin && topDistance < nearestDistance) {
			dir = kTop;
			nearestDistance = topDistance;
		}
		if (bottomDistance <= margin && bottomDistance < nearestDistance) {
			dir = kBottom;
		}
	}

	m_primaryLeaveFailedRecently = dir != kNoDirection;
	m_primaryLeaveFailureDir = dir;
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

    const bool asyncPrimarySnapshots =
        m_screen != NULL && m_screen->getPlatformScreen() != NULL &&
        m_screen->getPlatformScreen()->hasAsyncClipboardSnapshots();

    const std::string primaryName = getName(m_primaryClient);
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        ClipboardInfo& clipboard = m_clipboards[id];
        const bool pendingFetch = clipboard.m_clipboardOwner == primaryName &&
            clipboard.m_pendingClipboardFetch;
        if (pendingFetch) {
            onClipboardChanged(m_primaryClient, id, clipboard.m_clipboardSeqNum);
            continue;
        }

        // A remote screen has announced a newer clipboard revision but has
        // not committed its payload yet. The primary fallback snapshot is
        // stale in this interval and must not take ownership back.
        if (clipboard.m_pendingClipboardFetch) {
            continue;
        }

        // X11 only reports SelectionClear after Weave has owned a selection.
        // Snapshot the regular clipboard on every primary leave so the first
        // local copy after startup is not missed. PRIMARY selection remains
        // event-driven to avoid unnecessary reads and switch latency.
        if (id == kClipboardClipboard) {
            // Async platforms emit clipboardChanged only after their worker has
            // committed a generation. An opportunistic cache read here could
            // observe an unannounced snapshot and the legacy retry helper would
            // sleep on the input event thread while a snapshot is still pending.
            if (asyncPrimarySnapshots) {
                continue;
            }

            const std::string previousOwner = clipboard.m_clipboardOwner;
            const bool previousPendingFetch = clipboard.m_pendingClipboardFetch;

            Clipboard observedClipboard;
            if (!readClipboardWithRetry(m_primaryClient, id, observedClipboard)) {
                continue;
            }

            RemoteFileClipboard::Data observedFileClipboard;
            const bool materializedFileEcho =
                !m_readyFileClipboardSession.empty() &&
                m_readyFileClipboardRevision == m_clipboardRevision &&
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
                clipboard.m_pendingClipboardFetch = false;
                m_readyFileClipboardSession.clear();
                m_readyFileClipboardPaths.clear();
                m_readyFileClipboardRevision.reset();
                continue;
            }

            const std::string observedData = observedClipboard.marshall();
            const bool observedEmpty = observedData.size() == sizeof(UInt32);
            if (clipboard.m_clipboardData.matches(observedData) ||
                (previousOwner != primaryName && observedEmpty)) {
                continue;
            }

            clipboard.m_clipboardOwner = primaryName;
            clipboard.m_pendingClipboardFetch = true;
            if (!onClipboardChanged(m_primaryClient, id,
                    clipboard.m_clipboardSeqNum, &observedClipboard)) {
                clipboard.m_clipboardOwner = previousOwner;
                clipboard.m_pendingClipboardFetch = previousPendingFetch;
            }
        }
    }
}

void
Server::scheduleClipboardSync(bool fetchPrimary)
{
	m_clipboardFetchPending = m_clipboardFetchPending || fetchPrimary;
	if (m_clipboardSyncTimer != NULL) {
		return;
	}

	m_clipboardSyncTimer =
		m_events->newOneShotTimer(kClipboardSyncDelaySeconds, NULL);
	m_events->adoptHandler(Event::kTimer, m_clipboardSyncTimer,
		new TMethodEventJob<Server>(this, &Server::handleClipboardSync));
}

void
Server::handleClipboardSync(const Event&, void*)
{
	EventQueueTimer* timer = m_clipboardSyncTimer;
	m_clipboardSyncTimer = NULL;
	if (timer != NULL) {
		m_events->removeHandler(Event::kTimer, timer);
		m_events->deleteTimer(timer);
	}

	const bool fetchPrimary = m_clipboardFetchPending;
	m_clipboardFetchPending = false;
	if (!m_enableClipboard) {
		return;
	}

	if (fetchPrimary) {
		fetchPendingPrimaryClipboards();
	}
	replayClipboardsToActive();
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
        if (clipboard.m_clipboardOwner == activeName) {
            LOG((CLOG_DEBUG "not replaying clipboard %d to its current owner \"%s\"",
                 id, activeName.c_str()));
            continue;
        }
		if (id == kClipboardClipboard) {
			RemoteFileClipboard::Data remoteFileClipboard;
			if (RemoteFileClipboard::readFromClipboard(clipboard.m_clipboard, remoteFileClipboard) &&
				remoteFileClipboard.mode == RemoteFileClipboard::Mode::SourcePaths) {
				if (clipboard.m_clipboardOwner == primaryName &&
					m_active != m_primaryClient &&
					m_active->supportsTransactionalFileTransfer()) {
					m_active->setClipboard(id, &clipboard.m_clipboard);
                    if (!m_clipboardRevision.valid()) {
                        m_clipboardRevision.advance();
                    }
                    sendClipboardSelectionToClient(
                        m_active, remoteFileClipboard.paths,
                        remoteFileClipboard.sessionId,
                        m_clipboardRevision);
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
	if (getPrimaryRecoveryPoint(m_active, x, y)) {
		if (!switchScreen(m_primaryClient, x, y, false)) {
			reanchorActiveAfterFailedSwitch(m_primaryClient);
		}
	}
	else {
		LOG((CLOG_WARN "primary shape is not valid; notifying active client to leave before local fallback"));
		m_active->leave();
		forceLeaveClient(m_active);
	}

	m_primaryLeaveFailedRecently = false;
	m_primaryLeaveFailureDir = kNoDirection;
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
Server::rememberPrimaryReturnAnchor(BaseClientProxy* dst, SInt32 x, SInt32 y,
									 EDirection dir)
{
	if (dst == NULL) {
		m_primaryReturnAnchorActive = false;
		m_primaryReturnAnchorClientName.clear();
		m_primaryReturnAnchorDir = kNoDirection;
		return;
	}

	m_primaryReturnAnchorActive = true;
	m_primaryReturnAnchorClientName = getName(dst);
	m_primaryReturnAnchorDir = dir;
	m_primaryReturnAnchorX = x;
	m_primaryReturnAnchorY = y;
	LOG((CLOG_INFO "remembered primary return anchor for \"%s\" at %d,%d",
		m_primaryReturnAnchorClientName.c_str(), x, y));
}

void
Server::adjustPrimaryReturnPoint(BaseClientProxy* src, SInt32& x, SInt32& y)
{
	if (!m_primaryReturnAnchorActive || src == NULL ||
		getName(src) != m_primaryReturnAnchorClientName) {
		return;
	}

	const SInt32 originalX = x;
	const SInt32 originalY = y;
	SInt32 ax, ay, aw, ah;
	m_primaryClient->getShape(ax, ay, aw, ah);
	if (m_primaryReturnAnchorDir != kNoDirection &&
		aw >= kMinUsableScreenDimension && ah >= kMinUsableScreenDimension) {
		const SInt32 maxInset = (std::max)(static_cast<SInt32>(1),
			static_cast<SInt32>((std::min)(aw, ah) / 8));
		const SInt32 inset = (std::min)(kSwitchEdgeHysteresisInset, maxInset);
		// A relative-motion overshoot is expressed in the source screen's
		// coordinate space.  Carrying it into an aggregate multi-monitor primary
		// can land thousands of pixels away from the physical edge that the user
		// crossed.  Re-enter at the remembered physical edge and preserve only
		// the orthogonal coordinate.
		switch (m_primaryReturnAnchorDir) {
		case kLeft:
			x = m_primaryReturnAnchorX + inset;
			break;
		case kRight:
			x = m_primaryReturnAnchorX - inset;
			break;
		case kTop:
			y = m_primaryReturnAnchorY + inset;
			break;
		case kBottom:
			y = m_primaryReturnAnchorY - inset;
			break;
		case kNoDirection:
			break;
		}
	}

	bool adjustedToOutput = false;
	if (m_screen != NULL && m_screen->getPlatformScreen() != NULL) {
		adjustedToOutput =
			m_screen->getPlatformScreen()->adjustPointToVisibleAreaNearAnchor(
				m_primaryReturnAnchorX, m_primaryReturnAnchorY, x, y);
	}
	if (!adjustedToOutput && aw >= kMinUsableScreenDimension &&
		ah >= kMinUsableScreenDimension) {
		x = (std::max)(ax, (std::min)(x, ax + aw - 1));
		y = (std::max)(ay, (std::min)(y, ay + ah - 1));
	}

	if (x != originalX || y != originalY) {
		LOG((CLOG_INFO "anchored primary return from \"%s\" at %d,%d to %d,%d using primary exit %d,%d",
			getName(src).c_str(), originalX, originalY, x, y,
			m_primaryReturnAnchorX, m_primaryReturnAnchorY));
	}
}

bool
Server::getPrimaryRecoveryPoint(BaseClientProxy* src, SInt32& x, SInt32& y)
{
	if (m_primaryClient == NULL) {
		return false;
	}
	SInt32 sx, sy, sw, sh;
	m_primaryClient->getShape(sx, sy, sw, sh);
	if (sw < kMinUsableScreenDimension || sh < kMinUsableScreenDimension) {
		return false;
	}

	if (m_primaryReturnAnchorActive && src != NULL &&
		getName(src) == m_primaryReturnAnchorClientName) {
		x = m_primaryReturnAnchorX;
		y = m_primaryReturnAnchorY;
		adjustPrimaryReturnPoint(src, x, y);
		if (clampToClientShape(m_primaryClient, x, y)) {
			LOG((CLOG_INFO "recovering primary near remembered edge at %d,%d",
				x, y));
			return true;
		}
	}

	x = 0;
	y = 0;
	m_primaryClient->getCursorPos(x, y);
	if (x >= sx && x < sx + sw && y >= sy && y < sy + sh) {
		LOG((CLOG_INFO "recovering primary at last local cursor position %d,%d",
			x, y));
		return true;
	}

	x = 0;
	y = 0;
	m_primaryClient->getCursorCenter(x, y);
	if (clampToClientShape(m_primaryClient, x, y)) {
		LOG((CLOG_WARN "falling back to primary center for recovery at %d,%d",
			x, y));
		return true;
	}

	return false;
}

bool
Server::isSwitchOkay(BaseClientProxy* newScreen,
				EDirection dir, SInt32 x, SInt32 y,
				SInt32 xActive, SInt32 yActive)
{
	LOG((CLOG_DEBUG1 "try to leave \"%s\" on %s", getName(m_active).c_str(), Config::dirName(dir)));

	if (!canLeavePrimaryNow("edge switch", dir)) {
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

	// Platform backends must filter their own warp artifacts. The first
	// reverse motion here may be the user's only attempt to return, so the
	// routing layer must never discard it based on timing or direction.

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
	// A prepared target is speculative.  Once CIRV is sent, the source may
	// already have stopped injecting input, so only its ACK or timeout may end
	// the transaction.
	if (m_inputHandoffPending && !m_inputHandoffSourceRevokePending) {
		bool nearPendingEdge = false;
		if (m_inputHandoffSource != NULL) {
			SInt32 sx, sy, sw, sh;
			m_inputHandoffSource->getShape(sx, sy, sw, sh);
			const SInt32 margin = (std::max)(
				static_cast<SInt32>(2 * kSwitchEdgeHysteresisInset),
				static_cast<SInt32>(getJumpZoneSize(m_inputHandoffSource) + 8));
			if (sw >= kMinUsableScreenDimension &&
				sh >= kMinUsableScreenDimension) {
				switch (m_inputHandoffGuardDir) {
				case kLeft:
					nearPendingEdge = x <= sx + margin;
					break;
				case kRight:
					nearPendingEdge = x >= sx + sw - 1 - margin;
					break;
				case kTop:
					nearPendingEdge = y <= sy + margin;
					break;
				case kBottom:
					nearPendingEdge = y >= sy + sh - 1 - margin;
					break;
				case kNoDirection:
					break;
				}
			}
		}

		if (!nearPendingEdge) {
			cancelInputHandoff("pointer left the switch edge", false);
		}
	}
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
	if (info->m_id == kClipboardClipboard) {
		supersedeFileClipboard("clipboard grab");
	}

	// mark screen as owning clipboard
	LOG((CLOG_INFO "screen \"%s\" grabbed clipboard %d from \"%s\"", getName(grabber).c_str(), info->m_id, clipboard.m_clipboardOwner.c_str()));
	if (!clipboard.m_hasClipboardRollback) {
		clipboard.m_previousClipboardOwner = clipboard.m_clipboardOwner;
		clipboard.m_previousClipboardSeqNum = clipboard.m_clipboardSeqNum;
		clipboard.m_hasClipboardRollback = true;
	}
	clipboard.m_clipboardOwner  = getName(grabber);
	clipboard.m_clipboardSeqNum = info->m_sequenceNumber;
	clipboard.m_pendingClipboardFetch = true;

	if (grabber == m_primaryClient) {
		const bool asyncSnapshot =
			m_screen != NULL && m_screen->getPlatformScreen() != NULL &&
			m_screen->getPlatformScreen()->hasAsyncClipboardSnapshots();
		if (asyncSnapshot) {
			// The platform will emit clipboardChanged only after its isolated
			// worker has committed this owner generation.
			LOG((CLOG_DEBUG "waiting for clipboard %d snapshot from primary \"%s\"",
				info->m_id, getName(grabber).c_str()));
		}
		else {
			// Legacy platforms retain their deferred main-thread read until they
			// gain an isolated snapshot implementation.
			scheduleClipboardSync(true);
		}
	}
	else {
		LOG((CLOG_DEBUG "waiting for clipboard %d payload from remote \"%s\"",
			info->m_id, getName(grabber).c_str()));
	}
}

void
Server::commitClipboardFetch(ClipboardID id)
{
	ClipboardInfo& clipboard = m_clipboards[id];
	clipboard.m_pendingClipboardFetch = false;
	clipboard.m_previousClipboardOwner.clear();
	clipboard.m_previousClipboardSeqNum = 0;
	clipboard.m_hasClipboardRollback = false;
}

void
Server::rollbackClipboardFetch(ClipboardID id)
{
	ClipboardInfo& clipboard = m_clipboards[id];
	if (clipboard.m_hasClipboardRollback) {
		clipboard.m_clipboardOwner = clipboard.m_previousClipboardOwner;
		clipboard.m_clipboardSeqNum = clipboard.m_previousClipboardSeqNum;
	}
	clipboard.m_pendingClipboardFetch = false;
	clipboard.m_previousClipboardOwner.clear();
	clipboard.m_previousClipboardSeqNum = 0;
	clipboard.m_hasClipboardRollback = false;
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
Server::handleClipboardPublished(const Event& event, void*)
{
	const IScreen::ClipboardPublicationInfo* info =
		static_cast<const IScreen::ClipboardPublicationInfo*>(event.getData());
	if (info == NULL || info->m_id != kClipboardClipboard ||
		info->m_publicationId == 0 ||
		info->m_publicationId !=
			m_pendingMaterializedClipboardPublicationId) {
		return;
	}

	const std::uint64_t publicationId = info->m_publicationId;
	if (info->m_result !=
			IScreen::ClipboardPublicationResult::Succeeded) {
		LOG((CLOG_WARN
			"remote file clipboard publication did not commit: session=%s publication=%llu result=%s",
			m_pendingMaterializedClipboardSession.c_str(),
			static_cast<unsigned long long>(publicationId),
			info->m_result == IScreen::ClipboardPublicationResult::Superseded
				? "superseded" : "failed"));
		clearMaterializedClipboardPublication();
		return;
	}

	if (!m_pendingMaterializedClipboardData ||
		m_pendingMaterializedClipboardRevision != m_clipboardRevision ||
		m_pendingMaterializedClipboardRevision !=
			m_readyFileClipboardRevision ||
		m_pendingMaterializedClipboardSession.empty() ||
		m_pendingMaterializedClipboardSession !=
			m_readyFileClipboardSession) {
		LOG((CLOG_INFO
			"ignoring committed stale file clipboard publication: publication=%llu",
			static_cast<unsigned long long>(publicationId)));
		clearMaterializedClipboardPublication();
		return;
	}

	const std::shared_ptr<const String> data =
		m_pendingMaterializedClipboardData;
	const std::string sessionId =
		m_pendingMaterializedClipboardSession;
	const std::size_t pathCount = m_readyFileClipboardPaths.size();
	commitMaterializedFileClipboard(
		data, sessionId, pathCount, publicationId);
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
	const bool completesActiveReplacement =
		client == m_activeReplacementSource;
	if (client == m_activeReplacementTarget) {
		cancelActiveClientReplacement(
			"replacement target disconnected", false);
	}
	abortFileReceiveSource(client);
	removeActiveClient(client);
	removeOldClient(client);
	if (completesActiveReplacement) {
		completeActiveClientReplacement();
	}

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
	const bool completesActiveReplacement =
		client == m_activeReplacementSource;
	if (client == m_activeReplacementTarget) {
		cancelActiveClientReplacement(
			"replacement target close timed out", false);
	}
	LOG((CLOG_NOTE "forced disconnection of client \"%s\"", getName(client).c_str()));
	abortFileReceiveSource(client);
	removeOldClient(client);
	if (completesActiveReplacement) {
		completeActiveClientReplacement();
	}

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
	FileReceiveCompletionInfo* info =
		static_cast<FileReceiveCompletionInfo*>(event.getDataObject());
	const std::uint64_t generation = info == NULL ?
		m_fileReceiveSession.generation() : info->m_generation;
	onFileRecieveCompleted(generation);
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

	if (!info->m_revision.valid() || info->m_revision != m_clipboardRevision) {
		LOG((CLOG_WARN
			"ignoring superseded file clipboard ready event: session=%s revision=%llu current=%llu",
			info->m_sessionId.c_str(),
			static_cast<unsigned long long>(info->m_revision.sequence()),
			static_cast<unsigned long long>(m_clipboardRevision.sequence())));
		return;
	}

	if (!info->m_publishClipboard &&
		(m_remoteFileClipboardSession.empty() ||
		 m_remoteFileClipboardSession != info->m_sessionId ||
		 m_remoteFileClipboardRevision != info->m_revision)) {
		LOG((CLOG_WARN "ignoring stale remote clipboard ready event: session=%s current=%s",
			info->m_sessionId.c_str(), m_remoteFileClipboardSession.c_str()));
		return;
	}

	m_readyFileClipboardSession = info->m_sessionId;
	m_readyFileClipboardPaths = info->m_paths;
	m_readyFileClipboardRevision = info->m_revision;
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
	serviceSendFileCompletion();
	if (!m_sendFileTransactionState && m_sendFileThread != NULL) {
		scheduleSendFileReap();
	}
	if (m_sendFileThread == NULL) {
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
					ClipboardID id, UInt32 seqNum,
					const Clipboard* snapshot)
{
	ClipboardInfo& clipboard = m_clipboards[id];

	// ignore update if sequence number is old
	if (seqNum < clipboard.m_clipboardSeqNum) {
		LOG((CLOG_INFO "ignored screen \"%s\" update of clipboard %d (missequenced)", getName(sender).c_str(), id));
		return false;
	}

	// An asynchronous platform completion can arrive after another screen has
	// taken ownership with the same sequence number. Assertions disappear in
	// release builds, so reject by runtime identity before reading any payload.
	const ClientList::const_iterator owner =
		m_clients.find(clipboard.m_clipboardOwner);
	if (owner == m_clients.end() || owner->second != sender) {
		LOG((CLOG_INFO
			"ignored screen \"%s\" update of clipboard %d (current owner is \"%s\")",
			getName(sender).c_str(), id, clipboard.m_clipboardOwner.c_str()));
		return false;
	}

	// Read into a candidate first. Wire-provided materialized paths must never
	// enter the authoritative clipboard, even when this update is rejected.
	Clipboard candidate;
	if (snapshot != NULL) {
		if (!Clipboard::copy(&candidate, snapshot)) {
			if (sender != m_primaryClient) {
				rollbackClipboardFetch(id);
			}
			return false;
		}
	}
	else {
		const bool asyncPrimarySnapshot =
			sender == m_primaryClient && m_screen != NULL &&
			m_screen->getPlatformScreen() != NULL &&
			m_screen->getPlatformScreen()->hasAsyncClipboardSnapshots();
		if (asyncPrimarySnapshot) {
			// clipboardChanged means the platform cache is already committed.
			// A miss indicates a superseded owner generation; retry sleeps here
			// would stall input without making that stale revision valid again.
				if (!sender->getClipboard(id, &candidate)) {
					return false;
				}
			}
			else if (!readClipboardWithRetry(
					sender, id, candidate)) {
				if (sender != m_primaryClient) {
					rollbackClipboardFetch(id);
				}
				return false;
			}
		}
		if (id == kClipboardClipboard) {
			RemoteFileClipboard::Data materializedClipboard;
			const bool containsFileList =
				RemoteFileClipboard::containsFileList(candidate);
			const bool hasFileClipboard =
				RemoteFileClipboard::readFromClipboard(
					candidate, materializedClipboard);
			if (sender != m_primaryClient && containsFileList &&
				!sender->supportsTransactionalFileTransfer()) {
				LOG((CLOG_WARN
					"rejected file clipboard metadata from legacy remote origin \"%s\"",
					getName(sender).c_str()));
				rollbackClipboardFetch(id);
				return false;
			}
			if (sender != m_primaryClient && containsFileList &&
				!hasFileClipboard) {
				LOG((CLOG_WARN
					"rejected invalid file clipboard metadata from remote origin \"%s\"",
					getName(sender).c_str()));
				rollbackClipboardFetch(id);
				return false;
			}
			if (hasFileClipboard &&
					materializedClipboard.mode ==
				RemoteFileClipboard::Mode::MaterializedPaths) {
			const bool trustedLocalEcho = sender == m_primaryClient &&
				!m_readyFileClipboardSession.empty() &&
				materializedClipboard.sessionId ==
					m_readyFileClipboardSession &&
				m_readyFileClipboardRevision == m_clipboardRevision &&
				RemoteFileClipboard::pathsMatch(
					materializedClipboard, m_readyFileClipboardPaths);
			if (!trustedLocalEcho) {
				LOG((CLOG_WARN
					"rejected materialized-path clipboard from untrusted wire origin \"%s\"",
					getName(sender).c_str()));
				rollbackClipboardFetch(id);
				return false;
			}

			if (!Clipboard::copy(&clipboard.m_clipboard, &candidate)) {
				rollbackClipboardFetch(id);
				return false;
			}
			clipboard.m_clipboardData.set(candidate.marshall());
			rollbackClipboardFetch(id);
			m_readyFileClipboardSession.clear();
			m_readyFileClipboardPaths.clear();
			m_readyFileClipboardRevision.reset();
			LOG((CLOG_INFO "suppressed trusted local materialized clipboard echo"));
			return true;
			}
		}
		RemoteFileClipboard::AutomaticSharingStatus clipboardSharingStatus =
		RemoteFileClipboard::AutomaticSharingStatus::Safe;
	if (id == kClipboardClipboard) {
		clipboardSharingStatus =
			RemoteFileClipboard::prepareForAutomaticClipboardSharing(candidate);
		if (clipboardSharingStatus ==
			RemoteFileClipboard::AutomaticSharingStatus::SafeAfterRemovingImageFileMetadata) {
			LOG((CLOG_INFO "stripped image file-transfer metadata before forwarding clipboard"));
		}
	}

	// ignore if data hasn't changed
    std::string data = candidate.marshall();
	if (clipboard.m_clipboardData.matches(data)) {
		LOG((CLOG_DEBUG "ignored screen \"%s\" update of clipboard %d (unchanged)", clipboard.m_clipboardOwner.c_str(), id));
		commitClipboardFetch(id);
		return true;
	}
	if (id == kClipboardClipboard) {
		supersedeFileClipboard("clipboard data update");
	}

	// got new data
	LOG((CLOG_INFO "screen \"%s\" updated clipboard %d", clipboard.m_clipboardOwner.c_str(), id));
	if (clipboardSharingStatus ==
		RemoteFileClipboard::AutomaticSharingStatus::ContainsFileList) {
		RemoteFileClipboard::Data sourceFileClipboard;
		std::string error;
		if (!RemoteFileClipboard::normalizeClipboard(
				candidate, &sourceFileClipboard, &error)) {
			LOG((CLOG_WARN "file clipboard metadata could not be normalized: %s",
				error.c_str()));
			rollbackClipboardFetch(id);
			return false;
		}

		data = candidate.marshall();
		if (!Clipboard::copy(&clipboard.m_clipboard, &candidate)) {
			rollbackClipboardFetch(id);
			return false;
		}
		m_readyFileClipboardSession.clear();
		m_readyFileClipboardPaths.clear();
		m_readyFileClipboardRevision.reset();
		if (sender == m_primaryClient) {
			m_remoteFileClipboardSession.clear();
			m_remoteFileClipboardRevision.reset();
			m_remoteFileClipboardOriginBinding.clear();
			LOG((CLOG_INFO
				"cached local file clipboard for transfer on the active remote screen: session=%s items=%lu",
				sourceFileClipboard.sessionId.c_str(),
				static_cast<unsigned long>(sourceFileClipboard.paths.size())));
		}
		else {
			m_remoteFileClipboardSession = sourceFileClipboard.sessionId;
			m_remoteFileClipboardRevision = m_clipboardRevision;
			m_remoteFileClipboardOriginBinding =
				sender->getConnectionBinding();
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
		commitClipboardFetch(id);
		return true;
	}

	if (!Clipboard::copy(&clipboard.m_clipboard, &candidate)) {
		rollbackClipboardFetch(id);
		return false;
	}
	clipboard.m_clipboardData.set(data);
	commitClipboardFetch(id);

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
Server::supersedeFileClipboard(const char* reason)
{
	m_clipboardRevision.advance();
	m_pendingFileClipboardPrefetchTarget = NULL;
	m_pendingFileClipboardPrefetchPaths.clear();
	m_pendingFileClipboardPrefetchSession.clear();
	m_pendingFileClipboardPrefetchRevision.reset();
	if (m_sendFileIsClipboardPrefetch && m_sendFileChunker) {
		m_sendFileChunker->interruptFile();
	}
	if (!m_remoteFileClipboardSession.empty()) {
		LOG((CLOG_INFO
			"superseding pending remote file clipboard: session=%s revision=%llu reason=%s",
			m_remoteFileClipboardSession.c_str(),
			static_cast<unsigned long long>(m_remoteFileClipboardRevision.sequence()),
			reason));
	}
	m_remoteFileClipboardSession.clear();
	m_remoteFileClipboardRevision.reset();
	m_remoteFileClipboardOriginBinding.clear();
	m_readyFileClipboardSession.clear();
	m_readyFileClipboardPaths.clear();
	m_readyFileClipboardRevision.reset();
	clearMaterializedClipboardPublication();
}

bool
Server::canReceiveFileChunk(BaseClientProxy* source,
	barrier::BulkChannel* channel) const
{
	if (source == NULL) {
		return false;
	}
	if (m_fileReceiveSource != NULL) {
		return m_fileReceiveSource == source &&
			m_fileReceiveBulkChannel == channel;
	}

	const FileReceiveSession::State state = m_fileReceiveSession.state();
	return state == FileReceiveSession::kIdle ||
		state == FileReceiveSession::kFailed;
}

void
Server::bindFileReceiveClipboardRevision(BaseClientProxy* source,
	barrier::BulkChannel* channel)
{
	m_fileReceiveSource = source;
	m_fileReceiveBulkChannel = channel;
	if (!m_clipboardRevision.valid()) {
		m_clipboardRevision.advance();
	}
	m_fileReceiveClipboardGeneration = m_fileReceiveSession.generation();
	m_fileReceiveClipboardRevision = m_clipboardRevision;
	m_fileReceiveRemoteFileClipboardSession.clear();
	m_fileReceiveClipboardOrigin.clear();
	if (!m_remoteFileClipboardSession.empty() &&
		m_remoteFileClipboardRevision == m_clipboardRevision) {
		m_fileReceiveRemoteFileClipboardSession = m_remoteFileClipboardSession;
		if (source != NULL) {
			m_fileReceiveClipboardOrigin = getName(source);
		}
	}
}

void
Server::completeFileReceiveRoute(BaseClientProxy* source,
	barrier::BulkChannel* channel)
{
	if (m_fileReceiveSource == source &&
		m_fileReceiveBulkChannel == channel) {
		if (channel != NULL) {
			const std::uint64_t generation = m_fileReceiveSession.generation();
			std::shared_ptr<barrier::BulkChannel> ownedChannel =
				source == NULL ? std::shared_ptr<barrier::BulkChannel>() :
				source->acquireBulkChannel();
			if (!ownedChannel || ownedChannel.get() != channel ||
				!channel->pauseInputForCommit(generation) ||
				!m_fileReceiveSession.installCommitBarrier(
					generation,
					channel->makeInputResumeCallback(generation),
					channel->makeInputProgressCallback(generation))) {
				channel->resumeInputAfterCommit(generation);
				LOG((CLOG_ERR
					"failed to install server bulk receive commit barrier, generation=%llu",
					static_cast<unsigned long long>(generation)));
				abortFileReceiveRoute(source, channel);
				channel->close();
				renewBulkChannel(source);
			}
		}
		m_fileReceiveSource = NULL;
		m_fileReceiveBulkChannel = NULL;
	}
}

void
Server::abortFileReceiveRoute(BaseClientProxy* source,
	barrier::BulkChannel* channel)
{
	if (m_fileReceiveSource != source ||
		m_fileReceiveBulkChannel != channel) {
		return;
	}
	cleanupFileReceiveCompletionPoll();
	FileChunk::releaseReceiveBuffer(m_fileReceiveSession);
	m_fileReceiveSource = NULL;
	m_fileReceiveBulkChannel = NULL;
	m_fileReceiveClipboardGeneration = 0;
	m_fileReceiveRemoteFileClipboardSession.clear();
	m_fileReceiveClipboardOrigin.clear();
	m_fileReceiveClipboardRevision.reset();
}

void
Server::abortFileReceiveSource(BaseClientProxy* source)
{
	if (m_fileReceiveSource == source) {
		abortFileReceiveRoute(source, m_fileReceiveBulkChannel);
	}
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
			const bool restoreAccepted =
				switchScreen(screen, m_xSaver, m_ySaver, false);
			restoredSaver = restoreAccepted && m_active == screen;
			if (!restoreAccepted) {
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
				index->second->keyDownBroadcast(id, mask, button);
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
				index->second->keyUpBroadcast(id, mask, button);
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
	clearPrimaryLeaveFailureIfMovedAway(m_x, m_y);

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
	if (!newScreen->supportsTransactionalFileTransfer()) {
		LOG((CLOG_WARN
			"not sending drag metadata to legacy client \"%s\"",
			newScreen->getName().c_str()));
		return;
	}
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
	if (!target->supportsTransactionalFileTransfer()) {
		LOG((CLOG_WARN
			"dropping file chunk for legacy client \"%s\"",
			target->getName().c_str()));
		if (m_sendFileChunker) {
			m_sendFileChunker->interruptFile();
		}
		m_sendFilePreflightFailed.store(true);
		m_sendFileCompletionPending = true;
		finishCompletedSendFileIfReady();
		return;
	}

	if (chunk->m_transactional) {
		ClientProxy1_12* transactionalTarget =
			dynamic_cast<ClientProxy1_12*>(target);
		if (transactionalTarget == NULL || !m_sendFileTransactionState ||
			m_sendFileTransactionState->transferId() != chunk->m_transferId ||
			chunk->m_transferId != m_sendFileTransferId) {
			LOG((CLOG_ERR
				"rejecting transactional file frame without matching 1.12 route"));
			if (m_sendFileTransactionState) {
				m_sendFileTransactionState->fail(
					barrier::FileTransferReason::kProtocolError);
			}
			m_sendFileCompletionPending = true;
			return;
		}

		const std::string binding = target->getConnectionBinding();
		barrier::FileTransferFrame frame;
		switch (chunk->m_chunk[0]) {
		case kDataStart: {
			UInt32 totalSize = 0;
			const char* first = &chunk->m_chunk[1];
			const char* last = first + chunk->m_dataSize;
			const std::from_chars_result parsed =
				std::from_chars(first, last, totalSize, 10);
			if (parsed.ec != std::errc() || parsed.ptr != last) {
				m_sendFileTransactionState->fail(
					barrier::FileTransferReason::kProtocolError);
				m_sendFileCompletionPending = true;
				return;
			}
			frame = barrier::FileTransferFrame::start(
				binding, chunk->m_transferId, totalSize,
				m_sendFileTransactionState->kind(),
				m_sendFileTransactionState->clipboardRevision(),
				m_sendFileTransactionState->clipboardSessionId());
			break;
		}
		case kDataChunk:
			frame = barrier::FileTransferFrame::data(
				binding, chunk->m_transferId, chunk->m_offset,
				std::string(&chunk->m_chunk[1], chunk->m_dataSize));
			break;
		case kDataEnd:
			frame = barrier::FileTransferFrame::end(
				binding, chunk->m_transferId, chunk->m_offset,
				std::string(&chunk->m_chunk[1], chunk->m_dataSize));
			break;
		case kDataCancel: {
			const barrier::FileTransferReason reason =
				chunk->m_transferReason ==
					barrier::FileTransferReason::kNone ?
					barrier::FileTransferReason::kCancelled :
					chunk->m_transferReason;
			frame = barrier::FileTransferFrame::cancel(
				binding, chunk->m_transferId, reason);
			break;
		}
		default:
			m_sendFileTransactionState->fail(
				barrier::FileTransferReason::kProtocolError);
			m_sendFileCompletionPending = true;
			return;
		}

		if (!transactionalTarget->sendTransactionalFileFrame(
				frame, m_sendFileTransactionState->startAcknowledged())) {
			LOG((CLOG_WARN
				"transactional file route failed, transfer=%u",
				chunk->m_transferId));
				m_sendFileTransactionState->connectionLost();
				m_sendFileCompletionPending = true;
				cleanupSendFileCancelAckTimeout();
				m_sendFileCancelAckPending = false;
				if (m_sendFileChunker) {
				m_sendFileChunker->interruptFile();
			}
			return;
		}

		if (frame.type == barrier::FileTransferFrameType::kStart) {
			m_sendFileStarted = true;
			m_sendFileCompletionPending = false;
		}
		else if (frame.type == barrier::FileTransferFrameType::kEnd) {
			m_sendFileCompletionPending = true;
		}
			else if (frame.type == barrier::FileTransferFrameType::kCancel) {
				m_sendFileCompletionPending = true;
				scheduleSendFileCancelAckTimeout();
		}
		finishCompletedSendFileIfReady();
		return;
	}

	// The route is pinned for the complete transfer. If bulk disconnects,
	// writes fail on that closed stream; remaining frames must never spill into
	// control and corrupt the peer's transfer state.
	if (m_sendFileBulkChannel) {
		if (!m_sendFileBulkChannel->isActive()) {
			if (m_sendFileChunker) {
				m_sendFileChunker->interruptFile();
			}
			if (completesTransfer) {
				m_sendFileCompletionPending = true;
				finishCompletedSendFileIfReady();
			}
			return;
		}
		FileChunk::send(m_sendFileBulkChannel->getStream(), chunk->m_chunk[0],
			&chunk->m_chunk[1], chunk->m_dataSize);
	}
	else {
		target->fileChunkSending(chunk->m_chunk[0], &chunk->m_chunk[1],
			chunk->m_dataSize);
	}
	if (chunk->m_chunk[0] == kDataStart) {
		m_sendFileStarted = true;
		m_sendFileCompletionPending = false;
	}
	if (completesTransfer) {
		m_sendFileCompletionPending = true;
		finishCompletedSendFileIfReady();
	}
}

void
Server::handleFileReceiveCompletionPoll(const Event&, void*)
{
	const std::uint64_t generation = m_fileReceiveCompletionGeneration;
	cleanupFileReceiveCompletionPoll();
	onFileRecieveCompleted(generation);
}

void
Server::scheduleFileReceiveCompletionPoll(std::uint64_t generation)
{
	if (m_fileReceiveCompletionTimer != NULL &&
		m_fileReceiveCompletionGeneration == generation) {
		return;
	}
	cleanupFileReceiveCompletionPoll();
	m_fileReceiveCompletionGeneration = generation;
	m_fileReceiveCompletionTimer =
		m_events->newOneShotTimer(kFileReceiveCompletionPollSeconds, NULL);
	if (m_fileReceiveCompletionTimer != NULL) {
		m_events->adoptHandler(Event::kTimer, m_fileReceiveCompletionTimer,
			new TMethodEventJob<Server>(this,
				&Server::handleFileReceiveCompletionPoll));
	}
}

void
Server::cleanupFileReceiveCompletionPoll()
{
	if (m_fileReceiveCompletionTimer != NULL) {
		m_events->removeHandler(Event::kTimer, m_fileReceiveCompletionTimer);
		m_events->deleteTimer(m_fileReceiveCompletionTimer);
		m_fileReceiveCompletionTimer = NULL;
	}
	m_fileReceiveCompletionGeneration = 0;
}

void
Server::onFileRecieveCompleted(std::uint64_t generation)
{
	if (!m_fileReceiveSession.matchesGeneration(generation)) {
		LOG((CLOG_DEBUG1 "ignoring stale file completion generation=%llu current=%llu",
			static_cast<unsigned long long>(generation),
			static_cast<unsigned long long>(m_fileReceiveSession.generation())));
		return;
	}
	if (m_fileReceiveSession.isFinalizing()) {
		scheduleFileReceiveCompletionPoll(generation);
		return;
	}
	if (isReceivedFileSizeValid()) {
		cleanupFileReceiveCompletionPoll();
		std::shared_ptr<CompletedFileTransfer> transfer = takeCompletedFileTransfer();
		startDropDirTransfer(transfer);
		return;
	}

	LOG((CLOG_ERR "received file completion with invalid size, expected=%llu actual=%llu",
		static_cast<unsigned long long>(m_fileReceiveSession.expectedSize()),
		static_cast<unsigned long long>(m_fileReceiveSession.receivedSize())));
	cleanupFileReceiveCompletionPoll();
	FileChunk::releaseReceiveBuffer(m_fileReceiveSession);
}

bool
Server::startDropDirTransfer(std::shared_ptr<CompletedFileTransfer> transfer)
{
	if (!transfer) {
		return false;
	}

	if (!reapWriteToDropDirThreadIfReady()) {
		return queueDropDirTransfer(transfer);
	}

	try {
		m_writeToDropDirThread = new Thread([this, transfer]() {
			write_to_drop_dir_thread(transfer);
			m_events->addEvent(Event(
				m_events->forFile().dropDirWriteFinished(), this));
		});
	}
	catch (...) {
		FileChunk::releaseReceiveBuffer(
			transfer->data, transfer->expectedSize, &transfer->spoolPath);
		return false;
	}
	return true;
}

bool
Server::queueDropDirTransfer(std::shared_ptr<CompletedFileTransfer> transfer)
{
	if (!transfer) {
		return false;
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
		return false;
	}
	if (m_pendingDropDirTransfers.size() >= kMaxPendingDropDirTransfers) {
		LOG((CLOG_ERR "drop-dir writer queue full; dropping completed file transfer"));
		FileChunk::releaseReceiveBuffer(transfer->data, transfer->expectedSize, &transfer->spoolPath);
		return false;
	}
	LOG((CLOG_WARN "queueing completed file transfer while drop-dir writer is still running (%lu/%lu)",
		static_cast<unsigned long>(m_pendingDropDirTransfers.size() + 1),
		static_cast<unsigned long>(kMaxPendingDropDirTransfers)));
	m_pendingDropDirTransfers.push_back(transfer);
	return true;
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
	if (transfer->kind == barrier::FileTransferKind::kClipboard &&
		!transfer->remoteFileClipboardSession.empty() &&
		(hasSpooledReceive
			? TransferArchive::isPackageFile(transfer->spoolPath)
			: TransferArchive::isPackageData(transfer->data))) {
		LOG((CLOG_INFO "remote clipboard package received: session=%s", transfer->remoteFileClipboardSession.c_str()));
		const barrier::fs::path cacheRoot =
			barrier::DataDirectories::profile() / "clipboard-cache" / "server";
		const barrier::fs::path spoolDir =
			RemoteFileClipboard::materializedSessionRoot(
				cacheRoot, transfer->remoteFileClipboardOrigin,
				transfer->clipboardRevision.sequence(),
				transfer->remoteFileClipboardSession);
		std::vector<barrier::fs::path> roots;
		std::string error;
		const bool extracted = hasSpooledReceive
			? RemoteFileClipboard::extractPackageFile(transfer->spoolPath, spoolDir, roots, error)
			: RemoteFileClipboard::extractPackage(transfer->data, spoolDir, roots, error);
		if (extracted) {
			RemoteFileClipboard::pruneMaterializedCacheKeepingRoot(
				cacheRoot, spoolDir, 8, 512u * 1024u * 1024u);
			LOG((CLOG_INFO "remote clipboard package materialized: session=%s items=%lu",
				transfer->remoteFileClipboardSession.c_str(),
				static_cast<unsigned long>(roots.size())));
			FileClipboardReadyInfo* info = new FileClipboardReadyInfo();
			info->m_sessionId = transfer->remoteFileClipboardSession;
			info->m_revision = transfer->clipboardRevision;
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

	if (!droppedPaths.empty() &&
		transfer->kind == barrier::FileTransferKind::kClipboard) {
		const std::string sessionId = RemoteFileClipboard::createSessionId();
		FileClipboardReadyInfo* info = new FileClipboardReadyInfo();
		info->m_sessionId = sessionId;
		info->m_revision = transfer->clipboardRevision;
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

	const std::shared_ptr<const String> data(
		new String(clipboard.marshall()));
	if (m_screen != NULL && m_screen->hasAsyncClipboardPublications()) {
		const std::uint64_t publicationId =
			allocateClipboardPublicationId();
		m_pendingMaterializedClipboardPublicationId = publicationId;
		m_pendingMaterializedClipboardData = data;
		m_pendingMaterializedClipboardSession = sessionId;
		m_pendingMaterializedClipboardRevision = m_clipboardRevision;
		if (!m_screen->setClipboardSnapshot(
				kClipboardClipboard, data, publicationId)) {
			LOG((CLOG_ERR
				"failed to queue remote file clipboard publication: session=%s publication=%llu",
				sessionId.c_str(),
				static_cast<unsigned long long>(publicationId)));
			clearMaterializedClipboardPublication();
			return;
		}
		LOG((CLOG_DEBUG
			"queued remote file clipboard publication: session=%s publication=%llu",
			sessionId.c_str(),
			static_cast<unsigned long long>(publicationId)));
		return;
	}

	if (m_screen == NULL ||
		!m_screen->setClipboardChecked(kClipboardClipboard, &clipboard)) {
		LOG((CLOG_ERR
			"failed to publish remote file clipboard: session=%s",
			sessionId.c_str()));
		return;
	}
	commitMaterializedFileClipboard(data, sessionId, paths.size(), 0);
}

void
Server::commitMaterializedFileClipboard(
	const std::shared_ptr<const String>& data, const std::string& sessionId,
	std::size_t pathCount, std::uint64_t publicationId)
{
	if (!data || !Clipboard::isValidMarshalled(*data)) {
		clearMaterializedClipboardPublication();
		return;
	}

	Clipboard committed;
	committed.unmarshall(*data, 0);
	ClipboardInfo& state = m_clipboards[kClipboardClipboard];
	if (!Clipboard::copy(&state.m_clipboard, &committed)) {
		LOG((CLOG_ERR
			"failed to cache committed remote file clipboard: session=%s",
			sessionId.c_str()));
		clearMaterializedClipboardPublication();
		return;
	}
	state.m_clipboardData.set(*data);
	if (m_primaryClient != NULL) {
		m_primaryClient->setClipboardDirty(kClipboardClipboard, false);
	}
	m_lastCommittedClipboardPublicationId =
		publicationId != 0 ? publicationId :
			allocateClipboardPublicationId();
	LOG((CLOG_INFO "remote clipboard published locally: session=%s items=%lu",
		sessionId.c_str(),
		static_cast<unsigned long>(pathCount)));
	if (m_remoteFileClipboardSession == sessionId &&
		m_remoteFileClipboardRevision == m_clipboardRevision) {
		m_remoteFileClipboardSession.clear();
		m_remoteFileClipboardRevision.reset();
		m_remoteFileClipboardOriginBinding.clear();
	}
	clearMaterializedClipboardPublication();
}

void
Server::clearMaterializedClipboardPublication()
{
	m_pendingMaterializedClipboardPublicationId = 0;
	m_pendingMaterializedClipboardData.reset();
	m_pendingMaterializedClipboardSession.clear();
	m_pendingMaterializedClipboardRevision.reset();
}

std::uint64_t
Server::allocateClipboardPublicationId()
{
	++m_nextClipboardPublicationId;
	if (m_nextClipboardPublicationId == 0) {
		++m_nextClipboardPublicationId;
	}
	return m_nextClipboardPublicationId;
}

void
Server::sendClipboardSelectionToClient(BaseClientProxy* target,
									   const std::vector<barrier::fs::path>& sourcePaths,
									   const std::string& sessionId,
									   const barrier::ClipboardRevision& revision)
{
	if (target == NULL || sourcePaths.empty() || sessionId.empty() ||
		!revision.valid() || revision != m_clipboardRevision) {
		return;
	}
	if (!target->supportsTransactionalFileTransfer()) {
		LOG((CLOG_WARN
			"not starting file clipboard prefetch for legacy client \"%s\"",
			target->getName().c_str()));
		return;
	}

	m_pendingFileClipboardPrefetchTarget = target;
	m_pendingFileClipboardPrefetchPaths = sourcePaths;
	m_pendingFileClipboardPrefetchSession = sessionId;
	m_pendingFileClipboardPrefetchRevision = revision;
	if (!reapSendFileThreadIfReady()) {
		if (m_sendFileIsClipboardPrefetch && m_sendFileChunker) {
			m_sendFileChunker->interruptFile();
			LOG((CLOG_DEBUG "remote clipboard prefetch superseded; stopping the previous sender"));
		}
		else {
			LOG((CLOG_DEBUG "remote clipboard prefetch queued behind the active file sender"));
		}
		return;
	}

	startPendingFileClipboardPrefetch();
}

void
Server::startPendingFileClipboardPrefetch()
{
	BaseClientProxy* pendingTarget = m_pendingFileClipboardPrefetchTarget;
	if (pendingTarget != NULL &&
		!pendingTarget->supportsTransactionalFileTransfer()) {
		m_pendingFileClipboardPrefetchTarget = NULL;
		m_pendingFileClipboardPrefetchPaths.clear();
		m_pendingFileClipboardPrefetchSession.clear();
		m_pendingFileClipboardPrefetchRevision.reset();
		return;
	}
	if (pendingTarget == NULL || m_pendingFileClipboardPrefetchPaths.empty() ||
		m_clientSet.count(pendingTarget) == 0 || !reapSendFileThreadIfReady()) {
		return;
	}
	if (m_pendingFileClipboardPrefetchSession.empty() ||
		!m_pendingFileClipboardPrefetchRevision.valid() ||
		m_pendingFileClipboardPrefetchRevision != m_clipboardRevision) {
		LOG((CLOG_INFO
			"discarding superseded clipboard prefetch before sender start"));
		m_pendingFileClipboardPrefetchTarget = NULL;
		m_pendingFileClipboardPrefetchPaths.clear();
		m_pendingFileClipboardPrefetchSession.clear();
		m_pendingFileClipboardPrefetchRevision.reset();
		return;
	}
	std::shared_ptr<barrier::BulkChannel> pendingBulkChannel =
		pendingTarget->acquireBulkChannel();
	if (pendingTarget->supportsBulkChannel() && !pendingBulkChannel) {
		LOG((CLOG_DEBUG
			"remote clipboard prefetch deferred until the bulk channel for \"%s\" is ready",
			pendingTarget->getName().c_str()));
		return;
	}

	finishCompletedSendFileIfReady();
	if (m_sendFileTarget != NULL) {
		const bool currentTargetConnected =
			m_clientSet.count(m_sendFileTarget) != 0;
		const bool currentTransferCanRetire =
			m_sendFileThread == NULL && currentTargetConnected &&
			!m_sendFileCancelAckPending &&
			(m_sendFileCompletionPending ||
			 m_sendFilePreflightFailed.load());
		if (!currentTransferCanRetire) {
			LOG((CLOG_DEBUG "remote clipboard prefetch deferred until the active sender completes"));
			return;
		}

		// The completed frame is already owned by the connected stream.  Starting
		// the next transfer preserves FIFO ordering without waiting for socket drain.
		m_sendFileTarget = NULL;
		m_sendFileBulkChannel.reset();
		m_sendFileTransactionState.reset();
		m_sendFileCompletionPending = false;
		m_sendFileStarted = false;
		m_sendFilePreflightFailed.store(false);
		resetSendFileOutputDrainDeadline();
		m_sendFileCleanupPending = false;
		m_sendFileIsClipboardPrefetch = false;
		cleanupSendFileCancelAckTimeout();
		cleanupSendFileReap();
		m_sendFileCancelAckPending = false;
	}

	std::vector<barrier::fs::path> sourcePaths;
	sourcePaths.swap(m_pendingFileClipboardPrefetchPaths);
	std::string clipboardSessionId;
	clipboardSessionId.swap(m_pendingFileClipboardPrefetchSession);
	const barrier::ClipboardRevision clipboardRevision =
		m_pendingFileClipboardPrefetchRevision;
	m_pendingFileClipboardPrefetchTarget = NULL;
	m_pendingFileClipboardPrefetchRevision.reset();

	auto chunker = std::make_shared<StreamChunker>();
	m_sendFileChunker = chunker;
	m_sendFileTarget = pendingTarget;
	m_sendFileCompletionPending = false;
	m_sendFileStarted = false;
	m_sendFilePreflightFailed.store(false);
	resetSendFileOutputDrainDeadline();
	m_sendFileCleanupPending = false;
	m_sendFileIsClipboardPrefetch = true;
	cleanupSendFileCancelAckTimeout();
	cleanupSendFileReap();
	m_sendFileCancelAckPending = false;
	const UInt32 transferId = allocateSendFileTransferId(pendingTarget);
	m_sendFileTransactionState =
		pendingTarget->supportsTransactionalFileTransfer() ?
			std::make_shared<barrier::FileTransferSendState>(
				transferId, barrier::FileTransferKind::kClipboard,
				clipboardRevision.sequence(), clipboardSessionId) :
			std::shared_ptr<barrier::FileTransferSendState>();
	m_sendFileBulkChannel = pendingBulkChannel;
	barrier::IStream* stream = m_sendFileBulkChannel ?
		m_sendFileBulkChannel->getStream() : pendingTarget->getStream();
	LOG((CLOG_INFO "remote clipboard prefetch started: direction=server-to-client items=%lu target=%s",
		static_cast<unsigned long>(sourcePaths.size()),
		pendingTarget->getName().c_str()));
	const std::shared_ptr<barrier::FileTransferSendState> transactionState =
		m_sendFileTransactionState;
	m_sendFileThread = new Thread([this, stream, sourcePaths, chunker,
		transferId, transactionState]() {
		send_clipboard_file_thread(
			stream, sourcePaths, chunker, transferId, transactionState);
	});
}

void
Server::send_clipboard_file_thread(barrier::IStream* stream,
								   const std::vector<barrier::fs::path>& sourcePaths,
								   const std::shared_ptr<StreamChunker>& chunker,
								   UInt32 transferId,
								   const std::shared_ptr<barrier::FileTransferSendState>& transactionState)
{
	barrier::fs::path packagePath;
	bool chunkerOwnsMaintenanceEvent = false;
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
		chunkerOwnsMaintenanceEvent = true;
		chunker->sendFile(packagePath.u8string().c_str(), m_events, this,
			stream, transferId, transactionState);
	}
	catch (XThread&) {
		if (!packagePath.empty()) {
			barrier::fs::remove(packagePath);
		}
		throw;
	}
	catch (std::runtime_error& error) {
		LOG((CLOG_ERR "failed sending remote clipboard file package: %s", error.what()));
		if (transactionState) {
			transactionState->fail(barrier::FileTransferReason::kIoError);
		}
	}

	if (!chunkerOwnsMaintenanceEvent) {
		m_sendFilePreflightFailed.store(true);
		m_events->addEvent(Event(m_events->forFile().keepAlive(), this));
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
	if (client == m_primaryClient) {
		m_events->adoptHandler(m_events->forClipboard().clipboardPublished(),
			client->getEventTarget(),
			new TMethodEventJob<Server>(
				this, &Server::handleClipboardPublished));
	}

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
	if (m_inputHandoffPending && client == m_inputHandoffTarget) {
		if (m_inputHandoffSourceRevokePending) {
			// CIRV is irreversible.  The source may become inactive after this
			// target disappears, so retain the transaction and redirect a
			// successful revoke to the always-local primary lease.
			SInt32 recoveryX = m_x;
			SInt32 recoveryY = m_y;
			if (!getPrimaryRecoveryPoint(m_inputHandoffSource,
					recoveryX, recoveryY)) {
				clampToClientShape(m_primaryClient, recoveryX, recoveryY);
			}
			LOG((CLOG_WARN
				"handoff target disconnected after source revoke request; redirecting seq=%u to primary at %d,%d",
				m_inputHandoffSeqNum, recoveryX, recoveryY));
			m_inputHandoffTarget = m_primaryClient;
			m_inputHandoffX = recoveryX;
			m_inputHandoffY = recoveryY;
			m_inputHandoffGuardDir = kNoDirection;
		}
		else {
			cancelInputHandoff("handoff target disconnected", true, false);
		}
	}
	else if (m_inputHandoffPending && client == m_inputHandoffSource) {
		cancelInputHandoff("handoff source disconnected", false);
	}
	if (m_inputHandoffCommitted && client == m_inputHandoffTarget) {
		finishCommittedInputHandoff();
	}
	else if (m_inputHandoffCommitted && client == m_inputHandoffSource) {
		// Keep the target record so a later rejection can still recover locally.
		// Protocol 1.10 also has a deadline; legacy records retire on the next
		// switch or when the target disconnects.
		m_inputHandoffSource = NULL;
	}
	discardPendingMouseMove(client);
	eraseBulkBindings(client);
	m_transactionalDragFileLists.erase(client);
	if (!m_remoteFileClipboardOriginBinding.empty() &&
		m_remoteFileClipboardOriginBinding == client->getConnectionBinding()) {
		supersedeFileClipboard("clipboard origin disconnected");
	}
	if (client == m_sendFileTarget && m_sendFileTransactionState) {
		m_sendFileTransactionState->connectionLost();
		cleanupSendFileCancelAckTimeout();
		m_sendFileCancelAckPending = false;
	}
	client->detachBulkChannel();
	if (m_pendingManualFileSendTarget == client) {
		m_pendingManualFileSendTarget = NULL;
		m_pendingManualFileSendPath.clear();
	}
	if (m_pendingFileClipboardPrefetchTarget == client) {
		m_pendingFileClipboardPrefetchTarget = NULL;
		m_pendingFileClipboardPrefetchPaths.clear();
		m_pendingFileClipboardPrefetchSession.clear();
		m_pendingFileClipboardPrefetchRevision.reset();
	}

	// remove event handlers
	m_events->removeHandler(m_events->forIScreen().shapeChanged(),
							client->getEventTarget());
	m_events->removeHandler(m_events->forClipboard().clipboardGrabbed(),
							client->getEventTarget());
	m_events->removeHandler(m_events->forClipboard().clipboardChanged(),
								client->getEventTarget());
	if (client == m_primaryClient) {
		m_events->removeHandler(
			m_events->forClipboard().clipboardPublished(),
			client->getEventTarget());
	}
	m_events->removeHandler(m_events->forClientProxy().inputHandoffReady(), client);

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
	if (client == m_sendFileTarget && m_sendFileTransactionState) {
		m_sendFileTransactionState->connectionLost();
		cleanupSendFileCancelAckTimeout();
		m_sendFileCancelAckPending = false;
	}
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
		m_events->removeHandler(m_events->forClientProxy().inputHandoffReady(), client);
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
		// Prefer the physical edge used to leave the primary.  If no edge
		// lease exists, retain the platform's last valid local point; the center
		// is only a final fallback inside getPrimaryRecoveryPoint().
		const bool primaryUsable =
			getPrimaryRecoveryPoint(active, m_x, m_y);
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
		m_primaryLeaveFailedRecently = false;
		m_primaryLeaveFailureDir = kNoDirection;
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
	m_pendingClipboardFetch(false),
	m_previousClipboardOwner(),
	m_previousClipboardSeqNum(0),
	m_hasClipboardRollback(false)
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
	if (!m_fileReceiveSession.isComplete()) {
		return false;
	}
	const barrier::fs::path spoolPath = m_fileReceiveSession.spoolPath();
	if (!spoolPath.empty()) {
		return barrier::fs::exists(spoolPath) &&
			static_cast<size_t>(barrier::fs::file_size(spoolPath)) ==
				m_fileReceiveSession.expectedSize();
	}
	return m_fileReceiveSession.expectedSize() == m_fileReceiveSession.data().size();
}

void
Server::sendFileToClient(const std::string& filename)
{
	if (filename.empty()) {
		LOG((CLOG_WARN "file send rejected because the source path is empty"));
		return;
	}
	if (m_pendingManualFileSendTarget != NULL) {
		LOG((CLOG_WARN
			"file send rejected because another manual request is already queued"));
		return;
	}

	BaseClientProxy* target = m_active;
	if (target == NULL || m_clientSet.count(target) == 0 || target == m_primaryClient) {
		LOG((CLOG_WARN "cannot send file without an active secondary target"));
		return;
	}
	if (!target->supportsTransactionalFileTransfer()) {
		LOG((CLOG_WARN
			"file send rejected because client \"%s\" lacks transactional transfer support",
			target->getName().c_str()));
		return;
	}

	if (!reapSendFileThreadIfReady()) {
		LOG((CLOG_WARN
			"file send rejected because another transfer is still active for \"%s\"",
			target->getName().c_str()));
		return;
	}
	finishCompletedSendFileIfReady();
	if (m_sendFileTarget != NULL && m_sendFileThread == NULL &&
		m_sendFilePreflightFailed.load()) {
		// A stopped worker that never produced Start has no remote transfer
		// state. Retire it only while servicing a new manual request; the normal
		// keepalive path must still preserve targets for already queued frames.
		LOG((CLOG_WARN
			"retiring file sender that stopped before producing a wire frame"));
		cleanupSendFileThread(false);
	}
	if (m_sendFileTarget != NULL) {
		LOG((CLOG_WARN
			"file send rejected because the previous transfer has no completed wire boundary"));
		return;
	}
	std::shared_ptr<barrier::BulkChannel> bulkChannel =
		target->acquireBulkChannel();
	if (target->supportsBulkChannel() && !bulkChannel) {
		m_pendingManualFileSendTarget = target;
		m_pendingManualFileSendPath = filename;
		LOG((CLOG_NOTE
			"file send queued until the required bulk channel for \"%s\" is ready",
			target->getName().c_str()));
		return;
	}

    startManualFileSend(target, filename, bulkChannel);
}

void
Server::startPendingManualFileSend()
{
	BaseClientProxy* pendingTarget = m_pendingManualFileSendTarget;
	if (pendingTarget == NULL || m_pendingManualFileSendPath.empty()) {
		return;
	}
	if (m_clientSet.count(pendingTarget) == 0) {
		m_pendingManualFileSendTarget = NULL;
		m_pendingManualFileSendPath.clear();
		return;
	}
	if (!pendingTarget->supportsTransactionalFileTransfer()) {
		m_pendingManualFileSendTarget = NULL;
		m_pendingManualFileSendPath.clear();
		return;
	}
	if (!reapSendFileThreadIfReady()) {
		LOG((CLOG_WARN
			"queued file send rejected because another transfer acquired the route"));
		m_pendingManualFileSendTarget = NULL;
		m_pendingManualFileSendPath.clear();
		return;
	}

	finishCompletedSendFileIfReady();
	if (m_sendFileTarget != NULL) {
		LOG((CLOG_WARN
			"queued file send rejected because the previous transfer has no completed wire boundary"));
		m_pendingManualFileSendTarget = NULL;
		m_pendingManualFileSendPath.clear();
		return;
	}

	std::shared_ptr<barrier::BulkChannel> bulkChannel =
		pendingTarget->acquireBulkChannel();
	if (pendingTarget->supportsBulkChannel() && !bulkChannel) {
		return;
	}

	std::string filename;
	filename.swap(m_pendingManualFileSendPath);
	m_pendingManualFileSendTarget = NULL;
	LOG((CLOG_NOTE "starting queued manual file send to \"%s\"",
		pendingTarget->getName().c_str()));
	startManualFileSend(pendingTarget, filename, bulkChannel);
}

void
Server::startManualFileSend(
	BaseClientProxy* target,
	const std::string& filename,
	const std::shared_ptr<barrier::BulkChannel>& bulkChannel)
{
	if (target == NULL || !target->supportsTransactionalFileTransfer()) {
		LOG((CLOG_WARN
			"not starting manual file send without a transactional client"));
		return;
	}
	auto chunker = std::make_shared<StreamChunker>();
	m_sendFileChunker = chunker;
	m_sendFileTarget = target;
	m_sendFileCompletionPending = false;
	m_sendFileStarted = false;
	m_sendFilePreflightFailed.store(false);
	resetSendFileOutputDrainDeadline();
	m_sendFileCleanupPending = false;
	m_sendFileIsClipboardPrefetch = false;
	cleanupSendFileCancelAckTimeout();
	cleanupSendFileReap();
	m_sendFileCancelAckPending = false;
	const UInt32 transferId = allocateSendFileTransferId(target);
	m_sendFileTransactionState =
		target->supportsTransactionalFileTransfer() ?
			std::make_shared<barrier::FileTransferSendState>(
				transferId, barrier::FileTransferKind::kManual, 0,
				std::string()) :
			std::shared_ptr<barrier::FileTransferSendState>();
	m_sendFileBulkChannel = bulkChannel;
	barrier::IStream* stream = m_sendFileBulkChannel ?
		m_sendFileBulkChannel->getStream() : target->getStream();
    const std::shared_ptr<barrier::FileTransferSendState> transactionState =
		m_sendFileTransactionState;
    m_sendFileThread = new Thread([this, stream, filename, chunker,
		transferId, transactionState]() {
		send_file_thread(
			stream, filename, chunker, transferId, transactionState);
	});
}

UInt32
Server::allocateSendFileTransferId(BaseClientProxy* target)
{
	if (target != NULL && target->supportsTransactionalFileTransfer()) {
		UInt32 sequence =
			barrier::FileTransferProtocol::transferSequence(
				m_sendFileTransferId) + 1u;
		if (sequence == 0 ||
			sequence > barrier::FileTransferProtocol::kTransferSequenceMask) {
			sequence = 1;
		}
		m_sendFileTransferId =
			barrier::FileTransferProtocol::makeTransferId(
				barrier::FileTransferRole::kPrimary, sequence);
		return m_sendFileTransferId;
	}

	++m_sendFileTransferId;
	if (m_sendFileTransferId == 0) {
		++m_sendFileTransferId;
	}
	return m_sendFileTransferId;
}

void Server::send_file_thread(barrier::IStream* stream,
                              const std::string& filename,
                              const std::shared_ptr<StreamChunker>& chunker,
                              UInt32 transferId,
                              const std::shared_ptr<barrier::FileTransferSendState>& transactionState)
{
	barrier::fs::path sourcePath;
	barrier::fs::path tempPackagePath;
	bool chunkerOwnsMaintenanceEvent = false;
	try {
		Thread::testCancel();
		LOG((CLOG_DEBUG "sending file to client, filename=%s", filename.c_str()));
		std::string error;
		if (!prepareTransferSource(filename.c_str(), sourcePath, tempPackagePath, error)) {
			m_sendFilePreflightFailed.store(true);
			throw std::runtime_error(error);
		}

		Thread::testCancel();
		const barrier::fs::path& transferPath =
			tempPackagePath.empty() ? sourcePath : tempPackagePath;
		chunkerOwnsMaintenanceEvent = true;
		chunker->sendFile(transferPath.u8string().c_str(), m_events, this,
			stream, transferId, transactionState);
	}
	catch (XThread&) {
		if (!tempPackagePath.empty()) {
			barrier::fs::remove(tempPackagePath);
		}
		throw;
	}
	catch (std::runtime_error &error) {
		LOG((CLOG_ERR "failed sending file chunks, error: %s", error.what()));
		if (transactionState) {
			transactionState->fail(barrier::FileTransferReason::kIoError);
		}
	}

	if (!chunkerOwnsMaintenanceEvent) {
		m_events->addEvent(Event(m_events->forFile().keepAlive(), this));
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
		if (!m_sendFileThread->wait(0.0)) {
			if (cancel) {
				LOG((CLOG_DEBUG "requesting asynchronous file sender cancellation"));
				m_sendFileCleanupPending = true;
				m_sendFileThread->cancel();
				m_sendFileThread->unblockPollSocket();
			}
			return false;
		}
		delete m_sendFileThread;
		m_sendFileThread = NULL;
	}

	m_sendFileChunker.reset();
	m_sendFileBulkChannel.reset();
	m_sendFileTransactionState.reset();
	m_sendFileTarget = NULL;
	m_sendFileCompletionPending = false;
	m_sendFileStarted = false;
	m_sendFilePreflightFailed.store(false);
	resetSendFileOutputDrainDeadline();
	m_sendFileCleanupPending = false;
	m_sendFileIsClipboardPrefetch = false;
	cleanupSendFileCancelAckTimeout();
	cleanupSendFileReap();
	m_sendFileCancelAckPending = false;
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
	if (completedSendFileMayRetire()) {
		m_sendFileCompletionPending = true;
	}
	if (m_sendFileCleanupPending) {
		BaseClientProxy* target = m_sendFileTarget;
		m_sendFileTarget = NULL;
		m_sendFileBulkChannel.reset();
		m_sendFileTransactionState.reset();
		m_sendFileCompletionPending = false;
		m_sendFileStarted = false;
		m_sendFilePreflightFailed.store(false);
		resetSendFileOutputDrainDeadline();
		m_sendFileCleanupPending = false;
		m_sendFileIsClipboardPrefetch = false;
		cleanupSendFileCancelAckTimeout();
		cleanupSendFileReap();
		m_sendFileCancelAckPending = false;
		deleteDeferredClient(target);
		deleteDeferredClients();
	}
	return true;
}

bool
Server::finishCompletedSendFileIfReady()
{
	if (!m_sendFileCompletionPending || m_sendFileThread != NULL) {
		return false;
	}
	if (m_sendFileCancelAckPending &&
		m_sendFileTransactionState &&
		m_sendFileTransactionState->result() !=
			barrier::FileTransferReason::kConnectionLost) {
		return false;
	}

	BaseClientProxy* target = m_sendFileTarget;
	if (target != NULL && m_clientSet.count(target) != 0) {
		barrier::IStream* stream = m_sendFileBulkChannel ?
			(m_sendFileBulkChannel->isActive() ?
				m_sendFileBulkChannel->getStream() : NULL) :
			target->getStream();
		if (stream != NULL && stream->getBufferedOutputSize() > 0) {
			return false;
		}
	}

	m_sendFileTarget = NULL;
	m_sendFileBulkChannel.reset();
	m_sendFileTransactionState.reset();
	m_sendFileCompletionPending = false;
	m_sendFileStarted = false;
	m_sendFilePreflightFailed.store(false);
	resetSendFileOutputDrainDeadline();
	m_sendFileCleanupPending = false;
	m_sendFileIsClipboardPrefetch = false;
	cleanupSendFileCancelAckTimeout();
	cleanupSendFileReap();
	m_sendFileCancelAckPending = false;
	deleteDeferredClient(target);
	deleteDeferredClients();
	return true;
}

void
Server::serviceSendFileCompletion()
{
	const bool hadWorker = (m_sendFileThread != NULL);
	const bool reaped = reapSendFileThreadIfReady();
	const bool preflightFailed = m_sendFilePreflightFailed.load();
	if (hadWorker && reaped && m_sendFilePreflightFailed.exchange(false)) {
		// A sender that explicitly failed preflight produces no Start/End
		// frames. Its maintenance event is therefore the completion boundary.
		m_sendFileCompletionPending = true;
	}
	if (!reaped && (preflightFailed || completedSendFileMayRetire())) {
		scheduleSendFileReap();
	}
	else if (reaped) {
		cleanupSendFileReap();
	}
	bool retired = finishCompletedSendFileIfReady();
	if (!retired && m_sendFileThread == NULL &&
		m_sendFileCompletionPending && !m_sendFileCancelAckPending &&
		sendFileBulkOutputPending()) {
		if (!scheduleSendFileReap()) {
			resetStalledSendFileBulkRoute();
			retired = finishCompletedSendFileIfReady();
		}
	}
	if (retired || m_sendFileTarget == NULL) {
		startPendingManualFileSend();
		startPendingFileClipboardPrefetch();
	}
}

bool
Server::completedSendFileMayRetire() const
{
	if (m_sendFileCompletionPending) {
		return true;
	}
	if (!m_sendFileTransactionState) {
		return false;
	}
	const barrier::FileTransferReason result =
		m_sendFileTransactionState->result();
	return m_sendFileTransactionState->committed() ||
		result == barrier::FileTransferReason::kConnectionLost ||
		(!m_sendFileStarted &&
		 result != barrier::FileTransferReason::kNone) ||
		(!m_sendFileTransactionState->startAcknowledged() &&
		 result != barrier::FileTransferReason::kNone &&
		 result != barrier::FileTransferReason::kTimeout &&
		 result != barrier::FileTransferReason::kCancelled);
}

bool
Server::scheduleSendFileReap()
{
	const bool waitingForWorker = (m_sendFileThread != NULL);
	const bool waitingForOutput = !waitingForWorker &&
		m_sendFileCompletionPending && !m_sendFileCancelAckPending &&
		sendFileBulkOutputPending();
	if ((!waitingForWorker && !waitingForOutput) ||
		m_sendFileTransferId == 0) {
		return false;
	}
	if (waitingForOutput) {
		if (m_sendFileOutputDrainTransferId != m_sendFileTransferId) {
			m_sendFileOutputDrainTransferId = m_sendFileTransferId;
			m_sendFileOutputDrainDeadline =
				ARCH->time() + kFileTransferCancelAckTimeoutSeconds;
		}
		if (ARCH->time() >= m_sendFileOutputDrainDeadline) {
			return false;
		}
	}
	if (m_sendFileReapTimer != NULL &&
		m_sendFileReapTransferId == m_sendFileTransferId) {
		return true;
	}
	cleanupSendFileReap();
	m_sendFileReapTransferId = m_sendFileTransferId;
	m_sendFileReapTimer =
		m_events->newOneShotTimer(kFileSenderReapPollSeconds, NULL);
	if (m_sendFileReapTimer == NULL) {
		m_sendFileReapTransferId = 0;
		LOG((CLOG_ERR
			"unable to schedule file sender reaper, transfer=%u",
			m_sendFileTransferId));
		return false;
	}
	m_events->adoptHandler(Event::kTimer, m_sendFileReapTimer,
		new TMethodEventJob<Server>(this, &Server::handleSendFileReap));
	return true;
}

bool
Server::sendFileBulkOutputPending() const
{
	return m_sendFileBulkChannel && m_sendFileBulkChannel->isActive() &&
		m_sendFileBulkChannel->getStream() != NULL &&
		m_sendFileBulkChannel->getStream()->getBufferedOutputSize() > 0;
}

void
Server::resetSendFileOutputDrainDeadline()
{
	m_sendFileOutputDrainTransferId = 0;
	m_sendFileOutputDrainDeadline = 0.0;
}

void
Server::resetStalledSendFileBulkRoute()
{
	BaseClientProxy* target = m_sendFileTarget;
	if (!m_sendFileBulkChannel) {
		resetSendFileOutputDrainDeadline();
		return;
	}

	LOG((CLOG_WARN
		"bulk output did not drain before sender completion deadline; resetting route, transfer=%u",
		m_sendFileTransferId));
	m_sendFileBulkChannel->close();
	if (target != NULL) {
		target->detachBulkChannel();
	}
	resetSendFileOutputDrainDeadline();
	if (target != NULL && m_clientSet.count(target) != 0) {
		renewBulkChannel(target);
	}
}

void
Server::cleanupSendFileReap()
{
	if (m_sendFileReapTimer != NULL) {
		m_events->removeHandler(Event::kTimer, m_sendFileReapTimer);
		m_events->deleteTimer(m_sendFileReapTimer);
		m_sendFileReapTimer = NULL;
	}
	m_sendFileReapTransferId = 0;
}

void
Server::handleSendFileReap(const Event&, void*)
{
	const UInt32 transferId = m_sendFileReapTransferId;
	cleanupSendFileReap();
	if (transferId == 0 || transferId != m_sendFileTransferId) {
		return;
	}
	serviceSendFileCompletion();
}

void
Server::scheduleSendFileCancelAckTimeout()
{
	cleanupSendFileCancelAckTimeout();
	if (!m_sendFileTransactionState) {
		m_sendFileCancelAckPending = false;
		return;
	}

	m_sendFileCancelAckPending = true;
	m_sendFileCancelAckTimeoutTransferId =
		m_sendFileTransactionState->transferId();
	m_sendFileCancelAckTimeoutTimer =
		m_events->newOneShotTimer(kFileTransferCancelAckTimeoutSeconds, NULL);
	if (m_sendFileCancelAckTimeoutTimer == NULL) {
		LOG((CLOG_ERR
			"unable to schedule transactional file CancelAck timeout, transfer=%u",
			m_sendFileCancelAckTimeoutTransferId));
		m_sendFileCancelAckTimeoutTransferId = 0;
		m_sendFileCancelAckPending = false;
		serviceSendFileCompletion();
		return;
	}
	m_events->adoptHandler(Event::kTimer, m_sendFileCancelAckTimeoutTimer,
		new TMethodEventJob<Server>(
			this, &Server::handleSendFileCancelAckTimeout));
}

void
Server::cleanupSendFileCancelAckTimeout()
{
	if (m_sendFileCancelAckTimeoutTimer != NULL) {
		m_events->removeHandler(
			Event::kTimer, m_sendFileCancelAckTimeoutTimer);
		m_events->deleteTimer(m_sendFileCancelAckTimeoutTimer);
		m_sendFileCancelAckTimeoutTimer = NULL;
	}
	m_sendFileCancelAckTimeoutTransferId = 0;
}

void
Server::handleSendFileCancelAckTimeout(const Event&, void*)
{
	const UInt32 expiredTransferId = m_sendFileCancelAckTimeoutTransferId;
	cleanupSendFileCancelAckTimeout();
	if (!m_sendFileCancelAckPending || !m_sendFileTransactionState ||
		m_sendFileTransactionState->transferId() != expiredTransferId) {
		return;
	}

	LOG((CLOG_WARN
		"transactional file CancelAck timed out, transfer=%u",
		expiredTransferId));
	m_sendFileCancelAckPending = false;
	serviceSendFileCompletion();
}

bool
Server::cleanupWriteToDropDirThread()
{
	if (m_writeToDropDirThread != NULL) {
		if (!m_writeToDropDirThread->wait(0.0)) {
			LOG((CLOG_DEBUG "requesting asynchronous drop-dir writer cancellation"));
			m_writeToDropDirThread->cancel();
			m_writeToDropDirThread->unblockPollSocket();
			return false;
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
	const std::uint64_t receiveGeneration = m_fileReceiveSession.generation();
	if (m_fileReceiveClipboardGeneration == receiveGeneration) {
		transfer->remoteFileClipboardSession =
			m_fileReceiveRemoteFileClipboardSession;
		transfer->remoteFileClipboardOrigin = m_fileReceiveClipboardOrigin;
		transfer->clipboardRevision = m_fileReceiveClipboardRevision;
		if (!transfer->remoteFileClipboardSession.empty()) {
			transfer->kind = barrier::FileTransferKind::kClipboard;
		}
	}
	m_fileReceiveSession.takeCompleted(
		transfer->data, transfer->expectedSize, transfer->spoolPath);
	m_fileReceiveSource = NULL;
	m_fileReceiveBulkChannel = NULL;
	if (m_screen != NULL && m_screen->getPlatformScreen() != NULL) {
		transfer->dropTarget = m_screen->getDropTarget();
	}
	transfer->dragFileList.swap(m_fakeDragFileList);
	m_fileReceiveClipboardGeneration = 0;
	m_fileReceiveRemoteFileClipboardSession.clear();
	m_fileReceiveClipboardOrigin.clear();
	m_fileReceiveClipboardRevision.reset();
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
