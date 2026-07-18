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

#include "barrier/IClient.h"

#include "barrier/Clipboard.h"
#include "barrier/ClipboardRevision.h"
#include "barrier/DragInformation.h"
#include "barrier/FileReceiveSession.h"
#include "barrier/FileTransferProtocol.h"
#include "base/Event.h"
#include "barrier/INode.h"
#include "barrier/ClientArgs.h"
#include "barrier/BulkChannel.h"
#include "net/NetworkAddress.h"
#include "base/EventTypes.h"
#include "mt/CondVar.h"
#include "io/filesystem.h"

#include <deque>
#include <memory>
#include <vector>

class EventQueueTimer;
namespace barrier {
class CompletedFilePayload;
class FileTransferSendState;
class Screen;
}
class ServerProxy;
class IDataSocket;
class ISocketFactory;
namespace barrier { class IStream; }
class IEventQueue;
class Thread;
class TCPSocket;
class StreamChunker;

//! Barrier client
/*!
This class implements the top-level client algorithms for barrier.
*/
class Client : public IClient, public INode,
               public barrier::IBulkChannelHandler {
public:
	class FileClipboardReadyInfo : public EventData {
	public:
		FileClipboardReadyInfo() : m_publishClipboard(false) { }

		std::string m_sessionId;
		std::vector<std::string> m_paths;
		barrier::ClipboardRevision m_revision;
		bool m_publishClipboard;
	};

    class FailInfo {
    public:
        FailInfo(const char* what) : m_retry(false), m_what(what) { }
        bool            m_retry;
        std::string m_what;
    };

public:
    /*!
    This client will attempt to connect to the server using \p name
    as its name and \p address as the server's address and \p factory
    to create the socket.  \p screen is    the local screen.
    */
    Client(IEventQueue* events, const std::string& name,
           const NetworkAddress& address, ISocketFactory* socketFactory,
           barrier::Screen* screen, ClientArgs const& args);

    ~Client();

    //! @name manipulators
    //@{

    //! Connect to server
    /*!
    Starts an attempt to connect to the server.  This is ignored if
    the client is trying to connect or is already connected.
    */
    void                connect();

    //! Disconnect
    /*!
    Disconnects from the server with an optional error message.
    */
    void                disconnect(const char* msg);

    void                connectBulkChannel(const std::string& token);
    bool                connectBulkChannel(const std::string& token,
                                           const std::string& connectionBinding);
    std::shared_ptr<barrier::BulkChannel> acquireBulkChannel() const;

    bool                handleBulkMessage(const UInt8* code,
                                          barrier::IStream* stream) override;
    void                handleBulkInputPauseFailed(
                            barrier::BulkChannel* channel,
                            std::uint64_t generation);
    void                handleBulkDisconnected(
                            barrier::BulkChannel* channel,
                            std::uint64_t pausedGeneration = 0) override;
    void                handleClipboardSendRouteFailure(ClipboardID id);

    //! Notify of handshake complete
    /*!
    Notifies the client that the connection handshake has completed.
    */
    virtual void        handshakeComplete();

    //! Received drag information
    void dragInfoReceived(UInt32 fileNum, std::string data);

    //! Create a new thread and use it to send file to Server
    void                sendFileToServer(const std::string& filename);

    //! Send dragging file information back to server
    void sendDragInfo(UInt32 fileCount, std::string& info, size_t size);


    //@}
    //! @name accessors
    //@{

    //! Test if connected
    /*!
    Returns true iff the client is successfully connected to the server.
    */
    bool                isConnected() const;

    //! Test if connecting
    /*!
    Returns true iff the client is currently attempting to connect to
    the server.
    */
    bool                isConnecting() const;

    //! Return true when the enabled local screen can accept a new input lease.
    bool                canAcceptInputHandoff() const;

    //! Commit a prepared input lease only after the platform accepts entry.
    bool                enterInputLease(SInt32 xAbs, SInt32 yAbs,
                            UInt32 seqNum, KeyModifierMask mask,
                            bool forScreensaver);

    //! Return the input backend generation used to validate a prepared lease.
    std::uint64_t       inputHandoffGeneration() const;

    static bool         negotiateProtocolVersion(SInt16 serverMajor,
                                SInt16 serverMinor, SInt16& negotiatedMinor);

    //! Get address of server
    /*!
    Returns the address of the server the client is connected (or wants
    to connect) to.
    */
    NetworkAddress        getServerAddress() const;

    //! Return true if received file size is valid
    bool                isReceivedFileSizeValid();

    FileReceiveSession& getFileReceiveSession() { return m_fileReceiveSession; }
    void                bindFileReceiveClipboardRevision();
    barrier::FileTransferReason beginTransactionalFileReceive(
                            const barrier::FileTransferFrame& startFrame);
    void                cancelTransactionalFileReceive(UInt32 transferId);
    barrier::FileTransferReason acceptTransactionalFileReceive(
                            UInt32 transferId,
                            barrier::CompletedFilePayload&& payload);
    bool                signalTransactionalFileTransferAck(
                            const barrier::FileTransferFrame& frame);

    //! Return drag file list
    DragFileList        getDragFileList() { return m_dragFileList; }

    //@}

    // IScreen overrides
    virtual void*        getEventTarget() const;
    virtual bool        getClipboard(ClipboardID id, IClipboard*) const;
    virtual void        getShape(SInt32& x, SInt32& y,
                            SInt32& width, SInt32& height) const;
    virtual void        getCursorPos(SInt32& x, SInt32& y) const;

    // IClient overrides
    virtual void        enter(SInt32 xAbs, SInt32 yAbs,
                            UInt32 seqNum, KeyModifierMask mask,
                            bool forScreensaver);
    virtual bool        leave();
    virtual void        setClipboard(ClipboardID, const IClipboard*);
    bool                setClipboardData(
                            ClipboardID,
                            const std::shared_ptr<const String>& data);
    virtual void        grabClipboard(ClipboardID);
    virtual void        setClipboardDirty(ClipboardID, bool);
    virtual void        keyDown(KeyID, KeyModifierMask, KeyButton);
    virtual void        keyRepeat(KeyID, KeyModifierMask,
                            SInt32 count, KeyButton);
    virtual void        keyUp(KeyID, KeyModifierMask, KeyButton);
    virtual void        mouseDown(ButtonID);
    virtual void        mouseUp(ButtonID);
    virtual void        mouseMove(SInt32 xAbs, SInt32 yAbs);
    virtual void        mouseRelativeMove(SInt32 xRel, SInt32 yRel);
    virtual void        mouseWheel(SInt32 xDelta, SInt32 yDelta);
    virtual void        screensaver(bool activate);
    virtual void        resetOptions();
    virtual void        setOptions(const OptionsList& options);
    virtual std::string getName() const;

private:
    struct CompletedFileTransfer {
        std::size_t expectedSize;
        std::string data;
        barrier::fs::path spoolPath;
        std::string dropTarget;
        DragFileList dragFileList;
        std::string remoteFileClipboardSession;
        barrier::ClipboardRevision clipboardRevision;
        barrier::FileTransferKind kind;
        std::uint64_t senderClipboardRevision;

        CompletedFileTransfer() :
            expectedSize(0),
            kind(barrier::FileTransferKind::kManual),
            senderClipboardRevision(0)
        {
        }
    };

    void                sendClipboard(ClipboardID);
    void                clearPendingFileClipboard(ClipboardID id);
    bool                finishPendingClipboardSend(
                            ClipboardID id, const std::string& data,
                            const std::shared_ptr<const String>* immutable = NULL);
    void                sendEvent(Event::Type, void*);
    void                sendConnectionFailedEvent(const char* msg);
    void                sendFileChunk(const void* data);
    void                send_file_thread(barrier::IStream* stream,
                                         const std::string& filename,
                                         const std::shared_ptr<StreamChunker>& chunker,
                                         UInt32 transferId,
                                         const std::shared_ptr<barrier::FileTransferSendState>&
                                             transactionState);
    void                send_clipboard_file_thread(const std::vector<barrier::fs::path>& sourcePaths,
                                                   barrier::IStream* stream,
                                                   const std::shared_ptr<StreamChunker>& chunker,
                                                   UInt32 transferId,
                                                   const std::shared_ptr<barrier::FileTransferSendState>&
                                                       transactionState);
    void write_to_drop_dir_thread(std::shared_ptr<CompletedFileTransfer> transfer);
    barrier::FileTransferReason startDropDirTransfer(
                            std::shared_ptr<CompletedFileTransfer> transfer);
    bool                queueDropDirTransfer(
                            std::shared_ptr<CompletedFileTransfer> transfer);
    void                drainDropDirTransferQueue();
    void                releasePendingDropDirTransfers();
    void                handleFileClipboardReady(const Event&, void*);
    void                setupConnecting();
    void                setupConnection();
    void                cleanupBulkConnection();
    void                cleanupBulkHandshake();
    void                cleanupBulkRetry();
    void                acceptBulkChannelOffer(const std::string& token);
    void                startBulkConnection(const std::string& token);
    void                scheduleBulkRetry(const std::string& token);
    void                handleBulkConnected(const Event&, void*);
    void                handleBulkConnectionFailed(const Event&, void*);
    void                handleBulkHandshakeData(const Event&, void*);
    void                handleBulkHandshakeError(const Event&, void*);
    void                handleBulkRetry(const Event&, void*);
    void                setupScreen();
    void                setupTimer();
    void                cleanupConnecting();
    void                cleanupConnection();
    void                cleanupScreen();
    void                cleanupTimer();
    void                cleanupStream();
    void                detachServerProxyForClipboardThread();
    void                releaseDetachedServerProxies();
    void                detachStreamForSendFileThread();
    void                releaseDetachedSendFileStream();
    bool                cleanupSendFileThread(bool cancel);
    bool                reapSendFileThreadIfReady();
    void                reapDetachedConnectionState();
    bool                cleanupWriteToDropDirThread();
    bool                reapWriteToDropDirThreadIfReady();
    std::shared_ptr<CompletedFileTransfer> takeCompletedFileTransfer();
    void                sendDisconnectedEvent();
    void                handleConnected(const Event&, void*);
    void                handleConnectionFailed(const Event&, void*);
    void                handleConnectTimeout(const Event&, void*);
    void                handleOutputError(const Event&, void*);
    void                handleDisconnected(const Event&, void*);
    void                handleShapeChanged(const Event&, void*);
    void                handleClipboardGrabbed(const Event&, void*);
    void                handleClipboardChanged(const Event&, void*);
    void                handleClipboardPublished(const Event&, void*);
    void                handleClipboardRetry(const Event&, void*);
    void                handleHello(const Event&, void*);
    void                handleSuspend(const Event& event, void*);
    void                handleResume(const Event& event, void*);
    void                handleFileChunkSending(const Event&, void*);
    void                handleFileRecieveCompleted(const Event&, void*);
    void                handleFileReceiveCompletionPoll(const Event&, void*);
    void                handleFileKeepAlive(const Event&, void*);
    void                handleSendFileCancelAckTimeout(const Event&, void*);
    void                handleSendFileReap(const Event&, void*);
    void                handleDropDirWriteFinished(const Event&, void*);
    void                handleStopRetry(const Event&, void*);
    void                cleanupClipboardRetryTimer();
    void                scheduleClipboardRetry(ClipboardID id);
    bool                hasPendingClipboardRetry() const;
    void                onFileRecieveCompleted(std::uint64_t generation);
    void                scheduleFileReceiveCompletionPoll(std::uint64_t generation);
    void                cleanupFileReceiveCompletionPoll();
    void                supersedeFileClipboard(const char* reason);
    void                publishMaterializedFileClipboard(const std::vector<std::string>& paths,
                                                         const std::string& sessionId);
    void                commitMaterializedFileClipboard(
                            const std::shared_ptr<const String>& data,
                            const std::string& sessionId,
                            std::size_t pathCount,
                            std::uint64_t publicationId);
    void                clearMaterializedClipboardPublication();
    std::uint64_t       allocateClipboardPublicationId();
    void                sendClipboardThread(void*);
    void                sendClipboardSelectionToServer(
                            const std::vector<barrier::fs::path>& sourcePaths,
                            const std::string& sessionId,
                            const barrier::ClipboardRevision& revision);
    void                startPendingManualFileSend();
    void                startManualFileSend(
                            const std::string& filename,
                            const std::shared_ptr<barrier::BulkChannel>& bulkChannel);
    void                startPendingFileClipboardPrefetch();
    UInt32              allocateSendFileTransferId();
    bool                transactionalFileTransferReady() const;
    bool                finishCompletedSendFileIfReady();
    void                serviceSendFileCompletion();
    void                scheduleSendFileCancelAckTimeout();
    void                cleanupSendFileCancelAckTimeout();
    void                scheduleSendFileReap();
    void                cleanupSendFileReap();
    void                resetSendFileDrainPoll();
    bool                completedSendFileMayRetire() const;
    bool                hasActivePointerLease(const char* inputType) const;

public:
    bool                m_mock;
#if defined(BARRIER_TEST_ENV)
    void                testOnFileRecieveCompleted()
    {
        onFileRecieveCompleted(m_fileReceiveSession.generation());
    }
    void                testOnFileRecieveCompleted(std::uint64_t generation)
    {
        onFileRecieveCompleted(generation);
    }
    void                testAttachStream(barrier::IStream* stream)
    {
        m_stream = stream;
        setupScreen();
    }
    void                testSetSendFileTransferId(UInt32 transferId)
    {
        m_sendFileTransferId = transferId;
    }
    UInt32              testAllocateSendFileTransferId()
    {
        return allocateSendFileTransferId();
    }
    void                testSetSendFileTransactionState(
                            const std::shared_ptr<barrier::FileTransferSendState>& state)
    {
        m_sendFileTransactionState = state;
    }
    bool                testHasSendFileTransactionState() const
    {
        return static_cast<bool>(m_sendFileTransactionState);
    }
    std::shared_ptr<barrier::FileTransferSendState>
                        testSendFileTransactionState() const
    {
        return m_sendFileTransactionState;
    }
    bool                testSendFileCancelAckPending() const
    {
        return m_sendFileCancelAckPending;
    }
    bool                testHasSendFileCancelAckTimer() const
    {
        return m_sendFileCancelAckTimeoutTimer != NULL;
    }
    void                testHandleSendFileCancelAckTimeout()
    {
        handleSendFileCancelAckTimeout(Event(), NULL);
    }
    bool                testHasSendFileReapTimer() const
    {
        return m_sendFileReapTimer != NULL;
    }
    bool                testCompletedSendFileMayRetire() const
    {
        return completedSendFileMayRetire();
    }
    void                testHandleSendFileReap()
    {
        handleSendFileReap(Event(), NULL);
    }
    void                testServiceSendFileCompletion()
    {
        serviceSendFileCompletion();
    }
    void                testSetSendFileDrainPollState(
                            UInt32 polls, UInt32 stalls, UInt32 buffered)
    {
        m_sendFileDrainPollCount = polls;
        m_sendFileDrainStallCount = stalls;
        m_sendFileLastBufferedOutput = buffered;
    }
    void                testSendClipboard(ClipboardID id) { sendClipboard(id); }
    void                testSendFileChunk(const void* data) { sendFileChunk(data); }
    void                testCleanupConnection() { cleanupConnection(); }
    void                testCleanupScreen() { cleanupScreen(); }
    void                testSetStreamOnly(barrier::IStream* stream) { m_stream = stream; }
    void                testAttachBulkStream(
                            barrier::IStream* stream,
                            const std::string& acceptedToken = std::string())
    {
        m_bulkChannel.reset(new barrier::BulkChannel(stream, this, m_events));
        m_bulkActiveToken = acceptedToken;
    }
    void                testSetupConnecting(barrier::IStream* stream)
    {
        m_stream = stream;
        setupConnecting();
    }
    void                testCleanupConnecting() { cleanupConnecting(); }
    void                testSetServerProxy(ServerProxy* server) { m_server = server; }
    void                testSetProtocolMinorVersion(SInt16 version)
    {
        m_protocolMinorVersion = version;
    }
    bool                testHasBulkRetryTimer() const
    {
        return m_bulkRetryTimer != NULL;
    }
    UInt32              testBulkRetryAttempt() const
    {
        return m_bulkRetryAttempt;
    }
    const std::string&  testBulkRetryToken() const
    {
        return m_bulkRetryToken;
    }
    void                testSetBulkHandshake(
                            barrier::IStream* stream,
                            const std::string& token)
    {
        m_bulkHandshakeStream = stream;
        m_bulkHandshakeState = kBulkWaitingForHello;
        m_bulkBindingToken = token;
        m_bulkRetryToken = token;
    }
    const std::string&  testBulkBindingToken() const
    {
        return m_bulkBindingToken;
    }
    void                testConnectBulkChannel(const std::string& token)
    {
        connectBulkChannel(token);
    }
    bool                testConnectBoundBulkChannel(
                            const std::string& token,
                            const std::string& connectionBinding)
    {
        return connectBulkChannel(token, connectionBinding);
    }
    const std::string&  testControlConnectionBinding() const
    {
        return m_controlConnectionBinding;
    }
    void                testSetControlConnectionBinding(
                            const std::string& connectionBinding)
    {
        m_controlConnectionBinding = connectionBinding;
    }
    void                testHandleBulkHandshakeData()
    {
        handleBulkHandshakeData(Event(), NULL);
    }
    void                testHandleBulkRetry()
    {
        handleBulkRetry(Event(), NULL);
    }
    void                testHandleClipboardGrabbed(ClipboardID id, UInt32 sequenceNumber = 1)
    {
        IScreen::ClipboardInfo info;
        info.m_id = id;
        info.m_sequenceNumber = sequenceNumber;
        handleClipboardGrabbed(Event(Event::kUnknown, NULL, &info, Event::kDontFreeData), NULL);
    }
    void                testHandleClipboardRetry()
    {
        handleClipboardRetry(Event(), NULL);
    }
    void                testHandleClipboardChanged(ClipboardID id,
                                                   UInt32 sequenceNumber = 1)
    {
        IScreen::ClipboardInfo info;
        info.m_id = id;
        info.m_sequenceNumber = sequenceNumber;
        handleClipboardChanged(
            Event(Event::kUnknown, NULL, &info, Event::kDontFreeData), NULL);
    }
    void                testSetActive(bool active) { m_active = active; }
    void                testSetClipboardOwnership(ClipboardID id, bool own)
    {
        m_ownClipboard[id] = own;
    }
    void                testSetClipboardSent(ClipboardID id, bool sent)
    {
        m_sentClipboard[id] = sent;
    }
    void                testSetClipboardRetryCount(ClipboardID id, UInt32 count)
    {
        m_clipboardRetryCount[id] = count;
    }
    UInt32              testClipboardRetryCount(ClipboardID id) const
    {
        return m_clipboardRetryCount[id];
    }
    void                testReleaseDetachedServerProxies() { releaseDetachedServerProxies(); }
    std::size_t         testDetachedServerProxyCount() const { return m_detachedServerProxies.size(); }
    std::size_t         testDetachedSendFileStreamCount() const { return m_detachedSendFileStreams.size(); }
    bool                testCleanupSendFileThread(bool cancel) { return cleanupSendFileThread(cancel); }
    bool                testReapSendFileThreadIfReady() { return reapSendFileThreadIfReady(); }
    void                testReapDetachedConnectionState() { reapDetachedConnectionState(); }
    void                testHandleFileKeepAlive() { handleFileKeepAlive(Event(), NULL); }
    bool                testCleanupWriteToDropDirThread() { return cleanupWriteToDropDirThread(); }
    bool                testHasWriteToDropDirThread() const { return m_writeToDropDirThread != NULL; }
    void                testSetWriteToDropDirThread(Thread* thread) { m_writeToDropDirThread = thread; }
    void                testDrainDropDirTransferQueue() { drainDropDirTransferQueue(); }
    std::size_t         testPendingDropDirTransferCount() const { return m_pendingDropDirTransfers.size(); }
    bool                testOwnClipboard(ClipboardID id) const { return m_ownClipboard[id]; }
    bool                testClipboardSent(ClipboardID id) const { return m_sentClipboard[id]; }
    bool                testClipboardSendPending(ClipboardID id) const { return m_clipboardSendPending[id]; }
    bool                testClipboardRetryPending(ClipboardID id) const { return m_clipboardRetryPending[id]; }
    void                testStartDropDirTransfer(const std::string& data)
    {
        std::shared_ptr<CompletedFileTransfer> transfer(new CompletedFileTransfer());
        transfer->expectedSize = data.size();
        transfer->data = data;
        transfer->dropTarget = barrier::fs::temp_directory_path().u8string();
        startDropDirTransfer(transfer);
    }
    void                testQueueDropDirTransfer(const std::string& data)
    {
        std::shared_ptr<CompletedFileTransfer> transfer(new CompletedFileTransfer());
        transfer->expectedSize = data.size();
        transfer->data = data;
        transfer->dropTarget = barrier::fs::temp_directory_path().u8string();
        queueDropDirTransfer(transfer);
    }
    void                testSetDetachedSendFileStream(barrier::IStream* stream)
    {
        m_detachedSendFileStreams.push_back(stream);
    }
    void                testSetSendFileThread(Thread* thread) { m_sendFileThread = thread; }
    bool                testHasSendFileThread() const { return m_sendFileThread != NULL; }
    UInt32              testSendFileTransferId() const { return m_sendFileTransferId; }
    void                testSetSendFileChunker(const std::shared_ptr<StreamChunker>& chunker)
    {
        m_sendFileChunker = chunker;
    }
    void                testSetSendFileBulkChannel(
                            const std::shared_ptr<barrier::BulkChannel>& channel)
    {
        m_sendFileBulkChannel = channel;
    }
    void                testSetSendFileIsClipboardPrefetch(bool isClipboardPrefetch)
    {
        m_sendFileIsClipboardPrefetch = isClipboardPrefetch;
    }
    void                testSetSendFileProtocolState(bool started, bool completed)
    {
        m_sendFileStarted = started;
        m_sendFileCompletionPending = completed;
    }
    const std::string&  testPendingManualFileSend() const
    {
        return m_pendingManualFileSend;
    }
    void                testSendClipboardSelectionToServer(
                            const std::vector<barrier::fs::path>& sourcePaths)
    {
        if (!m_clipboardRevision.valid()) {
            m_clipboardRevision.advance();
        }
        sendClipboardSelectionToServer(
            sourcePaths, "00000000000000000000000000000001",
            m_clipboardRevision);
    }
    void                testSendClipboardSelectionToServer(
                            const std::vector<barrier::fs::path>& sourcePaths,
                            const std::string& sessionId)
    {
        if (!m_clipboardRevision.valid()) {
            m_clipboardRevision.advance();
        }
        sendClipboardSelectionToServer(
            sourcePaths, sessionId, m_clipboardRevision);
    }
    const std::vector<barrier::fs::path>& testPendingFileClipboardPrefetchPaths() const
    {
        return m_pendingFileClipboardPrefetchPaths;
    }
    void                testSupersedeFileClipboard() { supersedeFileClipboard("test"); }
    void                testSetFileClipboardSessions(const std::string& remoteSession,
                                                     const std::string& readySession,
                                                     const std::vector<std::string>& readyPaths)
    {
        m_clipboardRevision.advance();
        m_remoteFileClipboardSession = remoteSession;
        m_remoteFileClipboardRevision = m_clipboardRevision;
        m_readyFileClipboardSession = readySession;
        m_readyFileClipboardPaths = readyPaths;
        m_readyFileClipboardRevision = m_clipboardRevision;
    }
    const std::string&  testRemoteFileClipboardSession() const { return m_remoteFileClipboardSession; }
    std::uint64_t       testClipboardRevisionSequence() const
    {
        return m_clipboardRevision.sequence();
    }
    const std::string&  testReadyFileClipboardSession() const { return m_readyFileClipboardSession; }
    const std::vector<std::string>& testReadyFileClipboardPaths() const { return m_readyFileClipboardPaths; }
    std::uint64_t       testPendingClipboardPublicationId() const
    {
        return m_pendingMaterializedClipboardPublicationId;
    }
    bool                testMaterializedClipboardCommitted() const
    {
        return m_lastCommittedClipboardPublicationId != 0;
    }
    void                testHandleClipboardPublished(
                            std::uint64_t publicationId,
                            IScreen::ClipboardPublicationResult result)
    {
        IScreen::ClipboardPublicationInfo info;
        info.m_id = kClipboardClipboard;
        info.m_publicationId = publicationId;
        info.m_result = result;
        info.m_platformSequence = 0;
        handleClipboardPublished(
            Event(Event::kUnknown, getEventTarget(), &info,
                  Event::kDontFreeData), NULL);
    }
    barrier::FileTransferKind testTransactionalReceiveKind() const
    {
        return m_transactionalReceive ? m_transactionalReceive->kind :
            barrier::FileTransferKind::kManual;
    }
    std::uint64_t       testTransactionalReceiveSenderRevision() const
    {
        return m_transactionalReceive ?
            m_transactionalReceive->senderClipboardRevision : 0;
    }
    const std::string&  testTransactionalReceiveSession() const
    {
        static const std::string empty;
        return m_transactionalReceive ?
            m_transactionalReceive->remoteFileClipboardSession : empty;
    }
    void                testHandleFileClipboardReady(const std::string& sessionId,
                                                     const std::vector<std::string>& paths,
                                                     bool publishClipboard)
    {
        FileClipboardReadyInfo info;
        info.m_sessionId = sessionId;
        info.m_paths = paths;
        info.m_revision = publishClipboard ? m_clipboardRevision :
            m_remoteFileClipboardRevision;
        info.m_publishClipboard = publishClipboard;
        Event event(Event::kUnknown, getEventTarget(), NULL,
                    Event::kDontFreeData);
        event.setDataObject(&info);
        handleFileClipboardReady(event, NULL);
    }
    void                testWriteRemoteClipboardTransfer(const std::string& data,
                                                         const std::string& sessionId)
    {
        std::shared_ptr<CompletedFileTransfer> transfer(new CompletedFileTransfer());
        transfer->expectedSize = data.size();
        transfer->data = data;
        transfer->remoteFileClipboardSession = sessionId;
        transfer->kind = barrier::FileTransferKind::kClipboard;
        m_clipboardRevision.advance();
        transfer->clipboardRevision = m_clipboardRevision;
        write_to_drop_dir_thread(transfer);
    }
    void                testWriteDroppedFileTransfer(
                            barrier::FileTransferKind kind,
                            const std::string& dropTarget,
                            const std::string& filename,
                            const std::string& data)
    {
        std::shared_ptr<CompletedFileTransfer> transfer(new CompletedFileTransfer());
        transfer->expectedSize = data.size();
        transfer->data = data;
        transfer->dropTarget = dropTarget;
        transfer->kind = kind;
        DragInformation drag;
        String mutableFilename(filename);
        drag.setFilename(mutableFilename);
        drag.setFilesize(data.size());
        drag.setEntryType(DragInformation::File);
        transfer->dragFileList.push_back(drag);
        write_to_drop_dir_thread(transfer);
    }
#endif
private:
    std::string                m_name;
    NetworkAddress        m_serverAddress;
    ISocketFactory*        m_socketFactory;
    barrier::Screen*    m_screen;
    barrier::IStream*    m_stream;
    barrier::IStream*    m_bulkHandshakeStream;
    enum BulkHandshakeState { kBulkIdle, kBulkWaitingForHello, kBulkWaitingForAck };
    BulkHandshakeState   m_bulkHandshakeState;
    std::string          m_bulkBindingToken;
    std::string          m_bulkActiveToken;
    std::shared_ptr<barrier::BulkChannel> m_bulkChannel;
    EventQueueTimer*     m_bulkRetryTimer;
    std::string          m_bulkRetryToken;
    UInt32               m_bulkRetryAttempt;
    std::string          m_controlConnectionBinding;
    std::vector<barrier::IStream*> m_detachedSendFileStreams;
    std::vector<ServerProxy*> m_detachedServerProxies;
    EventQueueTimer*    m_timer;
    EventQueueTimer*    m_clipboardRetryTimer;
    EventQueueTimer*    m_fileReceiveCompletionTimer;
    std::uint64_t       m_fileReceiveCompletionGeneration;
    ServerProxy*        m_server;
    bool                m_ready;
    bool                m_active;
    bool                m_inputBackendPoisoned;
    SInt16              m_protocolMinorVersion;
    bool                m_suspended;
    bool                m_connectOnResume;
    bool                m_terminalEventSent;
    bool                m_ownClipboard[kClipboardEnd];
    bool                m_sentClipboard[kClipboardEnd];
    bool                m_clipboardSendPending[kClipboardEnd];
    bool                m_clipboardRetryPending[kClipboardEnd];
    UInt32              m_clipboardRetryCount[kClipboardEnd];
    IClipboard::Time    m_timeClipboard[kClipboardEnd];
    ClipboardDataSnapshot m_dataClipboard[kClipboardEnd];
    ClipboardDataSnapshot m_pendingClipboardData[kClipboardEnd];
    std::shared_ptr<const String>
                        m_pendingImmutableClipboard[kClipboardEnd];
    std::vector<barrier::fs::path> m_pendingFileClipboardPaths[kClipboardEnd];
    std::string         m_pendingFileClipboardSession[kClipboardEnd];
    barrier::ClipboardRevision
                        m_pendingFileClipboardRevision[kClipboardEnd];
    IEventQueue*        m_events;
    FileReceiveSession     m_fileReceiveSession;
    DragFileList        m_dragFileList;
    std::string m_dragFileExt;
    Thread*                m_sendFileThread;
    std::shared_ptr<StreamChunker> m_sendFileChunker;
    std::shared_ptr<barrier::BulkChannel> m_sendFileBulkChannel;
    UInt32              m_sendFileTransferId;
    UInt32              m_nextSendFileTransferSequence;
    std::shared_ptr<barrier::FileTransferSendState>
                        m_sendFileTransactionState;
    bool                m_sendFileIsClipboardPrefetch;
    bool                m_sendFileStarted;
    bool                m_sendFileStartAcknowledged;
    bool                m_sendFileCompletionPending;
    bool                m_sendFileCancelAckPending;
    EventQueueTimer*    m_sendFileCancelAckTimeoutTimer;
    UInt32              m_sendFileCancelAckTimeoutTransferId;
    EventQueueTimer*    m_sendFileReapTimer;
    UInt32              m_sendFileReapTransferId;
    UInt32              m_sendFileDrainPollCount;
    UInt32              m_sendFileDrainStallCount;
    UInt32              m_sendFileLastBufferedOutput;
    std::string         m_pendingManualFileSend;
    std::vector<barrier::fs::path> m_pendingFileClipboardPrefetchPaths;
    std::string         m_pendingFileClipboardPrefetchSession;
    barrier::ClipboardRevision
                        m_pendingFileClipboardPrefetchRevision;
    Thread*                m_writeToDropDirThread;
    std::deque<std::shared_ptr<CompletedFileTransfer> > m_pendingDropDirTransfers;
    UInt32              m_transactionalReceiveId;
    std::shared_ptr<CompletedFileTransfer> m_transactionalReceive;
    TCPSocket*            m_socket;
    bool                m_useSecureNetwork;
    ClientArgs            m_args;
    bool                m_enableClipboard;
    barrier::ClipboardRevision m_clipboardRevision;
    std::string         m_remoteFileClipboardSession;
    barrier::ClipboardRevision m_remoteFileClipboardRevision;
    std::string         m_readyFileClipboardSession;
    std::vector<std::string> m_readyFileClipboardPaths;
    barrier::ClipboardRevision m_readyFileClipboardRevision;
    std::uint64_t       m_nextClipboardPublicationId;
    std::uint64_t       m_pendingMaterializedClipboardPublicationId;
    std::uint64_t       m_lastCommittedClipboardPublicationId;
    std::shared_ptr<const String> m_pendingMaterializedClipboardData;
    std::string         m_pendingMaterializedClipboardSession;
    barrier::ClipboardRevision m_pendingMaterializedClipboardRevision;
    std::uint64_t       m_fileReceiveClipboardGeneration;
    std::string         m_fileReceiveRemoteFileClipboardSession;
    barrier::ClipboardRevision m_fileReceiveClipboardRevision;
};
