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
#include <stdexcept>
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
