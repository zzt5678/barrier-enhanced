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

TEST(XWindowsClipboardURIListFileConverterTests, fromIClipboard_withPathList_returnsUriTargets)
{
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        return;
    }

    XWindowsClipboardURIListFileConverter converter(display);
    XWindowsClipboardURIListFileConverter gnomeConverter(
        display, "x-special/gnome-copied-files", true);

    EXPECT_EQ("file:///tmp/example.txt\r\n",
              converter.fromIClipboard("/tmp/example.txt"));
    EXPECT_EQ("copy\nfile:///tmp/example.txt\r\n",
              gnomeConverter.fromIClipboard("/tmp/example.txt"));

    XCloseDisplay(display);
}
