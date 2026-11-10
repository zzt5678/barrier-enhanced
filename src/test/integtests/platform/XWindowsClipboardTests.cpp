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

// TODO: fix tests - compile error on linux
#if 0

#include "platform/XWindowsClipboard.h"

#include "test/global/gtest.h"
#include <iostream>

class CXWindowsClipboardTests : public ::testing::Test
{
protected:
    virtual void
    SetUp()
    {
        m_display = XOpenDisplay(NULL);
        int screen = DefaultScreen(m_display);
        Window root = XRootWindow(m_display, screen);

        XSetWindowAttributes attr;
        attr.do_not_propagate_mask = 0;
        attr.override_redirect = True;
        attr.cursor = Cursor();

        m_window = XCreateWindow(
            m_display, root, 0, 0, 1, 1, 0, 0,
            InputOnly, CopyFromParent, 0, &attr);
    }

    virtual void
    TearDown()
    {
        XDestroyWindow(m_display, m_window);
        XCloseDisplay(m_display);
    }

    CXWindowsClipboard&
    createClipboard()
    {
        CXWindowsClipboard* clipboard;
        clipboard = new CXWindowsClipboard(m_display, m_window, 0);
        clipboard->open(0); // needed to empty the clipboard
        clipboard->empty(); // needed to own the clipboard
        return *clipboard;
    }

    Display* m_display;
    Window m_window;
};

TEST_F(CXWindowsClipboardTests, empty_openCalled_returnsTrue)
{
    CXWindowsClipboard clipboard = createClipboard();

    bool actual = clipboard.empty();

    EXPECT_EQ(true, actual);
}

TEST_F(CXWindowsClipboardTests, empty_singleFormat_hasReturnsFalse)
{
    CXWindowsClipboard clipboard = createClipboard();
    clipboard.add(CXWindowsClipboard::kText, "barrier rocks!");

    clipboard.empty();

    bool actual = clipboard.has(CXWindowsClipboard::kText);
    EXPECT_FALSE(actual);
}

TEST_F(CXWindowsClipboardTests, add_newValue_valueWasStored)
{
    CXWindowsClipboard clipboard = createClipboard();

    clipboard.add(IClipboard::kText, "barrier rocks!");

    String actual = clipboard.get(IClipboard::kText);
    EXPECT_EQ("barrier rocks!", actual);
}

TEST_F(CXWindowsClipboardTests, add_replaceValue_valueWasReplaced)
{
    CXWindowsClipboard clipboard = createClipboard();

    clipboard.add(IClipboard::kText, "barrier rocks!");
    clipboard.add(IClipboard::kText, "maxivista sucks"); // haha, just kidding.

    String actual = clipboard.get(IClipboard::kText);
    EXPECT_EQ("maxivista sucks", actual);
}

TEST_F(CXWindowsClipboardTests, close_isOpen_noErrors)
{
    CXWindowsClipboard clipboard = createClipboard();

    // clipboard opened in createClipboard()
    clipboard.close();

    // can't assert anything
}

TEST_F(CXWindowsClipboardTests, has_withFormatAdded_returnsTrue)
{
    CXWindowsClipboard clipboard = createClipboard();
    clipboard.add(IClipboard::kText, "barrier rocks!");

    bool actual = clipboard.has(IClipboard::kText);

    EXPECT_EQ(true, actual);
}

TEST_F(CXWindowsClipboardTests, has_withNoFormats_returnsFalse)
{
    CXWindowsClipboard clipboard = createClipboard();

    bool actual = clipboard.has(IClipboard::kText);

    EXPECT_FALSE(actual);
}

TEST_F(CXWindowsClipboardTests, get_withNoFormats_returnsEmpty)
{
    CXWindowsClipboard clipboard = createClipboard();

    String actual = clipboard.get(IClipboard::kText);

    EXPECT_EQ("", actual);
}

TEST_F(CXWindowsClipboardTests, get_withFormatAdded_returnsExpected)
{
    CXWindowsClipboard clipboard = createClipboard();
    clipboard.add(IClipboard::kText, "barrier rocks!");

    String actual = clipboard.get(IClipboard::kText);

    EXPECT_EQ("barrier rocks!", actual);
}

#endif

#include "test/global/gtest.h"
#include "platform/XWindowsClipboard.h"
#include "platform/XWindowsImpl.h"
#include "platform/XWindowsUtil.h"
#include "barrier/Clipboard.h"
#include <chrono>

class XWindowsClipboardNativeTests : public ::testing::TestWithParam<int> {};

TEST_P(XWindowsClipboardNativeTests, rereadingOwnedClipboardDoesNotRequestDataFromItself)
{
    XWindowsImpl impl;
    Display* display = XOpenDisplay(NULL);
    ASSERT_NE(nullptr, display);
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                       0, 0, 1, 1, 0, 0, 0);
    {
        XWindowsClipboard source(&impl, display, window, GetParam());
        const auto timestamp = XWindowsUtil::getCurrentTime(display, window);
        ASSERT_TRUE(source.open(timestamp));
        ASSERT_TRUE(source.empty());
        source.add(IClipboard::kText, "weave-owned-clipboard-regression");
        source.close();
        Clipboard copy;
        const auto begin = std::chrono::steady_clock::now();
        ASSERT_TRUE(Clipboard::copy(&copy, &source, timestamp + 1));
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - begin).count();
        EXPECT_LT(elapsed, 0.1);
        ASSERT_TRUE(copy.open(0));
        EXPECT_EQ("weave-owned-clipboard-regression", copy.get(IClipboard::kText));
        copy.close();
        // Repeat with a later caller timestamp: neither cache nor ownership
        // time may be invalidated merely because the caller advanced time.
        ASSERT_TRUE(Clipboard::copy(&copy, &source, timestamp + 2));
        ASSERT_TRUE(copy.open(0));
        EXPECT_EQ("weave-owned-clipboard-regression", copy.get(IClipboard::kText));
        copy.close();

        // SelectionClear has not been dispatched yet. The fast path must
        // query actual X ownership rather than returning stale local bytes.
        XSetSelectionOwner(display, source.getSelection(), None, CurrentTime);
        XSync(display, False);
        ASSERT_TRUE(Clipboard::copy(&copy, &source, timestamp + 3));
        ASSERT_TRUE(copy.open(0));
        EXPECT_FALSE(copy.has(IClipboard::kText));
        copy.close();
    }
    XDestroyWindow(display, window);
    XCloseDisplay(display);
}

INSTANTIATE_TEST_SUITE_P(BothSelections, XWindowsClipboardNativeTests,
                        ::testing::Values(kClipboardClipboard, kClipboardSelection));
