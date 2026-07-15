/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ipc/IpcCommandValidator.h"

#include "test/global/gtest.h"

TEST(IpcCommandValidatorTests, allowsKnownDaemonCommands)
{
    EXPECT_EQ(IpcCommandValidator::CommandRole::kEmpty,
              IpcCommandValidator::classifyDaemonCommand(""));
    EXPECT_EQ(IpcCommandValidator::CommandRole::kEmpty,
              IpcCommandValidator::classifyDaemonCommand("\"\""));
    EXPECT_EQ(IpcCommandValidator::CommandRole::kServer,
              IpcCommandValidator::classifyDaemonCommand("/opt/weave/weaves --no-tray"));
    EXPECT_EQ(IpcCommandValidator::CommandRole::kServer,
              IpcCommandValidator::classifyDaemonCommand("\"C:\\Program Files\\Weave\\weaves.exe\" --debug INFO"));
    EXPECT_EQ(IpcCommandValidator::CommandRole::kServer,
              IpcCommandValidator::classifyDaemonCommand("barriers -f"));
    EXPECT_EQ(IpcCommandValidator::CommandRole::kClient,
              IpcCommandValidator::classifyDaemonCommand("weavec 10.0.0.2"));
    EXPECT_EQ(IpcCommandValidator::CommandRole::kClient,
              IpcCommandValidator::classifyDaemonCommand("\"C:\\Program Files\\Barrier\\barrierc.exe\" server"));
}

TEST(IpcCommandValidatorTests, rejectsUnknownOrMalformedCommands)
{
    std::string reason;
    EXPECT_FALSE(IpcCommandValidator::isAllowedDaemonCommand("cmd.exe /c calc", &reason));
    EXPECT_FALSE(reason.empty());

    EXPECT_FALSE(IpcCommandValidator::isAllowedDaemonCommand("/tmp/weaves-malicious", &reason));
    EXPECT_FALSE(IpcCommandValidator::isAllowedDaemonCommand("\"unterminated", &reason));
    EXPECT_FALSE(IpcCommandValidator::isAllowedDaemonCommand("\"weavec.exe\"unexpected", &reason));
    EXPECT_FALSE(IpcCommandValidator::isAllowedDaemonCommand(std::string("weaves\0--bad", 11), &reason));
}

TEST(IpcCommandValidatorTests, classifiesWeaveServerNameAsServer)
{
    EXPECT_TRUE(IpcCommandValidator::isServerCommand("weaves --config test.conf"));
    EXPECT_TRUE(IpcCommandValidator::isServerCommand("\"C:\\Program Files\\Weave\\weaves.exe\""));
    EXPECT_FALSE(IpcCommandValidator::isServerCommand("weavec 10.0.0.2"));
}

TEST(IpcCommandValidatorTests, rewritesClientExecutableToTrustedSibling)
{
    std::string rewritten;
    std::string reason;
    EXPECT_TRUE(IpcCommandValidator::rewriteDaemonExecutable(
        "\"C:\\Users\\user\\Downloads\\weavec.exe\" --name desk 10.0.0.2",
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        rewritten,
        &reason));
    EXPECT_EQ("\"C:\\Program Files\\Weave\\weavec.exe\" --name desk 10.0.0.2",
              rewritten);
    EXPECT_TRUE(reason.empty());
}

TEST(IpcCommandValidatorTests, rewritesLegacyServerNameToTrustedWeaveBinary)
{
    std::string rewritten;
    EXPECT_TRUE(IpcCommandValidator::rewriteDaemonExecutable(
        "barriers.exe -f --debug INFO",
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        rewritten));
    EXPECT_EQ("\"C:\\Program Files\\Weave\\weaves.exe\" -f --debug INFO",
              rewritten);
}

TEST(IpcCommandValidatorTests, rewriteRejectsInvalidOrUnsafeTrustedExecutable)
{
    std::string rewritten;
    std::string reason;
    EXPECT_FALSE(IpcCommandValidator::rewriteDaemonExecutable(
        "cmd.exe /c calc",
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        rewritten,
        &reason));
    EXPECT_FALSE(reason.empty());

    reason.clear();
    EXPECT_FALSE(IpcCommandValidator::rewriteDaemonExecutable(
        "weavec 10.0.0.2",
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Bad\"Name\\weavec.exe",
        rewritten,
        &reason));
    EXPECT_FALSE(reason.empty());
}

TEST(IpcCommandValidatorTests, rewritePreservesEmptyStopCommand)
{
    std::string rewritten("stale");
    EXPECT_TRUE(IpcCommandValidator::rewriteDaemonExecutable(
        "\"\"",
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        rewritten));
    EXPECT_TRUE(rewritten.empty());
}
