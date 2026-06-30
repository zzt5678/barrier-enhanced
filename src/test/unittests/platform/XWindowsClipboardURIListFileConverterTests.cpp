#include "test/global/gtest.h"

#include "barrier/RemoteFileClipboard.h"
#include "platform/XWindowsClipboardURIListFileConverter.h"

#include <X11/Xlib.h>

#include <string>

TEST(XWindowsClipboardURIListFileConverterTests, fromIClipboard_withPlainText_returnsEmpty)
{
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        return;
    }

    XWindowsClipboardURIListFileConverter converter(display);
    XWindowsClipboardURIListFileConverter gnomeConverter(
        display, "x-special/gnome-copied-files", true);

    EXPECT_EQ("", converter.fromIClipboard("hello from windows"));
    EXPECT_EQ("", gnomeConverter.fromIClipboard("hello from windows"));

    XCloseDisplay(display);
}

TEST(XWindowsClipboardURIListFileConverterTests, fromIClipboard_withPathLikeText_returnsEmpty)
{
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        return;
    }

    XWindowsClipboardURIListFileConverter converter(display);
    XWindowsClipboardURIListFileConverter gnomeConverter(
        display, "x-special/gnome-copied-files", true);

    EXPECT_EQ("", converter.fromIClipboard("/tmp/example.txt"));
    EXPECT_EQ("", gnomeConverter.fromIClipboard("/tmp/example.txt"));

    XCloseDisplay(display);
}

TEST(XWindowsClipboardURIListFileConverterTests, toIClipboard_rejectsOversizedRawSelection)
{
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        return;
    }

    XWindowsClipboardURIListFileConverter converter(display);
    EXPECT_EQ("", converter.toIClipboard(std::string(RemoteFileClipboard::kMaxNativeFileSelectionBytes + 1, 'x')));

    XCloseDisplay(display);
}

TEST(XWindowsClipboardURIListFileConverterTests, toIClipboard_rejectsDecodedPathOverSharedLimit)
{
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        return;
    }

    XWindowsClipboardURIListFileConverter converter(display);
    const std::string uri = "file:///tmp/" + std::string(RemoteFileClipboard::kMaxClipboardPathBytes, 'a') + "\r\n";
    EXPECT_EQ("", converter.toIClipboard(uri));

    XCloseDisplay(display);
}
