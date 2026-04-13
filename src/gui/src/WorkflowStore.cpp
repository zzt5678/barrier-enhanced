#include "WorkflowStore.h"

#include "AppConfig.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace {

const int kDefaultMetadataBudgetBytes = 8 * 1024 * 1024;
const int kDefaultPreviewCountLimit = 20;
const int kDefaultPreviewBudgetBytes = 12 * 1024 * 1024;
const int kReceiptLimit = 60;
const int kSuggestionLimit = 40;
const int kBurstSeconds = 15;

QString normalizePathLikeText(const QString& text)
{
    QString normalized = text.trimmed();
    if (normalized.startsWith('"') && normalized.endsWith('"') && normalized.size() > 1) {
        normalized = normalized.mid(1, normalized.size() - 2);
    }
    return normalized;
}

QString joinPathsSignature(const QStringList& paths)
{
    QStringList normalized = paths;
    normalized.sort();
    return normalized.join(QStringLiteral("||"));
}

bool writeImageFile(const QString& filePath, const QImage& image)
{
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    return image.save(filePath, "PNG");
}

} // namespace

WorkflowStore::WorkflowStore(AppConfig& appConfig, QObject* parent) :
    QObject(parent),
    m_appConfig(&appConfig),
    m_clipboard(nullptr),
    m_interactiveSessionDepth(0),
    m_runtimeMode(WorkflowRuntimeMode::Dormant),
    m_lastActivity(QDateTime::currentDateTimeUtc()),
    m_storageRoot(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)),
    m_ignoreClipboardChanges(false)
{
    if (m_storageRoot.isEmpty()) {
        m_storageRoot = appConfig.barrierProgramDir() + QStringLiteral("workflow");
    }

    m_storageRoot = QDir(m_storageRoot).filePath(QStringLiteral("workflow"));
    m_payloadDir = QDir(m_storageRoot).filePath(QStringLiteral("payload"));
    m_previewDir = QDir(m_storageRoot).filePath(QStringLiteral("preview"));
    m_inboxDir = appConfig.workflowInboxDir();

    QDir().mkpath(m_storageRoot);
    QDir().mkpath(m_payloadDir);
    QDir().mkpath(m_previewDir);
    QDir().mkpath(m_inboxDir);

    auto* timer = new QTimer(this);
    timer->setInterval(5000);
    connect(timer, &QTimer::timeout, this, &WorkflowStore::evaluateRuntimeMode);
    timer->start();
}

void WorkflowStore::attachClipboard(QClipboard* clipboard)
{
    if (m_clipboard == clipboard) {
        return;
    }

    if (m_clipboard != nullptr) {
        disconnect(m_clipboard, &QClipboard::dataChanged, this, &WorkflowStore::handleClipboardChanged);
    }

    m_clipboard = clipboard;
    if (m_clipboard != nullptr) {
        connect(m_clipboard, &QClipboard::dataChanged, this, &WorkflowStore::handleClipboardChanged);
    }
}

void WorkflowStore::setPeerDeviceHint(const QString& peerDevice)
{
    m_peerDeviceHint = peerDevice.trimmed();
}

void WorkflowStore::setInteractiveSessionOpen(bool open)
{
    if (open) {
        ++m_interactiveSessionDepth;
        markActivity(WorkflowRuntimeMode::Active);
        return;
    }

    m_interactiveSessionDepth = qMax(0, m_interactiveSessionDepth - 1);
    evaluateRuntimeMode();
}

QString WorkflowStore::runtimeModeText() const
{
    return workflowRuntimeModeText(m_runtimeMode);
}

QStringList WorkflowStore::capabilities() const
{
    return {
        QStringLiteral("workflow_v1"),
        QStringLiteral("clipboard_index_v1"),
        QStringLiteral("action_bus_v1"),
        QStringLiteral("suggestions_v1")
    };
}

QString WorkflowStore::payloadPath(const QString& contextId) const
{
    const ContextItem* item = contextById(contextId);
    return item ? item->payloadRef : QString();
}

QString WorkflowStore::previewPath(const QString& contextId) const
{
    const ContextItem* item = contextById(contextId);
    return item ? item->previewRef : QString();
}

QVariantMap WorkflowStore::getContextSummary(const QString& contextId) const
{
    QVariantMap summary;
    const ContextItem* item = contextById(contextId);
    if (item == nullptr) {
        return summary;
    }

    summary.insert(QStringLiteral("id"), item->id);
    summary.insert(QStringLiteral("source_device"), item->sourceDevice);
    summary.insert(QStringLiteral("kind"), contextKindText(item->kind));
    summary.insert(QStringLiteral("summary"), item->summary);
    summary.insert(QStringLiteral("byte_size"), item->byteSize);
    summary.insert(QStringLiteral("preview_ref"), item->previewRef);
    summary.insert(QStringLiteral("sensitivity"), sensitivityText(item->sensitivity));
    summary.insert(QStringLiteral("suggested_actions"), item->suggestedActions);
    summary.insert(QStringLiteral("created_at"), item->createdAt);
    summary.insert(QStringLiteral("expires_at"), item->expiresAt);
    return summary;
}

QByteArray WorkflowStore::fetchContextPayload(const QString& contextId) const
{
    const ContextItem* item = contextById(contextId);
    if (item == nullptr || item->payloadRef.isEmpty()) {
        return QByteArray();
    }

    QFile file(item->payloadRef);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }

    return file.readAll();
}

const ContextItem* WorkflowStore::contextById(const QString& contextId) const
{
    for (const ContextItem& item : m_history) {
        if (item.id == contextId) {
            return &item;
        }
    }

    return nullptr;
}

const SuggestionCard* WorkflowStore::suggestionById(const QString& suggestionId) const
{
    for (const SuggestionCard& card : m_suggestions) {
        if (card.id == suggestionId) {
            return &card;
        }
    }

    return nullptr;
}

void WorkflowStore::recordReceipt(const QString& contextId,
                                  const QString& actionId,
                                  const QString& status,
                                  const QString& detail)
{
    TransferReceipt receipt;
    receipt.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    receipt.contextId = contextId;
    receipt.actionId = actionId;
    receipt.status = status;
    receipt.detail = detail;
    receipt.createdAt = QDateTime::currentDateTimeUtc();

    m_receipts.prepend(receipt);
    pruneReceipts();
    emit receiptsChanged();
    emit notificationRequested(QStringLiteral("Barrier Workflow"), detail.isEmpty()
        ? status
        : QStringLiteral("%1: %2").arg(status, detail));
}

void WorkflowStore::recordLogLine(const QString& line)
{
    static const QRegularExpression droppedFileExpr(
        QStringLiteral("dropped file \"([^\"]+)\" in \"([^\"]+)\""));
    static const QRegularExpression transferFailedExpr(
        QStringLiteral("drop file failed: (.+)$"));

    const QRegularExpressionMatch droppedMatch = droppedFileExpr.match(line);
    if (droppedMatch.hasMatch()) {
        const QString fileName = droppedMatch.captured(1);
        const QString directory = droppedMatch.captured(2);
        const QString fullPath = QDir(directory).filePath(fileName);
        ContextItem item = buildPathContext(QStringList{fullPath});
        item.sourceDevice = currentSourceDevice();
        item.summary = QStringLiteral("Received %1").arg(QFileInfo(fullPath).fileName());
        const QString signature = QStringLiteral("drop:%1").arg(fullPath);
        addContextItem(item, signature);
        recordReceipt(item.id,
                      QStringLiteral("save_to_inbox"),
                      QStringLiteral("Saved"),
                      QStringLiteral("Received %1 in %2").arg(fileName, directory));
        return;
    }

    const QRegularExpressionMatch failedMatch = transferFailedExpr.match(line);
    if (failedMatch.hasMatch()) {
        recordReceipt(QString(),
                      QStringLiteral("save_to_inbox"),
                      QStringLiteral("Failed"),
                      failedMatch.captured(1));
        return;
    }

    if (line.contains(QStringLiteral("file transfer finished"))) {
        recordReceipt(QString(),
                      QStringLiteral("save_to_inbox"),
                      QStringLiteral("Finished"),
                      QStringLiteral("File transfer finished successfully."));
    }
}

bool WorkflowStore::addCapturedImageContext(const QImage& image, QString* contextId)
{
    if (image.isNull()) {
        return false;
    }

    ContextItem item = buildImageContext(image);
    item.summary = QStringLiteral("Screenshot %1x%2").arg(image.width()).arg(image.height());
    item.sourceDevice = m_appConfig->screenName();
    const QString signature = QStringLiteral("screenshot:%1:%2:%3")
        .arg(item.summary)
        .arg(item.createdAt.toSecsSinceEpoch())
        .arg(item.byteSize);
    addContextItem(item, signature);
    recordReceipt(item.id,
                  QStringLiteral("capture_screenshot"),
                  QStringLiteral("Captured"),
                  QStringLiteral("Saved a screenshot preview to the workflow history."));

    if (contextId != nullptr) {
        *contextId = item.id;
    }

    return true;
}

void WorkflowStore::handleClipboardChanged()
{
    if (!m_appConfig->getWorkflowEnabled() || m_ignoreClipboardChanges || m_clipboard == nullptr) {
        return;
    }

    consumeClipboardMime(m_clipboard->mimeData());
}

void WorkflowStore::evaluateRuntimeMode()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const qint64 idleSeconds = m_lastActivity.secsTo(now);

    if (now < m_burstUntil) {
        setRuntimeModeInternal(WorkflowRuntimeMode::Burst);
        return;
    }

    if (m_interactiveSessionDepth == 0 &&
        idleSeconds >= qMax(10, m_appConfig->getWorkflowDormantSeconds())) {
        prunePreviewAssets();
        setRuntimeModeInternal(WorkflowRuntimeMode::Dormant);
        return;
    }

    setRuntimeModeInternal(WorkflowRuntimeMode::Active);
}

ContextItem WorkflowStore::buildTextContext(const QString& text)
{
    ContextItem item;
    item.id = newContextId();
    item.sourceDevice = currentSourceDevice();
    item.kind = looksLikeUrl(text) ? ContextKind::Link : ContextKind::Text;
    item.summary = summarizeText(text);
    item.byteSize = text.toUtf8().size();
    item.mimeType = item.kind == ContextKind::Link
        ? QStringLiteral("text/uri-list")
        : QStringLiteral("text/plain");
    item.sensitivity = detectSensitivity(text);
    item.createdAt = QDateTime::currentDateTimeUtc();
    item.expiresAt = item.createdAt.addDays(7);
    item.payloadRef = writeTextPayload(
        item.id,
        text,
        item.kind == ContextKind::Link ? QStringLiteral(".url.txt") : QStringLiteral(".txt"));
    item.payloadAvailable = !item.payloadRef.isEmpty();
    item.managedPayload = item.payloadAvailable;
    return item;
}

ContextItem WorkflowStore::buildImageContext(const QImage& image)
{
    ContextItem item;
    item.id = newContextId();
    item.sourceDevice = currentSourceDevice();
    item.kind = ContextKind::Image;
    item.summary = QStringLiteral("Image %1x%2").arg(image.width()).arg(image.height());
    item.mimeType = QStringLiteral("image/png");
    item.sensitivity = SensitivityLevel::Normal;
    item.createdAt = QDateTime::currentDateTimeUtc();
    item.expiresAt = item.createdAt.addDays(7);

    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    item.byteSize = data.size();
    item.payloadRef = writeBinaryPayload(item.id, data, QStringLiteral(".png"));
    item.previewRef = writePreviewImage(item.id, image);
    item.payloadAvailable = !item.payloadRef.isEmpty();
    item.managedPayload = item.payloadAvailable;
    item.managedPreview = !item.previewRef.isEmpty();
    return item;
}

ContextItem WorkflowStore::buildUrlsContext(const QList<QUrl>& urls)
{
    QStringList localPaths;
    QStringList remoteUrls;
    bool hasFolders = false;
    qint64 byteSize = 0;

    for (const QUrl& url : urls) {
        if (!url.isLocalFile()) {
            if (url.isValid()) {
                remoteUrls << url.toString();
            }
            continue;
        }

        const QString localPath = url.toLocalFile();
        QFileInfo info(localPath);
        if (!info.exists()) {
            continue;
        }

        localPaths << info.absoluteFilePath();
        if (info.isDir()) {
            hasFolders = true;
        }
        else {
            byteSize += info.size();
        }
    }

    if (localPaths.isEmpty() && !remoteUrls.isEmpty()) {
        return buildTextContext(remoteUrls.first());
    }

    return buildPathContext(localPaths, hasFolders, byteSize);
}

ContextItem WorkflowStore::buildPathContext(const QStringList& paths)
{
    bool hasFolders = false;
    qint64 byteSize = 0;
    for (const QString& path : paths) {
        QFileInfo info(path);
        if (info.isDir()) {
            hasFolders = true;
            continue;
        }
        if (info.isFile()) {
            byteSize += info.size();
        }
    }
    return buildPathContext(paths, hasFolders, byteSize);
}

ContextItem WorkflowStore::buildPathContext(const QStringList& paths, bool hasFolders, qint64 knownBytes)
{
    ContextItem item;
    item.id = newContextId();
    item.sourceDevice = currentSourceDevice();
    item.kind = hasFolders ? ContextKind::Folder : ContextKind::File;
    item.summary = summarizePaths(paths, hasFolders);
    item.byteSize = knownBytes;
    item.mimeType = QStringLiteral("application/x-barrier-path-manifest");
    item.sensitivity = SensitivityLevel::Normal;
    item.createdAt = QDateTime::currentDateTimeUtc();
    item.expiresAt = item.createdAt.addDays(14);
    item.payloadRef = writeJsonPayload(item.id, paths);
    item.payloadAvailable = !item.payloadRef.isEmpty();
    item.managedPayload = item.payloadAvailable;
    return item;
}

void WorkflowStore::consumeClipboardMime(const QMimeData* mimeData)
{
    if (mimeData == nullptr) {
        return;
    }

    ContextItem item;
    QString signature;

    if (mimeData->hasUrls() && !mimeData->urls().isEmpty()) {
        item = buildUrlsContext(mimeData->urls());
        QStringList paths;
        QStringList remoteUrls;
        for (const QUrl& url : mimeData->urls()) {
            if (url.isLocalFile()) {
                paths << url.toLocalFile();
            }
            else if (url.isValid()) {
                remoteUrls << url.toString();
            }
        }
        signature = QStringLiteral("urls:%1").arg(paths.isEmpty()
            ? remoteUrls.join(QStringLiteral("||"))
            : joinPathsSignature(paths));
    }
    else if (mimeData->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mimeData->imageData());
        if (image.isNull()) {
            return;
        }
        item = buildImageContext(image);
        signature = QStringLiteral("image:%1:%2:%3")
            .arg(image.width())
            .arg(image.height())
            .arg(item.byteSize);
    }
    else if (mimeData->hasText()) {
        const QString text = normalizePathLikeText(mimeData->text());
        if (text.isEmpty()) {
            return;
        }

        if (looksLikeExistingPath(text)) {
            item = buildPathContext(QStringList{text});
            signature = QStringLiteral("path:%1").arg(text);
        }
        else {
            item = buildTextContext(text);
            signature = QStringLiteral("text:%1:%2").arg(contextKindText(item.kind), text.left(512));
        }
    }
    else {
        return;
    }

    addContextItem(item, signature);
}

void WorkflowStore::addContextItem(ContextItem item, const QString& signature)
{
    if (signature == m_lastSignature) {
        return;
    }

    m_lastSignature = signature;
    item.suggestedActions.clear();

    m_history.prepend(item);
    addSuggestionsForItem(m_history.front());
    pruneHistory();
    pruneSuggestions();
    markActivity(item.byteSize > (1024 * 1024) ? WorkflowRuntimeMode::Burst : WorkflowRuntimeMode::Active);
    emit historyChanged();
}

void WorkflowStore::addSuggestionsForItem(const ContextItem& item)
{
    if (!m_appConfig->getSuggestionsEnabled()) {
        return;
    }

    const auto addSuggestion = [&](const QString& actionId,
                                   double confidence,
                                   bool requiresConfirmation = false)
    {
        SuggestionCard card;
        card.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        card.contextId = item.id;
        card.source = item.sourceDevice;
        card.targetDevice = QStringLiteral("local");
        card.actionId = actionId;
        card.previewText = item.summary;
        card.confidence = confidence;
        card.expiresAt = QDateTime::currentDateTimeUtc().addSecs(3600);
        card.requiresConfirmation = requiresConfirmation || item.sensitivity == SensitivityLevel::Sensitive;
        m_suggestions.prepend(card);
    };

    switch (item.kind) {
    case ContextKind::Link:
        addSuggestion(QStringLiteral("open_link"), 0.98, item.sensitivity == SensitivityLevel::Sensitive);
        addSuggestion(QStringLiteral("save_to_inbox"), 0.84);
        break;
    case ContextKind::Image:
        addSuggestion(QStringLiteral("open_file"), 0.86);
        addSuggestion(QStringLiteral("save_to_inbox"), 0.90);
        break;
    case ContextKind::File:
    case ContextKind::Folder:
        addSuggestion(QStringLiteral("open_file"), 0.88);
        addSuggestion(QStringLiteral("reveal_in_folder"), 0.93);
        addSuggestion(QStringLiteral("save_to_inbox"), 0.72);
        break;
    case ContextKind::Text:
        addSuggestion(QStringLiteral("save_to_inbox"), 0.78, item.sensitivity == SensitivityLevel::Sensitive);
        if (looksLikeExistingPath(item.summary)) {
            addSuggestion(QStringLiteral("reveal_in_folder"), 0.64);
        }
        break;
    case ContextKind::Unknown:
    default:
        addSuggestion(QStringLiteral("save_to_inbox"), 0.55);
        break;
    }

    for (const SuggestionCard& card : m_suggestions) {
        if (card.contextId == item.id) {
            if (!m_history.isEmpty()) {
                m_history.front().suggestedActions << card.actionId;
            }
        }
    }

    emit suggestionsChanged();
    if (!m_suggestions.isEmpty()) {
        emit notificationRequested(QStringLiteral("Barrier Suggestions"),
                                   QStringLiteral("New workflow suggestions are ready for %1.").arg(item.summary));
    }
}

void WorkflowStore::pruneHistory()
{
    const int historyLimit = qMax(10, m_appConfig->getWorkflowHistoryLimit());

    while (m_history.size() > historyLimit || totalHistoryBytes() > kDefaultMetadataBudgetBytes) {
        removeManagedAssets(m_history.back());
        m_history.removeLast();
    }
}

void WorkflowStore::pruneSuggestions()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int i = m_suggestions.size() - 1; i >= 0; --i) {
        if (m_suggestions.at(i).expiresAt < now || contextById(m_suggestions.at(i).contextId) == nullptr) {
            m_suggestions.removeAt(i);
        }
    }

    while (m_suggestions.size() > kSuggestionLimit) {
        m_suggestions.removeLast();
    }
}

void WorkflowStore::pruneReceipts()
{
    while (m_receipts.size() > kReceiptLimit) {
        m_receipts.removeLast();
    }
}

void WorkflowStore::prunePreviewAssets()
{
    while (totalPreviewBytes() > kDefaultPreviewBudgetBytes) {
        bool removedPreview = false;
        for (int i = m_history.size() - 1; i >= 0; --i) {
            ContextItem& item = m_history[i];
            if (!item.previewRef.isEmpty() && item.managedPreview) {
                QFile::remove(item.previewRef);
                item.previewRef.clear();
                item.managedPreview = false;
                removedPreview = true;
                break;
            }
        }

        if (!removedPreview) {
            break;
        }
    }

    int previewCount = 0;
    for (int i = 0; i < m_history.size(); ++i) {
        ContextItem& item = m_history[i];
        if (item.previewRef.isEmpty()) {
            continue;
        }

        ++previewCount;
        if (previewCount > kDefaultPreviewCountLimit && item.managedPreview) {
            QFile::remove(item.previewRef);
            item.previewRef.clear();
            item.managedPreview = false;
        }
    }
}

void WorkflowStore::removeManagedAssets(const ContextItem& item)
{
    if (item.managedPayload && !item.payloadRef.isEmpty()) {
        QFile::remove(item.payloadRef);
    }

    if (item.managedPreview && !item.previewRef.isEmpty()) {
        QFile::remove(item.previewRef);
    }
}

void WorkflowStore::markActivity(WorkflowRuntimeMode preferredMode)
{
    m_lastActivity = QDateTime::currentDateTimeUtc();
    if (preferredMode == WorkflowRuntimeMode::Burst) {
        m_burstUntil = m_lastActivity.addSecs(kBurstSeconds);
    }
    setRuntimeModeInternal(preferredMode);
}

void WorkflowStore::setRuntimeModeInternal(WorkflowRuntimeMode mode)
{
    if (m_runtimeMode == mode) {
        return;
    }

    m_runtimeMode = mode;
    emit runtimeModeChanged(workflowRuntimeModeText(mode));
}

QString WorkflowStore::newContextId() const
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString WorkflowStore::currentSourceDevice() const
{
    if (!m_peerDeviceHint.isEmpty()) {
        return m_peerDeviceHint;
    }

    return m_appConfig->screenName();
}

QString WorkflowStore::storageFilePath(const QString& subDir, const QString& id, const QString& suffix) const
{
    const QString directory = QDir(m_storageRoot).filePath(subDir);
    QDir().mkpath(directory);
    return QDir(directory).filePath(id + suffix);
}

QString WorkflowStore::writeTextPayload(const QString& id, const QString& text, const QString& suffix)
{
    const QString filePath = storageFilePath(QStringLiteral("payload"), id, suffix);
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }

    file.write(text.toUtf8());
    return filePath;
}

QString WorkflowStore::writeBinaryPayload(const QString& id, const QByteArray& data, const QString& suffix)
{
    const QString filePath = storageFilePath(QStringLiteral("payload"), id, suffix);
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }

    file.write(data);
    return filePath;
}

QString WorkflowStore::writeJsonPayload(const QString& id, const QStringList& paths)
{
    QJsonObject root;
    QJsonArray items;
    for (const QString& path : paths) {
        items.append(path);
    }

    root.insert(QStringLiteral("paths"), items);
    root.insert(QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

    return writeBinaryPayload(
        id,
        QJsonDocument(root).toJson(QJsonDocument::Compact),
        QStringLiteral(".json"));
}

QString WorkflowStore::writePreviewImage(const QString& id, const QImage& image)
{
    const QString filePath = storageFilePath(QStringLiteral("preview"), id, QStringLiteral(".png"));
    const QImage preview = image.scaled(320, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return writeImageFile(filePath, preview) ? filePath : QString();
}

QString WorkflowStore::summarizeText(const QString& text) const
{
    QString summary = text.simplified();
    if (summary.size() > 320) {
        summary = summary.left(317) + QStringLiteral("...");
    }
    return summary;
}

QString WorkflowStore::summarizePaths(const QStringList& paths, bool hasFolders) const
{
    if (paths.isEmpty()) {
        return hasFolders ? QStringLiteral("Folder reference") : QStringLiteral("File reference");
    }

    QFileInfo first(paths.first());
    if (paths.size() == 1) {
        return first.fileName().isEmpty() ? first.absoluteFilePath() : first.fileName();
    }

    return QStringLiteral("%1 and %2 more item(s)")
        .arg(first.fileName().isEmpty() ? first.absoluteFilePath() : first.fileName())
        .arg(paths.size() - 1);
}

qint64 WorkflowStore::approximateMetadataBytes(const ContextItem& item) const
{
    return (item.summary.size() + item.sourceDevice.size() + item.previewRef.size() +
            item.payloadRef.size() + item.mimeType.size()) * 2 + 512;
}

qint64 WorkflowStore::totalHistoryBytes() const
{
    qint64 total = 0;
    for (const ContextItem& item : m_history) {
        total += approximateMetadataBytes(item);
    }
    return total;
}

qint64 WorkflowStore::totalPreviewBytes() const
{
    qint64 total = 0;
    for (const ContextItem& item : m_history) {
        if (!item.previewRef.isEmpty()) {
            total += QFileInfo(item.previewRef).size();
        }
    }
    return total;
}

SensitivityLevel WorkflowStore::detectSensitivity(const QString& text) const
{
    static const QList<QRegularExpression> patterns = {
        QRegularExpression(QStringLiteral("(?i)(api[_-]?key|token|secret|password)\\s*[:=]\\s*\\S+")),
        QRegularExpression(QStringLiteral("(?<!\\d)\\d{6}(?!\\d)")),
        QRegularExpression(QStringLiteral("(?i)-----BEGIN [A-Z ]+PRIVATE KEY-----")),
        QRegularExpression(QStringLiteral("(?i)gh[pousr]_[A-Za-z0-9_]{20,}")),
        QRegularExpression(QStringLiteral("(?i)sk-[A-Za-z0-9]{20,}"))
    };

    for (const QRegularExpression& pattern : patterns) {
        if (pattern.match(text).hasMatch()) {
            return SensitivityLevel::Sensitive;
        }
    }

    return SensitivityLevel::Normal;
}

bool WorkflowStore::looksLikeUrl(const QString& text) const
{
    const QUrl url = QUrl::fromUserInput(text.trimmed());
    return url.isValid() && !url.scheme().isEmpty() &&
           (url.scheme().startsWith(QStringLiteral("http")) ||
            url.scheme() == QStringLiteral("file"));
}

bool WorkflowStore::looksLikeExistingPath(const QString& text) const
{
    const QString candidate = normalizePathLikeText(text);
    if (candidate.isEmpty()) {
        return false;
    }

    if (candidate.startsWith(QStringLiteral("file://"))) {
        const QUrl url(candidate);
        return url.isLocalFile() && QFileInfo(url.toLocalFile()).exists();
    }

    return QFileInfo(candidate).exists();
}
