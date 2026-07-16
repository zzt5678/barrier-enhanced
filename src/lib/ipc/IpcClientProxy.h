/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2012 Nick Bolton
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

#include "ipc/Ipc.h"
#include "arch/IArchMultithread.h"
#include "base/EventTypes.h"
#include "base/Event.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <string>

namespace barrier { class IStream; }
class IpcMessage;
class IpcCommandMessage;
class IpcHelloMessage;
class IpcNodeReadyV2Message;
class IEventQueue;

class IpcClientProxy {
    friend class IpcServer;

public:
    IpcClientProxy(barrier::IStream& stream, IEventQueue* events);
    virtual ~IpcClientProxy();

#if defined(BARRIER_TEST_ENV) || defined(BARRIER_TEST_ACCESS)
public:
#else
private:
#endif
    void                send(const IpcMessage& message);
    bool                tryAddSendRef();
    void                releaseSendRef();
    void                waitForSendRefs();
    void                handleData(const Event&, void*);
    void                handleDisconnect(const Event&, void*);
    void                handleWriteError(const Event&, void*);
    IpcHelloMessage*    parseHello();
    IpcMessage*         parseReady();
    IpcNodeReadyV2Message* parseReadyV2();
    IpcCommandMessage*    parseCommand();
    void                disconnect();
    bool                matchesInputReadiness(UInt32 processId,
                                              UInt32 sessionId,
                                              const std::string& desktopName,
                                              const std::string& buildId,
                                              std::uint64_t queryNonce,
                                              bool requireDesktopMatch,
                                              std::string* reportedDesktopName) const;

#if defined(BARRIER_TEST_ENV) || defined(BARRIER_TEST_ACCESS)
public:
#else
private:
#endif
    barrier::IStream&    m_stream;
    std::atomic<EIpcClientType> m_clientType;
    std::atomic<UInt32> m_processId;
    std::atomic<bool>   m_ready;
    std::atomic<bool>   m_inputReady;
    std::atomic<bool>    m_disconnecting;
    bool                m_deleting;
    UInt32              m_sendRefCount;
    std::mutex          m_sendRefMutex;
    std::condition_variable m_sendRefCond;
    std::mutex m_readMutex;
    std::mutex m_writeMutex;
    mutable std::mutex m_readyMutex;
    UInt32 m_readySessionId;
    std::uint64_t m_readyInputGeneration;
    std::string m_readyDesktopName;
    std::string m_readyBuildId;
    std::chrono::steady_clock::time_point m_readyReceivedAt;
    bool m_proofInputReady;
    UInt32 m_proofSessionId;
    std::uint64_t m_proofInputGeneration;
    std::string m_proofDesktopName;
    std::string m_proofBuildId;
    std::uint64_t m_proofQueryNonce;
    std::chrono::steady_clock::time_point m_proofReceivedAt;
    IEventQueue*        m_events;
};
