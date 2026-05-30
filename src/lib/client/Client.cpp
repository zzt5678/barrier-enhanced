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

std::string trimClipboardLine(std::string value)
{
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' ||
                              value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }

    size_t start = 0;
    while (start < value.size() &&
           (value[start] == ' ' || value[start] == '\t' ||
            value[start] == '\r' || value[start] == '\n')) {
        ++start;
    }

    return value.substr(start);
}

std::vector<barrier::fs::path> clipboardFilePaths(const Clipboard& clipboard)
{
    std::vector<barrier::fs::path> paths;
    if (!clipboard.open(0)) {
        return paths;
    }

    if (clipboard.has(IClipboard::kText)) {
        std::istringstream lines(clipboard.get(IClipboard::kText));
        std::string line;
        while (std::getline(lines, line)) {
            line = trimClipboardLine(line);
            if (line.empty()) {
                continue;
            }

            barrier::fs::path path = barrier::fs::u8path(line);
            if (barrier::fs::exists(path)) {
                paths.push_back(path);
            }
        }
    }

    clipboard.close();
    return paths;
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
    m_timer(NULL),
    m_clipboardRetryTimer(NULL),
    m_server(NULL),
    m_ready(false),
    m_active(false),
    m_suspended(false),
    m_connectOnResume(false),
    m_events(events),
    m_sendFileThread(NULL),
    m_writeToDropDirThread(NULL),
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
        m_clipboardRetryPending[id] = false;
        m_clipboardRetryCount[id] = 0;
        m_timeClipboard[id] = 0;
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
        sendEvent(m_events->forClient().disconnected(), NULL);
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

    if (m_sendFileChunker) {
        m_sendFileChunker->interruptFile();
        m_sendFileThread = NULL;
    }
}

bool
Client::leave()
{
    m_active = false;

    m_screen->leave();

    if (m_enableClipboard) {
        // Re-read clipboards on leave instead of relying only on the
        // earlier ownership flag. On Windows the clipboard-viewer
        // notification can be missed and checkClipboards() only detects
        // the new owner during Screen::leave(), so gating on
        // m_ownClipboard here can drop a just-copied clipboard update.
        for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
            sendClipboard(id);
        }
    }

    return true;
}

void
Client::setClipboard(ClipboardID id, const IClipboard* clipboard)
{
    m_screen->setClipboard(id, clipboard);

    if (id == kClipboardClipboard && clipboard != NULL) {
        RemoteFileClipboard::Data remoteFileClipboard;
        if (RemoteFileClipboard::readFromClipboard(*clipboard, remoteFileClipboard) &&
            remoteFileClipboard.mode == RemoteFileClipboard::Mode::SourcePaths) {
            m_remoteFileClipboardSession = remoteFileClipboard.sessionId;
        }
        else {
            m_remoteFileClipboardSession.clear();
            m_readyFileClipboardSession.clear();
            m_readyFileClipboardPaths.clear();
        }
    }

    m_ownClipboard[id]  = false;
    m_sentClipboard[id] = false;
}

void
Client::grabClipboard(ClipboardID id)
{
    m_screen->grabClipboard(id);
    m_ownClipboard[id]  = false;
    m_sentClipboard[id] = false;
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

    bool hasFileList = false;
    if (id == kClipboardClipboard) {
        if (clipboard.open(0)) {
            hasFileList = clipboard.has(IClipboard::kFileList);
            clipboard.close();
        }

        if (hasFileList) {
            if (RemoteFileClipboard::stripImageFileTransferMetadata(clipboard)) {
                LOG((CLOG_INFO "stripped image file-transfer metadata before sending clipboard to server"));
            }

            RemoteFileClipboard::Data remoteFileClipboard;
            if (RemoteFileClipboard::normalizeClipboard(clipboard, &remoteFileClipboard) &&
                remoteFileClipboard.mode == RemoteFileClipboard::Mode::SourcePaths) {
                LOG((CLOG_INFO "remote clipboard source prepared: session=%s items=%lu",
                     remoteFileClipboard.sessionId.c_str(),
                     static_cast<unsigned long>(remoteFileClipboard.paths.size())));
                sendClipboardSelectionToServer(remoteFileClipboard.paths);
            }
        }
    }

    // check time
    if (m_timeClipboard[id] == 0 ||
        clipboard.getTime() != m_timeClipboard[id]) {
        // save new time
        m_timeClipboard[id] = clipboard.getTime();

        // marshall the data
        std::string data = clipboard.marshall();

        // save and send data if different or not yet sent
        if (!m_sentClipboard[id] || data != m_dataClipboard[id]) {
            if (!hasFileList && sendClipboardFileSelection(id, clipboard)) {
                m_sentClipboard[id] = true;
                m_dataClipboard[id] = data;
                return;
            }
            m_sentClipboard[id] = true;
            m_dataClipboard[id] = data;
            m_server->onClipboardChanged(id, &clipboard);
        }
    }
}

bool
Client::sendClipboardFileSelection(ClipboardID id, const Clipboard& clipboard)
{
    if (id != kClipboardClipboard || !m_args.m_enableDragDrop || m_server == NULL) {
        return false;
    }

    if (clipboard.open(0)) {
        const bool hasImagePayload = clipboard.has(IClipboard::kPNG) ||
                                     clipboard.has(IClipboard::kBitmap);
        clipboard.close();
        if (hasImagePayload) {
            LOG((CLOG_DEBUG "skipping clipboard file-transfer path because image payload is present"));
            return false;
        }
    }

    const std::vector<barrier::fs::path> paths = clipboardFilePaths(clipboard);
    if (paths.empty()) {
        return false;
    }

    DragFileList dragFileList;
    std::string transferPaths;
    for (const auto& path : paths) {
        if (!transferPaths.empty()) {
            transferPaths.push_back('\n');
        }
        transferPaths += path.u8string();

        DragInformation info;
        std::string pathString = path.u8string();
        info.setFilename(pathString);
        if (barrier::fs::is_directory(path)) {
            info.setEntryType(DragInformation::Directory);
        }
        dragFileList.push_back(info);
    }

    std::string infoString;
    UInt32 fileCount = DragInformation::setupDragInfo(dragFileList, infoString);
    if (fileCount == 0) {
        return false;
    }

    LOG((CLOG_INFO "clipboard file selection detected, sending %u item(s)", fileCount));
    sendDragInfo(fileCount, infoString, infoString.size());
    sendFileToServer(transferPaths);
    return true;
}

void
Client::sendEvent(Event::Type type, void* data)
{
    m_events->addEvent(Event(type, getEventTarget(), data));
}

void
Client::sendConnectionFailedEvent(const char* msg)
{
    FailInfo* info = new FailInfo(msg);
    info->m_retry = true;
    Event event(m_events->forClient().connectionFailed(), getEventTarget(), info, Event::kDontFreeData);
    m_events->addEvent(event);
}

void
Client::sendFileChunk(const void* data)
{
    FileChunk* chunk = static_cast<FileChunk*>(const_cast<void*>(data));
    LOG((CLOG_DEBUG1 "send file chunk"));
    assert(m_server != NULL);

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
        m_events->removeHandler(m_events->forIDataSocket().connectionFailed(),
                            m_stream->getEventTarget());
    }
}

void
Client::cleanupConnection()
{
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
        cleanupStream();
    }
}

void
Client::cleanupScreen()
{
    cleanupClipboardRetryTimer();
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        m_clipboardRetryPending[id] = false;
        m_clipboardRetryCount[id] = 0;
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
    delete m_stream;
    m_stream = NULL;
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
    sendEvent(m_events->forClient().disconnected(), NULL);
}

void
Client::handleDisconnected(const Event&, void*)
{
    cleanupTimer();
    cleanupScreen();
    cleanupConnection();
    LOG((CLOG_DEBUG1 "disconnected"));
    sendEvent(m_events->forClient().disconnected(), NULL);
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
    m_timeClipboard[info->m_id] = 0;
    m_clipboardRetryPending[info->m_id] = false;
    m_clipboardRetryCount[info->m_id] = 0;

    // if we're not the active screen then send the clipboard now,
    // otherwise we'll wait until we leave.
    if (!m_active) {
        sendClipboard(info->m_id);
    }
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
Client::handleFileRecieveCompleted(const Event& event, void*)
{
    onFileRecieveCompleted();
}

void
Client::onFileRecieveCompleted()
{
    if (isReceivedFileSizeValid()) {
        m_writeToDropDirThread = new Thread([this](){ write_to_drop_dir_thread(); });
    }
}

void
Client::handleFileClipboardReady(const Event& event, void*)
{
    FileClipboardReadyInfo* info =
        static_cast<FileClipboardReadyInfo*>(event.getDataObject());
    if (info == NULL) {
        return;
    }

    m_readyFileClipboardSession = info->m_sessionId;
    m_readyFileClipboardPaths = info->m_paths;
    if (!m_remoteFileClipboardSession.empty() &&
        m_remoteFileClipboardSession == m_readyFileClipboardSession) {
        publishMaterializedFileClipboard(m_readyFileClipboardPaths,
                                         m_readyFileClipboardSession);
    }
}

void
Client::handleStopRetry(const Event&, void*)
{
    m_args.m_restartable = false;
}

void Client::write_to_drop_dir_thread()
{
    LOG((CLOG_DEBUG "starting write to drop dir thread"));

    if (!m_remoteFileClipboardSession.empty() &&
        TransferArchive::isPackageData(m_receivedFileData)) {
        LOG((CLOG_INFO "remote clipboard package received: session=%s", m_remoteFileClipboardSession.c_str()));
        const barrier::fs::path spoolDir =
            barrier::DataDirectories::profile() / "clipboard-cache" / "client";
        std::vector<barrier::fs::path> roots;
        std::string error;
        if (RemoteFileClipboard::extractPackage(m_receivedFileData, spoolDir, roots, error)) {
            LOG((CLOG_INFO "remote clipboard package materialized: session=%s items=%lu",
                 m_remoteFileClipboardSession.c_str(),
                 static_cast<unsigned long>(roots.size())));
            FileClipboardReadyInfo* info = new FileClipboardReadyInfo();
            info->m_sessionId = m_remoteFileClipboardSession;
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

        String().swap(m_receivedFileData);
        return;
    }

    std::vector<String> droppedPaths = DropHelper::writeToDir(m_screen->getDropTarget(), m_dragFileList,
                    m_receivedFileData);

    if (!droppedPaths.empty()) {
        std::string clipboardPaths;
        for (const auto& path : droppedPaths) {
            if (!clipboardPaths.empty()) {
                clipboardPaths.push_back('\n');
            }
            clipboardPaths += path;
        }

        Clipboard clipboard;
        const std::string sessionId = RemoteFileClipboard::createSessionId();
        if (RemoteFileClipboard::buildMaterializedClipboard(
                utf8PathsToFsPaths(droppedPaths), sessionId, clipboard)) {
            setClipboard(kClipboardClipboard, &clipboard);
            m_sentClipboard[kClipboardClipboard] = true;
            m_dataClipboard[kClipboardClipboard] = clipboard.marshall();
            LOG((CLOG_INFO "dropped file(s) published as file clipboard: session=%s items=%lu",
                 sessionId.c_str(),
                 static_cast<unsigned long>(droppedPaths.size())));
        }
        else if (clipboard.open(0)) {
            clipboard.empty();
            clipboard.add(IClipboard::kText, clipboardPaths);
            clipboard.close();
            setClipboard(kClipboardClipboard, &clipboard);
            m_sentClipboard[kClipboardClipboard] = true;
            m_dataClipboard[kClipboardClipboard] = clipboard.marshall();
        }
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

    if (m_sendFileChunker) {
        m_sendFileChunker->interruptFile();
    }

    auto chunker = std::make_shared<StreamChunker>();
    m_sendFileChunker = chunker;
    LOG((CLOG_INFO "remote clipboard prefetch started: direction=client-to-server items=%lu",
         static_cast<unsigned long>(sourcePaths.size())));
    m_sendFileThread = new Thread([this, sourcePaths, chunker]() {
        send_clipboard_file_thread(sourcePaths, chunker);
    });
}

void
Client::send_clipboard_file_thread(const std::vector<barrier::fs::path>& sourcePaths,
                                   const std::shared_ptr<StreamChunker>& chunker)
{
    barrier::fs::path packagePath;
    try {
        RemoteFileClipboard::Data payload;
        payload.mode = RemoteFileClipboard::Mode::SourcePaths;
        payload.paths = sourcePaths;

        std::string error;
        if (!RemoteFileClipboard::createPackage(payload, packagePath, error)) {
            throw std::runtime_error(error);
        }

        chunker->sendFile(packagePath.u8string().c_str(), m_events, this, m_stream);
    }
    catch (std::runtime_error& error) {
        LOG((CLOG_ERR "failed sending remote clipboard file package: %s", error.what()));
    }

    if (!packagePath.empty()) {
        barrier::fs::remove(packagePath);
    }
    if (m_sendFileChunker == chunker) {
        m_sendFileChunker.reset();
    }
    m_sendFileThread = NULL;
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
    return m_expectedFileSize == m_receivedFileData.size();
}

void
Client::sendFileToServer(const std::string& filename)
{
    if (m_sendFileChunker) {
        m_sendFileChunker->interruptFile();
    }

    auto chunker = std::make_shared<StreamChunker>();
    m_sendFileChunker = chunker;
    m_sendFileThread = new Thread([this, filename, chunker]() { send_file_thread(filename, chunker); });
}

void Client::send_file_thread(const std::string& filename, const std::shared_ptr<StreamChunker>& chunker)
{
    barrier::fs::path sourcePath;
    barrier::fs::path tempPackagePath;
    try {
        std::string error;
        if (!prepareTransferSource(filename.c_str(), sourcePath, tempPackagePath, error)) {
            throw std::runtime_error(error);
        }

        const barrier::fs::path& transferPath =
            tempPackagePath.empty() ? sourcePath : tempPackagePath;
        chunker->sendFile(transferPath.u8string().c_str(), m_events, this, m_stream);
    }
    catch (std::runtime_error& error) {
        LOG((CLOG_ERR "failed sending file chunks: %s", error.what()));
    }

    if (!tempPackagePath.empty()) {
        barrier::fs::remove(tempPackagePath);
    }
    if (m_sendFileChunker == chunker) {
        m_sendFileChunker.reset();
    }
    m_sendFileThread = NULL;
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

    return true;
}

}
