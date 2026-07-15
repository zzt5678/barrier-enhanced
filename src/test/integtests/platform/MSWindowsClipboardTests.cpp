/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2011 Nick Bolton
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "platform/MSWindowsClipboard.h"
#include "platform/MSWindowsClipboardChangeTracker.h"
#include "platform/IMSWindowsClipboardFacade.h"

#include "test/global/gmock.h"
#include "test/global/gtest.h"

class MSWindowsClipboardTests : public ::testing::Test
{
protected:
    virtual void SetUp()
    {
        emptyClipboard();
    }

    virtual void TearDown()
    {
        emptyClipboard();
    }

private:
    void emptyClipboard()
    {
        MSWindowsClipboard clipboard(NULL);
        clipboard.open(0);
        clipboard.empty();
    }
};

class MockFacade : public IMSWindowsClipboardFacade
{
public:
    MOCK_METHOD2(write, void(HANDLE, UINT));
};

TEST(MSWindowsClipboardChangeTrackerTests, consecutiveExternalRevisionsNotify)
{
    MSWindowsClipboardChangeTracker tracker;
    tracker.reset(100);

    EXPECT_TRUE(tracker.observe(101, false));
    EXPECT_TRUE(tracker.observe(102, false));
}

TEST(MSWindowsClipboardChangeTrackerTests, duplicateRevisionDoesNotNotify)
{
    MSWindowsClipboardChangeTracker tracker;
    tracker.reset(100);

    EXPECT_FALSE(tracker.observe(100, false));
    EXPECT_TRUE(tracker.observe(101, false));
    EXPECT_FALSE(tracker.observe(101, false));
}

TEST(MSWindowsClipboardChangeTrackerTests, weaveOwnedRevisionDoesNotEcho)
{
    MSWindowsClipboardChangeTracker tracker;
    tracker.reset(100);

    EXPECT_FALSE(tracker.observe(101, true));
    EXPECT_TRUE(tracker.observe(102, false));
}

TEST_F(MSWindowsClipboardTests, emptyUnowned_openCalled_returnsTrue)
{
    MSWindowsClipboard clipboard(NULL);
    clipboard.open(0);

    bool actual = clipboard.emptyUnowned();

    EXPECT_EQ(true, actual);
}

TEST_F(MSWindowsClipboardTests, empty_openCalled_returnsTrue)
{
    MSWindowsClipboard clipboard(NULL);
    clipboard.open(0);

    bool actual = clipboard.empty();

    EXPECT_EQ(true, actual);
}

TEST_F(MSWindowsClipboardTests, empty_singleFormat_hasReturnsFalse)
{
    MSWindowsClipboard clipboard(NULL);
    clipboard.open(0);
    clipboard.add(MSWindowsClipboard::kText, "barrier rocks!");

    clipboard.empty();

    bool actual = clipboard.has(MSWindowsClipboard::kText);
    EXPECT_EQ(false, actual);
}

TEST_F(MSWindowsClipboardTests, add_newValue_valueWasStored)
{
    MSWindowsClipboard clipboard(NULL);
    clipboard.open(0);

    clipboard.add(IClipboard::kText, "barrier rocks!");

    String actual = clipboard.get(IClipboard::kText);
    EXPECT_EQ("barrier rocks!", actual);
}

TEST_F(MSWindowsClipboardTests, add_newValue_writeWasCalled)
{
    MockFacade facade;
    EXPECT_CALL(facade, write(testing::_, testing::_));

    MSWindowsClipboard clipboard(NULL);
    clipboard.setFacade(facade);
    clipboard.open(0);

    clipboard.add(IClipboard::kText, "barrier rocks!");
}

TEST_F(MSWindowsClipboardTests, add_replaceValue_valueWasReplaced)
{
    MSWindowsClipboard clipboard(NULL);
    clipboard.open(0);

    clipboard.add(IClipboard::kText, "barrier rocks!");
    clipboard.add(IClipboard::kText, "maxivista sucks"); // haha, just kidding.

    String actual = clipboard.get(IClipboard::kText);
    EXPECT_EQ("maxivista sucks", actual);
}

TEST_F(MSWindowsClipboardTests, open_timeIsZero_returnsTrue)
{
    MSWindowsClipboard clipboard(NULL);

    bool actual = clipboard.open(0);

    EXPECT_EQ(true, actual);
}

TEST_F(MSWindowsClipboardTests, open_timeIsOne_returnsTrue)
{
    MSWindowsClipboard clipboard(NULL);

    bool actual = clipboard.open(1);

    EXPECT_EQ(true, actual);
}

TEST_F(MSWindowsClipboardTests, close_isOpen_noErrors)
{
    MSWindowsClipboard clipboard(NULL);
    clipboard.open(0);

    clipboard.close();

    // can't assert anything
}

// looks like this test may fail intermittently:
// * http://buildbot.symless.com:8000/builders/trunk-win32/builds/246/steps/shell_3/logs/stdio
/*TEST_F(MSWindowsClipboardTests, getTime_openWithNoEmpty_returnsOne)
{
    MSWindowsClipboard clipboard(NULL);
    clipboard.open(1);

    MSWindowsClipboard::Time actual = clipboard.getTime();

    // this behavior is different to that of Clipboard which only
    // returns the value passed into open(t) after empty() is called.
    EXPECT_EQ(1, actual);
}*/

// this also fails intermittently:
// http://buildbot.symless.com:8000/builders/trunk-win32/builds/266/steps/shell_3/logs/stdio
/*TEST_F(MSWindowsClipboardTests, getTime_openAndEmpty_returnsOne)
{
    MSWindowsClipboard clipboard(NULL);
    clipboard.open(1);
    clipboard.empty();

    MSWindowsClipboard::Time actual = clipboard.getTime();

    EXPECT_EQ(1, actual);
}*/

TEST_F(MSWindowsClipboardTests, has_withFormatAdded_returnsTrue)
{
    MSWindowsClipboard clipboard(NULL);
    clipboard.open(0);
    clipboard.empty();
    clipboard.add(IClipboard::kText, "barrier rocks!");

    bool actual = clipboard.has(IClipboard::kText);

    EXPECT_EQ(true, actual);
}

TEST_F(MSWindowsClipboardTests, has_withNoFormats_returnsFalse)
{
    MSWindowsClipboard clipboard(NULL);
    clipboard.open(0);
    clipboard.empty();

    bool actual = clipboard.has(IClipboard::kText);

    EXPECT_EQ(false, actual);
}

TEST_F(MSWindowsClipboardTests, get_withNoFormats_returnsEmpty)
{
    MSWindowsClipboard clipboard(NULL);
    clipboard.open(0);
    clipboard.empty();

    String actual = clipboard.get(IClipboard::kText);

    EXPECT_EQ("", actual);
}

TEST_F(MSWindowsClipboardTests, get_withFormatAdded_returnsExpected)
{
    MSWindowsClipboard clipboard(NULL);
    clipboard.open(0);
    clipboard.empty();
    clipboard.add(IClipboard::kText, "barrier rocks!");

    String actual = clipboard.get(IClipboard::kText);

    EXPECT_EQ("barrier rocks!", actual);
}

TEST_F(MSWindowsClipboardTests, isOwnedByBarrier_defaultState_noError)
{
    MSWindowsClipboard clipboard(NULL);
    clipboard.open(0);

    bool actual = clipboard.isOwnedByBarrier();

    EXPECT_EQ(true, actual);
}

TEST(MSWindowsClipboardConversionTests, bitfields32Dib_convertsToPng)
{
    const UInt32 width = 2;
    const UInt32 height = 3;
    const size_t pixelOffset = 40 + 3 * sizeof(UInt32);
    std::string dib(pixelOffset + width * height * 4, '\0');

    const auto writeLE16 = [&dib](size_t offset, UInt16 value) {
        dib[offset] = static_cast<char>(value & 0xffu);
        dib[offset + 1] = static_cast<char>((value >> 8) & 0xffu);
    };
    const auto writeLE32 = [&dib](size_t offset, UInt32 value) {
        dib[offset] = static_cast<char>(value & 0xffu);
        dib[offset + 1] = static_cast<char>((value >> 8) & 0xffu);
        dib[offset + 2] = static_cast<char>((value >> 16) & 0xffu);
        dib[offset + 3] = static_cast<char>((value >> 24) & 0xffu);
    };

    writeLE32(0, 40);
    writeLE32(4, width);
    writeLE32(8, height);
    writeLE16(12, 1);
    writeLE16(14, 32);
    writeLE32(16, BI_BITFIELDS);
    writeLE32(20, width * height * 4);
    writeLE32(40, 0x00ff0000u);
    writeLE32(44, 0x0000ff00u);
    writeLE32(48, 0x000000ffu);

    for (size_t offset = pixelOffset; offset < dib.size(); offset += 4) {
        dib[offset] = static_cast<char>(143);
        dib[offset + 1] = static_cast<char>(157);
        dib[offset + 2] = static_cast<char>(42);
        dib[offset + 3] = static_cast<char>(255);
    }

    const std::string png = MSWindowsClipboard::convertDIBToPNGForTest(dib);

    ASSERT_GE(png.size(), 24u);
    EXPECT_EQ(static_cast<unsigned char>(0x89),
              static_cast<unsigned char>(png[0]));
    EXPECT_EQ("PNG", png.substr(1, 3));
    EXPECT_EQ(width, (static_cast<UInt32>(static_cast<unsigned char>(png[16])) << 24) |
                     (static_cast<UInt32>(static_cast<unsigned char>(png[17])) << 16) |
                     (static_cast<UInt32>(static_cast<unsigned char>(png[18])) << 8) |
                     static_cast<UInt32>(static_cast<unsigned char>(png[19])));
    EXPECT_EQ(height, (static_cast<UInt32>(static_cast<unsigned char>(png[20])) << 24) |
                      (static_cast<UInt32>(static_cast<unsigned char>(png[21])) << 16) |
                      (static_cast<UInt32>(static_cast<unsigned char>(png[22])) << 8) |
                      static_cast<UInt32>(static_cast<unsigned char>(png[23])));
}
