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

#include "client/ServerProxy.h"

#include "client/Client.h"
#include "barrier/FileChunk.h"
#include "barrier/ClipboardChunk.h"
#include "barrier/RemoteFileClipboard.h"
#include "barrier/BulkChannel.h"
#include "barrier/StreamChunker.h"
#include "barrier/Clipboard.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/option_types.h"
#include "barrier/protocol_types.h"
#include "barrier/XBarrier.h"
#include "io/IStream.h"
#include "base/Log.h"
#include "base/IEventQueue.h"
#include "base/TMethodEventJob.h"
#include "base/XBase.h"
#include "mt/Thread.h"
#include "mt/ThreadShutdown.h"

#include <algorithm>
#include <memory>
#include <cstring>
#include <utility>

namespace {

const UInt32 kMaxKeepAliveAlarmDeferrals = 8;
const size_t kSynchronousClipboardSendLimit = 256 * 1024;
const size_t kMaxFramesPerInputBatch = 64;
const size_t kMaxBytesPerInputBatch = 256 * 1024;
const double kMaxSecondsPerInputBatch = 0.002;
const double kFileTransferReceiveInitialPollSeconds = 0.01;
const double kFileTransferReceiveMaxPollSeconds = 0.5;
const double kFileTransferReceivePollDeadlineSeconds = 5.0;

bool isNewerInputSequence(UInt32 candidate, UInt32 current)
{
    const UInt32 distance = candidate - current;
    return distance != 0 && distance < 0x80000000u;
}

bool isTransactionalFileControlCode(const UInt8* code)
{
    return std::memcmp(code, kMsgDFileTransferStart1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferStartAck1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferCancel1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferCancelAck1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferCommitAck1_12, 4) == 0;
}

bool isTransactionalFileBulkCode(const UInt8* code)
{
    return std::memcmp(code, kMsgDFileTransferData1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferEnd1_12, 4) == 0;
}

}

//
// ServerProxy
//

ServerProxy::ServerProxy(Client* client, barrier::IStream* stream, IEventQueue* events,
                         SInt16 protocolMinorVersion) :
    m_client(client),
    m_stream(stream),
    m_seqNum(0),
    m_hasEnterSequence(false),
    m_inputActive(false),
    m_protocolMinorVersion(protocolMinorVersion),
    m_connectionBinding(),
    m_preparedEnterSequence(0),
    m_hasPreparedEnter(false),
    m_preparedEnterReady(false),
    m_preparedInputGeneration(0),
    m_lastInputSequence(0),
    m_hasInputSequence(false),
    m_inputFrameAccepted(true),
    m_inputFrameBroadcast(false),
    m_inputFrameHasEpoch(false),
    m_epochPressedKeys(),
    m_epochPressedButtons(),
    m_compressMouse(false),
    m_compressMouseRelative(false),
    m_xMouse(0),
    m_yMouse(0),
    m_dxMouse(0),
    m_dyMouse(0),
    m_ignoreMouse(false),
    m_infoAckTimer(true),
    m_lowLatencyMode(false),
    m_nestedRemoteMode(false),
    m_keepAliveAlarm(0.0),
    m_keepAliveAlarmTimer(NULL),
    m_keepAliveAlarmDeferrals(0),
    m_keepAliveMissedAlarms(0),
    m_lastKeepAlivePendingInput(false),
    m_lastKeepAliveBufferedOutput(0),
    m_keepAliveActivityTimer(true),
    m_parser(&ServerProxy::parseHandshakeMessage),
    m_events(events),
    m_clipboardSendThread(NULL),
    m_clipboardBulkChannel(),
    m_clipboardSendStream(stream),
    m_detachedForDeferredCleanup(false),
    m_clipboardSendId(kClipboardEnd),
    m_clipboardSendSucceeded(false),
    m_clipboardSendResultAvailable(false),
    m_clipboardSendAttempt(),
    m_nextClipboardSendAttempt(0),
    m_latestClipboardSendAttempt(),
    m_fileTransferReceiver(),
    m_fileTransferReceiveBulkChannel(),
    m_fileTransferReceiveTimer(NULL),
    m_fileTransferReceiveId(0),
    m_fileTransferCancelAckPending(false),
    m_fileTransferReceivePollBudgetId(0),
    m_fileTransferReceivePollElapsed(0.0),
    m_fileTransferReceiveNextPollDelay(
        kFileTransferReceiveInitialPollSeconds)
{
    assert(m_client != NULL);
    assert(m_stream != NULL);

    // initialize modifier translation table
    for (KeyModifierID id = 0; id < kKeyModifierIDLast; ++id)
        m_modifierTranslationTable[id] = id;

    // handle data on stream
    m_events->adoptHandler(m_events->forIStream().inputReady(),
                            m_stream->getEventTarget(),
                            new TMethodEventJob<ServerProxy>(this,
                                &ServerProxy::handleData));

    m_events->adoptHandler(m_events->forClipboard().clipboardSending(),
                            this,
                            new TMethodEventJob<ServerProxy>(this,
                                &ServerProxy::handleClipboardSendingEvent));
    m_events->adoptHandler(m_events->forFile().keepAlive(),
                            this,
                            new TMethodEventJob<ServerProxy>(this,
                                &ServerProxy::handleKeepAliveEvent));

    // send heartbeat
    setKeepAliveRate(kKeepAliveRate);
}

ServerProxy::~ServerProxy()
{
    resetTransactionalFileReceive(true);
    if (!cleanupClipboardSendThread(true) && m_clipboardSendThread != NULL) {
        LOG((CLOG_ERR "waiting for clipboard sender before destroying server proxy"));
        barrier::waitForFinalThreadShutdown(
            "client clipboard sender",
            barrier::kFinalThreadShutdownDeadlineSeconds,
            [this](double timeout) {
                return m_clipboardSendThread->wait(timeout);
            });
        delete m_clipboardSendThread;
        m_clipboardSendThread = NULL;
        m_clipboardChunker.reset();
    }
    setKeepAliveRate(-1.0);
    if (!m_detachedForDeferredCleanup) {
        m_events->removeHandler(m_events->forIStream().inputReady(),
                                m_stream->getEventTarget());
        m_events->removeHandler(m_events->forClipboard().clipboardSending(), this);
        m_events->removeHandler(m_events->forFile().keepAlive(), this);
    }
}

void
ServerProxy::resetKeepAliveAlarm()
{
    if (m_keepAliveAlarmTimer != NULL) {
        m_events->removeHandler(Event::kTimer, m_keepAliveAlarmTimer);
        m_events->deleteTimer(m_keepAliveAlarmTimer);
        m_keepAliveAlarmTimer = NULL;
    }
    if (m_keepAliveAlarm > 0.0) {
        m_keepAliveAlarmTimer =
            m_events->newOneShotTimer(m_keepAliveAlarm, NULL);
        m_events->adoptHandler(Event::kTimer, m_keepAliveAlarmTimer,
                            new TMethodEventJob<ServerProxy>(this,
                                &ServerProxy::handleKeepAliveAlarm));
    }
}

void
ServerProxy::setKeepAliveRate(double rate)
{
    m_keepAliveAlarm = rate * kKeepAlivesUntilDeath;
    m_keepAliveAlarmDeferrals = 0;
    m_keepAliveMissedAlarms = 0;
    m_lastKeepAlivePendingInput = false;
    m_lastKeepAliveBufferedOutput = 0;
    m_keepAliveActivityTimer.start();
    m_keepAliveActivityTimer.reset();
    resetKeepAliveAlarm();
}

void
ServerProxy::handleData(const Event&, void*)
{
    bool receivedMessage = false;
    size_t parsedFrames = 0;
    size_t parsedBytes = 0;
    Stopwatch parseTimer;

    while (true) {
        const UInt32 frameSize = m_stream->getSize();
        UInt8 code[4];
        const UInt32 n = m_stream->read(code, 4);
        if (n == 0) {
            break;
        }

        // verify we got an entire code
        if (n != 4) {
            LOG((CLOG_ERR "incomplete message from server: %d bytes", n));
            m_client->disconnect("incomplete message from server");
            return;
        }
        receivedMessage = true;

        // parse message
        LOG((CLOG_DEBUG2 "msg from server: %c%c%c%c", code[0], code[1], code[2], code[3]));
        Client* const client = m_client;
        try {
            switch ((this->*m_parser)(code)) {
            case kOkay:
                break;

            case kUnknown:
                LOG((CLOG_ERR "invalid message from server: %c%c%c%c", code[0], code[1], code[2], code[3]));
                m_client->disconnect("invalid message from server");
                return;

            case kDisconnect:
                // Some legacy message handlers disconnect synchronously and
                // delete this proxy before returning kDisconnect.  Use the
                // stable client pointer captured before parsing, and only
                // close raw protocol-failure paths that have not already
                // completed their connection cleanup.
                if (client->isConnected()) {
                    client->disconnect("invalid message from server");
                }
                return;
            }
        } catch (const XBadClient& e) {
            // TODO: disconnect handling is currently dispersed across both parseMessage() and
            // handleData() functions, we should collect that to a single place

            LOG((CLOG_ERR "protocol error from server: %s", e.what()));
            ProtocolUtil::writef(m_stream, kMsgEBad);
            m_client->disconnect("invalid message from server");
            return;
        }

        ++parsedFrames;
        parsedBytes += frameSize >= 4 ? frameSize : 4;
        const bool budgetExhausted =
            parsedFrames >= kMaxFramesPerInputBatch ||
            parsedBytes >= kMaxBytesPerInputBatch ||
            parseTimer.getTime() >= kMaxSecondsPerInputBatch;
        if (budgetExhausted) {
            if (m_stream->getSize() != 0) {
                m_events->addEvent(Event(m_events->forIStream().inputReady(),
                    m_stream->getEventTarget()));
            }
            break;
        }
    }

    if (receivedMessage) {
        m_keepAliveAlarmDeferrals = 0;
        m_keepAliveMissedAlarms = 0;
        m_lastKeepAlivePendingInput = false;
        m_lastKeepAliveBufferedOutput = 0;
        m_keepAliveActivityTimer.reset();
    }
    flushCompressedMouse();
}

ServerProxy::EResult
ServerProxy::parseHandshakeMessage(const UInt8* code)
{
    if (memcmp(code, kMsgQInfo, 4) == 0) {
        queryInfo();
    }

    else if (memcmp(code, kMsgCInfoAck, 4) == 0) {
        infoAcknowledgment();
    }

    else if (m_protocolMinorVersion >= 12 &&
             memcmp(code, kMsgCBulkOffer1_12, 4) == 0) {
        if (!bulkOffer1_12()) {
            m_client->disconnect("invalid connection-bound bulk offer");
            return kDisconnect;
        }
    }

    else if (m_protocolMinorVersion >= 9 && m_protocolMinorVersion < 12 &&
             memcmp(code, kMsgCBulkOffer, 4) == 0) {
        bulkOffer();
    }

    else if (memcmp(code, kMsgDSetOptions, 4) == 0) {
        setOptions();

        // handshake is complete
        m_parser = &ServerProxy::parseMessage;
        m_client->handshakeComplete();
    }

    else if (memcmp(code, kMsgCResetOptions, 4) == 0) {
        resetOptions();
    }

    else if (memcmp(code, kMsgCKeepAlive, 4) == 0) {
        // echo keep alives and reset alarm
        ProtocolUtil::writef(m_stream, kMsgCKeepAlive);
        m_keepAliveAlarmDeferrals = 0;
        m_keepAliveMissedAlarms = 0;
        resetKeepAliveAlarm();
    }

    else if (memcmp(code, kMsgCNoop, 4) == 0) {
        // accept and discard no-op
    }

    else if (memcmp(code, kMsgCClose, 4) == 0) {
        // server wants us to hangup
        LOG((CLOG_DEBUG1 "recv close"));
        m_client->disconnect(NULL);
        return kDisconnect;
    }

    else if (memcmp(code, kMsgEIncompatible, 4) == 0) {
        SInt32 major, minor;
        ProtocolUtil::readf(m_stream,
                        kMsgEIncompatible + 4, &major, &minor);
        LOG((CLOG_ERR "server has incompatible version %d.%d", major, minor));
        m_client->disconnect("server has incompatible version");
        return kDisconnect;
    }

    else if (memcmp(code, kMsgEBusy, 4) == 0) {
        LOG((CLOG_ERR "server already has a connected client with name \"%s\"", m_client->getName().c_str()));
        m_client->disconnect("server already has a connected client with our name");
        return kDisconnect;
    }

    else if (memcmp(code, kMsgEUnknown, 4) == 0) {
        LOG((CLOG_ERR "server refused client with name \"%s\"", m_client->getName().c_str()));
        m_client->disconnect("server refused client with our name");
        return kDisconnect;
    }

    else if (memcmp(code, kMsgEBad, 4) == 0) {
        LOG((CLOG_ERR "server disconnected due to a protocol error"));
        m_client->disconnect("server reported a protocol error");
        return kDisconnect;
    }
    else {
        return kUnknown;
    }

    return kOkay;
}

ServerProxy::EResult
ServerProxy::parseMessage(const UInt8* code)
{
    if (m_protocolMinorVersion >= 8 &&
        memcmp(code, kMsgDMouseMove1_8, 4) == 0) {
        mouseMove1_8();
    }

    else if (m_protocolMinorVersion >= 8 &&
             memcmp(code, kMsgDMouseRelMove1_8, 4) == 0) {
        mouseRelativeMove1_8();
    }

    else if (m_protocolMinorVersion >= 8 &&
             memcmp(code, kMsgDMouseWheel1_8, 4) == 0) {
        mouseWheel1_8();
    }

    else if (m_protocolMinorVersion >= 8 &&
             memcmp(code, kMsgDKeyDown1_8, 4) == 0) {
        keyDown1_8();
    }

    else if (m_protocolMinorVersion >= 8 &&
             memcmp(code, kMsgDKeyUp1_8, 4) == 0) {
        keyUp1_8();
    }

    else if (m_protocolMinorVersion >= 8 &&
             memcmp(code, kMsgDMouseDown1_8, 4) == 0) {
        mouseDown1_8();
    }

    else if (m_protocolMinorVersion >= 8 &&
             memcmp(code, kMsgDMouseUp1_8, 4) == 0) {
        mouseUp1_8();
    }

    else if (m_protocolMinorVersion >= 8 &&
             memcmp(code, kMsgDKeyRepeat1_8, 4) == 0) {
        keyRepeat1_8();
    }

    else if (m_protocolMinorVersion < 8 &&
             memcmp(code, kMsgDMouseMove, 4) == 0) {
        mouseMove();
    }

    else if (m_protocolMinorVersion < 8 &&
             memcmp(code, kMsgDMouseRelMove, 4) == 0) {
        mouseRelativeMove();
    }

    else if (m_protocolMinorVersion < 8 &&
             memcmp(code, kMsgDMouseWheel, 4) == 0) {
        mouseWheel();
    }

    else if (m_protocolMinorVersion < 8 &&
             memcmp(code, kMsgDKeyDown, 4) == 0) {
        keyDown();
    }

    else if (m_protocolMinorVersion < 8 &&
             memcmp(code, kMsgDKeyUp, 4) == 0) {
        keyUp();
    }

    else if (m_protocolMinorVersion < 8 &&
             memcmp(code, kMsgDMouseDown, 4) == 0) {
        mouseDown();
    }

    else if (m_protocolMinorVersion < 8 &&
             memcmp(code, kMsgDMouseUp, 4) == 0) {
        mouseUp();
    }

    else if (m_protocolMinorVersion < 8 &&
             memcmp(code, kMsgDKeyRepeat, 4) == 0) {
        keyRepeat();
    }

    else if (memcmp(code, kMsgCKeepAlive, 4) == 0) {
        // echo keep alives and reset alarm
        ProtocolUtil::writef(m_stream, kMsgCKeepAlive);
        m_keepAliveAlarmDeferrals = 0;
        m_keepAliveMissedAlarms = 0;
        resetKeepAliveAlarm();
    }

    else if (memcmp(code, kMsgCNoop, 4) == 0) {
        // accept and discard no-op
    }

    else if (memcmp(code, kMsgCEnter, 4) == 0) {
        if (!enter()) {
            return kDisconnect;
        }
    }

    else if (m_protocolMinorVersion >= 7 &&
             memcmp(code, kMsgCPrepareEnter, 4) == 0) {
        prepareEnter();
    }

    else if (m_protocolMinorVersion >= 7 &&
             memcmp(code, kMsgCAbortEnter, 4) == 0) {
        abortEnter();
    }

    else if (m_protocolMinorVersion >= 11 &&
             memcmp(code, kMsgCRevokeInput, 4) == 0) {
        revokeInputLeaseRequest();
    }

    else if (memcmp(code, kMsgCLeave, 4) == 0) {
        leave();
    }

    else if (memcmp(code, kMsgCClipboard, 4) == 0) {
        grabClipboard();
    }

    else if (memcmp(code, kMsgCScreenSaver, 4) == 0) {
        screensaver();
    }

    else if (memcmp(code, kMsgQInfo, 4) == 0) {
        queryInfo();
    }

    else if (memcmp(code, kMsgCInfoAck, 4) == 0) {
        infoAcknowledgment();
    }

    else if (memcmp(code, kMsgDClipboard, 4) == 0) {
        setClipboard();
    }

    else if (m_protocolMinorVersion >= 12 &&
             memcmp(code, kMsgCBulkOffer1_12, 4) == 0) {
        if (!bulkOffer1_12()) {
            m_client->disconnect("invalid connection-bound bulk offer");
            return kDisconnect;
        }
    }

    else if (m_protocolMinorVersion >= 9 && m_protocolMinorVersion < 12 &&
             memcmp(code, kMsgCBulkOffer, 4) == 0) {
        bulkOffer();
    }

    else if (memcmp(code, kMsgCResetOptions, 4) == 0) {
        resetOptions();
    }

    else if (memcmp(code, kMsgDSetOptions, 4) == 0) {
        setOptions();
    }

    else if (m_protocolMinorVersion >= 12 &&
             isTransactionalFileControlCode(code)) {
        const EResult result = transactionalControlFrame(code);
        if (result != kOkay) {
            return result;
        }
    }
    else if (m_protocolMinorVersion >= 12 &&
             isTransactionalFileBulkCode(code)) {
        LOG((CLOG_WARN
            "rejecting transactional bulk payload on the control route"));
        return kDisconnect;
    }
    else if (m_protocolMinorVersion < 12 &&
             memcmp(code, kMsgDFileTransfer, 4) == 0) {
        if (!discardLegacyFileChunk(m_stream)) {
            m_client->disconnect("invalid file transfer on control connection");
            return kDisconnect;
        }
    }
    else if (memcmp(code, kMsgDDragInfo, 4) == 0) {
        if (m_protocolMinorVersion < 12) {
            if (!discardLegacyDragInfo(m_stream)) {
                m_client->disconnect("invalid drag metadata on control connection");
                return kDisconnect;
            }
        }
        else {
            dragInfoReceived();
        }
    }

    else if (memcmp(code, kMsgCClose, 4) == 0) {
        // server wants us to hangup
        LOG((CLOG_DEBUG1 "recv close"));
        m_client->disconnect(NULL);
        return kDisconnect;
    }
    else if (memcmp(code, kMsgEBad, 4) == 0) {
        LOG((CLOG_ERR "server disconnected due to a protocol error"));
        m_client->disconnect("server reported a protocol error");
        return kDisconnect;
    }
    else {
        return kUnknown;
    }

    // send a reply.  this is intended to work around a delay when
    // running a linux server and an OS X (any BSD?) client.  the
    // client waits to send an ACK (if the system control flag
    // net.inet.tcp.delayed_ack is 1) in hopes of piggybacking it
    // on a data packet.  we provide that packet here.  i don't
    // know why a delayed ACK should cause the server to wait since
    // TCP_NODELAY is enabled.
    ProtocolUtil::writef(m_stream, kMsgCNoop);

    return kOkay;
}

bool
ServerProxy::shouldDeferKeepAliveAlarm(bool hasPendingInput, UInt32 bufferedOutput) const
{
    return hasPendingInput || bufferedOutput > 0;
}

void
ServerProxy::keepAlive()
{
    ProtocolUtil::writef(m_stream, kMsgCKeepAlive);
}

void
ServerProxy::handleKeepAliveAlarm(const Event&, void*)
{
    const bool hasPendingInput = m_stream->isReady();
    const UInt32 bufferedOutput = m_stream->getBufferedOutputSize();
    if ((hasPendingInput && !m_lastKeepAlivePendingInput) ||
        (m_lastKeepAliveBufferedOutput > 0 &&
         bufferedOutput < m_lastKeepAliveBufferedOutput)) {
        m_keepAliveAlarmDeferrals = 0;
        m_keepAliveMissedAlarms = 0;
    }
    m_lastKeepAlivePendingInput = hasPendingInput;
    m_lastKeepAliveBufferedOutput = bufferedOutput;

    if (shouldDeferKeepAliveAlarm(hasPendingInput, bufferedOutput)) {
        if (m_keepAliveAlarmDeferrals >= kMaxKeepAliveAlarmDeferrals) {
            LOG((CLOG_NOTE
                 "server keepalive stalled with pending work; disconnecting, "
                 "pendingInput=%d bufferedOutput=%u idle=%.3fs",
                 hasPendingInput ? 1 : 0,
                 bufferedOutput,
                 m_keepAliveActivityTimer.getTime()));
            m_client->disconnect("server is not making progress");
            return;
        }

        ++m_keepAliveAlarmDeferrals;
        m_keepAliveMissedAlarms = 0;
        LOG((CLOG_WARN
             "server keepalive delayed while stream has pending work; deferring disconnect (%u/%u), "
             "pendingInput=%d bufferedOutput=%u idle=%.3fs",
             m_keepAliveAlarmDeferrals,
             kMaxKeepAliveAlarmDeferrals,
             hasPendingInput ? 1 : 0,
             bufferedOutput,
             m_keepAliveActivityTimer.getTime()));
        resetKeepAliveAlarm();
        return;
    }

    if (m_keepAliveAlarm > 0.0 &&
        m_keepAliveActivityTimer.getTime() < m_keepAliveAlarm) {
        m_keepAliveMissedAlarms = 0;
        resetKeepAliveAlarm();
        return;
    }

    static const UInt32 kMaxMissedKeepAlivesBeforeDisconnect = 3;
    if (m_keepAliveMissedAlarms < kMaxMissedKeepAlivesBeforeDisconnect) {
        ++m_keepAliveMissedAlarms;
        LOG((CLOG_WARN
             "server keepalive missed; probing before disconnect (%u/%u), idle=%.3fs",
             m_keepAliveMissedAlarms,
             kMaxMissedKeepAlivesBeforeDisconnect,
             m_keepAliveActivityTimer.getTime()));
        keepAlive();
        resetKeepAliveAlarm();
        return;
    }

    LOG((CLOG_NOTE "server is dead"));
    m_client->disconnect("server is not responding");
}

void
ServerProxy::handleKeepAliveEvent(const Event&, void*)
{
    keepAlive();
}

void
ServerProxy::onInfoChanged()
{
    // ignore mouse motion until we receive acknowledgment of our info
    // change message.
    m_ignoreMouse = true;
    m_infoAckTimer.start();
    m_infoAckTimer.reset();

    // send info update
    queryInfo();
}

void
ServerProxy::clearStaleInfoAckGate()
{
    if (m_ignoreMouse && m_infoAckTimer.getTime() > 2.0) {
        LOG((CLOG_WARN "clearing stale mouse gate after missing info acknowledgment"));
        m_ignoreMouse = false;
        queryInfo();
    }
}

bool
ServerProxy::onGrabClipboard(ClipboardID id)
{
    LOG((CLOG_DEBUG1 "sending clipboard %d changed", id));
    ProtocolUtil::writef(m_stream, kMsgCClipboard, id, m_seqNum);
    return true;
}

ServerProxy::ClipboardSendResult
ServerProxy::onClipboardChanged(ClipboardID id, const IClipboard* clipboard)
{
    if (clipboard == NULL) {
        return kClipboardSendFailed;
    }
    if (id == kClipboardClipboard && m_protocolMinorVersion < 12 &&
        RemoteFileClipboard::containsFileList(*clipboard)) {
        LOG((CLOG_WARN
            "not sending file clipboard metadata to a legacy server"));
        return kClipboardSendFailed;
    }

    // Do not marshal a potentially large clipboard when the previous sender
    // cannot yet be reaped. The immutable snapshot overload performs the same
    // gate before touching its payload.
    if (!cleanupClipboardSendThread(true)) {
        LOG((CLOG_WARN
            "clipboard %d send skipped because previous sender is still stopping",
            id));
        return kClipboardSendFailed;
    }

    const std::shared_ptr<const std::string> data(
        new std::string(IClipboard::marshall(clipboard)));
    return onClipboardDataChanged(
        id, data,
        id == kClipboardClipboard &&
            RemoteFileClipboard::containsFileList(*clipboard));
}

ServerProxy::ClipboardSendResult
ServerProxy::onClipboardDataChanged(
    ClipboardID id, const std::shared_ptr<const std::string>& data,
    bool needsOrderedBulkRoute)
{
    LOG((CLOG_DEBUG "sending clipboard %d seqnum=%d", id, m_seqNum));

    if (!data) {
        return kClipboardSendFailed;
    }
    if (m_protocolMinorVersion < 12 && needsOrderedBulkRoute) {
        LOG((CLOG_WARN
            "not sending ordered file clipboard metadata to a legacy server"));
        return kClipboardSendFailed;
    }

    if (!cleanupClipboardSendThread(true)) {
        LOG((CLOG_WARN "clipboard %d send skipped because previous sender is still stopping", id));
        return kClipboardSendFailed;
    }

    const bool requiresBulk =
        data->size() > kSynchronousClipboardSendLimit || needsOrderedBulkRoute;
    m_clipboardBulkChannel = requiresBulk ? m_client->acquireBulkChannel() :
        std::shared_ptr<barrier::BulkChannel>();
    if (requiresBulk && m_protocolMinorVersion >= 9 &&
        !m_clipboardBulkChannel) {
        m_clipboardSendStream = m_stream;
        LOG((CLOG_WARN
            "clipboard %d deferred because the required bulk channel is unavailable",
            id));
        return kClipboardSendFailed;
    }
    m_clipboardSendStream = m_clipboardBulkChannel ?
        m_clipboardBulkChannel->getStream() : m_stream;

    ++m_nextClipboardSendAttempt;
    if (m_nextClipboardSendAttempt == 0) {
        ++m_nextClipboardSendAttempt;
    }
    std::shared_ptr<barrier::ClipboardSendAttempt> attempt(
        new barrier::ClipboardSendAttempt(id, m_nextClipboardSendAttempt));
    m_latestClipboardSendAttempt[id] = m_nextClipboardSendAttempt;

    if (data->size() <= kSynchronousClipboardSendLimit) {
        const bool sent = StreamChunker::sendClipboard(
            *data, data->size(), id, m_seqNum, m_events, this,
            m_clipboardSendStream, m_clipboardBulkChannel, attempt);
        if (!sent) {
            LOG((CLOG_WARN "clipboard %d was not fully queued for sending", id));
        }
        m_clipboardBulkChannel.reset();
        m_clipboardSendStream = m_stream;
        return sent ? kClipboardSendQueued : kClipboardSendFailed;
    }

    std::shared_ptr<StreamChunker> chunker = std::make_shared<StreamChunker>();
    m_clipboardChunker = chunker;
    const UInt32 sequence = m_seqNum;
    m_clipboardSendId = id;
    m_clipboardSendSucceeded = false;
    m_clipboardSendResultAvailable = false;
    m_clipboardSendAttempt = attempt;
    m_clipboardSendThread = new Thread([this, data, id, sequence, chunker]() {
        sendClipboardThread(data, id, sequence, chunker);
    });
    return kClipboardSendPending;
}

void
ServerProxy::sendClipboardThread(const std::shared_ptr<const std::string>& data,
                                 ClipboardID id,
                                 UInt32 sequence,
                                 const std::shared_ptr<StreamChunker>& chunker)
{
    const bool sent = chunker->sendClipboardData(
            *data, data->size(), id, sequence, m_events, this,
            m_clipboardSendStream, m_clipboardBulkChannel,
            m_clipboardSendAttempt);
    m_clipboardSendSucceeded = sent &&
        (!m_clipboardSendAttempt || !m_clipboardSendAttempt->failed());
    m_clipboardSendResultAvailable = true;
    if (!sent) {
        LOG((CLOG_WARN "clipboard %d was not fully queued for sending", id));
    }
}

bool
ServerProxy::reapClipboardSendResult(ClipboardID id, bool& succeeded)
{
    if (m_clipboardSendThread == NULL) {
        return false;
    }
    if (!m_clipboardSendThread->wait(0.0)) {
        return false;
    }

    delete m_clipboardSendThread;
    m_clipboardSendThread = NULL;
    m_clipboardChunker.reset();

    succeeded = m_clipboardSendResultAvailable &&
        m_clipboardSendId == id &&
        m_clipboardSendSucceeded &&
        (!m_clipboardSendAttempt || !m_clipboardSendAttempt->failed());
    m_clipboardSendId = kClipboardEnd;
    m_clipboardSendSucceeded = false;
    m_clipboardSendResultAvailable = false;
    m_clipboardSendAttempt.reset();
    return true;
}

bool
ServerProxy::cleanupClipboardSendThread(bool cancel)
{
    if (cancel && m_clipboardChunker) {
        m_clipboardChunker->interruptFile();
    }

    if (m_clipboardSendThread != NULL) {
        if (!m_clipboardSendThread->wait(0.0)) {
            if (cancel) {
                LOG((CLOG_DEBUG "requesting asynchronous clipboard sender cancellation"));
                m_clipboardSendThread->cancel();
                m_clipboardSendThread->unblockPollSocket();
            }
            return false;
        }
        delete m_clipboardSendThread;
        m_clipboardSendThread = NULL;
    }

    m_clipboardChunker.reset();
    m_clipboardSendAttempt.reset();
    m_clipboardSendId = kClipboardEnd;
    m_clipboardSendSucceeded = false;
    m_clipboardSendResultAvailable = false;
    m_clipboardBulkChannel.reset();
    m_clipboardSendStream = m_stream;
    return true;
}

void
ServerProxy::handleBulkDisconnected(barrier::BulkChannel* channel)
{
    if (m_clipboardBulkChannel &&
        m_clipboardBulkChannel.get() == channel && m_clipboardChunker) {
        m_clipboardChunker->interruptFile();
    }
    if (m_fileTransferReceiveBulkChannel &&
        m_fileTransferReceiveBulkChannel.get() == channel) {
        if (m_fileTransferReceiver) {
            m_fileTransferReceiver->abortActiveTransfer();
        }
        resetTransactionalFileReceive(true);
    }
}

void
ServerProxy::detachForDeferredCleanup()
{
    if (m_detachedForDeferredCleanup) {
        return;
    }

    resetTransactionalFileReceive(true);
    setKeepAliveRate(-1.0);
    m_events->removeHandler(m_events->forIStream().inputReady(),
                            m_stream->getEventTarget());
    m_events->removeHandler(m_events->forClipboard().clipboardSending(), this);
    m_events->removeHandler(m_events->forFile().keepAlive(), this);
    m_detachedForDeferredCleanup = true;
}

void
ServerProxy::flushCompressedMouse()
{
    if (!m_inputActive) {
        discardCompressedMouse();
        return;
    }

    if (m_compressMouse) {
        m_compressMouse = false;
        m_client->mouseMove(m_xMouse, m_yMouse);
    }
    if (m_compressMouseRelative) {
        m_compressMouseRelative = false;
        m_client->mouseRelativeMove(m_dxMouse, m_dyMouse);
        m_dxMouse = 0;
        m_dyMouse = 0;
    }
}

void
ServerProxy::discardCompressedMouse()
{
    m_compressMouse = false;
    m_compressMouseRelative = false;
    m_dxMouse = 0;
    m_dyMouse = 0;
}

bool
ServerProxy::hasActivePointerLease(const char* inputType) const
{
    if (m_inputActive) {
        return true;
    }

    LOG((CLOG_DEBUG1 "dropping %s outside the active enter sequence", inputType));
    return false;
}

bool
ServerProxy::shouldCompressMouseMoves() const
{
    // Callers also require a complete message to be waiting. Coalescing only
    // after input is already backlogged prevents motion floods from delaying
    // control messages in normal mode. Immediate-delivery modes already apply
    // their own pacing and must not lose another set of coordinates here.
    return !m_lowLatencyMode && !m_nestedRemoteMode;
}

void
ServerProxy::sendInfo(const ClientInfo& info)
{
    LOG((CLOG_DEBUG1 "sending info shape=%d,%d %dx%d", info.m_x, info.m_y, info.m_w, info.m_h));
    ProtocolUtil::writef(m_stream, kMsgDInfo,
                                info.m_x, info.m_y,
                                info.m_w, info.m_h, 0,
                                info.m_mx, info.m_my);
}

KeyID
ServerProxy::translateKey(KeyID id) const
{
    static const KeyID s_translationTable[kKeyModifierIDLast][2] = {
        { kKeyNone,      kKeyNone },
        { kKeyShift_L,   kKeyShift_R },
        { kKeyControl_L, kKeyControl_R },
        { kKeyAlt_L,     kKeyAlt_R },
        { kKeyMeta_L,    kKeyMeta_R },
        { kKeySuper_L,   kKeySuper_R },
        { kKeyAltGr,     kKeyAltGr}
    };

    KeyModifierID id2 = kKeyModifierIDNull;
    UInt32 side      = 0;
    switch (id) {
    case kKeyShift_L:
        id2  = kKeyModifierIDShift;
        side = 0;
        break;

    case kKeyShift_R:
        id2  = kKeyModifierIDShift;
        side = 1;
        break;

    case kKeyControl_L:
        id2  = kKeyModifierIDControl;
        side = 0;
        break;

    case kKeyControl_R:
        id2  = kKeyModifierIDControl;
        side = 1;
        break;

    case kKeyAlt_L:
        id2  = kKeyModifierIDAlt;
        side = 0;
        break;

    case kKeyAlt_R:
        id2  = kKeyModifierIDAlt;
        side = 1;
        break;

    case kKeyAltGr:
        id2 = kKeyModifierIDAltGr;
        side = 1; // there is only one alt gr key on the right side
        break;

    case kKeyMeta_L:
        id2  = kKeyModifierIDMeta;
        side = 0;
        break;

    case kKeyMeta_R:
        id2  = kKeyModifierIDMeta;
        side = 1;
        break;

    case kKeySuper_L:
        id2  = kKeyModifierIDSuper;
        side = 0;
        break;

    case kKeySuper_R:
        id2  = kKeyModifierIDSuper;
        side = 1;
        break;
    }

    if (id2 != kKeyModifierIDNull) {
        return s_translationTable[m_modifierTranslationTable[id2]][side];
    }
    else {
        return id;
    }
}

KeyModifierMask
ServerProxy::translateModifierMask(KeyModifierMask mask) const
{
    static const KeyModifierMask s_masks[kKeyModifierIDLast] = {
        0x0000,
        KeyModifierShift,
        KeyModifierControl,
        KeyModifierAlt,
        KeyModifierMeta,
        KeyModifierSuper,
        KeyModifierAltGr
    };

    KeyModifierMask newMask = mask & ~(KeyModifierShift |
                                        KeyModifierControl |
                                        KeyModifierAlt |
                                        KeyModifierMeta |
                                        KeyModifierSuper |
                                        KeyModifierAltGr );
    if ((mask & KeyModifierShift) != 0) {
        newMask |= s_masks[m_modifierTranslationTable[kKeyModifierIDShift]];
    }
    if ((mask & KeyModifierControl) != 0) {
        newMask |= s_masks[m_modifierTranslationTable[kKeyModifierIDControl]];
    }
    if ((mask & KeyModifierAlt) != 0) {
        newMask |= s_masks[m_modifierTranslationTable[kKeyModifierIDAlt]];
    }
    if ((mask & KeyModifierAltGr) != 0) {
        newMask |= s_masks[m_modifierTranslationTable[kKeyModifierIDAltGr]];
    }
    if ((mask & KeyModifierMeta) != 0) {
        newMask |= s_masks[m_modifierTranslationTable[kKeyModifierIDMeta]];
    }
    if ((mask & KeyModifierSuper) != 0) {
        newMask |= s_masks[m_modifierTranslationTable[kKeyModifierIDSuper]];
    }
    return newMask;
}

bool
ServerProxy::enter()
{
    // parse
    SInt16 x, y;
    UInt16 mask;
    UInt32 seqNum;
    ProtocolUtil::readf(m_stream, kMsgCEnter + 4, &x, &y, &seqNum, &mask);
    LOG((CLOG_DEBUG1 "recv enter, %d,%d %u %04x", x, y, seqNum, mask));

    if (m_hasEnterSequence && !isNewerInputSequence(seqNum, m_seqNum)) {
        LOG((CLOG_WARN "ignoring stale enter sequence %u; current=%u", seqNum, m_seqNum));
        return true;
    }

    if (m_hasPreparedEnter && seqNum != m_preparedEnterSequence) {
        LOG((CLOG_WARN "ignoring enter sequence %u while prepared sequence %u is pending",
            seqNum, m_preparedEnterSequence));
        return true;
    }
    if (m_hasPreparedEnter && !m_preparedEnterReady) {
        LOG((CLOG_WARN "ignoring rejected enter sequence %u", seqNum));
        return true;
    }
    if (m_hasPreparedEnter &&
        (m_client->inputHandoffGeneration() != m_preparedInputGeneration ||
         !m_client->canAcceptInputHandoff())) {
        LOG((CLOG_WARN
            "rejecting enter sequence %u because the prepared input backend changed",
            seqNum));
        m_hasPreparedEnter = false;
        m_preparedEnterReady = false;
        m_preparedInputGeneration = 0;
        ProtocolUtil::writef(m_stream, kMsgDEnterReady, seqNum,
                             static_cast<UInt8>(0));
        return true;
    }

    if (m_inputActive) {
        LOG((CLOG_WARN "replacing active input lease %u with %u", m_seqNum, seqNum));
        discardCompressedMouse();
        releaseEpochPressedInput();
        if (!m_client->leave()) {
            LOG((CLOG_ERR
                "input backend could not release active lease %u before enter %u",
                m_seqNum, seqNum));
            m_hasPreparedEnter = false;
            m_preparedEnterReady = false;
            m_preparedInputGeneration = 0;
            if (m_protocolMinorVersion >= 7) {
                ProtocolUtil::writef(m_stream, kMsgDEnterReady, seqNum,
                                     static_cast<UInt8>(0));
                return true;
            }
            m_client->disconnect("input backend could not replace active lease");
            return false;
        }
        m_inputActive = false;
    }

    // discard old compressed mouse motion, if any
    discardCompressedMouse();
    m_seqNum                = seqNum;
    m_hasEnterSequence      = true;
    m_ignoreMouse           = false;
    m_hasPreparedEnter      = false;
    m_preparedEnterReady    = false;
    m_preparedInputGeneration = 0;
    m_lastInputSequence     = 0;
    m_hasInputSequence      = false;

    // Do not publish the lease until the platform input backend confirms that
    // it accepted the commit.  A failed Windows desktop command must roll the
    // server handoff back instead of leaving both peers without the pointer.
    m_inputActive = m_client->enterInputLease(
        x, y, seqNum, static_cast<KeyModifierMask>(mask), false);
    if (!m_inputActive) {
        LOG((CLOG_ERR "input backend rejected committed enter sequence %u", seqNum));
        if (m_protocolMinorVersion >= 7) {
            ProtocolUtil::writef(m_stream, kMsgDEnterReady, seqNum,
                                 static_cast<UInt8>(0));
        }
        else {
            m_client->disconnect("input backend rejected screen enter");
            return false;
        }
    }
    else {
        LOG((CLOG_INFO "input backend committed enter sequence %u", seqNum));
        if (m_protocolMinorVersion >= 10) {
            ProtocolUtil::writef(m_stream, kMsgDEnterReady, seqNum,
                                 static_cast<UInt8>(1));
        }
    }
    return true;
}

void
ServerProxy::prepareEnter()
{
    SInt16 x = 0;
    SInt16 y = 0;
    UInt16 mask = 0;
    UInt32 seqNum = 0;
    ProtocolUtil::readf(m_stream, kMsgCPrepareEnter + 4,
                        &x, &y, &seqNum, &mask);

    const bool duplicatePrepare =
        m_hasPreparedEnter && seqNum == m_preparedEnterSequence;
    const bool sequenceAcceptable = duplicatePrepare ||
        !m_hasEnterSequence || isNewerInputSequence(seqNum, m_seqNum);
    const bool ready = !m_inputActive && sequenceAcceptable &&
        m_client->canAcceptInputHandoff();

    m_preparedEnterSequence = seqNum;
    m_hasPreparedEnter = true;
    m_preparedEnterReady = ready;
    m_preparedInputGeneration = ready ?
        m_client->inputHandoffGeneration() : 0;

    LOG((CLOG_DEBUG1 "recv prepare enter, %d,%d %u %04x ready=%d",
        x, y, seqNum, mask, ready ? 1 : 0));
    ProtocolUtil::writef(m_stream, kMsgDEnterReady, seqNum,
                         static_cast<UInt8>(ready ? 1 : 0));
}

void
ServerProxy::abortEnter()
{
    UInt32 seqNum = 0;
    ProtocolUtil::readf(m_stream, kMsgCAbortEnter + 4, &seqNum);
    if (m_hasPreparedEnter && seqNum == m_preparedEnterSequence) {
        LOG((CLOG_DEBUG1 "recv abort prepared enter %u", seqNum));
        m_hasPreparedEnter = false;
        m_preparedEnterReady = false;
        m_preparedInputGeneration = 0;
    }
}

void
ServerProxy::revokeInputLeaseRequest()
{
    UInt32 handoffSeqNum = 0;
    UInt32 inputEpoch = 0;
    ProtocolUtil::readf(m_stream, kMsgCRevokeInput + 4,
                        &handoffSeqNum, &inputEpoch);

    bool revoked = false;
    if (!m_hasEnterSequence || inputEpoch != m_seqNum) {
        LOG((CLOG_WARN
            "rejecting stale input lease revoke, handoff=%u epoch=%u active=%u",
            handoffSeqNum, inputEpoch, m_seqNum));
    }
    else if (!m_inputActive) {
        discardCompressedMouse();
        revoked = true;
        LOG((CLOG_DEBUG1
            "acknowledging already inactive input lease, handoff=%u epoch=%u",
            handoffSeqNum, inputEpoch));
    }
    else {
        flushCompressedMouse();
        releaseEpochPressedInput();
        if (m_client->leave()) {
            m_inputActive = false;
            m_hasPreparedEnter = false;
            m_preparedEnterReady = false;
            m_preparedInputGeneration = 0;
            revoked = true;
            LOG((CLOG_INFO
                "input backend revoked source lease, handoff=%u epoch=%u",
                handoffSeqNum, inputEpoch));
        }
        else {
            LOG((CLOG_ERR
                "input backend rejected source lease revoke, handoff=%u epoch=%u",
                handoffSeqNum, inputEpoch));
        }
    }

    ProtocolUtil::writef(m_stream, kMsgDRevokeInputAck,
                         handoffSeqNum,
                         static_cast<UInt8>(revoked ? 1 : 0));
}

void
ServerProxy::leave()
{
    // parse
    LOG((CLOG_DEBUG1 "recv leave"));

    if (!m_inputActive) {
        discardCompressedMouse();
        LOG((CLOG_DEBUG1 "ignoring leave without an active input lease"));
        return;
    }

    // send last mouse motion
    flushCompressedMouse();
    releaseEpochPressedInput();

    // Keep the proxy and client ownership state aligned. COUT has no response,
    // so a failed platform leave must tear down the connection and let the
    // supervisor reclaim the input backend instead of silently losing it.
    if (!m_client->leave()) {
        LOG((CLOG_ERR
            "input backend could not release active lease %u; disconnecting",
            m_seqNum));
        m_client->disconnect("input backend rejected screen leave");
        return;
    }
    m_inputActive = false;
    m_hasPreparedEnter = false;
    m_preparedEnterReady = false;
    m_preparedInputGeneration = 0;
}

void
ServerProxy::setClipboard()
{
    setClipboard(m_stream);
}

void
ServerProxy::setClipboard(barrier::IStream* stream)
{
    // parse
    ClipboardID id;
    UInt32 seq;

    int r = ClipboardChunk::assemble(stream, m_clipboardReceiveBuffer, id, seq);

    if (r == kStart) {
        size_t size = m_clipboardReceiveBuffer.expectedSize;
        LOG((CLOG_DEBUG "receiving clipboard %d size=%d", id, size));
    }
    else if (r == kFinish) {
        LOG((CLOG_DEBUG "received clipboard %d size=%d", id, m_clipboardReceiveBuffer.data.size()));

        // Transfer ownership of the validated wire buffer. Windows can publish
        // it on its clipboard worker without parsing or image conversion on
        // the input/control event thread.
        std::shared_ptr<String> snapshot(new String());
        snapshot->swap(m_clipboardReceiveBuffer.data);
        m_clipboardReceiveBuffer.release();
        if (!m_client->setClipboardData(id, snapshot)) {
            LOG((CLOG_WARN "clipboard %d snapshot was rejected", id));
            return;
        }

        LOG((CLOG_INFO "clipboard snapshot was queued for publication"));
    }
}

void
ServerProxy::grabClipboard()
{
    // parse
    ClipboardID id;
    UInt32 seqNum;
    ProtocolUtil::readf(m_stream, kMsgCClipboard + 4, &id, &seqNum);
    LOG((CLOG_DEBUG "recv grab clipboard %d", id));

    // validate
    if (id >= kClipboardEnd) {
        return;
    }

    // forward
    m_client->grabClipboard(id);
}

void
ServerProxy::keyDown()
{
    // get mouse up to date
    flushCompressedMouse();

    // parse
    UInt16 id, mask, button;
    ProtocolUtil::readf(m_stream, kMsgDKeyDown + 4, &id, &mask, &button);
    LOG((CLOG_DEBUG1 "recv key down id=0x%08x, mask=0x%04x, button=0x%04x", id, mask, button));

    // translate
    KeyID id2             = translateKey(static_cast<KeyID>(id));
    KeyModifierMask mask2 = translateModifierMask(
                                static_cast<KeyModifierMask>(mask));
    if (id2   != static_cast<KeyID>(id) ||
        mask2 != static_cast<KeyModifierMask>(mask))
        LOG((CLOG_DEBUG1 "key down translated to id=0x%08x, mask=0x%04x", id2, mask2));

    // forward
    // Keyboard broadcast intentionally targets inactive screens.  The 1.6
    // protocol has no broadcast marker, so keyboard events cannot use the
    // pointer lease gate without breaking that feature.
    if (m_inputFrameAccepted) {
        m_client->keyDown(id2, mask2, button);
        if (m_inputFrameHasEpoch && !m_inputFrameBroadcast) {
            m_epochPressedKeys[button] = PressedKey(id2, mask2);
        }
    }
}

void
ServerProxy::keyRepeat()
{
    // get mouse up to date
    flushCompressedMouse();

    // parse
    UInt16 id, mask, count, button;
    ProtocolUtil::readf(m_stream, kMsgDKeyRepeat + 4,
                                &id, &mask, &count, &button);
    LOG((CLOG_DEBUG1 "recv key repeat id=0x%08x, mask=0x%04x, count=%d, button=0x%04x", id, mask, count, button));

    // translate
    KeyID id2             = translateKey(static_cast<KeyID>(id));
    KeyModifierMask mask2 = translateModifierMask(
                                static_cast<KeyModifierMask>(mask));
    if (id2   != static_cast<KeyID>(id) ||
        mask2 != static_cast<KeyModifierMask>(mask))
        LOG((CLOG_DEBUG1 "key repeat translated to id=0x%08x, mask=0x%04x", id2, mask2));

    // forward
    if (m_inputFrameAccepted) {
        m_client->keyRepeat(id2, mask2, count, button);
    }
}

void
ServerProxy::keyUp()
{
    // get mouse up to date
    flushCompressedMouse();

    // parse
    UInt16 id, mask, button;
    ProtocolUtil::readf(m_stream, kMsgDKeyUp + 4, &id, &mask, &button);
    LOG((CLOG_DEBUG1 "recv key up id=0x%08x, mask=0x%04x, button=0x%04x", id, mask, button));

    // translate
    KeyID id2             = translateKey(static_cast<KeyID>(id));
    KeyModifierMask mask2 = translateModifierMask(
                                static_cast<KeyModifierMask>(mask));
    if (id2   != static_cast<KeyID>(id) ||
        mask2 != static_cast<KeyModifierMask>(mask))
        LOG((CLOG_DEBUG1 "key up translated to id=0x%08x, mask=0x%04x", id2, mask2));

    // forward
    if (m_inputFrameAccepted) {
        m_client->keyUp(id2, mask2, button);
        if (m_inputFrameHasEpoch && !m_inputFrameBroadcast) {
            m_epochPressedKeys.erase(button);
        }
    }
}

void
ServerProxy::mouseDown()
{
    // get mouse up to date
    flushCompressedMouse();

    // parse
    SInt8 id;
    ProtocolUtil::readf(m_stream, kMsgDMouseDown + 4, &id);
    LOG((CLOG_DEBUG1 "recv mouse down id=%d", id));

    // forward
    if (m_inputFrameAccepted && hasActivePointerLease("mouse down")) {
        m_client->mouseDown(static_cast<ButtonID>(id));
        if (m_inputFrameHasEpoch) {
            m_epochPressedButtons.insert(static_cast<ButtonID>(id));
        }
    }
}

void
ServerProxy::mouseUp()
{
    // get mouse up to date
    flushCompressedMouse();

    // parse
    SInt8 id;
    ProtocolUtil::readf(m_stream, kMsgDMouseUp + 4, &id);
    LOG((CLOG_DEBUG1 "recv mouse up id=%d", id));

    // forward
    if (m_inputFrameAccepted && hasActivePointerLease("mouse up")) {
        m_client->mouseUp(static_cast<ButtonID>(id));
        if (m_inputFrameHasEpoch) {
            m_epochPressedButtons.erase(static_cast<ButtonID>(id));
        }
    }
}

void
ServerProxy::mouseMove()
{
    // parse
    bool ignore;
    SInt16 x, y;
    ProtocolUtil::readf(m_stream, kMsgDMouseMove + 4, &x, &y);

    // note if we should ignore the move
    clearStaleInfoAckGate();
    ignore = m_ignoreMouse || !m_inputFrameAccepted ||
        !hasActivePointerLease("mouse move");

    // compress mouse motion events if more input follows
    if (!ignore && shouldCompressMouseMoves() &&
        !m_compressMouse && m_stream->isReady()) {
        m_compressMouse = true;
    }

    // if compressing then ignore the motion but record it
    if (m_compressMouse && m_inputFrameAccepted) {
        m_compressMouseRelative = false;
        ignore    = true;
        m_xMouse  = x;
        m_yMouse  = y;
        m_dxMouse = 0;
        m_dyMouse = 0;
    }
    LOG((CLOG_DEBUG2 "recv mouse move %d,%d", x, y));

    // forward
    if (!ignore) {
        m_client->mouseMove(x, y);
    }
}

void
ServerProxy::mouseRelativeMove()
{
    // parse
    bool ignore;
    SInt16 dx, dy;
    ProtocolUtil::readf(m_stream, kMsgDMouseRelMove + 4, &dx, &dy);

    // note if we should ignore the move
    clearStaleInfoAckGate();
    ignore = m_ignoreMouse || !m_inputFrameAccepted ||
        !hasActivePointerLease("relative mouse move");

    // compress mouse motion events if more input follows
    if (!ignore && shouldCompressMouseMoves() &&
        !m_compressMouseRelative && m_stream->isReady()) {
        m_compressMouseRelative = true;
    }

    // if compressing then ignore the motion but record it
    if (m_compressMouseRelative && m_inputFrameAccepted) {
        ignore     = true;
        m_dxMouse += dx;
        m_dyMouse += dy;
    }
    LOG((CLOG_DEBUG2 "recv mouse relative move %d,%d", dx, dy));

    // forward
    if (!ignore) {
        m_client->mouseRelativeMove(dx, dy);
    }
}

void
ServerProxy::mouseWheel()
{
    // get mouse up to date
    flushCompressedMouse();

    // parse
    SInt16 xDelta, yDelta;
    ProtocolUtil::readf(m_stream, kMsgDMouseWheel + 4, &xDelta, &yDelta);
    LOG((CLOG_DEBUG2 "recv mouse wheel %+d,%+d", xDelta, yDelta));

    // forward
    if (m_inputFrameAccepted && hasActivePointerLease("mouse wheel")) {
        m_client->mouseWheel(xDelta, yDelta);
    }
}

bool
ServerProxy::acceptEpochInput(UInt32 epoch, UInt32 sequence, UInt8 flags,
                              const char* inputType)
{
    if ((flags & ~static_cast<UInt8>(kInputMessageBroadcast)) != 0) {
        LOG((CLOG_WARN "dropping %s with invalid input flags 0x%02x",
            inputType, flags));
        return false;
    }

    const bool broadcast = (flags & kInputMessageBroadcast) != 0;
    const bool epochMatches =
        (m_hasEnterSequence && epoch == m_seqNum) ||
        (!m_hasEnterSequence && broadcast && epoch == 0);
    if (!epochMatches) {
        LOG((CLOG_DEBUG1 "dropping %s for stale input epoch %u; current=%u",
            inputType, epoch, m_seqNum));
        return false;
    }
    if (!broadcast && !m_inputActive) {
        LOG((CLOG_DEBUG1 "dropping %s without an active input lease",
            inputType));
        return false;
    }
    if (m_hasInputSequence &&
        !isNewerInputSequence(sequence, m_lastInputSequence)) {
        LOG((CLOG_DEBUG1 "dropping replayed %s sequence %u; current=%u",
            inputType, sequence, m_lastInputSequence));
        return false;
    }

    m_lastInputSequence = sequence;
    m_hasInputSequence = true;
    return true;
}

void
ServerProxy::dispatchEpochInput(UInt32 epoch, UInt32 sequence, UInt8 flags,
                                const char* inputType,
                                InputPayloadHandler handler)
{
    const bool previousAccepted = m_inputFrameAccepted;
    const bool previousBroadcast = m_inputFrameBroadcast;
    const bool previousHasEpoch = m_inputFrameHasEpoch;
    m_inputFrameAccepted = acceptEpochInput(epoch, sequence, flags, inputType);
    m_inputFrameBroadcast =
        (flags & static_cast<UInt8>(kInputMessageBroadcast)) != 0;
    m_inputFrameHasEpoch = true;
    try {
        (this->*handler)();
    }
    catch (...) {
        m_inputFrameAccepted = previousAccepted;
        m_inputFrameBroadcast = previousBroadcast;
        m_inputFrameHasEpoch = previousHasEpoch;
        throw;
    }
    m_inputFrameAccepted = previousAccepted;
    m_inputFrameBroadcast = previousBroadcast;
    m_inputFrameHasEpoch = previousHasEpoch;
}

void
ServerProxy::releaseEpochPressedInput()
{
    if (m_epochPressedKeys.empty() && m_epochPressedButtons.empty()) {
        return;
    }

    std::map<KeyButton, PressedKey> keys;
    std::set<ButtonID> buttons;
    keys.swap(m_epochPressedKeys);
    buttons.swap(m_epochPressedButtons);

    LOG((CLOG_DEBUG1 "releasing %lu key(s) and %lu mouse button(s) for input epoch %u",
        static_cast<unsigned long>(keys.size()),
        static_cast<unsigned long>(buttons.size()), m_seqNum));
    for (std::set<ButtonID>::const_iterator i = buttons.begin();
         i != buttons.end(); ++i) {
        m_client->mouseUp(*i);
    }
    for (std::map<KeyButton, PressedKey>::const_iterator i = keys.begin();
         i != keys.end(); ++i) {
        m_client->keyUp(i->second.id, i->second.mask, i->first);
    }
}

void
ServerProxy::revokeInputLease()
{
    discardCompressedMouse();
    releaseEpochPressedInput();
    m_inputActive = false;
    m_hasPreparedEnter = false;
    m_preparedEnterReady = false;
    m_preparedInputGeneration = 0;
}

void
ServerProxy::keyDown1_8()
{
    UInt32 epoch = 0;
    UInt32 sequence = 0;
    UInt8 flags = 0;
    ProtocolUtil::readf(m_stream, "%4i%4i%1i", &epoch, &sequence, &flags);
    dispatchEpochInput(epoch, sequence, flags, "key down", &ServerProxy::keyDown);
}

void
ServerProxy::keyRepeat1_8()
{
    UInt32 epoch = 0;
    UInt32 sequence = 0;
    UInt8 flags = 0;
    ProtocolUtil::readf(m_stream, "%4i%4i%1i", &epoch, &sequence, &flags);
    dispatchEpochInput(epoch, sequence, flags, "key repeat", &ServerProxy::keyRepeat);
}

void
ServerProxy::keyUp1_8()
{
    UInt32 epoch = 0;
    UInt32 sequence = 0;
    UInt8 flags = 0;
    ProtocolUtil::readf(m_stream, "%4i%4i%1i", &epoch, &sequence, &flags);
    dispatchEpochInput(epoch, sequence, flags, "key up", &ServerProxy::keyUp);
}

void
ServerProxy::mouseDown1_8()
{
    UInt32 epoch = 0;
    UInt32 sequence = 0;
    ProtocolUtil::readf(m_stream, "%4i%4i", &epoch, &sequence);
    dispatchEpochInput(epoch, sequence, kInputMessageNoFlags,
                       "mouse down", &ServerProxy::mouseDown);
}

void
ServerProxy::mouseUp1_8()
{
    UInt32 epoch = 0;
    UInt32 sequence = 0;
    ProtocolUtil::readf(m_stream, "%4i%4i", &epoch, &sequence);
    dispatchEpochInput(epoch, sequence, kInputMessageNoFlags,
                       "mouse up", &ServerProxy::mouseUp);
}

void
ServerProxy::mouseMove1_8()
{
    UInt32 epoch = 0;
    UInt32 sequence = 0;
    ProtocolUtil::readf(m_stream, "%4i%4i", &epoch, &sequence);
    dispatchEpochInput(epoch, sequence, kInputMessageNoFlags,
                       "mouse move", &ServerProxy::mouseMove);
}

void
ServerProxy::mouseRelativeMove1_8()
{
    UInt32 epoch = 0;
    UInt32 sequence = 0;
    ProtocolUtil::readf(m_stream, "%4i%4i", &epoch, &sequence);
    dispatchEpochInput(epoch, sequence, kInputMessageNoFlags,
                       "relative mouse move", &ServerProxy::mouseRelativeMove);
}

void
ServerProxy::mouseWheel1_8()
{
    UInt32 epoch = 0;
    UInt32 sequence = 0;
    ProtocolUtil::readf(m_stream, "%4i%4i", &epoch, &sequence);
    dispatchEpochInput(epoch, sequence, kInputMessageNoFlags,
                       "mouse wheel", &ServerProxy::mouseWheel);
}

void
ServerProxy::screensaver()
{
    // parse
    SInt8 on;
    ProtocolUtil::readf(m_stream, kMsgCScreenSaver + 4, &on);
    LOG((CLOG_DEBUG1 "recv screen saver on=%d", on));

    // forward
    m_client->screensaver(on != 0);
}

void
ServerProxy::resetOptions()
{
    // parse
    LOG((CLOG_DEBUG1 "recv reset options"));

    // forward
    m_client->resetOptions();

    // reset keep alive
    setKeepAliveRate(kKeepAliveRate);

    // reset modifier translation table
    for (KeyModifierID id = 0; id < kKeyModifierIDLast; ++id) {
        m_modifierTranslationTable[id] = id;
    }

    m_lowLatencyMode = false;
    m_nestedRemoteMode = false;
}

void
ServerProxy::setOptions()
{
    // parse
    OptionsList options;
    if (!ProtocolUtil::readf(m_stream, kMsgDSetOptions + 4, &options)) {
        LOG((CLOG_ERR "invalid options from server: could not read options list"));
        m_client->disconnect("invalid options from server");
        return;
    }
    if (!hasCompleteOptionPairs(options)) {
        LOG((CLOG_ERR "invalid options from server: odd option list size=%d", options.size()));
        m_client->disconnect("invalid options from server");
        return;
    }
    LOG((CLOG_DEBUG1 "recv set options size=%d", options.size()));

    // forward
    m_client->setOptions(options);

    // update modifier table
    for (UInt32 i = 0, n = (UInt32)options.size(); i < n; i += 2) {
        KeyModifierID id = kKeyModifierIDNull;
        if (options[i] == kOptionModifierMapForShift) {
            id = kKeyModifierIDShift;
        }
        else if (options[i] == kOptionModifierMapForControl) {
            id = kKeyModifierIDControl;
        }
        else if (options[i] == kOptionModifierMapForAlt) {
            id = kKeyModifierIDAlt;
        }
        else if (options[i] == kOptionModifierMapForAltGr) {
            id = kKeyModifierIDAltGr;
        }
        else if (options[i] == kOptionModifierMapForMeta) {
            id = kKeyModifierIDMeta;
        }
        else if (options[i] == kOptionModifierMapForSuper) {
            id = kKeyModifierIDSuper;
        }
        else if (options[i] == kOptionHeartbeat) {
            // update keep alive
            setKeepAliveRate(1.0e-3 * static_cast<double>(options[i + 1]));
        }
        else if (options[i] == kOptionLowLatencyMode) {
            m_lowLatencyMode = (options[i + 1] != 0);
        }
        else if (options[i] == kOptionNestedRemoteMode) {
            m_nestedRemoteMode = (options[i + 1] != 0);
        }

        if (id != kKeyModifierIDNull) {
            m_modifierTranslationTable[id] =
                static_cast<KeyModifierID>(options[i + 1]);
            LOG((CLOG_DEBUG1 "modifier %d mapped to %d", id, m_modifierTranslationTable[id]));
        }
    }

    if (m_lowLatencyMode) {
        LOG((CLOG_NOTE "server requested low latency mode - immediate mouse delivery enabled while input is current"));
    }
    if (m_nestedRemoteMode) {
        LOG((CLOG_NOTE "server requested nested remote mode - immediate mouse delivery enabled while input is current"));
    }
}

bool
ServerProxy::hasCompleteOptionPairs(const OptionsList& options)
{
    return (options.size() % 2) == 0;
}

void
ServerProxy::queryInfo()
{
    ClientInfo info;
    m_client->getShape(info.m_x, info.m_y, info.m_w, info.m_h);
    m_client->getCursorPos(info.m_mx, info.m_my);
    sendInfo(info);
}

void
ServerProxy::infoAcknowledgment()
{
    LOG((CLOG_DEBUG1 "recv info acknowledgment"));
    m_ignoreMouse = false;
}

int
ServerProxy::fileChunkReceived()
{
    return fileChunkReceived(m_stream);
}

int
ServerProxy::fileChunkReceived(barrier::IStream* stream)
{
    int result = FileChunk::assemble(
                    stream,
                    m_client->getFileReceiveSession());

    if (result == kFinish) {
        FileReceiveSession& session = m_client->getFileReceiveSession();
        const std::uint64_t generation = session.generation();
        std::shared_ptr<barrier::BulkChannel> bulkChannel =
            m_client->acquireBulkChannel();
        if (bulkChannel && bulkChannel->getStream() == stream) {
            if (!bulkChannel->pauseInputForCommit(generation) ||
                !session.installCommitBarrier(
                    generation,
                    bulkChannel->makeInputResumeCallback(generation),
                    bulkChannel->makeInputProgressCallback(generation))) {
                bulkChannel->resumeInputAfterCommit(generation);
                LOG((CLOG_ERR
                    "failed to install bulk file receive commit barrier, generation=%llu",
                    static_cast<unsigned long long>(generation)));
                m_client->handleBulkInputPauseFailed(
                    bulkChannel.get(), generation);
                return kError;
            }
        }
        FileReceiveCompletionInfo* completionInfo = NULL;
        try {
            completionInfo = new FileReceiveCompletionInfo(generation);
            Event completed(
                m_events->forFile().fileRecieveCompleted(), m_client);
            completed.setDataObject(completionInfo);
            m_events->addEvent(completed);
            completionInfo = NULL;
        }
        catch (...) {
            delete completionInfo;
            session.fail();
            LOG((CLOG_ERR
                "failed to queue completed file receive, generation=%llu",
                static_cast<unsigned long long>(generation)));
            return kError;
        }
    }
    else if (result == kStart) {
        FileReceiveSession& session = m_client->getFileReceiveSession();
        if (stream == m_stream &&
            session.expectedSize() > FileChunk::kMemoryReceiveLimit) {
            LOG((CLOG_WARN
                "discarding legacy file transfer that requires disk spooling; "
                "bulk transport is required, size=%llu",
                static_cast<unsigned long long>(session.expectedSize())));
            session.discardRemaining();
            return kStart;
        }
        m_client->bindFileReceiveClipboardRevision();
        if (m_client->getDragFileList().size() > 0) {
            std::string filename = m_client->getDragFileList().at(0).getFilename();
            LOG((CLOG_DEBUG "start receiving %s", filename.c_str()));
        }
    }
    else if (result == kBackpressure) {
        FileReceiveSession& session = m_client->getFileReceiveSession();
        const std::uint64_t generation = session.generation();
        std::shared_ptr<barrier::BulkChannel> bulkChannel =
            m_client->acquireBulkChannel();
        if (bulkChannel && bulkChannel->getStream() == stream) {
            if (!bulkChannel->pauseInputForBackpressure(generation) ||
                !session.installBackpressureBarrier(
                    generation,
                    bulkChannel->makeInputResumeCallback(generation),
                    bulkChannel->makeInputProgressCallback(generation))) {
                bulkChannel->resumeInputAfterBackpressure(generation);
                session.fail();
                LOG((CLOG_ERR
                    "failed to install bulk receive backpressure barrier, generation=%llu",
                    static_cast<unsigned long long>(generation)));
                return kError;
            }
        }
        else {
            LOG((CLOG_WARN
                "discarding flow-controlled legacy file transfer while "
                "preserving the control connection"));
            session.discardRemaining();
        }
    }
    return result;
}

bool
ServerProxy::discardLegacyFileChunk(barrier::IStream* stream)
{
    UInt8 mark = 0;
    std::string content;
    if (stream == NULL ||
        !ProtocolUtil::readf(
            stream, kMsgDFileTransfer + 4, &mark, &content)) {
        LOG((CLOG_WARN "invalid legacy file payload from server"));
        return false;
    }
    LOG((CLOG_WARN
        "ignored legacy file payload from server; protocol 1.12 is required"));
    return true;
}

bool
ServerProxy::discardLegacyDragInfo(barrier::IStream* stream)
{
    UInt32 fileCount = 0;
    std::string content;
    if (stream == NULL ||
        !ProtocolUtil::readf(
            stream, kMsgDDragInfo + 4, &fileCount, &content)) {
        LOG((CLOG_WARN "invalid legacy drag metadata from server"));
        return false;
    }
    LOG((CLOG_WARN
        "ignored legacy drag metadata from server; protocol 1.12 is required"));
    return true;
}

void
ServerProxy::bulkOffer()
{
    std::string token;
    if (ProtocolUtil::readf(m_stream, kMsgCBulkOffer + 4, &token) &&
        m_protocolMinorVersion >= 9 && m_protocolMinorVersion < 12 &&
        !token.empty()) {
        m_client->connectBulkChannel(token);
    }
}

bool
ServerProxy::bulkOffer1_12()
{
    std::string token;
    std::string connectionBinding;
    if (!ProtocolUtil::readf(m_stream, kMsgCBulkOffer1_12 + 4,
                             &token, &connectionBinding) ||
        token.empty() || !isValidConnectionBinding(connectionBinding) ||
        (!m_connectionBinding.empty() &&
         m_connectionBinding != connectionBinding) ||
        !m_client->connectBulkChannel(token, connectionBinding)) {
        return false;
    }
    initializeTransactionalFileTransfer(connectionBinding);
    return true;
}

void
ServerProxy::initializeTransactionalFileTransfer(
    const std::string& connectionBinding)
{
    if (!isValidConnectionBinding(connectionBinding)) {
        resetTransactionalFileReceive(true);
        m_fileTransferReceiver.reset();
        m_connectionBinding.clear();
        return;
    }
    if (m_fileTransferReceiver &&
        m_connectionBinding == connectionBinding) {
        return;
    }

    resetTransactionalFileReceive(true);
    m_connectionBinding = connectionBinding;
    m_fileTransferReceiver.reset(new barrier::FileTransferReceiver(
        connectionBinding, barrier::FileTransferRole::kPrimary));
}

bool
ServerProxy::writeTransactionalFileTransferFrame(
    const barrier::FileTransferFrame& frame,
    barrier::FileTransferRole initiatorRole)
{
    if (m_protocolMinorVersion < 12 ||
        !isValidConnectionBinding(m_connectionBinding) ||
        frame.connectionBinding != m_connectionBinding) {
        return false;
    }
    try {
        return barrier::FileTransferProtocol::encode(
            m_stream, frame, initiatorRole);
    }
    catch (...) {
        return false;
    }
}

bool
ServerProxy::sendTransactionalFileTransferFrame(
    const barrier::FileTransferFrame& frame)
{
    if (frame.type != barrier::FileTransferFrameType::kStart &&
        frame.type != barrier::FileTransferFrameType::kCancel) {
        return false;
    }
    return writeTransactionalFileTransferFrame(
        frame, barrier::FileTransferRole::kSecondary);
}

ServerProxy::EResult
ServerProxy::transactionalControlFrame(const UInt8* code)
{
    if (m_protocolMinorVersion < 12 || !m_fileTransferReceiver ||
        !isValidConnectionBinding(m_connectionBinding)) {
        return kDisconnect;
    }

    const bool peerInitiated =
        std::memcmp(code, kMsgDFileTransferStart1_12, 4) == 0 ||
        std::memcmp(code, kMsgDFileTransferCancel1_12, 4) == 0;
    barrier::FileTransferFrame frame;
    if (!barrier::FileTransferProtocol::decode(
            code, m_stream,
            peerInitiated ? barrier::FileTransferRole::kPrimary :
                            barrier::FileTransferRole::kSecondary,
            m_connectionBinding, frame)) {
        return kDisconnect;
    }

    if (!peerInitiated) {
        return m_client->signalTransactionalFileTransferAck(frame) ?
            kOkay : kDisconnect;
    }

    if (frame.type == barrier::FileTransferFrameType::kStart) {
        std::shared_ptr<barrier::BulkChannel> bulkChannel =
            m_client->acquireBulkChannel();
        if (!bulkChannel || !bulkChannel->isActive()) {
            const barrier::FileTransferFrame ack =
                barrier::FileTransferFrame::startAck(
                    m_connectionBinding, frame.transferId,
                    barrier::FileTransferReason::kConnectionLost);
            return writeTransactionalFileTransferFrame(
                ack, barrier::FileTransferRole::kPrimary) ?
                    kOkay : kDisconnect;
        }

        const barrier::FileTransferReceiveResult result =
            m_fileTransferReceiver->handle(frame);
        if (result.status ==
                barrier::FileTransferReceiveStatus::kProtocolError) {
            return kDisconnect;
        }

        barrier::FileTransferReason reason = result.reason;
        if (result.status ==
                barrier::FileTransferReceiveStatus::kStartAccepted) {
            reason = m_client->beginTransactionalFileReceive(
                frame);
            if (reason == barrier::FileTransferReason::kNone) {
                m_fileTransferReceiveId = frame.transferId;
                m_fileTransferReceiveBulkChannel = bulkChannel;
                m_fileTransferCancelAckPending = false;
            }
            else {
                m_fileTransferReceiver->reset();
            }
        }
        else if (result.status !=
                     barrier::FileTransferReceiveStatus::kStartRejected) {
            return kDisconnect;
        }

        const barrier::FileTransferFrame ack =
            barrier::FileTransferFrame::startAck(
                m_connectionBinding, frame.transferId, reason);
        if (!writeTransactionalFileTransferFrame(
                ack, barrier::FileTransferRole::kPrimary)) {
            if (reason == barrier::FileTransferReason::kNone) {
                resetTransactionalFileReceive(true);
            }
            return kDisconnect;
        }
        return kOkay;
    }

    if (frame.type == barrier::FileTransferFrameType::kCancel) {
        return handleTransactionalFileCancel(frame) ?
            kOkay : kDisconnect;
    }

    return kDisconnect;
}

bool
ServerProxy::handleTransactionalFileCancel(
    const barrier::FileTransferFrame& frame)
{
    if (!m_fileTransferReceiver) {
        return false;
    }
    const barrier::FileTransferReceiveResult result =
        m_fileTransferReceiver->handle(frame);
    if (result.status ==
            barrier::FileTransferReceiveStatus::kCancelAlreadyApplied) {
        const barrier::FileTransferFrame ack =
            barrier::FileTransferFrame::cancelAck(
                m_connectionBinding, frame.transferId,
                barrier::FileTransferReason::kNone);
        return writeTransactionalFileTransferFrame(
            ack, barrier::FileTransferRole::kPrimary);
    }
    if (result.status != barrier::FileTransferReceiveStatus::kCancelled) {
        resetTransactionalFileReceive(true);
        return false;
    }

    cleanupTransactionalFileReceivePoll();
    resetTransactionalFileReceivePollBudget();
    m_client->cancelTransactionalFileReceive(frame.transferId);
    m_fileTransferReceiveBulkChannel.reset();
    m_fileTransferCancelAckPending = true;
    if (m_fileTransferReceiver->workerCleanupPending()) {
        return scheduleTransactionalFileReceivePoll(frame.transferId) ||
            quarantineTransactionalFileReceive(frame.transferId, true);
    }
    m_fileTransferCancelAckPending = false;
    m_fileTransferReceiveId = 0;
    resetTransactionalFileReceivePollBudget();
    const barrier::FileTransferFrame ack =
        barrier::FileTransferFrame::cancelAck(
            m_connectionBinding, frame.transferId,
            barrier::FileTransferReason::kNone);
    return writeTransactionalFileTransferFrame(
        ack, barrier::FileTransferRole::kPrimary);
}

bool
ServerProxy::handleBulkMessage(const UInt8* code, barrier::IStream* stream)
{
    if (m_protocolMinorVersion >= 12 &&
        isTransactionalFileBulkCode(code)) {
        return transactionalBulkFrame(code, stream);
    }
    if (m_protocolMinorVersion < 12 &&
        memcmp(code, kMsgDFileTransfer, 4) == 0) {
        return discardLegacyFileChunk(stream);
    }
    if (memcmp(code, kMsgDClipboard, 4) == 0) {
        setClipboard(stream);
        return true;
    }
    return false;
}

bool
ServerProxy::transactionalBulkFrame(
    const UInt8* code, barrier::IStream* stream)
{
    std::shared_ptr<barrier::BulkChannel> currentBulk =
        m_client->acquireBulkChannel();
    if (m_protocolMinorVersion < 12 || !m_fileTransferReceiver ||
        !currentBulk || !currentBulk->isActive() ||
        currentBulk->getStream() != stream) {
        return false;
    }

    barrier::FileTransferFrame frame;
    if (!barrier::FileTransferProtocol::decode(
            code, stream, barrier::FileTransferRole::kPrimary,
            m_connectionBinding, frame)) {
        resetTransactionalFileReceive(true);
        return false;
    }
    const barrier::FileTransferReceiveResult result =
        m_fileTransferReceiver->handle(frame);
    if (result.status ==
            barrier::FileTransferReceiveStatus::kCancelledPayloadDiscarded) {
        LOG((CLOG_DEBUG1
            "discarding queued bulk payload for cancelled transfer %u",
            frame.transferId));
        return true;
    }
    if (!m_fileTransferReceiver->hasActiveTransfer() ||
        m_fileTransferReceiveId == 0 ||
        currentBulk != m_fileTransferReceiveBulkChannel ||
        frame.transferId != m_fileTransferReceiveId) {
        resetTransactionalFileReceive(true);
        return false;
    }

    switch (result.status) {
    case barrier::FileTransferReceiveStatus::kDataAccepted:
        return frame.type == barrier::FileTransferFrameType::kData;

    case barrier::FileTransferReceiveStatus::kBackpressure:
        if (frame.type != barrier::FileTransferFrameType::kData ||
            !currentBulk->pauseInputForBackpressure(
                result.sessionGeneration) ||
            !m_fileTransferReceiver->installBackpressureBarrier(
                result.sessionGeneration,
                currentBulk->makeInputResumeCallback(
                    result.sessionGeneration),
                currentBulk->makeInputProgressCallback(
                    result.sessionGeneration))) {
            currentBulk->resumeInputAfterBackpressure(
                result.sessionGeneration);
            resetTransactionalFileReceive(true);
            return false;
        }
        return true;

    case barrier::FileTransferReceiveStatus::kAwaitingCommit:
    case barrier::FileTransferReceiveStatus::kReadyToCommit:
        if (frame.type != barrier::FileTransferFrameType::kEnd ||
            !currentBulk->pauseInputForCommit(result.sessionGeneration) ||
            !m_fileTransferReceiver->installCommitBarrier(
                result.sessionGeneration,
                currentBulk->makeInputResumeCallback(
                    result.sessionGeneration),
                currentBulk->makeInputProgressCallback(
                    result.sessionGeneration))) {
            currentBulk->resumeInputAfterCommit(result.sessionGeneration);
            resetTransactionalFileReceive(true);
            return false;
        }
        if (result.status ==
                barrier::FileTransferReceiveStatus::kReadyToCommit) {
            pollTransactionalFileReceive();
        }
        else {
            scheduleTransactionalFileReceivePoll(frame.transferId);
        }
        return true;

    case barrier::FileTransferReceiveStatus::kTransferFailed:
        if (frame.type == barrier::FileTransferFrameType::kEnd) {
            const UInt32 transferId = m_fileTransferReceiveId;
            cleanupTransactionalFileReceivePoll();
            m_fileTransferReceiver->reset();
            m_client->cancelTransactionalFileReceive(transferId);
            m_fileTransferReceiveId = 0;
            m_fileTransferReceiveBulkChannel.reset();
            const barrier::FileTransferFrame ack =
                barrier::FileTransferFrame::commitAck(
                    m_connectionBinding, transferId, result.reason);
            return writeTransactionalFileTransferFrame(
                ack, barrier::FileTransferRole::kPrimary);
        }
        resetTransactionalFileReceive(true);
        return false;

    case barrier::FileTransferReceiveStatus::kStartAccepted:
    case barrier::FileTransferReceiveStatus::kStartRejected:
    case barrier::FileTransferReceiveStatus::kCancelled:
    case barrier::FileTransferReceiveStatus::kCancelledPayloadDiscarded:
    case barrier::FileTransferReceiveStatus::kProtocolError:
        resetTransactionalFileReceive(true);
        return false;
    }

    resetTransactionalFileReceive(true);
    return false;
}

void
ServerProxy::pollTransactionalFileReceive()
{
    if (!m_fileTransferReceiver || m_fileTransferReceiveId == 0) {
        cleanupTransactionalFileReceivePoll();
        return;
    }

    const UInt32 transferId = m_fileTransferReceiveId;
    if (m_fileTransferCancelAckPending) {
        if (m_fileTransferReceiver->workerCleanupPending()) {
            if (!scheduleTransactionalFileReceivePoll(transferId) &&
                !quarantineTransactionalFileReceive(transferId, true)) {
                LOG((CLOG_WARN
                    "failed to acknowledge quarantined file cancellation, transfer=%u",
                    transferId));
            }
            return;
        }
        cleanupTransactionalFileReceivePoll();
        m_fileTransferCancelAckPending = false;
        m_fileTransferReceiveId = 0;
        resetTransactionalFileReceivePollBudget();
        const barrier::FileTransferFrame ack =
            barrier::FileTransferFrame::cancelAck(
                m_connectionBinding, transferId,
                barrier::FileTransferReason::kNone);
        if (!writeTransactionalFileTransferFrame(
                ack, barrier::FileTransferRole::kPrimary)) {
            LOG((CLOG_WARN
                "failed to send transactional file cancel acknowledgment, transfer=%u",
                transferId));
        }
        return;
    }
    if (!m_fileTransferReceiver->hasActiveTransfer()) {
        cleanupTransactionalFileReceivePoll();
        return;
    }
    const barrier::FileTransferReceiveResult result =
        m_fileTransferReceiver->pollCompletion(transferId);
    if (result.status ==
            barrier::FileTransferReceiveStatus::kAwaitingCommit) {
        if (!scheduleTransactionalFileReceivePoll(transferId) &&
            !quarantineTransactionalFileReceive(transferId, false)) {
            LOG((CLOG_WARN
                "failed to terminate timed-out file receive, transfer=%u",
                transferId));
        }
        return;
    }

    barrier::FileTransferReason reason = result.reason;
    if (result.status ==
            barrier::FileTransferReceiveStatus::kReadyToCommit) {
        barrier::CompletedFilePayload payload;
        if (!m_fileTransferReceiver->takeCompleted(
                transferId, payload)) {
            reason = barrier::FileTransferReason::kIoError;
            m_client->cancelTransactionalFileReceive(transferId);
        }
        else {
            reason = m_client->acceptTransactionalFileReceive(
                transferId, std::move(payload));
        }
    }
    else if (result.status ==
                 barrier::FileTransferReceiveStatus::kTransferFailed) {
        m_client->cancelTransactionalFileReceive(transferId);
    }
    else {
        resetTransactionalFileReceive(true);
        return;
    }

    cleanupTransactionalFileReceivePoll();
    m_fileTransferReceiveId = 0;
    m_fileTransferCancelAckPending = false;
    m_fileTransferReceiveBulkChannel.reset();
    resetTransactionalFileReceivePollBudget();
    const barrier::FileTransferFrame ack =
        barrier::FileTransferFrame::commitAck(
            m_connectionBinding, transferId, reason);
    if (!writeTransactionalFileTransferFrame(
            ack, barrier::FileTransferRole::kPrimary)) {
        LOG((CLOG_WARN
            "failed to send transactional file commit acknowledgment, transfer=%u",
            transferId));
    }
}

bool
ServerProxy::scheduleTransactionalFileReceivePoll(UInt32 transferId)
{
    if (transferId == 0 || transferId != m_fileTransferReceiveId) {
        return false;
    }
    if (m_fileTransferReceiveTimer != NULL) {
        return m_fileTransferReceivePollBudgetId == transferId;
    }
    if (m_fileTransferReceivePollBudgetId != transferId) {
        resetTransactionalFileReceivePollBudget();
        m_fileTransferReceivePollBudgetId = transferId;
    }

    const double remaining = kFileTransferReceivePollDeadlineSeconds -
        m_fileTransferReceivePollElapsed;
    if (remaining <= 0.0) {
        return false;
    }
    const double delay = (std::min)(
        m_fileTransferReceiveNextPollDelay, remaining);
    m_fileTransferReceiveTimer = m_events->newOneShotTimer(
        delay, NULL);
    if (m_fileTransferReceiveTimer == NULL) {
        LOG((CLOG_ERR
            "unable to schedule transactional file receive poll, transfer=%u",
            transferId));
        return false;
    }
    m_fileTransferReceivePollElapsed += delay;
    m_fileTransferReceiveNextPollDelay = (std::min)(
        kFileTransferReceiveMaxPollSeconds,
        m_fileTransferReceiveNextPollDelay * 2.0);
    m_events->adoptHandler(Event::kTimer, m_fileTransferReceiveTimer,
        new TMethodEventJob<ServerProxy>(
            this, &ServerProxy::handleTransactionalFileReceivePoll));
    return true;
}

void
ServerProxy::handleTransactionalFileReceivePoll(const Event&, void*)
{
    cleanupTransactionalFileReceivePoll();
    pollTransactionalFileReceive();
}

void
ServerProxy::cleanupTransactionalFileReceivePoll()
{
    if (m_fileTransferReceiveTimer != NULL) {
        m_events->removeHandler(Event::kTimer,
                                m_fileTransferReceiveTimer);
        m_events->deleteTimer(m_fileTransferReceiveTimer);
        m_fileTransferReceiveTimer = NULL;
    }
}

void
ServerProxy::resetTransactionalFileReceivePollBudget()
{
    m_fileTransferReceivePollBudgetId = 0;
    m_fileTransferReceivePollElapsed = 0.0;
    m_fileTransferReceiveNextPollDelay =
        kFileTransferReceiveInitialPollSeconds;
}

bool
ServerProxy::quarantineTransactionalFileReceive(
    UInt32 transferId, bool cancelled)
{
    cleanupTransactionalFileReceivePoll();
    if (!m_fileTransferReceiver || transferId == 0 ||
        transferId != m_fileTransferReceiveId) {
        return false;
    }

    LOG((CLOG_WARN
        "quarantining timed-out transactional file receive cleanup, transfer=%u",
        transferId));
    m_client->cancelTransactionalFileReceive(transferId);
    m_fileTransferReceiver->reset();
    m_fileTransferReceiver->quarantineRetiredWorkerCleanup();
    m_fileTransferReceiveId = 0;
    m_fileTransferCancelAckPending = false;
    m_fileTransferReceiveBulkChannel.reset();
    resetTransactionalFileReceivePollBudget();

    const barrier::FileTransferFrame ack = cancelled ?
        barrier::FileTransferFrame::cancelAck(
            m_connectionBinding, transferId,
            barrier::FileTransferReason::kNone) :
        barrier::FileTransferFrame::commitAck(
            m_connectionBinding, transferId,
            barrier::FileTransferReason::kTimeout);
    return writeTransactionalFileTransferFrame(
        ack, barrier::FileTransferRole::kPrimary);
}

void
ServerProxy::resetTransactionalFileReceive(bool notifyClient)
{
    cleanupTransactionalFileReceivePoll();
    const UInt32 transferId = m_fileTransferReceiveId;
    if (m_fileTransferReceiver) {
        m_fileTransferReceiver->reset();
    }
    m_fileTransferReceiveId = 0;
    m_fileTransferCancelAckPending = false;
    m_fileTransferReceiveBulkChannel.reset();
    resetTransactionalFileReceivePollBudget();
    if (notifyClient && transferId != 0) {
        m_client->cancelTransactionalFileReceive(transferId);
    }
}

void
ServerProxy::dragInfoReceived()
{
    // parse
    UInt32 fileNum = 0;
    std::string content;
    ProtocolUtil::readf(m_stream, kMsgDDragInfo + 4, &fileNum, &content);

    m_client->dragInfoReceived(fileNum, content);
}

void
ServerProxy::handleClipboardSendingEvent(const Event& event, void*)
{
    ClipboardChunk* chunk = static_cast<ClipboardChunk*>(event.getData());
    handleClipboardSendingChunk(chunk);
}

void
ServerProxy::handleClipboardSendingChunk(ClipboardChunk* chunk)
{
    if (chunk == NULL) {
        return;
    }
    if (!chunk->isSendRouteActive()) {
        chunk->failSendAttempt();
        const std::shared_ptr<barrier::ClipboardSendAttempt> attempt =
            chunk->getSendAttempt();
        const ClipboardID id = chunk->getClipboardId();
        if (attempt && id < kClipboardEnd && attempt->id() == id &&
            m_latestClipboardSendAttempt[id] == attempt->attemptId()) {
            m_clipboardSendSucceeded = false;
            m_client->handleClipboardSendRouteFailure(id);
        }
        if (m_clipboardChunker) {
            m_clipboardChunker->interruptFile();
        }
        return;
    }
    try {
        ClipboardChunk::send(chunk->getSendStream(m_stream), chunk);
    }
    catch (...) {
        chunk->failSendAttempt();
        const std::shared_ptr<barrier::ClipboardSendAttempt> attempt =
            chunk->getSendAttempt();
        const ClipboardID id = chunk->getClipboardId();
        if (attempt && id < kClipboardEnd && attempt->id() == id &&
            m_latestClipboardSendAttempt[id] == attempt->attemptId()) {
            m_clipboardSendSucceeded = false;
            m_client->handleClipboardSendRouteFailure(id);
        }
        throw;
    }
}

void
ServerProxy::fileChunkSending(UInt8 mark, char* data, size_t dataSize)
{
    if (m_protocolMinorVersion < 12) {
        LOG((CLOG_WARN
            "not sending legacy file payload to server"));
        return;
    }
    FileChunk::send(m_stream, mark, data, dataSize);
}

void
ServerProxy::sendDragInfo(UInt32 fileCount, const char* info, size_t size)
{
    if (m_protocolMinorVersion < 12) {
        LOG((CLOG_WARN
            "not sending drag metadata to a legacy server"));
        return;
    }
    std::string data(info, size);
    ProtocolUtil::writef(m_stream, kMsgDDragInfo, fileCount, &data);
}
