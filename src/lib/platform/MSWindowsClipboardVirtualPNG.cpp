#include "platform/MSWindowsClipboardVirtualPNG.h"
#include "platform/MSWindowsClipboard.h"
#include "platform/MSWindowsClipboardBridge.h"
#include "barrier/Clipboard.h"
#include "ext/lodepng/lodepng.h"
#include <shlobj.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace {
const size_t kMaxBytes = MSWindowsClipboardBridgeProtocol::kMaxSnapshotBytes - 12;
const unsigned long long kMaxPixels = 32ull * 1024 * 1024;
struct Medium {
    STGMEDIUM value = {};
    ~Medium() { if (value.tymed != TYMED_NULL) ReleaseStgMedium(&value); }
};
struct GlobalLockGuard {
    explicit GlobalLockGuard(HGLOBAL handle) : handle(handle), data(GlobalLock(handle)) {}
    ~GlobalLockGuard() { if (data) GlobalUnlock(handle); }
    HGLOBAL handle;
    void* data;
};
struct OleScope {
    OleScope() : result(OleInitialize(nullptr)) {}
    ~OleScope() { if (SUCCEEDED(result)) OleUninitialize(); }
    HRESULT result;
};
struct DataObject {
    IDataObject* value = nullptr;
    ~DataObject() { if (value) value->Release(); }
};
bool validPNG(const std::string& bytes)
{
    static const unsigned char signature[] = {137,80,78,71,13,10,26,10};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), signature, 8)) return false;
    lodepng::State state;
    unsigned width = 0, height = 0;
    const unsigned char* input = reinterpret_cast<const unsigned char*>(bytes.data());
    size_t offset = 8;
    bool ended = false;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 12) return false;
        const size_t length = lodepng_chunk_length(input + offset);
        if (length > bytes.size() - offset - 12) return false;
        const bool iend = lodepng_chunk_type_equals(input + offset, "IEND") != 0;
        offset += length + 12;
        if (iend) {
            if (length != 0 || offset != bytes.size()) return false;
            ended = true;
            break;
        }
    }
    if (!ended) return false;
    if (lodepng_inspect(&width, &height, &state, input, bytes.size()) != 0 ||
        !width || !height || static_cast<unsigned long long>(width) * height > kMaxPixels) return false;
    state.decoder.zlibsettings.max_output_size = static_cast<size_t>(kMaxPixels * 8 + 1024 * 1024);
    std::vector<unsigned char> decoded;
    return lodepng::decode(decoded, width, height, state, input, bytes.size()) == 0;
}
}

namespace MSWindowsClipboardVirtualPNG {
bool read(IDataObject* object, std::string& png)
{
    png.clear();
    if (!object) return false;
    FORMATETC format = {static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW")),
        nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    Medium descriptor;
    if (FAILED(object->GetData(&format, &descriptor.value)) ||
        descriptor.value.tymed != TYMED_HGLOBAL || !descriptor.value.hGlobal ||
        GlobalSize(descriptor.value.hGlobal) < sizeof(FILEGROUPDESCRIPTORW)) return false;
    FILEDESCRIPTORW file = {};
    {
        GlobalLockGuard locked(descriptor.value.hGlobal);
        if (!locked.data) return false;
        const FILEGROUPDESCRIPTORW* group = static_cast<const FILEGROUPDESCRIPTORW*>(locked.data);
        if (group->cItems != 1) return false;
        file = group->fgd[0];
    }
    if (!(file.dwFlags & FD_FILESIZE) ||
        ((file.dwFlags & FD_ATTRIBUTES) && (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ||
        file.nFileSizeHigh || file.nFileSizeLow == 0 || file.nFileSizeLow > kMaxBytes) return false;
    wchar_t* end = std::find(file.cFileName, file.cFileName + MAX_PATH, L'\0');
    if (end == file.cFileName + MAX_PATH || end - file.cFileName < 5 ||
        _wcsicmp(end - 4, L".png") != 0) return false;
    // A virtual filename is metadata, never a path to open. Reject paths too,
    // so future consumers cannot accidentally reinterpret it as one.
    if (std::find(file.cFileName, end, L'/') != end ||
        std::find(file.cFileName, end, L'\\') != end ||
        std::find(file.cFileName, end, L':') != end) return false;

    format.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
    format.lindex = 0;
    format.tymed = TYMED_HGLOBAL | TYMED_ISTREAM;
    Medium content;
    if (FAILED(object->GetData(&format, &content.value))) return false;
    std::string bytes;
    const size_t size = file.nFileSizeLow;
    if (content.value.tymed == TYMED_HGLOBAL && content.value.hGlobal) {
        const SIZE_T allocated = GlobalSize(content.value.hGlobal);
        if (allocated < size || allocated > kMaxBytes) return false;
        GlobalLockGuard locked(content.value.hGlobal);
        if (!locked.data) return false;
        bytes.assign(static_cast<const char*>(locked.data), size);
    }
    else if (content.value.tymed == TYMED_ISTREAM && content.value.pstm) {
        // Sequential reads also work for non-seekable virtual streams. Never
        // trust Stat or allocate from a provider-controlled stream size.
        bytes.resize(size);
        size_t offset = 0;
        while (offset < size) {
            ULONG read = 0;
            const ULONG wanted = static_cast<ULONG>((std::min)(size - offset, size_t(64 * 1024)));
            const HRESULT result = content.value.pstm->Read(&bytes[offset], wanted, &read);
            if (FAILED(result) || read == 0 || read > wanted) return false;
            offset += read;
        }
        unsigned char extra = 0;
        ULONG read = 0;
        if (FAILED(content.value.pstm->Read(&extra, 1, &read)) || read != 0) return false;
    }
    else return false;
    if (!validPNG(bytes)) return false;
    png.swap(bytes);
    return true;
}

bool addToSnapshot(IDataObject* object, Clipboard& clipboard)
{
    std::string png;
    // A known virtual source that cannot be read must not turn into a
    // successful empty snapshot. Existing callers retry without publishing.
    if (!read(object, png)) return false;
    clipboard.open(clipboard.getTime());
    clipboard.add(IClipboard::kPNG, png);
    clipboard.close();
    return true;
}

bool copySnapshot(Clipboard& clipboard)
{
    MSWindowsClipboard source(nullptr);
    if (!Clipboard::copy(&clipboard, &source)) return false;
    // Clipboard::copy has closed the native clipboard here. OLE may open it
    // and make cross-process calls; never do that while holding its lock.
    clipboard.open(clipboard.getTime());
    const bool hasPNG = clipboard.has(IClipboard::kPNG);
    clipboard.close();
    if (hasPNG || !IsClipboardFormatAvailable(RegisterClipboardFormatW(L"FileGroupDescriptorW"))) return true;
    OleScope ole;
    if (FAILED(ole.result)) return false;
    DataObject object;
    if (FAILED(OleGetClipboard(&object.value))) return false;
    return addToSnapshot(object.value, clipboard);
}
}
