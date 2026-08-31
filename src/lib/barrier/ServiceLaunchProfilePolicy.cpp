/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/ServiceLaunchProfilePolicy.h"

#include "server/Config.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

const ServiceLaunchFileRule kClientRules[] = {
    {"SSL/Barrier.pem", 128u * 1024u, true, false, false},
    {"SSL/Fingerprints/TrustedServers.txt", 64u * 1024u, false, true, true},
    {"SSL/Fingerprints/Local.txt", 64u * 1024u, false, true, true}
};

const ServiceLaunchFileRule kServerRules[] = {
    {"SSL/Barrier.pem", 128u * 1024u, true, false, false},
    {"SSL/Fingerprints/TrustedClients.txt", 64u * 1024u, false, true, true},
    {"SSL/Fingerprints/Local.txt", 64u * 1024u, false, true, true},
    {"barrier.sgc", 1024u * 1024u, true, false, false}
};

bool isHex(char value)
{
    return std::isxdigit(static_cast<unsigned char>(value)) != 0;
}

bool isLegacySha1(const std::string& line)
{
    if (line.size() != 59u) {
        return false;
    }
    for (std::size_t i = 0; i < line.size(); ++i) {
        if ((i + 1u) % 3u == 0u) {
            if (line[i] != ':') {
                return false;
            }
        }
        else if (!isHex(line[i])) {
            return false;
        }
    }
    return true;
}

bool isExactV2(const std::string& line)
{
    static const char sha1Prefix[] = "v2:sha1:";
    static const char sha256Prefix[] = "v2:sha256:";
    const char* prefix = nullptr;
    std::size_t prefixSize = 0u;
    std::size_t hexSize = 0u;

    if (line.compare(0, sizeof(sha1Prefix) - 1u, sha1Prefix) == 0) {
        prefix = sha1Prefix;
        prefixSize = sizeof(sha1Prefix) - 1u;
        hexSize = 40u;
    }
    else if (line.compare(0, sizeof(sha256Prefix) - 1u, sha256Prefix) == 0) {
        prefix = sha256Prefix;
        prefixSize = sizeof(sha256Prefix) - 1u;
        hexSize = 64u;
    }
    else {
        return false;
    }

    (void)prefix;
    return line.size() == prefixSize + hexSize &&
        std::all_of(line.begin() + prefixSize, line.end(), isHex);
}

} // namespace

const ServiceLaunchFileRule* serviceLaunchFileRules(
    ServiceLaunchRole role, std::size_t& count)
{
    if (role == ServiceLaunchRole::kServer) {
        count = sizeof(kServerRules) / sizeof(kServerRules[0]);
        return kServerRules;
    }
    count = sizeof(kClientRules) / sizeof(kClientRules[0]);
    return kClientRules;
}

const ServiceLaunchFileRule* findServiceLaunchFileRule(
    ServiceLaunchRole role, const std::string& relativePath)
{
    std::size_t count = 0u;
    const ServiceLaunchFileRule* rules = serviceLaunchFileRules(role, count);
    for (std::size_t i = 0; i < count; ++i) {
        if (relativePath == rules[i].relativePath) {
            return &rules[i];
        }
    }
    return nullptr;
}

bool isServiceLaunchFileSizeAllowed(const ServiceLaunchFileRule& rule,
                                    std::uint64_t size)
{
    return (size > 0u || rule.allowEmpty) && size <= rule.maximumBytes;
}

bool validateServiceFingerprintDatabase(const std::string& contents)
{
    std::istringstream input(contents);
    std::string line;
    bool found = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (!isLegacySha1(line) && !isExactV2(line)) {
            return false;
        }
        found = true;
    }
    return found;
}

bool validateServiceLaunchPem(const std::string& contents)
{
    if (contents.empty() || contents.size() > 128u * 1024u) {
        return false;
    }

    BIO* certificateBio = BIO_new_mem_buf(
        contents.data(), static_cast<int>(contents.size()));
    BIO* keyBio = BIO_new_mem_buf(
        contents.data(), static_cast<int>(contents.size()));
    if (certificateBio == nullptr || keyBio == nullptr) {
        BIO_free(certificateBio);
        BIO_free(keyBio);
        return false;
    }

    X509* certificate = PEM_read_bio_X509(
        certificateBio, nullptr, nullptr, nullptr);
    EVP_PKEY* privateKey = PEM_read_bio_PrivateKey(
        keyBio, nullptr, nullptr, nullptr);
    const bool valid = certificate != nullptr && privateKey != nullptr &&
        X509_check_private_key(certificate, privateKey) == 1;

    EVP_PKEY_free(privateKey);
    X509_free(certificate);
    BIO_free(keyBio);
    BIO_free(certificateBio);
    return valid;
}

bool validateServiceLaunchServerConfig(const std::string& contents)
{
    if (contents.empty() || contents.size() > 1024u * 1024u) {
        return false;
    }
    try {
        std::istringstream input(contents);
        Config config(nullptr);
        input >> config;
        return !input.bad() && config.begin() != config.end();
    }
    catch (...) {
        return false;
    }
}

const char* serviceLaunchRoleName(ServiceLaunchRole role)
{
    return role == ServiceLaunchRole::kServer ? "server" : "client";
}
