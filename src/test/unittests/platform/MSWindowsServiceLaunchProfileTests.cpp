/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/win32/MSWindowsServiceLaunchProfile.h"

#include "test/global/gtest.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace {

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) : m_handle(handle) { }
    ~ScopedHandle()
    {
        if (m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }
    }
    HANDLE get() const { return m_handle; }
private:
    ScopedHandle(const ScopedHandle&);
    ScopedHandle& operator=(const ScopedHandle&);
    HANDLE m_handle;
};

} // namespace

TEST(MSWindowsServiceLaunchProfileTests, rejectsMissingAuthenticatedToken)
{
    const ServiceLaunchProfileResult result = stageWindowsServiceLaunchProfile(
        nullptr, "S-1-5-21-1-2-3-4", ServiceLaunchRole::kClient);
    EXPECT_EQ(ServiceLaunchProfileError::kInvalidArgument, result.error);
    EXPECT_TRUE(result.profilePath.empty());
    EXPECT_TRUE(result.generation.empty());
}

TEST(MSWindowsServiceLaunchProfileTests, rejectsSidThatDoesNotOwnToken)
{
    HANDLE rawToken = nullptr;
    ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken));
    ScopedHandle token(rawToken);

    const ServiceLaunchProfileResult result = stageWindowsServiceLaunchProfile(
        token.get(), "S-1-5-21-1-2-3-4294967294",
        ServiceLaunchRole::kClient);
    EXPECT_EQ(ServiceLaunchProfileError::kIdentityMismatch, result.error);
    EXPECT_TRUE(result.profilePath.empty());
    EXPECT_TRUE(result.generation.empty());
}

TEST(MSWindowsServiceLaunchProfileTests, rejectsUnknownRoleBeforeFilesystemWork)
{
    HANDLE rawToken = nullptr;
    ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken));
    ScopedHandle token(rawToken);

    const ServiceLaunchProfileResult result = stageWindowsServiceLaunchProfile(
        token.get(), "S-1-5-21-1-2-3-4",
        static_cast<ServiceLaunchRole>(99));
    EXPECT_EQ(ServiceLaunchProfileError::kInvalidArgument, result.error);
}

TEST(MSWindowsServiceLaunchProfileTests, restartLoaderRejectsInvalidSid)
{
    const ServiceLaunchProfileResult result = loadWindowsServiceLaunchProfile(
        "not-a-sid", "v1-0123456789abcdef0123456789abcdef",
        "sha256:0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef",
        ServiceLaunchRole::kClient);
    EXPECT_EQ(ServiceLaunchProfileError::kInvalidArgument, result.error);
    EXPECT_TRUE(result.profilePath.empty());
}

TEST(MSWindowsServiceLaunchProfileTests, restartLoaderRejectsUnsafeGeneration)
{
    const ServiceLaunchProfileResult result = loadWindowsServiceLaunchProfile(
        "S-1-5-18", "../v1-0123456789abcdef0123456789abcdef",
        "sha256:0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef",
        ServiceLaunchRole::kServer);
    EXPECT_EQ(ServiceLaunchProfileError::kInvalidArgument, result.error);
    EXPECT_TRUE(result.profilePath.empty());
}

TEST(MSWindowsServiceLaunchProfileTests, restartLoaderRejectsNonCanonicalDigest)
{
    const ServiceLaunchProfileResult result = loadWindowsServiceLaunchProfile(
        "S-1-5-18", "v1-0123456789abcdef0123456789abcdef",
        "SHA256:0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef",
        ServiceLaunchRole::kClient);
    EXPECT_EQ(ServiceLaunchProfileError::kInvalidArgument, result.error);
    EXPECT_TRUE(result.profilePath.empty());
}

TEST(MSWindowsServiceLaunchProfileTests, restartLoaderRejectsUnknownRole)
{
    const ServiceLaunchProfileResult result = loadWindowsServiceLaunchProfile(
        "S-1-5-18", "v1-0123456789abcdef0123456789abcdef",
        "sha256:0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef",
        static_cast<ServiceLaunchRole>(99));
    EXPECT_EQ(ServiceLaunchProfileError::kInvalidArgument, result.error);
    EXPECT_TRUE(result.profilePath.empty());
}
