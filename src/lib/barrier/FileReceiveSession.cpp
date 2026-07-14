/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/FileReceiveSession.h"

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

namespace {

struct RetiredSpoolWorker {
    std::thread* thread;
    std::shared_ptr<std::atomic<bool> > done;
};

class SpoolWorkerReaper {
public:
    SpoolWorkerReaper() :
        m_stopping(false),
        m_reaper(&SpoolWorkerReaper::run, this)
    {
    }

    ~SpoolWorkerReaper()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_wake.notify_one();
        m_reaper.join();
    }

    void retire(std::thread* thread,
                const std::shared_ptr<std::atomic<bool> >& done)
    {
        if (thread == NULL) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            RetiredSpoolWorker worker = { thread, done };
            m_workers.push_back(worker);
        }
        m_wake.notify_one();
    }

private:
    void run()
    {
        for (;;) {
            std::vector<RetiredSpoolWorker> ready;
            std::vector<RetiredSpoolWorker> abandoned;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (!m_stopping && m_workers.empty()) {
                    m_wake.wait(lock, [this]() {
                        return m_stopping || !m_workers.empty();
                    });
                }
                else if (!m_stopping) {
                    m_wake.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                        return m_stopping;
                    });
                }

                std::deque<RetiredSpoolWorker>::iterator worker = m_workers.begin();
                while (worker != m_workers.end()) {
                    if (worker->done->load(std::memory_order_acquire)) {
                        ready.push_back(*worker);
                        worker = m_workers.erase(worker);
                    }
                    else if (m_stopping) {
                        abandoned.push_back(*worker);
                        worker = m_workers.erase(worker);
                    }
                    else {
                        ++worker;
                    }
                }
                if (m_stopping && m_workers.empty()) {
                    lock.unlock();
                    release(ready, true);
                    release(abandoned, false);
                    return;
                }
            }
            release(ready, true);
        }
    }

    static void release(std::vector<RetiredSpoolWorker>& workers, bool join)
    {
        for (std::vector<RetiredSpoolWorker>::iterator worker = workers.begin();
             worker != workers.end(); ++worker) {
            if (worker->thread->joinable()) {
                if (join) {
                    worker->thread->join();
                }
                else {
                    worker->thread->detach();
                }
            }
            delete worker->thread;
        }
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<RetiredSpoolWorker> m_workers;
    bool m_stopping;
    std::thread m_reaper;
};

void
retireSpoolWorker(std::thread* worker,
                  const std::shared_ptr<std::atomic<bool> >& done) noexcept
{
    if (worker == NULL) {
        return;
    }
    try {
        static SpoolWorkerReaper reaper;
        reaper.retire(worker, done);
    }
    catch (...) {
        if (worker->joinable()) {
            try {
                worker->detach();
            }
            catch (...) {
                return;
            }
        }
        delete worker;
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

}

struct FileReceiveSession::AsyncState {
    AsyncState(size_t expected, size_t queueLimit) :
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
        workerDone(new std::atomic<bool>(false))
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
    std::shared_ptr<std::atomic<bool> > workerDone;
};

FileReceiveSession::FileReceiveSession() :
    m_state(kIdle),
    m_expectedSize(0),
    m_receivedSize(0),
    m_spoolWorker(NULL),
    m_generation(0)
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
    reset();
    m_expectedSize = expectedSize;
    m_state = kReceiving;

    if (expectedSize > memoryLimit) {
        if (asyncQueueLimit == 0) {
            fail();
            return false;
        }
        try {
            m_asyncState.reset(new AsyncState(expectedSize, asyncQueueLimit));
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

bool
FileReceiveSession::append(std::string content)
{
    const size_t contentSize = content.size();
    if (state() != kReceiving ||
        m_receivedSize > m_expectedSize ||
        contentSize > m_expectedSize - m_receivedSize) {
        return false;
    }

    if (m_asyncState) {
        std::lock_guard<std::mutex> lock(m_asyncState->mutex);
        if (m_asyncState->failed || m_asyncState->cancelRequested ||
            m_asyncState->finishRequested ||
            contentSize > m_asyncState->queueLimit - m_asyncState->queuedBytes) {
            return false;
        }
        try {
            m_asyncState->chunks.push_back(std::move(content));
        }
        catch (const std::exception&) {
            return false;
        }
        m_asyncState->queuedBytes += contentSize;
        m_asyncState->wake.notify_one();
    }
    else {
        try {
            m_data.append(content);
        }
        catch (const std::exception&) {
            return false;
        }
    }

    m_receivedSize += contentSize;
    return true;
}

bool
FileReceiveSession::finish()
{
    if (state() != kReceiving || m_receivedSize != m_expectedSize) {
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
FileReceiveSession::runSpoolWorker(const std::shared_ptr<AsyncState>& state)
{
    struct CompletionGuard {
        explicit CompletionGuard(const std::shared_ptr<std::atomic<bool> >& done) :
            m_done(done)
        {
        }
        ~CompletionGuard()
        {
            m_done->store(true, std::memory_order_release);
        }
        std::shared_ptr<std::atomic<bool> > m_done;
    } completion(state->workerDone);

    barrier::fs::path spoolPath;
    std::ofstream spool;
    bool keepSpool = false;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->cancelRequested) {
            return;
        }
    }

    if (!barrier::create_secure_temp_file(
            "weave-receive-", ".part", spoolPath)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->failed = true;
        state->wake.notify_all();
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
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->failed = true;
            state->spoolPath.clear();
            state->wake.notify_all();
        }
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
            }
            else if (!cancel && state->finishRequested) {
                finish = true;
            }
        }

        if (cancel) {
            break;
        }
        if (!chunk.empty()) {
            spool.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            if (spool.fail()) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->failed = true;
                state->wake.notify_all();
                break;
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            state->writtenBytes += chunk.size();
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

void
FileReceiveSession::fail()
{
    clearPayload();
    advanceGeneration();
    m_state = kFailed;
}

void
FileReceiveSession::reset()
{
    clearPayload();
    advanceGeneration();
    m_state = kIdle;
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
        retireSpoolWorker(m_spoolWorker, m_asyncState->workerDone);
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
        {
            std::lock_guard<std::mutex> lock(m_asyncState->mutex);
            m_asyncState->cancelRequested = true;
        }
        m_asyncState->wake.notify_one();
        retireSpoolWorker(m_spoolWorker, workerDone);
        m_asyncState.reset();
    }
    else if (m_spoolWorker != NULL) {
        m_spoolWorker->detach();
        delete m_spoolWorker;
    }
    m_spoolWorker = NULL;
}

void
FileReceiveSession::clearPayload()
{
    cancelSpoolWorker();
    std::string().swap(m_data);
    m_expectedSize = 0;
    m_receivedSize = 0;
}
