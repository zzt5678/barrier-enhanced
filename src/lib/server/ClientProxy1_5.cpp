/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2013-2016 Symless Ltd.
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

#include "server/ClientProxy1_5.h"

#include "server/Server.h"
#include "barrier/FileChunk.h"
#include "barrier/StreamChunker.h"
#include "barrier/ProtocolUtil.h"
#include "io/IStream.h"
#include "base/TMethodEventJob.h"
#include "base/Log.h"

#include <sstream>

//
// ClientProxy1_5
//

ClientProxy1_5::ClientProxy1_5(const std::string& name, barrier::IStream* stream, Server* server,
                               IEventQueue* events) :
    ClientProxy1_4(name, stream, server, events),
    m_events(events)
{

    m_events->adoptHandler(m_events->forFile().keepAlive(),
                            this,
                            new TMethodEventJob<ClientProxy1_3>(this,
                                &ClientProxy1_3::handleKeepAlive, NULL));
}

ClientProxy1_5::~ClientProxy1_5()
{
    m_events->removeHandler(m_events->forFile().keepAlive(), this);
}

void
ClientProxy1_5::sendDragInfo(UInt32 fileCount, const char* info, size_t size)
{
    if (!supportsTransactionalFileTransfer()) {
        LOG((CLOG_WARN
            "not sending drag metadata to legacy client \"%s\"",
            getName().c_str()));
        return;
    }
    std::string data(info, size);

    ProtocolUtil::writef(getStream(), kMsgDDragInfo, fileCount, &data);
}

void
ClientProxy1_5::fileChunkSending(UInt8 mark, char* data, size_t dataSize)
{
    if (!supportsTransactionalFileTransfer()) {
        LOG((CLOG_WARN
            "not sending legacy file payload to client \"%s\"",
            getName().c_str()));
        return;
    }
    FileChunk::send(getStream(), mark, data, dataSize);
}

bool
ClientProxy1_5::parseMessage(const UInt8* code)
{
    if (memcmp(code, kMsgDFileTransfer, 4) == 0) {
        return discardLegacyFileChunk(getStream());
    }
    else if (memcmp(code, kMsgDDragInfo, 4) == 0) {
        return discardLegacyDragInfo(getStream());
    }
    else {
        return ClientProxy1_4::parseMessage(code);
    }

    return true;
}

bool
ClientProxy1_5::discardLegacyFileChunk(barrier::IStream* stream)
{
    UInt8 mark = 0;
    std::string content;
    if (stream == NULL ||
        !ProtocolUtil::readf(
            stream, kMsgDFileTransfer + 4, &mark, &content)) {
        LOG((CLOG_WARN "invalid legacy file payload from \"%s\"",
            getName().c_str()));
        return false;
    }
    LOG((CLOG_WARN
        "ignored legacy file payload from \"%s\"; protocol 1.12 is required",
        getName().c_str()));
    return true;
}

bool
ClientProxy1_5::discardLegacyDragInfo(barrier::IStream* stream)
{
    UInt32 fileCount = 0;
    std::string content;
    if (stream == NULL ||
        !ProtocolUtil::readf(
            stream, kMsgDDragInfo + 4, &fileCount, &content)) {
        LOG((CLOG_WARN "invalid legacy drag metadata from \"%s\"",
            getName().c_str()));
        return false;
    }
    LOG((CLOG_WARN
        "ignored legacy drag metadata from \"%s\"; protocol 1.12 is required",
        getName().c_str()));
    return true;
}

int
ClientProxy1_5::fileChunkReceived()
{
    return fileChunkReceived(getStream());
}

int
ClientProxy1_5::fileChunkReceived(barrier::IStream* stream,
                                  barrier::BulkChannel* channel)
{
    Server* server = getServer();
    if (!server->canReceiveFileChunk(this, channel)) {
        LOG((CLOG_WARN "rejecting file data from a stale or competing route"));
        return kError;
    }
    int result = FileChunk::assemble(
                    stream,
                    server->getFileReceiveSession());


    if (result == kFinish) {
        const std::uint64_t generation =
            server->getFileReceiveSession().generation();
        FileReceiveCompletionInfo* completionInfo = NULL;
        try {
            completionInfo = new FileReceiveCompletionInfo(generation);
            Event completed(m_events->forFile().fileRecieveCompleted(), server);
            completed.setDataObject(completionInfo);
            m_events->addEvent(completed);
            completionInfo = NULL;
        }
        catch (...) {
            delete completionInfo;
            server->getFileReceiveSession().fail();
            server->abortFileReceiveRoute(this, channel);
            LOG((CLOG_ERR
                "failed to queue completed file receive, generation=%llu",
                static_cast<unsigned long long>(generation)));
            return kError;
        }
        server->completeFileReceiveRoute(this, channel);
    }
    else if (result == kStart) {
        server->bindFileReceiveClipboardRevision(this, channel);
        if (channel == NULL &&
            server->getFileReceiveSession().expectedSize() >
                FileChunk::kMemoryReceiveLimit) {
            LOG((CLOG_WARN
                "discarding legacy file transfer that requires disk spooling; "
                "bulk transport is required, size=%llu",
                static_cast<unsigned long long>(
                    server->getFileReceiveSession().expectedSize())));
            server->getFileReceiveSession().discardRemaining();
            return kStart;
        }
        if (server->getFakeDragFileList().size() > 0) {
            std::string filename = server->getFakeDragFileList().at(0).getFilename();
            LOG((CLOG_DEBUG "start receiving %s", filename.c_str()));
        }
    }
    else if (result == kCancelled) {
        server->abortFileReceiveRoute(this, channel);
    }
    else if (result == kError) {
        server->abortFileReceiveRoute(this, channel);
    }
    else if (result == kBackpressure && channel == NULL) {
        LOG((CLOG_WARN
            "discarding flow-controlled legacy file transfer while preserving "
            "the control connection"));
        server->getFileReceiveSession().discardRemaining();
    }
    return result;
}

void
ClientProxy1_5::dragInfoReceived()
{
    // parse
    UInt32 fileNum = 0;
    std::string content;
    ProtocolUtil::readf(getStream(), kMsgDDragInfo + 4, &fileNum, &content);

    m_server->dragInfoReceived(fileNum, content);
}
