/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/FileReceiveSession.h"

#include "base/Log.h"

FileReceiveSession::FileReceiveSession() :
    m_state(kIdle),
    m_expectedSize(0),
    m_receivedSize(0),
    m_spoolOpenCount(0)
{
}

FileReceiveSession::~FileReceiveSession()
{
    reset();
}

bool
FileReceiveSession::begin(size_t expectedSize,
                          size_t memoryLimit,
                          size_t reserveLimit)
{
    reset();
    m_expectedSize = expectedSize;
    m_state = kReceiving;

    if (expectedSize > memoryLimit) {
        if (!barrier::create_secure_temp_file(
                "weave-receive-", ".part", m_spoolPath)) {
            fail();
            return false;
        }

        barrier::open_utf8_path(
            m_spool, m_spoolPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!m_spool.is_open()) {
            fail();
            return false;
        }
        ++m_spoolOpenCount;
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
FileReceiveSession::append(const std::string& content)
{
    if (m_state != kReceiving ||
        m_receivedSize > m_expectedSize ||
        content.size() > m_expectedSize - m_receivedSize) {
        return false;
    }

    if (m_spool.is_open()) {
        m_spool.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (m_spool.fail()) {
            return false;
        }
    }
    else {
        try {
            m_data.append(content);
        }
        catch (const std::exception&) {
            return false;
        }
    }

    m_receivedSize += content.size();
    return true;
}

bool
FileReceiveSession::finish()
{
    if (m_state != kReceiving || m_receivedSize != m_expectedSize) {
        return false;
    }

    if (m_spool.is_open()) {
        m_spool.flush();
        m_spool.close();
        if (m_spool.fail()) {
            return false;
        }
    }

    m_state = kComplete;
    return true;
}

void
FileReceiveSession::fail()
{
    clearPayload(true);
    m_state = kFailed;
}

void
FileReceiveSession::reset()
{
    clearPayload(true);
    m_state = kIdle;
    m_spoolOpenCount = 0;
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
    spoolPath = m_spoolPath;
    m_spoolPath.clear();
    m_expectedSize = 0;
    m_receivedSize = 0;
    m_state = kIdle;
    m_spoolOpenCount = 0;
}

void
FileReceiveSession::clearPayload(bool removeSpool)
{
    if (m_spool.is_open()) {
        m_spool.close();
    }
    m_spool.clear();
    if (removeSpool && !m_spoolPath.empty()) {
        barrier::fs::remove(m_spoolPath);
    }
    m_spoolPath.clear();
    std::string().swap(m_data);
    m_expectedSize = 0;
    m_receivedSize = 0;
}
