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

#if defined(BARRIER_TEST_ENV)
    static void testMarkInboxTargetManaged(const QString& path) { markInboxTargetManaged(path); }
    static void testPruneManagedInboxDir(const QString& inboxDir, int maxItems, qint64 maxBytes)
    {
        pruneManagedInboxDir(inboxDir, maxItems, maxBytes);
    }
    static bool testCopyRecursively(const QString& sourcePath,
                                    const QString& destinationPath,
                                    QString* errorMessage);
#endif

signals:
    void notificationRequested(const QString& title, const QString& body);

private:
    bool openContext(const ContextItem& item, QWidget* anchor, QString* errorMessage);
    bool revealContext(const ContextItem& item, QString* errorMessage);
    bool saveContextToInbox(const ContextItem& item, QString* savedPath, QString* errorMessage);
    bool captureScreenshot(QString* contextId, QString* errorMessage);
    static void markInboxTargetManaged(const QString& path);
    static void pruneManagedInboxDir(const QString& inboxDir, int maxItems, qint64 maxBytes);
    void pruneManagedInbox(int maxItems = -1, qint64 maxBytes = -1) const;
    bool confirmIfNeeded(const ContextItem& item,
                         const QString& actionLabel,
                         QWidget* anchor) const;
    QStringList manifestPathsForContext(const ContextItem& item) const;
    QString uniqueInboxTarget(const QString& baseName, const QString& suffix = QString()) const;

private:
    WorkflowStore* m_store;
};
