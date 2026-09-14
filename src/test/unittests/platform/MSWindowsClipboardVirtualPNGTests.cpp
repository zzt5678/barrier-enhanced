#include "platform/MSWindowsClipboardVirtualPNG.h"
#include "barrier/Clipboard.h"
#include "test/global/gtest.h"
#include "ext/lodepng/lodepng.h"
#include <shlobj.h>
#include <cstring>
#include <vector>

namespace {
class VirtualPNG : public IDataObject {
public:
    VirtualPNG() {
        descriptor.cItems = 1;
        descriptor.fgd[0].dwFlags = FD_FILESIZE | FD_ATTRIBUTES;
        descriptor.fgd[0].dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        wcscpy_s(descriptor.fgd[0].cFileName, L"synthetic.png");
        std::vector<unsigned char> bytes;
        const unsigned char pixel[] = {12, 34, 56, 255};
        lodepng::encode(bytes, pixel, 1, 1);
        data.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        descriptor.fgd[0].nFileSizeLow = static_cast<DWORD>(data.size());
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id != IID_IUnknown && id != IID_IDataObject) { *out = nullptr; return E_NOINTERFACE; }
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        std::memset(medium, 0, sizeof(*medium));
        if (format->cfFormat == RegisterClipboardFormatW(L"FileGroupDescriptorW")) {
            if (format->lindex != -1 || !(format->tymed & TYMED_HGLOBAL)) return DV_E_FORMATETC;
            medium->tymed = TYMED_HGLOBAL;
            medium->hGlobal = GlobalAlloc(GMEM_MOVEABLE, descriptorBytes);
            void* p = GlobalLock(medium->hGlobal);
            std::memcpy(p, &descriptor, (std::min)(descriptorBytes, sizeof(descriptor)));
            GlobalUnlock(medium->hGlobal);
            return S_OK;
        }
        if (format->cfFormat != RegisterClipboardFormatW(L"FileContents") ||
            format->lindex != 0 || format->dwAspect != DVASPECT_CONTENT ||
            !(format->tymed & (stream ? TYMED_ISTREAM : TYMED_HGLOBAL))) return DV_E_FORMATETC;
        ++contentReads;
        HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, data.size());
        void* p = GlobalLock(global); std::memcpy(p, data.data(), data.size()); GlobalUnlock(global);
        if (stream) {
            medium->tymed = TYMED_ISTREAM;
            return CreateStreamOnHGlobal(global, TRUE, &medium->pstm);
        }
        medium->tymed = TYMED_HGLOBAL; medium->hGlobal = global; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
    FILEGROUPDESCRIPTORW descriptor = {};
    size_t descriptorBytes = sizeof(FILEGROUPDESCRIPTORW);
    std::string data;
    bool stream = false;
    int contentReads = 0;
    ULONG refs = 1;
};
}

TEST(MSWindowsClipboardVirtualPNGTests, readsExactPngFromHGlobal) {
    VirtualPNG source; std::string png;
    ASSERT_TRUE(MSWindowsClipboardVirtualPNG::read(&source, png));
    EXPECT_EQ(source.data, png);
}
TEST(MSWindowsClipboardVirtualPNGTests, readsExactPngFromIStream) {
    VirtualPNG source; source.stream = true; std::string png;
    ASSERT_TRUE(MSWindowsClipboardVirtualPNG::read(&source, png));
    EXPECT_EQ(source.data, png);
}
TEST(MSWindowsClipboardVirtualPNGTests, rejectsMultipleDescriptorsBeforeReadingContent) {
    VirtualPNG source; source.descriptor.cItems = 2; std::string png;
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::read(&source, png)); EXPECT_EQ(0, source.contentReads);
}
TEST(MSWindowsClipboardVirtualPNGTests, rejectsTruncatedDescriptor) {
    VirtualPNG source; source.descriptorBytes = sizeof(UINT); std::string png;
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::read(&source, png)); EXPECT_EQ(0, source.contentReads);
}
TEST(MSWindowsClipboardVirtualPNGTests, rejectsDirectoriesAndUnterminatedNames) {
    VirtualPNG source; source.descriptor.fgd[0].dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY; std::string png;
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::read(&source, png));
    source.descriptor.fgd[0].dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    std::fill_n(source.descriptor.fgd[0].cFileName, MAX_PATH, L'a');
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::read(&source, png)); EXPECT_EQ(0, source.contentReads);
}
TEST(MSWindowsClipboardVirtualPNGTests, rejectsOversizedDeclaredContentBeforeRead) {
    VirtualPNG source; source.descriptor.fgd[0].nFileSizeHigh = 1; std::string png;
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::read(&source, png)); EXPECT_EQ(0, source.contentReads);
}
TEST(MSWindowsClipboardVirtualPNGTests, rejectsShortStream) {
    VirtualPNG source; source.stream = true; source.descriptor.fgd[0].nFileSizeLow += 10; std::string png;
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::read(&source, png)); EXPECT_TRUE(png.empty());
}
TEST(MSWindowsClipboardVirtualPNGTests, rejectsInvalidPngEvenWithValidSignature) {
    VirtualPNG source; source.data.resize(8); source.descriptor.fgd[0].nFileSizeLow = 8; std::string png;
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::read(&source, png)); EXPECT_TRUE(png.empty());
}
TEST(MSWindowsClipboardVirtualPNGTests, rejectsMissingSizeAndNonPngNames) {
    VirtualPNG source; source.descriptor.fgd[0].dwFlags = FD_ATTRIBUTES; std::string png;
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::read(&source, png));
    source.descriptor.fgd[0].dwFlags |= FD_FILESIZE;
    wcscpy_s(source.descriptor.fgd[0].cFileName, L"synthetic.txt");
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::read(&source, png)); EXPECT_EQ(0, source.contentReads);
}
TEST(MSWindowsClipboardVirtualPNGTests, rejectsPngTrailingGarbage) {
    VirtualPNG source; source.data += "trailing"; source.descriptor.fgd[0].nFileSizeLow = static_cast<DWORD>(source.data.size()); std::string png;
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::read(&source, png)); EXPECT_TRUE(png.empty());
}
TEST(MSWindowsClipboardVirtualPNGTests, rejectsStreamLongerThanDescriptor) {
    VirtualPNG source; source.stream = true; source.data += "extra"; std::string png;
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::read(&source, png)); EXPECT_TRUE(png.empty());
}
TEST(MSWindowsClipboardVirtualPNGTests, rejectsPathMetadata) {
    VirtualPNG source; wcscpy_s(source.descriptor.fgd[0].cFileName, L"C:\\secret.png"); std::string png;
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::read(&source, png)); EXPECT_EQ(0, source.contentReads);
}
TEST(MSWindowsClipboardVirtualPNGTests, rejectsCorruptPngCRC) {
    VirtualPNG source; source.data[29] ^= 1; std::string png;
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::read(&source, png)); EXPECT_TRUE(png.empty());
}
TEST(MSWindowsClipboardVirtualPNGTests, rejectsPixelBombBeforeDecode) {
    VirtualPNG source;
    source.data[16] = 0x7f; source.data[17] = static_cast<char>(0xff);
    source.data[18] = static_cast<char>(0xff); source.data[19] = static_cast<char>(0xff);
    lodepng_chunk_generate_crc(reinterpret_cast<unsigned char*>(&source.data[8])); std::string png;
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::read(&source, png)); EXPECT_TRUE(png.empty());
}
TEST(MSWindowsClipboardVirtualPNGTests, failedVirtualReadKeepsSnapshotAndRequestsRetry) {
    VirtualPNG source; source.data.resize(8);
    source.descriptor.fgd[0].nFileSizeLow = 8;
    Clipboard snapshot;
    snapshot.open(0); snapshot.add(IClipboard::kText, "keep previous formats"); snapshot.close();
    const String before = snapshot.marshall();
    EXPECT_FALSE(MSWindowsClipboardVirtualPNG::addToSnapshot(&source, snapshot));
    EXPECT_EQ(before, snapshot.marshall());
}
TEST(MSWindowsClipboardVirtualPNGTests, validVirtualReadAddsPngAndPreservesOtherFormats) {
    VirtualPNG source; Clipboard snapshot;
    snapshot.open(0); snapshot.add(IClipboard::kText, "keep text"); snapshot.close();
    ASSERT_TRUE(MSWindowsClipboardVirtualPNG::addToSnapshot(&source, snapshot));
    snapshot.open(0);
    EXPECT_EQ(source.data, snapshot.get(IClipboard::kPNG));
    EXPECT_EQ("keep text", snapshot.get(IClipboard::kText));
    snapshot.close();
}
