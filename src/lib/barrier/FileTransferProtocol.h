/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "common/basic_types.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace barrier {

class IStream;

// The high transfer-id bit identifies which side initiated the transfer and
// allocated its sequence. Response frames retain the initiator's role bit.
enum class FileTransferRole : UInt8 {
    kPrimary = 0,
    kSecondary = 1
};

// Values are wire-stable. A transfer's purpose is declared by Start rather
// than inferred from mutable clipboard or drag state at the receiver.
enum class FileTransferKind : UInt8 {
    kManual = 0,
    kDrag = 1,
    kClipboard = 2
};

// Values are wire-stable. Append new reasons; never reorder existing values.
enum class FileTransferReason : UInt8 {
    kNone = 0,
    kCancelled = 1,
    kRejected = 2,
    kBusy = 3,
    kSizeLimit = 4,
    kOffsetMismatch = 5,
    kDigestMismatch = 6,
    kIoError = 7,
    kProtocolError = 8,
    kTimeout = 9,
    kConnectionLost = 10
};

enum class FileTransferFrameType : UInt8 {
    kStart,
    kStartAck,
    kData,
    kEnd,
    kCancel,
    kCancelAck,
    kCommitAck
};

enum class FileTransferValidationError : UInt8 {
    kNone,
    kMalformedFrame,
    kUnknownMessage,
    kInvalidConnectionBinding,
    kConnectionBindingMismatch,
    kInvalidTransferSequence,
    kWrongTransferRole,
    kTransferTooLarge,
    kInvalidOffset,
    kInvalidPayload,
    kInvalidDigest,
    kInvalidReason,
    kInvalidTransferKind,
    kInvalidClipboardRevision,
    kInvalidClipboardSession
};

//! Typed value for one protocol 1.12 transactional file-transfer frame.
/*!
Only fields belonging to the selected type are populated: Start uses
totalSize and its transfer identity, Data uses offset and payload, End uses
offset for the final byte count and payload for the canonical SHA-256 digest,
and acknowledgment or cancellation frames use reason.
*/
struct FileTransferFrame {
    FileTransferFrameType type = FileTransferFrameType::kStart;
    std::string connectionBinding;
    UInt32 transferId = 0;
    UInt32 totalSize = 0;
    UInt32 offset = 0;
    std::string payload;
    FileTransferReason reason = FileTransferReason::kNone;
    FileTransferKind kind = FileTransferKind::kManual;
    std::uint64_t clipboardRevision = 0;
    std::string clipboardSessionId;

    static FileTransferFrame start(
        const std::string& binding, UInt32 transferId, UInt32 totalSize,
        FileTransferKind kind = FileTransferKind::kManual,
        std::uint64_t clipboardRevision = 0,
        const std::string& clipboardSessionId = std::string());
    static FileTransferFrame startAck(
        const std::string& binding, UInt32 transferId,
        FileTransferReason reason);
    static FileTransferFrame data(
        const std::string& binding, UInt32 transferId, UInt32 offset,
        const std::string& payload);
    static FileTransferFrame end(
        const std::string& binding, UInt32 transferId, UInt32 finalOffset,
        const std::string& digest);
    static FileTransferFrame cancel(
        const std::string& binding, UInt32 transferId,
        FileTransferReason reason);
    static FileTransferFrame cancelAck(
        const std::string& binding, UInt32 transferId,
        FileTransferReason reason);
    static FileTransferFrame commitAck(
        const std::string& binding, UInt32 transferId,
        FileTransferReason reason);
};

//! Strict protocol 1.12 file-transfer frame codec and structural validation.
class FileTransferProtocol {
public:
    static constexpr UInt32 kMaxTransferSize = 512u * 1024u * 1024u;
    static constexpr UInt32 kMaxDataSize = 1024u * 1024u;
    static constexpr std::size_t kConnectionBindingHexSize = 32u;
    static constexpr std::size_t kClipboardSessionHexSize = 32u;
    static constexpr std::size_t kEncodedSha256DigestSize = 71u;
    static constexpr UInt32 kTransferRoleMask = 0x80000000u;
    static constexpr UInt32 kTransferSequenceMask = 0x7fffffffu;

    static UInt32 makeTransferId(FileTransferRole role, UInt32 sequence);
    static UInt32 transferSequence(UInt32 transferId);
    static FileTransferRole transferRole(UInt32 transferId);

    static bool validate(
        const FileTransferFrame& frame,
        FileTransferRole expectedInitiatorRole,
        FileTransferValidationError* error = nullptr);

    //! Encode a complete message, including its four-byte message code.
    static bool encode(
        IStream* stream, const FileTransferFrame& frame,
        FileTransferRole initiatorRole,
        FileTransferValidationError* error = nullptr);

    //! Decode after the caller has consumed the four-byte message code.
    static bool decode(
        const UInt8 code[4], IStream* stream,
        FileTransferRole expectedInitiatorRole,
        const std::string& expectedConnectionBinding,
        FileTransferFrame& frame,
        FileTransferValidationError* error = nullptr);

    static const char* messageCode(FileTransferFrameType type);

private:
    static bool validateConnectionBinding(const std::string& binding);
    static bool validateReason(FileTransferReason reason);
};

} // namespace barrier
