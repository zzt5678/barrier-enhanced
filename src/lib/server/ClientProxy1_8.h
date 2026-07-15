/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "server/ClientProxy1_7.h"

//! Proxy for client implementing protocol version 1.8
class ClientProxy1_8 : public ClientProxy1_7 {
public:
    ClientProxy1_8(const std::string& name, barrier::IStream* adoptedStream,
                   Server* server, IEventQueue* events);
    ~ClientProxy1_8();

    void enter(SInt32 xAbs, SInt32 yAbs, UInt32 seqNum,
               KeyModifierMask mask, bool forScreensaver) override;
    void keyDown(KeyID key, KeyModifierMask mask, KeyButton button) override;
    void keyDownBroadcast(KeyID key, KeyModifierMask mask,
                          KeyButton button) override;
    void keyRepeat(KeyID key, KeyModifierMask mask, SInt32 count,
                   KeyButton button) override;
    void keyUp(KeyID key, KeyModifierMask mask, KeyButton button) override;
    void keyUpBroadcast(KeyID key, KeyModifierMask mask,
                        KeyButton button) override;
    void mouseDown(ButtonID button) override;
    void mouseUp(ButtonID button) override;
    void mouseMove(SInt32 xAbs, SInt32 yAbs) override;
    void mouseRelativeMove(SInt32 xRel, SInt32 yRel) override;
    void mouseWheel(SInt32 xDelta, SInt32 yDelta) override;

private:
    UInt32 nextInputSequence();
    void sendKeyDown(KeyID key, KeyModifierMask mask, KeyButton button,
                     UInt8 flags);
    void sendKeyUp(KeyID key, KeyModifierMask mask, KeyButton button,
                   UInt8 flags);

    UInt32 m_inputEpoch;
    UInt32 m_inputSequence;
};
