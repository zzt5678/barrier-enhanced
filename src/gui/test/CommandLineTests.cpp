#include "CommandLine.h"

#include "gtest/gtest.h"

#include <string>

TEST(CommandLineTests, quoteArgument_quotesEmptyAndSpacedArgs)
{
    EXPECT_EQ(std::string("\"\""), CommandLine::quoteArgument(QString()).toStdString());
    EXPECT_EQ(std::string("plain"),
              CommandLine::quoteArgument(QStringLiteral("plain")).toStdString());
    EXPECT_EQ(std::string("\"two words\""),
              CommandLine::quoteArgument(QStringLiteral("two words")).toStdString());
}

TEST(CommandLineTests, assembleCommand_quotesEachSpacedArgument)
{
    const QString command = CommandLine::assembleCommand(
        QStringLiteral("C:/Program Files/Weave/weaves.exe"),
        QStringList()
            << QStringLiteral("-f")
            << QStringLiteral("--drop-dir")
            << QStringLiteral("C:/Users/Test User/AppData/Local/Weave/workflow/inbox")
            << QStringLiteral("--profile-dir")
            << QStringLiteral("C:/Users/Test User/AppData/Roaming/Weave")
            << QStringLiteral("--log")
            << QStringLiteral("C:/Users/Test User/AppData/Local/Weave/weave log.txt")
            << QStringLiteral("-c")
            << QStringLiteral("C:/Users/Test User/AppData/Local/Temp/Weave Config.sgc"));

    EXPECT_EQ(
        std::string("\"C:/Program Files/Weave/weaves.exe\" -f --drop-dir "
                    "\"C:/Users/Test User/AppData/Local/Weave/workflow/inbox\" "
                    "--profile-dir \"C:/Users/Test User/AppData/Roaming/Weave\" "
                    "--log \"C:/Users/Test User/AppData/Local/Weave/weave log.txt\" "
                    "-c \"C:/Users/Test User/AppData/Local/Temp/Weave Config.sgc\""),
        command.toStdString());
}
