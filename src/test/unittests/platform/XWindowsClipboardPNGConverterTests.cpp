#include "test/global/gtest.h"

#include "platform/XWindowsClipboardPNGConverter.h"

#include <X11/Xlib.h>

#include <string>

TEST(XWindowsClipboardPNGConverterTests, toIClipboard_acceptsPngSignatureWithHighBitByte)
{
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        return;
    }

    XWindowsClipboardPNGConverter converter(display);
    const char signature[] = {
        static_cast<char>(0x89), 'P', 'N', 'G', '\r', '\n',
        static_cast<char>(0x1a), '\n'
    };
    const std::string pngData(signature, sizeof(signature));

    EXPECT_EQ(pngData, converter.toIClipboard(pngData));

    XCloseDisplay(display);
}

TEST(XWindowsClipboardPNGConverterTests, pngBinaryTextFallback_isSuppressed)
{
    const char signature[] = {
        static_cast<char>(0x89), 'P', 'N', 'G', '\r', '\n',
        static_cast<char>(0x1a), '\n'
    };

    EXPECT_TRUE(XWindowsClipboard::shouldSuppressPngTextFallbackForTest(
        std::string(signature, sizeof(signature))));
    EXPECT_FALSE(XWindowsClipboard::shouldSuppressPngTextFallbackForTest(
        "normal clipboard text"));
}
