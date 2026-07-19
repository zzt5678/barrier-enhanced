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
#include "barrier/FileTransferReceiver.h"
#include "barrier/FileTransferSendState.h"
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
#include "mt/ThreadShutdown.h"
#include "mt/XThread.h"
#include "net/TCPSocket.h"
#include "net/IDataSocket.h"
#include "net/ISocketFactory.h"
#include "net/SecureSocket.h"
#include "arch/Arch.h"
#include "base/Log.h"
#include "base/IEventQueue.h"
#include "base/TMethodEventJob.h"

#if defined(_WIN32)
#include "common/win32/SessionUserImpersonation.h"
#endif

#include <cstring>
#include <charconv>
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
const double kFileReceiveCompletionPollSeconds = 0.01;
const double kBulkRetryInitialSeconds = 0.25;
const double kBulkRetryMaximumSeconds = 4.0;
const double kFileTransferCancelAckTimeoutSeconds = 15.0;
const double kFileSenderReapPollSeconds = 0.01;
const UInt32 kFileSenderDrainMaxPolls = 3000;
const UInt32 kFileSenderDrainMaxStalledPolls = 500;

bool prepareTransferSource(const char* filename,
                           barrier::fs::path& sourcePath,
                           barrier::fs::path& tempPackagePath,
                           std::string& error);

const size_t kMaxPendingDropDirTransfers = 4;
const size_t kMaxPendingDropDirTransferMemoryBytes = 32 * 1024 * 1024;

bool parseTransactionalFileSize(const FileChunk& chunk, UInt32& expectedSize)
{
    expectedSize = 0;
    if (chunk.m_dataSize == 0) {
        return false;
    }
    const char* first = &chunk.m_chunk[1];
    const char* last = first + chunk.m_dataSize;
    const std::from_chars_result parsed =
        std::from_chars(first, last, expectedSize, 10);
    return parsed.ec == std::errc() && parsed.ptr == last &&
        expectedSize <= barrier::FileTransferProtocol::kMaxTransferSize;
}

bool isValidClipboardSessionId(const std::string& sessionId)
{
    if (sessionId.size() !=
        barrier::FileTransferProtocol::kClipboardSessionHexSize) {
        return false;
    }
    for (char value : sessionId) {
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}

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
    m_bulkHandshakeStream(NULL),
    m_bulkHandshakeState(kBulkIdle),
    m_bulkBindingToken(),
    m_bulkActiveToken(),
    m_bulkChannel(),
    m_bulkRetryTimer(NULL),
    m_bulkRetryToken(),
    m_bulkRetryAttempt(0),
    m_controlConnectionBinding(),
    m_detachedSendFileStreams(),
    m_detachedServerProxies(),
    m_timer(NULL),
    m_clipboardRetryTimer(NULL),
    m_fileReceiveCompletionTimer(NULL),
    m_fileReceiveCompletionGeneration(0),
    m_server(NULL),
    m_ready(false),
    m_active(false),
    m_inputBackendPoisoned(false),
    m_protocolMinorVersion(kProtocolMinorVersion),
    m_suspended(false),
    m_connectOnResume(false),
	    m_terminalEventSent(false),
	    m_events(events),
	    m_fileReceiveSession(),
    m_sendFileThread(NULL),
    m_sendFileBulkChannel(),
    m_sendFileTransferId(0),
    m_nextSendFileTransferSequence(0),
    m_sendFileTransactionState(),
    m_sendFileIsClipboardPrefetch(false),
    m_sendFileStarted(false),
    m_sendFileStartAcknowledged(false),
    m_sendFileCompletionPending(false),
    m_sendFileCancelAckPending(false),
    m_sendFileCancelAckTimeoutTimer(NULL),
    m_sendFileCancelAckTimeoutTransferId(0),
    m_sendFileReapTimer(NULL),
    m_sendFileReapTransferId(0),
    m_sendFileDrainPollCount(0),
    m_sendFileDrainStallCount(0),
    m_sendFileLastBufferedOutput(0),
    m_pendingManualFileSend(),
    m_pendingFileClipboardPrefetchPaths(),
    m_pendingFileClipboardPrefetchSession(),
    m_pendingFileClipboardPrefetchRevision(),
    m_writeToDropDirThread(NULL),
    m_pendingDropDirTransfers(),
    m_transactionalReceiveId(0),
    m_transactionalReceive(),
    m_socket(NULL),
    m_useSecureNetwork(args.m_enableCrypto),
    m_args(args),
    m_enableClipboard(true),
    m_clipboardRevision(),
    m_remoteFileClipboardRevision(),
    m_readyFileClipboardRevision(),
    m_nextClipboardPublicationId(0),
    m_pendingMaterializedClipboardPublicationId(0),
    m_lastCommittedClipboardPublicationId(0),
    m_pendingMaterializedClipboardData(),
    m_pendingMaterializedClipboardSession(),
    m_pendingMaterializedClipboardRevision(),
    m_fileReceiveClipboardGeneration(0),
    m_fileReceiveClipboardRevision()
{
    assert(m_socketFactory != NULL);
    assert(m_screen        != NULL);

    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        m_ownClipboard[id] = false;
        m_sentClipboard[id] = false;
        m_clipboardSendPending[id] = false;
        m_pendingImmutableClipboard[id].reset();
        m_clipboardRetryPending[id] = false;
        m_clipboardRetryCount[id] = 0;
        m_timeClipboard[id] = 0;
        clearPendingFileClipboard(id);
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

    cleanupSendFileCancelAckTimeout();
    cleanupSendFileReap();

	if (!cleanupSendFileThread(true) && m_sendFileThread != NULL) {
		barrier::waitForFinalThreadShutdown(
			"client file sender",
			barrier::kFinalThreadShutdownDeadlineSeconds,
			[this](double timeout) { return m_sendFileThread->wait(timeout); });
		delete m_sendFileThread;
		m_sendFileThread = NULL;
		m_sendFileChunker.reset();
        releaseDetachedSendFileStream();
	}
	if (!cleanupWriteToDropDirThread() && m_writeToDropDirThread != NULL) {
		barrier::waitForFinalThreadShutdown(
			"client drop-dir writer",
			barrier::kFinalThreadShutdownDeadlineSeconds,
			[this](double timeout) { return m_writeToDropDirThread->wait(timeout); });
		delete m_writeToDropDirThread;
		m_writeToDropDirThread = NULL;
	}
	cleanupFileReceiveCompletionPoll();
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
Client::connectBulkChannel(const std::string& token)
{
    if (m_protocolMinorVersion < 9 || m_protocolMinorVersion >= 12 ||
        token.empty() || m_stream == NULL || m_server == NULL) {
        return;
    }
    (void)acceptBulkChannelOffer(token);
}

bool
Client::connectBulkChannel(const std::string& token,
                           const std::string& connectionBinding)
{
    if (m_protocolMinorVersion < 12 || token.empty() || m_stream == NULL ||
        m_server == NULL || !isValidConnectionBinding(connectionBinding)) {
        return false;
    }
    if (!m_controlConnectionBinding.empty() &&
        m_controlConnectionBinding != connectionBinding) {
        LOG((CLOG_WARN
            "rejected bulk offer with changed control connection binding"));
        return false;
    }
    m_controlConnectionBinding = connectionBinding;
    return acceptBulkChannelOffer(token);
}

bool
Client::acceptBulkChannelOffer(const std::string& token)
{
    if (m_bulkHandshakeStream != NULL && token == m_bulkBindingToken) {
        LOG((CLOG_DEBUG "ignoring duplicate offer for the in-flight bulk handshake"));
        return true;
    }
    if (m_bulkChannel && m_bulkChannel->isActive()) {
        if (token == m_bulkActiveToken) {
            LOG((CLOG_DEBUG "ignoring duplicate offer for the active bulk channel"));
            return true;
        }
        cleanupBulkRetry();
        m_bulkRetryToken = token;
        LOG((CLOG_DEBUG
            "remembering replacement bulk offer until the active route disconnects"));
        return true;
    }

    cleanupBulkRetry();
    m_bulkRetryToken = token;
    return startBulkConnection(token);
}

bool
Client::startBulkConnection(const std::string& token)
{
    if (m_protocolMinorVersion < 9 || token.empty() || m_stream == NULL ||
        m_server == NULL || (m_bulkChannel && m_bulkChannel->isActive()) ||
        (m_protocolMinorVersion >= 12 &&
         !isValidConnectionBinding(m_controlConnectionBinding))) {
        return false;
    }

    if (!abandonBulkHandshake()) {
        LOG((CLOG_ERR
            "failed to replace bulk handshake after rejecting pending file start"));
        return false;
    }
    if (m_bulkChannel) {
        m_bulkChannel->close();
        m_bulkChannel.reset();
    }

    ConnectionSecurityLevel securityLevel = ConnectionSecurityLevel::PLAINTEXT;
    if (m_useSecureNetwork) {
        securityLevel = ConnectionSecurityLevel::ENCRYPTED_AUTHENTICATED;
    }

    try {
        IDataSocket* socket = m_socketFactory->create(
            ARCH->getAddrFamily(m_serverAddress.getAddress()), securityLevel);
        if (socket == NULL) {
            throw XBase("could not create bulk socket");
        }
        m_bulkHandshakeStream = new PacketStreamFilter(m_events, socket, true);
        m_bulkBindingToken = token;
        m_bulkHandshakeState = kBulkWaitingForHello;

        Event::Type connectedType = m_useSecureNetwork ?
            m_events->forIDataSocket().secureConnected() :
            m_events->forIDataSocket().connected();
        m_events->adoptHandler(connectedType,
            m_bulkHandshakeStream->getEventTarget(),
            new TMethodEventJob<Client>(this, &Client::handleBulkConnected));
        m_events->adoptHandler(m_events->forIDataSocket().connectionFailed(),
            m_bulkHandshakeStream->getEventTarget(),
            new TMethodEventJob<Client>(this,
                &Client::handleBulkConnectionFailed));

        LOG((CLOG_DEBUG1 "connecting separate bulk channel"));
        socket->connect(m_serverAddress);
        return true;
    }
    catch (const XBase& e) {
        LOG((CLOG_WARN "bulk connection setup failed; retry scheduled: %s",
             e.what()));
        const std::string failedToken = token;
        cleanupBulkHandshake();
        scheduleBulkRetry(failedToken);
        return m_bulkRetryTimer != NULL;
    }
}

std::shared_ptr<barrier::BulkChannel>
Client::acquireBulkChannel() const
{
    if (m_bulkChannel && m_bulkChannel->isActive()) {
        return m_bulkChannel;
    }
    return std::shared_ptr<barrier::BulkChannel>();
}

bool
Client::isBoundBulkHandshakeWaitingForAck(
    const std::string& connectionBinding) const
{
    return m_protocolMinorVersion >= 12 &&
        m_bulkHandshakeStream != NULL &&
        m_bulkHandshakeState == kBulkWaitingForAck &&
        isValidConnectionBinding(connectionBinding) &&
        m_controlConnectionBinding == connectionBinding;
}

bool
Client::handleBulkMessage(const UInt8* code, barrier::IStream* stream)
{
    if (!m_bulkChannel || !m_bulkChannel->isActive() ||
        m_bulkChannel->getStream() != stream) {
        LOG((CLOG_WARN "rejecting payload from a stale bulk route"));
        return false;
    }
    return m_server != NULL && m_server->handleBulkMessage(code, stream);
}

void
Client::handleBulkInputPauseFailed(barrier::BulkChannel* channel,
                                   std::uint64_t generation)
{
    if (!m_bulkChannel || m_bulkChannel.get() != channel ||
        !m_fileReceiveSession.matchesGeneration(generation)) {
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
        "bulk input pause failed; cancelling receive generation=%llu state=%d",
        static_cast<unsigned long long>(generation), static_cast<int>(state)));
    cleanupFileReceiveCompletionPoll();
    FileChunk::releaseReceiveBuffer(m_fileReceiveSession);
    m_fileReceiveClipboardGeneration = 0;
    m_fileReceiveRemoteFileClipboardSession.clear();
    m_fileReceiveClipboardRevision.reset();
}

void
Client::handleBulkDisconnected(barrier::BulkChannel* channel,
                               std::uint64_t pausedGeneration)
{
    const bool currentRoute = m_bulkChannel &&
        m_bulkChannel.get() == channel;
    if (m_sendFileBulkChannel &&
        m_sendFileBulkChannel.get() == channel &&
        m_sendFileTransactionState) {
        m_sendFileTransactionState->connectionLost();
        m_sendFileCompletionPending = true;
        cleanupSendFileCancelAckTimeout();
        m_sendFileCancelAckPending = false;
    }
    if (m_sendFileBulkChannel &&
        m_sendFileBulkChannel.get() == channel && m_sendFileChunker) {
        m_sendFileChunker->interruptFile();
    }
    if (m_server != NULL) {
        m_server->handleBulkDisconnected(channel);
    }

    if (currentRoute) {
        if (pausedGeneration != 0) {
            handleBulkInputPauseFailed(channel, pausedGeneration);
        }
        else {
            const FileReceiveSession::State receiveState =
                m_fileReceiveSession.state();
            if (receiveState == FileReceiveSession::kReceiving ||
                receiveState == FileReceiveSession::kFinalizing ||
                receiveState == FileReceiveSession::kComplete ||
                receiveState == FileReceiveSession::kFailed ||
                receiveState == FileReceiveSession::kDiscarding) {
                cleanupFileReceiveCompletionPoll();
                FileChunk::releaseReceiveBuffer(m_fileReceiveSession);
                m_fileReceiveClipboardGeneration = 0;
                m_fileReceiveRemoteFileClipboardSession.clear();
                m_fileReceiveClipboardRevision.reset();
            }
        }
        m_bulkActiveToken.clear();
        LOG((CLOG_WARN "bulk channel disconnected; control connection remains active"));
        if (!m_bulkRetryToken.empty()) {
            LOG((CLOG_DEBUG
                "scheduling the latest replacement bulk offer after route disconnect"));
            scheduleBulkRetry(m_bulkRetryToken);
        }
    }
}

void
Client::handleClipboardSendRouteFailure(ClipboardID id)
{
    if (id >= kClipboardEnd) {
        return;
    }
    m_sentClipboard[id] = false;
    LOG((CLOG_WARN
        "clipboard %d send route was lost before queued frames were written; retrying the current revision",
        id));
    scheduleClipboardRetry(id);
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

bool
Client::canAcceptInputHandoff() const
{
    return m_ready && !m_active && !m_inputBackendPoisoned &&
        m_screen != NULL && m_screen->canEnter();
}

std::uint64_t
Client::inputHandoffGeneration() const
{
    return m_screen == NULL ? 0 : m_screen->inputGeneration();
}

bool
Client::negotiateProtocolVersion(SInt16 serverMajor, SInt16 serverMinor,
                                 SInt16& negotiatedMinor)
{
    if (serverMajor != kProtocolMajorVersion ||
        serverMinor < kProtocolMinimumMinorVersion) {
        return false;
    }

    negotiatedMinor = serverMinor < kProtocolMinorVersion ?
        serverMinor : kProtocolMinorVersion;
    return true;
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
Client::enter(SInt32 xAbs, SInt32 yAbs, UInt32 seqNum, KeyModifierMask mask, bool)
{
    enterInputLease(xAbs, yAbs, seqNum, mask, false);
}

bool
Client::enterInputLease(SInt32 xAbs, SInt32 yAbs, UInt32 seqNum,
                        KeyModifierMask mask, bool)
{
    m_screen->setSequenceNumber(seqNum);
    if (!m_screen->enter(mask)) {
        LOG((CLOG_WARN "input backend rejected enter sequence %u", seqNum));
        return false;
    }

    if (!m_screen->tryMouseMove(xAbs, yAbs)) {
        LOG((CLOG_WARN "input backend rejected initial position for enter sequence %u",
            seqNum));
        if (!m_screen->leave()) {
            LOG((CLOG_ERR "input backend could not roll back failed enter sequence %u",
                seqNum));
            // Screen::enter() already committed platform state.  If the
            // matching leave also fails, the backend's ownership is unknown;
            // another enter would violate Screen's state machine and can
            // strand the cursor.  Quarantine this connection and let the
            // normal disconnected event tear it down on the event queue.
            m_inputBackendPoisoned = true;
            if (m_stream != NULL) {
                m_stream->close();
            }
        }
        return false;
    }

    m_active = true;

    if (m_sendFileChunker && !m_sendFileIsClipboardPrefetch) {
        m_sendFileChunker->interruptFile();
        reapSendFileThreadIfReady();
    }

    return true;
}

bool
Client::leave()
{
    if (!m_screen->leave()) {
        LOG((CLOG_ERR "input backend rejected leave; retaining active lease"));
        return false;
    }

    m_active = false;

    if (m_enableClipboard && m_server != NULL) {
        // Windows can miss the clipboard-viewer notification and only
        // discover the new owner during Screen::leave(). Defer the snapshot
        // so clipboard providers cannot block the input handoff. A previous
        // one-shot timer may be stale after rapid enter/leave cycles, so each
        // leave creates a fresh snapshot attempt.
        cleanupClipboardRetryTimer();
        for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
            m_clipboardRetryPending[id] = false;
            m_clipboardRetryCount[id] = 0;
        }
        for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
            if (m_ownClipboard[id]) {
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
        const bool containsFileList =
            RemoteFileClipboard::containsFileList(*clipboard);
        if (containsFileList && m_protocolMinorVersion < 12) {
            LOG((CLOG_WARN
                "rejected file clipboard from legacy protocol peer"));
            return;
        }
        const bool hasFileClipboard =
            RemoteFileClipboard::readFromClipboard(
                *clipboard, remoteFileClipboard);
        if (containsFileList && !hasFileClipboard) {
            LOG((CLOG_WARN
                "rejected remote clipboard with invalid file metadata"));
            return;
        }
        if (hasFileClipboard && remoteFileClipboard.mode ==
                RemoteFileClipboard::Mode::MaterializedPaths) {
            LOG((CLOG_WARN
                "rejected remote materialized-path clipboard for local safety"));
            return;
        }
        const bool hasSourcePaths = hasFileClipboard &&
            remoteFileClipboard.mode == RemoteFileClipboard::Mode::SourcePaths;
        const bool repeatsPendingSourcePaths =
            hasSourcePaths &&
            remoteFileClipboard.sessionId == m_remoteFileClipboardSession &&
            m_remoteFileClipboardRevision == m_clipboardRevision;

        if (!repeatsPendingSourcePaths) {
            supersedeFileClipboard("remote clipboard update");
        }
        if (hasSourcePaths) {
            m_remoteFileClipboardSession = remoteFileClipboard.sessionId;
            m_remoteFileClipboardRevision = m_clipboardRevision;
            publishClipboard = false;
        }
    }

    if (publishClipboard) {
        m_screen->setClipboard(id, clipboard);
    }

    m_ownClipboard[id]  = false;
    m_sentClipboard[id] = false;
    m_clipboardSendPending[id] = false;
    m_pendingImmutableClipboard[id].reset();
    m_pendingClipboardData[id].clear();
}

bool
Client::setClipboardData(
    ClipboardID id, const std::shared_ptr<const String>& data)
{
    if (id >= kClipboardEnd || !data) {
        return false;
    }
    if (!Clipboard::isValidMarshalled(*data)) {
        LOG((CLOG_WARN "rejected malformed remote clipboard snapshot"));
        return false;
    }
    if (id == kClipboardClipboard && m_protocolMinorVersion < 12 &&
        Clipboard::marshalledHasFormat(*data, IClipboard::kFileList)) {
        LOG((CLOG_WARN
            "rejected file clipboard snapshot from legacy protocol peer"));
        return false;
    }
    if (m_ownClipboard[id] && !m_sentClipboard[id]) {
        LOG((CLOG_INFO
            "preserving unsent local clipboard %d instead of applying remote snapshot",
            id));
        return false;
    }

    if (id == kClipboardClipboard &&
        Clipboard::marshalledHasFormat(*data, IClipboard::kFileList)) {
        Clipboard clipboard;
        clipboard.unmarshall(*data, 0);
        RemoteFileClipboard::Data fileClipboard;
        if (!RemoteFileClipboard::readFromClipboard(
                clipboard, fileClipboard)) {
            LOG((CLOG_WARN "rejected invalid remote file clipboard metadata"));
            return false;
        }
        if (fileClipboard.mode ==
            RemoteFileClipboard::Mode::MaterializedPaths) {
            LOG((CLOG_WARN
                "rejected remote materialized-path clipboard for local safety"));
            return false;
        }
        setClipboard(id, &clipboard);
        return true;
    }

    if (id == kClipboardClipboard) {
        supersedeFileClipboard("remote clipboard snapshot update");
    }
    if (!m_screen->setClipboardSnapshot(id, data)) {
        Clipboard clipboard;
        clipboard.unmarshall(*data, 0);
        setClipboard(id, &clipboard);
        return true;
    }

    m_ownClipboard[id] = false;
    m_sentClipboard[id] = false;
    m_clipboardSendPending[id] = false;
    m_pendingImmutableClipboard[id].reset();
    m_pendingClipboardData[id].clear();
    return true;
}

void
Client::supersedeFileClipboard(const char* reason)
{
    m_clipboardRevision.advance();
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        clearPendingFileClipboard(id);
    }
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
    m_readyFileClipboardSession.clear();
    m_readyFileClipboardPaths.clear();
    m_readyFileClipboardRevision.reset();
    clearMaterializedClipboardPublication();
}

void
Client::bindFileReceiveClipboardRevision()
{
    if (!m_clipboardRevision.valid()) {
        m_clipboardRevision.advance();
    }
    m_fileReceiveClipboardGeneration = m_fileReceiveSession.generation();
    m_fileReceiveClipboardRevision = m_clipboardRevision;
    m_fileReceiveRemoteFileClipboardSession.clear();
    if (!m_remoteFileClipboardSession.empty() &&
        m_remoteFileClipboardRevision == m_clipboardRevision) {
        m_fileReceiveRemoteFileClipboardSession = m_remoteFileClipboardSession;
    }
}

barrier::FileTransferReason
Client::beginTransactionalFileReceive(
    const barrier::FileTransferFrame& startFrame)
{
    if (m_protocolMinorVersion < 12 ||
        startFrame.type != barrier::FileTransferFrameType::kStart ||
        !barrier::FileTransferProtocol::validate(
            startFrame, barrier::FileTransferRole::kPrimary)) {
        return barrier::FileTransferReason::kProtocolError;
    }
    if (m_transactionalReceiveId != 0 || m_transactionalReceive) {
        return barrier::FileTransferReason::kBusy;
    }

    try {
        std::shared_ptr<CompletedFileTransfer> transfer(
            new CompletedFileTransfer());
        transfer->expectedSize = startFrame.totalSize;
        transfer->kind = startFrame.kind;
        if (m_screen != NULL && m_ready &&
            m_screen->getPlatformScreen() != NULL) {
            transfer->dropTarget = m_screen->getDropTarget();
        }

        if (startFrame.kind == barrier::FileTransferKind::kDrag) {
            transfer->dragFileList.swap(m_dragFileList);
        }
        else if (startFrame.kind == barrier::FileTransferKind::kClipboard) {
            transfer->senderClipboardRevision =
                startFrame.clipboardRevision;
            transfer->remoteFileClipboardSession =
                startFrame.clipboardSessionId;
        }

        m_transactionalReceiveId = startFrame.transferId;
        m_transactionalReceive = transfer;
        return barrier::FileTransferReason::kNone;
    }
    catch (...) {
        return barrier::FileTransferReason::kIoError;
    }
}

void
Client::cancelTransactionalFileReceive(UInt32 transferId)
{
    if (transferId == 0 || transferId != m_transactionalReceiveId) {
        return;
    }
    if (m_transactionalReceive) {
        FileChunk::releaseReceiveBuffer(
            m_transactionalReceive->data,
            m_transactionalReceive->expectedSize,
            &m_transactionalReceive->spoolPath);
    }
    m_transactionalReceive.reset();
    m_transactionalReceiveId = 0;
}

barrier::FileTransferReason
Client::acceptTransactionalFileReceive(
    UInt32 transferId, barrier::CompletedFilePayload&& payload)
{
    const auto releasePayload = [&payload]() {
        FileChunk::releaseReceiveBuffer(
            payload.data, payload.expectedSize, &payload.spoolPath);
    };
    if (transferId == 0 || transferId != m_transactionalReceiveId ||
        !m_transactionalReceive) {
        releasePayload();
        return barrier::FileTransferReason::kRejected;
    }
    if (payload.expectedSize != m_transactionalReceive->expectedSize ||
        (payload.spoolPath.empty() &&
         payload.data.size() != payload.expectedSize)) {
        releasePayload();
        cancelTransactionalFileReceive(transferId);
        return barrier::FileTransferReason::kIoError;
    }

    std::shared_ptr<CompletedFileTransfer> transfer =
        m_transactionalReceive;
    if (transfer->kind == barrier::FileTransferKind::kClipboard) {
        const bool metadataMatches =
            m_clipboardRevision.valid() &&
            !m_remoteFileClipboardSession.empty() &&
            m_remoteFileClipboardSession ==
                transfer->remoteFileClipboardSession &&
            m_remoteFileClipboardRevision == m_clipboardRevision;
        if (!metadataMatches) {
            LOG((CLOG_WARN
                "rejecting transactional clipboard payload without current matching metadata: transfer=%u session=%s current=%s",
                transferId,
                transfer->remoteFileClipboardSession.c_str(),
                m_remoteFileClipboardSession.c_str()));
            releasePayload();
            cancelTransactionalFileReceive(transferId);
            return barrier::FileTransferReason::kRejected;
        }

        const bool isPackage = payload.spoolPath.empty() ?
            TransferArchive::isPackageData(payload.data) :
            TransferArchive::isPackageFile(payload.spoolPath);
        if (!isPackage) {
            LOG((CLOG_WARN
                "rejecting transactional clipboard payload that is not a transfer package: transfer=%u",
                transferId));
            releasePayload();
            cancelTransactionalFileReceive(transferId);
            return barrier::FileTransferReason::kIoError;
        }
        transfer->clipboardRevision = m_clipboardRevision;
    }
    transfer->data.swap(payload.data);
    transfer->spoolPath.swap(payload.spoolPath);
    m_transactionalReceive.reset();
    m_transactionalReceiveId = 0;
    return startDropDirTransfer(transfer);
}

bool
Client::signalTransactionalFileTransferAck(
    const barrier::FileTransferFrame& frame)
{
    if (m_protocolMinorVersion < 12) {
        return false;
    }
    if (frame.type != barrier::FileTransferFrameType::kStartAck &&
        frame.type != barrier::FileTransferFrameType::kCancelAck &&
        frame.type != barrier::FileTransferFrameType::kCommitAck) {
        return false;
    }
    if (!m_sendFileTransactionState ||
        frame.transferId != m_sendFileTransferId ||
        frame.transferId != m_sendFileTransactionState->transferId()) {
        LOG((CLOG_DEBUG
            "ignoring stale transactional file acknowledgment: transfer=%u current=%u",
            frame.transferId, m_sendFileTransferId));
        return true;
    }

    bool accepted = false;
    switch (frame.type) {
    case barrier::FileTransferFrameType::kStartAck:
        accepted = m_sendFileTransactionState->signalStartAck(
            frame.transferId, frame.reason);
        if (accepted) {
            m_sendFileStartAcknowledged =
                frame.reason == barrier::FileTransferReason::kNone;
        }
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
        LOG((CLOG_DEBUG
            "ignoring out-of-phase transactional file acknowledgment: transfer=%u type=%u",
            frame.transferId, static_cast<UInt32>(frame.type)));
        return true;
    }
    if (accepted &&
        (frame.type != barrier::FileTransferFrameType::kStartAck ||
         frame.reason != barrier::FileTransferReason::kNone)) {
        serviceSendFileCompletion();
    }
    return true;
}

void
Client::grabClipboard(ClipboardID id)
{
    if (id == kClipboardClipboard) {
        supersedeFileClipboard("remote clipboard grab");
    }
    m_screen->grabClipboard(id);
    m_ownClipboard[id]  = false;
    m_sentClipboard[id] = false;
    m_clipboardSendPending[id] = false;
    m_pendingImmutableClipboard[id].reset();
    m_pendingClipboardData[id].clear();
}

void
Client::setClipboardDirty(ClipboardID, bool)
{
    assert(0 && "shouldn't be called");
}

bool
Client::hasActivePointerLease(const char* inputType) const
{
    if (m_active) {
        return true;
    }

    LOG((CLOG_DEBUG1 "dropping %s without an active input lease", inputType));
    return false;
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
    if (hasActivePointerLease("mouse down")) {
        m_screen->mouseDown(id);
    }
}

void
Client::mouseUp(ButtonID id)
{
    if (hasActivePointerLease("mouse up")) {
        m_screen->mouseUp(id);
    }
}

void
Client::mouseMove(SInt32 x, SInt32 y)
{
    if (hasActivePointerLease("mouse move")) {
        m_screen->mouseMove(x, y);
    }
}

void
Client::mouseRelativeMove(SInt32 dx, SInt32 dy)
{
    if (hasActivePointerLease("relative mouse move")) {
        m_screen->mouseRelativeMove(dx, dy);
    }
}

void
Client::mouseWheel(SInt32 xDelta, SInt32 yDelta)
{
    if (hasActivePointerLease("mouse wheel")) {
        m_screen->mouseWheel(xDelta, yDelta);
    }
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
Client::clearPendingFileClipboard(ClipboardID id)
{
    if (id >= kClipboardEnd) {
        return;
    }
    m_pendingFileClipboardPaths[id].clear();
    m_pendingFileClipboardSession[id].clear();
    m_pendingFileClipboardRevision[id].reset();
}

void
Client::sendClipboard(ClipboardID id)
{
    // note -- m_mutex must be locked on entry
    assert(m_screen != NULL);
    assert(m_server != NULL);

    std::shared_ptr<const String> immutableSnapshot;
    UInt32 immutableSnapshotTime = 0;
    if (m_screen->getClipboardSnapshot(
            id, &immutableSnapshot, &immutableSnapshotTime) &&
        immutableSnapshot &&
        !Clipboard::marshalledHasFormat(
            *immutableSnapshot, IClipboard::kFileList)) {
        m_clipboardRetryPending[id] = false;
        m_clipboardRetryCount[id] = 0;

        if (!finishPendingClipboardSend(
                id, *immutableSnapshot, &immutableSnapshot)) {
            return;
        }

        const bool clipboardTimeChanged =
            m_timeClipboard[id] == 0 ||
            immutableSnapshotTime != m_timeClipboard[id];
        if (clipboardTimeChanged) {
            m_timeClipboard[id] = immutableSnapshotTime;
        }

        if (clipboardTimeChanged || !m_sentClipboard[id]) {
            ServerProxy::ClipboardSendResult result =
                m_server->onClipboardDataChanged(
                    id, immutableSnapshot, false);
            if (result == ServerProxy::kClipboardSendFailed) {
                scheduleClipboardRetry(id);
                return;
            }
            if (result == ServerProxy::kClipboardSendPending) {
                m_clipboardSendPending[id] = true;
                m_pendingImmutableClipboard[id] = immutableSnapshot;
                m_pendingClipboardData[id].clear();
                scheduleClipboardRetry(id);
                return;
            }
            m_sentClipboard[id] = true;
            m_clipboardSendPending[id] = false;
            m_pendingImmutableClipboard[id].reset();
            m_pendingClipboardData[id].clear();
            m_dataClipboard[id].clear();
            clearPendingFileClipboard(id);
        }
        return;
    }

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
                clearPendingFileClipboard(id);
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
        m_readyFileClipboardRevision == m_clipboardRevision &&
        RemoteFileClipboard::pathsMatch(localFileClipboard,
                                        m_readyFileClipboardPaths);
    if (materializedFileEcho) {
        LOG((CLOG_INFO
            "suppressed remote file clipboard echo: session=%s items=%lu",
            m_readyFileClipboardSession.c_str(),
            static_cast<unsigned long>(m_readyFileClipboardPaths.size())));
        m_sentClipboard[id] = true;
        m_clipboardSendPending[id] = false;
        m_pendingImmutableClipboard[id].reset();
        m_pendingClipboardData[id].clear();
        m_dataClipboard[id].set(data);
        clearPendingFileClipboard(id);
        m_readyFileClipboardSession.clear();
        m_readyFileClipboardPaths.clear();
        m_readyFileClipboardRevision.reset();
        return;
    }

    // save and send data if different or not yet sent
    if (clipboardTimeChanged || !m_sentClipboard[id] || !m_dataClipboard[id].matches(data)) {
        if (clipboardSharingStatus ==
            RemoteFileClipboard::AutomaticSharingStatus::ContainsFileList) {
            if (m_protocolMinorVersion < 12) {
                LOG((CLOG_WARN
                    "not sharing local file clipboard with legacy protocol peer"));
                m_sentClipboard[id] = false;
                m_clipboardSendPending[id] = false;
                m_pendingImmutableClipboard[id].reset();
                m_pendingClipboardData[id].clear();
                clearPendingFileClipboard(id);
                return;
            }
            LOG((CLOG_INFO "sending local file clipboard metadata before package transfer"));
            supersedeFileClipboard("local file clipboard update");
            ServerProxy::ClipboardSendResult result =
                m_server->onClipboardChanged(id, &clipboard);
            if (result == ServerProxy::kClipboardSendFailed) {
                clearPendingFileClipboard(id);
                scheduleClipboardRetry(id);
                return;
            }
            if (result == ServerProxy::kClipboardSendPending) {
                m_sentClipboard[id] = false;
                m_clipboardSendPending[id] = true;
                m_pendingImmutableClipboard[id].reset();
                m_pendingClipboardData[id].set(data);
                m_pendingFileClipboardPaths[id] = localFileClipboard.paths;
                m_pendingFileClipboardSession[id] =
                    localFileClipboard.sessionId;
                m_pendingFileClipboardRevision[id] = m_clipboardRevision;
                scheduleClipboardRetry(id);
                return;
            }
            m_sentClipboard[id] = true;
            m_clipboardSendPending[id] = false;
            m_pendingImmutableClipboard[id].reset();
            m_pendingClipboardData[id].clear();
            m_dataClipboard[id].set(data);
            clearPendingFileClipboard(id);
            sendClipboardSelectionToServer(
                localFileClipboard.paths, localFileClipboard.sessionId,
                m_clipboardRevision);
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
            m_pendingImmutableClipboard[id].reset();
            m_pendingClipboardData[id].set(data);
            scheduleClipboardRetry(id);
            return;
        }
        m_sentClipboard[id] = true;
        m_clipboardSendPending[id] = false;
        m_pendingImmutableClipboard[id].reset();
        m_pendingClipboardData[id].clear();
        m_dataClipboard[id].set(data);
    }
}

bool
Client::finishPendingClipboardSend(
    ClipboardID id, const std::string& data,
    const std::shared_ptr<const String>* immutable)
{
    if (!m_clipboardSendPending[id]) {
        return true;
    }

    const bool pendingImmutable =
        static_cast<bool>(m_pendingImmutableClipboard[id]);
    const bool sameRevision = pendingImmutable
        ? immutable != NULL && *immutable &&
            immutable->get() == m_pendingImmutableClipboard[id].get()
        : m_pendingClipboardData[id].matches(data);
    if (!sameRevision) {
        if (!m_server->cleanupClipboardSendThread(true)) {
            scheduleClipboardRetry(id);
            return false;
        }
        m_clipboardSendPending[id] = false;
        m_pendingImmutableClipboard[id].reset();
        m_pendingClipboardData[id].clear();
        clearPendingFileClipboard(id);
        return true;
    }

    bool succeeded = false;
    if (!m_server->reapClipboardSendResult(id, succeeded)) {
        scheduleClipboardRetry(id);
        return false;
    }

    m_clipboardSendPending[id] = false;
    m_pendingImmutableClipboard[id].reset();
    if (!succeeded) {
        LOG((CLOG_WARN "clipboard %d async send failed; retrying", id));
        return true;
    }

    m_sentClipboard[id] = true;
    if (pendingImmutable) {
        m_dataClipboard[id].clear();
    }
    else {
        m_dataClipboard[id].set(data);
    }
    m_pendingClipboardData[id].clear();
    const std::vector<barrier::fs::path> filePaths =
        m_pendingFileClipboardPaths[id];
    const std::string fileSession = m_pendingFileClipboardSession[id];
    const barrier::ClipboardRevision fileRevision =
        m_pendingFileClipboardRevision[id];
    clearPendingFileClipboard(id);
    if (!filePaths.empty()) {
        sendClipboardSelectionToServer(
            filePaths, fileSession, fileRevision);
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
    if (m_protocolMinorVersion < 12) {
        LOG((CLOG_WARN
            "dropping file chunk for legacy protocol connection"));
        if (m_sendFileChunker) {
            m_sendFileChunker->interruptFile();
        }
        return;
    }
    const bool hasTransferGeneration =
        (chunk->m_transferId != 0 || m_sendFileTransferId != 0);
    if (hasTransferGeneration && chunk->m_transferId != m_sendFileTransferId) {
        LOG((CLOG_DEBUG "dropping stale file chunk, transfer=%u current=%u",
            chunk->m_transferId, m_sendFileTransferId));
        return;
    }

    if (m_protocolMinorVersion >= 12) {
        if (!chunk->m_transactional || !m_sendFileTransactionState ||
            chunk->m_transferId == 0 ||
            chunk->m_transferId != m_sendFileTransactionState->transferId() ||
            barrier::FileTransferProtocol::transferRole(chunk->m_transferId) !=
                barrier::FileTransferRole::kSecondary) {
            LOG((CLOG_WARN
                "rejecting invalid transactional file sender state, transfer=%u",
                chunk->m_transferId));
            if (m_sendFileTransactionState) {
                m_sendFileTransactionState->fail(
                    barrier::FileTransferReason::kProtocolError);
            }
            if (m_sendFileChunker) {
                m_sendFileChunker->interruptFile();
            }
            return;
        }

        barrier::FileTransferFrame frame;
        barrier::IStream* destination = NULL;
        const UInt8 mark = static_cast<UInt8>(chunk->m_chunk[0]);
        if (mark == kDataStart) {
            UInt32 expectedSize = 0;
            if (!parseTransactionalFileSize(*chunk, expectedSize)) {
                m_sendFileTransactionState->fail(
                    barrier::FileTransferReason::kProtocolError);
                return;
            }
            frame = barrier::FileTransferFrame::start(
                m_server->getConnectionBinding(), chunk->m_transferId,
                expectedSize, m_sendFileTransactionState->kind(),
                m_sendFileTransactionState->clipboardRevision(),
                m_sendFileTransactionState->clipboardSessionId());
            if (!m_server->sendTransactionalFileTransferFrame(frame)) {
                m_sendFileTransactionState->connectionLost();
                m_sendFileCompletionPending = true;
                if (m_sendFileChunker) {
                    m_sendFileChunker->interruptFile();
                }
                return;
            }
            m_sendFileStarted = true;
            m_sendFileCompletionPending = false;
            return;
        }
        if (mark == kDataCancel) {
            barrier::FileTransferReason reason = chunk->m_transferReason;
            if (reason == barrier::FileTransferReason::kNone) {
                reason = barrier::FileTransferReason::kCancelled;
            }
            frame = barrier::FileTransferFrame::cancel(
                m_server->getConnectionBinding(), chunk->m_transferId,
                reason);
            if (!m_server->sendTransactionalFileTransferFrame(frame)) {
                m_sendFileTransactionState->connectionLost();
                cleanupSendFileCancelAckTimeout();
                m_sendFileCancelAckPending = false;
            }
            m_sendFileCompletionPending = true;
            if (m_sendFileTransactionState->result() !=
                    barrier::FileTransferReason::kConnectionLost) {
                scheduleSendFileCancelAckTimeout();
            }
            finishCompletedSendFileIfReady();
            return;
        }

        std::shared_ptr<barrier::BulkChannel> currentBulk =
            acquireBulkChannel();
        if (!currentBulk || currentBulk != m_sendFileBulkChannel ||
            !m_sendFileBulkChannel->isActive()) {
            LOG((CLOG_WARN
                "transactional file route disappeared, transfer=%u",
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
        destination = m_sendFileBulkChannel->getStream();
        if (mark == kDataChunk) {
            frame = barrier::FileTransferFrame::data(
                m_server->getConnectionBinding(), chunk->m_transferId,
                chunk->m_offset,
                std::string(&chunk->m_chunk[1], chunk->m_dataSize));
        }
        else if (mark == kDataEnd) {
            frame = barrier::FileTransferFrame::end(
                m_server->getConnectionBinding(), chunk->m_transferId,
                chunk->m_offset,
                std::string(&chunk->m_chunk[1], chunk->m_dataSize));
        }
        else {
            m_sendFileTransactionState->fail(
                barrier::FileTransferReason::kProtocolError);
            return;
        }

        if (!barrier::FileTransferProtocol::encode(
                destination, frame,
                barrier::FileTransferRole::kSecondary)) {
            m_sendFileTransactionState->connectionLost();
            m_sendFileCompletionPending = true;
            cleanupSendFileCancelAckTimeout();
            m_sendFileCancelAckPending = false;
            if (m_sendFileChunker) {
                m_sendFileChunker->interruptFile();
            }
            return;
        }
        if (mark == kDataEnd) {
            m_sendFileCompletionPending = true;
            finishCompletedSendFileIfReady();
        }
        return;
    }

    // Keep the selected route for the entire transfer. A failed bulk stream
    // must not spill the remainder of a file frame sequence into control.
    if (m_sendFileBulkChannel) {
        if (!m_sendFileBulkChannel->isActive()) {
            if (m_sendFileChunker) {
                m_sendFileChunker->interruptFile();
            }
            if (chunk->m_chunk[0] == kDataEnd ||
                chunk->m_chunk[0] == kDataCancel) {
                m_sendFileCompletionPending = true;
                m_sendFileBulkChannel.reset();
            }
            return;
        }
        FileChunk::send(m_sendFileBulkChannel->getStream(), chunk->m_chunk[0],
                        &chunk->m_chunk[1], chunk->m_dataSize);
    }
    else {
        m_server->fileChunkSending(chunk->m_chunk[0], &chunk->m_chunk[1],
                                   chunk->m_dataSize);
    }
    if (chunk->m_chunk[0] == kDataStart) {
        m_sendFileStarted = true;
        m_sendFileCompletionPending = false;
    }
    else if (chunk->m_chunk[0] == kDataEnd || chunk->m_chunk[0] == kDataCancel) {
        m_sendFileCompletionPending = true;
        m_sendFileBulkChannel.reset();
    }
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
Client::cleanupBulkHandshake()
{
    if (m_bulkHandshakeStream == NULL) {
        m_bulkHandshakeState = kBulkIdle;
        m_bulkBindingToken.clear();
        return;
    }
    void* target = m_bulkHandshakeStream->getEventTarget();
    m_events->removeHandler(m_events->forIDataSocket().connected(), target);
    m_events->removeHandler(m_events->forIDataSocket().secureConnected(), target);
    m_events->removeHandler(m_events->forIDataSocket().connectionFailed(), target);
    m_events->removeHandler(m_events->forIStream().inputReady(), target);
    m_events->removeHandler(m_events->forIStream().outputError(), target);
    m_events->removeHandler(m_events->forIStream().inputShutdown(), target);
    m_events->removeHandler(m_events->forIStream().outputShutdown(), target);
    m_events->removeHandler(m_events->forIStream().inputFormatError(), target);
    m_bulkHandshakeStream->close();
    delete m_bulkHandshakeStream;
    m_bulkHandshakeStream = NULL;
    m_bulkHandshakeState = kBulkIdle;
    m_bulkBindingToken.clear();
}

bool
Client::abandonBulkHandshake()
{
    bool controlRouteHealthy = true;
    if (m_bulkHandshakeStream != NULL &&
        m_bulkHandshakeState == kBulkWaitingForAck &&
        m_server != NULL) {
        controlRouteHealthy = m_server->handleBulkHandshakeFailed();
    }
    cleanupBulkHandshake();
    return controlRouteHealthy;
}

void
Client::cleanupBulkConnection()
{
    cleanupBulkRetry();
    cleanupBulkHandshake();
    m_bulkActiveToken.clear();
    if (m_bulkChannel) {
        m_bulkChannel->close();
        m_bulkChannel.reset();
    }
}

void
Client::cleanupBulkRetry()
{
    if (m_bulkRetryTimer != NULL) {
        m_events->removeHandler(Event::kTimer, m_bulkRetryTimer);
        m_events->deleteTimer(m_bulkRetryTimer);
        m_bulkRetryTimer = NULL;
    }
    m_bulkRetryToken.clear();
    m_bulkRetryAttempt = 0;
}

void
Client::scheduleBulkRetry(const std::string& token)
{
    if (token.empty() || m_stream == NULL || m_server == NULL ||
        m_protocolMinorVersion < 9 ||
        (!m_bulkRetryToken.empty() && m_bulkRetryToken != token)) {
        return;
    }
    m_bulkRetryToken = token;
    if (m_bulkRetryTimer != NULL) {
        return;
    }

    const UInt32 exponent = m_bulkRetryAttempt < 4 ? m_bulkRetryAttempt : 4;
    double delay = kBulkRetryInitialSeconds * static_cast<double>(1u << exponent);
    if (delay > kBulkRetryMaximumSeconds) {
        delay = kBulkRetryMaximumSeconds;
    }
    ++m_bulkRetryAttempt;
    m_bulkRetryTimer = m_events->newOneShotTimer(delay, NULL);
    m_events->adoptHandler(Event::kTimer, m_bulkRetryTimer,
        new TMethodEventJob<Client>(this, &Client::handleBulkRetry));
}

void
Client::handleBulkRetry(const Event&, void*)
{
    if (m_bulkRetryTimer != NULL) {
        EventQueueTimer* timer = m_bulkRetryTimer;
        m_bulkRetryTimer = NULL;
        m_events->removeHandler(Event::kTimer, timer);
        m_events->deleteTimer(timer);
    }
    const std::string token = m_bulkRetryToken;
    (void)startBulkConnection(token);
}

void
Client::handleBulkConnected(const Event&, void*)
{
    if (m_bulkHandshakeStream == NULL) {
        return;
    }
    void* target = m_bulkHandshakeStream->getEventTarget();
    m_events->removeHandler(m_events->forIDataSocket().connected(), target);
    m_events->removeHandler(m_events->forIDataSocket().secureConnected(), target);
    m_events->removeHandler(m_events->forIDataSocket().connectionFailed(), target);
    m_events->adoptHandler(m_events->forIStream().inputReady(), target,
        new TMethodEventJob<Client>(this, &Client::handleBulkHandshakeData));
    m_events->adoptHandler(m_events->forIStream().outputError(), target,
        new TMethodEventJob<Client>(this, &Client::handleBulkHandshakeError));
    m_events->adoptHandler(m_events->forIStream().inputShutdown(), target,
        new TMethodEventJob<Client>(this, &Client::handleBulkHandshakeError));
    m_events->adoptHandler(m_events->forIStream().outputShutdown(), target,
        new TMethodEventJob<Client>(this, &Client::handleBulkHandshakeError));
    m_events->adoptHandler(m_events->forIStream().inputFormatError(), target,
        new TMethodEventJob<Client>(this, &Client::handleBulkHandshakeError));
}

void
Client::handleBulkConnectionFailed(const Event& event, void*)
{
    IDataSocket::ConnectionFailedInfo* info =
        static_cast<IDataSocket::ConnectionFailedInfo*>(event.getData());
    LOG((CLOG_WARN "bulk connection failed; retry scheduled: %s",
         info == NULL ? "unknown error" : info->m_what.c_str()));
    delete info;
    const std::string failedToken = m_bulkBindingToken;
    if (!abandonBulkHandshake()) {
        LOG((CLOG_ERR
            "failed to reject file start after bulk connection failure"));
        disconnect("failed to reject file start after bulk connection failure");
        return;
    }
    scheduleBulkRetry(failedToken);
}

void
Client::handleBulkHandshakeError(const Event&, void*)
{
    LOG((CLOG_WARN "bulk handshake failed; retry scheduled"));
    const std::string failedToken = m_bulkBindingToken;
    if (!abandonBulkHandshake()) {
        LOG((CLOG_ERR
            "failed to reject file start after bulk handshake failure"));
        disconnect("failed to reject file start after bulk handshake failure");
        return;
    }
    scheduleBulkRetry(failedToken);
}

void
Client::handleBulkHandshakeData(const Event&, void*)
{
    if (m_bulkHandshakeStream == NULL) {
        return;
    }

    if (m_bulkHandshakeState == kBulkWaitingForHello) {
        SInt16 major = 0;
        SInt16 minor = 0;
        if (!ProtocolUtil::readf(m_bulkHandshakeStream, kMsgHello,
                                 &major, &minor) ||
            major != kProtocolMajorVersion || minor < 9 ||
            (m_protocolMinorVersion >= 12 && minor < 12)) {
            handleBulkHandshakeError(Event(), NULL);
            return;
        }
        if (m_protocolMinorVersion >= 12) {
            if (!isValidConnectionBinding(m_controlConnectionBinding)) {
                handleBulkHandshakeError(Event(), NULL);
                return;
            }
            ProtocolUtil::writef(
                m_bulkHandshakeStream, kMsgHelloBulkBack1_12,
                kProtocolMajorVersion, m_protocolMinorVersion,
                &m_name, &m_bulkBindingToken, &m_controlConnectionBinding);
        }
        else {
            ProtocolUtil::writef(m_bulkHandshakeStream, kMsgHelloBulkBack,
                                 kProtocolMajorVersion, m_protocolMinorVersion,
                                 &m_name, &m_bulkBindingToken);
        }
        m_bulkHandshakeState = kBulkWaitingForAck;
        if (!m_bulkHandshakeStream->isReady()) {
            return;
        }
    }

    UInt8 code[4];
    if (m_bulkHandshakeStream->read(code, 4) != 4 ||
        memcmp(code, kMsgDBulkAccepted, 4) != 0) {
        handleBulkHandshakeError(Event(), NULL);
        return;
    }

    barrier::IStream* stream = m_bulkHandshakeStream;
    const std::string acceptedToken = m_bulkBindingToken;
    void* target = stream->getEventTarget();
    m_events->removeHandler(m_events->forIStream().inputReady(), target);
    m_events->removeHandler(m_events->forIStream().outputError(), target);
    m_events->removeHandler(m_events->forIStream().inputShutdown(), target);
    m_events->removeHandler(m_events->forIStream().outputShutdown(), target);
    m_events->removeHandler(m_events->forIStream().inputFormatError(), target);
    m_bulkHandshakeStream = NULL;
    m_bulkHandshakeState = kBulkIdle;
    m_bulkBindingToken.clear();
    m_bulkChannel = std::make_shared<barrier::BulkChannel>(stream, this, m_events);
    m_bulkActiveToken = acceptedToken;
    cleanupBulkRetry();
    LOG((CLOG_NOTE "separate bulk channel is ready"));
    if (m_server != NULL && !m_server->handleBulkChannelReady()) {
        disconnect("failed to resume file transfer after bulk handshake");
        return;
    }
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        if (m_ownClipboard[id] && !m_sentClipboard[id]) {
            m_clipboardRetryCount[id] = 0;
            scheduleClipboardRetry(id);
        }
    }
    startPendingManualFileSend();
    startPendingFileClipboardPrefetch();
    if (stream->isReady()) {
        m_events->addEvent(Event(m_events->forIStream().inputReady(), target));
    }
}

void
Client::setupScreen()
{
    reapDetachedConnectionState();
    assert(m_server == NULL);

    m_ready  = false;
    m_inputBackendPoisoned = false;
    m_server = new ServerProxy(this, m_stream, m_events,
                               m_protocolMinorVersion);
    m_events->adoptHandler(m_events->forIScreen().shapeChanged(),
                            getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleShapeChanged));
    m_events->adoptHandler(m_events->forClipboard().clipboardGrabbed(),
                            getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleClipboardGrabbed));
    m_events->adoptHandler(m_events->forClipboard().clipboardChanged(),
                            getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleClipboardChanged));
    m_events->adoptHandler(m_events->forClipboard().clipboardPublished(),
                            getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleClipboardPublished));
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
    m_controlConnectionBinding.clear();
    cleanupFileReceiveCompletionPoll();
    FileChunk::releaseReceiveBuffer(m_fileReceiveSession);
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        clearPendingFileClipboard(id);
    }
    m_pendingManualFileSend.clear();
    m_pendingFileClipboardPrefetchPaths.clear();
    m_pendingFileClipboardPrefetchSession.clear();
    m_pendingFileClipboardPrefetchRevision.reset();
    if (m_sendFileTransactionState) {
        m_sendFileTransactionState->connectionLost();
        m_sendFileCompletionPending = true;
        cleanupSendFileCancelAckTimeout();
        m_sendFileCancelAckPending = false;
    }
    if (m_protocolMinorVersion >= 12) {
        m_sendFileTransferId = 0;
    }
    else {
        ++m_sendFileTransferId;
        if (m_sendFileTransferId == 0) {
            ++m_sendFileTransferId;
        }
    }
    if (m_transactionalReceiveId != 0) {
        cancelTransactionalFileReceive(m_transactionalReceiveId);
    }
    const bool fileSenderStopped = cleanupSendFileThread(true);
    // A secondary transport is valid only while its authenticated control
    // connection is alive. Closing it does not destroy a route pinned by a
    // still-running sender; that shared owner is released after cancellation.
    cleanupBulkConnection();
    if (fileSenderStopped) {
        m_sendFileBulkChannel.reset();
    }
    m_sendFileStarted = false;
    m_sendFileCompletionPending = false;
    resetSendFileDrainPoll();

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
    // Revoke the logical pointer lease before any cleanup can defer or return.
    // Screen::disable() releases the platform state for a ready connection.
    if (m_server != NULL) {
        m_server->revokeInputLease();
    }
    m_active = false;
    releaseDetachedServerProxies();
    cleanupClipboardRetryTimer();
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        m_clipboardRetryPending[id] = false;
        m_clipboardRetryCount[id] = 0;
        m_clipboardSendPending[id] = false;
        m_pendingImmutableClipboard[id].reset();
        m_pendingClipboardData[id].clear();
        clearPendingFileClipboard(id);
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
        m_events->removeHandler(m_events->forClipboard().clipboardChanged(),
                            getEventTarget());
        m_events->removeHandler(m_events->forClipboard().clipboardPublished(),
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
        m_pendingImmutableClipboard[id].reset();
        m_pendingClipboardData[id].clear();
        clearPendingFileClipboard(id);
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
    if (info->m_id == kClipboardClipboard) {
        supersedeFileClipboard("local clipboard grab");
    }

    // grab ownership
    m_server->onGrabClipboard(info->m_id);

    // we now own the clipboard and it has not been sent to the server
    m_ownClipboard[info->m_id]  = true;
    m_sentClipboard[info->m_id] = false;
    m_clipboardSendPending[info->m_id] = false;
    m_pendingImmutableClipboard[info->m_id].reset();
    m_pendingClipboardData[info->m_id].clear();
    m_timeClipboard[info->m_id] = 0;
    m_clipboardRetryPending[info->m_id] = false;
    m_clipboardRetryCount[info->m_id] = 0;

    IPlatformScreen* platform = m_screen->getPlatformScreen();
    if (!m_active && platform != NULL &&
        !platform->hasAsyncClipboardSnapshots()) {
        scheduleClipboardRetry(info->m_id);
    }
}

void
Client::handleClipboardChanged(const Event& event, void*)
{
    if (!m_enableClipboard || m_active) {
        return;
    }

    const IScreen::ClipboardInfo* info =
        static_cast<const IScreen::ClipboardInfo*>(event.getData());
    if (info == NULL || info->m_id >= kClipboardEnd ||
        !m_ownClipboard[info->m_id]) {
        return;
    }

    // Snapshot completion is stronger evidence than the retry timer. Reset a
    // previously exhausted budget and let the existing deferred send path read
    // only the platform's committed cache.
    m_clipboardRetryCount[info->m_id] = 0;
    scheduleClipboardRetry(info->m_id);
}

void
Client::handleClipboardPublished(const Event& event, void*)
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
        if (!m_ownClipboard[id]) {
            m_clipboardRetryPending[id] = false;
            m_clipboardRetryCount[id] = 0;
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
    if (!negotiateProtocolVersion(major, minor, m_protocolMinorVersion)) {
        sendConnectionFailedEvent(XIncompatibleClient(major, minor).what());
        cleanupTimer();
        cleanupConnection();
        return;
    }

    // say hello back
    LOG((CLOG_DEBUG1 "say hello version %d.%d", kProtocolMajorVersion,
        m_protocolMinorVersion));
    ProtocolUtil::writef(m_stream, kMsgHelloBack,
                            kProtocolMajorVersion,
                            m_protocolMinorVersion, &m_name);

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
    serviceSendFileCompletion();
    if (m_protocolMinorVersion < 12 && m_sendFileThread != NULL) {
        scheduleSendFileReap();
    }
    m_server->keepAlive();
}

void
Client::serviceSendFileCompletion()
{
    const bool reaped = reapSendFileThreadIfReady();
    const bool finished = finishCompletedSendFileIfReady();
    if (!finished && completedSendFileMayRetire()) {
        scheduleSendFileReap();
    }
    else if (reaped) {
        cleanupSendFileReap();
    }
    startPendingManualFileSend();
    startPendingFileClipboardPrefetch();
}

bool
Client::completedSendFileMayRetire() const
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
        (!m_sendFileStartAcknowledged &&
         result != barrier::FileTransferReason::kNone &&
         result != barrier::FileTransferReason::kTimeout &&
         result != barrier::FileTransferReason::kCancelled);
}

void
Client::scheduleSendFileReap()
{
    if (m_sendFileTransferId == 0 ||
        (m_sendFileThread == NULL && !completedSendFileMayRetire())) {
        return;
    }
    if (m_sendFileReapTimer != NULL &&
        m_sendFileReapTransferId == m_sendFileTransferId) {
        return;
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
        if (m_sendFileCompletionPending && m_sendFileBulkChannel &&
            m_sendFileBulkChannel->isActive()) {
            std::shared_ptr<barrier::BulkChannel> failedChannel =
                m_sendFileBulkChannel;
            failedChannel->close();
            handleBulkDisconnected(failedChannel.get());
            finishCompletedSendFileIfReady();
        }
        return;
    }
    m_events->adoptHandler(Event::kTimer, m_sendFileReapTimer,
        new TMethodEventJob<Client>(this, &Client::handleSendFileReap));
}

void
Client::cleanupSendFileReap()
{
    if (m_sendFileReapTimer != NULL) {
        m_events->removeHandler(Event::kTimer, m_sendFileReapTimer);
        m_events->deleteTimer(m_sendFileReapTimer);
        m_sendFileReapTimer = NULL;
    }
    m_sendFileReapTransferId = 0;
}

void
Client::resetSendFileDrainPoll()
{
    m_sendFileDrainPollCount = 0;
    m_sendFileDrainStallCount = 0;
    m_sendFileLastBufferedOutput = 0;
}

void
Client::handleSendFileReap(const Event&, void*)
{
    const UInt32 transferId = m_sendFileReapTransferId;
    cleanupSendFileReap();
    if (transferId == 0 || transferId != m_sendFileTransferId) {
        return;
    }
    serviceSendFileCompletion();
}

void
Client::scheduleSendFileCancelAckTimeout()
{
    cleanupSendFileCancelAckTimeout();
    if (m_sendFileTransactionState == NULL) {
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
        new TMethodEventJob<Client>(
            this, &Client::handleSendFileCancelAckTimeout));
}

void
Client::cleanupSendFileCancelAckTimeout()
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
Client::handleSendFileCancelAckTimeout(const Event&, void*)
{
    const UInt32 expiredTransferId = m_sendFileCancelAckTimeoutTransferId;
    cleanupSendFileCancelAckTimeout();
    if (!m_sendFileCancelAckPending ||
        m_sendFileTransactionState == NULL ||
        m_sendFileTransactionState->transferId() != expiredTransferId) {
        return;
    }

    LOG((CLOG_WARN
        "transactional file CancelAck timed out, transfer=%u",
        expiredTransferId));
    m_sendFileCancelAckPending = false;
    serviceSendFileCompletion();
}

void
Client::handleFileRecieveCompleted(const Event& event, void*)
{
    FileReceiveCompletionInfo* info =
        static_cast<FileReceiveCompletionInfo*>(event.getDataObject());
    const std::uint64_t generation = info == NULL ?
        m_fileReceiveSession.generation() : info->m_generation;
    onFileRecieveCompleted(generation);
}

void
Client::handleFileReceiveCompletionPoll(const Event&, void*)
{
    const std::uint64_t generation = m_fileReceiveCompletionGeneration;
    cleanupFileReceiveCompletionPoll();
    onFileRecieveCompleted(generation);
}

void
Client::scheduleFileReceiveCompletionPoll(std::uint64_t generation)
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
            new TMethodEventJob<Client>(this,
                &Client::handleFileReceiveCompletionPoll));
    }
}

void
Client::cleanupFileReceiveCompletionPoll()
{
    if (m_fileReceiveCompletionTimer != NULL) {
        m_events->removeHandler(Event::kTimer, m_fileReceiveCompletionTimer);
        m_events->deleteTimer(m_fileReceiveCompletionTimer);
        m_fileReceiveCompletionTimer = NULL;
    }
    m_fileReceiveCompletionGeneration = 0;
}

void
Client::onFileRecieveCompleted(std::uint64_t generation)
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

void
Client::handleFileClipboardReady(const Event& event, void*)
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
Client::handleDropDirWriteFinished(const Event&, void*)
{
    drainDropDirTransferQueue();
}

void
Client::handleStopRetry(const Event&, void*)
{
    m_args.m_restartable = false;
}

barrier::FileTransferReason
Client::startDropDirTransfer(std::shared_ptr<CompletedFileTransfer> transfer)
{
    if (!transfer) {
        return barrier::FileTransferReason::kIoError;
    }

    if (!reapWriteToDropDirThreadIfReady()) {
        return queueDropDirTransfer(transfer) ?
            barrier::FileTransferReason::kNone :
            barrier::FileTransferReason::kBusy;
    }

    try {
        m_writeToDropDirThread = new Thread([this, transfer]() {
            write_to_drop_dir_thread(transfer);
            m_events->addEvent(
                Event(m_events->forFile().dropDirWriteFinished(), this));
        });
        return barrier::FileTransferReason::kNone;
    }
    catch (...) {
        FileChunk::releaseReceiveBuffer(
            transfer->data, transfer->expectedSize, &transfer->spoolPath);
        return barrier::FileTransferReason::kIoError;
    }
}

bool
Client::queueDropDirTransfer(std::shared_ptr<CompletedFileTransfer> transfer)
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
	if (transfer->kind == barrier::FileTransferKind::kClipboard &&
	    !transfer->remoteFileClipboardSession.empty() &&
	    (hasSpooledReceive
	        ? TransferArchive::isPackageFile(transfer->spoolPath)
	        : TransferArchive::isPackageData(transfer->data))) {
	    LOG((CLOG_INFO "remote clipboard package received: session=%s", transfer->remoteFileClipboardSession.c_str()));
	    const barrier::fs::path cacheRoot =
	        barrier::DataDirectories::profile() / "clipboard-cache" / "client";
	    std::ostringstream clipboardOrigin;
	    clipboardOrigin << m_serverAddress.getHostname() << ':'
	                    << m_serverAddress.getPort();
	    const barrier::fs::path spoolDir =
	        RemoteFileClipboard::materializedSessionRoot(
	            cacheRoot, clipboardOrigin.str(),
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
Client::publishMaterializedFileClipboard(const std::vector<std::string>& paths,
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
    if (m_screen->hasAsyncClipboardPublications()) {
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

    if (!m_screen->setClipboardChecked(kClipboardClipboard, &clipboard)) {
        LOG((CLOG_ERR
            "failed to publish remote file clipboard: session=%s",
            sessionId.c_str()));
        return;
    }
    commitMaterializedFileClipboard(data, sessionId, paths.size(), 0);
}

void
Client::commitMaterializedFileClipboard(
    const std::shared_ptr<const String>& data, const std::string& sessionId,
    std::size_t pathCount, std::uint64_t publicationId)
{
    if (!data || !Clipboard::isValidMarshalled(*data)) {
        clearMaterializedClipboardPublication();
        return;
    }

    m_sentClipboard[kClipboardClipboard] = true;
    m_clipboardSendPending[kClipboardClipboard] = false;
    m_pendingImmutableClipboard[kClipboardClipboard].reset();
    m_pendingClipboardData[kClipboardClipboard].clear();
    m_dataClipboard[kClipboardClipboard].set(*data);
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
    }
    clearMaterializedClipboardPublication();
}

void
Client::clearMaterializedClipboardPublication()
{
    m_pendingMaterializedClipboardPublicationId = 0;
    m_pendingMaterializedClipboardData.reset();
    m_pendingMaterializedClipboardSession.clear();
    m_pendingMaterializedClipboardRevision.reset();
}

std::uint64_t
Client::allocateClipboardPublicationId()
{
    ++m_nextClipboardPublicationId;
    if (m_nextClipboardPublicationId == 0) {
        ++m_nextClipboardPublicationId;
    }
    return m_nextClipboardPublicationId;
}

void
Client::sendClipboardSelectionToServer(
    const std::vector<barrier::fs::path>& sourcePaths,
    const std::string& sessionId,
    const barrier::ClipboardRevision& revision)
{
    if (m_protocolMinorVersion < 12) {
        LOG((CLOG_WARN
            "not starting file clipboard prefetch for legacy protocol peer"));
        m_pendingFileClipboardPrefetchPaths.clear();
        m_pendingFileClipboardPrefetchSession.clear();
        m_pendingFileClipboardPrefetchRevision.reset();
        return;
    }
    if (sourcePaths.empty()) {
        return;
    }
    if (!revision.valid() || revision != m_clipboardRevision ||
        !isValidClipboardSessionId(sessionId)) {
        LOG((CLOG_WARN
            "discarding stale or invalid clipboard prefetch identity: session=%s revision=%llu current=%llu",
            sessionId.c_str(),
            static_cast<unsigned long long>(revision.sequence()),
            static_cast<unsigned long long>(m_clipboardRevision.sequence())));
        return;
    }

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
Client::startPendingFileClipboardPrefetch()
{
    if (m_protocolMinorVersion < 12) {
        m_pendingFileClipboardPrefetchPaths.clear();
        m_pendingFileClipboardPrefetchSession.clear();
        m_pendingFileClipboardPrefetchRevision.reset();
        return;
    }
    if (m_pendingFileClipboardPrefetchPaths.empty() ||
        m_stream == NULL || m_server == NULL ||
        !reapSendFileThreadIfReady()) {
        return;
    }
    if (!m_pendingFileClipboardPrefetchRevision.valid() ||
        m_pendingFileClipboardPrefetchRevision != m_clipboardRevision ||
        !isValidClipboardSessionId(
            m_pendingFileClipboardPrefetchSession)) {
        LOG((CLOG_WARN
            "discarding clipboard prefetch superseded before sender start"));
        m_pendingFileClipboardPrefetchPaths.clear();
        m_pendingFileClipboardPrefetchSession.clear();
        m_pendingFileClipboardPrefetchRevision.reset();
        return;
    }
    finishCompletedSendFileIfReady();
    if (m_sendFileTransactionState ||
        (m_sendFileStarted && !m_sendFileCompletionPending)) {
        LOG((CLOG_DEBUG "remote clipboard prefetch waiting for the active transfer terminator"));
        return;
    }

    std::shared_ptr<barrier::BulkChannel> bulkChannel = acquireBulkChannel();
    if (m_protocolMinorVersion >= 9 && !bulkChannel) {
        LOG((CLOG_DEBUG
            "remote clipboard prefetch deferred until the required bulk channel is ready"));
        return;
    }
    if (m_protocolMinorVersion >= 12 &&
        !transactionalFileTransferReady()) {
        LOG((CLOG_DEBUG
            "remote clipboard prefetch deferred until the bound control route is ready"));
        return;
    }

    std::vector<barrier::fs::path> sourcePaths;
    sourcePaths.swap(m_pendingFileClipboardPrefetchPaths);
    std::string clipboardSession;
    clipboardSession.swap(m_pendingFileClipboardPrefetchSession);
    const barrier::ClipboardRevision clipboardRevision =
        m_pendingFileClipboardPrefetchRevision;
    m_pendingFileClipboardPrefetchRevision.reset();

    auto chunker = std::make_shared<StreamChunker>();
    m_sendFileChunker = chunker;
    m_sendFileIsClipboardPrefetch = true;
    m_sendFileStarted = false;
    m_sendFileStartAcknowledged = false;
    m_sendFileCompletionPending = false;
    resetSendFileDrainPoll();
    cleanupSendFileCancelAckTimeout();
    cleanupSendFileReap();
    m_sendFileCancelAckPending = false;
    m_sendFileBulkChannel = bulkChannel;
    barrier::IStream* stream = m_sendFileBulkChannel ?
        m_sendFileBulkChannel->getStream() : m_stream;
    const UInt32 transferId = allocateSendFileTransferId();
    std::shared_ptr<barrier::FileTransferSendState> transactionState;
    if (m_protocolMinorVersion >= 12) {
        transactionState.reset(
            new barrier::FileTransferSendState(
                transferId, barrier::FileTransferKind::kClipboard,
                clipboardRevision.sequence(), clipboardSession));
    }
    m_sendFileTransactionState = transactionState;
    LOG((CLOG_INFO "remote clipboard prefetch started: direction=client-to-server items=%lu",
         static_cast<unsigned long>(sourcePaths.size())));
    m_sendFileThread = new Thread([
        this, sourcePaths, stream, chunker, transferId, transactionState]() {
        send_clipboard_file_thread(
            sourcePaths, stream, chunker, transferId, transactionState);
    });
}

void
Client::send_clipboard_file_thread(const std::vector<barrier::fs::path>& sourcePaths,
                                   barrier::IStream* stream,
                                   const std::shared_ptr<StreamChunker>& chunker,
                                   UInt32 transferId,
                                   const std::shared_ptr<barrier::FileTransferSendState>&
                                       transactionState)
{
    barrier::fs::path packagePath;
    try {
        Thread::testCancel();
#if defined(_WIN32)
        SessionUserImpersonation impersonation;
        if (!impersonation.ready()) {
            std::ostringstream message;
            message << "could not use the active session user identity to package "
                       "clipboard files: failure=" << impersonation.failureName()
                    << " session=" << impersonation.sessionId()
                    << " error=" << impersonation.error();
            throw std::runtime_error(message.str());
        }
        if (impersonation.required()) {
            LOG((CLOG_DEBUG "packaging remote clipboard files as the active session user: "
                 "session=%lu", impersonation.sessionId()));
        }
#endif
        RemoteFileClipboard::Data payload;
        payload.mode = RemoteFileClipboard::Mode::SourcePaths;
        payload.paths = sourcePaths;

        std::string error;
        if (!RemoteFileClipboard::createPackage(payload, packagePath, error)) {
            throw std::runtime_error(error);
        }
#if defined(_WIN32)
        if (!impersonation.finish()) {
            std::ostringstream message;
            message << "could not restore the process identity after packaging "
                       "clipboard files: failure=" << impersonation.failureName()
                    << " error=" << impersonation.error();
            throw std::runtime_error(message.str());
        }
        if (impersonation.usedRevertFallback()) {
            LOG((CLOG_WARN "RevertToSelf failed after packaging clipboard files; "
                 "the fallback cleared the thread token: error=%lu",
                 impersonation.revertError()));
        }
#endif

        Thread::testCancel();
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

    m_events->addEvent(Event(m_events->forFile().keepAlive(), this));

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
    const barrier::fs::path spoolPath = m_fileReceiveSession.spoolPath();
    if (!spoolPath.empty()) {
        return barrier::fs::exists(spoolPath) &&
            static_cast<std::size_t>(barrier::fs::file_size(spoolPath)) ==
                m_fileReceiveSession.expectedSize();
    }
    return m_fileReceiveSession.expectedSize() == m_fileReceiveSession.data().size();
}

void
Client::sendFileToServer(const std::string& filename)
{
    if (filename.empty()) {
        LOG((CLOG_WARN "file send rejected because the source path is empty"));
        return;
    }
    if (m_protocolMinorVersion < 12) {
        LOG((CLOG_WARN
            "file send rejected because the peer lacks transactional transfer support"));
        return;
    }
    if (!m_pendingManualFileSend.empty()) {
        LOG((CLOG_WARN
            "file send rejected because another manual request is already queued"));
        return;
    }

    if (!reapSendFileThreadIfReady()) {
        LOG((CLOG_WARN
            "file send rejected because another transfer is still active"));
        return;
    }
    finishCompletedSendFileIfReady();
    if (m_sendFileTransactionState ||
        (m_sendFileStarted && !m_sendFileCompletionPending)) {
        LOG((CLOG_WARN
            "file send rejected because the previous transfer has no completed wire boundary"));
        return;
    }

    std::shared_ptr<barrier::BulkChannel> bulkChannel = acquireBulkChannel();
    if (m_protocolMinorVersion >= 9 && !bulkChannel) {
        m_pendingManualFileSend = filename;
        LOG((CLOG_NOTE
            "file send queued until the required bulk channel is ready"));
        return;
    }
    if (m_protocolMinorVersion >= 12 &&
        !transactionalFileTransferReady()) {
        m_pendingManualFileSend = filename;
        LOG((CLOG_NOTE
            "file send queued until the bound control route is ready"));
        return;
    }

    startManualFileSend(filename, bulkChannel);
}

UInt32
Client::allocateSendFileTransferId()
{
    if (m_protocolMinorVersion < 12) {
        ++m_sendFileTransferId;
        if (m_sendFileTransferId == 0) {
            ++m_sendFileTransferId;
        }
        return m_sendFileTransferId;
    }

    m_nextSendFileTransferSequence =
        (m_nextSendFileTransferSequence + 1) &
        barrier::FileTransferProtocol::kTransferSequenceMask;
    if (m_nextSendFileTransferSequence == 0) {
        m_nextSendFileTransferSequence = 1;
    }
    m_sendFileTransferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary,
        m_nextSendFileTransferSequence);
    return m_sendFileTransferId;
}

bool
Client::transactionalFileTransferReady() const
{
    return m_protocolMinorVersion < 12 ||
        (m_server != NULL &&
         isValidConnectionBinding(m_server->getConnectionBinding()) &&
         m_bulkChannel && m_bulkChannel->isActive());
}

void
Client::startPendingManualFileSend()
{
    if (m_protocolMinorVersion < 12) {
        m_pendingManualFileSend.clear();
        return;
    }
    if (m_pendingManualFileSend.empty() || m_stream == NULL || m_server == NULL ||
        !reapSendFileThreadIfReady()) {
        return;
    }
    finishCompletedSendFileIfReady();
    if (m_sendFileTransactionState ||
        (m_sendFileStarted && !m_sendFileCompletionPending)) {
        return;
    }

    std::shared_ptr<barrier::BulkChannel> bulkChannel = acquireBulkChannel();
    if (m_protocolMinorVersion >= 9 && !bulkChannel) {
        return;
    }
    if (m_protocolMinorVersion >= 12 &&
        !transactionalFileTransferReady()) {
        return;
    }

    std::string filename;
    filename.swap(m_pendingManualFileSend);
    LOG((CLOG_NOTE "starting queued manual file send"));
    startManualFileSend(filename, bulkChannel);
}

void
Client::startManualFileSend(
    const std::string& filename,
    const std::shared_ptr<barrier::BulkChannel>& bulkChannel)
{
    if (m_protocolMinorVersion < 12) {
        LOG((CLOG_WARN
            "not starting manual file send for legacy protocol peer"));
        return;
    }
    auto chunker = std::make_shared<StreamChunker>();
    m_sendFileChunker = chunker;
    m_sendFileIsClipboardPrefetch = false;
    m_sendFileStarted = false;
    m_sendFileStartAcknowledged = false;
    m_sendFileCompletionPending = false;
    resetSendFileDrainPoll();
    cleanupSendFileCancelAckTimeout();
    cleanupSendFileReap();
    m_sendFileCancelAckPending = false;
    m_sendFileBulkChannel = bulkChannel;
    barrier::IStream* stream = m_sendFileBulkChannel ?
        m_sendFileBulkChannel->getStream() : m_stream;
    const UInt32 transferId = allocateSendFileTransferId();
    std::shared_ptr<barrier::FileTransferSendState> transactionState;
    if (m_protocolMinorVersion >= 12) {
        transactionState.reset(
            new barrier::FileTransferSendState(
                transferId, barrier::FileTransferKind::kManual, 0,
                std::string()));
    }
    m_sendFileTransactionState = transactionState;
    m_sendFileThread = new Thread([
        this, stream, filename, chunker, transferId, transactionState]() {
        send_file_thread(
            stream, filename, chunker, transferId, transactionState);
    });
}

void Client::send_file_thread(barrier::IStream* stream,
                              const std::string& filename,
                              const std::shared_ptr<StreamChunker>& chunker,
                              UInt32 transferId,
                              const std::shared_ptr<barrier::FileTransferSendState>&
                                  transactionState)
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
        chunker->sendFile(transferPath.u8string().c_str(), m_events, this,
                          stream, transferId, transactionState);
    }
    catch (XThread&) {
        if (!tempPackagePath.empty()) {
            barrier::fs::remove(tempPackagePath);
        }
        throw;
    }
    catch (std::runtime_error& error) {
        LOG((CLOG_ERR "failed sending file chunks: %s", error.what()));
        if (transactionState) {
            transactionState->fail(barrier::FileTransferReason::kIoError);
        }
    }

    if (!tempPackagePath.empty()) {
        barrier::fs::remove(tempPackagePath);
    }
    m_events->addEvent(Event(m_events->forFile().keepAlive(), this));
}

bool
Client::cleanupSendFileThread(bool cancel)
{
    if (cancel && m_sendFileChunker) {
        m_sendFileChunker->interruptFile();
    }
    if (cancel && m_sendFileTransactionState) {
        m_sendFileTransactionState->interrupt();
    }

    if (m_sendFileThread != NULL) {
        if (!m_sendFileThread->wait(0.0)) {
            if (cancel) {
                LOG((CLOG_DEBUG "requesting asynchronous file sender cancellation"));
                if (m_protocolMinorVersion < 12 ||
                    !m_sendFileTransactionState ||
                    m_sendFileTransactionState->result() ==
                        barrier::FileTransferReason::kConnectionLost) {
                    m_sendFileThread->cancel();
                }
                m_sendFileThread->unblockPollSocket();
            }
            return false;
        }
        delete m_sendFileThread;
        m_sendFileThread = NULL;
    }

    m_sendFileChunker.reset();
    if (m_protocolMinorVersion >= 12 && m_sendFileTransactionState) {
        if (completedSendFileMayRetire()) {
            m_sendFileCompletionPending = true;
        }
        return finishCompletedSendFileIfReady();
    }
    m_sendFileTransactionState.reset();
    m_sendFileBulkChannel.reset();
    m_sendFileIsClipboardPrefetch = false;
    m_sendFileStarted = false;
    m_sendFileStartAcknowledged = false;
    m_sendFileCompletionPending = false;
    cleanupSendFileCancelAckTimeout();
    cleanupSendFileReap();
    m_sendFileCancelAckPending = false;
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
    if (m_protocolMinorVersion >= 12 && m_sendFileTransactionState) {
        if (completedSendFileMayRetire()) {
            m_sendFileCompletionPending = true;
        }
        finishCompletedSendFileIfReady();
        return true;
    }
    m_sendFileTransactionState.reset();
    m_sendFileBulkChannel.reset();
    m_sendFileIsClipboardPrefetch = false;
    m_sendFileStarted = false;
    m_sendFileStartAcknowledged = false;
    m_sendFileCompletionPending = false;
    cleanupSendFileCancelAckTimeout();
    cleanupSendFileReap();
    m_sendFileCancelAckPending = false;
    releaseDetachedSendFileStream();
    return true;
}

bool
Client::finishCompletedSendFileIfReady()
{
    if (!m_sendFileCompletionPending || m_sendFileThread != NULL) {
        return false;
    }
    if (m_sendFileCancelAckPending && m_sendFileTransactionState &&
        m_sendFileTransactionState->result() !=
            barrier::FileTransferReason::kConnectionLost) {
        return false;
    }
    if (m_sendFileBulkChannel && m_sendFileBulkChannel->isActive()) {
        const UInt32 buffered =
            m_sendFileBulkChannel->getStream()->getBufferedOutputSize();
        if (buffered > 0) {
            ++m_sendFileDrainPollCount;
            if (m_sendFileLastBufferedOutput == 0 ||
                buffered < m_sendFileLastBufferedOutput) {
                m_sendFileDrainStallCount = 0;
            }
            else {
                ++m_sendFileDrainStallCount;
            }
            m_sendFileLastBufferedOutput = buffered;

            if (m_sendFileDrainPollCount < kFileSenderDrainMaxPolls &&
                m_sendFileDrainStallCount <
                    kFileSenderDrainMaxStalledPolls) {
                return false;
            }

            LOG((CLOG_WARN
                "closing stalled bulk route after file cancellation, transfer=%u buffered=%u polls=%u stalled=%u",
                m_sendFileTransferId, buffered, m_sendFileDrainPollCount,
                m_sendFileDrainStallCount));
            std::shared_ptr<barrier::BulkChannel> failedChannel =
                m_sendFileBulkChannel;
            failedChannel->close();
            handleBulkDisconnected(failedChannel.get());
        }
    }

    m_sendFileChunker.reset();
    m_sendFileTransactionState.reset();
    m_sendFileBulkChannel.reset();
    m_sendFileIsClipboardPrefetch = false;
    m_sendFileStarted = false;
    m_sendFileStartAcknowledged = false;
    m_sendFileCompletionPending = false;
    resetSendFileDrainPoll();
    cleanupSendFileCancelAckTimeout();
    cleanupSendFileReap();
    m_sendFileCancelAckPending = false;
    releaseDetachedServerProxies();
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
	const std::uint64_t receiveGeneration = m_fileReceiveSession.generation();
	if (m_fileReceiveClipboardGeneration == receiveGeneration) {
		transfer->remoteFileClipboardSession =
			m_fileReceiveRemoteFileClipboardSession;
		transfer->clipboardRevision = m_fileReceiveClipboardRevision;
		if (!transfer->remoteFileClipboardSession.empty()) {
			transfer->kind = barrier::FileTransferKind::kClipboard;
		}
	}
	m_fileReceiveSession.takeCompleted(
		transfer->data, transfer->expectedSize, transfer->spoolPath);
	if (m_screen != NULL && m_screen->getPlatformScreen() != NULL) {
		transfer->dropTarget = m_screen->getDropTarget();
	}
	transfer->dragFileList.swap(m_dragFileList);
	m_fileReceiveClipboardGeneration = 0;
	m_fileReceiveRemoteFileClipboardSession.clear();
	m_fileReceiveClipboardRevision.reset();
	return transfer;
}

void
Client::sendDragInfo(UInt32 fileCount, std::string& info, size_t size)
{
    if (m_protocolMinorVersion < 12) {
        LOG((CLOG_WARN
            "not sending drag metadata to legacy protocol peer"));
        return;
    }
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
