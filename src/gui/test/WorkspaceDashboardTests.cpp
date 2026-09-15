#include "../src/WorkspaceDashboard.h"
#include <gtest/gtest.h>
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QFile>
#include <QTranslator>
#include <QTextDocument>
TEST(WorkspaceDashboardTests, LocalIdentityIsAnAccessibleCardHighlightAndSharingReplacesActivity) {
    WorkspaceDashboard view(new QWidget);
    view.setDevices({{"windows", {-1,0}, false, WorkspaceDeviceRole::Client, WorkspaceDeviceState::Connected, "windows"},
        {"ubuntu", {0,0}, true, WorkspaceDeviceRole::Server, WorkspaceDeviceState::Connected, "ubuntu"},
        {"macbook", {1,0}, false, WorkspaceDeviceRole::Client, WorkspaceDeviceState::Connected, "macos"}});
    EXPECT_EQ(nullptr, view.findChild<QLabel*>("deviceLocal"));
    const auto cards = view.findChildren<QWidget*>("deviceCard");
    ASSERT_EQ(3, cards.size());
    EXPECT_TRUE(cards[1]->property("local").toBool());
    EXPECT_FALSE(cards[0]->property("local").toBool());
    EXPECT_TRUE(cards[1]->accessibleDescription().contains("This computer"));
    QTextDocument tip; tip.setHtml(cards[1]->toolTip());
    EXPECT_TRUE(tip.toPlainText().contains("This computer"));
    EXPECT_TRUE(tip.toPlainText().contains("Ubuntu"));
    EXPECT_EQ(nullptr, view.findChild<QListWidget*>("recentTransfers"));
    EXPECT_EQ(nullptr, view.findChild<QLabel*>("emptyTransfers"));
    auto* controls = view.findChild<QWidget*>("sharingControl");
    ASSERT_NE(nullptr, controls);
    auto* sharing = controls->findChild<QPushButton*>("sharingButton");
    auto* settings = controls->findChild<QPushButton*>("sharingSettingsButton");
    ASSERT_NE(nullptr, sharing); ASSERT_NE(nullptr, settings);
    int requested = 0;
    QObject::connect(&view, &WorkspaceDashboard::preferencesRequested, [&] { ++requested; });
    settings->click(); EXPECT_EQ(1, requested);
    view.setConnectionState("Runtime fixture", true, true);
    EXPECT_EQ(QString("Pause sharing"), sharing->text());
    EXPECT_TRUE(view.findChild<QLabel*>("keyboardSharingState")->text().contains("sharing enabled"));
    view.setConnectionState("Stopped fixture", false, false);
    EXPECT_TRUE(view.findChild<QLabel*>("keyboardSharingState")->text().contains("paused"));
    view.setClipboardSharingState("Awaiting authoritative clipboard state");
    EXPECT_TRUE(view.findChild<QLabel*>("clipboardSharingState")->text().contains("Awaiting authoritative clipboard state"));
}

TEST(WorkspaceDashboardTests, PlatformMetadataReachesEachIllustrationWithoutNameInference) {
    WorkspaceDashboard view(new QWidget);
    view.setDevices({{"not-an-os-name", {0,0}, true, WorkspaceDeviceRole::Server, WorkspaceDeviceState::Connected, "ubuntu"},
        {"windows", {1,0}, false, WorkspaceDeviceRole::Client, WorkspaceDeviceState::Configured, "unknown"}});
    const auto illustrations = view.findChildren<QLabel*>("deviceIllustration");
    ASSERT_EQ(2, illustrations.size());
    EXPECT_EQ(QString("ubuntu"), illustrations[0]->property("platform").toString());
    EXPECT_EQ(QString("unknown"), illustrations[1]->property("platform").toString());
}
TEST(WorkspaceDashboardTests, MissingCoordinatesRenderDistinctListRowsInsteadOfStackedDevices) {
    WorkspaceDashboard view(new QWidget);
    view.setDevices({{"first", {}, true, WorkspaceDeviceRole::Client},
        {"server", {}, false, WorkspaceDeviceRole::Server},
        {"third", {}, false, WorkspaceDeviceRole::Client}}, false, "Conflicting layout links");
    view.resize(760,640); view.show(); QApplication::processEvents();
    auto* description = view.findChild<QLabel*>("deviceLayoutDescription");
    ASSERT_NE(nullptr, description);
    EXPECT_TRUE(description->isVisible());
    EXPECT_TRUE(description->text().contains("Conflicting layout links"));
    const auto labels = view.findChildren<QLabel*>("deviceName");
    ASSERT_EQ(3, labels.size());
    EXPECT_LT(labels[0]->mapTo(&view, {}).y(), labels[1]->mapTo(&view, {}).y());
    EXPECT_LT(labels[1]->mapTo(&view, {}).y(), labels[2]->mapTo(&view, {}).y());
}
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
    EXPECT_FALSE(QRect(arrange->mapTo(&view, {}), arrange->size()).intersects(QRect(sharing->mapTo(&view, {}), sharing->size())));
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
TEST(WorkspaceDashboardTests, UnknownClipboardAndNarrowWindowRemainUsable) {
    WorkspaceDashboard view(new QWidget);
    view.resize(560, 420); view.show(); QApplication::processEvents();
    auto* state = view.findChild<QLabel*>("clipboardSharingState");
    ASSERT_NE(nullptr, state);
    EXPECT_TRUE(state->isVisible());
    EXPECT_EQ(QString("Clipboard: State unknown"), state->text());
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
TEST(WorkspaceDashboardTests, PlatformIllustrationsSurviveResizeAndUnknownRemainsNeutral) {
    WorkspaceDashboard view(new QWidget);
    view.setDevices({{"one", {0,0}, true, WorkspaceDeviceRole::Server, WorkspaceDeviceState::Connected, "ubuntu"},
        {"two", {1,0}, false, WorkspaceDeviceRole::Client, WorkspaceDeviceState::Connected, "windows"},
        {"three", {2,0}, false, WorkspaceDeviceRole::Client, WorkspaceDeviceState::Connected, "macos"},
        {"windows-name-only", {3,0}, false}});
    view.resize(1100,760); view.show(); QApplication::processEvents();
    const auto monitors = view.findChildren<QLabel*>("deviceIllustration");
    ASSERT_EQ(4, monitors.size());
    QVector<QImage> pictures;
    for (auto* monitor : monitors) { ASSERT_NE(nullptr, monitor->pixmap()); pictures.append(monitor->pixmap()->toImage()); }
    for (int i=0; i<3; ++i) EXPECT_NE(pictures[i], pictures[3]);
    EXPECT_NE(pictures[0], pictures[1]); EXPECT_NE(pictures[1], pictures[2]);
    view.resize(620,760); QApplication::processEvents();
    view.resize(1100,760); QApplication::processEvents();
    for (int i=0; i<4; ++i) EXPECT_EQ(pictures[i], monitors[i]->pixmap()->toImage());
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
