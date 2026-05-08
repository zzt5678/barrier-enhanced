#include "platform/MSWindowsClipboardFileListConverter.h"

#include "barrier/RemoteFileClipboard.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <ShlObj_core.h>

#include <vector>

namespace {

std::wstring wideFromUtf8(const std::string& value)
{
    if (value.empty()) {
        return std::wstring();
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, NULL, 0);
    if (size <= 1) {
        return std::wstring();
    }

    std::wstring result(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &result[0], size);
    return result;
}

std::string utf8FromWide(const std::wstring& value)
{
    if (value.empty()) {
        return std::string();
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, NULL, 0, NULL, NULL);
    if (size <= 1) {
        return std::string();
    }

    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], size, NULL, NULL);
    return result;
}

} // namespace

MSWindowsClipboardFileListConverter::MSWindowsClipboardFileListConverter()
{
}

MSWindowsClipboardFileListConverter::~MSWindowsClipboardFileListConverter()
{
}

IClipboard::EFormat
MSWindowsClipboardFileListConverter::getFormat() const
{
    return IClipboard::kFileList;
}

UINT
MSWindowsClipboardFileListConverter::getWin32Format() const
{
    return CF_HDROP;
}

HANDLE
MSWindowsClipboardFileListConverter::fromIClipboard(const std::string& data) const
{
    RemoteFileClipboard::Data payload;
    if (!RemoteFileClipboard::parse(data, payload) ||
        payload.mode != RemoteFileClipboard::Mode::MaterializedPaths ||
        payload.paths.empty()) {
        return NULL;
    }

    std::vector<std::wstring> paths;
    size_t charCount = 1;
    for (size_t i = 0; i < payload.paths.size(); ++i) {
        const std::wstring widePath = wideFromUtf8(payload.paths[i].u8string());
        if (widePath.empty()) {
            return NULL;
        }
        charCount += widePath.size() + 1;
        paths.push_back(widePath);
    }

    const SIZE_T bytes = sizeof(DROPFILES) + charCount * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, bytes);
    if (handle == NULL) {
        return NULL;
    }

    DROPFILES* dropFiles = static_cast<DROPFILES*>(GlobalLock(handle));
    if (dropFiles == NULL) {
        GlobalFree(handle);
        return NULL;
    }

    dropFiles->pFiles = sizeof(DROPFILES);
    dropFiles->pt.x = 0;
    dropFiles->pt.y = 0;
    dropFiles->fNC = FALSE;
    dropFiles->fWide = TRUE;

    wchar_t* cursor = reinterpret_cast<wchar_t*>(
        reinterpret_cast<unsigned char*>(dropFiles) + sizeof(DROPFILES));
    for (size_t i = 0; i < paths.size(); ++i) {
        memcpy(cursor, paths[i].c_str(), (paths[i].size() + 1) * sizeof(wchar_t));
        cursor += paths[i].size() + 1;
    }
    *cursor = L'\0';

    GlobalUnlock(handle);
    return handle;
}

std::string
MSWindowsClipboardFileListConverter::toIClipboard(HANDLE data) const
{
    if (data == NULL) {
        return std::string();
    }

    const HDROP drop = static_cast<HDROP>(data);
    const UINT fileCount = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    if (fileCount == 0) {
        return std::string();
    }

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.cut = false;

    for (UINT i = 0; i < fileCount; ++i) {
        const UINT length = DragQueryFileW(drop, i, NULL, 0);
        if (length == 0) {
            continue;
        }

        std::wstring widePath;
        widePath.resize(length);
        if (DragQueryFileW(drop, i, &widePath[0], length + 1) == 0) {
            continue;
        }

        payload.paths.push_back(barrier::fs::u8path(utf8FromWide(widePath)));
    }

    return payload.paths.empty() ? std::string() : RemoteFileClipboard::serialize(payload);
}
