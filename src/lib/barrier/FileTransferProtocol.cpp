/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/FileTransferProtocol.h"

#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "io/IStream.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace {

void setError(barrier::FileTransferValidationError* destination,
              barrier::FileTransferValidationError value)
{
    if (destination != nullptr) {
        *destination = value;
    }
}

bool hasNoUnusedPayloadFields(const barrier::FileTransferFrame& frame)
{
    return frame.totalSize == 0 && frame.offset == 0 && frame.payload.empty() &&
        frame.kind == barrier::FileTransferKind::kManual &&
        frame.clipboardRevision == 0 && frame.clipboardSessionId.empty();
}

bool isLowerHex(char value)
{
    return (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f');
}

bool isCanonicalSha256(const std::string& digest)
{
    static const char prefix[] = "sha256:";
    if (digest.size() !=
            barrier::FileTransferProtocol::kEncodedSha256DigestSize ||
        digest.compare(0, sizeof(prefix) - 1, prefix) != 0) {
        return false;
    }
    for (std::size_t i = sizeof(prefix) - 1; i < digest.size(); ++i) {
        if (!isLowerHex(digest[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace barrier {

FileTransferFrame
FileTransferFrame::start(
    const std::string& binding, UInt32 id, UInt32 size,
    FileTransferKind transferKind, std::uint64_t revision,
    const std::string& sessionId)
{
    FileTransferFrame frame;
    frame.type = FileTransferFrameType::kStart;
    frame.connectionBinding = binding;
    frame.transferId = id;
    frame.totalSize = size;
    frame.kind = transferKind;
    frame.clipboardRevision = revision;
    frame.clipboardSessionId = sessionId;
    return frame;
}

FileTransferFrame
FileTransferFrame::startAck(
    const std::string& binding, UInt32 id, FileTransferReason result)
{
    FileTransferFrame frame;
    frame.type = FileTransferFrameType::kStartAck;
    frame.connectionBinding = binding;
    frame.transferId = id;
    frame.reason = result;
    return frame;
}

FileTransferFrame
FileTransferFrame::data(
    const std::string& binding, UInt32 id, UInt32 dataOffset,
    const std::string& content)
{
    FileTransferFrame frame;
    frame.type = FileTransferFrameType::kData;
    frame.connectionBinding = binding;
    frame.transferId = id;
    frame.offset = dataOffset;
    frame.payload = content;
    return frame;
}

FileTransferFrame
FileTransferFrame::end(
    const std::string& binding, UInt32 id, UInt32 finalOffset,
    const std::string& digest)
{
    FileTransferFrame frame;
    frame.type = FileTransferFrameType::kEnd;
    frame.connectionBinding = binding;
    frame.transferId = id;
    frame.offset = finalOffset;
    frame.payload = digest;
    return frame;
}

FileTransferFrame
FileTransferFrame::cancel(
    const std::string& binding, UInt32 id, FileTransferReason cancelReason)
{
    FileTransferFrame frame;
    frame.type = FileTransferFrameType::kCancel;
    frame.connectionBinding = binding;
    frame.transferId = id;
    frame.reason = cancelReason;
    return frame;
}

FileTransferFrame
FileTransferFrame::cancelAck(
    const std::string& binding, UInt32 id, FileTransferReason result)
{
    FileTransferFrame frame;
    frame.type = FileTransferFrameType::kCancelAck;
    frame.connectionBinding = binding;
    frame.transferId = id;
    frame.reason = result;
    return frame;
}

FileTransferFrame
FileTransferFrame::commitAck(
    const std::string& binding, UInt32 id, FileTransferReason result)
{
    FileTransferFrame frame;
    frame.type = FileTransferFrameType::kCommitAck;
    frame.connectionBinding = binding;
    frame.transferId = id;
    frame.reason = result;
    return frame;
}

UInt32
FileTransferProtocol::makeTransferId(FileTransferRole role, UInt32 sequence)
{
    if ((role != FileTransferRole::kPrimary &&
         role != FileTransferRole::kSecondary) ||
        sequence == 0 || (sequence & ~kTransferSequenceMask) != 0) {
        return 0;
    }

    return sequence |
        (role == FileTransferRole::kSecondary ? kTransferRoleMask : 0u);
}

UInt32
FileTransferProtocol::transferSequence(UInt32 transferId)
{
    return transferId & kTransferSequenceMask;
}

FileTransferRole
FileTransferProtocol::transferRole(UInt32 transferId)
{
    return (transferId & kTransferRoleMask) == 0 ?
        FileTransferRole::kPrimary : FileTransferRole::kSecondary;
}

bool
FileTransferProtocol::validateConnectionBinding(const std::string& binding)
{
    if (binding.size() != kConnectionBindingHexSize) {
        return false;
    }

    for (char value : binding) {
        if (!isLowerHex(value)) {
            return false;
        }
    }
    return true;
}

bool
FileTransferProtocol::validateReason(FileTransferReason reason)
{
    return static_cast<UInt32>(reason) <=
        static_cast<UInt32>(FileTransferReason::kConnectionLost);
}

bool
FileTransferProtocol::validate(
    const FileTransferFrame& frame, FileTransferRole expectedInitiatorRole,
    FileTransferValidationError* error)
{
    setError(error, FileTransferValidationError::kNone);
    if (!validateConnectionBinding(frame.connectionBinding)) {
        setError(error, FileTransferValidationError::kInvalidConnectionBinding);
        return false;
    }

    if (transferSequence(frame.transferId) == 0) {
        setError(error, FileTransferValidationError::kInvalidTransferSequence);
        return false;
    }
    if ((expectedInitiatorRole != FileTransferRole::kPrimary &&
         expectedInitiatorRole != FileTransferRole::kSecondary) ||
        transferRole(frame.transferId) != expectedInitiatorRole) {
        setError(error, FileTransferValidationError::kWrongTransferRole);
        return false;
    }

    switch (frame.type) {
    case FileTransferFrameType::kStart:
        if (frame.kind != FileTransferKind::kManual &&
            frame.kind != FileTransferKind::kDrag &&
            frame.kind != FileTransferKind::kClipboard) {
            setError(error, FileTransferValidationError::kInvalidTransferKind);
            return false;
        }
        if (frame.totalSize > kMaxTransferSize) {
            setError(error, FileTransferValidationError::kTransferTooLarge);
            return false;
        }
        if (frame.offset != 0 || !frame.payload.empty() ||
            frame.reason != FileTransferReason::kNone) {
            setError(error, FileTransferValidationError::kMalformedFrame);
            return false;
        }
        if (frame.kind == FileTransferKind::kClipboard) {
            if (frame.clipboardRevision == 0) {
                setError(
                    error,
                    FileTransferValidationError::kInvalidClipboardRevision);
                return false;
            }
            if (frame.clipboardSessionId.size() != kClipboardSessionHexSize ||
                !std::all_of(
                    frame.clipboardSessionId.begin(),
                    frame.clipboardSessionId.end(), isLowerHex)) {
                setError(
                    error,
                    FileTransferValidationError::kInvalidClipboardSession);
                return false;
            }
        }
        else if (frame.clipboardRevision != 0 ||
                 !frame.clipboardSessionId.empty()) {
            setError(error, FileTransferValidationError::kMalformedFrame);
            return false;
        }
        break;

    case FileTransferFrameType::kStartAck:
    case FileTransferFrameType::kCancelAck:
    case FileTransferFrameType::kCommitAck:
        if (!validateReason(frame.reason)) {
            setError(error, FileTransferValidationError::kInvalidReason);
            return false;
        }
        if (!hasNoUnusedPayloadFields(frame)) {
            setError(error, FileTransferValidationError::kMalformedFrame);
            return false;
        }
        break;

    case FileTransferFrameType::kData: {
        if (frame.totalSize != 0 ||
            frame.reason != FileTransferReason::kNone) {
            setError(error, FileTransferValidationError::kMalformedFrame);
            return false;
        }
        if (frame.payload.empty() || frame.payload.size() > kMaxDataSize) {
            setError(error, FileTransferValidationError::kInvalidPayload);
            return false;
        }
        const UInt32 payloadSize = static_cast<UInt32>(frame.payload.size());
        if (frame.offset > kMaxTransferSize ||
            payloadSize > kMaxTransferSize - frame.offset) {
            setError(error, FileTransferValidationError::kInvalidOffset);
            return false;
        }
        break;
    }

    case FileTransferFrameType::kEnd:
        if (frame.totalSize != 0 ||
            frame.reason != FileTransferReason::kNone) {
            setError(error, FileTransferValidationError::kMalformedFrame);
            return false;
        }
        if (frame.offset > kMaxTransferSize) {
            setError(error, FileTransferValidationError::kInvalidOffset);
            return false;
        }
        if (!isCanonicalSha256(frame.payload)) {
            setError(error, FileTransferValidationError::kInvalidDigest);
            return false;
        }
        break;

    case FileTransferFrameType::kCancel:
        if (!validateReason(frame.reason) ||
            frame.reason == FileTransferReason::kNone) {
            setError(error, FileTransferValidationError::kInvalidReason);
            return false;
        }
        if (!hasNoUnusedPayloadFields(frame)) {
            setError(error, FileTransferValidationError::kMalformedFrame);
            return false;
        }
        break;

    default:
        setError(error, FileTransferValidationError::kUnknownMessage);
        return false;
    }

    return true;
}

const char*
FileTransferProtocol::messageCode(FileTransferFrameType type)
{
    switch (type) {
    case FileTransferFrameType::kStart:
        return kMsgDFileTransferStart1_12;
    case FileTransferFrameType::kStartAck:
        return kMsgDFileTransferStartAck1_12;
    case FileTransferFrameType::kData:
        return kMsgDFileTransferData1_12;
    case FileTransferFrameType::kEnd:
        return kMsgDFileTransferEnd1_12;
    case FileTransferFrameType::kCancel:
        return kMsgDFileTransferCancel1_12;
    case FileTransferFrameType::kCancelAck:
        return kMsgDFileTransferCancelAck1_12;
    case FileTransferFrameType::kCommitAck:
        return kMsgDFileTransferCommitAck1_12;
    default:
        return nullptr;
    }
}

bool
FileTransferProtocol::encode(
    IStream* stream, const FileTransferFrame& frame,
    FileTransferRole initiatorRole, FileTransferValidationError* error)
{
    if (stream == nullptr || !validate(frame, initiatorRole, error)) {
        if (stream == nullptr) {
            setError(error, FileTransferValidationError::kMalformedFrame);
        }
        return false;
    }

    std::string binding = frame.connectionBinding;
    std::string payload = frame.payload;
    std::string clipboardSessionId = frame.clipboardSessionId;
    switch (frame.type) {
    case FileTransferFrameType::kStart:
        {
        const UInt32 revisionHigh = static_cast<UInt32>(
            frame.clipboardRevision >> 32);
        const UInt32 revisionLow = static_cast<UInt32>(
            frame.clipboardRevision & 0xffffffffu);
        ProtocolUtil::writef(stream, kMsgDFileTransferStart1_12,
                             &binding, frame.transferId,
                             static_cast<UInt32>(frame.kind),
                             revisionHigh, revisionLow,
                             &clipboardSessionId, frame.totalSize);
        break;
        }
    case FileTransferFrameType::kStartAck:
        ProtocolUtil::writef(stream, kMsgDFileTransferStartAck1_12,
                             &binding, frame.transferId,
                             static_cast<UInt32>(frame.reason));
        break;
    case FileTransferFrameType::kData:
        ProtocolUtil::writef(stream, kMsgDFileTransferData1_12,
                             &binding, frame.transferId, frame.offset,
                             &payload);
        break;
    case FileTransferFrameType::kEnd:
        ProtocolUtil::writef(stream, kMsgDFileTransferEnd1_12,
                             &binding, frame.transferId, frame.offset,
                             &payload);
        break;
    case FileTransferFrameType::kCancel:
        ProtocolUtil::writef(stream, kMsgDFileTransferCancel1_12,
                             &binding, frame.transferId,
                             static_cast<UInt32>(frame.reason));
        break;
    case FileTransferFrameType::kCancelAck:
        ProtocolUtil::writef(stream, kMsgDFileTransferCancelAck1_12,
                             &binding, frame.transferId,
                             static_cast<UInt32>(frame.reason));
        break;
    case FileTransferFrameType::kCommitAck:
        ProtocolUtil::writef(stream, kMsgDFileTransferCommitAck1_12,
                             &binding, frame.transferId,
                             static_cast<UInt32>(frame.reason));
        break;
    default:
        setError(error, FileTransferValidationError::kUnknownMessage);
        return false;
    }

    setError(error, FileTransferValidationError::kNone);
    return true;
}

bool
FileTransferProtocol::decode(
    const UInt8 code[4], IStream* stream,
    FileTransferRole expectedInitiatorRole,
    const std::string& expectedConnectionBinding,
    FileTransferFrame& frame, FileTransferValidationError* error)
{
    setError(error, FileTransferValidationError::kNone);
    if (code == nullptr || stream == nullptr) {
        setError(error, FileTransferValidationError::kMalformedFrame);
        return false;
    }
    if (!validateConnectionBinding(expectedConnectionBinding)) {
        setError(error, FileTransferValidationError::kInvalidConnectionBinding);
        return false;
    }

    FileTransferFrame decoded;
    UInt8 reason = 0;
    UInt8 kind = 0;
    UInt32 revisionHigh = 0;
    UInt32 revisionLow = 0;
    bool parsed = false;
    if (std::memcmp(code, kMsgDFileTransferStart1_12, 4) == 0) {
        decoded.type = FileTransferFrameType::kStart;
        parsed = ProtocolUtil::readf(
            stream, kMsgDFileTransferStart1_12 + 4,
            &decoded.connectionBinding, &decoded.transferId,
            &kind, &revisionHigh, &revisionLow,
            &decoded.clipboardSessionId, &decoded.totalSize);
        decoded.kind = static_cast<FileTransferKind>(kind);
        decoded.clipboardRevision =
            (static_cast<std::uint64_t>(revisionHigh) << 32) |
            static_cast<std::uint64_t>(revisionLow);
    }
    else if (std::memcmp(code, kMsgDFileTransferStartAck1_12, 4) == 0) {
        decoded.type = FileTransferFrameType::kStartAck;
        parsed = ProtocolUtil::readf(
            stream, kMsgDFileTransferStartAck1_12 + 4,
            &decoded.connectionBinding, &decoded.transferId, &reason);
    }
    else if (std::memcmp(code, kMsgDFileTransferData1_12, 4) == 0) {
        decoded.type = FileTransferFrameType::kData;
        parsed = ProtocolUtil::readf(
            stream, kMsgDFileTransferData1_12 + 4,
            &decoded.connectionBinding, &decoded.transferId,
            &decoded.offset, &decoded.payload);
    }
    else if (std::memcmp(code, kMsgDFileTransferEnd1_12, 4) == 0) {
        decoded.type = FileTransferFrameType::kEnd;
        parsed = ProtocolUtil::readf(
            stream, kMsgDFileTransferEnd1_12 + 4,
            &decoded.connectionBinding, &decoded.transferId,
            &decoded.offset, &decoded.payload);
    }
    else if (std::memcmp(code, kMsgDFileTransferCancel1_12, 4) == 0) {
        decoded.type = FileTransferFrameType::kCancel;
        parsed = ProtocolUtil::readf(
            stream, kMsgDFileTransferCancel1_12 + 4,
            &decoded.connectionBinding, &decoded.transferId, &reason);
    }
    else if (std::memcmp(code, kMsgDFileTransferCancelAck1_12, 4) == 0) {
        decoded.type = FileTransferFrameType::kCancelAck;
        parsed = ProtocolUtil::readf(
            stream, kMsgDFileTransferCancelAck1_12 + 4,
            &decoded.connectionBinding, &decoded.transferId, &reason);
    }
    else if (std::memcmp(code, kMsgDFileTransferCommitAck1_12, 4) == 0) {
        decoded.type = FileTransferFrameType::kCommitAck;
        parsed = ProtocolUtil::readf(
            stream, kMsgDFileTransferCommitAck1_12 + 4,
            &decoded.connectionBinding, &decoded.transferId, &reason);
    }
    else {
        setError(error, FileTransferValidationError::kUnknownMessage);
        return false;
    }

    if (!parsed) {
        setError(error, FileTransferValidationError::kMalformedFrame);
        return false;
    }

    decoded.reason = static_cast<FileTransferReason>(reason);
    if (!validate(decoded, expectedInitiatorRole, error)) {
        return false;
    }
    if (decoded.connectionBinding != expectedConnectionBinding) {
        setError(error,
                 FileTransferValidationError::kConnectionBindingMismatch);
        return false;
    }

    frame = std::move(decoded);
    setError(error, FileTransferValidationError::kNone);
    return true;
}

} // namespace barrier
