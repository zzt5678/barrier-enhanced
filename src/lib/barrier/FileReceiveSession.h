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

#include <fstream>
#include <string>

class FileReceiveSession {
public:
    enum State {
        kIdle,
        kReceiving,
        kComplete,
        kFailed
    };

    FileReceiveSession();
    ~FileReceiveSession();

    bool                begin(size_t expectedSize,
                              size_t memoryLimit,
                              size_t reserveLimit);
    bool                append(const std::string& content);
    bool                finish();
    void                fail();
    void                reset();
    void                takeCompleted(std::string& data,
                                      size_t& expectedSize,
                                      barrier::fs::path& spoolPath);

    State               state() const { return m_state; }
    bool                isComplete() const { return m_state == kComplete; }
    bool                isSpoolOpen() const { return m_spool.is_open(); }
    size_t              spoolOpenCount() const { return m_spoolOpenCount; }
    size_t              expectedSize() const { return m_expectedSize; }
    size_t              receivedSize() const { return m_receivedSize; }
    const std::string&  data() const { return m_data; }
    const barrier::fs::path& spoolPath() const { return m_spoolPath; }

private:
    FileReceiveSession(const FileReceiveSession&);
    FileReceiveSession& operator=(const FileReceiveSession&);

    void                clearPayload(bool removeSpool);

private:
    State               m_state;
    size_t              m_expectedSize;
    size_t              m_receivedSize;
    std::string         m_data;
    barrier::fs::path   m_spoolPath;
    std::ofstream       m_spool;
    size_t              m_spoolOpenCount;
};
