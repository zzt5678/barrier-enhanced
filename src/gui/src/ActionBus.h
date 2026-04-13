#pragma once

#include <QObject>
#include <QList>
#include <QStringList>

#include "WorkflowTypes.h"

class QWidget;
class WorkflowStore;

class ActionBus : public QObject
{
    Q_OBJECT

public:
    explicit ActionBus(WorkflowStore& store, QObject* parent = nullptr);

    QStringList capabilities() const;
    QList<ActionDescriptor> descriptorsForContext(const ContextItem& item) const;
    QString labelForAction(const QString& actionId) const;
    QVariantMap getContextSummary(const QString& contextId) const;
    QByteArray fetchContextPayload(const QString& contextId) const;

    bool execute(const QString& actionId,
                 const QString& contextId,
                 QWidget* anchor = nullptr,
                 QString* errorMessage = nullptr);
    bool executeQuickCommand(const QString& commandId,
                             QWidget* anchor = nullptr,
                             QString* errorMessage = nullptr);

signals:
    void notificationRequested(const QString& title, const QString& body);

private:
    bool openContext(const ContextItem& item, QWidget* anchor, QString* errorMessage);
    bool revealContext(const ContextItem& item, QString* errorMessage);
    bool saveContextToInbox(const ContextItem& item, QString* savedPath, QString* errorMessage);
    bool captureScreenshot(QString* contextId, QString* errorMessage);
    bool confirmIfNeeded(const ContextItem& item,
                         const QString& actionLabel,
                         QWidget* anchor) const;
    QStringList manifestPathsForContext(const ContextItem& item) const;
    QString uniqueInboxTarget(const QString& baseName, const QString& suffix = QString()) const;

private:
    WorkflowStore* m_store;
};
