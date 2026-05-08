#pragma once

#include "platform/MSWindowsClipboard.h"

class MSWindowsClipboardFileListConverter : public IMSWindowsClipboardConverter {
public:
    MSWindowsClipboardFileListConverter();
    virtual ~MSWindowsClipboardFileListConverter();

    virtual IClipboard::EFormat getFormat() const;
    virtual UINT getWin32Format() const;
    virtual HANDLE fromIClipboard(const std::string& data) const;
    virtual std::string toIClipboard(HANDLE data) const;
};
