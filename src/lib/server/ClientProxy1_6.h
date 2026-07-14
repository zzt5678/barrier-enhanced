/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2015-2016 Symless Ltd.
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

#include "barrier/ClipboardChunk.h"
#include "server/ClientProxy1_5.h"

#include <memory>

class Server;
class IEventQueue;
class StreamChunker;
class Thread;

//! Proxy for client implementing protocol version 1.6
class ClientProxy1_6 : public ClientProxy1_5 {
public:
    ClientProxy1_6(const std::string& name, barrier::IStream* adoptedStream, Server* server,
                   IEventQueue* events);
    ~ClientProxy1_6();

    virtual void        setClipboard(ClipboardID id, const IClipboard* clipboard);
    virtual bool        recvClipboard();

    virtual bool        cleanupClipboardSendThread(bool cancel);

#ifdef BARRIER_TEST_ENV
    bool                testClipboardDirty(ClipboardID id) const { return m_clipboard[id].m_dirty; }
    void                testSetClipboardSendThread(Thread* thread) { m_clipboardSendThread = thread; }
#endif

private:
    void                handleClipboardSendingEvent(const Event&, void*);
    void                sendClipboardThread(
                            const std::shared_ptr<const std::string>& data,
                            ClipboardID id,
                            const std::shared_ptr<StreamChunker>& chunker);

private:
    IEventQueue*        m_events;
    ClipboardChunk::ReceiveBuffer m_clipboardReceiveBuffer;
    Thread*             m_clipboardSendThread;
    std::shared_ptr<StreamChunker> m_clipboardChunker;
    ClipboardID         m_clipboardSendId;
    bool                m_clipboardSendSucceeded;
    bool                m_clipboardSendResultAvailable;
};
