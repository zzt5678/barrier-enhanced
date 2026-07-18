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
