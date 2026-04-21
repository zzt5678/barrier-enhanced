/*
* barrier -- mouse and keyboard sharing utility
* Copyright (C) 2018 Debauchee Open Source Group
*
* This package is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* found in the file LICENSE that should have accompanied this file.
*
* This package is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "LogWindow.h"

#include <QDateTime>
#include <QScrollBar>
#include <QTextDocument>

namespace {
constexpr int kFlushIntervalMs = 16;
constexpr int kMaxLogBlocks = 5000;
}

static QString getTimeStamp()
{
    QDateTime current = QDateTime::currentDateTime();
    return '[' + current.toString(Qt::ISODate) + ']';
}

LogWindow::LogWindow(QWidget *parent) :
    QDialog(parent),
    m_flushTimer(this)
{
    // explicitly unset DeleteOnClose so the log window can be show and hidden
    // repeatedly until Barrier is finished
    setAttribute(Qt::WA_DeleteOnClose, false);
    setupUi(this);

    m_flushTimer.setSingleShot(true);
    connect(&m_flushTimer, &QTimer::timeout, this, &LogWindow::flushPendingLines);
    m_pLogOutput->document()->setMaximumBlockCount(kMaxLogBlocks);
}

void LogWindow::startNewInstance()
{
    // put a space between last log output and new instance.
    if (!m_pendingLines.isEmpty() || !m_pLogOutput->document()->isEmpty())
        appendRaw("");
}

void LogWindow::appendInfo(const QString& text)
{
    appendRaw(getTimeStamp() + " INFO: " + text);
}

void LogWindow::appendDebug(const QString& text)
{
    appendRaw(getTimeStamp() + " DEBUG: " + text);
}

void LogWindow::appendError(const QString& text)
{
    appendRaw(getTimeStamp() + " ERROR: " + text);
}

void LogWindow::appendRaw(const QString& text)
{
    m_pendingLines.append(text);
    if (!m_flushTimer.isActive()) {
        m_flushTimer.start(kFlushIntervalMs);
    }
}

void LogWindow::flushPendingLines()
{
    if (m_pendingLines.isEmpty()) {
        return;
    }

    const bool stickToBottom =
        m_pLogOutput->verticalScrollBar()->value() >= m_pLogOutput->verticalScrollBar()->maximum();
    const QString output = m_pendingLines.join('\n');
    m_pendingLines.clear();

    m_pLogOutput->appendPlainText(output);

    if (stickToBottom) {
        m_pLogOutput->verticalScrollBar()->setValue(m_pLogOutput->verticalScrollBar()->maximum());
    }
}

void LogWindow::on_m_pButtonHide_clicked()
{
    hide();
}

void LogWindow::on_m_pButtonClearLog_clicked()
{
    m_flushTimer.stop();
    m_pendingLines.clear();
    m_pLogOutput->clear();
}
