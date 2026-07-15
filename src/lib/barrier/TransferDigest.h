/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace barrier {

class TransferDigest {
public:
    TransferDigest();
    ~TransferDigest();

    bool isReady() const;
    bool update(const void* data, std::size_t size);
    bool finish(std::string& encodedDigest);
    bool verify(const std::string& encodedDigest);

private:
    TransferDigest(const TransferDigest&);
    TransferDigest& operator=(const TransferDigest&);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace barrier
