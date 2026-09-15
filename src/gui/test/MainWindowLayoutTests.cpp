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
#include "../src/SettingsDialog.h"
#include "../src/ServerConfigDialog.h"
#include "../src/ScreenSettingsDialog.h"
#include <QTranslator>
#include <QLineEdit>
#include <QDialogButtonBox>
#include "../src/BarrierLocale.h"
#include "../src/CommandPaletteDialog.h"
#include "../src/WorkflowHubDialog.h"
#include "../src/WorkflowStore.h"
#include "../src/ActionBus.h"
#include "../src/HotkeyDialog.h"
#include "../src/ActionDialog.h"
#include "../src/WorkspaceDashboard.h"
#include <QListWidget>
#include <QScreen>
#include <QTimer>
#include <QTextBrowser>
#include <QTabWidget>
#include <QMouseEvent>
#include <QDrag>
#include <QMessageBox>

TEST(DialogPolishTests, HotkeyEditorLoadsExistingSequenceAndCancelPreservesIt) {
    Hotkey hotkey; KeySequence sequence;
    sequence.appendKey(Qt::Key_Control, Qt::ControlModifier);
    ASSERT_TRUE(sequence.appendKey(Qt::Key_K, Qt::ControlModifier));
    hotkey.setKeySequence(sequence);
    HotkeyDialog editor(nullptr, hotkey);
    const auto* capture = static_cast<const KeySequenceWidget*>(editor.m_pKeySequenceWidgetHotkey);
    EXPECT_EQ(sequence.toString(), capture->keySequence().toString());
    EXPECT_FALSE(capture->text().contains("keystroke("));
    EXPECT_NE(Qt::NoFocus, capture->focusPolicy());
    editor.reject();
    EXPECT_EQ(sequence.toString(), hotkey.keySequence().toString());
    HotkeyDialog accepted(nullptr, hotkey);
    accepted.buttonBox->button(QDialogButtonBox::Ok)->click();
    EXPECT_EQ(QDialog::Accepted, accepted.result());
    EXPECT_EQ(sequence.toString(), hotkey.keySequence().toString());
}

TEST(DialogPolishTests, KeyboardCaptureEscapeRestoresTheLastAcceptedSequence) {
    Hotkey hotkey; KeySequence sequence;
    ASSERT_TRUE(sequence.appendKey(Qt::Key_K, Qt::NoModifier));
    hotkey.setKeySequence(sequence);
    HotkeyDialog editor(nullptr, hotkey); editor.show(); QApplication::processEvents();
    auto* capture = editor.m_pKeySequenceWidgetHotkey;
    QKeyEvent start(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(capture, &start);
    EXPECT_TRUE(capture->isDown());
    QKeyEvent finish(QEvent::KeyPress, Qt::Key_J, Qt::NoModifier);
    QApplication::sendEvent(capture, &finish);
    EXPECT_EQ(QString("j"), static_cast<const KeySequenceWidget*>(capture)->keySequence().toString());
    QApplication::sendEvent(capture, &start);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(capture, &escape);
    EXPECT_FALSE(capture->isDown());
    EXPECT_EQ(QString("j"), static_cast<const KeySequenceWidget*>(capture)->keySequence().toString());
    editor.reject();
    EXPECT_EQ(QString("k"), hotkey.keySequence().toString());
}

TEST(DialogPolishTests, ThreeViewerRoleLocalAndStaleFixturesKeepPhysicalOrder) {
    QFile style(":/res/styles/dark.qss"); ASSERT_TRUE(style.open(QIODevice::ReadOnly));
    WorkspaceDashboard view(new QWidget); view.setStyleSheet(QString::fromUtf8(style.readAll()));
    const QString output = qEnvironmentVariable("WEAVE_LAYOUT_PREVIEW");
    const QStringList names{"zyt", "zyt-System-Product-Name", "macbook-air"};
    for (const QString& language : {QString("en"), QString("zh-CN")}) {
        BarrierLocale::switchLanguage(language); QApplication::processEvents(); view.retranslate();
        for (int local = 0; local < 3; ++local) {
            QVector<WorkspaceDevice> devices;
            for (int i = 0; i < 3; ++i) devices.append({names[i], {i,0}, i == local,
                i == 1 ? WorkspaceDeviceRole::Server : WorkspaceDeviceRole::Client,
                i == 2 ? WorkspaceDeviceState::Stale : WorkspaceDeviceState::Configured});
            view.setDevices(devices);
            view.setConnectionState(language == "en" ? "Presentation fixture · configured layout" : QString::fromUtf8("界面测试样本 · 已配置布局"), false, local == 1);
            view.resize(760,640); view.show(); QApplication::processEvents();
            const auto labels = view.findChildren<QLabel*>("deviceName");
            ASSERT_EQ(3, labels.size());
            for (int i = 0; i < 3; ++i) EXPECT_EQ(names[i], labels[i]->accessibleName());
            EXPECT_EQ(QString("Server"), view.findChildren<QLabel*>("deviceRole")[1]->text());
            EXPECT_EQ(local != 1, view.findChild<QPushButton*>("arrangeButton")->isHidden());
            if (!output.isEmpty()) EXPECT_TRUE(view.grab().save(output + "/" + language + "-viewer-" + QString::number(local) + ".png"));
        }
    }
    BarrierLocale::switchLanguage("en"); QApplication::processEvents();
}

TEST(DialogPolishTests, SmallAliasListsAndConditionalActionsDoNotWasteViewport) {
    Screen screen("device"); ScreenSettingsDialog editor(nullptr, &screen);
    QFile style(":/res/styles/dark.qss"); ASSERT_TRUE(style.open(QIODevice::ReadOnly));
    editor.setStyleSheet(QString::fromUtf8(style.readAll())); editor.resize(420,540);
    editor.show(); QApplication::processEvents();
    EXPECT_LE(editor.m_pListAliases->height(), 100);
    EXPECT_LE(editor.m_pGroupAliases->height(), 180);
    QTemporaryDir dir; QSettings settings(dir.filePath("actions.ini"), QSettings::IniFormat);
    ServerConfig server(&settings, 5, 3, "device", nullptr);
    Hotkey hotkey; Action action; ActionDialog actions(nullptr, server, hotkey, action);
    EXPECT_TRUE(actions.m_pComboSwitchToScreen->isHidden());
    EXPECT_TRUE(actions.m_pComboSwitchInDirection->isHidden());
    actions.m_pRadioSwitchInDirection->setChecked(true);
    EXPECT_FALSE(actions.m_pComboSwitchInDirection->isHidden());
    EXPECT_TRUE(actions.m_pKeySequenceWidgetHotkey->isHidden());
    actions.reject();
    EXPECT_EQ(Action::keystroke, action.type());
}

TEST(DialogPolishTests, EmptyHistoryExplainsNextStepAndUnavailableCommandsExplainWhy) {
    QTemporaryDir dir; QSettings settings(dir.filePath("empty.ini"), QSettings::IniFormat);
    settings.setValue("workflowEnabled", false);
    AppConfig config(&settings); WorkflowStore store(config); ActionBus bus(store);
    WorkflowHubDialog history(store, bus); history.resize(620,500); history.show();
    CommandPaletteDialog palette(store, bus); palette.show(); QApplication::processEvents();
    auto* preview = history.findChild<QLabel*>("m_pWorkflowPreview");
    EXPECT_TRUE(preview->isHidden());
    EXPECT_TRUE(history.findChild<QTextBrowser*>()->toPlainText().contains("No clipboard history"));
    auto* tabs = history.findChild<QTabWidget*>();
    EXPECT_LE(tabs->height(), 150);
    auto* list = palette.findChild<QListWidget*>();
    for (int i : {1,2,3,4}) {
        EXPECT_FALSE(list->item(i)->flags().testFlag(Qt::ItemIsEnabled));
        EXPECT_FALSE(list->item(i)->toolTip().isEmpty());
    }
    palette.findChild<QLineEdit*>()->setText("no matching command 123");
    auto* empty = palette.findChild<QLabel*>("paletteEmptyState");
    ASSERT_NE(nullptr, empty); EXPECT_TRUE(empty->isVisible());
}

TEST(DialogLanguageTests, AppConfirmationButtonsFollowSelectedLanguage) {
    QMessageBox box(QMessageBox::Question, "Test", "Test", QMessageBox::Yes | QMessageBox::No | QMessageBox::Retry | QMessageBox::Cancel);
    BarrierLocale::switchLanguage("zh-CN"); QApplication::processEvents();
    EXPECT_TRUE(box.button(QMessageBox::Yes)->text().contains(QString::fromUtf8("是")));
    EXPECT_TRUE(box.button(QMessageBox::No)->text().contains(QString::fromUtf8("否")));
    EXPECT_TRUE(box.button(QMessageBox::Retry)->text().contains(QString::fromUtf8("重试")));
    EXPECT_TRUE(box.button(QMessageBox::Cancel)->text().contains(QString::fromUtf8("取消")));
    BarrierLocale::switchLanguage("en"); QApplication::processEvents();
    EXPECT_TRUE(box.button(QMessageBox::Yes)->text().contains("Yes"));
}

TEST(DialogLanguageTests, KeepsDeviceDragIconsAndEditedKeySequenceAcrossSwitches) {
    ASSERT_EQ(QString("1"), qEnvironmentVariable("WEAVE_ISOLATED_UI_TEST"));
    QTemporaryDir dir; QSettings settings(dir.filePath("retranslate.ini"), QSettings::IniFormat);
    ServerConfig server(&settings, 5, 3, "device", nullptr);
    ServerConfigDialog layout(nullptr, server, "device");
    Hotkey hotkey; Action action; ActionDialog editor(nullptr, server, hotkey, action);
    KeySequence edited; ASSERT_TRUE(edited.appendKey(Qt::Key_K, Qt::ControlModifier));
    editor.m_pKeySequenceWidgetHotkey->setKeySequence(edited);
    const QString text = editor.m_pKeySequenceWidgetHotkey->text();
    ASSERT_FALSE(text.isEmpty());
    for (const QString& language : {QString("zh-CN"), QString("en"), QString("zh-CN"), QString("en")}) {
        BarrierLocale::switchLanguage(language); QApplication::processEvents(); QApplication::processEvents();
        EXPECT_FALSE(layout.m_pLabelNewScreenWidget->pixmap(Qt::ReturnByValue).isNull());
        EXPECT_FALSE(layout.m_pTrashScreenWidget->pixmap(Qt::ReturnByValue).isNull());
        EXPECT_EQ(text, editor.m_pKeySequenceWidgetHotkey->text());
        EXPECT_EQ(edited.toString(), static_cast<const KeySequenceWidget*>(editor.m_pKeySequenceWidgetHotkey)->keySequence().toString());
    }
    layout.show(); QApplication::processEvents();
    QTimer::singleShot(50, [] { QDrag::cancel(); });
    QMouseEvent dragPress(QEvent::MouseButtonPress, QPointF(4, 4), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(layout.m_pLabelNewScreenWidget, &dragPress);
    EXPECT_FALSE(layout.m_pLabelNewScreenWidget->pixmap(Qt::ReturnByValue).isNull());
    layout.m_pLabelNewScreenWidget->clear();
    QMouseEvent missingImagePress(QEvent::MouseButtonPress, QPointF(4, 4), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(layout.m_pLabelNewScreenWidget, &missingImagePress);
    EXPECT_FALSE(missingImagePress.isAccepted());

    editor.show(); QApplication::processEvents();
    auto* recording = editor.m_pKeySequenceWidgetHotkey;
    QMouseEvent recordPress(QEvent::MouseButtonPress, QPointF(4, 4), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(recording, &recordPress);
    QKeyEvent control(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
    QApplication::sendEvent(recording, &control);
    const QString inProgress = recording->text();
    ASSERT_TRUE(recording->isDown());
    BarrierLocale::switchLanguage("zh-CN"); QApplication::processEvents();
    EXPECT_EQ(inProgress, recording->text()); EXPECT_TRUE(recording->isDown());
    QKeyEvent finish(QEvent::KeyPress, Qt::Key_J, Qt::ControlModifier);
    QApplication::sendEvent(recording, &finish);
    EXPECT_FALSE(recording->isDown());
    EXPECT_TRUE(recording->text().contains("j", Qt::CaseInsensitive));
    BarrierLocale::switchLanguage("en"); QApplication::processEvents();
}

TEST(DialogLanguageTests, PreviewCancelRestoresLanguageWithoutSavingEdits) {
    ASSERT_EQ(QString("1"), qEnvironmentVariable("WEAVE_ISOLATED_UI_TEST"));
    EXPECT_EQ(QString("en"), BarrierLocale::normalize("unavailable"));
    EXPECT_EQ(QString("en"), BarrierLocale::normalize("en_US.UTF-8"));
    EXPECT_EQ(QString("zh-CN"), BarrierLocale::normalize("zh-Hans-CN"));
    EXPECT_EQ(QString("zh-TW"), BarrierLocale::normalize("zh_tw"));
    QTemporaryDir dir;
    QSettings settings(dir.filePath("preview.ini"), QSettings::IniFormat);
    settings.setValue("language", "en");
    AppConfig config(&settings);
    SettingsDialog dialog(nullptr, config);
    const QString title = dialog.windowTitle();
    dialog.m_pLineEditScreenName->setText("unsaved");
    dialog.m_pComboLanguage->setCurrentIndex(dialog.m_pComboLanguage->findData("zh-CN"));
    QApplication::processEvents();
    EXPECT_NE(title, dialog.windowTitle());
    dialog.buttonBox->button(QDialogButtonBox::Cancel)->click();
    QApplication::processEvents();
    EXPECT_EQ(title, dialog.windowTitle());
    EXPECT_NE(QString("unsaved"), config.screenName());
    EXPECT_TRUE(BarrierLocale::switchLanguage("unavailable"));
}

TEST(DialogLanguageTests, PaletteAndHistoryRetainSelectionAndTranslateWhileOpen) {
    ASSERT_EQ(QString("1"), qEnvironmentVariable("WEAVE_ISOLATED_UI_TEST"));
    QTemporaryDir dir;
    QSettings settings(dir.filePath("history.ini"), QSettings::IniFormat);
    settings.setValue("workflowEnabled", false);
    AppConfig config(&settings);
    WorkflowStore store(config);
    ActionBus bus(store);
    CommandPaletteDialog palette(store, bus);
    WorkflowHubDialog history(store, bus);
    store.recordReceipt("", "open_latest", "success", "Test receipt", false);
    auto* tabs = history.findChild<QTabWidget*>();
    tabs->setCurrentIndex(2);
    auto* receipts = qobject_cast<QListWidget*>(tabs->widget(2));
    receipts->setCurrentRow(0);
    const QVariant receiptId = receipts->currentItem()->data(Qt::UserRole);
    const QString paletteTitle = palette.windowTitle(), historyTitle = history.windowTitle();
    auto* list = palette.findChild<QListWidget*>();
    list->setCurrentRow(6);
    const QVariant id = list->currentItem()->data(Qt::UserRole);
    BarrierLocale::switchLanguage("zh_CN");
    QApplication::processEvents();
    EXPECT_NE(paletteTitle, palette.windowTitle());
    EXPECT_NE(historyTitle, history.windowTitle());
    EXPECT_FALSE(history.findChild<QLabel*>("workflowRuntime")->toolTip().contains("Dormant"));
    EXPECT_EQ(QString("Text"), contextKindText(ContextKind::Text));
    EXPECT_EQ(QString("Sensitive"), sensitivityText(SensitivityLevel::Sensitive));
    EXPECT_EQ(receiptId, receipts->currentItem()->data(Qt::UserRole));
    EXPECT_EQ(QString("success"), store.receipts().front().status);
    EXPECT_TRUE(history.findChild<QTextBrowser*>()->toPlainText().contains(QString::fromUtf8("成功")));
    EXPECT_TRUE(history.findChild<QTextBrowser*>()->toPlainText().contains(QString::fromUtf8("操作")));
    EXPECT_EQ(id, list->currentItem()->data(Qt::UserRole));
    BarrierLocale::switchLanguage("en");
    QApplication::processEvents();
    EXPECT_EQ(paletteTitle, palette.windowTitle());
}

TEST(DialogLanguageTests, PaletteNeverExecutesACommandHiddenByLiveTranslation) {
    ASSERT_EQ(QString("1"), qEnvironmentVariable("WEAVE_ISOLATED_UI_TEST"));
    BarrierLocale::switchLanguage("en"); QApplication::processEvents();
    QTemporaryDir dir; QSettings settings(dir.filePath("palette-filter.ini"), QSettings::IniFormat);
    settings.setValue("workflowEnabled", false);
    AppConfig config(&settings); WorkflowStore store(config); ActionBus bus(store);
    CommandPaletteDialog palette(store, bus);
    int opened = 0;
    QObject::connect(&palette, &CommandPaletteDialog::workflowHubRequested, [&] { ++opened; });
    palette.show(); QApplication::processEvents();
    auto* filter = palette.findChild<QLineEdit*>();
    auto* list = palette.findChild<QListWidget*>();
    filter->setText("Open local history"); list->setCurrentRow(0);
    ASSERT_FALSE(list->currentItem()->isHidden());
    BarrierLocale::switchLanguage("zh-CN"); QApplication::processEvents();
    EXPECT_EQ(QString("Open local history"), filter->text());
    EXPECT_EQ(nullptr, list->currentItem());
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(filter, &enter);
    EXPECT_EQ(0, opened);
    // Defend the execution boundary even if a stale selection is injected.
    list->setCurrentRow(0);
    ASSERT_TRUE(list->currentItem()->isHidden());
    QApplication::sendEvent(filter, &enter);
    EXPECT_EQ(0, opened);
    filter->setText(QString::fromUtf8("打开本地历史"));
    ASSERT_NE(nullptr, list->currentItem());
    EXPECT_FALSE(list->currentItem()->isHidden());
    QApplication::sendEvent(filter, &enter);
    EXPECT_EQ(1, opened);
    BarrierLocale::switchLanguage("en"); QApplication::processEvents();
}

TEST(DialogLanguageTests, NormalizesSavedLocaleAndRetainsEditsAcrossLiveSwitches) {
    ASSERT_EQ(QString("1"), qEnvironmentVariable("WEAVE_ISOLATED_UI_TEST"));
    QTemporaryDir dir;
    QSettings settings(dir.filePath("locale.ini"), QSettings::IniFormat);
    settings.setValue("language", "zh_CN.UTF-8");
    AppConfig config(&settings);
    SettingsDialog dialog(nullptr, config);
    EXPECT_EQ(QString("zh-CN"), dialog.m_pComboLanguage->currentData().toString());
    ServerConfig server(&settings, 5, 3, "my-device", nullptr);
    ServerConfigDialog layout(nullptr, server, "my-device");
    EXPECT_GT(layout.m_pScreenSetupView->maximumHeight(), 273);
    auto* view = layout.m_pScreenSetupView;
    layout.show(); QApplication::processEvents();
    view->setCurrentIndex(view->model()->index(0, 0));
    QTimer::singleShot(0, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets())
            if (auto* popup = qobject_cast<ScreenSettingsDialog*>(widget)) popup->reject();
    });
    layout.findChild<QPushButton*>("editScreenButton")->click();
    EXPECT_TRUE(view->model()->index(0, 0).data().toString().isEmpty());
    Screen screen("original");
    ScreenSettingsDialog editor(nullptr, &screen);
    editor.m_pLineEditName->setText("edited-device");
    const QString englishTitle = editor.windowTitle();
    QTranslator translator;
    ASSERT_TRUE(translator.load(":/res/lang/gui_zh-CN.qm"));
    for (int repeat = 0; repeat < 2; ++repeat) {
        qApp->installTranslator(&translator);
        QApplication::processEvents();
        EXPECT_NE(englishTitle, editor.windowTitle());
        EXPECT_EQ(QString("edited-device"), editor.m_pLineEditName->text());
        qApp->removeTranslator(&translator);
        QApplication::processEvents();
        EXPECT_EQ(englishTitle, editor.windowTitle());
    }
}

TEST(DialogLayoutTests, EditorsStayInsideScreenWithReachableActionsInBothLanguages) {
    ASSERT_EQ(QString("1"), qEnvironmentVariable("WEAVE_ISOLATED_UI_TEST"));
    QTemporaryDir dir;
    QSettings settings(dir.filePath("dialogs.ini"), QSettings::IniFormat);
    settings.setValue("language", "en"); settings.setValue("workflowEnabled", false);
    AppConfig config(&settings);
    ServerConfig server(&settings, 5, 3, "my-device", nullptr);
    WorkflowStore store(config); ActionBus bus(store);
    SettingsDialog preferences(nullptr, config);
    ServerConfigDialog layout(nullptr, server, "my-device");
    Screen screen("Studio desktop"); ScreenSettingsDialog editor(nullptr, &screen);
    Hotkey hotkey; HotkeyDialog keys(nullptr, hotkey); Action action;
    ActionDialog actions(nullptr, server, hotkey, action);
    CommandPaletteDialog palette(store, bus); WorkflowHubDialog history(store, bus);
    const QList<QPair<QString, QDialog*>> dialogs{{"preferences", &preferences}, {"layout", &layout},
        {"screen", &editor}, {"hotkey", &keys}, {"action", &actions}, {"palette", &palette}, {"history", &history}};
    QFile file(":/res/styles/dark.qss"); ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QString style = QString::fromUtf8(file.readAll());
    for (const QString& language : {QString("en"), QString("zh-CN")}) {
        preferences.m_pComboLanguage->setCurrentIndex(preferences.m_pComboLanguage->findData(language));
        BarrierLocale::switchLanguage(language); QApplication::processEvents();
        for (const auto& item : dialogs) {
            auto* dialog = item.second;
            SCOPED_TRACE((language + "/" + item.first).toStdString());
            dialog->setStyleSheet(style);
            if (language == "en") dialog->move(-100, -100);
            dialog->show();
            QApplication::processEvents(); QApplication::processEvents();
            EXPECT_LE(dialog->height(), dialog->screen()->availableGeometry().height());
            EXPECT_LE(dialog->width(), dialog->screen()->availableGeometry().width());
            EXPECT_TRUE(dialog->screen()->availableGeometry().contains(dialog->frameGeometry()));
            if (auto* box = dialog->findChild<QDialogButtonBox*>()) {
                for (auto* button : box->buttons())
                    EXPECT_TRUE(dialog->rect().contains(QRect(button->mapTo(dialog, QPoint()), button->size())));
                if (box->button(QDialogButtonBox::Cancel)) {
                    EXPECT_EQ(language == "en" ? QString("Cancel") : QString::fromUtf8("取消"), box->button(QDialogButtonBox::Cancel)->text());
                    EXPECT_TRUE(box->button(QDialogButtonBox::Cancel)->icon().isNull());
                }
            }
            const QString out = qEnvironmentVariable("WEAVE_LAYOUT_PREVIEW");
            if (item.first == "preferences") {
                dialog->activateWindow(); dialog->raise();
                preferences.m_pLineEditScreenName->setFocus(Qt::TabFocusReason);
                QApplication::processEvents(); QApplication::processEvents();
                ASSERT_TRUE(dialog->isActiveWindow());
                ASSERT_TRUE(preferences.m_pLineEditScreenName->hasFocus());
            }
            if (!out.isEmpty()) EXPECT_TRUE(dialog->grab().save(out + "/" + language + "-" + item.first + ".png"));
            if (item.first == "preferences") {
                auto* check = preferences.m_pCheckBoxEnableCrypto;
                ASSERT_FALSE(check->hasFocus());
                const QImage unfocused = check->grab().toImage();
                check->setFocus(Qt::TabFocusReason); QApplication::processEvents();
                ASSERT_TRUE(check->hasFocus());
                EXPECT_FALSE(unfocused == check->grab().toImage())
                    << "Keyboard focus must visibly change the checkbox, not just its internal state";
                if (!out.isEmpty()) EXPECT_TRUE(dialog->grab().save(out + "/" + language + "-preferences-focus.png"));
            }
            if (item.first == "history") {
                for (auto* button : dialog->findChildren<QPushButton*>())
                    if (button->isVisible()) EXPECT_TRUE(dialog->rect().contains(QRect(button->mapTo(dialog, QPoint()), button->size())));
                dialog->resize(620, 500); QApplication::processEvents(); QApplication::processEvents();
                for (auto* button : dialog->findChildren<QPushButton*>())
                    if (button->isVisible()) EXPECT_TRUE(dialog->rect().contains(QRect(button->mapTo(dialog, QPoint()), button->size())));
                EXPECT_GE(dialog->findChild<QScrollArea*>("historyBodyScroll")->height(), dialog->height() * 0.75);
                if (!out.isEmpty()) EXPECT_TRUE(dialog->grab().save(out + "/" + language + "-history-narrow.png"));
            }
            if (item.first == "layout") {
                for (int tab = 1; tab < layout.m_pTabWidget->count(); ++tab) {
                    layout.m_pTabWidget->setCurrentIndex(tab); QApplication::processEvents();
                    auto* scroll = layout.findChild<QScrollArea*>("dialogEditorScroll");
                    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                    QApplication::processEvents();
                    EXPECT_TRUE(layout.rect().contains(QRect(layout.m_pButtonBox->mapTo(&layout, QPoint()), layout.m_pButtonBox->size())));
                    if (!out.isEmpty()) EXPECT_TRUE(layout.grab().save(out + "/" + language + QString("-layout-tab-%1.png").arg(tab)));
                }
                layout.m_pTabWidget->setCurrentIndex(0);
            }
            if (item.first == "screen") {
                dialog->resize(420, 540); QApplication::processEvents(); QApplication::processEvents();
                auto* scroll = dialog->findChild<QScrollArea*>("dialogEditorScroll");
                EXPECT_LE(scroll->widget()->width(), scroll->viewport()->width());
                if (!out.isEmpty()) EXPECT_TRUE(dialog->grab().save(out + "/" + language + "-screen-narrow.png"));
            }
            dialog->hide();
        }
    }
    BarrierLocale::switchLanguage("en"); QApplication::processEvents();
}

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
        for (auto* action : bar->findChildren<QPushButton*>())
            if (action->isVisible()) EXPECT_TRUE(window.rect().contains(QRect(action->mapTo(&window, QPoint()), action->size())));
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
        for (auto* illustration : window.findChildren<QLabel*>("deviceIllustration"))
            EXPECT_EQ(illustration->devicePixelRatioF(), illustration->pixmap(Qt::ReturnByValue).devicePixelRatioF());
    }
    BarrierLocale::switchLanguage("zh-CN");
    QApplication::processEvents(); QApplication::processEvents();
    EXPECT_NE(QString("Ready"), window.findChild<QLabel*>("m_pStatusLabel")->text());
    const QString out = qEnvironmentVariable("WEAVE_LAYOUT_PREVIEW");
    if (!out.isEmpty()) EXPECT_TRUE(window.grab().save(out + "/zh-CN-home.png"));
    for (auto* button : window.findChildren<QPushButton*>())
        if (button->text() == QString::fromUtf8("设置")) button->click();
    QApplication::processEvents(); QApplication::processEvents();
    if (!out.isEmpty()) EXPECT_TRUE(window.grab().save(out + "/zh-CN-settings.png"));
    BarrierLocale::switchLanguage("en"); QApplication::processEvents();
    EXPECT_EQ(QString("Ready"), window.findChild<QLabel*>("m_pStatusLabel")->text());
}
