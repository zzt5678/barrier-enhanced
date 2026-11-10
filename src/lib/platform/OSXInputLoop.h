#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

// Serial owner for the synchronous Quartz tap and its cursor transitions.
// Callers must not hold locks needed by input callbacks while invoking it.
class OSXInputLoop {
public:
    OSXInputLoop();
    ~OSXInputLoop();
    OSXInputLoop(const OSXInputLoop&) = delete;
    OSXInputLoop& operator=(const OSXInputLoop&) = delete;

    bool isOwner() const;
    // A queued command that misses the deadline is cancelled, never delayed.
    // Once started, wait for completion: returning early would allow captured
    // references and cursor side effects to outlive the caller's rollback.
    bool invoke(std::function<void()> command,
                std::chrono::milliseconds deadline = std::chrono::milliseconds(1000));
    enum class ReleaseResult { Completed, Pending, Rejected };
    // Releases keep their queue obligation after the caller's deadline. All
    // captures must own data or outlive stop(); never retain caller-stack refs.
    // stop() drains accepted releases before cleanup and rejects new ones.
    // An owner-thread caller executes inline without waiting on another loop.
    ReleaseResult invokeRelease(std::function<void()> command,
                std::chrono::milliseconds deadline = std::chrono::milliseconds(1000));
    void stop(std::function<void()> cleanup = {});

private:
    struct Command {
        std::function<void()> run;
        bool started = false, done = false, cancelled = false;
        bool mandatoryRelease = false;
        std::exception_ptr error;
    };
    void run();
    void drain();
    static void perform(void* context);

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<std::shared_ptr<Command>> m_pending;
    CFRunLoopRef m_loop = nullptr;
    CFRunLoopSourceRef m_source = nullptr;
    bool m_ready = false, m_stopping = false;
    std::exception_ptr m_startError;
    std::function<void()> m_cleanup;
};
