/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "server/ClientProxy1_8.h"

#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"

ClientProxy1_8::ClientProxy1_8(const std::string& name,
                               barrier::IStream* stream,
                               Server* server,
                               IEventQueue* events) :
    ClientProxy1_7(name, stream, server, events),
    m_inputEpoch(0),
    m_inputSequence(0)
{
}

ClientProxy1_8::~ClientProxy1_8()
{
}

void
ClientProxy1_8::enter(SInt32 xAbs, SInt32 yAbs, UInt32 seqNum,
                      KeyModifierMask mask, bool forScreensaver)
{
    m_inputEpoch = seqNum;
    ClientProxy1_7::enter(xAbs, yAbs, seqNum, mask, forScreensaver);
}

UInt32
ClientProxy1_8::nextInputSequence()
{
    return ++m_inputSequence;
}

void
ClientProxy1_8::sendKeyDown(KeyID key, KeyModifierMask mask,
                            KeyButton button, UInt8 flags)
{
    ProtocolUtil::writef(getStream(), kMsgDKeyDown1_8,
                         m_inputEpoch, nextInputSequence(), flags,
                         key, mask, button);
}

void
ClientProxy1_8::keyDown(KeyID key, KeyModifierMask mask, KeyButton button)
{
    sendKeyDown(key, mask, button, kInputMessageNoFlags);
}

void
ClientProxy1_8::keyDownBroadcast(KeyID key, KeyModifierMask mask,
                                 KeyButton button)
{
    sendKeyDown(key, mask, button, kInputMessageBroadcast);
}

void
ClientProxy1_8::keyRepeat(KeyID key, KeyModifierMask mask, SInt32 count,
                          KeyButton button)
{
    ProtocolUtil::writef(getStream(), kMsgDKeyRepeat1_8,
                         m_inputEpoch, nextInputSequence(),
                         static_cast<UInt8>(kInputMessageNoFlags),
                         key, mask, count, button);
}

void
ClientProxy1_8::sendKeyUp(KeyID key, KeyModifierMask mask,
                          KeyButton button, UInt8 flags)
{
    ProtocolUtil::writef(getStream(), kMsgDKeyUp1_8,
                         m_inputEpoch, nextInputSequence(), flags,
                         key, mask, button);
}

void
ClientProxy1_8::keyUp(KeyID key, KeyModifierMask mask, KeyButton button)
{
    sendKeyUp(key, mask, button, kInputMessageNoFlags);
}

void
ClientProxy1_8::keyUpBroadcast(KeyID key, KeyModifierMask mask,
                               KeyButton button)
{
    sendKeyUp(key, mask, button, kInputMessageBroadcast);
}

void
ClientProxy1_8::mouseDown(ButtonID button)
{
    ProtocolUtil::writef(getStream(), kMsgDMouseDown1_8,
                         m_inputEpoch, nextInputSequence(), button);
}

void
ClientProxy1_8::mouseUp(ButtonID button)
{
    ProtocolUtil::writef(getStream(), kMsgDMouseUp1_8,
                         m_inputEpoch, nextInputSequence(), button);
}

void
ClientProxy1_8::mouseMove(SInt32 xAbs, SInt32 yAbs)
{
    ProtocolUtil::writef(getStream(), kMsgDMouseMove1_8,
                         m_inputEpoch, nextInputSequence(), xAbs, yAbs);
}

void
ClientProxy1_8::mouseRelativeMove(SInt32 xRel, SInt32 yRel)
{
    ProtocolUtil::writef(getStream(), kMsgDMouseRelMove1_8,
                         m_inputEpoch, nextInputSequence(), xRel, yRel);
}

void
ClientProxy1_8::mouseWheel(SInt32 xDelta, SInt32 yDelta)
{
    ProtocolUtil::writef(getStream(), kMsgDMouseWheel1_8,
                         m_inputEpoch, nextInputSequence(), xDelta, yDelta);
}
