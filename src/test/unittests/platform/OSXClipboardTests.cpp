// SPDX-License-Identifier: GPL-2.0-or-later

#include "platform/OSXClipboard.h"
#include "test/global/gtest.h"
#include "lodepng.h"

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
