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
#include "barrier/protocol_types.h"
#include "io/IStream.h"
#include "base/TMethodEventJob.h"
#include "base/Log.h"

#include <chrono>
#include <algorithm>
#include <cstring>

namespace {

const std::chrono::milliseconds kInputReadinessLeaseLifetime(2000);

enum class FrameProbeState {
    NeedData,
    Complete,
    Invalid
};

struct FrameProbe {
    FrameProbeState state;
    std::size_t frameSize;
    std::size_t bytesNeeded;
};

UInt32 readUInt32(const UInt8* bytes)
{
    return (static_cast<UInt32>(bytes[0]) << 24) |
           (static_cast<UInt32>(bytes[1]) << 16) |
           (static_cast<UInt32>(bytes[2]) << 8) |
            static_cast<UInt32>(bytes[3]);
}

FrameProbe needBytes(const std::vector<UInt8>& buffer, std::size_t target)
{
    if (buffer.size() < target) {
        return { FrameProbeState::NeedData, 0, target - buffer.size() };
    }
    return { FrameProbeState::Complete, target, 0 };
}

FrameProbe probeClientFrame(const std::vector<UInt8>& buffer)
{
    if (buffer.size() < 4) {
        return needBytes(buffer, 4);
    }

    if (std::memcmp(buffer.data(), kIpcMsgHello, 4) == 0) {
        return needBytes(buffer, 9);
    }
    if (std::memcmp(buffer.data(), kIpcMsgReady, 4) == 0) {
        return needBytes(buffer, 4);
    }
    if (std::memcmp(buffer.data(), kIpcMsgActivated, 4) == 0) {
        return needBytes(buffer, 16);
    }
    if (std::memcmp(buffer.data(), kIpcMsgCommand, 4) == 0) {
        if (buffer.size() < 8) {
            return needBytes(buffer, 8);
        }
        const UInt32 commandLength = readUInt32(buffer.data() + 4);
        if (commandLength > PROTOCOL_MAX_STRING_LENGTH) {
            return { FrameProbeState::Invalid, 0, 0 };
        }
        return needBytes(buffer, 9u + commandLength);
    }
    if (std::memcmp(buffer.data(), kIpcMsgStopRequest, 4) == 0) {
        return needBytes(buffer, 12);
    }
    if (std::memcmp(buffer.data(), kIpcMsgReadyV2, 4) == 0) {
        // Fixed fields through the first string length prefix.
        if (buffer.size() < 25) {
            return needBytes(buffer, 25);
        }
        const UInt32 desktopLength = readUInt32(buffer.data() + 21);
        if (desktopLength > PROTOCOL_MAX_STRING_LENGTH) {
            return { FrameProbeState::Invalid, 0, 0 };
        }

        const std::size_t buildLengthOffset = 25u + desktopLength;
        if (buffer.size() < buildLengthOffset + 4) {
            return needBytes(buffer, buildLengthOffset + 4);
        }
        const UInt32 buildLength =
            readUInt32(buffer.data() + buildLengthOffset);
        if (buildLength > PROTOCOL_MAX_STRING_LENGTH) {
            return { FrameProbeState::Invalid, 0, 0 };
        }
        return needBytes(buffer,
                         buildLengthOffset + 4u + buildLength + 8u);
    }

    return { FrameProbeState::Invalid, 0, 0 };
}

std::string readString(const std::vector<UInt8>& buffer, std::size_t& offset)
{
    const UInt32 length = readUInt32(buffer.data() + offset);
    offset += 4;
    const char* begin = reinterpret_cast<const char*>(buffer.data() + offset);
    std::string result(begin, begin + length);
    offset += length;
    return result;
}

}

//
// IpcClientProxy
//

IpcClientProxy::IpcClientProxy(barrier::IStream& stream, IEventQueue* events,
                               const IpcPeerAuthContext& peerAuth) :
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
    m_activationChallengeNonce(0),
    m_activatedNonce(0),
    m_peerAuth(peerAuth),
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
    waitForSendRefs(
        barrier::kFinalThreadShutdownDeadlineSeconds,
        barrier::terminateProcessForFinalThreadShutdown);
}

void
IpcClientProxy::waitForSendRefs(
    double timeoutSeconds,
    const barrier::FinalProcessTerminator& terminator)
{
    std::unique_lock<std::mutex> lock(m_sendRefMutex);
    m_deleting = true;
    barrier::waitForFinalThreadShutdown(
        "ipc client proxy send operations",
        timeoutSeconds,
        [this, &lock](double timeout) {
            return m_sendRefCond.wait_for(
                lock,
                std::chrono::duration<double>(timeout),
                [this]() { return m_sendRefCount == 0; });
        },
        terminator);
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

    while (!m_disconnecting.load(std::memory_order_acquire)) {
        const FrameProbe probe = probeClientFrame(m_receiveBuffer);
        if (probe.state == FrameProbeState::Invalid) {
            LOG((CLOG_ERR "invalid ipc message"));
            disconnect();
            return;
        }

        if (probe.state == FrameProbeState::Complete) {
            LOG((CLOG_DEBUG "ipc read: %c%c%c%c",
                 m_receiveBuffer[0], m_receiveBuffer[1],
                 m_receiveBuffer[2], m_receiveBuffer[3]));
            IpcMessage* message = parseBufferedMessage();
            if (message == nullptr) {
                return;
            }

            m_receiveBuffer.erase(
                m_receiveBuffer.begin(),
                m_receiveBuffer.begin() + probe.frameSize);

            // don't delete with this event; the data is passed to a new event.
            Event event(m_events->forIpcClientProxy().messageReceived(),
                        this, NULL, Event::kDontFreeData);
            event.setDataObject(message);
            m_events->addEvent(event);
            continue;
        }

        UInt8 bytes[4096];
        const UInt32 requested = static_cast<UInt32>(
            std::min<std::size_t>(probe.bytesNeeded, sizeof(bytes)));
        const UInt32 count = m_stream.read(bytes, requested);
        if (count == 0) {
            break;
        }
        if (count > requested) {
            LOG((CLOG_ERR "ipc stream returned more bytes than requested"));
            disconnect();
            return;
        }
        m_receiveBuffer.insert(
            m_receiveBuffer.end(), bytes, bytes + count);
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

    case kIpcStopAck: {
        const IpcStopAckMessage& ack =
            static_cast<const IpcStopAckMessage&>(message);
        const UInt32 requestHigh =
            static_cast<UInt32>(ack.requestId() >> 32);
        const UInt32 requestLow =
            static_cast<UInt32>(ack.requestId() & 0xffffffffu);
        const UInt32 generationHigh =
            static_cast<UInt32>(ack.commandGeneration() >> 32);
        const UInt32 generationLow =
            static_cast<UInt32>(ack.commandGeneration() & 0xffffffffu);
        ProtocolUtil::writef(&m_stream, kIpcMsgStopAck,
                             requestHigh, requestLow,
                             generationHigh, generationLow);
        break;
    }

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

    case kIpcActivate: {
        const IpcActivateNodeMessage& activate =
            static_cast<const IpcActivateNodeMessage&>(message);
        const UInt32 nonceHigh =
            static_cast<UInt32>(activate.activationNonce() >> 32);
        const UInt32 nonceLow =
            static_cast<UInt32>(activate.activationNonce() & 0xffffffffu);
        if (activate.activationNonce() == 0) {
            LOG((CLOG_WARN "refusing zero ipc activation nonce"));
            break;
        }
        {
            std::lock_guard<std::mutex> readyLock(m_readyMutex);
            m_activationChallengeNonce = activate.activationNonce();
            m_activatedNonce = 0;
        }
        ProtocolUtil::writef(&m_stream, kIpcMsgActivate,
                             nonceHigh, nonceLow);
        break;
    }

    default:
        LOG((CLOG_ERR "ipc message not supported: %d", message.type()));
        break;
    }
}

IpcMessage*
IpcClientProxy::parseBufferedMessage()
{
    std::size_t offset = 4;
    if (std::memcmp(m_receiveBuffer.data(), kIpcMsgHello, 4) == 0) {
        const UInt8 type = m_receiveBuffer[offset++];
        const UInt32 processId = readUInt32(m_receiveBuffer.data() + offset);
        return parseHello(type, processId);
    }
    if (std::memcmp(m_receiveBuffer.data(), kIpcMsgReady, 4) == 0) {
        return parseReady();
    }
    if (std::memcmp(m_receiveBuffer.data(), kIpcMsgReadyV2, 4) == 0) {
        const UInt32 processId = readUInt32(m_receiveBuffer.data() + offset);
        offset += 4;
        const UInt32 sessionId = readUInt32(m_receiveBuffer.data() + offset);
        offset += 4;
        const UInt32 generationHigh =
            readUInt32(m_receiveBuffer.data() + offset);
        offset += 4;
        const UInt32 generationLow =
            readUInt32(m_receiveBuffer.data() + offset);
        offset += 4;
        const UInt8 inputReady = m_receiveBuffer[offset++];
        const std::string desktopName = readString(m_receiveBuffer, offset);
        const std::string buildId = readString(m_receiveBuffer, offset);
        const UInt32 queryNonceHigh =
            readUInt32(m_receiveBuffer.data() + offset);
        offset += 4;
        const UInt32 queryNonceLow =
            readUInt32(m_receiveBuffer.data() + offset);
        return parseReadyV2(
            processId, sessionId,
            (static_cast<std::uint64_t>(generationHigh) << 32) |
                generationLow,
            inputReady, desktopName, buildId,
            (static_cast<std::uint64_t>(queryNonceHigh) << 32) |
                queryNonceLow);
    }
    if (std::memcmp(m_receiveBuffer.data(), kIpcMsgActivated, 4) == 0) {
        const UInt32 processId = readUInt32(m_receiveBuffer.data() + offset);
        offset += 4;
        const UInt32 nonceHigh = readUInt32(m_receiveBuffer.data() + offset);
        offset += 4;
        const UInt32 nonceLow = readUInt32(m_receiveBuffer.data() + offset);
        return parseActivated(
            processId,
            (static_cast<std::uint64_t>(nonceHigh) << 32) | nonceLow);
    }
    if (std::memcmp(m_receiveBuffer.data(), kIpcMsgCommand, 4) == 0) {
        const EIpcClientType clientType =
            m_clientType.load(std::memory_order_acquire);
        if (clientType != kIpcClientGui) {
            LOG((CLOG_WARN
                "rejecting ipc command from non-gui client type=%d",
                static_cast<int>(clientType)));
            disconnect();
            return nullptr;
        }
        const std::string command = readString(m_receiveBuffer, offset);
        return parseCommand(command, m_receiveBuffer[offset]);
    }
    if (std::memcmp(m_receiveBuffer.data(), kIpcMsgStopRequest, 4) == 0) {
        const EIpcClientType clientType =
            m_clientType.load(std::memory_order_acquire);
        if (clientType != kIpcClientGui) {
            LOG((CLOG_WARN
                "rejecting ipc stop request from non-gui client type=%d",
                static_cast<int>(clientType)));
            disconnect();
            return nullptr;
        }
        const UInt32 requestHigh =
            readUInt32(m_receiveBuffer.data() + offset);
        offset += 4;
        const UInt32 requestLow =
            readUInt32(m_receiveBuffer.data() + offset);
        return parseStopRequest(
            (static_cast<std::uint64_t>(requestHigh) << 32) | requestLow);
    }

    disconnect();
    return nullptr;
}

IpcHelloMessage*
IpcClientProxy::parseHello(UInt8 type, UInt32 processId)
{
    if (type != kIpcClientGui && type != kIpcClientNode) {
        LOG((CLOG_WARN "rejecting invalid ipc client type=%d", type));
        m_clientType.store(kIpcClientUnknown, std::memory_order_release);
        m_processId.store(0, std::memory_order_release);
        disconnect();
        return nullptr;
    }

    const EIpcClientType clientType = static_cast<EIpcClientType>(type);
    std::string rejectReason;
    if (!m_peerAuth.authorizes(clientType, processId, &rejectReason)) {
        LOG((CLOG_WARN "rejecting unauthenticated ipc hello: %s",
             rejectReason.c_str()));
        m_clientType.store(kIpcClientUnknown, std::memory_order_release);
        m_processId.store(0, std::memory_order_release);
        disconnect();
        return nullptr;
    }
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
IpcClientProxy::parseReadyV2(
    UInt32 processId, UInt32 sessionId,
    std::uint64_t inputGeneration, UInt8 inputReady,
    const std::string& desktopName, const std::string& buildId,
    std::uint64_t queryNonce)
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

    if (processId != expectedProcessId || inputReady > 1) {
        LOG((CLOG_WARN
            "rejecting invalid ipc capability ready process=%u expected=%u ready=%u",
            processId, expectedProcessId, inputReady));
        disconnect();
        return nullptr;
    }

    std::string sessionRejectReason;
    if (!m_peerAuth.authorizesSession(sessionId, &sessionRejectReason)) {
        LOG((CLOG_WARN
            "rejecting ipc capability ready session=%u: %s",
            sessionId, sessionRejectReason.c_str()));
        disconnect();
        return nullptr;
    }

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

IpcNodeActivatedMessage*
IpcClientProxy::parseActivated(
    UInt32 processId, std::uint64_t activationNonce)
{
    const EIpcClientType clientType =
        m_clientType.load(std::memory_order_acquire);
    const UInt32 expectedProcessId =
        m_processId.load(std::memory_order_acquire);
    if (clientType != kIpcClientNode || expectedProcessId == 0) {
        LOG((CLOG_WARN "rejecting ipc activation ack before a valid node hello"));
        disconnect();
        return nullptr;
    }

    if (processId != expectedProcessId || activationNonce == 0) {
        LOG((CLOG_WARN
            "rejecting invalid ipc activation ack process=%u expected=%u nonce=%llu",
            processId, expectedProcessId,
            static_cast<unsigned long long>(activationNonce)));
        disconnect();
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_readyMutex);
        if (m_activationChallengeNonce != activationNonce) {
            LOG((CLOG_WARN
                "rejecting unchallenged ipc activation ack nonce=%llu",
                static_cast<unsigned long long>(activationNonce)));
            disconnect();
            return nullptr;
        }
        m_activatedNonce = activationNonce;
    }
    return new IpcNodeActivatedMessage(processId, activationNonce);
}

IpcInputReadinessResult
IpcClientProxy::inputReadiness(UInt32 processId, UInt32 sessionId,
                               const std::string& desktopName,
                               const std::string& buildId,
                               std::uint64_t queryNonce) const
{
    IpcInputReadinessResult result;
    if (m_disconnecting.load(std::memory_order_acquire) ||
        m_clientType.load(std::memory_order_acquire) != kIpcClientNode ||
        m_processId.load(std::memory_order_acquire) != processId) {
        return result;
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
    const bool validProof = inputReady &&
        readySessionId == sessionId &&
        !readyDesktopName.empty() &&
        inputGeneration != 0 &&
        readyBuildId == buildId &&
        readyQueryNonce == queryNonce &&
        receivedAt != std::chrono::steady_clock::time_point() &&
        receivedAt <= now &&
        now - receivedAt <= kInputReadinessLeaseLifetime;
    if (!validProof) {
        return result;
    }
    result.match = readyDesktopName == desktopName
        ? IpcInputReadinessMatch::Exact
        : IpcInputReadinessMatch::DesktopMismatch;
    result.desktopName = readyDesktopName;
    return result;
}

bool
IpcClientProxy::matchesActivation(UInt32 processId,
                                  std::uint64_t activationNonce) const
{
    if (processId == 0 || activationNonce == 0 ||
        m_disconnecting.load(std::memory_order_acquire) ||
        m_clientType.load(std::memory_order_acquire) != kIpcClientNode ||
        m_processId.load(std::memory_order_acquire) != processId) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_readyMutex);
    return m_activationChallengeNonce == activationNonce &&
        m_activatedNonce == activationNonce;
}

IpcCommandMessage*
IpcClientProxy::parseCommand(const std::string& command, UInt8 elevate)
{
    // The wire has no origin fields. Bind the command to the identity captured
    // from the accepted socket, not to the self-reported hello process ID.
    IpcCommandMessage* message = new IpcCommandMessage(command, elevate);
    message->setOrigin(m_peerAuth.commandOrigin());

    // must be deleted by event handler.
    return message;
}

IpcStopRequestMessage*
IpcClientProxy::parseStopRequest(std::uint64_t requestId)
{
    if (requestId == 0 || !m_peerAuth.hasKernelIdentity()) {
        LOG((CLOG_WARN "rejecting invalid or unauthenticated ipc stop request"));
        disconnect();
        return nullptr;
    }

    IpcStopRequestMessage* message = new IpcStopRequestMessage(requestId);
    message->setOrigin(m_peerAuth.commandOrigin());
    return message;
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
