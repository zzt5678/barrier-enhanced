/*
 * Weave -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "arch/win32/ArchDaemonWindows.h"
#include "common/ProductIdentity.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<SC_HANDLE> g_closedHandles;

BOOL WINAPI
recordClosedServiceHandle(SC_HANDLE handle)
{
    g_closedHandles.push_back(handle);
    return TRUE;
}

SC_HANDLE
fakeServiceHandle(std::uintptr_t value)
{
    return reinterpret_cast<SC_HANDLE>(value);
}

} // namespace

TEST(MSWindowsDaemonLifecycleTests, deleteServiceUsesWin32SuccessPolarity)
{
    EXPECT_TRUE(ArchDaemonWindowsPolicy::deleteServiceSucceeded(TRUE));
    EXPECT_FALSE(ArchDaemonWindowsPolicy::deleteServiceSucceeded(FALSE));
}

TEST(MSWindowsDaemonLifecycleTests, serviceTextConversionPreservesUnicode)
{
    const std::string utf8 =
        u8"C:\\Users\\\u7528\u6237\\Weave\\weaved.exe \U0001f600";
    std::wstring wide;

    ASSERT_TRUE(ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
        utf8, false, wide));
    EXPECT_EQ(
        L"C:\\Users\\\u7528\u6237\\Weave\\weaved.exe \U0001f600",
        wide);
}

TEST(MSWindowsDaemonLifecycleTests, serviceTextConversionFailsClosed)
{
    std::wstring wide = L"must be cleared";
    EXPECT_FALSE(ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
        std::string("bad\xc0\xaf", 5u), false, wide));
    EXPECT_TRUE(wide.empty());

    wide = L"must be cleared";
    EXPECT_FALSE(ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
        std::string("weaved.exe\0--hidden", 20u), false, wide));
    EXPECT_TRUE(wide.empty());

    EXPECT_FALSE(ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
        std::string(), false, wide));
    EXPECT_TRUE(ArchDaemonWindowsPolicy::utf8ServiceTextToWide(
        std::string(), true, wide));
    EXPECT_TRUE(wide.empty());
}

TEST(MSWindowsDaemonLifecycleTests, serviceArgumentsPreserveUnicode)
{
    const WCHAR serviceName[] = L"Weave\u670d\u52a1";
    const WCHAR screenName[] = L"\u663e\u793a\u5668\U0001f600";
    const WCHAR empty[] = L"";
    LPCWSTR wideArguments[] = { serviceName, screenName, empty };
    std::vector<std::string> utf8Arguments;

    ASSERT_TRUE(ArchDaemonWindowsPolicy::wideServiceArgumentsToUtf8(
        3u, wideArguments, utf8Arguments));
    ASSERT_EQ(3u, utf8Arguments.size());
    EXPECT_EQ(u8"Weave\u670d\u52a1", utf8Arguments[0]);
    EXPECT_EQ(u8"\u663e\u793a\u5668\U0001f600", utf8Arguments[1]);
    EXPECT_TRUE(utf8Arguments[2].empty());
}

TEST(MSWindowsDaemonLifecycleTests, serviceArgumentsRejectInvalidBoundaries)
{
    const WCHAR serviceName[] = L"WeaveService";
    const WCHAR malformed[] = { 0xd800, 0 };
    LPCWSTR malformedArguments[] = { serviceName, malformed };
    std::vector<std::string> utf8Arguments(1u, "must be cleared");

    EXPECT_FALSE(ArchDaemonWindowsPolicy::wideServiceArgumentsToUtf8(
        2u, malformedArguments, utf8Arguments));
    EXPECT_TRUE(utf8Arguments.empty());
    EXPECT_FALSE(ArchDaemonWindowsPolicy::wideServiceArgumentsToUtf8(
        0u, nullptr, utf8Arguments));

    LPCWSTR nullArguments[] = { serviceName, nullptr };
    EXPECT_FALSE(ArchDaemonWindowsPolicy::wideServiceArgumentsToUtf8(
        2u, nullArguments, utf8Arguments));
}

TEST(MSWindowsDaemonLifecycleTests, scopedHandlesCloseManagerAndServiceOnException)
{
    g_closedHandles.clear();
    const SC_HANDLE manager = fakeServiceHandle(1);
    const SC_HANDLE service = fakeServiceHandle(2);

    EXPECT_THROW(
        {
            ArchDaemonWindowsPolicy::ScopedServiceHandle managerHandle(
                manager, &recordClosedServiceHandle);
            ArchDaemonWindowsPolicy::ScopedServiceHandle serviceHandle(
                service, &recordClosedServiceHandle);
            throw std::runtime_error("simulated SCM failure");
        },
        std::runtime_error);

    ASSERT_EQ(2u, g_closedHandles.size());
    EXPECT_EQ(service, g_closedHandles[0]);
    EXPECT_EQ(manager, g_closedHandles[1]);
}

TEST(MSWindowsDaemonLifecycleTests, scopedNullHandleDoesNotInvokeCloser)
{
    g_closedHandles.clear();
    {
        ArchDaemonWindowsPolicy::ScopedServiceHandle handle(
            nullptr, &recordClosedServiceHandle);
    }
    EXPECT_TRUE(g_closedHandles.empty());
}

TEST(MSWindowsDaemonLifecycleTests, legacyMigrationAcceptsOnlyCanonicalWeaveDaemon)
{
    const std::wstring programFiles = L"C:\\Program Files";
    const std::wstring canonical =
        L"\\\\?\\C:\\Program Files\\Weave\\weaved.exe";

    EXPECT_TRUE(ArchDaemonWindowsPolicy::isSafeLegacyServiceMigrationPath(
        L"\"C:\\Program Files\\Weave\\weaved.exe\"",
        programFiles,
        canonical,
        canonical));
    EXPECT_TRUE(ArchDaemonWindowsPolicy::isSafeLegacyServiceMigrationPath(
        L"\"c:\\program files\\weave\\WEAVED.EXE\"",
        programFiles + L"\\",
        L"\\\\?\\c:\\program files\\weave\\WEAVED.EXE",
        canonical));
}

TEST(MSWindowsDaemonLifecycleTests, legacyMigrationRejectsArgumentsAndUnquotedPath)
{
    const std::wstring programFiles = L"C:\\Program Files";
    const std::wstring canonical =
        L"\\\\?\\C:\\Program Files\\Weave\\weaved.exe";

    EXPECT_FALSE(ArchDaemonWindowsPolicy::isSafeLegacyServiceMigrationPath(
        L"\"C:\\Program Files\\Weave\\weaved.exe\" --service",
        programFiles,
        canonical,
        canonical));
    EXPECT_FALSE(ArchDaemonWindowsPolicy::isSafeLegacyServiceMigrationPath(
        L"C:\\Program Files\\Weave\\weaved.exe",
        programFiles,
        canonical,
        canonical));
}

TEST(MSWindowsDaemonLifecycleTests, legacyMigrationRejectsLookalikeConfiguredPath)
{
    const std::wstring programFiles = L"C:\\Program Files";
    const std::wstring canonical =
        L"\\\\?\\C:\\Program Files\\Weave\\weaved.exe";

    EXPECT_FALSE(ArchDaemonWindowsPolicy::isSafeLegacyServiceMigrationPath(
        L"\"C:\\Program Files\\Barrier\\weaved.exe\"",
        programFiles,
        canonical,
        canonical));
    EXPECT_FALSE(ArchDaemonWindowsPolicy::isSafeLegacyServiceMigrationPath(
        L"\"C:\\Program Files\\Weave-old\\weaved.exe\"",
        programFiles,
        canonical,
        canonical));
    EXPECT_FALSE(ArchDaemonWindowsPolicy::isSafeLegacyServiceMigrationPath(
        L"\"C:\\Program Files\\Weave\\weaved.exe.bak\"",
        programFiles,
        canonical,
        canonical));
}

TEST(MSWindowsDaemonLifecycleTests, legacyMigrationRejectsReparseRedirection)
{
    const std::wstring programFiles = L"C:\\Program Files";
    const std::wstring canonical =
        L"\\\\?\\C:\\Program Files\\Weave\\weaved.exe";
    const std::wstring outside = L"\\\\?\\D:\\Untrusted\\weaved.exe";

    EXPECT_FALSE(ArchDaemonWindowsPolicy::isSafeLegacyServiceMigrationPath(
        L"\"C:\\Program Files\\Weave\\weaved.exe\"",
        programFiles,
        outside,
        canonical));
    EXPECT_FALSE(ArchDaemonWindowsPolicy::isSafeLegacyServiceMigrationPath(
        L"\"C:\\Program Files\\Weave\\weaved.exe\"",
        programFiles,
        canonical,
        outside));
}

TEST(MSWindowsDaemonLifecycleTests,
     postInstallMigrationRunsOnlyAfterWeaveServiceIsFullyConfigured)
{
    ArchDaemonWindowsPolicy::PostInstallMigrationState state;
    state.serviceConfigured = true;
    state.parametersConfigured = true;
    std::vector<std::string> calls;

    EXPECT_TRUE(ArchDaemonWindowsPolicy::migrateLegacyServiceAfterInstall(
        WEAVE_SERVICE_NAME,
        state,
        [&calls]() {
            calls.push_back("probe");
            return true;
        },
        [&calls]() {
            calls.push_back("remove");
        }));

    ASSERT_EQ(2u, calls.size());
    EXPECT_EQ("probe", calls[0]);
    EXPECT_EQ("remove", calls[1]);
}

TEST(MSWindowsDaemonLifecycleTests,
     postInstallMigrationSkipsOtherServicesAndIncompleteInstalls)
{
    unsigned int probeCalls = 0;
    unsigned int removalCalls = 0;
    const auto probe = [&probeCalls]() {
        ++probeCalls;
        return true;
    };
    const auto remove = [&removalCalls]() {
        ++removalCalls;
    };

    ArchDaemonWindowsPolicy::PostInstallMigrationState incompleteState;
    incompleteState.serviceConfigured = true;
    EXPECT_FALSE(ArchDaemonWindowsPolicy::migrateLegacyServiceAfterInstall(
        WEAVE_SERVICE_NAME, incompleteState, probe, remove));

    ArchDaemonWindowsPolicy::PostInstallMigrationState completeState;
    completeState.serviceConfigured = true;
    completeState.parametersConfigured = true;
    EXPECT_FALSE(ArchDaemonWindowsPolicy::migrateLegacyServiceAfterInstall(
        "Barrier", completeState, probe, remove));
    EXPECT_FALSE(ArchDaemonWindowsPolicy::migrateLegacyServiceAfterInstall(
        "WeaveService-lookalike", completeState, probe, remove));
    EXPECT_FALSE(ArchDaemonWindowsPolicy::migrateLegacyServiceAfterInstall(
        nullptr, completeState, probe, remove));

    EXPECT_EQ(0u, probeCalls);
    EXPECT_EQ(0u, removalCalls);
}

TEST(MSWindowsDaemonLifecycleTests,
     postInstallMigrationLeavesIneligibleLegacyServiceUntouched)
{
    ArchDaemonWindowsPolicy::PostInstallMigrationState state;
    state.serviceConfigured = true;
    state.parametersConfigured = true;
    unsigned int removalCalls = 0;

    EXPECT_FALSE(ArchDaemonWindowsPolicy::migrateLegacyServiceAfterInstall(
        WEAVE_SERVICE_NAME,
        state,
        []() { return false; },
        [&removalCalls]() { ++removalCalls; }));
    EXPECT_EQ(0u, removalCalls);
}

TEST(MSWindowsDaemonLifecycleTests,
     postInstallMigrationDoesNotHideLegacyRemovalFailure)
{
    ArchDaemonWindowsPolicy::PostInstallMigrationState state;
    state.serviceConfigured = true;
    state.parametersConfigured = true;

    EXPECT_THROW(
        ArchDaemonWindowsPolicy::migrateLegacyServiceAfterInstall(
            WEAVE_SERVICE_NAME,
            state,
            []() { return true; },
            []() { throw std::runtime_error("simulated removal failure"); }),
        std::runtime_error);
}
