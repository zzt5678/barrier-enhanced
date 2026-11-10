#include "platform/OSXInputLoop.h"
#include <stdexcept>

OSXInputLoop::OSXInputLoop()
{
    m_thread = std::thread([this] { run(); });
    std::unique_lock<std::mutex> lock(m_mutex);
    m_changed.wait(lock, [this] { return m_ready; });
    if (m_startError) {
        lock.unlock();
        m_thread.join();
        std::rethrow_exception(m_startError);
    }
}

OSXInputLoop::~OSXInputLoop() { stop(); }

bool OSXInputLoop::isOwner() const
{
    return std::this_thread::get_id() == m_thread.get_id();
}

bool OSXInputLoop::invoke(std::function<void()> function, std::chrono::milliseconds deadline)
{
    if (isOwner()) { function(); return true; }
    auto command = std::make_shared<Command>();
    command->run = std::move(function);
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_stopping) return false;
    m_pending.push_back(command);
    CFRunLoopSourceSignal(m_source);
    CFRunLoopWakeUp(m_loop);
    if (!m_changed.wait_for(lock, deadline, [&] { return command->started || command->done; })) {
        command->cancelled = true;
        return false;
    }
    m_changed.wait(lock, [&] { return command->done; });
    if (command->error) std::rethrow_exception(command->error);
    return !command->cancelled;
}

OSXInputLoop::ReleaseResult OSXInputLoop::invokeRelease(
    std::function<void()> function, std::chrono::milliseconds deadline)
{
    if (isOwner()) { function(); return ReleaseResult::Completed; }
    auto command = std::make_shared<Command>();
    command->run = std::move(function);
    command->mandatoryRelease = true;
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_stopping) return ReleaseResult::Rejected;
    m_pending.push_back(command);
    CFRunLoopSourceSignal(m_source);
    CFRunLoopWakeUp(m_loop);
    if (!m_changed.wait_for(lock, deadline, [&] { return command->done; })) {
        return ReleaseResult::Pending;
    }
    if (command->error) std::rethrow_exception(command->error);
    return command->cancelled ? ReleaseResult::Rejected : ReleaseResult::Completed;
}

void OSXInputLoop::stop(std::function<void()> cleanup)
{
    if (!m_thread.joinable()) return;
    // The screen lifetime is owned by the business thread, never the tap.
    if (isOwner()) std::terminate();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cleanup = std::move(cleanup);
        m_stopping = true;
        CFRunLoopSourceSignal(m_source);
        CFRunLoopWakeUp(m_loop);
    }
    m_thread.join();
}

void OSXInputLoop::perform(void* context)
{
    static_cast<OSXInputLoop*>(context)->drain();
}

void OSXInputLoop::drain()
{
    for (;;) {
        std::shared_ptr<Command> command;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_pending.empty()) break;
            command = m_pending.front();
            m_pending.pop_front();
            // Accepted releases own their obligation even after their caller
            // times out. Drain them before cleanup destroys the input state.
            if ((m_stopping && !command->mandatoryRelease) || command->cancelled) {
                command->cancelled = command->done = true;
                m_changed.notify_all();
                continue;
            }
            command->started = true;
            m_changed.notify_all();
        }
        std::exception_ptr error;
        try { command->run(); } catch (...) { error = std::current_exception(); }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            command->error = error;
            command->done = true;
            m_changed.notify_all();
        }
    }
    bool stopping;
    { std::lock_guard<std::mutex> lock(m_mutex); stopping = m_stopping; }
    if (stopping) CFRunLoopStop(m_loop);
}

void OSXInputLoop::run()
{
    pthread_setname_np("Weave input");
    CFRunLoopSourceContext context = {};
    context.info = this;
    context.perform = perform;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_loop = CFRunLoopGetCurrent();
        m_source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &context);
        if (!m_source) m_startError = std::make_exception_ptr(std::runtime_error("cannot create input loop"));
        else CFRunLoopAddSource(m_loop, m_source, kCFRunLoopDefaultMode);
        m_ready = true;
        m_changed.notify_all();
    }
    if (!m_source) return;
    CFRunLoopRun();
    if (m_cleanup) m_cleanup();
    CFRunLoopRemoveSource(m_loop, m_source, kCFRunLoopDefaultMode);
    CFRelease(m_source);
}
