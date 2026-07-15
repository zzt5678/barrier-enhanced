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

namespace {

void appendUInt32(std::string* data, UInt32 value)
{
    data->push_back(static_cast<char>((value >> 24) & 0xff));
    data->push_back(static_cast<char>((value >> 16) & 0xff));
    data->push_back(static_cast<char>((value >> 8) & 0xff));
    data->push_back(static_cast<char>(value & 0xff));
}

} // namespace

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
