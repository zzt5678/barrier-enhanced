#pragma once

#include <QObject>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QStringList>
#include <QUrl>

#include "WorkflowTypes.h"

class AppConfig;
class QClipboard;
class QImage;
class QMimeData;

class WorkflowStore : public QObject
{
    Q_OBJECT

public:
    explicit WorkflowStore(AppConfig& appConfig, QObject* parent = nullptr);

    void attachClipboard(QClipboard* clipboard);
    void setPeerDeviceHint(const QString& peerDevice);
    void setInteractiveSessionOpen(bool open);

    const QList<ContextItem>& history() const { return m_history; }
    const QList<SuggestionCard>& suggestions() const { return m_suggestions; }
    const QList<TransferReceipt>& receipts() const { return m_receipts; }
    WorkflowRuntimeMode runtimeMode() const { return m_runtimeMode; }
    QString runtimeModeText() const;
    QStringList capabilities() const;
    QString inboxDir() const { return m_inboxDir; }
    QString payloadPath(const QString& contextId) const;
    QString previewPath(const QString& contextId) const;
    QVariantMap getContextSummary(const QString& contextId) const;
    QByteArray fetchContextPayload(const QString& contextId) const;
    const ContextItem* contextById(const QString& contextId) const;
    const SuggestionCard* suggestionById(const QString& suggestionId) const;

    void recordReceipt(const QString& contextId,
                       const QString& actionId,
                       const QString& status,
                       const QString& detail);
    void recordLogLine(const QString& line);
    bool addCapturedImageContext(const QImage& image, QString* contextId = nullptr);

signals:
    void historyChanged();
    void suggestionsChanged();
    void receiptsChanged();
    void runtimeModeChanged(const QString& mode);
    void notificationRequested(const QString& title, const QString& body);

private slots:
    void handleClipboardChanged();
    void evaluateRuntimeMode();

private:
    ContextItem buildTextContext(const QString& text);
    ContextItem buildImageContext(const QImage& image);
    ContextItem buildUrlsContext(const QList<QUrl>& urls);
    ContextItem buildPathContext(const QStringList& paths);
    ContextItem buildPathContext(const QStringList& paths, bool hasFolders, qint64 knownBytes);
    void consumeClipboardMime(const QMimeData* mimeData);
    void addContextItem(ContextItem item, const QString& signature);
    void addSuggestionsForItem(const ContextItem& item);
    void pruneHistory();
    void pruneSuggestions();
    void pruneReceipts();
    void prunePreviewAssets();
    void removeManagedAssets(const ContextItem& item);
    void markActivity(WorkflowRuntimeMode preferredMode);
    void setRuntimeModeInternal(WorkflowRuntimeMode mode);

    QString newContextId() const;
    QString currentSourceDevice() const;
    QString storageFilePath(const QString& subDir, const QString& id, const QString& suffix) const;
    QString writeTextPayload(const QString& id, const QString& text, const QString& suffix);
    QString writeBinaryPayload(const QString& id, const QByteArray& data, const QString& suffix);
    QString writeJsonPayload(const QString& id, const QStringList& paths);
    QString writePreviewImage(const QString& id, const QImage& image);
    QString summarizeText(const QString& text) const;
    QString summarizePaths(const QStringList& paths, bool hasFolders) const;
    qint64 approximateMetadataBytes(const ContextItem& item) const;
    qint64 totalHistoryBytes() const;
    qint64 totalPreviewBytes() const;
    SensitivityLevel detectSensitivity(const QString& text) const;
    bool looksLikeUrl(const QString& text) const;
    bool looksLikeExistingPath(const QString& text) const;

private:
    AppConfig* m_appConfig;
    QClipboard* m_clipboard;
    QList<ContextItem> m_history;
    QList<SuggestionCard> m_suggestions;
    QList<TransferReceipt> m_receipts;
    QString m_peerDeviceHint;
    QString m_lastSignature;
    int m_interactiveSessionDepth;
    WorkflowRuntimeMode m_runtimeMode;
    QDateTime m_lastActivity;
    QDateTime m_burstUntil;
    QString m_storageRoot;
    QString m_payloadDir;
    QString m_previewDir;
    QString m_inboxDir;
    bool m_ignoreClipboardChanges;
};
