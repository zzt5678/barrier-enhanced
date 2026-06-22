#include "ActionBus.h"

#include "WorkflowStore.h"

#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
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
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace {

const int kManagedInboxItemLimit = 100;
const qint64 kManagedInboxBudgetBytes = 1024LL * 1024LL * 1024LL;
const int kInboxCopyFileLimit = 5000;
const qint64 kInboxCopyByteLimit = 512LL * 1024LL * 1024LL;
const char kManagedMarkerName[] = ".weave-managed";
const char kManagedMarkerSuffix[] = ".weave-managed";

QString sanitizePathSegment(QString text)
{
    text = text.simplified();
    if (text.isEmpty()) {
        text = QStringLiteral("weave-item");
    }

    text.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    text.replace(QStringLiteral(".."), QStringLiteral("_"));
    return text.left(80);
}

struct CopyBudget {
    int maxItems = kInboxCopyFileLimit;
    qint64 maxBytes = kInboxCopyByteLimit;
    int items = 0;
    qint64 bytes = 0;
    QSet<QString> visitedDirectories;
};

bool failCopy(QString* errorMessage, const QString& message)
{
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
    return false;
}

bool addCopyItem(const QFileInfo& sourceInfo, CopyBudget& budget, QString* errorMessage)
{
    ++budget.items;
    budget.bytes += sourceInfo.size();
    if (budget.items > budget.maxItems) {
        return failCopy(errorMessage, QStringLiteral("Refusing to save more than %1 files to the workflow inbox.")
            .arg(budget.maxItems));
    }
    if (budget.bytes > budget.maxBytes) {
        return failCopy(errorMessage, QStringLiteral("Refusing to save more than %1 MiB to the workflow inbox.")
            .arg(budget.maxBytes / (1024 * 1024)));
    }
    return true;
}

bool copyRecursively(const QString& sourcePath,
                     const QString& destinationPath,
                     CopyBudget& budget,
                     QString* errorMessage)
{
    QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists()) {
        return failCopy(errorMessage, QStringLiteral("Source path does not exist: %1").arg(sourcePath));
    }

    if (sourceInfo.isSymLink()) {
        return failCopy(errorMessage, QStringLiteral("Refusing to save symbolic link to the workflow inbox: %1")
            .arg(sourcePath));
    }

    if (sourceInfo.isDir()) {
        const QString canonicalPath = sourceInfo.canonicalFilePath();
        if (!canonicalPath.isEmpty()) {
            if (budget.visitedDirectories.contains(canonicalPath)) {
                return failCopy(errorMessage, QStringLiteral("Refusing to copy recursive folder: %1").arg(sourcePath));
            }
            budget.visitedDirectories.insert(canonicalPath);
        }

        QDir destinationDir(destinationPath);
        if (!destinationDir.exists() && !QDir().mkpath(destinationPath)) {
            return failCopy(errorMessage, QStringLiteral("Could not create folder: %1").arg(destinationPath));
        }

        QDir sourceDir(sourcePath);
        const QFileInfoList entries = sourceDir.entryInfoList(
            QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System);
        for (const QFileInfo& entry : entries) {
            const QString nextSource = entry.absoluteFilePath();
            const QString nextDestination = QDir(destinationPath).filePath(entry.fileName());
            if (!copyRecursively(nextSource, nextDestination, budget, errorMessage)) {
                return false;
            }
        }
        return true;
    }

    if (!addCopyItem(sourceInfo, budget, errorMessage)) {
        return false;
    }

    QDir().mkpath(QFileInfo(destinationPath).absolutePath());
    if (QFile::exists(destinationPath)) {
        QFile::remove(destinationPath);
    }

    if (!QFile::copy(sourcePath, destinationPath)) {
        QFile::remove(destinationPath);
        return failCopy(errorMessage, QStringLiteral("Could not copy %1 to %2").arg(sourcePath, destinationPath));
    }

    return true;
}

bool copyRecursively(const QString& sourcePath, const QString& destinationPath, QString* errorMessage)
{
    CopyBudget budget;
    return copyRecursively(sourcePath, destinationPath, budget, errorMessage);
}

qint64 pathSize(const QString& path)
{
    QFileInfo info(path);
    if (!info.exists()) {
        return 0;
    }
    if (info.isFile()) {
        return info.size();
    }

    qint64 total = 0;
    QDirIterator it(path, QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo entry = it.fileInfo();
        if (entry.isFile()) {
            total += entry.size();
        }
    }
    return total;
}

QString managedMarkerPathForTarget(const QString& targetPath)
{
    const QFileInfo info(targetPath);
    if (info.isDir()) {
        return QDir(targetPath).filePath(QString::fromLatin1(kManagedMarkerName));
    }
    return info.dir().filePath(QStringLiteral(".%1%2").arg(info.fileName(), QString::fromLatin1(kManagedMarkerSuffix)));
}

bool removePath(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        return true;
    }
    if (info.isDir()) {
        return QDir(path).removeRecursively();
    }
    return QFile::remove(path);
}

struct ManagedInboxItem {
    QString targetPath;
    QString markerPath;
    QDateTime modified;
    qint64 bytes = 0;
};

bool pathIsInside(const QString& rootPath, const QString& path)
{
    const QString root = QDir::cleanPath(rootPath);
    const QString candidate = QDir::cleanPath(path);
    return candidate == root || candidate.startsWith(root + QDir::separator());
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

#if defined(BARRIER_TEST_ENV)
bool ActionBus::testCopyRecursively(const QString& sourcePath,
                                    const QString& destinationPath,
                                    QString* errorMessage)
{
    return copyRecursively(sourcePath, destinationPath, errorMessage);
}
#endif

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
            emit notificationRequested(QStringLiteral("Weave Workflow"), detail);
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
        emit notificationRequested(QStringLiteral("Weave Workflow"),
                                   QStringLiteral("Open the Workflow Hub from the tray or the Weave menu."));
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
        markInboxTargetManaged(destination);
        pruneManagedInbox();
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

    if (savedPath != nullptr && !savedPath->isEmpty()) {
        markInboxTargetManaged(*savedPath);
        pruneManagedInbox();
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

void ActionBus::markInboxTargetManaged(const QString& path)
{
    const QString markerPath = managedMarkerPathForTarget(path);
    QDir().mkpath(QFileInfo(markerPath).absolutePath());
    QFile marker(markerPath);
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        marker.write("weave-managed-inbox-item\n");
        marker.close();
    }
}

void ActionBus::pruneManagedInbox(int maxItems, qint64 maxBytes) const
{
    const QString inboxPath = QDir::cleanPath(m_store->inboxDir());
    if (maxItems < 0) {
        maxItems = kManagedInboxItemLimit;
    }
    if (maxBytes < 0) {
        maxBytes = kManagedInboxBudgetBytes;
    }

    pruneManagedInboxDir(inboxPath, maxItems, maxBytes);
}

void ActionBus::pruneManagedInboxDir(const QString& inboxDir, int maxItems, qint64 maxBytes)
{
    const QString inboxPath = QDir::cleanPath(inboxDir);
    QList<ManagedInboxItem> items;
    QDirIterator it(inboxPath, QDir::Files | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo markerInfo = it.fileInfo();
        QString targetPath;
        if (markerInfo.fileName() == QString::fromLatin1(kManagedMarkerName)) {
            targetPath = markerInfo.dir().absolutePath();
        }
        else if (markerInfo.fileName().startsWith('.') &&
                 markerInfo.fileName().endsWith(QString::fromLatin1(kManagedMarkerSuffix))) {
            QString targetName = markerInfo.fileName().mid(1);
            targetName.chop(QString::fromLatin1(kManagedMarkerSuffix).size());
            targetPath = markerInfo.dir().filePath(targetName);
        }
        else {
            continue;
        }

        if (!pathIsInside(inboxPath, targetPath) || QDir::cleanPath(targetPath) == inboxPath) {
            continue;
        }

        ManagedInboxItem item;
        item.targetPath = targetPath;
        item.markerPath = markerInfo.absoluteFilePath();
        item.modified = markerInfo.lastModified();
        item.bytes = pathSize(targetPath) + markerInfo.size();
        items << item;
    }

    qint64 totalBytes = 0;
    for (const ManagedInboxItem& item : items) {
        totalBytes += item.bytes;
    }

    std::sort(items.begin(), items.end(), [](const ManagedInboxItem& lhs, const ManagedInboxItem& rhs) {
        if (lhs.modified != rhs.modified) {
            return lhs.modified < rhs.modified;
        }
        return lhs.targetPath < rhs.targetPath;
    });

    int itemCount = items.size();
    for (const ManagedInboxItem& item : items) {
        const bool tooManyItems = maxItems > 0 && itemCount > maxItems;
        const bool tooManyBytes = maxBytes > 0 && totalBytes > maxBytes;
        if (!tooManyItems && !tooManyBytes) {
            break;
        }

        const qint64 bytes = item.bytes;
        const bool removedTarget = removePath(item.targetPath);
        if (removedTarget) {
            QFile::remove(item.markerPath);
            --itemCount;
            totalBytes = qMax<qint64>(0, totalBytes - bytes);
        }
    }
}
