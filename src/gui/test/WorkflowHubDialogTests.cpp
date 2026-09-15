#include "WorkflowHubDialog.h"
#include "WorkflowStore.h"
#include "ActionBus.h"
#include "AppConfig.h"
#include "ElidingLabel.h"
#include <gtest/gtest.h>
#include <QApplication>
#include <QFile>
#include <QPushButton>
#include <QTemporaryDir>
#include <QSettings>
#include <QSet>
#include <QScrollArea>
#include <QTabWidget>
#include <QListWidget>
#include <QMenu>
#include <QElapsedTimer>
#include <QThread>

TEST(WorkflowHubDialogTests, EmptyHistoryLeavesMostHeightForContentAndOnlyApplicableActions)
{
    QTemporaryDir temporary;
    QSettings settings(temporary.filePath("config.ini"), QSettings::IniFormat);
    AppConfig config(&settings);
    WorkflowStore store(config);
    ActionBus bus(store);
    WorkflowHubDialog dialog(store,bus);
    QFile style(":/res/styles/dark.qss"); ASSERT_TRUE(style.open(QIODevice::ReadOnly));
    dialog.setStyleSheet(QString::fromUtf8(style.readAll()));
    dialog.resize(620,500); dialog.show(); QApplication::processEvents();
    QSet<int> rows;
    for (auto* button : dialog.findChildren<QPushButton*>()) {
        if (!button->isVisible()) continue;
        rows.insert(button->mapTo(&dialog,QPoint()).y());
        EXPECT_GE(button->width(),button->sizeHint().width()) << button->text().toStdString();
    }
    EXPECT_EQ(rows.size(),1);
    auto* body = dialog.findChild<QScrollArea*>("historyBodyScroll");
    ASSERT_NE(nullptr, body);
    EXPECT_GE(body->height(), dialog.height() * 0.75);
    EXPECT_FALSE(dialog.findChild<QPushButton*>("cancelInboxSaveButton") && dialog.findChild<QPushButton*>("cancelInboxSaveButton")->isVisible());
    EXPECT_LE(dialog.width(),760);
    const QString preview=qEnvironmentVariable("WEAVE_WORKFLOW_PREVIEW");
    if(!preview.isEmpty()) EXPECT_TRUE(dialog.grab().save(preview));
}

TEST(WorkflowHubDialogTests, LongRuntimePathDoesNotForceWideDialog)
{
    QTemporaryDir temporary;
    QSettings settings(temporary.filePath("config.ini"), QSettings::IniFormat);
    AppConfig config(&settings); WorkflowStore store(config); ActionBus bus(store);
    WorkflowHubDialog dialog(store,bus);
    auto* label=dialog.findChild<QLabel*>("workflowRuntime");
    ASSERT_NE(nullptr,label);
    const QString path=QString("/inbox/")+QString(600,'x');
    auto* eliding=dynamic_cast<ElidingLabel*>(label); ASSERT_NE(nullptr,eliding);
    eliding->setText(path); dialog.resize(760,640); dialog.show(); QApplication::processEvents();
    // show refreshes live data; test the same public label boundary after that refresh.
    eliding->setText(path); QApplication::processEvents();
    EXPECT_EQ(path,eliding->toolTip());
    EXPECT_LE(dialog.minimumSizeHint().width(),760);
    EXPECT_NE(path,eliding->text());
}

TEST(WorkflowHubDialogTests, ContextMenuAndCancelFollowRealLocalSaveLifecycle)
{
    QTemporaryDir temporary;
    QSettings settings(temporary.filePath("config.ini"), QSettings::IniFormat);
    AppConfig config(&settings); WorkflowStore store(config); ActionBus bus(store);
    const QString id = store.testAddTextContext("Isolated local save test");
    WorkflowHubDialog dialog(store, bus);
    dialog.show(); QApplication::processEvents();
    auto* list = qobject_cast<QListWidget*>(dialog.findChild<QTabWidget*>()->widget(0));
    ASSERT_GT(list->count(), 0);
    list->setCurrentRow(0);
    auto* menu = dialog.findChild<QPushButton*>("historyActionsButton")->menu();
    ASSERT_NE(nullptr, menu);
    bool saveAvailable = false;
    for (auto* action : menu->actions())
        if (action->text() == "Save to Inbox") saveAvailable = action->isVisible() && action->isEnabled();
    EXPECT_TRUE(saveAvailable);
    auto* cancel = dialog.findChild<QPushButton*>("cancelInboxSaveButton");
    EXPECT_FALSE(cancel->isVisible());
    QString error;
    ASSERT_TRUE(bus.execute("save_to_inbox", id, &dialog, &error)) << error.toStdString();
    EXPECT_TRUE(bus.inboxSaveRunning()); EXPECT_TRUE(cancel->isVisible());
    QElapsedTimer deadline; deadline.start();
    while (bus.inboxSaveRunning() && deadline.elapsed() < 3000) {
        QApplication::processEvents(); QThread::msleep(1);
    }
    ASSERT_FALSE(bus.inboxSaveRunning());
    QApplication::processEvents(); EXPECT_FALSE(cancel->isVisible());
    EXPECT_EQ(QString("save_to_inbox"), store.receipts().front().actionId);
}
