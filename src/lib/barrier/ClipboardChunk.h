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

#include "barrier/Chunk.h"
#include "barrier/clipboard_types.h"
#include "base/String.h"
#include "common/basic_types.h"

#include <atomic>
#include <cstdint>
#include <memory>

#define CLIPBOARD_CHUNK_META_SIZE 7

namespace barrier {
class BulkChannel;
class IStream;

class ClipboardSendAttempt {
public:
    ClipboardSendAttempt(ClipboardID id, std::uint64_t attemptId) :
        m_id(id),
        m_attemptId(attemptId),
        m_failed(false)
    {
    }

    ClipboardID id() const { return m_id; }
    std::uint64_t attemptId() const { return m_attemptId; }
    void fail() { m_failed.store(true); }
    bool failed() const { return m_failed.load(); }

private:
    ClipboardID m_id;
    std::uint64_t m_attemptId;
    std::atomic<bool> m_failed;
};
};

class ClipboardChunk : public Chunk {
public:
    static const size_t kMaxReceiveSize;

    class ReceiveBuffer {
    public:
        void clear();
        void release();

        String data;
        size_t expectedSize = 0;
        bool inProgress = false;
    };

    ClipboardChunk(size_t size);

    static ClipboardChunk*
                        start(
                            ClipboardID id,
                            UInt32 sequence,
                            const String& size);
    static ClipboardChunk*
                        data(
                            ClipboardID id,
                            UInt32 sequence,
                            const String& data);
    static ClipboardChunk*
                        end(ClipboardID id, UInt32 sequence);
    static ClipboardChunk*
                        cancel(ClipboardID id, UInt32 sequence);

    static int            assemble(
                            barrier::IStream* stream,
                            ReceiveBuffer& buffer,
                            ClipboardID& id,
                            UInt32& sequence);

    static void            send(barrier::IStream* stream, void* data);

    void                   setSendRoute(
                            barrier::IStream* stream,
                            const std::shared_ptr<barrier::BulkChannel>& bulkChannel,
                            const std::shared_ptr<barrier::ClipboardSendAttempt>& attempt =
                                std::shared_ptr<barrier::ClipboardSendAttempt>());
    barrier::IStream*      getSendStream(barrier::IStream* fallback) const;
    bool                   isSendRouteActive() const;
    ClipboardID            getClipboardId() const;
    std::shared_ptr<barrier::ClipboardSendAttempt> getSendAttempt() const;
    void                   failSendAttempt();

private:
    barrier::IStream*      m_sendStream;
    std::shared_ptr<barrier::BulkChannel> m_bulkChannelLease;
    std::shared_ptr<barrier::ClipboardSendAttempt> m_sendAttempt;

};
