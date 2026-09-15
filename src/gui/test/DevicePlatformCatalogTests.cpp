#include "DevicePlatformCatalog.h"
#include <gtest/gtest.h>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QTemporaryDir>
#ifndef Q_OS_WIN
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
bool writeFixture(const QString& path,const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly|QIODevice::Truncate) && file.write(bytes)==bytes.size();
}
QByteArray catalogBytes(const QJsonObject& devices) {
    return QJsonDocument(QJsonObject{{"version",1},{"devices",devices}}).toJson(QJsonDocument::Compact);
}
}

TEST(DevicePlatformCatalogTests, NormalizesOnlyActuallyIdentifiedProducts) {
    const struct { const char* input; const char* expected; } cases[]={
        {"ubuntu","ubuntu"},{"windows","windows"},{"osx","macos"},{"macos","macos"},
        {"debian","unknown"},{"linux","unknown"},{"","unknown"},{"unknown","unknown"},
        {"Ubuntu-laptop","unknown"},{"darwin","unknown"}
    };
    for(const auto& item:cases)
        EXPECT_EQ(QString::fromLatin1(item.expected),DevicePlatformCatalog::normalizeProductType(QString::fromLatin1(item.input)));
}

TEST(DevicePlatformCatalogTests, UsesExactCanonicalMappingsWithoutNameGuesses) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid()); const auto path=dir.filePath("platforms.json");
    ASSERT_TRUE(writeFixture(path,R"({"version":1,"devices":{"alpha":"windows","Beta":"macos","第三台":"ubuntu"}})"));
    DevicePlatformCatalog catalog; ASSERT_TRUE(catalog.load(path));
    EXPECT_EQ(QString("windows"),catalog.resolve("alpha",false));
    EXPECT_EQ(QString("macos"),catalog.resolve("Beta",false));
    EXPECT_EQ(QString("ubuntu"),catalog.resolve(QString::fromUtf8("第三台"),false));
    for(const auto& name:{"beta","prefix-alpha","windows","ubuntu","macbook",""})
        EXPECT_EQ(QString("unknown"),catalog.resolve(name,false));
    QFile::remove(path);
    EXPECT_EQ(QString("windows"),catalog.resolve("alpha",false)); // Cached, no timer disk I/O.
}

TEST(DevicePlatformCatalogTests, LocalDetectionOverridesEvenConflictingInventory) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid()); const auto path=dir.filePath("platforms.json");
    const auto actual=DevicePlatformCatalog::normalizeProductType(QSysInfo::productType());
    const auto contradictory=actual=="windows"?QString("ubuntu"):QString("windows");
    ASSERT_TRUE(writeFixture(path,catalogBytes(QJsonObject{{"local",contradictory}})));
    DevicePlatformCatalog catalog; ASSERT_TRUE(catalog.load(path));
    EXPECT_EQ(actual,catalog.resolve("local",true));
    EXPECT_EQ(actual,catalog.resolve("unlisted-local-name",true));
    EXPECT_EQ(contradictory,catalog.resolve("local",false));
}

TEST(DevicePlatformCatalogTests, MissingOrMalformedLoadDropsPreviousRemoteMetadata) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid()); const auto path=dir.filePath("platforms.json");
    const QByteArray invalid[]={"{",R"([])",R"({"version":2,"devices":{}})",
        R"({"version":true,"devices":{}})",R"({"version":1,"devices":[]})",
        R"({"version":1,"devices":{"alpha":"linux"}})",R"({"version":1,"devices":{"alpha":3}})",
        R"({"version":1,"devices":{"":"windows"}})",R"({"version":1,"devices":{"a\nb":"windows"}})",
        R"({"version":1,"devices":{"alpha":"windows"},"command":"unused"})",
        R"({"version":1,"version":1,"devices":{}})",
        R"({"version":1,"devices":{"alpha":"windows","alpha":"macos"}})",
        R"({"version":1,"devices":{"\ud800":"windows"}})"};
    DevicePlatformCatalog catalog;
    for(const auto& bytes:invalid) {
        ASSERT_TRUE(writeFixture(path,R"({"version":1,"devices":{"alpha":"windows"}})"));
        ASSERT_TRUE(catalog.load(path)); ASSERT_TRUE(writeFixture(path,bytes));
        EXPECT_FALSE(catalog.load(path))<<bytes.constData();
        EXPECT_EQ(QString("unknown"),catalog.resolve("alpha",false));
    }
    ASSERT_TRUE(writeFixture(path,R"({"version":1,"devices":{"alpha":"windows"}})"));
    ASSERT_TRUE(catalog.load(path)); QFile::remove(path);
    EXPECT_FALSE(catalog.load(path)); EXPECT_EQ(QString("unknown"),catalog.resolve("alpha",false));
}

TEST(DevicePlatformCatalogTests, EnforcesByteNodeAndNameBoundsAndRejectsInvalidUtf8) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid()); const auto path=dir.filePath("platforms.json");
    DevicePlatformCatalog catalog; QJsonObject devices;
    for(int i=0;i<64;++i) devices.insert(QString::number(i),"windows");
    ASSERT_TRUE(writeFixture(path,catalogBytes(devices))); EXPECT_TRUE(catalog.load(path));
    devices.insert("extra","macos"); ASSERT_TRUE(writeFixture(path,catalogBytes(devices))); EXPECT_FALSE(catalog.load(path));
    ASSERT_TRUE(writeFixture(path,catalogBytes(QJsonObject{{QString(256,'x'),"ubuntu"}}))); EXPECT_TRUE(catalog.load(path));
    ASSERT_TRUE(writeFixture(path,catalogBytes(QJsonObject{{QString(257,'x'),"ubuntu"}}))); EXPECT_FALSE(catalog.load(path));
    ASSERT_TRUE(writeFixture(path,catalogBytes(QJsonObject{{QString(85,QChar(0x4e00)),"macos"}}))); EXPECT_TRUE(catalog.load(path));
    ASSERT_TRUE(writeFixture(path,catalogBytes(QJsonObject{{QString(86,QChar(0x4e00)),"macos"}}))); EXPECT_FALSE(catalog.load(path));
    auto bytes=catalogBytes(QJsonObject{{"alpha","windows"}}); bytes.append(65536-bytes.size(),' ');
    ASSERT_TRUE(writeFixture(path,bytes)); EXPECT_TRUE(catalog.load(path));
    bytes.append(' '); ASSERT_TRUE(writeFixture(path,bytes)); EXPECT_FALSE(catalog.load(path));
    ASSERT_TRUE(writeFixture(path,QByteArray("{\"version\":1,\"devices\":{\"")+char(0xc0)+char(0xaf)+"\":\"windows\"}}"));
    EXPECT_FALSE(catalog.load(path));
    EXPECT_FALSE(catalog.load(dir.path()));
}

#ifndef Q_OS_WIN
TEST(DevicePlatformCatalogTests, RefusesSymlinkAndNonRegularFile) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const auto path=dir.filePath("platforms.json"), link=dir.filePath("link.json");
    ASSERT_TRUE(writeFixture(path,R"({"version":1,"devices":{"alpha":"windows"}})"));
    ASSERT_EQ(0,symlink(QFile::encodeName(path).constData(),QFile::encodeName(link).constData()));
    DevicePlatformCatalog catalog;
    EXPECT_FALSE(catalog.load(link)); EXPECT_EQ(QString("unknown"),catalog.resolve("alpha",false));
    const auto fifo=dir.filePath("not-a-regular-file");
    ASSERT_EQ(0,mkfifo(QFile::encodeName(fifo).constData(),0600));
    EXPECT_FALSE(catalog.load(fifo)); // The native open is nonblocking before fstat.
}
#endif
