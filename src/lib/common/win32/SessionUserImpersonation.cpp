/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "common/win32/SessionUserImpersonation.h"

#include <Wtsapi32.h>
#include <vector>

namespace {

enum class LocalSystemStatus {
    No,
    Yes,
    Unknown
};

LocalSystemStatus
localSystemStatus(DWORD& error)
{
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error = GetLastError();
        return LocalSystemStatus::Unknown;
    }

    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    const bool read = bytes != 0 &&
        GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes);
    error = read ? ERROR_SUCCESS : GetLastError();
    const bool isSystem = read &&
        IsWellKnownSid(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,
                       WinLocalSystemSid);
    CloseHandle(token);

    if (!read) {
        return LocalSystemStatus::Unknown;
    }
    return isSystem ? LocalSystemStatus::Yes : LocalSystemStatus::No;
}

}

SessionUserImpersonation::SessionUserImpersonation() :
    m_token(NULL),
    m_failure(kNoFailure),
    m_error(ERROR_SUCCESS),
    m_revertError(ERROR_SUCCESS),
    m_sessionId(0xffffffff),
    m_required(false),
    m_impersonating(false),
    m_usedRevertFallback(false)
{
    const LocalSystemStatus status = localSystemStatus(m_error);
    if (status == LocalSystemStatus::No) {
        return;
    }
    if (status == LocalSystemStatus::Unknown) {
        m_required = true;
        m_failure = m_error == ERROR_SUCCESS
            ? kReadProcessIdentity : kInspectProcessToken;
        return;
    }

    m_required = true;
    m_sessionId = WTSGetActiveConsoleSessionId();
    if (m_sessionId == 0xffffffff) {
        m_failure = kNoActiveSession;
        m_error = ERROR_NO_SUCH_LOGON_SESSION;
        return;
    }
    if (!WTSQueryUserToken(m_sessionId, &m_token)) {
        m_failure = kQuerySessionToken;
        m_error = GetLastError();
        return;
    }
    if (!ImpersonateLoggedOnUser(m_token)) {
        m_failure = kImpersonateSessionUser;
        m_error = GetLastError();
        return;
    }

    m_impersonating = true;
}

bool
SessionUserImpersonation::queryRequired(bool& required, DWORD& error)
{
    const LocalSystemStatus status = localSystemStatus(error);
    if (status == LocalSystemStatus::Unknown) {
        required = true;
        return false;
    }

    required = status == LocalSystemStatus::Yes;
    return true;
}

SessionUserImpersonation::~SessionUserImpersonation()
{
    finish();
    if (m_token != NULL) {
        CloseHandle(m_token);
    }
}

bool
SessionUserImpersonation::ready() const
{
    return m_failure == kNoFailure;
}

bool
SessionUserImpersonation::finish()
{
    if (!m_impersonating) {
        return m_failure == kNoFailure;
    }

    if (RevertToSelf()) {
        m_impersonating = false;
        return true;
    }

    m_revertError = GetLastError();
    if (SetThreadToken(NULL, NULL)) {
        m_impersonating = false;
        m_usedRevertFallback = true;
        return true;
    }

    m_failure = kRestoreProcessIdentity;
    m_error = GetLastError();
    return false;
}

const char*
SessionUserImpersonation::failureName() const
{
    switch (m_failure) {
    case kNoFailure:
        return "none";
    case kInspectProcessToken:
        return "inspect-process-token";
    case kReadProcessIdentity:
        return "read-process-identity";
    case kNoActiveSession:
        return "no-active-session";
    case kQuerySessionToken:
        return "query-session-token";
    case kImpersonateSessionUser:
        return "impersonate-session-user";
    case kRestoreProcessIdentity:
        return "restore-process-identity";
    }
    return "unknown";
}

DWORD
SessionUserImpersonation::error() const
{
    return m_error;
}

DWORD
SessionUserImpersonation::revertError() const
{
    return m_revertError;
}

DWORD
SessionUserImpersonation::sessionId() const
{
    return m_sessionId;
}

bool
SessionUserImpersonation::required() const
{
    return m_required;
}

bool
SessionUserImpersonation::usedRevertFallback() const
{
    return m_usedRevertFallback;
}
