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

#include "ipc/IpcClientProxy.h"

#include "ipc/Ipc.h"
#include "ipc/IpcMessage.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/XBarrier.h"
#include "io/IStream.h"
#include "arch/Arch.h"
#include "base/TMethodEventJob.h"
#include "base/Log.h"

#include <chrono>

namespace {

const std::chrono::milliseconds kInputReadinessLeaseLifetime(2000);

}

//
// IpcClientProxy
//

IpcClientProxy::IpcClientProxy(barrier::IStream& stream, IEventQueue* events) :
    m_stream(stream),
    m_clientType(kIpcClientUnknown),
    m_processId(0),
    m_ready(false),
    m_inputReady(false),
    m_disconnecting(false),
    m_deleting(false),
    m_sendRefCount(0),
    m_readySessionId(0),
    m_readyInputGeneration(0),
    m_proofInputReady(false),
    m_proofSessionId(0),
    m_proofInputGeneration(0),
    m_proofQueryNonce(0),
    m_events(events)
{
    m_events->adoptHandler(
        m_events->forIStream().inputReady(), stream.getEventTarget(),
        new TMethodEventJob<IpcClientProxy>(
        this, &IpcClientProxy::handleData));

    m_events->adoptHandler(
        m_events->forIStream().outputError(), stream.getEventTarget(),
        new TMethodEventJob<IpcClientProxy>(
        this, &IpcClientProxy::handleWriteError));

    m_events->adoptHandler(
        m_events->forIStream().inputShutdown(), stream.getEventTarget(),
        new TMethodEventJob<IpcClientProxy>(
        this, &IpcClientProxy::handleDisconnect));

    m_events->adoptHandler(
        m_events->forIStream().outputShutdown(), stream.getEventTarget(),
        new TMethodEventJob<IpcClientProxy>(
        this, &IpcClientProxy::handleWriteError));
}

IpcClientProxy::~IpcClientProxy()
{
    m_events->removeHandler(
        m_events->forIStream().inputReady(), m_stream.getEventTarget());
    m_events->removeHandler(
        m_events->forIStream().outputError(), m_stream.getEventTarget());
    m_events->removeHandler(
        m_events->forIStream().inputShutdown(), m_stream.getEventTarget());
    m_events->removeHandler(
        m_events->forIStream().outputShutdown(), m_stream.getEventTarget());

    waitForSendRefs();

    // don't delete the stream while it's being used.
    {
        std::lock_guard<std::mutex> lock_read(m_readMutex);
        std::lock_guard<std::mutex> lock_write(m_writeMutex);
        delete &m_stream;
    }
}

bool
IpcClientProxy::tryAddSendRef()
{
    std::lock_guard<std::mutex> lock(m_sendRefMutex);
    if (m_deleting || m_disconnecting) {
        return false;
    }

    ++m_sendRefCount;
    return true;
}

void
IpcClientProxy::releaseSendRef()
{
    std::lock_guard<std::mutex> lock(m_sendRefMutex);
    assert(m_sendRefCount > 0);
    --m_sendRefCount;
    if (m_sendRefCount == 0) {
        m_sendRefCond.notify_all();
    }
}

void
IpcClientProxy::waitForSendRefs()
{
    std::unique_lock<std::mutex> lock(m_sendRefMutex);
    m_deleting = true;
    m_sendRefCond.wait(lock, [this]() { return m_sendRefCount == 0; });
}

void
IpcClientProxy::handleDisconnect(const Event&, void*)
{
    disconnect();
    LOG((CLOG_DEBUG "ipc client disconnected"));
}

void
IpcClientProxy::handleWriteError(const Event&, void*)
{
    disconnect();
    LOG((CLOG_DEBUG "ipc client write error"));
}

void
IpcClientProxy::handleData(const Event&, void*)
{
    // don't allow the dtor to destroy the stream while we're using it.
    std::lock_guard<std::mutex> lock(m_readMutex);

    LOG((CLOG_DEBUG "start ipc handle data"));

    UInt8 code[4];
    UInt32 n = m_stream.read(code, 4);
    while (n != 0) {

        LOG((CLOG_DEBUG "ipc read: %c%c%c%c",
            code[0], code[1], code[2], code[3]));

        IpcMessage* m = nullptr;
        try {
            if (memcmp(code, kIpcMsgHello, 4) == 0) {
                m = parseHello();
            }
            else if (memcmp(code, kIpcMsgReady, 4) == 0) {
                m = parseReady();
            }
            else if (memcmp(code, kIpcMsgReadyV2, 4) == 0) {
                m = parseReadyV2();
            }
            else if (memcmp(code, kIpcMsgCommand, 4) == 0) {
                const EIpcClientType clientType =
                    m_clientType.load(std::memory_order_acquire);
                if (clientType != kIpcClientGui) {
                    LOG((CLOG_WARN
                        "rejecting ipc command from non-gui client type=%d",
                        static_cast<int>(clientType)));
                    disconnect();
                    return;
                }
                m = parseCommand();
            }
            else {
                LOG((CLOG_ERR "invalid ipc message"));
                disconnect();
                return;
            }
        }
        catch (const XBase& e) {
            LOG((CLOG_WARN "rejecting malformed ipc message: %s", e.what()));
            disconnect();
            return;
        }

        if (m == nullptr) {
            return;
        }

        // don't delete with this event; the data is passed to a new event.
        Event e(m_events->forIpcClientProxy().messageReceived(), this, NULL, Event::kDontFreeData);
        e.setDataObject(m);
        m_events->addEvent(e);

        n = m_stream.read(code, 4);
    }

    LOG((CLOG_DEBUG "finished ipc handle data"));
}

void
IpcClientProxy::send(const IpcMessage& message)
{
    // don't allow other threads to write until we've finished the entire
    // message. stream write is locked, but only for that single write.
    // also, don't allow the dtor to destroy the stream while we're using it.
    std::lock_guard<std::mutex> lock(m_writeMutex);

    LOG((CLOG_DEBUG4 "ipc write: %d", message.type()));

    switch (message.type()) {
    case kIpcLogLine: {
        const IpcLogLineMessage& llm = static_cast<const IpcLogLineMessage&>(message);
        const std::string logLine = llm.logLine();
        ProtocolUtil::writef(&m_stream, kIpcMsgLogLine, &logLine);
        break;
    }

    case kIpcShutdown:
        ProtocolUtil::writef(&m_stream, kIpcMsgShutdown);
        break;

    case kIpcReadyQuery: {
        const IpcInputReadyQueryMessage& query =
            static_cast<const IpcInputReadyQueryMessage&>(message);
        const UInt32 queryNonceHigh =
            static_cast<UInt32>(query.queryNonce() >> 32);
        const UInt32 queryNonceLow =
            static_cast<UInt32>(query.queryNonce() & 0xffffffffu);
        ProtocolUtil::writef(&m_stream, kIpcMsgReadyQuery,
                             queryNonceHigh, queryNonceLow);
        break;
    }

    default:
        LOG((CLOG_ERR "ipc message not supported: %d", message.type()));
        break;
    }
}

IpcHelloMessage*
IpcClientProxy::parseHello()
{
    UInt8 type;
    UInt32 processId = 0;
    ProtocolUtil::readf(&m_stream, kIpcMsgHello + 4, &type, &processId);

    if (type != kIpcClientGui && type != kIpcClientNode) {
        LOG((CLOG_WARN "rejecting invalid ipc client type=%d", type));
        m_clientType.store(kIpcClientUnknown, std::memory_order_release);
        m_processId.store(0, std::memory_order_release);
        disconnect();
        return nullptr;
    }

    const EIpcClientType clientType = static_cast<EIpcClientType>(type);
    m_clientType.store(clientType, std::memory_order_release);
    m_processId.store(processId, std::memory_order_release);

    // must be deleted by event handler.
    return new IpcHelloMessage(clientType, processId);
}

IpcMessage*
IpcClientProxy::parseReady()
{
    if (m_clientType.load(std::memory_order_acquire) != kIpcClientNode ||
        m_processId.load(std::memory_order_acquire) == 0) {
        LOG((CLOG_WARN "rejecting ipc ready before a valid node hello"));
        disconnect();
        return nullptr;
    }

    m_ready = true;
    return new IpcNodeReadyMessage();
}

IpcNodeReadyV2Message*
IpcClientProxy::parseReadyV2()
{
    const EIpcClientType clientType =
        m_clientType.load(std::memory_order_acquire);
    const UInt32 expectedProcessId =
        m_processId.load(std::memory_order_acquire);
    if (clientType != kIpcClientNode || expectedProcessId == 0) {
        LOG((CLOG_WARN "rejecting ipc capability ready before a valid node hello"));
        disconnect();
        return nullptr;
    }

    UInt32 processId = 0;
    UInt32 sessionId = 0;
    UInt32 generationHigh = 0;
    UInt32 generationLow = 0;
    UInt8 inputReady = 0;
    std::string desktopName;
    std::string buildId;
    UInt32 queryNonceHigh = 0;
    UInt32 queryNonceLow = 0;
    if (!ProtocolUtil::readf(&m_stream, kIpcMsgReadyV2 + 4,
                             &processId, &sessionId,
                             &generationHigh, &generationLow,
                             &inputReady, &desktopName, &buildId,
                             &queryNonceHigh, &queryNonceLow)) {
        LOG((CLOG_WARN "incomplete ipc capability ready message"));
        disconnect();
        return nullptr;
    }

    if (processId != expectedProcessId || inputReady > 1) {
        LOG((CLOG_WARN
            "rejecting invalid ipc capability ready process=%u expected=%u ready=%u",
            processId, expectedProcessId, inputReady));
        disconnect();
        return nullptr;
    }

    const std::uint64_t inputGeneration =
        (static_cast<std::uint64_t>(generationHigh) << 32) | generationLow;
    const std::uint64_t queryNonce =
        (static_cast<std::uint64_t>(queryNonceHigh) << 32) | queryNonceLow;
    {
        std::lock_guard<std::mutex> lock(m_readyMutex);
        const std::chrono::steady_clock::time_point receivedAt =
            std::chrono::steady_clock::now();
        if (queryNonce == 0) {
            const bool invalidatesProof = m_proofQueryNonce != 0 &&
                (!inputReady ||
                 m_proofSessionId != sessionId ||
                 m_proofInputGeneration != inputGeneration ||
                 m_proofDesktopName != desktopName ||
                 m_proofBuildId != buildId);
            if (invalidatesProof) {
                m_proofInputReady = false;
                m_proofQueryNonce = 0;
                m_proofReceivedAt =
                    std::chrono::steady_clock::time_point();
            }
            m_readySessionId = sessionId;
            m_readyInputGeneration = inputGeneration;
            m_readyDesktopName = desktopName;
            m_readyBuildId = buildId;
            m_readyReceivedAt = receivedAt;
            m_inputReady.store(inputReady != 0, std::memory_order_release);
        }
        else {
            m_proofInputReady = inputReady != 0;
            m_proofSessionId = sessionId;
            m_proofInputGeneration = inputGeneration;
            m_proofDesktopName = desktopName;
            m_proofBuildId = buildId;
            m_proofQueryNonce = queryNonce;
            m_proofReceivedAt = receivedAt;
        }
    }
    m_ready.store(true, std::memory_order_release);

    return new IpcNodeReadyV2Message(
        processId, sessionId, inputGeneration, inputReady != 0,
        desktopName, buildId, queryNonce);
}

bool
IpcClientProxy::matchesInputReadiness(UInt32 processId, UInt32 sessionId,
                                      const std::string& desktopName,
                                      const std::string& buildId,
                                      std::uint64_t queryNonce,
                                      bool requireDesktopMatch,
                                      std::string* reportedDesktopName) const
{
    if (m_disconnecting.load(std::memory_order_acquire) ||
        m_clientType.load(std::memory_order_acquire) != kIpcClientNode ||
        m_processId.load(std::memory_order_acquire) != processId) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_readyMutex);
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    const bool useProof = queryNonce != 0;
    const bool inputReady = useProof
        ? m_proofInputReady
        : m_inputReady.load(std::memory_order_relaxed);
    const UInt32 readySessionId = useProof
        ? m_proofSessionId : m_readySessionId;
    const std::uint64_t inputGeneration = useProof
        ? m_proofInputGeneration : m_readyInputGeneration;
    const std::string& readyDesktopName = useProof
        ? m_proofDesktopName : m_readyDesktopName;
    const std::string& readyBuildId = useProof
        ? m_proofBuildId : m_readyBuildId;
    const std::uint64_t readyQueryNonce = useProof
        ? m_proofQueryNonce : 0;
    const std::chrono::steady_clock::time_point receivedAt = useProof
        ? m_proofReceivedAt : m_readyReceivedAt;
    const bool desktopMatches = requireDesktopMatch
        ? readyDesktopName == desktopName
        : !readyDesktopName.empty();

    const bool matches = inputReady &&
        readySessionId == sessionId &&
        desktopMatches &&
        inputGeneration != 0 &&
        readyBuildId == buildId &&
        readyQueryNonce == queryNonce &&
        receivedAt != std::chrono::steady_clock::time_point() &&
        now - receivedAt <= kInputReadinessLeaseLifetime;
    if (matches && reportedDesktopName != NULL) {
        *reportedDesktopName = readyDesktopName;
    }
    return matches;
}

IpcCommandMessage*
IpcClientProxy::parseCommand()
{
    std::string command;
    UInt8 elevate;
    ProtocolUtil::readf(&m_stream, kIpcMsgCommand + 4, &command, &elevate);

    // must be deleted by event handler.
    return new IpcCommandMessage(command, elevate);
}

void
IpcClientProxy::disconnect()
{
    bool expected = false;
    if (!m_disconnecting.compare_exchange_strong(expected, true)) {
        return;
    }

    LOG((CLOG_DEBUG "ipc disconnect, closing stream"));
    m_stream.close();
    m_events->addEvent(Event(m_events->forIpcClientProxy().disconnected(), this));
}
