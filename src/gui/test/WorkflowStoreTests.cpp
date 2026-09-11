#include "AppConfig.h"
#include "WorkflowStore.h"

#include "gtest/gtest.h"

#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QMimeData>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

namespace {

struct WorkflowStoreFixture {
    WorkflowStoreFixture() :
        settings(settingsDir.filePath(QStringLiteral("weave.ini")), QSettings::IniFormat),
        config(&settings),
        store(config)
    {
        config.setWorkflowEnabled(true);
        config.setSuggestionsEnabled(true);
    }

    QTemporaryDir settingsDir;
    QSettings settings;
    AppConfig config;
    WorkflowStore store;
};

bool directoryContains(const QString& root, const QByteArray& needle)
{
    QDirIterator files(root, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile file(files.next());
        if (file.open(QIODevice::ReadOnly) && file.readAll().contains(needle)) {
            return true;
        }
    }
    return false;
}

QString sensitiveText(const QString& suffix)
{
    return QStringLiteral("password=weave-sensitive-%1-%2")
        .arg(suffix, QUuid::createUuid().toString(QUuid::WithoutBraces));
}

bool waitForImage(WorkflowStore& store)
{
    QElapsedTimer timer;
    timer.start();
    while (store.imageTaskRunning() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return !store.imageTaskRunning();
}

} // namespace

TEST(WorkflowStoreTests, capturedImagePersistsReadablePng)
{
    WorkflowStoreFixture fixture;
    QTemporaryDir storage;
    fixture.store.testSetStorageRoot(storage.path());
    QImage image(64, 48, QImage::Format_ARGB32);
    image.fill(Qt::red);
    QString id;
    ASSERT_TRUE(fixture.store.addCapturedImageContext(image, &id));
    const ContextItem* item = fixture.store.contextById(id);
    ASSERT_NE(nullptr, item);
    EXPECT_TRUE(item->payloadAvailable);
    EXPECT_EQ(image, QImage(item->payloadRef));
    EXPECT_FALSE(QImage(item->previewRef).isNull());
}

TEST(WorkflowStoreTests, transportLogDoesNotClaimClipboardPublication)
{
    WorkflowStoreFixture fixture;
    fixture.store.recordLogLine("INFO: file transfer finished");
    ASSERT_FALSE(fixture.store.receipts().isEmpty());
    EXPECT_EQ(QStringLiteral("Reported"), fixture.store.receipts().first().status);
    fixture.store.recordLogLine("INFO: remote clipboard package materialized: session=test items=1");
    EXPECT_EQ(QStringLiteral("Materialized"), fixture.store.receipts().first().status);
    EXPECT_TRUE(fixture.store.receipts().first().detail.contains("pending"));
}

TEST(WorkflowStoreTests, imageAdmissionRejectsDimensionAndPixelOverflow)
{
    WorkflowStoreFixture fixture;
    QTemporaryDir storage;
    fixture.store.testSetStorageRoot(storage.path());
    const int historySize = fixture.store.history().size();
    QString id = QStringLiteral("stale-id");
    EXPECT_FALSE(fixture.store.addCapturedImageContext(
        QImage(16385, 1, QImage::Format_Grayscale8), &id));
    EXPECT_TRUE(id.isEmpty());
    EXPECT_FALSE(fixture.store.addCapturedImageContext(
        QImage(8192, 4097, QImage::Format_Grayscale8), &id));
    QMimeData oversized;
    oversized.setImageData(QImage(16385, 1, QImage::Format_Grayscale8));
    fixture.store.testConsumeClipboardMime(&oversized);
    ASSERT_FALSE(fixture.store.receipts().isEmpty());
    EXPECT_EQ(QStringLiteral("Skipped"), fixture.store.receipts().first().status);
    EXPECT_EQ(historySize, fixture.store.history().size());
    QImage boundary(16384, 1, QImage::Format_Grayscale8);
    boundary.fill(0);
    EXPECT_TRUE(fixture.store.addCapturedImageContext(boundary, &id));
    EXPECT_FALSE(id.isEmpty());
}

TEST(WorkflowStoreTests, imageWriteFailureDoesNotRecordCaptureOrClipboardSuccess)
{
    WorkflowStoreFixture fixture;
    QTemporaryDir storage;
    QFile blockedRoot(storage.filePath(QStringLiteral("not-a-directory")));
    ASSERT_TRUE(blockedRoot.open(QIODevice::WriteOnly));
    blockedRoot.close();
    fixture.store.testSetStorageRoot(blockedRoot.fileName());
    QImage image(32, 32, QImage::Format_ARGB32);
    image.fill(Qt::blue);
    const int historySize = fixture.store.history().size();
    const int receiptSize = fixture.store.receipts().size();
    QString id = QStringLiteral("stale-id");
    EXPECT_FALSE(fixture.store.addCapturedImageContext(image, &id));
    EXPECT_TRUE(id.isEmpty());
    QMimeData mime;
    mime.setImageData(image);
    fixture.store.testConsumeClipboardMime(&mime);
    ASSERT_TRUE(waitForImage(fixture.store));
    EXPECT_EQ(historySize, fixture.store.history().size());
    EXPECT_EQ(receiptSize, fixture.store.receipts().size());
}

TEST(WorkflowStoreTests, incompressibleImageCannotExceedEncodedBudget)
{
    WorkflowStoreFixture fixture;
    QTemporaryDir storage;
    fixture.store.testSetStorageRoot(storage.path());
    QImage image(3072, 3072, QImage::Format_ARGB32);
    quint32 state = 0x12345678;
    for (int y = 0; y < image.height(); ++y) {
        auto* pixels = reinterpret_cast<quint32*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            pixels[x] = state;
        }
    }
    const int historySize = fixture.store.history().size();
    EXPECT_FALSE(fixture.store.addCapturedImageContext(image));
    EXPECT_EQ(historySize, fixture.store.history().size());
    EXPECT_TRUE(QDir(storage.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}

TEST(WorkflowStoreTests, asyncImageQueueIsBoundedAndPublishesOnlyOnCompletion)
{
    WorkflowStoreFixture fixture;
    QTemporaryDir storage;
    fixture.store.testSetStorageRoot(storage.path());
    QImage image(640, 480, QImage::Format_ARGB32);
    image.fill(Qt::green);
    QString id;
    ASSERT_TRUE(fixture.store.queueImageContext(image, true, &id));
    EXPECT_EQ(nullptr, fixture.store.contextById(id));
    EXPECT_EQ(QStringLiteral("Queued"), fixture.store.receipts().first().status);
    EXPECT_FALSE(fixture.store.queueImageContext(image, true));
    ASSERT_TRUE(waitForImage(fixture.store));
    const ContextItem* item = fixture.store.contextById(id);
    ASSERT_NE(nullptr, item);
    EXPECT_EQ(image, QImage(item->payloadRef));
    EXPECT_EQ(QStringLiteral("Captured"), fixture.store.receipts().first().status);
}

TEST(WorkflowStoreTests, delayedClipboardImageDoesNotOverrideNewerText)
{
    WorkflowStoreFixture fixture;
    QTemporaryDir storage;
    fixture.store.testSetStorageRoot(storage.path());
    QImage image(640, 480, QImage::Format_ARGB32);
    image.fill(Qt::green);
    QMimeData first;
    first.setImageData(image);
    fixture.store.testConsumeClipboardMime(&first);
    QMimeData next;
    next.setText("newer clipboard text");
    fixture.store.testConsumeClipboardMime(&next);
    ASSERT_TRUE(waitForImage(fixture.store));
    ASSERT_EQ(1, fixture.store.history().size());
    EXPECT_EQ(ContextKind::Text, fixture.store.history().first().kind);
    EXPECT_TRUE(QDir(storage.filePath("payload")).entryList({"*.png"}, QDir::Files).isEmpty());
}

TEST(WorkflowStoreTests, rapidImagesRetainOnlyTheLatestPendingImage)
{
    WorkflowStoreFixture fixture;
    QTemporaryDir storage;
    fixture.store.testSetStorageRoot(storage.path());
    QImage expected(320, 240, QImage::Format_ARGB32);
    for (int i = 0; i < 3; ++i) {
        expected.fill(i == 0 ? Qt::red : i == 1 ? Qt::green : Qt::blue);
        QMimeData mime;
        mime.setImageData(expected);
        fixture.store.testConsumeClipboardMime(&mime);
    }
    ASSERT_TRUE(waitForImage(fixture.store));
    ASSERT_EQ(1, fixture.store.history().size());
    EXPECT_EQ(expected, QImage(fixture.store.history().first().payloadRef));
    EXPECT_EQ(1, QDir(storage.filePath("payload")).entryList({"*.png"}, QDir::Files).size());
}

TEST(WorkflowStoreTests, destroyingImageWorkerDoesNotLeaveUnadoptedAssets)
{
    QTemporaryDir storage;
    {
        WorkflowStoreFixture fixture;
        fixture.store.testSetStorageRoot(storage.path());
        QImage image(512, 512, QImage::Format_ARGB32);
        image.fill(Qt::red);
        ASSERT_TRUE(fixture.store.queueImageContext(image, true));
        // Do not dispatch completion; destruction must cancel or reclaim it.
    }
    EXPECT_TRUE(QDir(storage.filePath("payload")).entryList(QDir::Files).isEmpty());
    EXPECT_TRUE(QDir(storage.filePath("preview")).entryList(QDir::Files).isEmpty());
}

TEST(WorkflowStoreTests, sensitiveTextIsMemoryOnlyAndDoesNotGenerateSuggestions)
{
    WorkflowStoreFixture fixture;
    const QString secret = sensitiveText(QStringLiteral("memory-only"));

    const QString contextId = fixture.store.testAddTextContext(secret);

    ASSERT_FALSE(contextId.isEmpty());
    const ContextItem* item = fixture.store.contextById(contextId);
    ASSERT_NE(nullptr, item);
    EXPECT_EQ(SensitivityLevel::Sensitive, item->sensitivity);
    EXPECT_TRUE(item->payloadRef.isEmpty());
    EXPECT_FALSE(item->managedPayload);
    EXPECT_EQ(secret.toUtf8(), fixture.store.fetchContextPayload(contextId));
    EXPECT_TRUE(fixture.store.suggestions().isEmpty());
    EXPECT_TRUE(item->suggestedActions.isEmpty());
    EXPECT_GE(item->expiresAt, item->createdAt.addSecs(14 * 60));
    EXPECT_LE(item->expiresAt, item->createdAt.addSecs(16 * 60));
    EXPECT_FALSE(directoryContains(fixture.store.testPayloadDirectory(), secret.toUtf8()));
}

TEST(WorkflowStoreTests, expiredAndRemovedSensitiveTextIsErasedFromMemory)
{
    WorkflowStoreFixture fixture;
    const QString expiredId = fixture.store.testAddTextContext(
        sensitiveText(QStringLiteral("expired")));
    ASSERT_EQ(1, fixture.store.testSensitivePayloadCount());

    fixture.store.testExpireContext(expiredId);
    EXPECT_TRUE(fixture.store.fetchContextPayload(expiredId).isEmpty());
    EXPECT_EQ(0, fixture.store.testSensitivePayloadCount());
    fixture.store.testPruneHistory();
    EXPECT_EQ(nullptr, fixture.store.contextById(expiredId));

    const QString removedId = fixture.store.testAddTextContext(
        sensitiveText(QStringLiteral("removed")));
    ASSERT_EQ(1, fixture.store.testSensitivePayloadCount());
    fixture.store.testRemoveContext(removedId);
    EXPECT_EQ(nullptr, fixture.store.contextById(removedId));
    EXPECT_TRUE(fixture.store.fetchContextPayload(removedId).isEmpty());
    EXPECT_EQ(0, fixture.store.testSensitivePayloadCount());
}

TEST(WorkflowStoreTests, historyPruningAlsoErasesSensitivePayloads)
{
    WorkflowStoreFixture fixture;
    fixture.config.setWorkflowHistoryLimit(10);

    QString oldestId;
    for (int i = 0; i < 11; ++i) {
        const QString id = fixture.store.testAddTextContext(
            sensitiveText(QString::number(i)));
        ASSERT_FALSE(id.isEmpty());
        if (i == 0) {
            oldestId = id;
        }
    }

    EXPECT_EQ(10, fixture.store.history().size());
    EXPECT_EQ(10, fixture.store.testSensitivePayloadCount());
    EXPECT_EQ(nullptr, fixture.store.contextById(oldestId));
    EXPECT_TRUE(fixture.store.fetchContextPayload(oldestId).isEmpty());
}

TEST(WorkflowStoreTests, oversizedSensitiveTextIsNotRetained)
{
    WorkflowStoreFixture fixture;
    const QString marker = sensitiveText(QStringLiteral("oversized"));
    const QString secret = marker + QString(300 * 1024, QLatin1Char('x'));

    const QString contextId = fixture.store.testAddTextContext(secret);

    ASSERT_FALSE(contextId.isEmpty());
    EXPECT_TRUE(fixture.store.fetchContextPayload(contextId).isEmpty());
    EXPECT_EQ(0, fixture.store.testSensitivePayloadCount());
    EXPECT_EQ(0, fixture.store.testSensitivePayloadBytes());
    EXPECT_FALSE(directoryContains(fixture.store.testPayloadDirectory(), marker.toUtf8()));
}

TEST(WorkflowStoreTests, sensitivePayloadCacheHasATotalByteBudget)
{
    WorkflowStoreFixture fixture;
    QString oldestId;
    for (int i = 0; i < 5; ++i) {
        const QString secret = sensitiveText(QString::number(i)) +
            QString(240 * 1024, QLatin1Char('x'));
        const QString id = fixture.store.testAddTextContext(secret);
        ASSERT_FALSE(id.isEmpty());
        if (i == 0) {
            oldestId = id;
        }
    }

    EXPECT_EQ(4, fixture.store.testSensitivePayloadCount());
    EXPECT_LE(fixture.store.testSensitivePayloadBytes(), 1024 * 1024);
    EXPECT_TRUE(fixture.store.fetchContextPayload(oldestId).isEmpty());
}

TEST(WorkflowStoreTests, normalTextRemainsDiskBacked)
{
    WorkflowStoreFixture fixture;
    const QString text = QStringLiteral("ordinary workflow text alpha beta gamma");

    const QString contextId = fixture.store.testAddTextContext(text);

    const ContextItem* item = fixture.store.contextById(contextId);
    ASSERT_NE(nullptr, item);
    EXPECT_EQ(SensitivityLevel::Normal, item->sensitivity);
    EXPECT_FALSE(item->payloadRef.isEmpty());
    EXPECT_TRUE(item->managedPayload);
    EXPECT_EQ(text.toUtf8(), fixture.store.fetchContextPayload(contextId));
    EXPECT_FALSE(fixture.store.suggestions().isEmpty());
    const QString payloadPath = item->payloadRef;
    fixture.store.testRemoveContext(contextId);
    EXPECT_FALSE(QFile::exists(payloadPath));
}

TEST(WorkflowStoreTests, sensitiveRemoteUrlSignatureNeverRetainsPlaintext)
{
    WorkflowStoreFixture fixture;
    const QString secret = QStringLiteral(
        "https://example.invalid/path?token=weave-sensitive-uri-token-1234567890");
    QMimeData mimeData;
    mimeData.setUrls({QUrl(secret)});

    fixture.store.testConsumeClipboardMime(&mimeData);

    ASSERT_EQ(1, fixture.store.history().size());
    EXPECT_EQ(SensitivityLevel::Sensitive,
              fixture.store.history().front().sensitivity);
    EXPECT_FALSE(fixture.store.testLastSignature().contains(secret));
    EXPECT_TRUE(fixture.store.testLastSignature().startsWith(
        QStringLiteral("sensitive:")));
}

TEST(WorkflowStoreTests, sensitivePayloadEvictionUpdatesAvailability)
{
    WorkflowStoreFixture fixture;
    QString oldestId;
    for (int i = 0; i < 5; ++i) {
        const QString secret = sensitiveText(QString::number(i)) +
            QString(240 * 1024, QLatin1Char('x'));
        const QString id = fixture.store.testAddTextContext(secret);
        ASSERT_FALSE(id.isEmpty());
        if (i == 0) {
            oldestId = id;
        }
    }

    const ContextItem* oldest = fixture.store.contextById(oldestId);
    ASSERT_NE(nullptr, oldest);
    EXPECT_FALSE(oldest->payloadAvailable);
    EXPECT_TRUE(fixture.store.fetchContextPayload(oldestId).isEmpty());
}
