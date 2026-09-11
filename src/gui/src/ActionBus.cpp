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
#include <QTemporaryDir>
#include <QThread>
#include <QLockFile>
#include <memory>
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
    const qint64 bytes = sourceInfo.isFile() ? sourceInfo.size() : 0;
    if (budget.items > budget.maxItems) {
        return failCopy(errorMessage, QStringLiteral("Refusing to save more than %1 file/folder entries to the workflow inbox.")
            .arg(budget.maxItems));
    }
    if (bytes < 0 || bytes > budget.maxBytes - budget.bytes) {
        return failCopy(errorMessage, QStringLiteral("Refusing to save more than %1 MiB to the workflow inbox.")
            .arg(budget.maxBytes / (1024 * 1024)));
    }
    budget.bytes += bytes;
    return true;
}

bool copyRecursively(const QString& sourcePath,
                     const QString& destinationPath,
                     CopyBudget& budget,
                     QString* errorMessage,
                     int depth = 0)
{
    if (QThread::currentThread()->isInterruptionRequested()) {
        return failCopy(errorMessage, QStringLiteral("Inbox save cancelled."));
    }
    if (depth > 64) {
        return failCopy(errorMessage, QStringLiteral("Inbox folder nesting exceeds 64 levels."));
    }
    QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists()) {
        return failCopy(errorMessage, QStringLiteral("Source path does not exist: %1").arg(sourcePath));
    }

    if (sourceInfo.isSymLink()) {
        return failCopy(errorMessage, QStringLiteral("Refusing to save symbolic link to the workflow inbox: %1")
            .arg(sourcePath));
    }

    if (!sourceInfo.isDir() && !sourceInfo.isFile()) {
        return failCopy(errorMessage, QStringLiteral("Refusing non-regular source: %1").arg(sourcePath));
    }
    if (!addCopyItem(sourceInfo, budget, errorMessage)) {
        return false;
    }
    const QFileInfo destinationInfo(destinationPath);
    if (destinationInfo.exists() || destinationInfo.isSymLink()) {
        return failCopy(errorMessage, QStringLiteral("Destination already exists: %1").arg(destinationPath));
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

        QDirIterator entries(sourcePath,
            QDir::NoDotAndDotDot | QDir::AllEntries | QDir::Hidden | QDir::System);
        while (entries.hasNext()) {
            entries.next();
            const QFileInfo entry = entries.fileInfo();
            const QString nextSource = entry.absoluteFilePath();
            const QString nextDestination = QDir(destinationPath).filePath(entry.fileName());
            if (!copyRecursively(nextSource, nextDestination, budget, errorMessage, depth + 1)) {
                return false;
            }
        }
        if (!QFile::setPermissions(destinationPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
            return failCopy(errorMessage, QStringLiteral("Could not protect inbox folder permissions."));
        }
        return true;
    }

    QDir().mkpath(QFileInfo(destinationPath).absolutePath());
    QFile source(sourcePath);
    QFile destination(destinationPath);
    if (!source.open(QIODevice::ReadOnly) ||
        !destination.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return failCopy(errorMessage, QStringLiteral("Could not copy %1 to %2").arg(sourcePath, destinationPath));
    }
    qint64 copied = 0;
    while (!source.atEnd()) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            return failCopy(errorMessage, QStringLiteral("Inbox save cancelled."));
        }
        const QByteArray chunk = source.read(256 * 1024);
        if (chunk.isEmpty() || chunk.size() > sourceInfo.size() - copied ||
            destination.write(chunk) != chunk.size()) {
            return failCopy(errorMessage, QStringLiteral("Read/write failed or source grew during inbox copy."));
        }
        copied += chunk.size();
    }
    if (!destination.flush()) {
        return failCopy(errorMessage, QStringLiteral("Could not flush inbox output."));
    }
    destination.close();
    if (!QFile::setPermissions(destinationPath, sourceInfo.permissions())) {
        return failCopy(errorMessage, QStringLiteral("Could not preserve file permissions."));
    }

    const QFileInfo after(sourcePath);
    if (after.isSymLink() || after.size() != sourceInfo.size() ||
        after.lastModified() != sourceInfo.lastModified() ||
        QFileInfo(destinationPath).size() != sourceInfo.size()) {
        return failCopy(errorMessage, QStringLiteral("Source changed during copy: %1").arg(sourcePath));
    }

    return true;
}

bool copyRecursively(const QString& sourcePath, const QString& destinationPath, QString* errorMessage)
{
    CopyBudget budget;
    return copyRecursively(sourcePath, destinationPath, budget, errorMessage);
}

// The temporary directory owns every partial output. Publishing stays on the
// same filesystem and never replaces an existing destination.
bool copyInboxBatch(const QStringList& paths, const QString& destination,
                    QString* errorMessage, int maxItems = kInboxCopyFileLimit,
                    qint64 maxBytes = kInboxCopyByteLimit)
{
    if (paths.isEmpty()) {
        return failCopy(errorMessage, QStringLiteral("The file manifest is empty."));
    }
    const QDir inbox = QFileInfo(destination).dir();
    const QString lockPath = inbox.filePath(".weave-inbox-copy.lock");
    if (QFileInfo(lockPath).isSymLink()) {
        return failCopy(errorMessage, QStringLiteral("Invalid inbox lock path."));
    }
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0); // only a dead owner may be recovered, never a slow copy
    if (!lock.tryLock()) {
        return failCopy(errorMessage, QStringLiteral("Another inbox save is active."));
    }
    // Only reclaim directories created by this transaction schema, while the
    // process-wide inbox lock proves that no other writer is using them.
    QDirIterator abandoned(inbox.path(), {".weave-staging-v1-*"},
                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    while (abandoned.hasNext()) {
        abandoned.next();
        const QFileInfo entry = abandoned.fileInfo();
        if (entry.isSymLink()) { continue; }
        // A user can save a directory whose name resembles our staging prefix.
        // A published item's sidecar takes precedence over staging recovery.
        if (QFileInfo::exists(inbox.filePath(QStringLiteral(".%1.weave-managed").arg(entry.fileName())))) {
            continue;
        }
        QFile marker(QDir(entry.absoluteFilePath()).filePath("owner"));
        if (QFileInfo(marker).isSymLink() || marker.size() != 23 || !marker.open(QIODevice::ReadOnly) ||
            marker.readAll() != QByteArray("weave-inbox-staging-v1\n")) { continue; }
        marker.close();
        if (!QDir(entry.absoluteFilePath()).removeRecursively()) {
            return failCopy(errorMessage, QStringLiteral("Could not reclaim interrupted inbox save."));
        }
    }
    QTemporaryDir staging(inbox.filePath(".weave-staging-v1-XXXXXX"));
    if (!staging.isValid()) {
        return failCopy(errorMessage, QStringLiteral("Could not create inbox staging folder."));
    }
    QFile owner(staging.filePath("owner"));
    const QByteArray ownership("weave-inbox-staging-v1\n");
    if (!owner.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        owner.write(ownership) != ownership.size() || !owner.flush()) {
        return failCopy(errorMessage, QStringLiteral("Could not register inbox staging ownership."));
    }
    owner.close();
    const QString output = staging.filePath("output");
    CopyBudget budget;
    budget.maxItems = maxItems;
    budget.maxBytes = maxBytes;
    QSet<QString> names;
    if (paths.size() > 1 && !QDir().mkpath(output)) {
        return failCopy(errorMessage, QStringLiteral("Could not create batch folder."));
    }
    for (const QString& path : paths) {
        const QString base = QFileInfo(path).fileName();
        if (base.isEmpty() || base == "." || base == "..") {
            return failCopy(errorMessage, QStringLiteral("Invalid source name."));
        }
        QString name = base;
        const QFileInfo sourceInfo(path);
        const QString extension = sourceInfo.isFile() && !sourceInfo.suffix().isEmpty()
            ? QStringLiteral(".") + sourceInfo.suffix() : QString();
        const QString stem = extension.isEmpty() ? base : sourceInfo.completeBaseName();
        int index = 1;
        while (names.contains(name.toCaseFolded())) {
            name = QStringLiteral("%1-%2%3").arg(stem).arg(index++).arg(extension);
        }
        names.insert(name.toCaseFolded());
        if (!copyRecursively(path, paths.size() > 1 ? QDir(output).filePath(name) : output,
                             budget, errorMessage)) {
            return false;
        }
    }
    if (QThread::currentThread()->isInterruptionRequested()) {
        return failCopy(errorMessage, QStringLiteral("Inbox save cancelled."));
    }
    if (QFileInfo(destination).exists() || QFileInfo(destination).isSymLink() ||
        !QDir().rename(output, destination)) {
        return failCopy(errorMessage, QStringLiteral("Could not publish inbox item: %1").arg(destination));
    }
    return true;
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
    // Keep metadata outside user payloads, including directories that contain
    // a legitimate file called .weave-managed.
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
    auto normalize = [](const QString& value) {
        QString normalized = QDir::cleanPath(value);
        normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
#if defined(Q_OS_WIN)
        normalized = normalized.toCaseFolded();
#endif
        return normalized;
    };

    const QString root = normalize(rootPath);
    const QString candidate = normalize(path);
    return candidate == root || candidate.startsWith(root + QLatin1Char('/'));
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

ActionBus::~ActionBus()
{
    if (m_inboxWorker != nullptr) {
        disconnect(m_inboxWorker, nullptr, this, nullptr);
        m_inboxWorker->requestInterruption();
        m_inboxWorker->wait();
        delete m_inboxWorker;
    }
}

void ActionBus::cancelInboxSave()
{
    if (m_inboxWorker != nullptr) { m_inboxWorker->requestInterruption(); }
}

#if defined(BARRIER_TEST_ENV)
bool ActionBus::testCopyRecursively(const QString& sourcePath,
                                    const QString& destinationPath,
                                    QString* errorMessage)
{
    return copyRecursively(sourcePath, destinationPath, errorMessage);
}

bool ActionBus::testPathIsInside(const QString& rootPath, const QString& path)
{
    return pathIsInside(rootPath, path);
}

bool ActionBus::testCopyInboxBatch(const QStringList& paths, const QString& destination,
                                 QString* error, int maxItems, qint64 maxBytes)
{
    return copyInboxBatch(paths, destination, error, maxItems, maxBytes);
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

    if (actionId == QStringLiteral("open_link") || actionId == QStringLiteral("open_file")) {
        success = openContext(*item, anchor, &detail);
    }
    else if (actionId == QStringLiteral("reveal_in_folder")) {
        success = revealContext(*item, &detail);
    }
    else if (actionId == QStringLiteral("save_to_inbox")) {
        // true means accepted; the completion receipt is emitted by the worker.
        return saveContextToInbox(*item, nullptr, errorMessage);
    }
    else if (actionId == QStringLiteral("capture_screenshot")) {
        return captureScreenshot(&createdContextId, errorMessage);
    }
    else if (actionId == QStringLiteral("launch_allowed_app")) {
        detail = QStringLiteral("No executable allowlist is configured for this action.");
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
    if (m_inboxWorker != nullptr) {
        return failCopy(errorMessage, QStringLiteral("An inbox save is already running; wait or cancel it."));
    }
    QDir inbox(m_store->inboxDir());
    if (!inbox.exists() && !QDir().mkpath(inbox.path())) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not create the workflow inbox.");
        }
        return false;
    }

    QStringList paths;
    QString destination;
    if (item.kind == ContextKind::Text || item.kind == ContextKind::Link || item.kind == ContextKind::Image) {
        const QString payloadPath = m_store->payloadPath(item.id);
        if (payloadPath.isEmpty()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("The item has no payload to save.");
            }
            return false;
        }

        const QFileInfo payloadInfo(payloadPath);
        destination = uniqueInboxTarget(sanitizePathSegment(item.summary), QStringLiteral(".") + payloadInfo.suffix());
        paths = QStringList{payloadPath};
    }
    else {
        paths = manifestPathsForContext(item);
        destination = paths.size() > 1
            ? uniqueInboxTarget(sanitizePathSegment(item.summary))
            : paths.isEmpty() ? QString() : uniqueInboxTarget(QFileInfo(paths.first()).fileName());
    }
    if (paths.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("The file manifest is empty.");
        }
        return false;
    }

    if (savedPath != nullptr) { savedPath->clear(); }
    struct Result { bool success = false; QString detail; };
    const auto result = std::make_shared<Result>();
    const QString contextId = item.id;
    const QString inboxPath = inbox.path();
    m_inboxWorker = QThread::create([paths, destination, inboxPath, result]() {
        try {
            result->success = copyInboxBatch(paths, destination, &result->detail);
            if (result->success) {
                if (!markInboxTargetManaged(destination)) {
                    result->success = false;
                    result->detail = QStringLiteral("Saved to %1, but could not register inbox metadata; file retained.")
                        .arg(destination);
                    return;
                }
                pruneManagedInboxDir(inboxPath, kManagedInboxItemLimit, kManagedInboxBudgetBytes);
                result->detail = destination;
            }
        }
        catch (const std::exception&) {
            result->success = false;
            result->detail = QStringLiteral("Inbox worker failed.");
        }
    });
    connect(m_inboxWorker, &QThread::finished, this, [this, contextId, result]() {
        m_inboxWorker->deleteLater();
        m_inboxWorker = nullptr;
        const QString status = result->success ? QStringLiteral("Completed")
            : result->detail == QStringLiteral("Inbox save cancelled.")
                ? QStringLiteral("Cancelled") : QStringLiteral("Failed");
        m_store->recordReceipt(contextId, QStringLiteral("save_to_inbox"), status, result->detail);
        emit inboxSaveFinished(contextId, result->success, result->detail);
    });
    m_store->recordReceipt(contextId, QStringLiteral("save_to_inbox"), QStringLiteral("Queued"),
                          QStringLiteral("Saving selected context to inbox…"), false);
    m_inboxWorker->start();
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

    if (!m_store->queueImageContext(pixmap.toImage(), true, contextId)) {
        return failCopy(errorMessage, QStringLiteral("Image worker is busy or screenshot exceeds the image budget."));
    }
    return true;
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
    while (QFileInfo::exists(candidate) || QFileInfo(candidate).isSymLink() ||
           QFileInfo::exists(managedMarkerPathForTarget(candidate))) {
        candidate = inbox.filePath(QStringLiteral("%1-%2%3").arg(baseName).arg(copyIndex++).arg(suffix));
    }
    return candidate;
}

bool ActionBus::markInboxTargetManaged(const QString& path)
{
    const QString markerPath = managedMarkerPathForTarget(path);
    QDir().mkpath(QFileInfo(markerPath).absolutePath());
    QFile marker(markerPath);
    if (marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        const QByteArray magic("weave-managed-inbox-item\n");
        return marker.write(magic) == magic.size() && marker.flush();
    }
    return false;
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
    QDirIterator it(inboxPath, QDir::Files | QDir::Hidden | QDir::System);
    while (it.hasNext()) {
        it.next();
        const QFileInfo markerInfo = it.fileInfo();
        QString targetPath;
        if (markerInfo.fileName().startsWith('.') &&
                 markerInfo.fileName().endsWith(QString::fromLatin1(kManagedMarkerSuffix))) {
            QString targetName = markerInfo.fileName().mid(1);
            targetName.chop(QString::fromLatin1(kManagedMarkerSuffix).size());
            targetPath = markerInfo.dir().filePath(targetName);
        }
        else {
            continue;
        }

        QFile marker(markerInfo.absoluteFilePath());
        const QByteArray magic("weave-managed-inbox-item\n");
        if (markerInfo.isSymLink() || markerInfo.size() != magic.size() ||
            !marker.open(QIODevice::ReadOnly) || marker.readAll() != magic ||
            QFileInfo(targetPath).isSymLink()) {
            continue;
        }
        marker.close();

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
