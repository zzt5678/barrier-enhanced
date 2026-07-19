/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ipc/IpcCommandValidator.h"
#include "ipc/IpcMessage.h"

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

TEST(IpcCommandValidatorTests, sanitizesClientPathsAndDowngradesAlwaysElevation)
{
    IpcCommandValidator::SanitizedDaemonRequest sanitized;
    std::string reason;
    ASSERT_TRUE(IpcCommandValidator::sanitizeDaemonRequest(
        "\"C:\\Users\\user\\Downloads\\weavec.exe\" -f --no-tray "
        "--debug INFO --name windows --ipc --enable-drag-drop "
        "--drop-dir \"C:\\Users\\user\\Inbox\" "
        "--profile-dir \"C:\\Users\\user\\Profile\" "
        "--plugin-dir \"C:\\Users\\user\\Plugins\" "
        "--log \"C:\\Windows\\System32\\drivers\\etc\\hosts\" "
        "server.example:24800",
        IpcCommandMessage::kElevateAlways,
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        sanitized,
        &reason)) << reason;

    EXPECT_EQ(IpcCommandValidator::CommandRole::kClient, sanitized.role);
    EXPECT_EQ(IpcCommandMessage::kElevateAsNeeded, sanitized.elevateMode);
    EXPECT_TRUE(sanitized.elevationDowngraded);
    EXPECT_TRUE(sanitized.ignoredUnsafeArguments);
    EXPECT_EQ(
        "\"C:\\Program Files\\Weave\\weavec.exe\" --no-daemon --no-tray "
        "--debug INFO --name windows --ipc --enable-drag-drop "
        "--stop-on-desk-switch server.example:24800",
        sanitized.command);
    EXPECT_EQ(std::string::npos, sanitized.command.find("C:\\Users"));
    EXPECT_EQ(std::string::npos, sanitized.command.find("C:\\Windows"));
}

TEST(IpcCommandValidatorTests, sanitizesServerConfigAndScriptPaths)
{
    IpcCommandValidator::SanitizedDaemonRequest sanitized;
    std::string reason;
    ASSERT_TRUE(IpcCommandValidator::sanitizeDaemonRequest(
        "weaves.exe --debug DEBUG1 --name ubuntu --game-mode "
        "--config C:\\Windows\\win.ini "
        "--screen-change-script C:\\Temp\\run.cmd "
        "--log C:\\Temp\\server.log --address [::]:24800",
        IpcCommandMessage::kElevateNever,
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        sanitized,
        &reason)) << reason;

    EXPECT_EQ(IpcCommandValidator::CommandRole::kServer, sanitized.role);
    EXPECT_EQ(IpcCommandMessage::kElevateNever, sanitized.elevateMode);
    EXPECT_FALSE(sanitized.elevationDowngraded);
    EXPECT_TRUE(sanitized.ignoredUnsafeArguments);
    EXPECT_EQ(
        "\"C:\\Program Files\\Weave\\weaves.exe\" --debug DEBUG1 "
        "--name ubuntu --game-mode --address [::]:24800 --no-daemon "
        "--no-tray --ipc",
        sanitized.command);
}

TEST(IpcCommandValidatorTests, daemonCommandAllowlistIsRoleSpecific)
{
    IpcCommandValidator::SanitizedDaemonRequest sanitized;
    std::string reason;
    EXPECT_FALSE(IpcCommandValidator::sanitizeDaemonRequest(
        "weavec --address 127.0.0.1:24800 server:24800",
        IpcCommandMessage::kElevateAsNeeded,
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        sanitized,
        &reason));
    EXPECT_FALSE(reason.empty());

    reason.clear();
    EXPECT_FALSE(IpcCommandValidator::sanitizeDaemonRequest(
        "weaves --yscroll 12",
        IpcCommandMessage::kElevateAsNeeded,
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        sanitized,
        &reason));
    EXPECT_FALSE(reason.empty());

    reason.clear();
    EXPECT_FALSE(IpcCommandValidator::sanitizeDaemonRequest(
        "weavec --service install server:24800",
        IpcCommandMessage::kElevateAsNeeded,
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        sanitized,
        &reason));
    EXPECT_FALSE(reason.empty());
}

TEST(IpcCommandValidatorTests, rejectsWatchdogOnlyStandbyFlagFromGuiCommand)
{
    IpcCommandValidator::SanitizedDaemonRequest sanitized;
    std::string reason;
    EXPECT_FALSE(IpcCommandValidator::sanitizeDaemonRequest(
        "weavec --service-standby server:24800",
        IpcCommandMessage::kElevateAsNeeded,
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        sanitized,
        &reason));
    EXPECT_NE(std::string::npos, reason.find("--service-standby"));
}

TEST(IpcCommandValidatorTests, elevatedDesktopCommandDropsFileCapability)
{
    std::string restricted;
    std::string reason;
    ASSERT_TRUE(IpcCommandValidator::restrictElevatedDesktopCommand(
        "\"C:\\Program Files\\Weave\\weavec.exe\" --no-daemon --ipc "
        "--enable-drag-drop --drop-dir C:\\User\\Inbox "
        "--profile-dir C:\\User\\Profile --plugin-dir C:\\User\\Plugins "
        "--log C:\\User\\weave.log --stop-on-desk-switch server:24800",
        restricted,
        &reason)) << reason;
    EXPECT_EQ(
        "\"C:\\Program Files\\Weave\\weavec.exe\" --no-daemon --ipc "
        "--stop-on-desk-switch server:24800",
              restricted);
}

TEST(IpcCommandValidatorTests, appendsOnlyTrustedAbsoluteProfileDirectory)
{
    std::string augmented;
    std::string reason;
    ASSERT_TRUE(IpcCommandValidator::appendTrustedProfileDirectory(
        "\"C:\\Program Files\\Weave\\weavec.exe\" --ipc server:24800",
        "C:\\ProgramData\\Weave\\LaunchProfiles\\owner\\v1-1234",
        augmented, &reason)) << reason;
    EXPECT_EQ(
        "\"C:\\Program Files\\Weave\\weavec.exe\" --ipc "
        "--profile-dir "
        "C:\\ProgramData\\Weave\\LaunchProfiles\\owner\\v1-1234 "
        "server:24800",
              augmented);

    EXPECT_FALSE(IpcCommandValidator::appendTrustedProfileDirectory(
        "weavec --ipc server:24800", "..\\attacker", augmented, &reason));
    EXPECT_FALSE(reason.empty());

    EXPECT_FALSE(IpcCommandValidator::appendTrustedProfileDirectory(
        "weavec --ipc server:24800", "C:\\safe\\..\\attacker",
        augmented, &reason));
    EXPECT_FALSE(IpcCommandValidator::appendTrustedProfileDirectory(
        "weavec --ipc server:24800", "C:\\safe\" --no-hooks",
        augmented, &reason));
}

TEST(IpcCommandValidatorTests, preservesUtf8TrustedProfileDirectory)
{
    const std::string profileDirectory =
        u8"C:\\Users\\\u7528\u6237 \u540d\\Weave\\LaunchProfiles\\v1";
    std::string augmented;
    std::string reason;

    ASSERT_TRUE(IpcCommandValidator::appendTrustedProfileDirectory(
        "weavec --ipc server:24800", profileDirectory,
        augmented, &reason)) << reason;
    EXPECT_EQ(
        std::string("weavec --ipc --profile-dir \"") +
            profileDirectory + "\" server:24800",
        augmented);
}

TEST(IpcCommandValidatorTests, derivesStandbyBeforeClientServerAddress)
{
    std::string derived;
    std::string reason;
    ASSERT_TRUE(IpcCommandValidator::deriveServiceStandbyCommand(
        "weavec --ipc server:24800", derived, &reason)) << reason;
    EXPECT_EQ("weavec --ipc --service-standby server:24800", derived);
    EXPECT_TRUE(IpcCommandValidator::deriveServiceStandbyCommand(
        derived, derived, &reason)) << reason;
    EXPECT_EQ("weavec --ipc --service-standby server:24800", derived);
}

TEST(IpcCommandValidatorTests, elevatedClientDerivationKeepsServerAddressLast)
{
    IpcCommandValidator::SanitizedDaemonRequest sanitized;
    std::string restricted;
    std::string profiled;
    std::string standby;
    std::string reason;

    ASSERT_TRUE(IpcCommandValidator::sanitizeDaemonRequest(
        "weavec --debug INFO --name windows --enable-drag-drop "
        "--drop-dir C:\\Users\\user\\Inbox 100.76.98.15:24800",
        IpcCommandMessage::kElevateAsNeeded,
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        sanitized,
        &reason)) << reason;
    ASSERT_TRUE(IpcCommandValidator::restrictElevatedDesktopCommand(
        sanitized.command, restricted, &reason)) << reason;
    ASSERT_TRUE(IpcCommandValidator::appendTrustedProfileDirectory(
        restricted,
        "C:\\ProgramData\\Weave\\LaunchProfiles\\owner\\v1-1234",
        profiled,
        &reason)) << reason;
    ASSERT_TRUE(IpcCommandValidator::deriveServiceStandbyCommand(
        profiled, standby, &reason)) << reason;

    EXPECT_EQ(
        "\"C:\\Program Files\\Weave\\weavec.exe\" --debug INFO "
        "--name windows --no-daemon --no-tray --ipc "
        "--stop-on-desk-switch --profile-dir "
        "C:\\ProgramData\\Weave\\LaunchProfiles\\owner\\v1-1234 "
        "--service-standby 100.76.98.15:24800",
        standby);
}

TEST(IpcCommandValidatorTests, refusesStandbyWithoutClientServerAddress)
{
    std::string derived;
    std::string reason;
    EXPECT_FALSE(IpcCommandValidator::deriveServiceStandbyCommand(
        "weavec --ipc", derived, &reason));
    EXPECT_NE(std::string::npos, reason.find("server address"));
}

TEST(IpcCommandValidatorTests, refusesDuplicateTrustedProfileDirectory)
{
    std::string augmented;
    std::string reason;
    EXPECT_FALSE(IpcCommandValidator::appendTrustedProfileDirectory(
        "weavec --profile-dir C:\\user --ipc server:24800",
        "C:\\ProgramData\\Weave\\LaunchProfiles\\owner\\v1-1234",
        augmented, &reason));
    EXPECT_NE(std::string::npos, reason.find("already contains"));
}

TEST(IpcCommandValidatorTests, refusesProfileAppendBeyondWindowsCommandLimit)
{
    std::string augmented;
    std::string reason;
    std::string command = "weavec";
    for (int index = 0; index < 8; ++index) {
        command += " " + std::string(4090, 'a');
    }
    command += " server:24800";
    EXPECT_FALSE(IpcCommandValidator::appendTrustedProfileDirectory(
        command, "C:\\ProgramData\\Weave\\Profile", augmented, &reason));
    EXPECT_TRUE(augmented.empty());
    EXPECT_NE(std::string::npos, reason.find("command-line limit"));
}

TEST(IpcCommandValidatorTests, sanitizeRejectsArgumentSmuggling)
{
    IpcCommandValidator::SanitizedDaemonRequest sanitized;
    std::string reason;
    EXPECT_FALSE(IpcCommandValidator::sanitizeDaemonRequest(
        "weavec --log C:\\Temp\\weave.log --no-hooks server:24800",
        IpcCommandMessage::kElevateAsNeeded,
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        sanitized,
        &reason));
    EXPECT_NE(std::string::npos, reason.find("disallowed argument"));

    reason.clear();
    EXPECT_FALSE(IpcCommandValidator::sanitizeDaemonRequest(
        "weavec server-a:24800 server-b:24800",
        IpcCommandMessage::kElevateAsNeeded,
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        sanitized,
        &reason));
    EXPECT_NE(std::string::npos, reason.find("disallowed argument"));

    reason.clear();
    EXPECT_FALSE(IpcCommandValidator::sanitizeDaemonRequest(
        "weavec --name bad/name server:24800",
        IpcCommandMessage::kElevateAsNeeded,
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        sanitized,
        &reason));
    EXPECT_NE(std::string::npos, reason.find("invalid screen name"));
}

TEST(IpcCommandValidatorTests, sanitizePreservesEmptyStopWithoutElevation)
{
    IpcCommandValidator::SanitizedDaemonRequest sanitized;
    std::string reason;
    ASSERT_TRUE(IpcCommandValidator::sanitizeDaemonRequest(
        "", IpcCommandMessage::kElevateAlways,
        "C:\\Program Files\\Weave\\weaves.exe",
        "C:\\Program Files\\Weave\\weavec.exe",
        sanitized,
        &reason)) << reason;
    EXPECT_EQ(IpcCommandValidator::CommandRole::kEmpty, sanitized.role);
    EXPECT_TRUE(sanitized.command.empty());
    EXPECT_EQ(IpcCommandMessage::kElevateAsNeeded, sanitized.elevateMode);
    EXPECT_TRUE(sanitized.elevationDowngraded);
}
