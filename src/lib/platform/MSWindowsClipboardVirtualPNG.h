#pragma once

#include <ole2.h>
#include <string>

class Clipboard;

namespace MSWindowsClipboardVirtualPNG {
// Reads only a single virtual PNG. Never opens descriptor filenames on disk.
bool read(IDataObject* object, std::string& png);
// Adds the virtual image atomically; false leaves existing formats unchanged.
bool addToSnapshot(IDataObject* object, Clipboard& clipboard);
// OLE access happens after the ordinary Win32 clipboard lock is released.
bool copySnapshot(Clipboard& clipboard);
}
