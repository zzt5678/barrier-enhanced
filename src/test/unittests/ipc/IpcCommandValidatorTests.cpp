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
    EXPECT_FALSE(IpcCommandValidator::isAllowedDaemonCommand(std::string("weaves\0--bad", 11), &reason));
}

TEST(IpcCommandValidatorTests, classifiesWeaveServerNameAsServer)
{
    EXPECT_TRUE(IpcCommandValidator::isServerCommand("weaves --config test.conf"));
    EXPECT_TRUE(IpcCommandValidator::isServerCommand("\"C:\\Program Files\\Weave\\weaves.exe\""));
    EXPECT_FALSE(IpcCommandValidator::isServerCommand("weavec 10.0.0.2"));
}
