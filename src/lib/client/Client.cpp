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

#include "client/Client.h"

#include "client/ServerProxy.h"
#include "barrier/Screen.h"
#include "barrier/FileChunk.h"
#include "barrier/DropHelper.h"
#include "barrier/PacketStreamFilter.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/RemoteFileClipboard.h"
#include "barrier/protocol_types.h"
#include "barrier/XBarrier.h"
#include "barrier/StreamChunker.h"
#include "barrier/TransferArchive.h"
#include "barrier/IPlatformScreen.h"
#include "barrier/IClipboard.h"
#include "common/DataDirectories.h"
#include "mt/Thread.h"
#include "mt/XThread.h"
#include "net/TCPSocket.h"
#include "net/IDataSocket.h"
#include "net/ISocketFactory.h"
#include "net/SecureSocket.h"
#include "arch/Arch.h"
#include "base/Log.h"
#include "base/IEventQueue.h"
#include "base/TMethodEventJob.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <fstream>
#include <vector>

namespace {

const UInt32 kClipboardReadRetryLimit = 20;
const double kClipboardReadRetrySeconds = 0.25;

bool prepareTransferSource(const char* filename,
                           barrier::fs::path& sourcePath,
                           barrier::fs::path& tempPackagePath,
                           std::string& error);

const size_t kMaxPendingDropDirTransfers = 4;
const size_t kMaxPendingDropDirTransferMemoryBytes = 32 * 1024 * 1024;

std::vector<barrier::fs::path> utf8PathsToFsPaths(const std::vector<std::string>& paths)
{
    std::vector<barrier::fs::path> result;
    result.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        result.push_back(barrier::fs::u8path(paths[i]));
    }
    return result;
}

}

//
// Client
//

Client::Client(IEventQueue* events, const std::string& name, const NetworkAddress& address,
               ISocketFactory* socketFactory,
               barrier::Screen* screen,
               ClientArgs const& args) :
    m_mock(false),
    m_name(name),
    m_serverAddress(address),
    m_socketFactory(socketFactory),
    m_screen(screen),
    m_stream(NULL),
    m_detachedSendFileStreams(),
    m_detachedServerProxies(),
    m_timer(NULL),
    m_clipboardRetryTimer(NULL),
    m_server(NULL),
    m_ready(false),
    m_active(false),
    m_suspended(false),
    m_connectOnResume(false),
	    m_terminalEventSent(false),
	    m_events(events),
	    m_fileReceiveSession(),
    m_sendFileThread(NULL),
    m_sendFileTransferId(0),
    m_sendFileIsClipboardPrefetch(false),
    m_writeToDropDirThread(NULL),
    m_pendingDropDirTransfers(),
    m_socket(NULL),
    m_useSecureNetwork(args.m_enableCrypto),
    m_args(args),
    m_enableClipboard(true)
{
    assert(m_socketFactory != NULL);
    assert(m_screen        != NULL);

    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        m_ownClipboard[id] = false;
        m_sentClipboard[id] = false;
        m_clipboardSendPending[id] = false;
        m_clipboardRetryPending[id] = false;
        m_clipboardRetryCount[id] = 0;
        m_timeClipboard[id] = 0;
        m_pendingFileClipboardPaths[id].clear();
    }

    // register suspend/resume event handlers
    m_events->adoptHandler(m_events->forIScreen().suspend(),
                            getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleSuspend));
    m_events->adoptHandler(m_events->forIScreen().resume(),
                            getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleResume));

    m_events->adoptHandler(m_events->forFile().fileChunkSending(),
                            this,
                            new TMethodEventJob<Client>(this,
                                &Client::handleFileChunkSending));
    m_events->adoptHandler(m_events->forFile().fileRecieveCompleted(),
                            this,
                            new TMethodEventJob<Client>(this,
                                &Client::handleFileRecieveCompleted));
    m_events->adoptHandler(m_events->forFile().fileClipboardReady(),
                            this,
                            new TMethodEventJob<Client>(this,
                                &Client::handleFileClipboardReady));
    m_events->adoptHandler(m_events->forFile().dropDirWriteFinished(),
                            this,
                            new TMethodEventJob<Client>(this,
                                &Client::handleDropDirWriteFinished));
    m_events->adoptHandler(m_events->forFile().keepAlive(),
                            this,
                            new TMethodEventJob<Client>(this,
                                &Client::handleFileKeepAlive));
}

Client::~Client()
{
    if (m_mock) {
        return;
    }

    m_events->removeHandler(m_events->forIScreen().suspend(),
                              getEventTarget());
    m_events->removeHandler(m_events->forIScreen().resume(),
                              getEventTarget());

	if (!cleanupSendFileThread(true) && m_sendFileThread != NULL) {
		LOG((CLOG_ERR "waiting for file sender before destroying client state"));
		m_sendFileThread->wait();
		delete m_sendFileThread;
		m_sendFileThread = NULL;
		m_sendFileChunker.reset();
        releaseDetachedSendFileStream();
	}
	if (!cleanupWriteToDropDirThread() && m_writeToDropDirThread != NULL) {
		LOG((CLOG_ERR "waiting for drop-dir writer before destroying client state"));
		m_writeToDropDirThread->wait();
		delete m_writeToDropDirThread;
		m_writeToDropDirThread = NULL;
	}
	FileChunk::releaseReceiveBuffer(m_fileReceiveSession);
    m_events->removeHandler(m_events->forFile().fileChunkSending(), this);
    m_events->removeHandler(m_events->forFile().fileRecieveCompleted(), this);
    m_events->removeHandler(m_events->forFile().fileClipboardReady(), this);
    m_events->removeHandler(m_events->forFile().dropDirWriteFinished(), this);
    m_events->removeHandler(m_events->forFile().keepAlive(), this);
    releasePendingDropDirTransfers();

    cleanupTimer();
    cleanupScreen();
    cleanupConnecting();
    cleanupConnection();
    delete m_socketFactory;
}

void
Client::connect()
{
    if (m_stream != NULL) {
        return;
    }
    if (m_suspended) {
        m_connectOnResume = true;
        return;
    }
    m_terminalEventSent = false;

    auto security_level = ConnectionSecurityLevel::PLAINTEXT;
    if (m_useSecureNetwork) {
        // client always authenticates server
        security_level = ConnectionSecurityLevel::ENCRYPTED_AUTHENTICATED;
    }

    try {
        // resolve the server hostname.  do this every time we connect
        // in case we couldn't resolve the address earlier or the address
        // has changed (which can happen frequently if this is a laptop
        // being shuttled between various networks).  patch by Brent
        // Priddy.
        m_serverAddress.resolve();

        // m_serverAddress will be null if the hostname address is not reolved
        if (m_serverAddress.getAddress() != NULL) {
          // to help users troubleshoot, show server host name (issue: 60)
          LOG((CLOG_NOTE "connecting to '%s': %s:%i",
          m_serverAddress.getHostname().c_str(),
          ARCH->addrToString(m_serverAddress.getAddress()).c_str(),
          m_serverAddress.getPort()));
        }

        // create the socket
        IDataSocket* socket = m_socketFactory->create(ARCH->getAddrFamily(m_serverAddress.getAddress()),
                                                      security_level);
        m_socket = dynamic_cast<TCPSocket*>(socket);

        // filter socket messages, including a packetizing filter
        m_stream = socket;
        m_stream = new PacketStreamFilter(m_events, m_stream, true);

        // connect
        LOG((CLOG_DEBUG1 "connecting to server"));
        setupConnecting();
        setupTimer();
        socket->connect(m_serverAddress);
    }
    catch (XBase& e) {
        cleanupTimer();
        cleanupConnecting();
        cleanupStream();
        LOG((CLOG_DEBUG1 "connection failed"));
        sendConnectionFailedEvent(e.what());
        return;
    }
}

void
Client::disconnect(const char* msg)
{
    m_connectOnResume = false;
    cleanupTimer();
    cleanupScreen();
    cleanupConnecting();
    cleanupConnection();
    if (msg != NULL) {
        sendConnectionFailedEvent(msg);
    }
    else {
        sendDisconnectedEvent();
    }
}

void
Client::handshakeComplete()
{
    m_ready = true;
    m_screen->enable();
    sendEvent(m_events->forClient().connected(), NULL);
}

bool
Client::isConnected() const
{
    return (m_server != NULL);
}

bool
Client::isConnecting() const
{
    return (m_timer != NULL);
}

NetworkAddress
Client::getServerAddress() const
{
    return m_serverAddress;
}

void*
Client::getEventTarget() const
{
    return m_screen->getEventTarget();
}

bool
Client::getClipboard(ClipboardID id, IClipboard* clipboard) const
{
    return m_screen->getClipboard(id, clipboard);
}

void
Client::getShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h) const
{
    m_screen->getShape(x, y, w, h);
}

void
Client::getCursorPos(SInt32& x, SInt32& y) const
{
    m_screen->getCursorPos(x, y);
}

void
Client::enter(SInt32 xAbs, SInt32 yAbs, UInt32, KeyModifierMask mask, bool)
{
    m_active = true;
    m_screen->enter(mask);
    m_screen->mouseMove(xAbs, yAbs);

    if (m_sendFileChunker && !m_sendFileIsClipboardPrefetch) {
        m_sendFileChunker->interruptFile();
        reapSendFileThreadIfReady();
    }
}

bool
Client::leave()
{
    m_active = false;

    m_screen->leave();

    if (m_enableClipboard && m_server != NULL) {
        // Windows can miss the clipboard-viewer notification and only
        // discover the new owner during Screen::leave(). Defer the snapshot
        // so clipboard providers cannot block the input handoff.
        for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
            if (id == kClipboardClipboard || m_ownClipboard[id]) {
                scheduleClipboardRetry(id);
            }
        }
    }

    return true;
}

void
Client::setClipboard(ClipboardID id, const IClipboard* clipboard)
{
    if (m_ownClipboard[id] && !m_sentClipboard[id]) {
        LOG((CLOG_INFO "preserving unsent local clipboard %d instead of applying remote clipboard", id));
        return;
    }

    bool publishClipboard = true;

    if (id == kClipboardClipboard && clipboard != NULL) {
        RemoteFileClipboard::Data remoteFileClipboard;
        if (RemoteFileClipboard::readFromClipboard(*clipboard, remoteFileClipboard) &&
            remoteFileClipboard.mode == RemoteFileClipboard::Mode::SourcePaths) {
            m_remoteFileClipboardSession = remoteFileClipboard.sessionId;
            publishClipboard = false;
        }
        else {
            m_remoteFileClipboardSession.clear();
            m_readyFileClipboardSession.clear();
            m_readyFileClipboardPaths.clear();
        }
    }

    if (publishClipboard) {
        m_screen->setClipboard(id, clipboard);
    }

    m_ownClipboard[id]  = false;
    m_sentClipboard[id] = false;
    m_clipboardSendPending[id] = false;
}

void
Client::grabClipboard(ClipboardID id)
{
    m_screen->grabClipboard(id);
    m_ownClipboard[id]  = false;
    m_sentClipboard[id] = false;
    m_clipboardSendPending[id] = false;
}

void
Client::setClipboardDirty(ClipboardID, bool)
{
    assert(0 && "shouldn't be called");
}

void
Client::keyDown(KeyID id, KeyModifierMask mask, KeyButton button)
{
     m_screen->keyDown(id, mask, button);
}

void
Client::keyRepeat(KeyID id, KeyModifierMask mask,
                SInt32 count, KeyButton button)
{
     m_screen->keyRepeat(id, mask, count, button);
}

void
Client::keyUp(KeyID id, KeyModifierMask mask, KeyButton button)
{
     m_screen->keyUp(id, mask, button);
}

void
Client::mouseDown(ButtonID id)
{
     m_screen->mouseDown(id);
}

void
Client::mouseUp(ButtonID id)
{
     m_screen->mouseUp(id);
}

void
Client::mouseMove(SInt32 x, SInt32 y)
{
    m_screen->mouseMove(x, y);
}

void
Client::mouseRelativeMove(SInt32 dx, SInt32 dy)
{
    m_screen->mouseRelativeMove(dx, dy);
}

void
Client::mouseWheel(SInt32 xDelta, SInt32 yDelta)
{
    m_screen->mouseWheel(xDelta, yDelta);
}

void
Client::screensaver(bool activate)
{
     m_screen->screensaver(activate);
}

void
Client::resetOptions()
{
    m_screen->resetOptions();
}

void
Client::setOptions(const OptionsList& options)
{
    for (OptionsList::const_iterator index = options.begin();
         index != options.end(); ++index) {
        const OptionID id       = *index;
        if (id == kOptionClipboardSharing) {
            index++;
            if (*index == static_cast<OptionValue>(false)) {
                LOG((CLOG_NOTE "clipboard sharing is disabled"));
            }
            m_enableClipboard = *index;

            break;
        }
    }

    m_screen->setOptions(options);
}

std::string
Client::getName() const
{
    return m_name;
}

void
Client::sendClipboard(ClipboardID id)
{
    // note -- m_mutex must be locked on entry
    assert(m_screen != NULL);
    assert(m_server != NULL);

    // get clipboard data.  set the clipboard time to the last
    // clipboard time before getting the data from the screen
    // as the screen may detect an unchanged clipboard and
    // avoid copying the data.
    Clipboard clipboard;
    if (clipboard.open(m_timeClipboard[id])) {
        clipboard.close();
    }
    if (!m_screen->getClipboard(id, &clipboard)) {
        LOG((CLOG_WARN "clipboard %d could not be read; scheduling retry to avoid publishing empty data", id));
        scheduleClipboardRetry(id);
        return;
    }
    m_clipboardRetryPending[id] = false;
    m_clipboardRetryCount[id] = 0;

    RemoteFileClipboard::AutomaticSharingStatus clipboardSharingStatus =
        RemoteFileClipboard::AutomaticSharingStatus::Safe;
    RemoteFileClipboard::Data localFileClipboard;
    if (id == kClipboardClipboard) {
        clipboardSharingStatus =
            RemoteFileClipboard::prepareForAutomaticClipboardSharing(clipboard);
        if (clipboardSharingStatus ==
            RemoteFileClipboard::AutomaticSharingStatus::SafeAfterRemovingImageFileMetadata) {
            LOG((CLOG_INFO "stripped image file-transfer metadata before sending clipboard to server"));
        }
        else if (clipboardSharingStatus ==
                 RemoteFileClipboard::AutomaticSharingStatus::ContainsFileList) {
            std::string error;
            if (!RemoteFileClipboard::normalizeClipboard(
                    clipboard, &localFileClipboard, &error)) {
                LOG((CLOG_WARN "local file clipboard could not be normalized: %s",
                     error.c_str()));
                m_sentClipboard[id] = false;
                m_pendingFileClipboardPaths[id].clear();
                scheduleClipboardRetry(id);
                return;
            }
        }
    }

    std::string data = clipboard.marshall();

    if (!finishPendingClipboardSend(id, data)) {
        return;
    }

    const bool clipboardTimeChanged =
        (m_timeClipboard[id] == 0 || clipboard.getTime() != m_timeClipboard[id]);
    if (clipboardTimeChanged) {
        m_timeClipboard[id] = clipboard.getTime();
    }

    const bool materializedFileEcho =
        clipboardSharingStatus ==
            RemoteFileClipboard::AutomaticSharingStatus::ContainsFileList &&
        !m_readyFileClipboardSession.empty() &&
        RemoteFileClipboard::pathsMatch(localFileClipboard,
                                        m_readyFileClipboardPaths);
    if (materializedFileEcho) {
        LOG((CLOG_INFO
            "suppressed remote file clipboard echo: session=%s items=%lu",
            m_readyFileClipboardSession.c_str(),
            static_cast<unsigned long>(m_readyFileClipboardPaths.size())));
        m_sentClipboard[id] = true;
        m_clipboardSendPending[id] = false;
        m_dataClipboard[id].set(data);
        m_pendingFileClipboardPaths[id].clear();
        m_readyFileClipboardSession.clear();
        m_readyFileClipboardPaths.clear();
        return;
    }

    // save and send data if different or not yet sent
    if (clipboardTimeChanged || !m_sentClipboard[id] || !m_dataClipboard[id].matches(data)) {
        if (clipboardSharingStatus ==
            RemoteFileClipboard::AutomaticSharingStatus::ContainsFileList) {
            LOG((CLOG_INFO "sending local file clipboard metadata before package transfer"));
            m_remoteFileClipboardSession.clear();
            m_readyFileClipboardSession.clear();
            m_readyFileClipboardPaths.clear();
            ServerProxy::ClipboardSendResult result =
                m_server->onClipboardChanged(id, &clipboard);
            if (result == ServerProxy::kClipboardSendFailed) {
                m_pendingFileClipboardPaths[id].clear();
                scheduleClipboardRetry(id);
                return;
            }
            if (result == ServerProxy::kClipboardSendPending) {
                m_sentClipboard[id] = false;
                m_clipboardSendPending[id] = true;
                m_pendingClipboardData[id].set(data);
                m_pendingFileClipboardPaths[id] = localFileClipboard.paths;
                scheduleClipboardRetry(id);
                return;
            }
            m_sentClipboard[id] = true;
            m_clipboardSendPending[id] = false;
            m_dataClipboard[id].set(data);
            m_pendingFileClipboardPaths[id].clear();
            sendClipboardSelectionToServer(localFileClipboard.paths);
            return;
        }
        ServerProxy::ClipboardSendResult result =
            m_server->onClipboardChanged(id, &clipboard);
        if (result == ServerProxy::kClipboardSendFailed) {
            scheduleClipboardRetry(id);
            return;
        }
        if (result == ServerProxy::kClipboardSendPending) {
            m_clipboardSendPending[id] = true;
            m_pendingClipboardData[id].set(data);
            scheduleClipboardRetry(id);
            return;
        }
        m_sentClipboard[id] = true;
        m_clipboardSendPending[id] = false;
        m_dataClipboard[id].set(data);
    }
}

bool
Client::finishPendingClipboardSend(ClipboardID id, const std::string& data)
{
    if (!m_clipboardSendPending[id]) {
        return true;
    }

    if (!m_pendingClipboardData[id].matches(data)) {
        if (!m_server->cleanupClipboardSendThread(true)) {
            scheduleClipboardRetry(id);
            return false;
        }
        m_clipboardSendPending[id] = false;
        m_pendingFileClipboardPaths[id].clear();
        return true;
    }

    bool succeeded = false;
    if (!m_server->reapClipboardSendResult(id, succeeded)) {
        scheduleClipboardRetry(id);
        return false;
    }

    m_clipboardSendPending[id] = false;
    if (!succeeded) {
        LOG((CLOG_WARN "clipboard %d async send failed; retrying", id));
        return true;
    }

    m_sentClipboard[id] = true;
    m_dataClipboard[id].set(data);
    const std::vector<barrier::fs::path> filePaths =
        m_pendingFileClipboardPaths[id];
    m_pendingFileClipboardPaths[id].clear();
    if (!filePaths.empty()) {
        sendClipboardSelectionToServer(filePaths);
    }
    return false;
}

void
Client::sendEvent(Event::Type type, void* data)
{
    m_events->addEvent(Event(type, getEventTarget(), data));
}

void
Client::sendConnectionFailedEvent(const char* msg)
{
    if (m_terminalEventSent) {
        return;
    }
    m_terminalEventSent = true;

    FailInfo* info = new FailInfo(msg);
    info->m_retry = true;
    Event event(m_events->forClient().connectionFailed(), getEventTarget(), info, Event::kDontFreeData);
    m_events->addEvent(event);
}

void
Client::sendDisconnectedEvent()
{
    if (m_terminalEventSent) {
        return;
    }
    m_terminalEventSent = true;
    sendEvent(m_events->forClient().disconnected(), NULL);
}

void
Client::sendFileChunk(const void* data)
{
    FileChunk* chunk = static_cast<FileChunk*>(const_cast<void*>(data));
    LOG((CLOG_DEBUG1 "send file chunk"));
    if (m_server == NULL) {
        LOG((CLOG_WARN "dropping stale file chunk because client is disconnected"));
        return;
    }
    const bool hasTransferGeneration =
        (chunk->m_transferId != 0 || m_sendFileTransferId != 0);
    if (hasTransferGeneration && chunk->m_transferId != m_sendFileTransferId) {
        LOG((CLOG_DEBUG "dropping stale file chunk, transfer=%u current=%u",
            chunk->m_transferId, m_sendFileTransferId));
        return;
    }

    // relay
    m_server->fileChunkSending(chunk->m_chunk[0], &chunk->m_chunk[1], chunk->m_dataSize);
}

void
Client::setupConnecting()
{
    assert(m_stream != NULL);

    if (m_args.m_enableCrypto) {
        m_events->adoptHandler(m_events->forIDataSocket().secureConnected(),
                    m_stream->getEventTarget(),
                        new TMethodEventJob<Client>(this,
                                &Client::handleConnected));
    }
    else {
        m_events->adoptHandler(m_events->forIDataSocket().connected(),
                    m_stream->getEventTarget(),
                        new TMethodEventJob<Client>(this,
                                &Client::handleConnected));
    }

    m_events->adoptHandler(m_events->forIDataSocket().connectionFailed(),
                            m_stream->getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleConnectionFailed));
    m_events->adoptHandler(m_events->forISocket().stopRetry(),
                           m_stream->getEventTarget(),
                           new TMethodEventJob<Client>(this,
                               &Client::handleStopRetry));
}

void
Client::setupConnection()
{
    assert(m_stream != NULL);

    m_events->adoptHandler(m_events->forISocket().disconnected(),
                            m_stream->getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleDisconnected));
    m_events->adoptHandler(m_events->forIStream().inputReady(),
                            m_stream->getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleHello));
    m_events->adoptHandler(m_events->forIStream().outputError(),
                            m_stream->getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleOutputError));
    m_events->adoptHandler(m_events->forIStream().inputShutdown(),
                            m_stream->getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleDisconnected));
    m_events->adoptHandler(m_events->forIStream().outputShutdown(),
                            m_stream->getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleDisconnected));

    m_events->adoptHandler(m_events->forISocket().stopRetry(),
                           m_stream->getEventTarget(),
                           new TMethodEventJob<Client>(this, &Client::handleStopRetry));
}

void
Client::setupScreen()
{
    reapDetachedConnectionState();
    assert(m_server == NULL);

    m_ready  = false;
    m_server = new ServerProxy(this, m_stream, m_events);
    m_events->adoptHandler(m_events->forIScreen().shapeChanged(),
                            getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleShapeChanged));
    m_events->adoptHandler(m_events->forClipboard().clipboardGrabbed(),
                            getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleClipboardGrabbed));
}

void
Client::setupTimer()
{
    assert(m_timer == NULL);

    m_timer = m_events->newOneShotTimer(15.0, NULL);
    m_events->adoptHandler(Event::kTimer, m_timer,
                            new TMethodEventJob<Client>(this,
                                &Client::handleConnectTimeout));
}

void
Client::cleanupConnecting()
{
    if (m_stream != NULL) {
        m_events->removeHandler(m_events->forIDataSocket().connected(),
                            m_stream->getEventTarget());
        m_events->removeHandler(m_events->forIDataSocket().secureConnected(),
                            m_stream->getEventTarget());
        m_events->removeHandler(m_events->forIDataSocket().connectionFailed(),
                            m_stream->getEventTarget());
        m_events->removeHandler(m_events->forISocket().stopRetry(),
                            m_stream->getEventTarget());
    }
}

void
Client::cleanupConnection()
{
    FileChunk::releaseReceiveBuffer(m_fileReceiveSession);
    ++m_sendFileTransferId;
    if (m_sendFileTransferId == 0) {
        ++m_sendFileTransferId;
    }
    const bool fileSenderStopped = cleanupSendFileThread(true);

    if (m_stream != NULL) {
        m_events->removeHandler(m_events->forIStream().inputReady(),
                            m_stream->getEventTarget());
        m_events->removeHandler(m_events->forIStream().outputError(),
                            m_stream->getEventTarget());
        m_events->removeHandler(m_events->forIStream().inputShutdown(),
                            m_stream->getEventTarget());
        m_events->removeHandler(m_events->forIStream().outputShutdown(),
                            m_stream->getEventTarget());
        m_events->removeHandler(m_events->forISocket().disconnected(),
                            m_stream->getEventTarget());
        m_events->removeHandler(m_events->forISocket().stopRetry(),
                                m_stream->getEventTarget());
        if (!fileSenderStopped) {
            LOG((CLOG_WARN "detaching stream so reconnect can proceed while file sender finishes"));
            detachStreamForSendFileThread();
            return;
        }

        delete m_stream;
        m_stream = NULL;
        m_socket = NULL;
    }
}

void
Client::cleanupScreen()
{
    releaseDetachedServerProxies();
    cleanupClipboardRetryTimer();
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        m_clipboardRetryPending[id] = false;
        m_clipboardRetryCount[id] = 0;
        m_clipboardSendPending[id] = false;
    }
    if (m_server != NULL) {
        if (m_ready) {
            m_screen->disable();
            m_ready = false;
        }
        m_events->removeHandler(m_events->forIScreen().shapeChanged(),
                            getEventTarget());
        m_events->removeHandler(m_events->forClipboard().clipboardGrabbed(),
                            getEventTarget());
        if (!m_server->cleanupClipboardSendThread(true)) {
            LOG((CLOG_WARN "detaching server proxy so reconnect can proceed while clipboard sender finishes"));
            detachServerProxyForClipboardThread();
            return;
        }
        delete m_server;
        m_server = NULL;
    }
}

void
Client::cleanupClipboardRetryTimer()
{
    if (m_clipboardRetryTimer != NULL) {
        m_events->removeHandler(Event::kTimer, m_clipboardRetryTimer);
        m_events->deleteTimer(m_clipboardRetryTimer);
        m_clipboardRetryTimer = NULL;
    }
}

bool
Client::hasPendingClipboardRetry() const
{
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        if (m_clipboardRetryPending[id]) {
            return true;
        }
    }
    return false;
}

void
Client::scheduleClipboardRetry(ClipboardID id)
{
    if (id >= kClipboardEnd) {
        return;
    }

    if (m_clipboardRetryCount[id] >= kClipboardReadRetryLimit) {
        LOG((CLOG_WARN "clipboard %d could not be read after %u deferred retry attempt(s); keeping previous data",
            id, kClipboardReadRetryLimit));
        m_clipboardRetryPending[id] = false;
        m_clipboardRetryCount[id] = 0;
        if (!hasPendingClipboardRetry()) {
            cleanupClipboardRetryTimer();
        }
        return;
    }

    m_clipboardRetryPending[id] = true;
    if (m_clipboardRetryTimer == NULL) {
        m_clipboardRetryTimer =
            m_events->newOneShotTimer(kClipboardReadRetrySeconds, NULL);
        m_events->adoptHandler(Event::kTimer, m_clipboardRetryTimer,
                            new TMethodEventJob<Client>(this,
                                &Client::handleClipboardRetry));
    }
}

void
Client::cleanupTimer()
{
    if (m_timer != NULL) {
        m_events->removeHandler(Event::kTimer, m_timer);
        m_events->deleteTimer(m_timer);
        m_timer = NULL;
    }
}

void
Client::cleanupStream()
{
    if (!cleanupSendFileThread(true)) {
        LOG((CLOG_WARN "detaching stream so reconnect can proceed while file sender finishes"));
        detachStreamForSendFileThread();
        return;
    }
    delete m_stream;
    m_stream = NULL;
    m_socket = NULL;
}

void
Client::detachServerProxyForClipboardThread()
{
    if (m_server == NULL) {
        return;
    }

    m_server->detachForDeferredCleanup();
    m_detachedServerProxies.push_back(m_server);
    m_server = NULL;

    if (m_stream == NULL) {
        return;
    }

    m_events->removeHandler(m_events->forIStream().inputReady(),
                            m_stream->getEventTarget());
    m_events->removeHandler(m_events->forIStream().outputError(),
                            m_stream->getEventTarget());
    m_events->removeHandler(m_events->forIStream().inputShutdown(),
                            m_stream->getEventTarget());
    m_events->removeHandler(m_events->forIStream().outputShutdown(),
                            m_stream->getEventTarget());
    m_events->removeHandler(m_events->forISocket().disconnected(),
                            m_stream->getEventTarget());
    m_events->removeHandler(m_events->forISocket().stopRetry(),
                            m_stream->getEventTarget());
    m_stream->close();
    m_detachedSendFileStreams.push_back(m_stream);
    m_stream = NULL;
    m_socket = NULL;
}

void
Client::releaseDetachedServerProxies()
{
    for (std::vector<ServerProxy*>::iterator i = m_detachedServerProxies.begin();
         i != m_detachedServerProxies.end();) {
        ServerProxy* proxy = *i;
        if (!proxy->cleanupClipboardSendThread(true)) {
            ++i;
            continue;
        }
        delete proxy;
        i = m_detachedServerProxies.erase(i);
    }

    if (m_detachedServerProxies.empty() && m_sendFileThread == NULL) {
        releaseDetachedSendFileStream();
    }
}

void
Client::reapDetachedConnectionState()
{
    reapSendFileThreadIfReady();
    releaseDetachedServerProxies();
}

void
Client::detachStreamForSendFileThread()
{
    if (m_stream == NULL) {
        return;
    }

    m_stream->close();
    m_detachedSendFileStreams.push_back(m_stream);
    m_stream = NULL;
    m_socket = NULL;
}

void
Client::releaseDetachedSendFileStream()
{
    if (!m_detachedServerProxies.empty()) {
        LOG((CLOG_DEBUG "keeping detached stream until server proxy sender finishes"));
        return;
    }

    for (std::vector<barrier::IStream*>::iterator i = m_detachedSendFileStreams.begin();
         i != m_detachedSendFileStreams.end(); ++i) {
        delete *i;
    }
    m_detachedSendFileStreams.clear();
}

void
Client::handleConnected(const Event&, void*)
{
    LOG((CLOG_DEBUG1 "connected;  wait for hello"));
    cleanupConnecting();
    setupConnection();

    // reset clipboard state
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        m_ownClipboard[id]  = false;
        m_sentClipboard[id] = false;
        m_clipboardSendPending[id] = false;
        m_clipboardRetryPending[id] = false;
        m_clipboardRetryCount[id] = 0;
        m_timeClipboard[id] = 0;
    }
    cleanupClipboardRetryTimer();
}

void
Client::handleConnectionFailed(const Event& event, void*)
{
    IDataSocket::ConnectionFailedInfo* info =
        static_cast<IDataSocket::ConnectionFailedInfo*>(event.getData());

    cleanupTimer();
    cleanupConnecting();
    cleanupStream();
    LOG((CLOG_DEBUG1 "connection failed"));
    sendConnectionFailedEvent(info->m_what.c_str());
    delete info;
}

void
Client::handleConnectTimeout(const Event&, void*)
{
    cleanupTimer();
    cleanupConnecting();
    cleanupConnection();
    cleanupStream();
    LOG((CLOG_DEBUG1 "connection timed out"));
    sendConnectionFailedEvent("Timed out");
}

void
Client::handleOutputError(const Event&, void*)
{
    cleanupTimer();
    cleanupScreen();
    cleanupConnection();
    LOG((CLOG_WARN "error sending to server"));
    sendDisconnectedEvent();
}

void
Client::handleDisconnected(const Event&, void*)
{
    cleanupTimer();
    cleanupScreen();
    cleanupConnection();
    LOG((CLOG_DEBUG1 "disconnected"));
    sendDisconnectedEvent();
}

void
Client::handleShapeChanged(const Event&, void*)
{
    LOG((CLOG_DEBUG "resolution changed"));
    m_server->onInfoChanged();
}

void
Client::handleClipboardGrabbed(const Event& event, void*)
{
    if (!m_enableClipboard) {
        return;
    }

    const IScreen::ClipboardInfo* info =
        static_cast<const IScreen::ClipboardInfo*>(event.getData());

    // grab ownership
    m_server->onGrabClipboard(info->m_id);

    // we now own the clipboard and it has not been sent to the server
    m_ownClipboard[info->m_id]  = true;
    m_sentClipboard[info->m_id] = false;
    m_clipboardSendPending[info->m_id] = false;
    m_timeClipboard[info->m_id] = 0;
    m_clipboardRetryPending[info->m_id] = false;
    m_clipboardRetryCount[info->m_id] = 0;

    // Do not push local clipboard changes while the user is still working on
    // this machine. The clipboard is synchronized when this screen is left.
}

void
Client::handleClipboardRetry(const Event&, void*)
{
    cleanupClipboardRetryTimer();

    if (!m_enableClipboard || m_server == NULL) {
        for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
            m_clipboardRetryPending[id] = false;
            m_clipboardRetryCount[id] = 0;
        }
        return;
    }

    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        if (!m_clipboardRetryPending[id]) {
            continue;
        }

        ++m_clipboardRetryCount[id];
        LOG((CLOG_DEBUG "retrying deferred clipboard %d read, attempt %u",
            id, m_clipboardRetryCount[id]));
        sendClipboard(id);
    }
}

void
Client::handleHello(const Event&, void*)
{
    SInt16 major, minor;
    if (!ProtocolUtil::readf(m_stream, kMsgHello, &major, &minor)) {
        sendConnectionFailedEvent("Protocol error from server, check encryption settings");
        cleanupTimer();
        cleanupConnection();
        return;
    }

    // check versions
    LOG((CLOG_DEBUG1 "got hello version %d.%d", major, minor));
    if (major < kProtocolMajorVersion ||
        (major == kProtocolMajorVersion && minor < kProtocolMinorVersion)) {
        sendConnectionFailedEvent(XIncompatibleClient(major, minor).what());
        cleanupTimer();
        cleanupConnection();
        return;
    }

    // say hello back
    LOG((CLOG_DEBUG1 "say hello version %d.%d", kProtocolMajorVersion, kProtocolMinorVersion));
    ProtocolUtil::writef(m_stream, kMsgHelloBack,
                            kProtocolMajorVersion,
                            kProtocolMinorVersion, &m_name);

    // now connected but waiting to complete handshake
    setupScreen();
    cleanupTimer();

    // make sure we process any remaining messages later.  we won't
    // receive another event for already pending messages so we fake
    // one.
    if (m_stream->isReady()) {
        m_events->addEvent(Event(m_events->forIStream().inputReady(),
                            m_stream->getEventTarget()));
    }
}

void
Client::handleSuspend(const Event&, void*)
{
    LOG((CLOG_INFO "suspend"));
    m_suspended       = true;
    bool wasConnected = isConnected();
    disconnect(NULL);
    m_connectOnResume = wasConnected;
}

void
Client::handleResume(const Event&, void*)
{
    LOG((CLOG_INFO "resume"));
    m_suspended = false;
    if (m_connectOnResume) {
        m_connectOnResume = false;
        connect();
    }
}

void
Client::handleFileChunkSending(const Event& event, void*)
{
    sendFileChunk(event.getData());
}

void
Client::handleFileKeepAlive(const Event&, void*)
{
    reapDetachedConnectionState();
    if (m_server == NULL) {
        return;
    }
    m_server->keepAlive();
}

void
Client::handleFileRecieveCompleted(const Event& event, void*)
{
    onFileRecieveCompleted();
}

void
Client::onFileRecieveCompleted()
{
	if (isReceivedFileSizeValid()) {
	    std::shared_ptr<CompletedFileTransfer> transfer = takeCompletedFileTransfer();
	    startDropDirTransfer(transfer);
	    return;
	}

	LOG((CLOG_ERR "received file completion with invalid size, expected=%llu actual=%llu",
		static_cast<unsigned long long>(m_fileReceiveSession.expectedSize()),
		static_cast<unsigned long long>(m_fileReceiveSession.receivedSize())));
	FileChunk::releaseReceiveBuffer(m_fileReceiveSession);
}

void
Client::handleFileClipboardReady(const Event& event, void*)
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
Client::handleDropDirWriteFinished(const Event&, void*)
{
    drainDropDirTransferQueue();
}

void
Client::handleStopRetry(const Event&, void*)
{
    m_args.m_restartable = false;
}

void
Client::startDropDirTransfer(std::shared_ptr<CompletedFileTransfer> transfer)
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
Client::queueDropDirTransfer(std::shared_ptr<CompletedFileTransfer> transfer)
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
Client::drainDropDirTransferQueue()
{
    if (!reapWriteToDropDirThreadIfReady() || m_pendingDropDirTransfers.empty()) {
        return;
    }

    std::shared_ptr<CompletedFileTransfer> transfer = m_pendingDropDirTransfers.front();
    m_pendingDropDirTransfers.pop_front();
    startDropDirTransfer(transfer);
}

void
Client::releasePendingDropDirTransfers()
{
    for (std::deque<std::shared_ptr<CompletedFileTransfer> >::iterator i = m_pendingDropDirTransfers.begin();
         i != m_pendingDropDirTransfers.end(); ++i) {
        if (*i) {
            FileChunk::releaseReceiveBuffer((*i)->data, (*i)->expectedSize, &(*i)->spoolPath);
        }
    }
    m_pendingDropDirTransfers.clear();
}

void Client::write_to_drop_dir_thread(std::shared_ptr<CompletedFileTransfer> transfer)
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
	        barrier::DataDirectories::profile() / "clipboard-cache" / "client";
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
Client::publishMaterializedFileClipboard(const std::vector<std::string>& paths,
                                         const std::string& sessionId)
{
    Clipboard clipboard;
    if (!RemoteFileClipboard::buildMaterializedClipboard(
            utf8PathsToFsPaths(paths), sessionId, clipboard)) {
        LOG((CLOG_ERR "failed to publish remote clipboard locally: session=%s", sessionId.c_str()));
        return;
    }

    m_screen->setClipboard(kClipboardClipboard, &clipboard);
    m_sentClipboard[kClipboardClipboard] = true;
    m_clipboardSendPending[kClipboardClipboard] = false;
    m_dataClipboard[kClipboardClipboard].set(clipboard.marshall());
    LOG((CLOG_INFO "remote clipboard published locally: session=%s items=%lu",
         sessionId.c_str(),
         static_cast<unsigned long>(paths.size())));
    m_remoteFileClipboardSession.clear();
}

void
Client::sendClipboardSelectionToServer(const std::vector<barrier::fs::path>& sourcePaths)
{
    if (sourcePaths.empty()) {
        return;
    }

    if (!reapSendFileThreadIfReady()) {
		LOG((CLOG_DEBUG "remote clipboard prefetch already active; keeping the current sender"));
		return;
	}

    auto chunker = std::make_shared<StreamChunker>();
    m_sendFileChunker = chunker;
	m_sendFileIsClipboardPrefetch = true;
    barrier::IStream* stream = m_stream;
    ++m_sendFileTransferId;
    if (m_sendFileTransferId == 0) {
        ++m_sendFileTransferId;
    }
    const UInt32 transferId = m_sendFileTransferId;
    LOG((CLOG_INFO "remote clipboard prefetch started: direction=client-to-server items=%lu",
         static_cast<unsigned long>(sourcePaths.size())));
    m_sendFileThread = new Thread([this, sourcePaths, stream, chunker, transferId]() {
        send_clipboard_file_thread(sourcePaths, stream, chunker, transferId);
    });
}

void
Client::send_clipboard_file_thread(const std::vector<barrier::fs::path>& sourcePaths,
                                   barrier::IStream* stream,
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
        chunker->sendFile(packagePath.u8string().c_str(), m_events, this, stream, transferId);
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

void
Client::dragInfoReceived(UInt32 fileNum, std::string data)
{
    // TODO: fix duplicate function from CServer
    if (!m_args.m_enableDragDrop) {
        LOG((CLOG_DEBUG "drag drop not enabled, ignoring drag info."));
        return;
    }

    DragInformation::parseDragInfo(m_dragFileList, fileNum, data);

    m_screen->startDraggingFiles(m_dragFileList);
}

bool
Client::isReceivedFileSizeValid()
{
    if (!m_fileReceiveSession.isComplete()) {
        return false;
    }
    if (!m_fileReceiveSession.spoolPath().empty()) {
        return barrier::fs::exists(m_fileReceiveSession.spoolPath()) &&
            static_cast<std::size_t>(barrier::fs::file_size(m_fileReceiveSession.spoolPath())) ==
                m_fileReceiveSession.expectedSize();
    }
    return m_fileReceiveSession.expectedSize() == m_fileReceiveSession.data().size();
}

void
Client::sendFileToServer(const std::string& filename)
{
    if (!cleanupSendFileThread(true)) {
        LOG((CLOG_WARN "file send skipped because previous file sender is still stopping"));
        return;
    }

    auto chunker = std::make_shared<StreamChunker>();
    m_sendFileChunker = chunker;
	m_sendFileIsClipboardPrefetch = false;
    barrier::IStream* stream = m_stream;
    ++m_sendFileTransferId;
    if (m_sendFileTransferId == 0) {
        ++m_sendFileTransferId;
    }
    const UInt32 transferId = m_sendFileTransferId;
    m_sendFileThread = new Thread([this, stream, filename, chunker, transferId]() {
        send_file_thread(stream, filename, chunker, transferId);
    });
}

void Client::send_file_thread(barrier::IStream* stream,
                              const std::string& filename,
                              const std::shared_ptr<StreamChunker>& chunker,
                              UInt32 transferId)
{
    barrier::fs::path sourcePath;
    barrier::fs::path tempPackagePath;
    try {
        Thread::testCancel();
        std::string error;
        if (!prepareTransferSource(filename.c_str(), sourcePath, tempPackagePath, error)) {
            throw std::runtime_error(error);
        }

        Thread::testCancel();
        const barrier::fs::path& transferPath =
            tempPackagePath.empty() ? sourcePath : tempPackagePath;
        chunker->sendFile(transferPath.u8string().c_str(), m_events, this, stream, transferId);
    }
    catch (XThread&) {
        if (!tempPackagePath.empty()) {
            barrier::fs::remove(tempPackagePath);
        }
        throw;
    }
    catch (std::runtime_error& error) {
        LOG((CLOG_ERR "failed sending file chunks: %s", error.what()));
    }

    if (!tempPackagePath.empty()) {
        barrier::fs::remove(tempPackagePath);
    }
}

bool
Client::cleanupSendFileThread(bool cancel)
{
    if (cancel && m_sendFileChunker) {
        m_sendFileChunker->interruptFile();
    }

    if (m_sendFileThread != NULL) {
        if (!m_sendFileThread->wait(0.0)) {
            if (cancel) {
                LOG((CLOG_DEBUG "requesting asynchronous file sender cancellation"));
                m_sendFileThread->cancel();
                m_sendFileThread->unblockPollSocket();
            }
            return false;
        }
		delete m_sendFileThread;
		m_sendFileThread = NULL;
	}

	m_sendFileChunker.reset();
	m_sendFileIsClipboardPrefetch = false;
    releaseDetachedServerProxies();
    releaseDetachedSendFileStream();
	return true;
}

bool
Client::reapSendFileThreadIfReady()
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
    releaseDetachedSendFileStream();
    return true;
}

bool
Client::cleanupWriteToDropDirThread()
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
Client::reapWriteToDropDirThreadIfReady()
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

std::shared_ptr<Client::CompletedFileTransfer>
Client::takeCompletedFileTransfer()
{
	std::shared_ptr<CompletedFileTransfer> transfer(new CompletedFileTransfer());
	m_fileReceiveSession.takeCompleted(
		transfer->data, transfer->expectedSize, transfer->spoolPath);
	if (m_screen != NULL) {
		transfer->dropTarget = m_screen->getDropTarget();
	}
	transfer->dragFileList.swap(m_dragFileList);
	transfer->remoteFileClipboardSession = m_remoteFileClipboardSession;
	return transfer;
}

void
Client::sendDragInfo(UInt32 fileCount, std::string& info, size_t size)
{
    m_server->sendDragInfo(fileCount, info.c_str(), size);
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
testClientPrepareTransferSource(const char* filename,
                                barrier::fs::path& sourcePath,
                                barrier::fs::path& tempPackagePath,
                                std::string& error)
{
    return prepareTransferSource(filename, sourcePath, tempPackagePath, error);
}
