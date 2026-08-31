/*  barrier -- mouse and keyboard sharing utility
    Copyright (C) 2026 OpenAI

    This package is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    found in the file LICENSE that should have accompanied this file.

    This package is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "../src/AppConfig.h"

#include <gtest/gtest.h>

#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>

TEST(AppConfigTests, LoadsPersistedProcessMode)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString settingsPath = dir.filePath("weave.ini");
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        settings.setValue("processMode", static_cast<int>(Service));
        settings.sync();
    }

    QSettings settings(settingsPath, QSettings::IniFormat);
    AppConfig config(&settings);

    EXPECT_EQ(Service, config.processMode());
}

TEST(AppConfigTests, LoadsPlatformDefaultProcessAndElevateModeFromEmptySettings)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    QSettings settings(dir.filePath("weave.ini"), QSettings::IniFormat);
    AppConfig config(&settings);

#if defined(Q_OS_WIN)
    EXPECT_EQ(Service, config.processMode());
    EXPECT_EQ(ElevateAlways, config.elevateMode());
    EXPECT_TRUE(config.autoConfig());
#else
    EXPECT_EQ(Desktop, config.processMode());
    EXPECT_EQ(ElevateAsNeeded, config.elevateMode());
    EXPECT_FALSE(config.autoConfig());
#endif
    EXPECT_TRUE(config.getRequireClientCertificate());
}

TEST(AppConfigTests, PreservesExplicitClientCertificateOptOut)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString settingsPath = dir.filePath("weave.ini");
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        settings.setValue("requireClientCertificate", false);
        settings.sync();
    }

    QSettings settings(settingsPath, QSettings::IniFormat);
    AppConfig config(&settings);
    EXPECT_FALSE(config.getRequireClientCertificate());
}

TEST(AppConfigTests, InvalidPersistedProcessModeFallsBackToPlatformDefault)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString settingsPath = dir.filePath("weave.ini");
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        settings.setValue("processMode", 999);
        settings.sync();
    }

    QSettings settings(settingsPath, QSettings::IniFormat);
    AppConfig config(&settings);

#if defined(Q_OS_WIN)
    EXPECT_EQ(Service, config.processMode());
#else
    EXPECT_EQ(Desktop, config.processMode());
#endif
}

TEST(AppConfigTests, InvalidPersistedElevateModeFallsBackToPlatformDefault)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const int invalidModes[] = {-1, 3, 257, 999};
    const QString settingsPath = dir.filePath("weave.ini");
    for (const int invalidMode : invalidModes) {
        QSettings settings(settingsPath, QSettings::IniFormat);
        settings.setValue("elevateModeEnum", invalidMode);
        settings.sync();

        AppConfig config(&settings);
        EXPECT_EQ(defaultElevateMode, config.elevateMode());
    }
}

TEST(AppConfigTests, SavesLoadedProcessMode)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString settingsPath = dir.filePath("weave.ini");
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        settings.setValue("processMode", static_cast<int>(Service));
        settings.sync();
        AppConfig config(&settings);
        EXPECT_EQ(Service, config.processMode());
    }

    QSettings settings(settingsPath, QSettings::IniFormat);
    EXPECT_EQ(static_cast<int>(Service), settings.value("processMode").toInt());
}

TEST(AppConfigTests, LegacyLinuxAutoConfigTrueWithoutExplicitMarkerLoadsDisabled)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString settingsPath = dir.filePath("weave.ini");
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        settings.setValue("autoConfig", true);
        settings.setValue("autoConfigPrompted", true);
        settings.sync();
    }

    QSettings settings(settingsPath, QSettings::IniFormat);
    AppConfig config(&settings);

#if defined(Q_OS_WIN)
    EXPECT_TRUE(config.autoConfig());
#else
    EXPECT_FALSE(config.autoConfig());
#endif
}

TEST(AppConfigTests, ExplicitAutoConfigSettingIsPreserved)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString settingsPath = dir.filePath("weave.ini");
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        settings.setValue("autoConfig", true);
        settings.setValue("autoConfigUserSet", true);
        settings.sync();
    }

    QSettings settings(settingsPath, QSettings::IniFormat);
    AppConfig config(&settings);

    EXPECT_TRUE(config.autoConfig());
}

TEST(AppConfigTests, SetAutoConfigMarksSettingAsExplicit)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString settingsPath = dir.filePath("weave.ini");
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        AppConfig config(&settings);
        config.setAutoConfig(true);
    }

    QSettings settings(settingsPath, QSettings::IniFormat);
    EXPECT_TRUE(settings.value("autoConfig").toBool());
    EXPECT_TRUE(settings.value("autoConfigUserSet").toBool());
}

TEST(AppConfigTests, SavingSettingsDoesNotMarkCancelledWizardComplete)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString settingsPath = dir.filePath("weave.ini");
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        settings.setValue("wizardLastRun", 0);
        settings.sync();

        AppConfig config(&settings);
        EXPECT_TRUE(config.wizardShouldRun());
        config.saveSettings();
    }

    QSettings settings(settingsPath, QSettings::IniFormat);
    EXPECT_EQ(0, settings.value("wizardLastRun").toInt());
}
