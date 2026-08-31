/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/FileReceiveSession.h"

#include "barrier/TransferDigest.h"
#include "mt/ThreadShutdown.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

const size_t FileReceiveSession::kDefaultAsyncQueueLimit;
const size_t FileReceiveSession::kMaxAsyncChunkSize;

namespace {

const std::chrono::milliseconds kSpoolWorkerReaperInitialPoll(10);
const std::chrono::milliseconds kSpoolWorkerReaperMaxPoll(250);

struct RetiredSpoolWorker {
    std::thread* thread;
    std::shared_ptr<std::atomic<bool> > done;
    std::shared_ptr<std::atomic<bool> > reaped;
};

class SpoolWorkerReaper {
public:
    SpoolWorkerReaper() :
        m_stopping(false),
        m_shutdownDeadline(),
        m_reaper(&SpoolWorkerReaper::run, this)
    {
    }

    ~SpoolWorkerReaper()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
            m_shutdownDeadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(static_cast<int>(
                    barrier::kFinalThreadShutdownDeadlineSeconds * 1000.0));
        }
        m_wake.notify_one();
        m_reaper.join();
    }

    void retire(std::thread* thread,
                const std::shared_ptr<std::atomic<bool> >& done,
                const std::shared_ptr<std::atomic<bool> >& reaped)
    {
        if (thread == NULL) {
            reaped->store(true, std::memory_order_release);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            RetiredSpoolWorker worker = { thread, done, reaped };
            m_workers.push_back(worker);
        }
        m_wake.notify_one();
    }

private:
    void run()
    {
        std::chrono::milliseconds pollDelay =
            kSpoolWorkerReaperInitialPoll;
        for (;;) {
            std::vector<RetiredSpoolWorker> ready;
            bool pollExpired = false;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (!m_stopping && m_workers.empty()) {
                    m_wake.wait(lock, [this]() {
                        return m_stopping || !m_workers.empty();
                    });
                    pollDelay = kSpoolWorkerReaperInitialPoll;
                }
                else if (!m_workers.empty()) {
                    const std::chrono::steady_clock::time_point now =
                        std::chrono::steady_clock::now();
                    if (m_stopping && now >= m_shutdownDeadline) {
                        barrier::terminateProcessForFinalThreadShutdown();
                    }
                    std::chrono::steady_clock::time_point wakeAt =
                        now + pollDelay;
                    if (m_stopping && m_shutdownDeadline < wakeAt) {
                        wakeAt = m_shutdownDeadline;
                    }
                    pollExpired = m_wake.wait_until(lock, wakeAt) ==
                        std::cv_status::timeout;
                    if (!pollExpired) {
                        pollDelay = kSpoolWorkerReaperInitialPoll;
                    }
                }

                std::deque<RetiredSpoolWorker>::iterator worker = m_workers.begin();
                while (worker != m_workers.end()) {
                    if (worker->done->load(std::memory_order_acquire)) {
                        ready.push_back(*worker);
                        worker = m_workers.erase(worker);
                    }
                    else {
                        ++worker;
                    }
                }
                if (!ready.empty() || m_workers.empty()) {
                    pollDelay = kSpoolWorkerReaperInitialPoll;
                }
                else if (pollExpired &&
                         pollDelay < kSpoolWorkerReaperMaxPoll) {
                    pollDelay *= 2;
                    if (pollDelay > kSpoolWorkerReaperMaxPoll) {
                        pollDelay = kSpoolWorkerReaperMaxPoll;
                    }
                }
                if (m_stopping && m_workers.empty()) {
                    lock.unlock();
                    release(ready);
                    return;
                }
            }
            release(ready);
        }
    }

    static void release(std::vector<RetiredSpoolWorker>& workers)
    {
        for (std::vector<RetiredSpoolWorker>::iterator worker = workers.begin();
             worker != workers.end(); ++worker) {
            if (worker->thread->joinable()) {
                worker->thread->join();
            }
            delete worker->thread;
            worker->reaped->store(true, std::memory_order_release);
        }
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<RetiredSpoolWorker> m_workers;
    bool m_stopping;
    std::chrono::steady_clock::time_point m_shutdownDeadline;
    std::thread m_reaper;
};

void
retireSpoolWorker(std::thread* worker,
                  const std::shared_ptr<std::atomic<bool> >& done,
                  const std::shared_ptr<std::atomic<bool> >& reaped) noexcept
{
    if (worker == NULL) {
        reaped->store(true, std::memory_order_release);
        return;
    }
    try {
        static SpoolWorkerReaper reaper;
        reaper.retire(worker, done, reaped);
    }
    catch (...) {
        barrier::terminateProcessForFinalThreadShutdown();
    }
}

void
removeSpoolFile(const barrier::fs::path& path)
{
    if (path.empty()) {
        return;
    }
    std::error_code error;
    barrier::fs::remove(path, error);
}

void
invokeNoexcept(std::function<void()> callback) noexcept
{
    if (!callback) {
        return;
    }
    try {
        callback();
    }
    catch (...) {
        // Flow-control release must never escape a worker or teardown path.
    }
}

}

struct FileReceiveSession::AsyncState {
    AsyncState(
        size_t expected,
        size_t queueLimit,
        const std::shared_ptr<std::atomic<bool> >& workerExitGateForTest) :
        expectedSize(expected),
        queueLimit(queueLimit),
        queuedBytes(0),
        writtenBytes(0),
        spoolOpen(false),
        spoolOpenCount(0),
        finishRequested(false),
        cancelRequested(false),
        complete(false),
        failed(false),
        pathTransferred(false),
        backpressureResume(),
        pauseProgress(),
        workerDone(new std::atomic<bool>(false)),
        workerReaped(new std::atomic<bool>(false)),
        workerExitGateForTest(workerExitGateForTest)
    {
    }

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> chunks;
    const size_t expectedSize;
    const size_t queueLimit;
    size_t queuedBytes;
    size_t writtenBytes;
    barrier::fs::path spoolPath;
    bool spoolOpen;
    size_t spoolOpenCount;
    bool finishRequested;
    bool cancelRequested;
    bool complete;
    bool failed;
    bool pathTransferred;
    std::function<void()> backpressureResume;
    std::function<void()> pauseProgress;
    std::shared_ptr<std::atomic<bool> > workerDone;
    std::shared_ptr<std::atomic<bool> > workerReaped;
    std::shared_ptr<std::atomic<bool> > workerExitGateForTest;
};

FileReceiveSession::FileReceiveSession() :
    m_state(kIdle),
    m_expectedSize(0),
    m_receivedSize(0),
    m_spoolWorker(NULL),
    m_retiredWorkerReaped(),
    m_retiredWorkerBlocksBegin(false),
    m_generation(0),
    m_commitBarrier()
{
}

FileReceiveSession::~FileReceiveSession()
{
    reset();
}

bool
FileReceiveSession::begin(size_t expectedSize,
                          size_t memoryLimit,
                          size_t reserveLimit,
                          size_t asyncQueueLimit)
{
    if (!releaseCompletedWorkerCleanup()) {
        return false;
    }
    const State currentState = state();
    if (currentState != kIdle && currentState != kFailed) {
        return false;
    }

    reset();
    if (!releaseCompletedWorkerCleanup()) {
        return false;
    }
    m_expectedSize = expectedSize;
    m_state = kReceiving;

    try {
        m_digest.reset(new barrier::TransferDigest());
    }
    catch (const std::exception&) {
        fail();
        return false;
    }
    if (!m_digest->isReady()) {
        fail();
        return false;
    }

    if (expectedSize > memoryLimit) {
        if (asyncQueueLimit == 0) {
            fail();
            return false;
        }
        try {
            m_asyncState.reset(new AsyncState(
                expectedSize, asyncQueueLimit, m_workerExitGateForTest));
            const std::shared_ptr<AsyncState> state = m_asyncState;
            m_spoolWorker = new std::thread([state]() {
                FileReceiveSession::runSpoolWorker(state);
            });
        }
        catch (const std::exception&) {
            fail();
            return false;
        }
    }
    else if (expectedSize <= reserveLimit) {
        try {
            m_data.reserve(expectedSize);
        }
        catch (const std::exception&) {
            fail();
            return false;
        }
    }

    return true;
}

FileReceiveSession::AppendResult
FileReceiveSession::append(std::string content)
{
    const size_t contentSize = content.size();
    if (state() != kReceiving ||
        m_receivedSize > m_expectedSize ||
        contentSize > m_expectedSize - m_receivedSize) {
        return kAppendFailed;
    }

    bool applyBackpressure = false;
    if (m_asyncState) {
        {
            std::lock_guard<std::mutex> lock(m_asyncState->mutex);
            if (m_asyncState->failed || m_asyncState->cancelRequested ||
                m_asyncState->finishRequested ||
                contentSize > kMaxAsyncChunkSize ||
                contentSize > m_asyncState->queueLimit - m_asyncState->queuedBytes) {
                return kAppendFailed;
            }
            try {
                m_asyncState->chunks.push_back(std::string());
            }
            catch (const std::exception&) {
                return kAppendFailed;
            }
            if (!m_digest || !m_digest->update(content.data(), contentSize)) {
                m_asyncState->chunks.pop_back();
                return kAppendFailed;
            }
            m_asyncState->chunks.back().swap(content);
            m_asyncState->queuedBytes += contentSize;
            const size_t reserve = (std::min)(
                m_asyncState->queueLimit, kMaxAsyncChunkSize);
            applyBackpressure =
                m_asyncState->queuedBytes >= m_asyncState->queueLimit - reserve;
        }
        m_asyncState->wake.notify_one();
    }
    else {
        try {
            m_data.append(content);
        }
        catch (const std::exception&) {
            return kAppendFailed;
        }
        if (!m_digest || !m_digest->update(content.data(), contentSize)) {
            return kAppendFailed;
        }
    }
    m_receivedSize += contentSize;
    return applyBackpressure ? kAppendBackpressure : kAppendQueued;
}

bool
FileReceiveSession::finish(const std::string& expectedDigest)
{
    if (state() != kReceiving || m_receivedSize != m_expectedSize) {
        return false;
    }
    const bool digestMatches = expectedDigest.empty() ||
        (m_digest && m_digest->verify(expectedDigest));
    m_digest.reset();
    if (!digestMatches) {
        return false;
    }

    if (m_asyncState) {
        {
            std::lock_guard<std::mutex> lock(m_asyncState->mutex);
            if (m_asyncState->failed || m_asyncState->cancelRequested) {
                return false;
            }
            m_asyncState->finishRequested = true;
        }
        m_state = kFinalizing;
        m_asyncState->wake.notify_one();
    }
    else {
        m_state = kComplete;
    }

    return true;
}

FileReceiveSession::State
FileReceiveSession::state() const
{
    if (m_asyncState) {
        std::lock_guard<std::mutex> lock(m_asyncState->mutex);
        if (m_asyncState->failed) {
            return kFailed;
        }
        if (m_asyncState->complete) {
            return kComplete;
        }
    }
    return m_state;
}

bool
FileReceiveSession::isSpoolOpen() const
{
    if (!m_asyncState) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_asyncState->mutex);
    return m_asyncState->spoolOpen;
}

size_t
FileReceiveSession::spoolOpenCount() const
{
    if (!m_asyncState) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_asyncState->mutex);
    return m_asyncState->spoolOpenCount;
}

bool
FileReceiveSession::workerCleanupPending() const
{
    return m_retiredWorkerBlocksBegin && retiredWorkerPending();
}

bool
FileReceiveSession::retiredWorkerPending() const
{
    return m_retiredWorkerReaped &&
        !m_retiredWorkerReaped->load(std::memory_order_acquire);
}

void
FileReceiveSession::quarantineRetiredWorkerCleanup()
{
    // The retired worker owns only its detached AsyncState. Once it has been
    // handed to SpoolWorkerReaper it cannot mutate a subsequent session.
    m_retiredWorkerBlocksBegin = false;
}

barrier::fs::path
FileReceiveSession::spoolPath() const
{
    if (!m_asyncState) {
        return barrier::fs::path();
    }
    std::lock_guard<std::mutex> lock(m_asyncState->mutex);
    return m_asyncState->spoolPath;
}

void
FileReceiveSession::runSpoolWorker(
    const std::shared_ptr<AsyncState>& state) noexcept
{
    struct CompletionGuard {
        CompletionGuard(
            const std::shared_ptr<std::atomic<bool> >& done,
            const std::shared_ptr<std::atomic<bool> >& exitGate) :
            m_done(done),
            m_exitGate(exitGate)
        {
        }
        ~CompletionGuard()
        {
            while (m_exitGate &&
                   !m_exitGate->load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            m_done->store(true, std::memory_order_release);
        }
        std::shared_ptr<std::atomic<bool> > m_done;
        std::shared_ptr<std::atomic<bool> > m_exitGate;
    } completion(state->workerDone, state->workerExitGateForTest);

    barrier::fs::path spoolPath;
    std::ofstream spool;
    bool keepSpool = false;

    try {
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->cancelRequested) {
            return;
        }
    }

    if (!barrier::create_secure_temp_file(
            "weave-receive-", ".part", spoolPath)) {
        std::function<void()> resume;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->failed = true;
            resume.swap(state->backpressureResume);
            state->pauseProgress = std::function<void()>();
            state->wake.notify_all();
        }
        invokeNoexcept(resume);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->spoolPath = spoolPath;
        if (state->cancelRequested) {
            state->spoolPath.clear();
        }
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->cancelRequested) {
            removeSpoolFile(spoolPath);
            return;
        }
    }

    barrier::open_utf8_path(
        spool, spoolPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!spool.is_open()) {
        std::function<void()> resume;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->failed = true;
            state->spoolPath.clear();
            resume.swap(state->backpressureResume);
            state->pauseProgress = std::function<void()>();
            state->wake.notify_all();
        }
        invokeNoexcept(resume);
        removeSpoolFile(spoolPath);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->spoolOpen = true;
        ++state->spoolOpenCount;
    }

    for (;;) {
        std::string chunk;
        std::function<void()> resume;
        bool finish = false;
        bool cancel = false;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->wake.wait(lock, [&state]() {
                return state->cancelRequested || state->finishRequested ||
                    !state->chunks.empty();
            });
            cancel = state->cancelRequested;
            if (!cancel && !state->chunks.empty()) {
                chunk.swap(state->chunks.front());
                state->chunks.pop_front();
                state->queuedBytes -= chunk.size();
                if (state->backpressureResume &&
                    state->queuedBytes <= state->queueLimit / 2) {
                    resume.swap(state->backpressureResume);
                    state->pauseProgress = std::function<void()>();
                }
            }
            else if (!cancel && state->finishRequested) {
                finish = true;
            }
        }

        invokeNoexcept(resume);

        if (cancel) {
            break;
        }
        if (!chunk.empty()) {
            spool.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            if (spool.fail()) {
                std::function<void()> failedResume;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->failed = true;
                    failedResume.swap(state->backpressureResume);
                    state->pauseProgress = std::function<void()>();
                    state->wake.notify_all();
                }
                invokeNoexcept(failedResume);
                break;
            }
            std::function<void()> progress;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->writtenBytes += chunk.size();
                try {
                    progress = state->pauseProgress;
                }
                catch (...) {
                    progress = std::function<void()>();
                }
            }
            invokeNoexcept(progress);
            continue;
        }
        if (!finish) {
            continue;
        }

        spool.flush();
        const bool valid = !spool.fail() &&
            state->writtenBytes == state->expectedSize;
        spool.close();
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->spoolOpen = false;
            state->complete = valid;
            state->failed = !valid;
            state->wake.notify_all();
            if (valid) {
                state->wake.wait(lock, [&state]() {
                    return state->cancelRequested || state->pathTransferred;
                });
                keepSpool = state->pathTransferred;
            }
        }
        break;
    }

    if (spool.is_open()) {
        spool.close();
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->spoolOpen = false;
        if (!keepSpool) {
            state->spoolPath.clear();
        }
    }
    if (!keepSpool) {
        removeSpoolFile(spoolPath);
    }
    }
    catch (...) {
        if (spool.is_open()) {
            spool.close();
        }
        std::function<void()> resume;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->failed = true;
            state->complete = false;
            state->spoolOpen = false;
            state->spoolPath.clear();
            resume.swap(state->backpressureResume);
            state->pauseProgress = std::function<void()>();
            state->wake.notify_all();
        }
        invokeNoexcept(resume);
        removeSpoolFile(spoolPath);
    }
}

void
FileReceiveSession::fail()
{
    clearPayload();
    advanceGeneration();
    m_state = kFailed;
    resolveCommitBarrier();
}

void
FileReceiveSession::discardRemaining()
{
    clearPayload();
    advanceGeneration();
    m_state = kDiscarding;
    resolveCommitBarrier();
}

void
FileReceiveSession::reset()
{
    clearPayload();
    advanceGeneration();
    m_state = kIdle;
    resolveCommitBarrier();
}

bool
FileReceiveSession::installCommitBarrier(
    std::uint64_t generation, const std::function<void()>& commit,
    const std::function<void()>& progress)
{
    const State currentState = state();
    if (!commit || generation == 0 || generation != m_generation ||
        (currentState != kFinalizing && currentState != kComplete) ||
        m_commitBarrier) {
        return false;
    }
    m_commitBarrier = commit;
    if (m_asyncState) {
        std::lock_guard<std::mutex> lock(m_asyncState->mutex);
        m_asyncState->pauseProgress = progress;
    }
    return true;
}

bool
FileReceiveSession::installBackpressureBarrier(
    std::uint64_t generation, const std::function<void()>& resume,
    const std::function<void()>& progress)
{
    if (!resume || generation == 0 || generation != m_generation ||
        m_state != kReceiving || !m_asyncState) {
        return false;
    }

    bool resumeImmediately = false;
    {
        std::lock_guard<std::mutex> lock(m_asyncState->mutex);
        if (m_asyncState->failed || m_asyncState->cancelRequested ||
            m_asyncState->finishRequested ||
            m_asyncState->backpressureResume) {
            return false;
        }
        if (m_asyncState->queuedBytes <= m_asyncState->queueLimit / 2) {
            resumeImmediately = true;
        }
        else {
            m_asyncState->backpressureResume = resume;
            m_asyncState->pauseProgress = progress;
        }
    }

    if (resumeImmediately) {
        invokeNoexcept(resume);
    }
    return true;
}

void
FileReceiveSession::takeCompleted(std::string& data,
                                  size_t& expectedSize,
                                  barrier::fs::path& spoolPath)
{
    if (!isComplete()) {
        data.clear();
        expectedSize = 0;
        spoolPath.clear();
        return;
    }

    data.swap(m_data);
    expectedSize = m_expectedSize;
    if (m_asyncState) {
        {
            std::lock_guard<std::mutex> lock(m_asyncState->mutex);
            spoolPath = m_asyncState->spoolPath;
            m_asyncState->pathTransferred = true;
        }
        m_asyncState->wake.notify_one();
        if (workerCleanupPending()) {
            barrier::terminateProcessForFinalThreadShutdown();
        }
        m_retiredWorkerReaped = m_asyncState->workerReaped;
        m_retiredWorkerBlocksBegin = false;
        retireSpoolWorker(m_spoolWorker, m_asyncState->workerDone,
                          m_asyncState->workerReaped);
        m_asyncState.reset();
        m_spoolWorker = NULL;
    }
    else {
        spoolPath.clear();
    }
    m_expectedSize = 0;
    m_receivedSize = 0;
    advanceGeneration();
    m_state = kIdle;
    resolveCommitBarrier();
}

void
FileReceiveSession::resolveCommitBarrier() noexcept
{
    std::function<void()> commit;
    commit.swap(m_commitBarrier);
    if (m_asyncState) {
        std::lock_guard<std::mutex> lock(m_asyncState->mutex);
        m_asyncState->pauseProgress = std::function<void()>();
    }
    if (commit) {
        try {
            commit();
        }
        catch (...) {
            // The payload is already committed; session teardown must not throw.
        }
    }
}

void
FileReceiveSession::advanceGeneration()
{
    ++m_generation;
    if (m_generation == 0) {
        ++m_generation;
    }
}

void
FileReceiveSession::cancelSpoolWorker()
{
    if (m_asyncState) {
        const std::shared_ptr<std::atomic<bool> > workerDone =
            m_asyncState->workerDone;
        std::function<void()> resume;
        {
            std::lock_guard<std::mutex> lock(m_asyncState->mutex);
            m_asyncState->cancelRequested = true;
            resume.swap(m_asyncState->backpressureResume);
            m_asyncState->pauseProgress = std::function<void()>();
        }
        m_asyncState->wake.notify_one();
        invokeNoexcept(resume);
        if (workerCleanupPending()) {
            barrier::terminateProcessForFinalThreadShutdown();
        }
        m_retiredWorkerReaped = m_asyncState->workerReaped;
        m_retiredWorkerBlocksBegin = true;
        retireSpoolWorker(m_spoolWorker, workerDone,
                          m_asyncState->workerReaped);
        m_asyncState.reset();
    }
    else if (m_spoolWorker != NULL) {
        barrier::terminateProcessForFinalThreadShutdown();
    }
    m_spoolWorker = NULL;
}

bool
FileReceiveSession::releaseCompletedWorkerCleanup()
{
    if (!m_retiredWorkerReaped) {
        return true;
    }
    if (!m_retiredWorkerBlocksBegin) {
        if (m_retiredWorkerReaped->load(std::memory_order_acquire)) {
            m_retiredWorkerReaped.reset();
        }
        return true;
    }
    if (!m_retiredWorkerReaped->load(std::memory_order_acquire)) {
        return false;
    }
    m_retiredWorkerReaped.reset();
    m_retiredWorkerBlocksBegin = false;
    return true;
}

void
FileReceiveSession::clearPayload()
{
    cancelSpoolWorker();
    std::string().swap(m_data);
    m_expectedSize = 0;
    m_receivedSize = 0;
    m_digest.reset();
}
