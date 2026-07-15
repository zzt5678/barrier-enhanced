/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/TransferDigest.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <array>

namespace {

const char kDigestPrefix[] = "sha256:";
const std::size_t kSha256Size = 32;
const char kHexDigits[] = "0123456789abcdef";

int hexValue(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

bool decodeDigest(const std::string& encoded,
                  std::array<unsigned char, kSha256Size>& digest)
{
    const std::size_t prefixSize = sizeof(kDigestPrefix) - 1;
    if (encoded.size() != prefixSize + kSha256Size * 2 ||
        encoded.compare(0, prefixSize, kDigestPrefix) != 0) {
        return false;
    }

    for (std::size_t i = 0; i < digest.size(); ++i) {
        const int high = hexValue(encoded[prefixSize + i * 2]);
        const int low = hexValue(encoded[prefixSize + i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        digest[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

} // namespace

namespace barrier {

struct TransferDigest::Impl {
    Impl() : context(EVP_MD_CTX_new()), active(false)
    {
        active = context != NULL && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
    }

    ~Impl()
    {
        EVP_MD_CTX_free(context);
    }

    EVP_MD_CTX* context;
    bool active;
};

TransferDigest::TransferDigest() :
    m_impl(new Impl())
{
}

TransferDigest::~TransferDigest()
{
}

bool
TransferDigest::isReady() const
{
    return m_impl && m_impl->active;
}

bool
TransferDigest::update(const void* data, std::size_t size)
{
    if (!isReady() || (size != 0 && data == NULL)) {
        return false;
    }
    return size == 0 || EVP_DigestUpdate(m_impl->context, data, size) == 1;
}

bool
TransferDigest::finish(std::string& encodedDigest)
{
    encodedDigest.clear();
    if (!isReady()) {
        return false;
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestSize = 0;
    const bool ok = EVP_DigestFinal_ex(m_impl->context, digest.data(), &digestSize) == 1 &&
        digestSize == kSha256Size;
    m_impl->active = false;
    if (!ok) {
        return false;
    }

    encodedDigest.assign(kDigestPrefix, sizeof(kDigestPrefix) - 1);
    encodedDigest.reserve(encodedDigest.size() + digestSize * 2);
    for (unsigned int i = 0; i < digestSize; ++i) {
        encodedDigest.push_back(kHexDigits[digest[i] >> 4]);
        encodedDigest.push_back(kHexDigits[digest[i] & 0x0f]);
    }
    return true;
}

bool
TransferDigest::verify(const std::string& encodedDigest)
{
    std::array<unsigned char, kSha256Size> expected{};
    if (!decodeDigest(encodedDigest, expected) || !isReady()) {
        return false;
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> actual{};
    unsigned int actualSize = 0;
    const bool finalized =
        EVP_DigestFinal_ex(m_impl->context, actual.data(), &actualSize) == 1;
    m_impl->active = false;
    return finalized && actualSize == expected.size() &&
        CRYPTO_memcmp(actual.data(), expected.data(), expected.size()) == 0;
}

} // namespace barrier
