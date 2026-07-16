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
#include "barrier/XBarrier.h"
#include "io/IStream.h"
#include "base/TMethodEventJob.h"
#include "base/Log.h"

//
// IpcServerProxy
//

IpcServerProxy::IpcServerProxy(barrier::IStream& stream, IEventQueue* events) :
    m_stream(stream),
    m_events(events)
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

    UInt8 code[4];
    UInt32 n = m_stream.read(code, 4);
    while (n != 0) {

        LOG((CLOG_DEBUG "ipc read: %c%c%c%c",
            code[0], code[1], code[2], code[3]));

        IpcMessage* m = nullptr;
        try {
            if (memcmp(code, kIpcMsgLogLine, 4) == 0) {
                m = parseLogLine();
            }
            else if (memcmp(code, kIpcMsgShutdown, 4) == 0) {
                m = new IpcShutdownMessage();
            }
            else if (memcmp(code, kIpcMsgReadyQuery, 4) == 0) {
                m = parseInputReadyQuery();
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
        Event e(m_events->forIpcServerProxy().messageReceived(), this, NULL, Event::kDontFreeData);
        e.setDataObject(m);
        m_events->addEvent(e);

        n = m_stream.read(code, 4);
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

IpcLogLineMessage*
IpcServerProxy::parseLogLine()
{
    std::string logLine;
    ProtocolUtil::readf(&m_stream, kIpcMsgLogLine + 4, &logLine);

    // must be deleted by event handler.
    return new IpcLogLineMessage(logLine);
}

IpcInputReadyQueryMessage*
IpcServerProxy::parseInputReadyQuery()
{
    UInt32 queryNonceHigh = 0;
    UInt32 queryNonceLow = 0;
    if (!ProtocolUtil::readf(&m_stream, kIpcMsgReadyQuery + 4,
                             &queryNonceHigh, &queryNonceLow)) {
        LOG((CLOG_WARN "incomplete ipc input readiness query"));
        disconnect();
        return nullptr;
    }

    const std::uint64_t queryNonce =
        (static_cast<std::uint64_t>(queryNonceHigh) << 32) |
        queryNonceLow;
    return new IpcInputReadyQueryMessage(queryNonce);
}

void
IpcServerProxy::disconnect()
{
    LOG((CLOG_DEBUG "ipc disconnect, closing stream"));
    m_stream.close();
}
