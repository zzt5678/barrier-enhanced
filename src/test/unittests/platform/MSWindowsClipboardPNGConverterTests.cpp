#include "platform/MSWindowsClipboardPNGConverter.h"
#include "test/global/gtest.h"

TEST(MSWindowsClipboardPNGConverterTests, preservesHighBitPngSignature)
{
    MSWindowsClipboardPNGConverter converter;
    const std::string png("\x89PNG\r\n\x1a\n", 8);
    HANDLE handle = converter.fromIClipboard(png);
    ASSERT_NE(nullptr, handle);
    EXPECT_EQ(png, converter.toIClipboard(handle));
    GlobalFree(handle);
}

TEST(MSWindowsClipboardPNGConverterTests, rejectsInvalidSignatureTail)
{
    MSWindowsClipboardPNGConverter converter;
    const std::string invalid("\x89PNGxxxx", 8);
    HANDLE handle = converter.fromIClipboard(invalid);
    EXPECT_EQ(nullptr, handle);
    if (handle) GlobalFree(handle);
}
