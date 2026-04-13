#include "ActionBus.h"

#include "WorkflowStore.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QUrl>

namespace {

QString sanitizePathSegment(QString text)
{
    text = text.simplified();
    if (text.isEmpty()) {
        text = QStringLiteral("barrier-item");
    }

    text.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    text.replace(QStringLiteral(".."), QStringLiteral("_"));
    return text.left(80);
}

bool copyRecursively(const QString& sourcePath, const QString& destinationPath, QString* errorMessage)
{
    QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Source path does not exist: %1").arg(sourcePath);
        }
        return false;
    }

    if (sourceInfo.isDir()) {
        QDir destinationDir(destinationPath);
        if (!destinationDir.exists() && !QDir().mkpath(destinationPath)) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("Could not create folder: %1").arg(destinationPath);
            }
            return false;
        }

        QDir sourceDir(sourcePath);
        const QFileInfoList entries = sourceDir.entryInfoList(
            QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System);
        for (const QFileInfo& entry : entries) {
            const QString nextSource = entry.absoluteFilePath();
            const QString nextDestination = QDir(destinationPath).filePath(entry.fileName());
            if (!copyRecursively(nextSource, nextDestination, errorMessage)) {
                return false;
            }
        }
        return true;
    }

    QDir().mkpath(QFileInfo(destinationPath).absolutePath());
    if (QFile::exists(destinationPath)) {
        QFile::remove(destinationPath);
    }

    if (!QFile::copy(sourcePath, destinationPath)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not copy %1 to %2").arg(sourcePath, destinationPath);
        }
        return false;
    }

    return true;
}

QString explorerArgumentForPath(const QString& path)
{
    QString nativePath = QDir::toNativeSeparators(path);
    nativePath.replace('/', '\\');
    return QStringLiteral("/select,%1").arg(nativePath);
}

} // namespace

ActionBus::ActionBus(WorkflowStore& store, QObject* parent) :
    QObject(parent),
    m_store(&store)
{
}

QStringList ActionBus::capabilities() const
{
    return m_store->capabilities();
}

QList<ActionDescriptor> ActionBus::descriptorsForContext(const ContextItem& item) const
{
    QList<ActionDescriptor> descriptors;
    for (const QString& actionId : item.suggestedActions) {
        ActionDescriptor descriptor;
        descriptor.id = actionId;
        descriptor.label = labelForAction(actionId);
        descriptor.riskLevel = item.sensitivity == SensitivityLevel::Sensitive
            ? QStringLiteral("sensitive")
            : QStringLiteral("normal");
        descriptor.requiresConfirmation = item.sensitivity == SensitivityLevel::Sensitive &&
            (actionId == QStringLiteral("save_to_inbox") || actionId == QStringLiteral("open_link"));
        descriptors << descriptor;
    }
    return descriptors;
}

QString ActionBus::labelForAction(const QString& actionId) const
{
    if (actionId == QStringLiteral("open_link")) {
        return QStringLiteral("Open Link");
    }
    if (actionId == QStringLiteral("open_file")) {
        return QStringLiteral("Open");
    }
    if (actionId == QStringLiteral("reveal_in_folder")) {
        return QStringLiteral("Reveal in Folder");
    }
    if (actionId == QStringLiteral("save_to_inbox")) {
        return QStringLiteral("Save to Inbox");
    }
    if (actionId == QStringLiteral("launch_allowed_app")) {
        return QStringLiteral("Open with Allowed App");
    }
    if (actionId == QStringLiteral("capture_screenshot")) {
        return QStringLiteral("Capture Screenshot");
    }
    if (actionId == QStringLiteral("show_notification")) {
        return QStringLiteral("Show Notification");
    }
    return actionId;
}

QVariantMap ActionBus::getContextSummary(const QString& contextId) const
{
    return m_store->getContextSummary(contextId);
}

QByteArray ActionBus::fetchContextPayload(const QString& contextId) const
{
    return m_store->fetchContextPayload(contextId);
}

bool ActionBus::execute(const QString& actionId,
                        const QString& contextId,
                        QWidget* anchor,
                        QString* errorMessage)
{
    const ContextItem* item = m_store->contextById(contextId);
    if (item == nullptr && actionId != QStringLiteral("capture_screenshot")) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("The selected workflow item no longer exists.");
        }
        return false;
    }

    if (item != nullptr && !confirmIfNeeded(*item, labelForAction(actionId), anchor)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("The action was cancelled.");
        }
        return false;
    }

    bool success = false;
    QString detail;
    QString createdContextId = contextId;

    if (actionId == QStringLiteral("open_link") || actionId == QStringLiteral("open_file") ||
        actionId == QStringLiteral("launch_allowed_app")) {
        success = openContext(*item, anchor, &detail);
    }
    else if (actionId == QStringLiteral("reveal_in_folder")) {
        success = revealContext(*item, &detail);
    }
    else if (actionId == QStringLiteral("save_to_inbox")) {
        success = saveContextToInbox(*item, &detail, &detail);
    }
    else if (actionId == QStringLiteral("capture_screenshot")) {
        success = captureScreenshot(&createdContextId, &detail);
    }
    else if (actionId == QStringLiteral("show_notification")) {
        success = true;
        detail = item != nullptr ? item->summary : QStringLiteral("Workflow notification");
    }

    if (success) {
        if (actionId == QStringLiteral("show_notification")) {
            emit notificationRequested(QStringLiteral("Barrier Workflow"), detail);
        }

        m_store->recordReceipt(createdContextId,
                               actionId,
                               QStringLiteral("Completed"),
                               detail);
        return true;
    }

    if (errorMessage != nullptr && errorMessage->isEmpty()) {
        *errorMessage = detail;
    }

    m_store->recordReceipt(createdContextId,
                           actionId,
                           QStringLiteral("Failed"),
                           detail.isEmpty() ? QStringLiteral("The action could not be completed.") : detail);
    return false;
}

bool ActionBus::executeQuickCommand(const QString& commandId,
                                    QWidget* anchor,
                                    QString* errorMessage)
{
    const QList<ContextItem>& history = m_store->history();
    const QString latestContextId = history.isEmpty() ? QString() : history.first().id;

    if (commandId == QStringLiteral("open_workflow_hub")) {
        emit notificationRequested(QStringLiteral("Barrier Workflow"),
                                   QStringLiteral("Open the Workflow Hub from the tray or the Barrier menu."));
        return true;
    }

    if (commandId == QStringLiteral("capture_screenshot")) {
        return execute(QStringLiteral("capture_screenshot"), QString(), anchor, errorMessage);
    }

    if (latestContextId.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("There is no recent workflow item to act on.");
        }
        return false;
    }

    if (commandId == QStringLiteral("save_latest_to_inbox")) {
        return execute(QStringLiteral("save_to_inbox"), latestContextId, anchor, errorMessage);
    }

    if (commandId == QStringLiteral("open_latest")) {
        const ContextItem* item = m_store->contextById(latestContextId);
        if (item == nullptr) {
            return false;
        }
        if (item->kind == ContextKind::Link) {
            return execute(QStringLiteral("open_link"), latestContextId, anchor, errorMessage);
        }
        return execute(QStringLiteral("open_file"), latestContextId, anchor, errorMessage);
    }

    if (commandId == QStringLiteral("reveal_latest")) {
        return execute(QStringLiteral("reveal_in_folder"), latestContextId, anchor, errorMessage);
    }

    if (commandId == QStringLiteral("show_latest_suggestion")) {
        const QList<SuggestionCard>& suggestions = m_store->suggestions();
        if (suggestions.isEmpty()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("There are no pending suggestions.");
            }
            return false;
        }

        const SuggestionCard& card = suggestions.first();
        return execute(card.actionId, card.contextId, anchor, errorMessage);
    }

    if (errorMessage != nullptr) {
        *errorMessage = QStringLiteral("Unknown command.");
    }
    return false;
}

bool ActionBus::openContext(const ContextItem& item, QWidget*, QString* errorMessage)
{
    if (item.kind == ContextKind::Link) {
        const QByteArray payload = m_store->fetchContextPayload(item.id);
        const QUrl url = QUrl::fromUserInput(QString::fromUtf8(payload).trimmed());
        if (!url.isValid() || url.isEmpty()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("The saved link is not valid.");
            }
            return false;
        }

        const bool opened = QDesktopServices::openUrl(url);
        if (!opened && errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not open the link.");
        }
        return opened;
    }

    if (item.kind == ContextKind::Image || item.kind == ContextKind::Text) {
        const QString filePath = m_store->payloadPath(item.id);
        const bool opened = !filePath.isEmpty() && QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
        if (!opened && errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not open the saved payload.");
        }
        return opened;
    }

    const QStringList paths = manifestPathsForContext(item);
    if (paths.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("This workflow item does not have an openable path.");
        }
        return false;
    }

    const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(paths.first()));
    if (!opened && errorMessage != nullptr) {
        *errorMessage = QStringLiteral("Could not open %1.").arg(paths.first());
    }
    return opened;
}

bool ActionBus::revealContext(const ContextItem& item, QString* errorMessage)
{
    QString path;
    if (item.kind == ContextKind::File || item.kind == ContextKind::Folder) {
        const QStringList paths = manifestPathsForContext(item);
        if (!paths.isEmpty()) {
            path = paths.first();
        }
    }
    else {
        path = m_store->payloadPath(item.id);
    }

    if (path.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Nothing is available to reveal.");
        }
        return false;
    }

#if defined(Q_OS_WIN)
    const bool started = QProcess::startDetached(QStringLiteral("explorer.exe"), QStringList{explorerArgumentForPath(path)});
    if (!started && errorMessage != nullptr) {
        *errorMessage = QStringLiteral("Could not open Explorer for %1.").arg(path);
    }
    return started;
#else
    const QFileInfo info(path);
    const QString revealPath = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(revealPath));
    if (!opened && errorMessage != nullptr) {
        *errorMessage = QStringLiteral("Could not reveal %1.").arg(revealPath);
    }
    return opened;
#endif
}

bool ActionBus::saveContextToInbox(const ContextItem& item, QString* savedPath, QString* errorMessage)
{
    QDir inbox(m_store->inboxDir());
    if (!inbox.exists() && !QDir().mkpath(inbox.path())) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not create the workflow inbox.");
        }
        return false;
    }

    if (item.kind == ContextKind::Text || item.kind == ContextKind::Link || item.kind == ContextKind::Image) {
        const QString payloadPath = m_store->payloadPath(item.id);
        if (payloadPath.isEmpty()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("The item has no payload to save.");
            }
            return false;
        }

        const QFileInfo payloadInfo(payloadPath);
        const QString destination = uniqueInboxTarget(sanitizePathSegment(item.summary), QStringLiteral(".") + payloadInfo.suffix());
        if (!copyRecursively(payloadPath, destination, errorMessage)) {
            return false;
        }

        if (savedPath != nullptr) {
            *savedPath = destination;
        }
        return true;
    }

    const QStringList paths = manifestPathsForContext(item);
    if (paths.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("The file manifest is empty.");
        }
        return false;
    }

    const bool multipleSources = paths.size() > 1;
    const QString destinationRoot = multipleSources
        ? uniqueInboxTarget(sanitizePathSegment(item.summary))
        : QString();

    if (multipleSources && !QDir().mkpath(destinationRoot)) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not create inbox folder %1.").arg(destinationRoot);
        }
        return false;
    }

    for (const QString& path : paths) {
        const QFileInfo info(path);
        const QString destination = multipleSources
            ? QDir(destinationRoot).filePath(info.fileName())
            : uniqueInboxTarget(info.fileName());
        if (!copyRecursively(path, destination, errorMessage)) {
            return false;
        }
        if (savedPath != nullptr) {
            *savedPath = multipleSources ? destinationRoot : destination;
        }
    }

    return true;
}

bool ActionBus::captureScreenshot(QString* contextId, QString* errorMessage)
{
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("No active screen is available.");
        }
        return false;
    }

    const QPixmap pixmap = screen->grabWindow(0);
    if (pixmap.isNull()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("The screenshot could not be captured.");
        }
        return false;
    }

    return m_store->addCapturedImageContext(pixmap.toImage(), contextId);
}

bool ActionBus::confirmIfNeeded(const ContextItem& item,
                                const QString& actionLabel,
                                QWidget* anchor) const
{
    if (item.sensitivity != SensitivityLevel::Sensitive) {
        return true;
    }

    const auto result = QMessageBox::question(
        anchor,
        QStringLiteral("Confirm Sensitive Action"),
        QStringLiteral("%1 may expose sensitive content.\n\nDo you want to continue?").arg(actionLabel),
        QMessageBox::Yes | QMessageBox::No);

    return result == QMessageBox::Yes;
}

QStringList ActionBus::manifestPathsForContext(const ContextItem& item) const
{
    QStringList paths;
    const QByteArray payload = m_store->fetchContextPayload(item.id);
    if (payload.isEmpty()) {
        return paths;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) {
        return paths;
    }

    const QJsonArray items = doc.object().value(QStringLiteral("paths")).toArray();
    for (const QJsonValue& value : items) {
        paths << value.toString();
    }
    return paths;
}

QString ActionBus::uniqueInboxTarget(const QString& baseName, const QString& suffix) const
{
    const QDir inbox(m_store->inboxDir());
    QString candidate = inbox.filePath(baseName + suffix);
    int copyIndex = 1;
    while (QFileInfo::exists(candidate)) {
        candidate = inbox.filePath(QStringLiteral("%1-%2%3").arg(baseName).arg(copyIndex++).arg(suffix));
    }
    return candidate;
}
