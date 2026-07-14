/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "ElidingLabel.h"

#include <QEvent>
#include <QResizeEvent>

namespace {
constexpr int kPreferredStatusWidth = 240;
constexpr int kMinimumStatusWidth = 64;
}

void ElidingLabel::setText(const QString& text)
{
    m_fullText = text;
    setToolTip(text);
    setAccessibleName(text);
    updateDisplayedText();
    updateGeometry();
}

QSize ElidingLabel::sizeHint() const
{
    QSize result = QLabel::sizeHint();
    const int fullTextWidth = fontMetrics().horizontalAdvance(m_fullText) +
        margin() * 2;
    result.setWidth(qMin(kPreferredStatusWidth, fullTextWidth));
    return result;
}

QSize ElidingLabel::minimumSizeHint() const
{
    QSize result = QLabel::minimumSizeHint();
    result.setWidth(qMin(kMinimumStatusWidth, sizeHint().width()));
    return result;
}

void ElidingLabel::resizeEvent(QResizeEvent* event)
{
    QLabel::resizeEvent(event);
    updateDisplayedText();
}

void ElidingLabel::changeEvent(QEvent* event)
{
    QLabel::changeEvent(event);
    if (event != nullptr &&
        (event->type() == QEvent::FontChange ||
         event->type() == QEvent::StyleChange)) {
        updateDisplayedText();
    }
}

void ElidingLabel::updateDisplayedText()
{
    const int availableWidth = qMax(0, contentsRect().width());
    QLabel::setText(fontMetrics().elidedText(
        m_fullText, Qt::ElideRight, availableWidth));
}
