/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/ElidingLabel.h"

#include <gtest/gtest.h>

#include <QApplication>

TEST(ElidingLabelTests, KeepsFullTextAccessibleWhenNarrow)
{
    const QString status = QStringLiteral(
        "Weave stopped unexpectedly. Retrying in 1 second(s).");
    ElidingLabel label;
    label.resize(150, 32);
    label.setText(status);
    label.show();
    QApplication::processEvents();

    EXPECT_EQ(status, label.fullText());
    EXPECT_EQ(status, label.toolTip());
    EXPECT_EQ(status, label.accessibleName());
    EXPECT_NE(status, label.text());
    EXPECT_TRUE(label.text().endsWith(QChar(0x2026)) ||
                label.text().endsWith(QStringLiteral("...")));
    EXPECT_LE(label.minimumSizeHint().width(), 80);
}

TEST(ElidingLabelTests, RestoresFullTextWhenSpaceReturns)
{
    const QString status = QStringLiteral(
        "Weave stopped unexpectedly. Retrying in 1 second(s).");
    ElidingLabel label;
    label.resize(800, 32);
    label.setText(status);
    label.show();
    QApplication::processEvents();

    EXPECT_EQ(status, label.text());
}
