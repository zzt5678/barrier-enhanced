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

#include "barrier/clipboard_types.h"
#include "barrier/ClipboardChunk.h"
#include "barrier/key_types.h"
#include "barrier/mouse_types.h"
#include "barrier/option_types.h"
#include "barrier/protocol_types.h"
#include "base/Event.h"
#include "base/Stopwatch.h"

#include <memory>
#include <map>
#include <set>
#include <string>

class Client;
class ClientInfo;
class EventQueueTimer;
class IClipboard;
class StreamChunker;
class Thread;
namespace barrier { class IStream; }
class IEventQueue;

//! Proxy for server
/*!
This class acts a proxy for the server, converting calls into messages
to the server and messages from the server to calls on the client.
*/
class ServerProxy {
public:
    enum ClipboardSendResult {
        kClipboardSendFailed,
        kClipboardSendQueued,
        kClipboardSendPending
    };

    /*!
    Process messages from the server on \p stream and forward to
    \p client.
    */
    ServerProxy(Client* client, barrier::IStream* stream, IEventQueue* events,
                SInt16 protocolMinorVersion = kProtocolMinorVersion);
    virtual ~ServerProxy();

    //! @name manipulators
    //@{

    void                onInfoChanged();
    bool                onGrabClipboard(ClipboardID);
    virtual ClipboardSendResult onClipboardChanged(ClipboardID, const IClipboard*);
    barrier::IStream*   getStream() const { return m_stream; }
    void                keepAlive();
    virtual bool        cleanupClipboardSendThread(bool cancel);
    virtual bool        reapClipboardSendResult(ClipboardID id, bool& succeeded);
    void                detachForDeferredCleanup();
    void                revokeInputLease();

    //@}

    // sending file chunk to server
    void                fileChunkSending(UInt8 mark, char* data, size_t dataSize);

    // sending dragging information to server
    void                sendDragInfo(UInt32 fileCount, const char* info, size_t size);

    static bool         hasCompleteOptionPairs(const OptionsList& options);

#ifdef BARRIER_TEST_ENV
    void                handleDataForTest() { handleData(Event(), NULL); }
#endif

#if defined(BARRIER_TEST_ENV) || defined(BARRIER_TEST_ACCESS)
public:
#else
protected:
#endif
    enum EResult { kOkay, kUnknown, kDisconnect };
    EResult                parseHandshakeMessage(const UInt8* code);
    EResult                parseMessage(const UInt8* code);

#if defined(BARRIER_TEST_ENV) || defined(BARRIER_TEST_ACCESS)
public:
#else
private:
#endif
    // if compressing mouse motion then send the last motion now
    void                flushCompressedMouse();
    void                discardCompressedMouse();
    bool                shouldCompressMouseMoves() const;
    bool                hasActivePointerLease(const char* inputType) const;
    void                clearStaleInfoAckGate();
    bool                acceptEpochInput(UInt32 epoch, UInt32 sequence,
                            UInt8 flags, const char* inputType);

    typedef void (ServerProxy::*InputPayloadHandler)();
    void                dispatchEpochInput(UInt32 epoch, UInt32 sequence,
                            UInt8 flags, const char* inputType,
                            InputPayloadHandler handler);
    void                releaseEpochPressedInput();

    void                sendInfo(const ClientInfo&);

    void                resetKeepAliveAlarm();
    void                setKeepAliveRate(double);
    bool                shouldDeferKeepAliveAlarm(bool hasPendingInput, UInt32 bufferedOutput) const;
    void                sendClipboardThread(
                            const std::shared_ptr<const std::string>& data,
                            ClipboardID id,
                            UInt32 sequence,
                            const std::shared_ptr<StreamChunker>& chunker);

    // modifier key translation
    KeyID                translateKey(KeyID) const;
    KeyModifierMask            translateModifierMask(KeyModifierMask) const;

    // event handlers
    void                handleData(const Event&, void*);
    void                handleKeepAliveAlarm(const Event&, void*);
    void                handleKeepAliveEvent(const Event&, void*);

    // message handlers
    void                enter();
    void                prepareEnter();
    void                abortEnter();
    void                leave();
    void                setClipboard();
    void                grabClipboard();
    void                keyDown();
    void                keyRepeat();
    void                keyUp();
    void                mouseDown();
    void                mouseUp();
    void                mouseMove();
    void                mouseRelativeMove();
    void                mouseWheel();
    void                keyDown1_8();
    void                keyRepeat1_8();
    void                keyUp1_8();
    void                mouseDown1_8();
    void                mouseUp1_8();
    void                mouseMove1_8();
    void                mouseRelativeMove1_8();
    void                mouseWheel1_8();
    void                screensaver();
    void                resetOptions();
    void                setOptions();
    void                queryInfo();
    void                infoAcknowledgment();
    void                fileChunkReceived();
    void                dragInfoReceived();
    void                handleClipboardSendingEvent(const Event&, void*);

#if defined(BARRIER_TEST_ENV) || defined(BARRIER_TEST_ACCESS)
public:
#else
private:
#endif
    typedef EResult (ServerProxy::*MessageParser)(const UInt8*);

    Client*            m_client;
    barrier::IStream*    m_stream;

    UInt32                m_seqNum;
    bool                  m_hasEnterSequence;
    bool                  m_inputActive;
    SInt16                m_protocolMinorVersion;
    UInt32                m_preparedEnterSequence;
    bool                  m_hasPreparedEnter;
    bool                  m_preparedEnterReady;
    UInt32                m_lastInputSequence;
    bool                  m_hasInputSequence;
    bool                  m_inputFrameAccepted;
    bool                  m_inputFrameBroadcast;
    bool                  m_inputFrameHasEpoch;

    struct PressedKey {
        PressedKey() : id(0), mask(0) { }
        PressedKey(KeyID keyId, KeyModifierMask keyMask) :
            id(keyId), mask(keyMask) { }

        KeyID id;
        KeyModifierMask mask;
    };
    std::map<KeyButton, PressedKey> m_epochPressedKeys;
    std::set<ButtonID> m_epochPressedButtons;

    bool                m_compressMouse;
    bool                m_compressMouseRelative;
    SInt32                m_xMouse, m_yMouse;
    SInt32                m_dxMouse, m_dyMouse;

    bool                m_ignoreMouse;
    Stopwatch           m_infoAckTimer;
    bool                m_lowLatencyMode;
    bool                m_nestedRemoteMode;

    KeyModifierID        m_modifierTranslationTable[kKeyModifierIDLast];

    double                m_keepAliveAlarm;
    EventQueueTimer*    m_keepAliveAlarmTimer;
    UInt32              m_keepAliveAlarmDeferrals;
    UInt32              m_keepAliveMissedAlarms;
    bool                m_lastKeepAlivePendingInput;
    UInt32              m_lastKeepAliveBufferedOutput;
    Stopwatch           m_keepAliveActivityTimer;
    ClipboardChunk::ReceiveBuffer m_clipboardReceiveBuffer;

    MessageParser        m_parser;
    IEventQueue*        m_events;
    Thread*             m_clipboardSendThread;
    std::shared_ptr<StreamChunker> m_clipboardChunker;
    bool                m_detachedForDeferredCleanup;
    ClipboardID         m_clipboardSendId;
    bool                m_clipboardSendSucceeded;
    bool                m_clipboardSendResultAvailable;
};
