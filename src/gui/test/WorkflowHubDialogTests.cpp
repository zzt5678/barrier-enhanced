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

TEST(WorkflowHubDialogTests, ActionsFitOnMultipleRowsAtLaptopWidth)
{
    QTemporaryDir temporary;
    QSettings settings(temporary.filePath("config.ini"), QSettings::IniFormat);
    AppConfig config(&settings);
    WorkflowStore store(config);
    ActionBus bus(store);
    WorkflowHubDialog dialog(store,bus);
    QFile style(":/res/styles/dark.qss"); ASSERT_TRUE(style.open(QIODevice::ReadOnly));
    dialog.setStyleSheet(QString::fromUtf8(style.readAll()));
    dialog.resize(760,640); dialog.show(); QApplication::processEvents();
    QSet<int> rows;
    for (auto* button : dialog.findChildren<QPushButton*>()) {
        rows.insert(button->mapTo(&dialog,QPoint()).y());
        EXPECT_GE(button->width(),button->sizeHint().width()) << button->text().toStdString();
    }
    EXPECT_GE(rows.size(),2);
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
