#include "test/global/gtest.h"

#include "barrier/RemoteFileClipboard.h"
#include "platform/XWindowsClipboardFileListConverter.h"

#include <X11/Xlib.h>

#include <string>

TEST(XWindowsClipboardFileListConverterTests, fromIClipboard_withMaterializedPaths_returnsUriTargets)
{
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        return;
    }

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::MaterializedPaths;
    payload.sessionId = "ready-session";
    payload.paths.push_back(barrier::fs::u8path("/tmp/weave-cache/example file.txt"));

    XWindowsClipboardFileListConverter converter(display, "text/uri-list", false);
    XWindowsClipboardFileListConverter gnomeConverter(
        display, "x-special/gnome-copied-files", true);

    EXPECT_EQ("file:///tmp/weave-cache/example%20file.txt\r\n",
              converter.fromIClipboard(RemoteFileClipboard::serialize(payload)));
    EXPECT_EQ("copy\nfile:///tmp/weave-cache/example%20file.txt\r\n",
              gnomeConverter.fromIClipboard(RemoteFileClipboard::serialize(payload)));

    XCloseDisplay(display);
}

TEST(XWindowsClipboardFileListConverterTests, fromIClipboard_withSourcePaths_returnsEmpty)
{
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        return;
    }

    RemoteFileClipboard::Data payload;
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;
    payload.sessionId = "source-session";
    payload.paths.push_back(barrier::fs::u8path("/tmp/source-only.txt"));

    XWindowsClipboardFileListConverter converter(display, "text/uri-list", false);
    XWindowsClipboardFileListConverter gnomeConverter(
        display, "x-special/gnome-copied-files", true);

    EXPECT_EQ("", converter.fromIClipboard(RemoteFileClipboard::serialize(payload)));
    EXPECT_EQ("", gnomeConverter.fromIClipboard(RemoteFileClipboard::serialize(payload)));

    XCloseDisplay(display);
}

TEST(XWindowsClipboardFileListConverterTests, toIClipboard_rejectsOversizedRawSelection)
{
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        return;
    }

    XWindowsClipboardFileListConverter converter(display, "text/uri-list", false);
    EXPECT_EQ("", converter.toIClipboard(std::string(RemoteFileClipboard::kMaxNativeFileSelectionBytes + 1, 'x')));

    XCloseDisplay(display);
}

TEST(XWindowsClipboardFileListConverterTests, toIClipboard_rejectsDecodedPathOverSharedLimit)
{
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        return;
    }

    XWindowsClipboardFileListConverter converter(display, "text/uri-list", false);
    const std::string uri = "file:///tmp/" + std::string(RemoteFileClipboard::kMaxClipboardPathBytes, 'a') + "\r\n";
    EXPECT_EQ("", converter.toIClipboard(uri));

    XCloseDisplay(display);
}
