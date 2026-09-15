#pragma once
#include <QCoreApplication>
#include <QString>

// Translate only at the presentation boundary. Wire IDs, signatures and stored
// history continue to use WorkflowTypes' original English tokens.
inline QString workflowDisplayText(const QString& token)
{
    return QCoreApplication::translate("WorkflowDisplayText", token.toUtf8().constData());
}
