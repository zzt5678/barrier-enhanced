/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2004 Chris Schoeneman
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

#include "platform/OSXEventQueueBuffer.h"

#include "base/Event.h"
#include "base/IEventQueue.h"
#include <stdexcept>

//
// EventQueueTimer
//

class EventQueueTimer { };

//
// OSXEventQueueBuffer
//

OSXEventQueueBuffer::OSXEventQueueBuffer(IEventQueue* events) :
    m_event(NULL),
    m_eventQueue(events),
    m_runLoop(NULL),
    m_wakeupSource(NULL)
{
    // do nothing
}

OSXEventQueueBuffer::~OSXEventQueueBuffer()
{
    if (m_wakeupSource != NULL) {
        CFRunLoopRemoveSource(m_runLoop, m_wakeupSource, kCFRunLoopDefaultMode);
        CFRelease(m_wakeupSource);
    }
    if (m_runLoop != NULL) {
        CFRelease(m_runLoop);
    }
    // release the last event
    if (m_event != NULL) {
        ReleaseEvent(m_event);
    }
}

void
OSXEventQueueBuffer::init()
{
    if (m_runLoop != NULL) {
        return;
    }
    CFRunLoopSourceContext context = {};
    context.perform = [](void*) {};
    m_wakeupSource = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &context);
    if (m_wakeupSource == NULL) {
        throw std::runtime_error("unable to create macOS event queue wakeup source");
    }
    m_runLoop = CFRunLoopGetCurrent();
    CFRetain(m_runLoop);
    CFRunLoopAddSource(m_runLoop, m_wakeupSource, kCFRunLoopDefaultMode);
}

void
OSXEventQueueBuffer::waitForEvent(double timeout)
{
    if (!isEmpty()) {
        return;
    }
    // Cocoa also consumes the process Carbon queue on the main thread. Keep
    // internal events off that shared queue and wake this specific run loop.
    // A signalled source remains pending if the post races this wait.
    CFRunLoopRunInMode(kCFRunLoopDefaultMode,
                      timeout < 0.0 ? 1.0e10 : timeout,
                      true);
}

IEventQueueBuffer::Type
OSXEventQueueBuffer::getEvent(Event& event, UInt32& dataID)
{
    // release the previous event
    if (m_event != NULL) {
        ReleaseEvent(m_event);
        m_event = NULL;
    }

    {
        std::lock_guard<std::mutex> lock(m_userMutex);
        if (!m_userEvents.empty()) {
            dataID = m_userEvents.front();
            m_userEvents.pop_front();
            return kUser;
        }
    }

    // get the next event
    OSStatus error = ReceiveNextEvent(0, NULL, 0.0, true, &m_event);

    // handle the event
    if (error == eventLoopQuitErr) {
        event = Event(Event::kQuit);
        return kSystem;
    }
    else if (error != noErr) {
        return kNone;
    }
    else {
        UInt32 eventClass = GetEventClass(m_event);
        switch (eventClass) {
        case 'Syne':
            dataID = GetEventKind(m_event);
            return kUser;

        default:
            event = Event(Event::kSystem,
                        m_eventQueue->getSystemTarget(), &m_event);
            return kSystem;
        }
    }
}

bool
OSXEventQueueBuffer::addEvent(UInt32 dataID)
{
    if (m_wakeupSource == NULL) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_userMutex);
        m_userEvents.push_back(dataID);
    }
    CFRunLoopSourceSignal(m_wakeupSource);
    CFRunLoopWakeUp(m_runLoop);
    return true;
}

bool
OSXEventQueueBuffer::isEmpty() const
{
    {
        std::lock_guard<std::mutex> lock(m_userMutex);
        if (!m_userEvents.empty()) {
            return false;
        }
    }
    EventRef event;
    OSStatus status = ReceiveNextEvent(0, NULL, 0.0, false, &event);
    return (status == eventLoopTimedOutErr);
}

EventQueueTimer*
OSXEventQueueBuffer::newTimer(double, bool) const
{
    return new EventQueueTimer;
}

void
OSXEventQueueBuffer::deleteTimer(EventQueueTimer* timer) const
{
    delete timer;
}
