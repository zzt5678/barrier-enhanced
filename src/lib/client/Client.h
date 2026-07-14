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
#include "barrier/DragInformation.h"
#include "barrier/FileReceiveSession.h"
#include "base/Event.h"
#include "barrier/INode.h"
#include "barrier/ClientArgs.h"
#include "net/NetworkAddress.h"
#include "base/EventTypes.h"
#include "mt/CondVar.h"
#include "io/filesystem.h"

#include <deque>
#include <memory>
#include <vector>

class EventQueueTimer;
namespace barrier { class Screen; }
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
class Client : public IClient, public INode {
public:
	class FileClipboardReadyInfo : public EventData {
	public:
		FileClipboardReadyInfo() : m_publishClipboard(false) { }

		std::string m_sessionId;
		std::vector<std::string> m_paths;
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

    //! Get address of server
    /*!
    Returns the address of the server the client is connected (or wants
    to connect) to.
    */
    NetworkAddress        getServerAddress() const;

    //! Return true if received file size is valid
    bool                isReceivedFileSizeValid();

    FileReceiveSession& getFileReceiveSession() { return m_fileReceiveSession; }

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

        CompletedFileTransfer() : expectedSize(0) { }
    };

    void                sendClipboard(ClipboardID);
    bool                finishPendingClipboardSend(ClipboardID id, const std::string& data);
    void                sendEvent(Event::Type, void*);
    void                sendConnectionFailedEvent(const char* msg);
    void                sendFileChunk(const void* data);
    void                send_file_thread(barrier::IStream* stream,
                                         const std::string& filename,
                                         const std::shared_ptr<StreamChunker>& chunker,
                                         UInt32 transferId);
    void                send_clipboard_file_thread(const std::vector<barrier::fs::path>& sourcePaths,
                                                   barrier::IStream* stream,
                                                   const std::shared_ptr<StreamChunker>& chunker,
                                                   UInt32 transferId);
    void write_to_drop_dir_thread(std::shared_ptr<CompletedFileTransfer> transfer);
    void                startDropDirTransfer(std::shared_ptr<CompletedFileTransfer> transfer);
    void                queueDropDirTransfer(std::shared_ptr<CompletedFileTransfer> transfer);
    void                drainDropDirTransferQueue();
    void                releasePendingDropDirTransfers();
    void                handleFileClipboardReady(const Event&, void*);
    void                setupConnecting();
    void                setupConnection();
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
    void                handleClipboardRetry(const Event&, void*);
    void                handleHello(const Event&, void*);
    void                handleSuspend(const Event& event, void*);
    void                handleResume(const Event& event, void*);
    void                handleFileChunkSending(const Event&, void*);
    void                handleFileRecieveCompleted(const Event&, void*);
    void                handleFileReceiveCompletionPoll(const Event&, void*);
    void                handleFileKeepAlive(const Event&, void*);
    void                handleDropDirWriteFinished(const Event&, void*);
    void                handleStopRetry(const Event&, void*);
    void                cleanupClipboardRetryTimer();
    void                scheduleClipboardRetry(ClipboardID id);
    bool                hasPendingClipboardRetry() const;
    void                onFileRecieveCompleted(std::uint64_t generation);
    void                scheduleFileReceiveCompletionPoll(std::uint64_t generation);
    void                cleanupFileReceiveCompletionPoll();
    void                publishMaterializedFileClipboard(const std::vector<std::string>& paths,
                                                         const std::string& sessionId);
    void                sendClipboardThread(void*);
    void                sendClipboardSelectionToServer(const std::vector<barrier::fs::path>& sourcePaths);

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
    void                testSendClipboard(ClipboardID id) { sendClipboard(id); }
    void                testSendFileChunk(const void* data) { sendFileChunk(data); }
    void                testCleanupConnection() { cleanupConnection(); }
    void                testCleanupScreen() { cleanupScreen(); }
    void                testSetStreamOnly(barrier::IStream* stream) { m_stream = stream; }
    void                testSetupConnecting(barrier::IStream* stream)
    {
        m_stream = stream;
        setupConnecting();
    }
    void                testCleanupConnecting() { cleanupConnecting(); }
    void                testSetServerProxy(ServerProxy* server) { m_server = server; }
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
    void                testSetSendFileChunker(const std::shared_ptr<StreamChunker>& chunker)
    {
        m_sendFileChunker = chunker;
    }
    void                testSetFileClipboardSessions(const std::string& remoteSession,
                                                     const std::string& readySession,
                                                     const std::vector<std::string>& readyPaths)
    {
        m_remoteFileClipboardSession = remoteSession;
        m_readyFileClipboardSession = readySession;
        m_readyFileClipboardPaths = readyPaths;
    }
    const std::string&  testRemoteFileClipboardSession() const { return m_remoteFileClipboardSession; }
    const std::string&  testReadyFileClipboardSession() const { return m_readyFileClipboardSession; }
    const std::vector<std::string>& testReadyFileClipboardPaths() const { return m_readyFileClipboardPaths; }
    void                testHandleFileClipboardReady(const std::string& sessionId,
                                                     const std::vector<std::string>& paths,
                                                     bool publishClipboard)
    {
        FileClipboardReadyInfo info;
        info.m_sessionId = sessionId;
        info.m_paths = paths;
        info.m_publishClipboard = publishClipboard;
        Event event(Event::kUnknown, getEventTarget(), &info, Event::kDontFreeData);
        handleFileClipboardReady(event, NULL);
    }
    void                testWriteRemoteClipboardTransfer(const std::string& data,
                                                         const std::string& sessionId)
    {
        std::shared_ptr<CompletedFileTransfer> transfer(new CompletedFileTransfer());
        transfer->expectedSize = data.size();
        transfer->data = data;
        transfer->remoteFileClipboardSession = sessionId;
        write_to_drop_dir_thread(transfer);
    }
#endif
private:
    std::string                m_name;
    NetworkAddress        m_serverAddress;
    ISocketFactory*        m_socketFactory;
    barrier::Screen*    m_screen;
    barrier::IStream*    m_stream;
    std::vector<barrier::IStream*> m_detachedSendFileStreams;
    std::vector<ServerProxy*> m_detachedServerProxies;
    EventQueueTimer*    m_timer;
    EventQueueTimer*    m_clipboardRetryTimer;
    EventQueueTimer*    m_fileReceiveCompletionTimer;
    std::uint64_t       m_fileReceiveCompletionGeneration;
    ServerProxy*        m_server;
    bool                m_ready;
    bool                m_active;
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
    std::vector<barrier::fs::path> m_pendingFileClipboardPaths[kClipboardEnd];
    IEventQueue*        m_events;
    FileReceiveSession     m_fileReceiveSession;
    DragFileList        m_dragFileList;
    std::string m_dragFileExt;
    Thread*                m_sendFileThread;
    std::shared_ptr<StreamChunker> m_sendFileChunker;
    UInt32              m_sendFileTransferId;
    bool                m_sendFileIsClipboardPrefetch;
    Thread*                m_writeToDropDirThread;
    std::deque<std::shared_ptr<CompletedFileTransfer> > m_pendingDropDirTransfers;
    TCPSocket*            m_socket;
    bool                m_useSecureNetwork;
    ClientArgs            m_args;
    bool                m_enableClipboard;
    std::string         m_remoteFileClipboardSession;
    std::string         m_readyFileClipboardSession;
    std::vector<std::string> m_readyFileClipboardPaths;
};
