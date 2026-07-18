/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "platform/MSWindowsClipboardBridge.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace {

void appendUInt32(std::string* data, UInt32 value)
{
    data->push_back(static_cast<char>((value >> 24) & 0xff));
    data->push_back(static_cast<char>((value >> 16) & 0xff));
    data->push_back(static_cast<char>((value >> 8) & 0xff));
    data->push_back(static_cast<char>(value & 0xff));
}

} // namespace

TEST(MSWindowsClipboardBridgeTests, PipeSuffixFailsClosedWhenRandomSourceFails)
{
    std::string suffix = "stale";

    EXPECT_FALSE(MSWindowsClipboardBridgeProtocol::generatePipeSuffix(
        suffix,
        [](unsigned char*, std::size_t) {
            return false;
        }));
    EXPECT_TRUE(suffix.empty());
}

TEST(MSWindowsClipboardBridgeTests, PipeSuffixUsesInjectedRandomBytes)
{
    std::string suffix;

    EXPECT_TRUE(MSWindowsClipboardBridgeProtocol::generatePipeSuffix(
        suffix,
        [](unsigned char* bytes, std::size_t size) {
            std::fill(bytes, bytes + size, 0xab);
            return true;
        }));
    EXPECT_EQ("abababababababababababababababab", suffix);
}

TEST(MSWindowsClipboardBridgeTests, AcceptsValidMarshalledClipboard)
{
    std::string snapshot;
    appendUInt32(&snapshot, 1);
    appendUInt32(&snapshot, IClipboard::kText);
    appendUInt32(&snapshot, 3);
    snapshot += "abc";

    std::string error;
    EXPECT_TRUE(MSWindowsClipboardBridgeProtocol::validateSnapshot(snapshot,
                                                                    &error));
    EXPECT_TRUE(error.empty());
}

TEST(MSWindowsClipboardBridgeTests, RejectsTruncatedMarshalledClipboard)
{
    std::string snapshot;
    appendUInt32(&snapshot, 1);
    appendUInt32(&snapshot, IClipboard::kText);
    appendUInt32(&snapshot, 5);
    snapshot += "abc";

    std::string error;
    EXPECT_FALSE(MSWindowsClipboardBridgeProtocol::validateSnapshot(snapshot,
                                                                     &error));
    EXPECT_FALSE(error.empty());
}

TEST(MSWindowsClipboardBridgeTests, RejectsTrailingMarshalledClipboardData)
{
    std::string snapshot(4, '\0');
    snapshot += "unexpected";

    EXPECT_FALSE(MSWindowsClipboardBridgeProtocol::validateSnapshot(snapshot,
                                                                     NULL));
}

TEST(MSWindowsClipboardBridgeTests, RejectsOversizedResponseBeforeAllocation)
{
    MSWindowsClipboardBridgeProtocol::ResponseHeader response;
    response.magic = MSWindowsClipboardBridgeProtocol::kMagic;
    response.status = MSWindowsClipboardBridgeProtocol::kSuccess;
    response.size = static_cast<UInt32>(
        MSWindowsClipboardBridgeProtocol::kMaxSnapshotBytes + 1);

    EXPECT_FALSE(MSWindowsClipboardBridgeProtocol::validateResponseHeader(
        response, NULL));
}

TEST(MSWindowsClipboardBridgeTests, RejectsErrorResponseWithPayload)
{
    MSWindowsClipboardBridgeProtocol::ResponseHeader response;
    response.magic = MSWindowsClipboardBridgeProtocol::kMagic;
    response.status =
        MSWindowsClipboardBridgeProtocol::kClipboardUnavailable;
    response.size = 4;

    EXPECT_FALSE(MSWindowsClipboardBridgeProtocol::validateResponseHeader(
        response, NULL));
}

TEST(MSWindowsClipboardBridgeTests, AcceptsPublishSuccessSequencePayload)
{
    MSWindowsClipboardBridgeProtocol::ResponseHeader response;
    response.magic = MSWindowsClipboardBridgeProtocol::kMagic;
    response.status = MSWindowsClipboardBridgeProtocol::kSuccess;
    response.size = sizeof(MSWindowsClipboardBridgeProtocol::PublishResponse);

    EXPECT_TRUE(MSWindowsClipboardBridgeProtocol::validateResponseHeader(
        response, NULL));
}

TEST(MSWindowsClipboardBridgeTests, AcceptsPayloadFreeRevisionSuperseded)
{
    MSWindowsClipboardBridgeProtocol::ResponseHeader response;
    response.magic = MSWindowsClipboardBridgeProtocol::kMagic;
    response.status = MSWindowsClipboardBridgeProtocol::kRevisionChanged;
    response.size = 0;

    EXPECT_TRUE(MSWindowsClipboardBridgeProtocol::validateResponseHeader(
        response, NULL));
}

TEST(MSWindowsClipboardBridgeTests, HelperSessionMustMatchOwningNodeSession)
{
    EXPECT_TRUE(MSWindowsClipboardBridgeProtocol::isCurrentHelperSession(7, 7));
    EXPECT_FALSE(MSWindowsClipboardBridgeProtocol::isCurrentHelperSession(7, 8));
    EXPECT_FALSE(MSWindowsClipboardBridgeProtocol::isCurrentHelperSession(
        0xffffffff, 0xffffffff));
}

TEST(MSWindowsClipboardBridgeTests, HelperOwnershipFollowsNodeSessionNotActiveConsole)
{
    EXPECT_EQ(7u, MSWindowsClipboardBridgeProtocol::selectOwningSession(7, 8));
    EXPECT_EQ(23u, MSWindowsClipboardBridgeProtocol::selectOwningSession(
        23, 0xffffffff));
    EXPECT_EQ(0xffffffffu,
        MSWindowsClipboardBridgeProtocol::selectOwningSession(
            0xffffffff, 8));
}

TEST(MSWindowsClipboardBridgeTests, HelperIdentityRequiresActiveMatchingOwner)
{
    EXPECT_TRUE(MSWindowsClipboardBridgeProtocol::isCurrentHelperIdentity(
        7, "S-1-5-21-100", 7, "S-1-5-21-100", true));
    EXPECT_FALSE(MSWindowsClipboardBridgeProtocol::isCurrentHelperIdentity(
        7, "S-1-5-21-100", 7, "S-1-5-21-100", false));
    EXPECT_FALSE(MSWindowsClipboardBridgeProtocol::isCurrentHelperIdentity(
        7, "S-1-5-21-100", 7, "S-1-5-21-200", true));
    EXPECT_FALSE(MSWindowsClipboardBridgeProtocol::isCurrentHelperIdentity(
        7, "", 7, "", true));
}
