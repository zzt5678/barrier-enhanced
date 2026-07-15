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
#include "barrier/FileReceiveSession.h"
#include "base/Event.h"
#include "base/String.h"
#include "common/basic_types.h"
#include "io/filesystem.h"

#define FILE_CHUNK_META_SIZE 2

namespace barrier {
class IStream;
};

class FileReceiveCompletionInfo : public EventData {
public:
    explicit FileReceiveCompletionInfo(std::uint64_t generation) :
        m_generation(generation)
    {
    }

    std::uint64_t m_generation;
};

class FileChunk : public Chunk {
public:
    FileChunk(size_t size);
    UInt32 m_transferId;

    static const size_t kMaxReceiveSize;
    static const size_t kMemoryReceiveLimit;

    static FileChunk*    start(const String& size);
    static FileChunk*    data(const UInt8* data, size_t dataSize);
    static FileChunk*    end(const String& digest = String());
    static FileChunk*    cancel();
    static int            assemble(
                            barrier::IStream* stream,
                            FileReceiveSession& session);
    static void            releaseReceiveBuffer(FileReceiveSession& session);
    static void            releaseReceiveBuffer(
                            String& dataCached,
                            size_t& expectedSize,
                            barrier::fs::path* spoolPath = NULL);
    static void            send(
                            barrier::IStream* stream,
                            UInt8 mark,
                            char* data,
                            size_t dataSize);
};
