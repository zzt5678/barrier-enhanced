// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform/OSXClipboard.h"
#include "barrier/Clipboard.h"
#include "test/global/gtest.h"
#include "lodepng.h"
#include <array>
#include <stdexcept>

namespace {
class SnapshotClipboard : public OSXClipboard {
public:
    SnapshotClipboard() : OSXClipboard(kPasteboardUniqueName) {}
    bool open(Time time) const override { return canOpen && OSXClipboard::open(time); }
    void close() const override { ++closeCalls; OSXClipboard::close(); }
    bool has(EFormat format) const override { return present[format]; }
    std::string get(EFormat format) const override {
        if (throwOnRead) throw std::runtime_error("injected clipboard read failure");
        if (format == kBitmap) {
            ++bitmapReads;
            if (varyBitmap) return "same pixels; changing DIB padding " + std::to_string(bitmapReads);
        }
        return data[format];
    }
    std::array<bool, kNumFormats> present{};
    std::array<std::string, kNumFormats> data{};
    mutable unsigned bitmapReads = 0;
    bool varyBitmap = false;
    bool canOpen = true;
    bool throwOnRead = false;
    mutable unsigned closeCalls = 0;
};

class SnapshotDestination : public Clipboard {
public:
    bool open(Time time) const override { return canOpen && Clipboard::open(time); }
    bool empty() override { return canEmpty && Clipboard::empty(); }
    void close() const override { ++closeCalls; Clipboard::close(); }
    bool canOpen = true, canEmpty = true;
    mutable unsigned closeCalls = 0;
};

std::string privateTestPng() {
    std::vector<unsigned char> encoded;
    const std::vector<unsigned char> pixels = {255, 32, 128, 255};
    if (lodepng::encode(encoded, pixels, 1, 1) != 0) return {};
    return std::string(encoded.begin(), encoded.end());
}
}

TEST(OSXClipboardSnapshotTests, sourceAndDestinationOpenFailuresPropagate)
{
    SnapshotClipboard source;
    SnapshotDestination destination;
    source.canOpen = false;
    EXPECT_FALSE(source.copyTo(&destination));
    EXPECT_EQ(0u, source.closeCalls);
    EXPECT_EQ(0u, destination.closeCalls);
    source.canOpen = true; destination.canOpen = false;
    EXPECT_FALSE(source.copyTo(&destination));
    EXPECT_EQ(1u, source.closeCalls);
    EXPECT_EQ(0u, destination.closeCalls);
}

TEST(OSXClipboardSnapshotTests, destinationEmptyFailureClosesBothSides)
{
    SnapshotClipboard source;
    SnapshotDestination destination;
    destination.canEmpty = false;
    EXPECT_FALSE(source.copyTo(&destination));
    EXPECT_EQ(1u, source.closeCalls);
    EXPECT_EQ(1u, destination.closeCalls);
}

TEST(OSXClipboardSnapshotTests, readExceptionClosesBothSides)
{
    SnapshotClipboard source;
    SnapshotDestination destination;
    source.present[IClipboard::kPNG] = true;
    source.throwOnRead = true;
    EXPECT_THROW(source.copyTo(&destination), std::runtime_error);
    EXPECT_EQ(1u, source.closeCalls);
    EXPECT_EQ(1u, destination.closeCalls);
}

TEST(OSXClipboardSnapshotTests, stablePngDoesNotReadVaryingRedundantBitmap)
{
    SnapshotClipboard source;
    source.present[IClipboard::kPNG] = source.present[IClipboard::kBitmap] = true;
    source.data[IClipboard::kPNG] = privateTestPng();
    source.varyBitmap = true;
    Clipboard first, second;
    ASSERT_TRUE(source.copyTo(&first));
    ASSERT_TRUE(source.copyTo(&second));
    EXPECT_EQ(first.marshall(), second.marshall());
    ASSERT_TRUE(first.open(0));
    EXPECT_EQ(0u, source.bitmapReads);
    EXPECT_FALSE(first.has(IClipboard::kBitmap));
    EXPECT_EQ(source.data[IClipboard::kPNG], first.get(IClipboard::kPNG));
}

TEST(OSXClipboardSnapshotTests, failedPngReadRetainsOnlyAvailableBitmap)
{
    SnapshotClipboard source;
    source.present[IClipboard::kPNG] = source.present[IClipboard::kBitmap] = true;
    source.data[IClipboard::kBitmap] = "bitmap fallback";
    Clipboard result;
    ASSERT_TRUE(source.copyTo(&result));
    ASSERT_TRUE(result.open(0));
    EXPECT_EQ(source.data[IClipboard::kBitmap], result.get(IClipboard::kBitmap));
    EXPECT_FALSE(result.has(IClipboard::kPNG));
    EXPECT_EQ(1u, source.bitmapReads);
}

TEST(OSXClipboardSnapshotTests, absentPngRetainsBitmapAndNonImageFormats)
{
    SnapshotClipboard source;
    for (auto format : {IClipboard::kText, IClipboard::kHTML, IClipboard::kBitmap, IClipboard::kFileList}) {
        source.present[format] = true;
        source.data[format] = "payload " + std::to_string(format);
    }
    Clipboard result;
    ASSERT_TRUE(source.copyTo(&result));
    ASSERT_TRUE(result.open(0));
    for (auto format : {IClipboard::kText, IClipboard::kHTML, IClipboard::kBitmap, IClipboard::kFileList}) {
        EXPECT_TRUE(result.has(format));
        EXPECT_EQ(source.data[format], result.get(format));
    }
}

TEST(OSXClipboardSnapshotTests, successfulPngPreservesEmptyTextHtmlAndFileList)
{
    SnapshotClipboard source;
    for (auto format : {IClipboard::kText, IClipboard::kHTML, IClipboard::kPNG, IClipboard::kFileList}) {
        source.present[format] = true;
        source.data[format] = "payload " + std::to_string(format);
    }
    source.data[IClipboard::kText].clear();
    source.data[IClipboard::kPNG] = privateTestPng();
    Clipboard result;
    ASSERT_TRUE(source.copyTo(&result));
    ASSERT_TRUE(result.open(0));
    for (auto format : {IClipboard::kText, IClipboard::kHTML, IClipboard::kPNG, IClipboard::kFileList}) {
        EXPECT_TRUE(result.has(format));
        EXPECT_EQ(source.data[format], result.get(format));
    }
}

TEST(OSXClipboardSnapshotTests, privateBoardSnapshotsPreferPngAndPreserveText)
{
    OSXClipboard source(kPasteboardUniqueName);
    ASSERT_TRUE(source.open(7));
    ASSERT_TRUE(source.empty());
    std::string dib(44, '\0');
    dib[0] = 40; dib[4] = 1; dib[8] = 1; dib[12] = 1; dib[14] = 32;
    const std::string png = privateTestPng();
    source.add(IClipboard::kText, "isolated snapshot");
    source.add(IClipboard::kPNG, png);
    source.add(IClipboard::kBitmap, dib);
    for (unsigned iteration = 0; iteration < 50; ++iteration) {
        Clipboard result;
        ASSERT_TRUE(source.copyTo(&result));
        ASSERT_TRUE(result.open(0));
        EXPECT_EQ(png, result.get(IClipboard::kPNG));
        EXPECT_FALSE(result.has(IClipboard::kBitmap));
        EXPECT_EQ("isolated snapshot", result.get(IClipboard::kText));
    }
    // Carbon may synthesize PNG even from a BMP-only board. Exact missing or
    // failed PNG reads are therefore injected in the fallback tests above.
    ASSERT_TRUE(source.empty()); source.close();
}

// This is a lifecycle probe for use with macOS `leaks --atExit`. Constructing
// the object acquires and synchronizes a Pasteboard reference. open() only
// checks that reference and records a timestamp. No clipboard contents are
// read or written: deliberately do not call get(), has(), add(), or empty().
TEST(OSXClipboardTests, repeatedLifecycleAcquiresAndReleasesPasteboard)
{
    for (int iteration = 0; iteration < 1000; ++iteration) {
        OSXClipboard clipboard;
        ASSERT_TRUE(clipboard.open(0)) << "iteration " << iteration;
        clipboard.close();
    }
}

TEST(OSXClipboardTests, privatePasteboardRoundTripsTextAndPngRepeatedly)
{
    // Apple guarantees NULL/kPasteboardUniqueName creates a new resource.
    // This can never address the user's copy/paste or find pasteboards.
    OSXClipboard clipboard(kPasteboardUniqueName);
    ASSERT_TRUE(clipboard.open(0));
    std::vector<unsigned char> pixels = {255, 32, 128, 255};
    std::vector<unsigned char> encoded;
    ASSERT_EQ(0u, lodepng::encode(encoded, pixels, 1, 1));
    const std::string png(encoded.begin(), encoded.end());
    const std::string text = "Weave private clipboard roundtrip";
    for (int iteration = 0; iteration < 1000; ++iteration) {
        ASSERT_TRUE(clipboard.empty());
        EXPECT_FALSE(clipboard.has(IClipboard::kText));
        EXPECT_TRUE(clipboard.get(IClipboard::kText).empty());
        clipboard.add(IClipboard::kText, text);
        ASSERT_TRUE(clipboard.has(IClipboard::kText));
        ASSERT_EQ(text, clipboard.get(IClipboard::kText));
        clipboard.add(IClipboard::kPNG, png);
        ASSERT_TRUE(clipboard.has(IClipboard::kPNG));
        ASSERT_EQ(png, clipboard.get(IClipboard::kPNG));
    }
    ASSERT_TRUE(clipboard.empty());
    clipboard.close();
}
