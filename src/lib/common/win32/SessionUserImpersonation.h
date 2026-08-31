/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

class SessionUserImpersonation {
public:
    SessionUserImpersonation();
    ~SessionUserImpersonation();

    static bool queryRequired(bool& required, DWORD& error);

    bool ready() const;
    bool finish();
    const char* failureName() const;
    DWORD error() const;
    DWORD revertError() const;
    DWORD sessionId() const;
    bool required() const;
    bool usedRevertFallback() const;

private:
    SessionUserImpersonation(const SessionUserImpersonation&);
    SessionUserImpersonation& operator=(const SessionUserImpersonation&);

    enum Failure {
        kNoFailure,
        kInspectProcessToken,
        kReadProcessIdentity,
        kNoActiveSession,
        kQuerySessionToken,
        kImpersonateSessionUser,
        kRestoreProcessIdentity
    };

    HANDLE m_token;
    Failure m_failure;
    DWORD m_error;
    DWORD m_revertError;
    DWORD m_sessionId;
    bool m_required;
    bool m_impersonating;
    bool m_usedRevertFallback;
};
