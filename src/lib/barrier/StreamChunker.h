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

#pragma once

#include "barrier/clipboard_types.h"
#include "base/String.h"
#include "common/basic_types.h"

#include <atomic>

class IEventQueue;
namespace barrier { class IStream; }

class StreamChunker {
public:
    StreamChunker();

    void sendFile(const char* filename, IEventQueue* events, void* eventTarget,
                  barrier::IStream* stream = nullptr, UInt32 transferId = 0);
    bool                   sendClipboardData(
                            const String& data,
                            size_t size,
                            ClipboardID id,
                            UInt32 sequence,
                            IEventQueue* events,
                            void* eventTarget,
                            barrier::IStream* stream = nullptr);
    static bool            sendClipboard(
                            const String& data,
                            size_t size,
                            ClipboardID id,
                            UInt32 sequence,
                            IEventQueue* events,
                            void* eventTarget,
                            barrier::IStream* stream = nullptr);
    void                   interruptFile();

private:
    bool                   shouldInterrupt() const;

private:
    std::atomic<bool>      m_interruptFile;
};
