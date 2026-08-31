/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License.
 */

#include "arch/win32/ArchMiscWindows.h"
#include "arch/XArch.h"

#include "test/global/gtest.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <sstream>
#include <string>
#include <vector>

TEST(MSWindowsRegistryTests, reportsRegistryWriteFailure)
{
    std::ostringstream name;
    name << "Software\\WeaveTests\\RegistryWrite-" << GetCurrentProcessId();
    const std::string path = name.str();

    HKEY writable = NULL;
    ASSERT_EQ(ERROR_SUCCESS, RegCreateKeyExA(
        HKEY_CURRENT_USER, path.c_str(), 0, NULL, 0, KEY_ALL_ACCESS,
        NULL, &writable, NULL));
    ASSERT_NE(static_cast<HKEY>(NULL), writable);
    RegCloseKey(writable);

    HKEY readOnly = NULL;
    ASSERT_EQ(ERROR_SUCCESS, RegOpenKeyExA(
        HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &readOnly));
    ASSERT_NE(static_cast<HKEY>(NULL), readOnly);

    EXPECT_THROW(
        ArchMiscWindows::setValue(readOnly, "Rejected", "value"), XArch);

    RegCloseKey(readOnly);
    EXPECT_EQ(ERROR_SUCCESS, RegDeleteKeyA(HKEY_CURRENT_USER, path.c_str()));
    RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\WeaveTests");
}

TEST(MSWindowsRegistryTests, utf8StringSurvivesRegistryCloseAndReopen)
{
    std::ostringstream name;
    name << "Software\\WeaveTests\\Utf8RoundTrip-" << GetCurrentProcessId();
    const std::string path = name.str();
    const std::string command =
        u8"\"C:\\Users\\\u7528\u6237\\Weave\\weavec.exe\" --name "
        u8"\u663e\u793a\u5668\U0001f600";

    HKEY key = NULL;
    ASSERT_EQ(ERROR_SUCCESS, RegCreateKeyExA(
        HKEY_CURRENT_USER, path.c_str(), 0, NULL, 0, KEY_ALL_ACCESS,
        NULL, &key, NULL));
    ASSERT_NE(static_cast<HKEY>(NULL), key);
    ASSERT_NO_THROW(ArchMiscWindows::setValueUtf8(
        key, "Command", command));
    RegCloseKey(key);

    key = NULL;
    ASSERT_EQ(ERROR_SUCCESS, RegOpenKeyExA(
        HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key));
    ASSERT_NE(static_cast<HKEY>(NULL), key);
    EXPECT_EQ(command, ArchMiscWindows::readValueStringUtf8(key, "Command"));

    DWORD type = 0;
    DWORD bytes = 0;
    ASSERT_EQ(ERROR_SUCCESS, RegQueryValueExW(
        key, L"Command", NULL, &type, NULL, &bytes));
    EXPECT_EQ(REG_SZ, type);
    EXPECT_EQ(0u, bytes % sizeof(WCHAR));
    RegCloseKey(key);

    EXPECT_EQ(ERROR_SUCCESS, RegDeleteKeyA(HKEY_CURRENT_USER, path.c_str()));
    RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\WeaveTests");
}

TEST(MSWindowsRegistryTests, utf8RegistryWriteRejectsInvalidText)
{
    std::ostringstream name;
    name << "Software\\WeaveTests\\Utf8Reject-" << GetCurrentProcessId();
    const std::string path = name.str();

    HKEY key = NULL;
    ASSERT_EQ(ERROR_SUCCESS, RegCreateKeyExA(
        HKEY_CURRENT_USER, path.c_str(), 0, NULL, 0, KEY_ALL_ACCESS,
        NULL, &key, NULL));
    ASSERT_NE(static_cast<HKEY>(NULL), key);

    EXPECT_THROW(ArchMiscWindows::setValueUtf8(
        key, "Command", std::string("bad\xc0\xaf", 5u)), XArch);
    EXPECT_THROW(ArchMiscWindows::setValueUtf8(
        key, "Command", std::string("visible\0hidden", 14u)), XArch);

    DWORD bytes = 0;
    EXPECT_EQ(ERROR_FILE_NOT_FOUND, RegQueryValueExW(
        key, L"Command", NULL, NULL, NULL, &bytes));
    RegCloseKey(key);

    EXPECT_EQ(ERROR_SUCCESS, RegDeleteKeyA(HKEY_CURRENT_USER, path.c_str()));
    RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\WeaveTests");
}

TEST(MSWindowsRegistryTests, utf8RegistryReadRejectsMalformedRegSz)
{
    std::ostringstream name;
    name << "Software\\WeaveTests\\Utf8Malformed-" << GetCurrentProcessId();
    const std::string path = name.str();

    HKEY key = NULL;
    ASSERT_EQ(ERROR_SUCCESS, RegCreateKeyExA(
        HKEY_CURRENT_USER, path.c_str(), 0, NULL, 0, KEY_ALL_ACCESS,
        NULL, &key, NULL));
    ASSERT_NE(static_cast<HKEY>(NULL), key);
    const WCHAR malformed[] = { 0xd800, 0 };
    ASSERT_EQ(ERROR_SUCCESS, RegSetValueExW(
        key, L"Command", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(malformed), sizeof(malformed)));

    EXPECT_TRUE(
        ArchMiscWindows::readValueStringUtf8(key, "Command").empty());
    RegCloseKey(key);

    EXPECT_EQ(ERROR_SUCCESS, RegDeleteKeyA(HKEY_CURRENT_USER, path.c_str()));
    RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\WeaveTests");
}
