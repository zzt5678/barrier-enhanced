/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/SettingsDialog.h"

#include "../src/AppConfig.h"
#include "../src/QBarrierApplication.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QVBoxLayout>

// SettingsDialog only needs these application helpers for runtime language
// switching. Layout tests keep the language fixed and provide narrow stubs so
// the production dialog can be exercised without linking the full main window.
QBarrierApplication* QBarrierApplication::getInstance()
{
    return nullptr;
}

QBarrierApplication::~QBarrierApplication() = default;

void QBarrierApplication::switchTranslator(QString)
{
}

void setIndexFromItemData(QComboBox* comboBox, const QVariant& itemData)
{
    for (int i = 0; i < comboBox->count(); ++i) {
        if (comboBox->itemData(i) == itemData) {
            comboBox->setCurrentIndex(i);
            return;
        }
    }
}

namespace {

QSettings* useEnglishSettings(QSettings& settings)
{
    settings.setValue(QStringLiteral("language"), QStringLiteral("en"));
    return &settings;
}

class TestSettingsDialog : public SettingsDialog
{
public:
    using SettingsDialog::SettingsDialog;
    using QWidget::focusNextChild;
};

struct SettingsDialogFixture
{
    SettingsDialogFixture() :
        settings(dir.filePath(QStringLiteral("weave.ini")), QSettings::IniFormat),
        config(useEnglishSettings(settings)),
        dialog(nullptr, config)
    {
    }

    QTemporaryDir dir;
    QSettings settings;
    AppConfig config;
    TestSettingsDialog dialog;
};

} // namespace

TEST(SettingsDialogTests, KeepsActionsVisibleAndScrollsContentOnShortScreens)
{
    SettingsDialogFixture fixture;
    auto* scrollArea = fixture.dialog.findChild<QScrollArea*>(
        QStringLiteral("settingsScrollArea"));
    ASSERT_NE(nullptr, scrollArea);

    fixture.dialog.resize(420, 600);
    fixture.dialog.show();
    QApplication::processEvents();

    ASSERT_TRUE(fixture.dialog.buttonBox->isVisibleTo(&fixture.dialog));
    const QRect actionsRect(
        fixture.dialog.buttonBox->mapTo(&fixture.dialog, QPoint(0, 0)),
        fixture.dialog.buttonBox->size());
    EXPECT_TRUE(fixture.dialog.contentsRect().contains(actionsRect));
    EXPECT_GT(scrollArea->verticalScrollBar()->maximum(), 0);
    EXPECT_FALSE(scrollArea->widget()->isAncestorOf(fixture.dialog.buttonBox));
}

TEST(SettingsDialogTests, DynamicWorkflowControlsFollowVisualTabOrder)
{
    SettingsDialogFixture fixture;
    fixture.dialog.resize(420, 600);
    fixture.dialog.show();
    QApplication::processEvents();

    auto* workflowEnabled = fixture.dialog.findChild<QCheckBox*>(
        QStringLiteral("m_pCheckBoxWorkflowEnabled"));
    auto* suggestions = fixture.dialog.findChild<QCheckBox*>(
        QStringLiteral("m_pCheckBoxWorkflowSuggestions"));
    auto* notifications = fixture.dialog.findChild<QCheckBox*>(
        QStringLiteral("m_pCheckBoxShowTrayNotifications"));
    auto* historyLimit = fixture.dialog.findChild<QSpinBox*>(
        QStringLiteral("m_pSpinBoxWorkflowHistoryLimit"));
    auto* dormantSeconds = fixture.dialog.findChild<QSpinBox*>(
        QStringLiteral("m_pSpinBoxWorkflowDormantSeconds"));

    ASSERT_NE(nullptr, workflowEnabled);
    ASSERT_NE(nullptr, suggestions);
    ASSERT_NE(nullptr, notifications);
    ASSERT_NE(nullptr, historyLimit);
    ASSERT_NE(nullptr, dormantSeconds);

    fixture.dialog.activateWindow();
    QApplication::setActiveWindow(&fixture.dialog);
    workflowEnabled->setFocus(Qt::TabFocusReason);
    QApplication::processEvents();
    ASSERT_EQ(workflowEnabled, QApplication::focusWidget());
    EXPECT_TRUE(fixture.dialog.focusNextChild());
    EXPECT_EQ(suggestions, QApplication::focusWidget());
    EXPECT_TRUE(fixture.dialog.focusNextChild());
    EXPECT_EQ(notifications, QApplication::focusWidget());
    EXPECT_TRUE(fixture.dialog.focusNextChild());
    EXPECT_EQ(historyLimit, QApplication::focusWidget());
    EXPECT_TRUE(fixture.dialog.focusNextChild());
    EXPECT_EQ(dormantSeconds, QApplication::focusWidget());
    EXPECT_TRUE(fixture.dialog.focusNextChild());
    EXPECT_EQ(fixture.dialog.m_pComboLogLevel, QApplication::focusWidget());
}
