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

#include "ipc/IpcServerProxy.h"

#include "ipc/IpcMessage.h"
#include "ipc/Ipc.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "io/IStream.h"
#include "base/TMethodEventJob.h"
#include "base/Log.h"

#include <algorithm>
#include <cstring>

namespace {

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

FrameProbe probeServerFrame(const std::vector<UInt8>& buffer)
{
    if (buffer.size() < 4) {
        return needBytes(buffer, 4);
    }

    if (std::memcmp(buffer.data(), kIpcMsgLogLine, 4) == 0) {
        if (buffer.size() < 8) {
            return needBytes(buffer, 8);
        }
        const UInt32 logLength = readUInt32(buffer.data() + 4);
        if (logLength > PROTOCOL_MAX_STRING_LENGTH) {
            return { FrameProbeState::Invalid, 0, 0 };
        }
        return needBytes(buffer, 8u + logLength);
    }
    if (std::memcmp(buffer.data(), kIpcMsgShutdown, 4) == 0) {
        return needBytes(buffer, 4);
    }
    if (std::memcmp(buffer.data(), kIpcMsgReadyQuery, 4) == 0 ||
        std::memcmp(buffer.data(), kIpcMsgActivate, 4) == 0) {
        return needBytes(buffer, 12);
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
// IpcServerProxy
//

IpcServerProxy::IpcServerProxy(barrier::IStream& stream, IEventQueue* events) :
    m_stream(stream),
    m_events(events),
    m_disconnected(false)
{
    m_events->adoptHandler(m_events->forIStream().inputReady(),
        stream.getEventTarget(),
        new TMethodEventJob<IpcServerProxy>(
        this, &IpcServerProxy::handleData));
}

IpcServerProxy::~IpcServerProxy()
{
    m_events->removeHandler(m_events->forIStream().inputReady(),
        m_stream.getEventTarget());
}

void
IpcServerProxy::handleData(const Event&, void*)
{
    LOG((CLOG_DEBUG "start ipc handle data"));

    while (!m_disconnected) {
        const FrameProbe probe = probeServerFrame(m_receiveBuffer);
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
            Event event(m_events->forIpcServerProxy().messageReceived(),
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
IpcServerProxy::send(const IpcMessage& message)
{
    LOG((CLOG_DEBUG4 "ipc write: %d", message.type()));

    switch (message.type()) {
    case kIpcHello: {
        const IpcHelloMessage& hm = static_cast<const IpcHelloMessage&>(message);
        ProtocolUtil::writef(&m_stream, kIpcMsgHello, hm.clientType(), hm.processId());
        break;
    }

    case kIpcReady:
        ProtocolUtil::writef(&m_stream, kIpcMsgReady);
        break;

    case kIpcReadyV2: {
        const IpcNodeReadyV2Message& ready =
            static_cast<const IpcNodeReadyV2Message&>(message);
        const UInt32 generationHigh =
            static_cast<UInt32>(ready.inputGeneration() >> 32);
        const UInt32 generationLow =
            static_cast<UInt32>(ready.inputGeneration() & 0xffffffffu);
        const UInt32 queryNonceHigh =
            static_cast<UInt32>(ready.queryNonce() >> 32);
        const UInt32 queryNonceLow =
            static_cast<UInt32>(ready.queryNonce() & 0xffffffffu);
        std::string desktopName = ready.desktopName();
        std::string buildId = ready.buildId();
        ProtocolUtil::writef(&m_stream, kIpcMsgReadyV2,
                             ready.processId(), ready.sessionId(),
                             generationHigh, generationLow,
                             ready.inputReady() ? 1u : 0u,
                             &desktopName, &buildId,
                             queryNonceHigh, queryNonceLow);
        break;
    }

    case kIpcActivated: {
        const IpcNodeActivatedMessage& activated =
            static_cast<const IpcNodeActivatedMessage&>(message);
        const UInt32 nonceHigh =
            static_cast<UInt32>(activated.activationNonce() >> 32);
        const UInt32 nonceLow =
            static_cast<UInt32>(activated.activationNonce() & 0xffffffffu);
        ProtocolUtil::writef(&m_stream, kIpcMsgActivated,
                             activated.processId(), nonceHigh, nonceLow);
        break;
    }

    case kIpcCommand: {
        const IpcCommandMessage& cm = static_cast<const IpcCommandMessage&>(message);
        std::string command = cm.command();
        ProtocolUtil::writef(&m_stream, kIpcMsgCommand, &command, cm.elevateMode());
        break;
    }

    default:
        LOG((CLOG_ERR "ipc message not supported: %d", message.type()));
        break;
    }
}

IpcMessage*
IpcServerProxy::parseBufferedMessage()
{
    std::size_t offset = 4;
    if (std::memcmp(m_receiveBuffer.data(), kIpcMsgLogLine, 4) == 0) {
        return parseLogLine(readString(m_receiveBuffer, offset));
    }
    if (std::memcmp(m_receiveBuffer.data(), kIpcMsgShutdown, 4) == 0) {
        return new IpcShutdownMessage();
    }

    const UInt32 nonceHigh = readUInt32(m_receiveBuffer.data() + offset);
    offset += 4;
    const UInt32 nonceLow = readUInt32(m_receiveBuffer.data() + offset);
    const std::uint64_t nonce =
        (static_cast<std::uint64_t>(nonceHigh) << 32) | nonceLow;
    if (std::memcmp(m_receiveBuffer.data(), kIpcMsgReadyQuery, 4) == 0) {
        return parseInputReadyQuery(nonce);
    }
    if (std::memcmp(m_receiveBuffer.data(), kIpcMsgActivate, 4) == 0) {
        return parseActivateNode(nonce);
    }

    disconnect();
    return nullptr;
}

IpcLogLineMessage*
IpcServerProxy::parseLogLine(const std::string& logLine)
{
    // must be deleted by event handler.
    return new IpcLogLineMessage(logLine);
}

IpcInputReadyQueryMessage*
IpcServerProxy::parseInputReadyQuery(std::uint64_t queryNonce)
{
    return new IpcInputReadyQueryMessage(queryNonce);
}

IpcActivateNodeMessage*
IpcServerProxy::parseActivateNode(std::uint64_t activationNonce)
{
    if (activationNonce == 0) {
        LOG((CLOG_WARN "rejecting zero ipc activation nonce"));
        disconnect();
        return nullptr;
    }
    return new IpcActivateNodeMessage(activationNonce);
}

void
IpcServerProxy::disconnect()
{
    if (m_disconnected) {
        return;
    }
    m_disconnected = true;
    LOG((CLOG_DEBUG "ipc disconnect, closing stream"));
    m_stream.close();
}
