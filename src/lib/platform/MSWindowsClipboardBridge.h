/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "barrier/IClipboard.h"

#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace MSWindowsClipboardBridgeProtocol {

static const UInt32 kMagic = 0x57434231u; // WCB1
static const size_t kMaxSnapshotBytes = 64u * 1024u * 1024u;

enum Command {
    kSnapshot = 1,
    kShutdown = 2
};

enum Status {
    kSuccess = 0,
    kClipboardUnavailable = 1,
    kSnapshotTooLarge = 2,
    kInvalidRequest = 3
};

struct RequestHeader {
    UInt32 magic;
    UInt32 command;
};

struct ReadyHeader {
    UInt32 magic;
    UInt32 processId;
};

struct ResponseHeader {
    UInt32 magic;
    UInt32 status;
    UInt32 size;
};

bool validateResponseHeader(const ResponseHeader& header, std::string* error);
bool validateSnapshot(const std::string& snapshot, std::string* error);

} // namespace MSWindowsClipboardBridgeProtocol

class MSWindowsClipboardBridge {
public:
    enum class ReadResult {
        NotRequired,
        Succeeded,
        Failed
    };

    MSWindowsClipboardBridge();
    ~MSWindowsClipboardBridge();

    void warmUp();
    ReadResult readSnapshot(IClipboard* destination, IClipboard::Time time,
                            std::string* error);

    static int runHelper(const std::string& pipeName);

private:
    MSWindowsClipboardBridge(const MSWindowsClipboardBridge&);
    MSWindowsClipboardBridge& operator=(const MSWindowsClipboardBridge&);

    bool ensureStarted(std::string* error);
    bool requestSnapshot(IClipboard* destination, IClipboard::Time time,
                         bool* connectionFailed, std::string* error);
    void stop();

    HANDLE m_pipe;
    HANDLE m_process;
    DWORD m_processId;
    DWORD m_sessionId;
    ULONGLONG m_nextStartAttemptAt;
};
