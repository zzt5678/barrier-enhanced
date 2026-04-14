/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2014-2016 Symless Ltd.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "platform/MSWindowsDropTarget.h"

#include "base/Log.h"
#include "common/common.h"

#include <stdio.h>
#include <Shlobj.h>
#include <Shellapi.h>

void getDropData(IDataObject *pDataObject);

MSWindowsDropTarget* MSWindowsDropTarget::s_instance = NULL;

MSWindowsDropTarget::MSWindowsDropTarget() :
    m_refCount(1),
    m_allowDrop(false)
{
    s_instance = this;
}

MSWindowsDropTarget::~MSWindowsDropTarget()
{
}

MSWindowsDropTarget&
MSWindowsDropTarget::instance()
{
    assert(s_instance != NULL);
    return *s_instance;
}

HRESULT
MSWindowsDropTarget::DragEnter(IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect)
{
    // check if data object contain drop
    m_allowDrop = queryDataObject(dataObject);
    if (m_allowDrop) {
        getDropData(dataObject);
    }

    *effect = DROPEFFECT_NONE;

    return S_OK;
}

HRESULT
MSWindowsDropTarget::DragOver(DWORD keyState, POINTL point, DWORD* effect)
{
    *effect = DROPEFFECT_NONE;

    return S_OK;
}

HRESULT
MSWindowsDropTarget::DragLeave(void)
{
    return S_OK;
}

HRESULT
MSWindowsDropTarget::Drop(IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect)
{
    *effect = DROPEFFECT_NONE;

    return S_OK;
}

bool
MSWindowsDropTarget::queryDataObject(IDataObject* dataObject)
{
    // check if it supports CF_HDROP using a HGLOBAL
    FORMATETC fmtetc = { CF_HDROP, 0, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };

    return dataObject->QueryGetData(&fmtetc) == S_OK ? true : false;
}

void
MSWindowsDropTarget::setDraggingFilename(const std::string& filename)
{
    m_dragFilename = filename;
}

void
MSWindowsDropTarget::setDraggingFileList(const std::string& fileList)
{
    m_dragFilename = fileList;
}

std::string
MSWindowsDropTarget::getDraggingFilename()
{
    return m_dragFilename;
}

void
MSWindowsDropTarget::clearDraggingFilename()
{
    m_dragFilename.clear();
}

void
getDropData(IDataObject* dataObject)
{
    FORMATETC fmtEtc = { CF_HDROP, 0, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stgMed;

    if (dataObject->QueryGetData(&fmtEtc) == S_OK) {
        if (dataObject->GetData(&fmtEtc, &stgMed) == S_OK) {
            PVOID data = GlobalLock(stgMed.hGlobal);
            if (data != nullptr) {
                const HDROP drop = static_cast<HDROP>(stgMed.hGlobal);
                const UINT fileCount = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                if (fileCount > 0) {
                    std::string joinedList;
                    for (UINT fileIndex = 0; fileIndex < fileCount; ++fileIndex) {
                        const UINT pathLength = DragQueryFileW(drop, fileIndex, nullptr, 0);
                        if (pathLength == 0) {
                            continue;
                        }

                        std::wstring wideFilename(pathLength + 1, L'\0');
                        const UINT copied = DragQueryFileW(
                            drop, fileIndex, &wideFilename[0], pathLength + 1);
                        wideFilename.resize(copied);

                        const int utf8Size = WideCharToMultiByte(
                            CP_UTF8, 0, wideFilename.c_str(), -1, nullptr, 0, nullptr, nullptr);
                        if (utf8Size > 1) {
                            std::string filename(static_cast<size_t>(utf8Size - 1), '\0');
                            WideCharToMultiByte(
                                CP_UTF8, 0, wideFilename.c_str(), -1,
                                &filename[0], utf8Size, nullptr, nullptr);
                            if (!joinedList.empty()) {
                                joinedList.push_back('\n');
                            }
                            joinedList.append(filename);
                        }
                    }

                    if (!joinedList.empty()) {
                        MSWindowsDropTarget::instance().setDraggingFileList(joinedList);
                    }
                }

                GlobalUnlock(stgMed.hGlobal);
            }

            // release the data using the COM API
            ReleaseStgMedium(&stgMed);
        }
    }
}

HRESULT __stdcall
MSWindowsDropTarget::QueryInterface (REFIID iid, void ** object)
{
    if (iid == IID_IDropTarget || iid == IID_IUnknown) {
        AddRef();
        *object = this;
        return S_OK;
    }
    else {
        *object = 0;
        return E_NOINTERFACE;
    }
}

ULONG __stdcall
MSWindowsDropTarget::AddRef(void)
{
    return InterlockedIncrement(&m_refCount);
}

ULONG __stdcall
MSWindowsDropTarget::Release(void)
{
    LONG count = InterlockedDecrement(&m_refCount);

    if (count == 0) {
        delete this;
        return 0;
    }
    else {
        return count;
    }
}
