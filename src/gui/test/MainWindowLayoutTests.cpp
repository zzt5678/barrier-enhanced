#include "../src/MainWindow.h"
#include "../src/AppConfig.h"
#include <gtest/gtest.h>
#include <QApplication>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QScrollArea>
#include <QScrollBar>
#include <QPushButton>
#include <QFrame>
#include <QLabel>

TEST(MainWindowLayoutTests, SettingsReflowInsideViewportAndKeepActionsReachable) {
    ASSERT_EQ(QString("1"), qEnvironmentVariable("WEAVE_ISOLATED_UI_TEST"))
        << "Run using test/run-layout-tests.sh to isolate desktop, clipboard and configuration.";
    QTemporaryDir dir;
    QSettings settings(dir.filePath("test.ini"), QSettings::IniFormat);
    settings.setValue("autoStart", false);
    settings.setValue("autoConfig", false);
    settings.setValue("autoConfigPrompted", true);
    settings.setValue("groupServerChecked", true);
    settings.setValue("groupClientChecked", false);
    settings.setValue("workflowEnabled", false);
    settings.setValue("wizardLastRun", 999);
    settings.setValue("screenName", "My workstation");
    settings.setValue("internalConfig/screens/size", 15);
    settings.setValue("internalConfig/screens/7/name", "Studio desktop");
    settings.setValue("internalConfig/screens/8/name", "My workstation");
    settings.setValue("internalConfig/screens/9/name", "Travel laptop");
    AppConfig config(&settings);
    MainWindow window(settings, config);
    QFile style(":/res/styles/dark.qss"); ASSERT_TRUE(style.open(QIODevice::ReadOnly));
    window.setStyleSheet(QString::fromUtf8(style.readAll()));
    window.show();
    for (auto* button : window.findChildren<QPushButton*>())
        if (button->text() == "Settings") button->click();
    auto* scroll = window.findChild<QScrollArea*>("m_pMainScrollArea");
    ASSERT_NE(nullptr, scroll);
    auto* server = window.findChild<QWidget*>("m_pGroupServer");
    auto* client = window.findChild<QWidget*>("m_pGroupClient");
    auto* sidebar = window.findChild<QFrame*>("workspaceSidebar");
    for (int width : {560, 760, 810, 1100}) {
        window.resize(width, 760);
        QApplication::processEvents(); QApplication::processEvents();
        SCOPED_TRACE(width);
        EXPECT_EQ(width, window.width());
        if (width < 1100) EXPECT_GT(client->y(), server->y());
        else EXPECT_EQ(client->y(), server->y());
        EXPECT_LE(scroll->widget()->width(), scroll->viewport()->width());
        auto* bar = window.findChild<QWidget*>("fixedControlBar");
        EXPECT_TRUE(window.rect().contains(QRect(bar->mapTo(&window, QPoint()), bar->size())));
        auto* status = window.findChild<QLabel*>("m_pStatusLabel");
        EXPECT_EQ(QString("Ready"), status->text());
        const QString out = qEnvironmentVariable("WEAVE_LAYOUT_PREVIEW");
        if (!out.isEmpty()) EXPECT_TRUE(window.grab().save(out + QString("/settings-%1.png").arg(width)));
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QApplication::processEvents();
        auto* footer = window.findChild<QWidget*>("footerCard");
        EXPECT_LE(footer->mapTo(scroll->viewport(), footer->rect().bottomRight()).y(), scroll->viewport()->height());
        if (!out.isEmpty()) EXPECT_TRUE(window.grab().save(out + QString("/settings-bottom-%1.png").arg(width)));
        scroll->verticalScrollBar()->setValue(0);
    }
    // Changing the sidebar alone changes usable settings width without a window resize.
    sidebar->setFixedWidth(420);
    QApplication::processEvents(); QApplication::processEvents();
    EXPECT_GT(client->y(), server->y());
    EXPECT_LE(scroll->widget()->width(), scroll->viewport()->width());
    for (auto* button : window.findChildren<QPushButton*>())
        if (button->text() == "Devices") button->click();
    sidebar->setFixedWidth(180);
    for (int width : {560, 760, 810, 1100}) {
        window.resize(width, 760);
        QApplication::processEvents(); QApplication::processEvents();
        const QString out = qEnvironmentVariable("WEAVE_LAYOUT_PREVIEW");
        if (!out.isEmpty()) EXPECT_TRUE(window.grab().save(out + QString("/home-%1.png").arg(width)));
    }
}
