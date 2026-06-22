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

#include "server/ClientProxy1_6.h"

#include "server/Server.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/StreamChunker.h"
#include "barrier/ClipboardChunk.h"
#include "io/IStream.h"
#include "base/TMethodEventJob.h"
#include "base/Log.h"
#include "mt/Thread.h"

namespace {

const size_t kSynchronousClipboardSendLimit = 256 * 1024;

}

//
// ClientProxy1_6
//

ClientProxy1_6::ClientProxy1_6(const std::string& name, barrier::IStream* stream, Server* server,
                               IEventQueue* events) :
    ClientProxy1_5(name, stream, server, events),
    m_events(events),
    m_clipboardSendThread(NULL),
    m_clipboardSendId(kClipboardEnd),
    m_clipboardSendSucceeded(false),
    m_clipboardSendResultAvailable(false)
{
    m_events->adoptHandler(m_events->forClipboard().clipboardSending(),
                                this,
                                new TMethodEventJob<ClientProxy1_6>(this,
                                    &ClientProxy1_6::handleClipboardSendingEvent));
}

ClientProxy1_6::~ClientProxy1_6()
{
    if (!cleanupClipboardSendThread(true) && m_clipboardSendThread != NULL) {
        LOG((CLOG_ERR "waiting for clipboard sender before destroying client proxy"));
        m_clipboardSendThread->wait();
        delete m_clipboardSendThread;
        m_clipboardSendThread = NULL;
        m_clipboardChunker.reset();
    }
    m_events->removeHandler(m_events->forClipboard().clipboardSending(), this);
}

void
ClientProxy1_6::setClipboard(ClipboardID id, const IClipboard* clipboard)
{
    // ignore if this clipboard is already clean
    if (m_clipboard[id].m_dirty) {
        if (!cleanupClipboardSendThread(true)) {
            LOG((CLOG_WARN "clipboard %d send skipped for \"%s\" because previous sender is still stopping",
                id, getName().c_str()));
            return;
        }

        Clipboard::copy(&m_clipboard[id].m_clipboard, clipboard);

        std::shared_ptr<const std::string> data(
            new std::string(m_clipboard[id].m_clipboard.marshall()));

        size_t size = data->size();
        LOG((CLOG_DEBUG "sending clipboard %d to \"%s\"", id, getName().c_str()));

        if (data->size() <= kSynchronousClipboardSendLimit) {
            if (!StreamChunker::sendClipboard(*data, size, id, 0, m_events, this, getStream())) {
                LOG((CLOG_WARN "clipboard %d was not fully queued for \"%s\"", id, getName().c_str()));
                return;
            }
            m_clipboard[id].m_dirty = false;
            return;
        }

        std::shared_ptr<StreamChunker> chunker = std::make_shared<StreamChunker>();
        m_clipboardChunker = chunker;
        m_clipboardSendId = id;
        m_clipboardSendSucceeded = false;
        m_clipboardSendResultAvailable = false;
        m_clipboardSendThread = new Thread([this, data, id, chunker]() {
            sendClipboardThread(data, id, chunker);
        });
    }
}

void
ClientProxy1_6::sendClipboardThread(const std::shared_ptr<const std::string>& data,
                                    ClipboardID id,
                                    const std::shared_ptr<StreamChunker>& chunker)
{
    const bool sent = chunker->sendClipboardData(
            *data, data->size(), id, 0, m_events, this, getStream());
    m_clipboardSendSucceeded = sent;
    m_clipboardSendResultAvailable = true;
    if (!sent) {
        LOG((CLOG_WARN "clipboard %d was not fully queued for \"%s\"", id, getName().c_str()));
    }
}

bool
ClientProxy1_6::cleanupClipboardSendThread(bool cancel)
{
    if (cancel && m_clipboardChunker) {
        m_clipboardChunker->interruptFile();
    }

    if (m_clipboardSendThread != NULL) {
        if (cancel && !m_clipboardSendThread->wait(0.5)) {
            LOG((CLOG_WARN "clipboard send thread did not stop after interrupt; cancelling"));
            m_clipboardSendThread->cancel();
            m_clipboardSendThread->unblockPollSocket();
            if (!m_clipboardSendThread->wait(2.0)) {
                LOG((CLOG_ERR "clipboard send thread still running after cancellation; cleanup deferred"));
                return false;
            }
        }
        else if (!cancel && !m_clipboardSendThread->wait(2.0)) {
            LOG((CLOG_ERR "clipboard send thread still running; cleanup deferred"));
            return false;
        }
        if (m_clipboardSendResultAvailable && m_clipboardSendId < kClipboardEnd) {
            m_clipboard[m_clipboardSendId].m_dirty = !m_clipboardSendSucceeded;
        }
        delete m_clipboardSendThread;
        m_clipboardSendThread = NULL;
        m_clipboardSendId = kClipboardEnd;
        m_clipboardSendSucceeded = false;
        m_clipboardSendResultAvailable = false;
    }

    m_clipboardChunker.reset();
    return true;
}

void
ClientProxy1_6::handleClipboardSendingEvent(const Event& event, void*)
{
    ClipboardChunk::send(getStream(), event.getData());
}

bool
ClientProxy1_6::recvClipboard()
{
    // parse message
    ClipboardID id;
    UInt32 seq;

    int r = ClipboardChunk::assemble(getStream(), m_clipboardReceiveBuffer, id, seq);

    if (r == kStart) {
        size_t size = m_clipboardReceiveBuffer.expectedSize;
        LOG((CLOG_DEBUG "receiving clipboard %d size=%d", id, size));
    }
    else if (r == kFinish) {
        LOG((CLOG_DEBUG "received client \"%s\" clipboard %d seqnum=%d, size=%d",
                getName().c_str(), id, seq, m_clipboardReceiveBuffer.data.size()));
        // save clipboard
        m_clipboard[id].m_clipboard.unmarshall(m_clipboardReceiveBuffer.data, 0);
        m_clipboard[id].m_sequenceNumber = seq;
        m_clipboardReceiveBuffer.release();

        // notify
        ClipboardInfo* info = new ClipboardInfo;
        info->m_id = id;
        info->m_sequenceNumber = seq;
        m_events->addEvent(Event(m_events->forClipboard().clipboardChanged(),
                                 getEventTarget(), info));
    }

    return true;
}
