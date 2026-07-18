/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/ServiceLaunchProfilePolicy.h"

#include "io/filesystem.h"
#include "net/SecureUtils.h"
#include "test/global/gtest.h"

#include <fstream>
#include <string>

namespace {

const ServiceLaunchFileRule* rule(ServiceLaunchRole role,
                                  const std::string& relativePath)
{
    return findServiceLaunchFileRule(role, relativePath);
}

std::string repeatedHex(std::size_t bytes)
{
    return std::string(bytes * 2, 'a');
}

std::string createPem(const char* stem)
{
    const barrier::fs::path path = barrier::fs::temp_directory_path() /
        (std::string(stem) + "-service-launch-profile.pem");
    std::error_code ignored;
    barrier::fs::remove(path, ignored);
    barrier::generate_pem_self_signed_cert(path.u8string());
    std::ifstream input(path, std::ios::binary);
    const std::string result((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    input.close();
    barrier::fs::remove(path, ignored);
    return result;
}

} // namespace

TEST(ServiceLaunchProfilePolicyTests, clientCopiesOnlyItsRequiredTlsState)
{
    const ServiceLaunchFileRule* certificate =
        rule(ServiceLaunchRole::kClient, "SSL/Barrier.pem");
    ASSERT_NE(nullptr, certificate);
    EXPECT_TRUE(certificate->required);
    EXPECT_EQ(128u * 1024u, certificate->maximumBytes);

    const ServiceLaunchFileRule* trust =
        rule(ServiceLaunchRole::kClient,
             "SSL/Fingerprints/TrustedServers.txt");
    ASSERT_NE(nullptr, trust);
    EXPECT_FALSE(trust->required);
    EXPECT_TRUE(trust->allowEmpty);
    EXPECT_TRUE(trust->fingerprintDatabase);
    EXPECT_EQ(64u * 1024u, trust->maximumBytes);

    const ServiceLaunchFileRule* local =
        rule(ServiceLaunchRole::kClient, "SSL/Fingerprints/Local.txt");
    ASSERT_NE(nullptr, local);
    EXPECT_FALSE(local->required);

    EXPECT_EQ(nullptr,
              rule(ServiceLaunchRole::kClient,
                   "SSL/Fingerprints/TrustedClients.txt"));
    EXPECT_EQ(nullptr, rule(ServiceLaunchRole::kClient, "barrier.sgc"));
    EXPECT_EQ(nullptr, rule(ServiceLaunchRole::kClient, "../Barrier.pem"));
}

TEST(ServiceLaunchProfilePolicyTests, serverRequiresClientTrustAndConfiguration)
{
    const ServiceLaunchFileRule* certificate =
        rule(ServiceLaunchRole::kServer, "SSL/Barrier.pem");
    const ServiceLaunchFileRule* trust =
        rule(ServiceLaunchRole::kServer,
             "SSL/Fingerprints/TrustedClients.txt");
    const ServiceLaunchFileRule* config =
        rule(ServiceLaunchRole::kServer, "barrier.sgc");

    ASSERT_NE(nullptr, certificate);
    ASSERT_NE(nullptr, trust);
    ASSERT_NE(nullptr, config);
    EXPECT_TRUE(certificate->required);
    EXPECT_FALSE(trust->required);
    EXPECT_TRUE(trust->allowEmpty);
    EXPECT_TRUE(config->required);
    EXPECT_EQ(1024u * 1024u, config->maximumBytes);
    EXPECT_EQ(nullptr,
              rule(ServiceLaunchRole::kServer,
                   "SSL/Fingerprints/TrustedServers.txt"));
}

TEST(ServiceLaunchProfilePolicyTests, emptyFirstRunTrustDatabaseIsAllowed)
{
    const ServiceLaunchFileRule* clientTrust =
        rule(ServiceLaunchRole::kClient,
             "SSL/Fingerprints/TrustedServers.txt");
    const ServiceLaunchFileRule* serverTrust =
        rule(ServiceLaunchRole::kServer,
             "SSL/Fingerprints/TrustedClients.txt");
    ASSERT_NE(nullptr, clientTrust);
    ASSERT_NE(nullptr, serverTrust);
    EXPECT_TRUE(isServiceLaunchFileSizeAllowed(*clientTrust, 0u));
    EXPECT_TRUE(isServiceLaunchFileSizeAllowed(*serverTrust, 0u));
    EXPECT_FALSE(rule(ServiceLaunchRole::kClient,
                      "SSL/Barrier.pem")->allowEmpty);
    EXPECT_FALSE(rule(ServiceLaunchRole::kServer,
                      "barrier.sgc")->allowEmpty);
}

TEST(ServiceLaunchProfilePolicyTests, sizeLimitIncludesBoundaryOnly)
{
    const ServiceLaunchFileRule* certificate =
        rule(ServiceLaunchRole::kClient, "SSL/Barrier.pem");
    ASSERT_NE(nullptr, certificate);
    EXPECT_TRUE(isServiceLaunchFileSizeAllowed(*certificate, 1u));
    EXPECT_TRUE(isServiceLaunchFileSizeAllowed(
        *certificate, certificate->maximumBytes));
    EXPECT_FALSE(isServiceLaunchFileSizeAllowed(*certificate, 0u));
    EXPECT_FALSE(isServiceLaunchFileSizeAllowed(
        *certificate, certificate->maximumBytes + 1u));
}

TEST(ServiceLaunchProfilePolicyTests, fingerprintDatabaseAcceptsExactKnownFormats)
{
    const std::string legacy =
        "AB:CD:EF:00:01:02:03:04:05:06:07:08:09:10:11:12:13:14:15:16";
    EXPECT_TRUE(validateServiceFingerprintDatabase(legacy + "\r\n"));
    EXPECT_TRUE(validateServiceFingerprintDatabase(
        "v2:sha1:" + repeatedHex(20) + "\n"));
    EXPECT_TRUE(validateServiceFingerprintDatabase(
        "v2:sha256:" + repeatedHex(32) + "\n\n"));
    EXPECT_TRUE(validateServiceFingerprintDatabase(
        legacy + "\n" + "v2:sha256:" + repeatedHex(32) + "\n"));
}

TEST(ServiceLaunchProfilePolicyTests, fingerprintDatabaseRejectsAmbiguousInput)
{
    EXPECT_FALSE(validateServiceFingerprintDatabase(""));
    EXPECT_FALSE(validateServiceFingerprintDatabase("\n\r\n"));
    EXPECT_FALSE(validateServiceFingerprintDatabase(
        "v2:md5:" + repeatedHex(16) + "\n"));
    EXPECT_FALSE(validateServiceFingerprintDatabase(
        "v2:sha1:" + repeatedHex(19) + "\n"));
    EXPECT_FALSE(validateServiceFingerprintDatabase(
        "v2:sha256:" + repeatedHex(33) + "\n"));
    EXPECT_FALSE(validateServiceFingerprintDatabase(
        " v2:sha256:" + repeatedHex(32) + "\n"));
    EXPECT_FALSE(validateServiceFingerprintDatabase(
        "v2:sha256:" + repeatedHex(31) + "zz\n"));
    EXPECT_FALSE(validateServiceFingerprintDatabase(
        "AB:CD:EF:00:01:02:03:04:05:06:07:08:09:10:11:12:13:14:15\n"));
}

TEST(ServiceLaunchProfilePolicyTests, pemRequiresMatchingCertificateAndPrivateKey)
{
    const std::string first = createPem("weave-first");
    const std::string second = createPem("weave-second");
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    EXPECT_TRUE(validateServiceLaunchPem(first));

    const std::string marker = "-----BEGIN CERTIFICATE-----";
    const std::size_t firstCertificate = first.find(marker);
    const std::size_t secondCertificate = second.find(marker);
    ASSERT_NE(std::string::npos, firstCertificate);
    ASSERT_NE(std::string::npos, secondCertificate);
    EXPECT_FALSE(validateServiceLaunchPem(
        first.substr(0u, firstCertificate) + second.substr(secondCertificate)));
    EXPECT_FALSE(validateServiceLaunchPem("not a pem"));
}

TEST(ServiceLaunchProfilePolicyTests, serverConfigMustParseCompletely)
{
    const std::string valid =
        "section: screens\n"
        "\tubuntu:\n"
        "\twindows:\n"
        "end\n"
        "section: links\n"
        "\tubuntu:\n"
        "\t\tright = windows\n"
        "\twindows:\n"
        "\t\tleft = ubuntu\n"
        "end\n";
    EXPECT_TRUE(validateServiceLaunchServerConfig(valid));
    EXPECT_FALSE(validateServiceLaunchServerConfig(""));
    EXPECT_FALSE(validateServiceLaunchServerConfig(
        "section: screens\nend\n"));
    EXPECT_FALSE(validateServiceLaunchServerConfig(
        "section: screens\n\tubuntu:\nend\nunknown data\n"));
}
