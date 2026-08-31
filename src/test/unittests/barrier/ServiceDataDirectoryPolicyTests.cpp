/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/ServiceDataDirectoryPolicy.h"

#include "test/global/gtest.h"

#include <string>

TEST(ServiceDataDirectoryPolicyTests, descriptorOwnsProtectedAclAsSystem)
{
    EXPECT_EQ(
        "O:SYG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
        std::string(ServiceDataDirectoryPolicy::protectedDirectorySddl()));
}

TEST(ServiceDataDirectoryPolicyTests, onlyAcceptsRealDirectories)
{
    EXPECT_TRUE(ServiceDataDirectoryPolicy::acceptsDirectoryObject(
        true, true, false));
    EXPECT_FALSE(ServiceDataDirectoryPolicy::acceptsDirectoryObject(
        false, false, false));
    EXPECT_FALSE(ServiceDataDirectoryPolicy::acceptsDirectoryObject(
        true, false, false));
    EXPECT_FALSE(ServiceDataDirectoryPolicy::acceptsDirectoryObject(
        true, true, true));
}
