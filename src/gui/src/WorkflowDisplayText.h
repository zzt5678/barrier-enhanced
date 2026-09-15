#pragma once
#include <QCoreApplication>
#include <QString>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTextBoundaryFinder>
#include "WorkflowTypes.h"

// Qt's standard word wrapping clips a single long token. Add optional visual
// breaks at grapheme boundaries in the paint option only. Model text, copied
// content, accessible text, tooltips and stored receipts stay byte-for-byte intact.
class WorkflowReceiptDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        const QString original = option->text;
        QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, original);
        QString wrapped;
        int start = 0;
        for (int end = boundary.toNextBoundary(); end >= 0; end = boundary.toNextBoundary()) {
            wrapped += original.mid(start, end - start);
            if (end < original.size()) wrapped += QChar(0x200b);
            start = end;
        }
        option->text = wrapped;
    }
};

// Translate only at the presentation boundary. Wire IDs, signatures and stored
// history continue to use WorkflowTypes' original English tokens.
inline QString workflowDisplayText(const QString& token)
{
    return QCoreApplication::translate("WorkflowDisplayText", token.toUtf8().constData());
}

inline QString workflowReceiptStatus(const TransferReceipt& receipt)
{
    if (receipt.actionId == "remote_clipboard" && receipt.status == "Ready")
        return workflowDisplayText("Published locally");
    return workflowDisplayText(receipt.status);
}

inline QString workflowReceiptDetail(const TransferReceipt& receipt)
{
    // Match only the exact templates emitted by the named internal producer.
    // Captured device names remain literal; user content and unknown errors do
    // not pass through translation lookup or substring replacement.
    if (receipt.actionId == "remote_clipboard") {
        struct Template { const char* status; const char* pattern; const char* source; };
        static const Template templates[] = {
            {"Prepared", "^Remote clipboard prepared for ([0-9]+) item\\(s\\)\\.$", "Remote clipboard prepared for %1 item(s)."},
            {"Prefetching", "^Remote clipboard prefetch started for ([0-9]+) item\\(s\\)\\.(?: Target: (.*))?$", "Remote clipboard prefetch started for %1 item(s)."},
            {"Materialized", "^Remote files materialized \\(([0-9]+) item\\(s\\)\\); clipboard publication is pending\\.$", "Remote files materialized (%1 item(s)); clipboard publication is pending."},
            {"Ready", "^Remote clipboard published locally with ([0-9]+) item\\(s\\)\\.$", "Remote clipboard published locally with %1 item(s)."}
        };
        for (const auto& item : templates) {
            if (receipt.status != QLatin1String(item.status)) continue;
            const auto match = QRegularExpression(QString::fromLatin1(item.pattern), QRegularExpression::DotMatchesEverythingOption).match(receipt.detail);
            if (!match.hasMatch()) continue;
            QString result = workflowDisplayText(QString::fromLatin1(item.source)).arg(match.captured(1));
            if (match.lastCapturedIndex() == 2 && !match.captured(2).isEmpty())
                result += "\n" + workflowDisplayText("Target: %1").arg(match.captured(2));
            return result;
        }
        if (receipt.status == "Received" && receipt.detail == "Remote clipboard package received.")
            return workflowDisplayText("Remote clipboard package received locally.");
        if (receipt.detail == "Remote clipboard could not be published locally.")
            return workflowDisplayText(receipt.detail);
    }
    if (receipt.actionId == "save_to_inbox" &&
        (receipt.detail == "Core reported a finished transfer; destination publication is not verified by this log." ||
         receipt.detail == "Saving selected context to inbox…" || receipt.detail == "Inbox save cancelled."))
        return workflowDisplayText(receipt.detail);
    if (receipt.actionId == "capture_screenshot" &&
        (receipt.detail == "Encoding screenshot…" || receipt.detail == "Saved screenshot to workflow history." ||
         receipt.detail == "Saved a screenshot to the workflow history." || receipt.detail == "Screenshot encoding or storage failed."))
        return workflowDisplayText(receipt.detail);
    return receipt.detail;
}
