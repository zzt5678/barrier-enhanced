/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2011 Nick Bolton
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

#include "base/IEventJob.h"
#include "base/IEventQueue.h"
#include "base/IEventQueueBuffer.h"

#include "test/global/gmock.h"

#include <mutex>
#include <vector>

class MockEventQueue : public IEventQueue
{
private:
    struct Handler {
        Handler(Event::Type type, void* target, IEventJob* job) :
            m_type(type),
            m_target(target),
            m_job(job)
        {
        }

        Event::Type m_type;
        void* m_target;
        IEventJob* m_job;
    };

public:
    MockEventQueue() :
        m_buffer(NULL),
        m_nextEventType(Event::kLast)
    {
        ON_CALL(*this, adoptHandler(::testing::_, ::testing::_, ::testing::_))
            .WillByDefault(::testing::Invoke(
                this, &MockEventQueue::takeHandlerOwnership));
        ON_CALL(*this, removeHandler(::testing::_, ::testing::_))
            .WillByDefault(::testing::Invoke(
                this, &MockEventQueue::releaseHandler));
        ON_CALL(*this, removeHandlers(::testing::_))
            .WillByDefault(::testing::Invoke(
                this, &MockEventQueue::releaseHandlers));
        ON_CALL(*this, addEvent(::testing::_))
            .WillByDefault(::testing::Invoke(
                this, &MockEventQueue::takeEventOwnership));
        ON_CALL(*this, registerTypeOnce(::testing::_, ::testing::_))
            .WillByDefault(::testing::Invoke(
                this, &MockEventQueue::registerEventTypeOnce));
        ON_CALL(*this, adoptBuffer(::testing::_))
            .WillByDefault(::testing::Invoke(
                this, &MockEventQueue::takeBufferOwnership));
    }

    ~MockEventQueue() override
    {
        std::vector<Handler> handlers;
        std::vector<Event> events;
        IEventQueueBuffer* buffer = NULL;
        {
            std::lock_guard<std::mutex> lock(m_ownershipMutex);
            handlers.swap(m_handlers);
            events.swap(m_events);
            buffer = m_buffer;
            m_buffer = NULL;
        }

        for (std::vector<Handler>::iterator handler = handlers.begin();
             handler != handlers.end(); ++handler) {
            delete handler->m_job;
        }
        for (std::vector<Event>::const_iterator event = events.begin();
             event != events.end(); ++event) {
            Event::deleteData(*event);
        }
        delete buffer;
    }

    MOCK_METHOD0(loop, void());
    MOCK_METHOD2(newOneShotTimer, EventQueueTimer*(double, void*));
    MOCK_METHOD2(newTimer, EventQueueTimer*(double, void*));
    MOCK_METHOD2(getEvent, bool(Event&, double));
    MOCK_METHOD1(adoptBuffer, void(IEventQueueBuffer*));
    MOCK_METHOD2(registerTypeOnce, Event::Type(Event::Type&, const char*));
    MOCK_METHOD1(removeHandlers, void(void*));
    MOCK_METHOD1(registerType, Event::Type(const char*));
    MOCK_CONST_METHOD0(isEmpty, bool());
    MOCK_METHOD3(adoptHandler, void(Event::Type, void*, IEventJob*));
    MOCK_METHOD1(getTypeName, const char*(Event::Type));
    MOCK_METHOD1(addEvent, void(const Event&));
    MOCK_METHOD2(removeHandler, void(Event::Type, void*));
    MOCK_METHOD1(dispatchEvent, bool(const Event&));
    MOCK_CONST_METHOD2(getHandler, IEventJob*(Event::Type, void*));
    MOCK_CONST_METHOD0(getQueuedEventCount, size_t());
    MOCK_METHOD1(deleteTimer, void(EventQueueTimer*));
    MOCK_CONST_METHOD1(getRegisteredType, Event::Type(const std::string&));
    MOCK_METHOD0(getSystemTarget, void*());
    MOCK_METHOD0(forClient, ClientEvents&());
    MOCK_METHOD0(forIStream, IStreamEvents&());
    MOCK_METHOD0(forIpcClient, IpcClientEvents&());
    MOCK_METHOD0(forIpcClientProxy, IpcClientProxyEvents&());
    MOCK_METHOD0(forIpcServer, IpcServerEvents&());
    MOCK_METHOD0(forIpcServerProxy, IpcServerProxyEvents&());
    MOCK_METHOD0(forIDataSocket, IDataSocketEvents&());
    MOCK_METHOD0(forIListenSocket, IListenSocketEvents&());
    MOCK_METHOD0(forISocket, ISocketEvents&());
    MOCK_METHOD0(forOSXScreen, OSXScreenEvents&());
    MOCK_METHOD0(forClientListener, ClientListenerEvents&());
    MOCK_METHOD0(forClientProxy, ClientProxyEvents&());
    MOCK_METHOD0(forClientProxyUnknown, ClientProxyUnknownEvents&());
    MOCK_METHOD0(forServer, ServerEvents&());
    MOCK_METHOD0(forServerApp, ServerAppEvents&());
    MOCK_METHOD0(forIKeyState, IKeyStateEvents&());
    MOCK_METHOD0(forIPrimaryScreen, IPrimaryScreenEvents&());
    MOCK_METHOD0(forIScreen, IScreenEvents&());
    MOCK_METHOD0(forClipboard, ClipboardEvents&());
    MOCK_METHOD0(forFile, FileEvents&());
    MOCK_CONST_METHOD0(waitForReady, void());

private:
    void takeHandlerOwnership(Event::Type type, void* target, IEventJob* job)
    {
        IEventJob* replaced = NULL;
        bool replacedExisting = false;
        {
            std::lock_guard<std::mutex> lock(m_ownershipMutex);
            for (std::vector<Handler>::iterator handler = m_handlers.begin();
                 handler != m_handlers.end(); ++handler) {
                if (handler->m_type == type && handler->m_target == target) {
                    replaced = handler->m_job;
                    handler->m_job = job;
                    replacedExisting = true;
                    break;
                }
            }
            if (!replacedExisting) {
                try {
                    m_handlers.push_back(Handler(type, target, job));
                }
                catch (...) {
                    delete job;
                    throw;
                }
            }
        }
        delete replaced;
    }

    void releaseHandler(Event::Type type, void* target)
    {
        IEventJob* released = NULL;
        {
            std::lock_guard<std::mutex> lock(m_ownershipMutex);
            for (std::vector<Handler>::iterator handler = m_handlers.begin();
                 handler != m_handlers.end(); ++handler) {
                if (handler->m_type == type && handler->m_target == target) {
                    released = handler->m_job;
                    m_handlers.erase(handler);
                    break;
                }
            }
        }
        delete released;
    }

    void releaseHandlers(void* target)
    {
        for (;;) {
            IEventJob* released = NULL;
            {
                std::lock_guard<std::mutex> lock(m_ownershipMutex);
                for (std::vector<Handler>::iterator handler = m_handlers.begin();
                     handler != m_handlers.end(); ++handler) {
                    if (handler->m_target == target) {
                        released = handler->m_job;
                        m_handlers.erase(handler);
                        break;
                    }
                }
            }
            if (released == NULL) {
                return;
            }
            delete released;
        }
    }

    void takeEventOwnership(const Event& event)
    {
        std::lock_guard<std::mutex> lock(m_ownershipMutex);
        try {
            m_events.push_back(event);
        }
        catch (...) {
            Event::deleteData(event);
            throw;
        }
    }

    Event::Type registerEventTypeOnce(Event::Type& type, const char*)
    {
        std::lock_guard<std::mutex> lock(m_ownershipMutex);
        if (type == Event::kUnknown) {
            type = m_nextEventType++;
        }
        return type;
    }

    void takeBufferOwnership(IEventQueueBuffer* buffer)
    {
        IEventQueueBuffer* replaced = NULL;
        {
            std::lock_guard<std::mutex> lock(m_ownershipMutex);
            replaced = m_buffer;
            m_buffer = buffer;
        }
        delete replaced;
    }

private:
    std::mutex m_ownershipMutex;
    std::vector<Handler> m_handlers;
    std::vector<Event> m_events;
    IEventQueueBuffer* m_buffer;
    Event::Type m_nextEventType;
};
