#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

class MSWindowsClipboardChangeTracker
{
public:
    MSWindowsClipboardChangeTracker() :
        m_sequence(0)
    {
    }

    void reset(DWORD sequence)
    {
        m_sequence = sequence;
    }

    bool changed(DWORD sequence) const
    {
        return sequence != 0 && sequence != m_sequence;
    }

    bool observe(DWORD sequence, bool ownedByWeave)
    {
        const bool newRevision = sequence == 0 || sequence != m_sequence;
        if (sequence != 0) {
            m_sequence = sequence;
        }
        return newRevision && !ownedByWeave;
    }

private:
    DWORD m_sequence;
};
