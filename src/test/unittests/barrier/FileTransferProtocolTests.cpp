/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/FileTransferProtocol.h"
#include "barrier/protocol_types.h"
#include "io/IStream.h"

#include "test/global/gtest.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

class FileTransferProtocolTestStream : public barrier::IStream {
public:
    void close() override { }

    UInt32 read(void* buffer, UInt32 size) override
    {
        const UInt32 available = static_cast<UInt32>(m_bytes.size() - m_readOffset);
        const UInt32 count = std::min(size, available);
        if (count != 0) {
            std::memcpy(buffer, m_bytes.data() + m_readOffset, count);
            m_readOffset += count;
        }
        return count;
    }

    void write(const void* buffer, UInt32 size) override
    {
        const UInt8* bytes = static_cast<const UInt8*>(buffer);
        m_bytes.insert(m_bytes.end(), bytes, bytes + size);
    }

    void writeLowPriority(const void* buffer, UInt32 size) override
    {
        write(buffer, size);
    }

    void flush() override { }
    void shutdownInput() override { }
    void shutdownOutput() override { }
    void* getEventTarget() const override { return NULL; }
    bool isReady() const override { return m_readOffset < m_bytes.size(); }
    UInt32 getSize() const override
    {
        return static_cast<UInt32>(m_bytes.size() - m_readOffset);
    }
    UInt32 getBufferedOutputSize() const override { return 0; }

    const UInt8* code() const { return m_bytes.data(); }
    void consumeCode() { m_readOffset = 4; }

private:
    std::vector<UInt8> m_bytes;
    std::size_t m_readOffset = 0;
};

const std::string kBinding = "00112233445566778899aabbccddeeff";
const std::string kClipboardSession =
    "ffeeddccbbaa99887766554433221100";
const std::string kDigest =
    "sha256:dddddddddddddddddddddddddddddddd"
    "dddddddddddddddddddddddddddddddd";

barrier::FileTransferFrame roundTrip(
    const barrier::FileTransferFrame& input,
    barrier::FileTransferRole initiatorRole,
    const char* expectedCode)
{
    FileTransferProtocolTestStream stream;
    barrier::FileTransferValidationError error =
        barrier::FileTransferValidationError::kMalformedFrame;
    EXPECT_TRUE(barrier::FileTransferProtocol::encode(
        &stream, input, initiatorRole, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kNone, error);
    EXPECT_EQ(0, std::memcmp(expectedCode, stream.code(), 4));

    barrier::FileTransferFrame output;
    stream.consumeCode();
    EXPECT_TRUE(barrier::FileTransferProtocol::decode(
        stream.code(), &stream, initiatorRole, kBinding, output, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kNone, error);
    return output;
}

} // namespace

TEST(FileTransferProtocolTests, currentProtocolAdvertisesTransactionalTransfers)
{
    EXPECT_GE(kProtocolMinorVersion, 12);
}

TEST(FileTransferProtocolTests, transferIdEncodesRoleAndNonZeroSequence)
{
    const UInt32 primary = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 17);
    const UInt32 secondary = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 17);

    EXPECT_EQ(17u, primary);
    EXPECT_EQ(0x80000011u, secondary);
    EXPECT_EQ(17u, barrier::FileTransferProtocol::transferSequence(secondary));
    EXPECT_EQ(barrier::FileTransferRole::kSecondary,
              barrier::FileTransferProtocol::transferRole(secondary));
    EXPECT_EQ(0u, barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 0));
    EXPECT_EQ(0u, barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 0x80000000u));
}

TEST(FileTransferProtocolTests, allFrameTypesRoundTripThroughProtocolUtilFormat)
{
    using barrier::FileTransferFrame;
    using barrier::FileTransferFrameType;
    using barrier::FileTransferProtocol;
    using barrier::FileTransferReason;
    using barrier::FileTransferRole;

    const UInt32 transferId = FileTransferProtocol::makeTransferId(
        FileTransferRole::kPrimary, 9);

    FileTransferFrame start = FileTransferFrame::start(
        kBinding, transferId, FileTransferProtocol::kMaxTransferSize);
    FileTransferFrame decoded = roundTrip(start, FileTransferRole::kPrimary,
                                          kMsgDFileTransferStart1_12);
    EXPECT_EQ(FileTransferFrameType::kStart, decoded.type);
    EXPECT_EQ(kBinding, decoded.connectionBinding);
    EXPECT_EQ(transferId, decoded.transferId);
    EXPECT_EQ(FileTransferProtocol::kMaxTransferSize, decoded.totalSize);

    FileTransferFrame startAck = FileTransferFrame::startAck(
        kBinding, transferId, FileTransferReason::kNone);
    decoded = roundTrip(startAck, FileTransferRole::kPrimary,
                        kMsgDFileTransferStartAck1_12);
    EXPECT_EQ(FileTransferFrameType::kStartAck, decoded.type);
    EXPECT_EQ(FileTransferReason::kNone, decoded.reason);

    const std::string binaryPayload("a\0bc", 4);
    FileTransferFrame data = FileTransferFrame::data(
        kBinding, transferId, 11, binaryPayload);
    decoded = roundTrip(data, FileTransferRole::kPrimary,
                        kMsgDFileTransferData1_12);
    EXPECT_EQ(FileTransferFrameType::kData, decoded.type);
    EXPECT_EQ(11u, decoded.offset);
    EXPECT_EQ(binaryPayload, decoded.payload);

    FileTransferFrame end = FileTransferFrame::end(
        kBinding, transferId, 15, kDigest);
    decoded = roundTrip(end, FileTransferRole::kPrimary,
                        kMsgDFileTransferEnd1_12);
    EXPECT_EQ(FileTransferFrameType::kEnd, decoded.type);
    EXPECT_EQ(15u, decoded.offset);
    EXPECT_EQ(kDigest, decoded.payload);

    FileTransferFrame cancel = FileTransferFrame::cancel(
        kBinding, transferId, FileTransferReason::kCancelled);
    decoded = roundTrip(cancel, FileTransferRole::kPrimary,
                        kMsgDFileTransferCancel1_12);
    EXPECT_EQ(FileTransferFrameType::kCancel, decoded.type);
    EXPECT_EQ(FileTransferReason::kCancelled, decoded.reason);

    FileTransferFrame cancelAck = FileTransferFrame::cancelAck(
        kBinding, transferId, FileTransferReason::kNone);
    decoded = roundTrip(cancelAck, FileTransferRole::kPrimary,
                        kMsgDFileTransferCancelAck1_12);
    EXPECT_EQ(FileTransferFrameType::kCancelAck, decoded.type);

    FileTransferFrame commitAck = FileTransferFrame::commitAck(
        kBinding, transferId, FileTransferReason::kNone);
    decoded = roundTrip(commitAck, FileTransferRole::kPrimary,
                        kMsgDFileTransferCommitAck1_12);
    EXPECT_EQ(FileTransferFrameType::kCommitAck, decoded.type);
}

TEST(FileTransferProtocolTests, clipboardStartRoundTripsExplicitIdentity)
{
    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 19);
    const std::uint64_t revision = 0x12345678abcdef01ULL;
    const barrier::FileTransferFrame start = barrier::FileTransferFrame::start(
        kBinding, transferId, 4096,
        barrier::FileTransferKind::kClipboard, revision,
        kClipboardSession);

    const barrier::FileTransferFrame decoded = roundTrip(
        start, barrier::FileTransferRole::kSecondary,
        kMsgDFileTransferStart1_12);

    EXPECT_EQ(barrier::FileTransferKind::kClipboard, decoded.kind);
    EXPECT_EQ(revision, decoded.clipboardRevision);
    EXPECT_EQ(kClipboardSession, decoded.clipboardSessionId);
    EXPECT_EQ(4096u, decoded.totalSize);
}

TEST(FileTransferProtocolTests, startIdentityIsStrictlyBoundToTransferKind)
{
    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 21);
    barrier::FileTransferValidationError error;

    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::start(
            kBinding, transferId, 1,
            static_cast<barrier::FileTransferKind>(255), 0, std::string()),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kInvalidTransferKind,
              error);

    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::start(
            kBinding, transferId, 1,
            barrier::FileTransferKind::kClipboard, 0, kClipboardSession),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kInvalidClipboardRevision,
              error);

    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::start(
            kBinding, transferId, 1,
            barrier::FileTransferKind::kClipboard, 1, "not-a-session"),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kInvalidClipboardSession,
              error);

    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::start(
            kBinding, transferId, 1,
            barrier::FileTransferKind::kDrag, 1, kClipboardSession),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kMalformedFrame, error);

    EXPECT_TRUE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::start(
            kBinding, transferId, 1,
            barrier::FileTransferKind::kManual, 0, std::string()),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_TRUE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::start(
            kBinding, transferId, 1,
            barrier::FileTransferKind::kDrag, 0, std::string()),
        barrier::FileTransferRole::kPrimary, &error));
}

TEST(FileTransferProtocolTests, bindingMustBeExactly128BitsOfHex)
{
    barrier::FileTransferValidationError error;
    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 1);

    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::start("", transferId, 1),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kInvalidConnectionBinding,
              error);
    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::start(std::string(31, 'a'), transferId, 1),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::start(std::string(32, 'z'), transferId, 1),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::start(
            "A0112233445566778899aabbccddeefF", transferId, 1),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kInvalidConnectionBinding,
              error);

    FileTransferProtocolTestStream stream;
    const barrier::FileTransferFrame start = barrier::FileTransferFrame::start(
        kBinding, transferId, 1);
    ASSERT_TRUE(barrier::FileTransferProtocol::encode(
        &stream, start, barrier::FileTransferRole::kPrimary, &error));
    stream.consumeCode();
    barrier::FileTransferFrame output;
    EXPECT_FALSE(barrier::FileTransferProtocol::decode(
        stream.code(), &stream, barrier::FileTransferRole::kPrimary,
        "ffeeddccbbaa99887766554433221100", output, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kConnectionBindingMismatch,
              error);
}

TEST(FileTransferProtocolTests, transferIdRejectsZeroSequenceAndWrongRole)
{
    barrier::FileTransferValidationError error;

    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::start(kBinding, 0, 1),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kInvalidTransferSequence,
              error);

    const UInt32 secondaryId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kSecondary, 1);
    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::start(kBinding, secondaryId, 1),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kWrongTransferRole, error);
}

TEST(FileTransferProtocolTests, transferSizeAndDataRangeAreBounded)
{
    barrier::FileTransferValidationError error;
    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 1);

    EXPECT_TRUE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::start(
            kBinding, transferId,
            barrier::FileTransferProtocol::kMaxTransferSize),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::start(
            kBinding, transferId,
            barrier::FileTransferProtocol::kMaxTransferSize + 1u),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kTransferTooLarge, error);

    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::data(
            kBinding, transferId,
            barrier::FileTransferProtocol::kMaxTransferSize + 1u, "x"),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kInvalidOffset, error);
    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::data(
            kBinding, transferId,
            barrier::FileTransferProtocol::kMaxTransferSize, "x"),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kInvalidOffset, error);
}

TEST(FileTransferProtocolTests, endRequiresFinalOffsetAndCanonicalSha256Digest)
{
    barrier::FileTransferValidationError error;
    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 1);

    EXPECT_TRUE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::end(kBinding, transferId, 7, kDigest),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::end(
            kBinding, transferId, 7, std::string(70, 'd')),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kInvalidDigest, error);
    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::end(
            kBinding, transferId, 7,
            "sha256:DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD"
            "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD"),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kInvalidDigest, error);
    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        barrier::FileTransferFrame::end(
            kBinding, transferId,
            barrier::FileTransferProtocol::kMaxTransferSize + 1u, kDigest),
        barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kInvalidOffset, error);
}

TEST(FileTransferProtocolTests, responderAckRetainsInitiatorRoleInTransferId)
{
    const UInt32 primaryInitiatedId =
        barrier::FileTransferProtocol::makeTransferId(
            barrier::FileTransferRole::kPrimary, 23);
    const barrier::FileTransferFrame response =
        barrier::FileTransferFrame::startAck(
            kBinding, primaryInitiatedId,
            barrier::FileTransferReason::kNone);

    const barrier::FileTransferFrame decoded = roundTrip(
        response, barrier::FileTransferRole::kPrimary,
        kMsgDFileTransferStartAck1_12);
    EXPECT_EQ(primaryInitiatedId, decoded.transferId);
    EXPECT_EQ(barrier::FileTransferRole::kPrimary,
              barrier::FileTransferProtocol::transferRole(decoded.transferId));
}

TEST(FileTransferProtocolTests, unknownReasonAndMessageCodeAreRejected)
{
    barrier::FileTransferValidationError error;
    const UInt32 transferId = barrier::FileTransferProtocol::makeTransferId(
        barrier::FileTransferRole::kPrimary, 1);
    barrier::FileTransferFrame ack = barrier::FileTransferFrame::startAck(
        kBinding, transferId, static_cast<barrier::FileTransferReason>(255));

    EXPECT_FALSE(barrier::FileTransferProtocol::validate(
        ack, barrier::FileTransferRole::kPrimary, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kInvalidReason, error);

    FileTransferProtocolTestStream stream;
    barrier::FileTransferFrame output;
    const UInt8 unknownCode[4] = {'N', 'O', 'P', 'E'};
    EXPECT_FALSE(barrier::FileTransferProtocol::decode(
        unknownCode, &stream, barrier::FileTransferRole::kPrimary,
        kBinding, output, &error));
    EXPECT_EQ(barrier::FileTransferValidationError::kUnknownMessage, error);
}
