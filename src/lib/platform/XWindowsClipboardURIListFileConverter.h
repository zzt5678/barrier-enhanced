/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier Contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "platform/XWindowsClipboard.h"

//! Convert X11 file URI clipboard selections to newline-separated paths.
class XWindowsClipboardURIListFileConverter : public IXWindowsClipboardConverter {
public:
    explicit XWindowsClipboardURIListFileConverter(Display* display,
                                                   const char* atomName = "text/uri-list",
                                                   bool gnomeSpecial = false);
    virtual ~XWindowsClipboardURIListFileConverter();

    virtual IClipboard::EFormat getFormat() const;
    virtual Atom getAtom() const;
    virtual int getDataSize() const;
    virtual std::string fromIClipboard(const std::string&) const;
    virtual std::string toIClipboard(const std::string&) const;

private:
    Atom m_atom;
    bool m_gnomeSpecial;
};
