/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 OpenAI
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#ifndef ELIDINGLABEL_H
#define ELIDINGLABEL_H

#include <QLabel>

class ElidingLabel : public QLabel
{
public:
    using QLabel::QLabel;

    void setText(const QString& text);
    const QString& fullText() const { return m_fullText; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void updateDisplayedText();

    QString m_fullText;
};

#endif
