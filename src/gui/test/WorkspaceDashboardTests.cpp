#include "../src/WorkspaceDashboard.h"
#include <gtest/gtest.h>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QFile>
#include <QTranslator>
TEST(WorkspaceDashboardTests, RolesLocalBadgeAndFreshnessAreIndependentAcrossViewers) {
    WorkspaceDashboard view(new QWidget);
    const QVector<WorkspaceDevice> devices{
        {"windows", {0,0}, false, WorkspaceDeviceRole::Server, WorkspaceDeviceState::Connected},
        {"ubuntu", {1,0}, true, WorkspaceDeviceRole::Client, WorkspaceDeviceState::AwaitingSync},
        {"mac", {2,0}, false, WorkspaceDeviceRole::Client, WorkspaceDeviceState::Stale}};
    view.setDevices(devices);
    const auto description = view.findChild<QWidget*>("deviceCanvas")->accessibleDescription();
    EXPECT_TRUE(description.contains("windows: Server · Connected"));
    EXPECT_TRUE(description.contains("ubuntu: Client · This computer · Awaiting sync"));
    EXPECT_TRUE(description.contains("mac: Client · Out of date"));
    const auto names = view.findChildren<QLabel*>("deviceName");
    ASSERT_EQ(3, names.size());
    EXPECT_EQ(QString("windows"), names[0]->accessibleName());
    auto alternate = devices;
    alternate[0].local = true; alternate[1].local = false;
    view.setDevices(alternate);
    const auto updated = view.findChild<QWidget*>("deviceCanvas")->accessibleDescription();
    EXPECT_TRUE(updated.contains("windows: Server · This computer · Connected"));
    EXPECT_TRUE(updated.contains("ubuntu: Client · Awaiting sync"));
}
TEST(WorkspaceDashboardTests, NarrowHomeActionsFitTheirViewport) {
    QFile style(":/res/styles/dark.qss"); ASSERT_TRUE(style.open(QIODevice::ReadOnly));
    WorkspaceDashboard view(new QWidget);
    view.setStyleSheet(QString::fromUtf8(style.readAll()));
    view.resize(560, 600); view.show(); QApplication::processEvents();
    auto* home = view.findChild<QWidget*>("workspaceHome");
    EXPECT_LE(home->width(), home->parentWidget()->width());
    auto* arrange = view.findChild<QPushButton*>("arrangeButton");
    auto* sharing = view.findChild<QPushButton*>("sharingButton");
    EXPECT_FALSE(arrange->geometry().intersects(sharing->geometry()));
}
TEST(WorkspaceDashboardTests, UnknownRemoteNeverClaimsOnline) {
    WorkspaceDashboard view(new QWidget);
    view.setDevices({{"workstation", {0,0}, true}, {"peer", {1,0}, false}});
    auto* canvas = view.findChild<QWidget*>("deviceCanvas");
    ASSERT_NE(nullptr, canvas);
    EXPECT_TRUE(canvas->accessibleDescription().contains("peer: Role unknown · Configured"));
    EXPECT_FALSE(canvas->accessibleDescription().contains("Online"));
}
TEST(WorkspaceDashboardTests, SharingAndLayoutActionsRespectAvailability) {
    WorkspaceDashboard view(new QWidget);
    int calls = 0;
    QObject::connect(&view, &WorkspaceDashboard::sharingRequested, [&]{ ++calls; });
    auto* sharing = view.findChild<QPushButton*>("sharingButton");
    auto* arrange = view.findChild<QPushButton*>("arrangeButton");
    ASSERT_NE(nullptr, sharing); ASSERT_NE(nullptr, arrange);
    view.setConnectionState("Stopped", false, false);
    EXPECT_FALSE(arrange->isEnabled());
    EXPECT_TRUE(arrange->isHidden());
    auto* notice = view.findChild<QLabel*>("layoutReadOnlyNotice");
    ASSERT_NE(nullptr, notice);
    EXPECT_FALSE(notice->isHidden());
    sharing->click(); EXPECT_EQ(1, calls);
    view.setConnectionState("Running", true, true);
    EXPECT_TRUE(arrange->isEnabled());
    EXPECT_FALSE(arrange->isHidden());
    EXPECT_TRUE(notice->isHidden());
    EXPECT_EQ(QString("Pause sharing"), sharing->text());
}
TEST(WorkspaceDashboardTests, EmptyReceiptsAndNarrowWindowRemainUsable) {
    WorkspaceDashboard view(new QWidget);
    view.resize(560, 420); view.show(); QApplication::processEvents();
    auto* empty = view.findChild<QLabel*>("emptyTransfers");
    ASSERT_NE(nullptr, empty);
    EXPECT_TRUE(empty->isVisible());
    view.setRecentTransfers({"Received example.txt"});
    EXPECT_FALSE(empty->isVisible());
    auto* list = view.findChild<QListWidget*>("recentTransfers");
    ASSERT_NE(nullptr, list); EXPECT_EQ(1, list->count());
    EXPECT_LE(view.minimumSizeHint().width(), 560);
}
TEST(WorkspaceDashboardTests, IsolatedVisualPreview) {
    const QString output = qEnvironmentVariable("WEAVE_DASHBOARD_PREVIEW");
    if (output.isEmpty()) return;
    QTranslator translator;
    const QString language = qEnvironmentVariable("WEAVE_DASHBOARD_PREVIEW_LANGUAGE");
    if (!language.isEmpty()) {
        ASSERT_TRUE(translator.load(":/res/lang/gui_"+language+".qm"));
        QApplication::installTranslator(&translator);
    }
    QFile style(":/res/styles/dark.qss"); ASSERT_TRUE(style.open(QIODevice::ReadOnly));
    WorkspaceDashboard view(new QWidget);
    view.setStyleSheet(QString::fromUtf8(style.readAll()));
    view.setDevices({{"Studio desktop", {0,0}, false}, {"My workstation", {1,0}, true}, {"Travel laptop", {2,0}, false}});
    view.setConnectionState(QApplication::translate("MainWindow", "Weave is running."), true, true);
    view.setRecentTransfers(language.startsWith("zh") ? QStringList{"已接收  ·  示例.txt\n2026-09-14 09:41", "已接收  ·  说明.md\n2026-09-14 09:38"} : QStringList{"Received  ·  example.txt\n2026-09-14 09:41", "Received  ·  notes.md\n2026-09-14 09:38"});
    const int width = qEnvironmentVariableIntValue("WEAVE_DASHBOARD_PREVIEW_WIDTH");
    const int height = qEnvironmentVariableIntValue("WEAVE_DASHBOARD_PREVIEW_HEIGHT");
    view.resize(width > 0 ? width : 1100, height > 0 ? height : 760); view.show(); QApplication::processEvents();
    EXPECT_TRUE(view.grab().save(output));
    QApplication::removeTranslator(&translator);
}
TEST(WorkspaceDashboardTests, DeviceIllustrationsAreNotCroppedAtDesktopWidth) {
    WorkspaceDashboard view(new QWidget);
    view.setDevices({{"left", {0,0}, false}, {"local", {1,0}, true}, {"right", {2,0}, false}});
    view.resize(1488,1056); view.show(); QApplication::processEvents();
    for (auto* label : view.findChildren<QLabel*>("deviceIllustration")) {
        const QPixmap* picture = label->pixmap();
        ASSERT_NE(nullptr, picture);
        ASSERT_FALSE(picture->isNull());
        EXPECT_GE(label->width(), picture->width() / picture->devicePixelRatioF());
    }
}
TEST(WorkspaceDashboardTests, UnchangedReceiptsPreserveUserSelection) {
    WorkspaceDashboard view(new QWidget);
    const QStringList receipts{"first", "second"};
    view.setRecentTransfers(receipts);
    auto* list = view.findChild<QListWidget*>("recentTransfers");
    ASSERT_NE(nullptr, list);
    list->setCurrentRow(1);
    view.setRecentTransfers(receipts);
    EXPECT_EQ(1, list->currentRow());
}
TEST(WorkspaceDashboardTests, LongNamesDoNotOverlapIllustrationsInSmallWindow) {
    QFile style(":/res/styles/dark.qss"); ASSERT_TRUE(style.open(QIODevice::ReadOnly));
    WorkspaceDashboard view(new QWidget);
    view.setStyleSheet(QString::fromUtf8(style.readAll()));
    view.setDevices({{"Studio desktop", {0,0}, false}, {"My workstation", {1,0}, true}, {"Travel laptop", {2,0}, false}});
    view.resize(560,420); view.show(); QApplication::processEvents();
    for (auto* image : view.findChildren<QLabel*>("deviceIllustration")) {
        auto* name = image->parentWidget()->findChild<QLabel*>("deviceName");
        ASSERT_NE(nullptr, name);
        EXPECT_GT(name->y(), image->geometry().bottom());
        EXPECT_TRUE(image->parentWidget()->rect().contains(image->geometry()));
    }
}
