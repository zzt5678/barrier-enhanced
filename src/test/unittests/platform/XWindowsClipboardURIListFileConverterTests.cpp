#include "test/global/gtest.h"

#include "platform/XWindowsClipboardURIListFileConverter.h"

#include <X11/Xlib.h>

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
