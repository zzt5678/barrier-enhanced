/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "common/common.h"
#include "io/filesystem.h"

#include <cstdint>
#include <memory>
#include <string>
#include <thread>

class FileReceiveSession {
public:
    enum State {
        kIdle,
        kReceiving,
        kFinalizing,
        kComplete,
        kFailed
    };

    static const size_t kDefaultAsyncQueueLimit = 16 * 1024 * 1024;

    FileReceiveSession();
    ~FileReceiveSession();

    bool                begin(size_t expectedSize,
                              size_t memoryLimit,
                              size_t reserveLimit,
                              size_t asyncQueueLimit = kDefaultAsyncQueueLimit);
    bool                append(std::string content);
    bool                finish();
    void                fail();
    void                reset();
    void                takeCompleted(std::string& data,
                                      size_t& expectedSize,
                                      barrier::fs::path& spoolPath);

    State               state() const;
    bool                isComplete() const { return state() == kComplete; }
    bool                isFinalizing() const { return state() == kFinalizing; }
    bool                isSpoolOpen() const;
    size_t              spoolOpenCount() const;
    size_t              expectedSize() const { return m_expectedSize; }
    size_t              receivedSize() const { return m_receivedSize; }
    const std::string&  data() const { return m_data; }
    barrier::fs::path   spoolPath() const;
    std::uint64_t       generation() const { return m_generation; }
    bool                matchesGeneration(std::uint64_t generation) const
    {
        return generation != 0 && generation == m_generation;
    }

private:
    FileReceiveSession(const FileReceiveSession&);
    FileReceiveSession& operator=(const FileReceiveSession&);

    struct AsyncState;

    static void         runSpoolWorker(const std::shared_ptr<AsyncState>& state);
    void                advanceGeneration();
    void                clearPayload();
    void                cancelSpoolWorker();

private:
    State               m_state;
    size_t              m_expectedSize;
    size_t              m_receivedSize;
    std::string         m_data;
    std::shared_ptr<AsyncState> m_asyncState;
    std::thread*        m_spoolWorker;
    std::uint64_t       m_generation;
};
