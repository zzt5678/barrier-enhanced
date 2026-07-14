/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <cstdint>

namespace barrier {

class ClipboardRevision {
public:
    ClipboardRevision() : m_sequence(0) { }

    void advance()
    {
        ++m_sequence;
        if (m_sequence == 0) {
            ++m_sequence;
        }
    }

    void reset() { m_sequence = 0; }
    bool valid() const { return m_sequence != 0; }
    std::uint64_t sequence() const { return m_sequence; }

    bool operator==(const ClipboardRevision& other) const
    {
        return m_sequence == other.m_sequence;
    }

    bool operator!=(const ClipboardRevision& other) const
    {
        return !(*this == other);
    }

private:
    std::uint64_t m_sequence;
};

} // namespace barrier
