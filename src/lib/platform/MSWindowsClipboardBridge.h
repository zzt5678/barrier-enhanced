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

#include <cstddef>
#include <functional>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace MSWindowsClipboardBridgeProtocol {

typedef std::function<bool (unsigned char*, std::size_t)>
    RandomBytesProvider;

static const UInt32 kMagic = 0x57434231u; // WCB1
static const size_t kMaxSnapshotBytes = 64u * 1024u * 1024u;

enum Command {
    kSnapshot = 1,
    kPublish = 2,
    kShutdown = 3
};

enum Status {
    kSuccess = 0,
    kClipboardUnavailable = 1,
    kSnapshotTooLarge = 2,
    kInvalidRequest = 3,
    kPublishFailed = 4,
    kRevisionChanged = 5
};

struct RequestHeader {
    UInt32 magic;
    UInt32 command;
    UInt32 size;
};

struct PublishRequestHeader {
    UInt32 expectedWindowsSequence;
    UInt32 snapshotSize;
};

struct PublishResponse {
    UInt32 committedWindowsSequence;
};

    struct ReadyHeader {
        UInt32 magic;
        UInt32 processId;
        UInt32 sessionId;
};

struct ResponseHeader {
    UInt32 magic;
    UInt32 status;
    UInt32 size;
};

bool validateResponseHeader(const ResponseHeader& header, std::string* error);
bool validateSnapshot(const std::string& snapshot, std::string* error);
bool isCurrentHelperSession(UInt32 helperSession, UInt32 owningSession);
bool isCurrentHelperIdentity(UInt32 helperSession,
                             const std::string& helperOwnerSid,
                             UInt32 owningSession,
                             const std::string& currentOwnerSid,
                             bool currentSessionActive);
UInt32 selectOwningSession(UInt32 nodeSession, UInt32 activeConsoleSession);
bool generatePipeSuffix(std::string& suffix,
                        const RandomBytesProvider& provider);

} // namespace MSWindowsClipboardBridgeProtocol

class MSWindowsClipboardBridge {
public:
    enum class ReadResult {
        NotRequired,
        Succeeded,
        Failed,
        Superseded
    };

    MSWindowsClipboardBridge();
    ~MSWindowsClipboardBridge();

    void warmUp();
    ReadResult readSnapshot(std::string* destination, std::string* error);
    ReadResult readSnapshot(IClipboard* destination, IClipboard::Time time,
                            std::string* error);
    ReadResult publishSnapshot(const std::string& snapshot,
                               UInt32 expectedWindowsSequence,
                               UInt32* committedWindowsSequence,
                               std::string* error);

    static int runHelper(const std::string& pipeName);

private:
    MSWindowsClipboardBridge(const MSWindowsClipboardBridge&);
    MSWindowsClipboardBridge& operator=(const MSWindowsClipboardBridge&);

    bool ensureStarted(std::string* error);
    bool requestSnapshot(std::string* destination, bool* connectionFailed,
                         std::string* error);
    bool requestPublish(const std::string& snapshot,
                        UInt32 expectedWindowsSequence,
                        UInt32* committedWindowsSequence, bool* superseded,
                        bool* connectionFailed,
                        std::string* error);
    bool helperMatchesCurrentNode(std::string* error) const;
    void stop();

    HANDLE m_pipe;
    HANDLE m_process;
    DWORD m_processId;
    DWORD m_sessionId;
    std::string m_userSid;
    ULONGLONG m_nextStartAttemptAt;
};
