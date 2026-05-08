#pragma once

#include "platform/XWindowsClipboard.h"

class XWindowsClipboardFileListConverter : public IXWindowsClipboardConverter {
public:
    XWindowsClipboardFileListConverter(Display* display, const char* name, bool gnomeCopiedFiles);
    virtual ~XWindowsClipboardFileListConverter();

    virtual IClipboard::EFormat getFormat() const;
    virtual Atom getAtom() const;
    virtual int getDataSize() const;
    virtual std::string fromIClipboard(const std::string& data) const;
    virtual std::string toIClipboard(const std::string& data) const;

private:
    Atom m_atom;
    bool m_gnomeCopiedFiles;
};
