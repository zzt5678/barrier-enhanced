/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/GuiInstanceCoordinator.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QLockFile>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

namespace {

QString uniqueServerName()
{
    return QStringLiteral("weave-gui-test-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool waitForActivation(const int& activationCount)
{
    for (int attempt = 0; attempt < 50 && activationCount == 0; ++attempt) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return activationCount != 0;
}

} // namespace

TEST(GuiInstanceCoordinatorTests, secondaryStartActivatesPrimary)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    const QString lockPath = tempDir.filePath(QStringLiteral("gui.lock"));
    const QString serverName = uniqueServerName();
    GuiInstanceCoordinator primary(lockPath, serverName);
    ASSERT_EQ(GuiInstanceCoordinator::StartResult::Primary,
              primary.start(1, 0));

    int activationCount = 0;
    primary.setActivationHandler([&activationCount]() {
        ++activationCount;
    });

    GuiInstanceCoordinator secondary(lockPath, serverName);
    EXPECT_EQ(GuiInstanceCoordinator::StartResult::ExistingActivated,
              secondary.start(20, 5));
    EXPECT_TRUE(waitForActivation(activationCount));
    EXPECT_EQ(1, activationCount);
}

TEST(GuiInstanceCoordinatorTests, activationBeforeHandlerIsDeliveredOnce)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    const QString lockPath = tempDir.filePath(QStringLiteral("gui.lock"));
    const QString serverName = uniqueServerName();
    GuiInstanceCoordinator primary(lockPath, serverName);
    ASSERT_EQ(GuiInstanceCoordinator::StartResult::Primary,
              primary.start(1, 0));

    GuiInstanceCoordinator secondary(lockPath, serverName);
    ASSERT_EQ(GuiInstanceCoordinator::StartResult::ExistingActivated,
              secondary.start(20, 5));
    QCoreApplication::processEvents();

    int activationCount = 0;
    primary.setActivationHandler([&activationCount]() {
        ++activationCount;
    });
    EXPECT_EQ(1, activationCount);
}

TEST(GuiInstanceCoordinatorTests, heldLockWithoutServerFailsWithoutUi)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    const QString lockPath = tempDir.filePath(QStringLiteral("gui.lock"));
    QLockFile heldLock(lockPath);
    ASSERT_TRUE(heldLock.tryLock());

    GuiInstanceCoordinator coordinator(lockPath, uniqueServerName());
    EXPECT_EQ(GuiInstanceCoordinator::StartResult::Unavailable,
              coordinator.start(2, 1));
}
