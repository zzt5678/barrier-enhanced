#include "test/global/gtest.h"

#include "platform/XWindowsClipboardURIListPNGConverter.h"

#include <X11/Xlib.h>

#include <fstream>
#include <string>

namespace {

std::string readFile(const std::string& path)
{
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        return {};
    }

    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

}

TEST(XWindowsClipboardURIListPNGConverterTests, fromIClipboard_withPngData_returnsFileUri)
{
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        return;
    }

    XWindowsClipboardURIListPNGConverter converter(display);
    const std::string png("\x89PNG\r\n\x1a\n", 8);

    const std::string uriList = converter.fromIClipboard(png);

    XCloseDisplay(display);

    ASSERT_FALSE(uriList.empty());
    ASSERT_EQ(0u, uriList.find("file://"));
    ASSERT_TRUE(uriList.size() > 7);
    ASSERT_EQ('\n', uriList[uriList.size() - 1]);

    std::string path = uriList.substr(7);
    if (!path.empty() && path[path.size() - 1] == '\n') {
        path.erase(path.size() - 1);
    }
    if (!path.empty() && path[path.size() - 1] == '\r') {
        path.erase(path.size() - 1);
    }

    EXPECT_EQ(png, readFile(path));
}
