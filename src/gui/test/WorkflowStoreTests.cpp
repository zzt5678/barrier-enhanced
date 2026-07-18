#include "AppConfig.h"
#include "WorkflowStore.h"

#include "gtest/gtest.h"

#include <QDirIterator>
#include <QFile>
#include <QMimeData>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>

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

} // namespace

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
