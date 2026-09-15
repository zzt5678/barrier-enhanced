#include "RuntimeTopologyAdapter.h"
#include <gtest/gtest.h>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QTranslator>
#include <QApplication>

namespace {
using namespace weave::topology;
Snapshot threeDevices(const std::string& viewer = "ubuntu") {
    Snapshot s;
    s.identity = {55, 66, "test-owner", 7};
    s.run = 8; s.sequence = 9; s.updatedMillis = 1000;
    s.localName = "local-alias"; s.localRole = viewer == "ubuntu" ? Role::Server : Role::Client;
    s.ready = true; s.topology.epoch = 10; s.topology.revision = 11;
    s.topology.viewerName = viewer; s.topology.layout = Layout::Available;
    s.topology.nodes = {{"windows", Role::Client, true, -1, 0},
        {"ubuntu", Role::Server, true, 0, 0}, {"mac", Role::Client, false, 1, 0}};
    return s;
}
RuntimeTopologyFallback pendingSettings() {
    RuntimeTopologyFallback f;
    f.localName = "pending-name"; f.serverAddress = "configured.example";
    f.server = true; f.editable = true; f.spatial = true;
    f.devices = {{"pending-name", {4,2}, true, WorkspaceDeviceRole::Server}};
    return f;
}
}

TEST(RuntimeTopologyAdapterTests, CanonicalRuntimeWinsOverPendingSettingsForEveryViewer) {
    for (const std::string& viewer : {"windows", "ubuntu", "mac"}) {
        RuntimeTopologyAdapter adapter;
        auto s = threeDevices(viewer);
        const auto view = adapter.apply(ReadStatus::Fresh, &s, 2000, pendingSettings());
        ASSERT_EQ(3, view.devices.size());
        EXPECT_EQ(QString("windows"), view.devices[0].name);
        EXPECT_EQ(QPoint(-1,0), view.devices[0].position);
        EXPECT_EQ(QPoint(0,0), view.devices[1].position);
        EXPECT_EQ(QPoint(1,0), view.devices[2].position);
        EXPECT_EQ(WorkspaceDeviceRole::Server, view.devices[1].role);
        for (const auto& device : view.devices)
            EXPECT_EQ(device.name == QString::fromStdString(viewer), device.local);
        EXPECT_EQ(viewer == "ubuntu", view.canArrange);
        EXPECT_EQ(WorkspaceDeviceState::Connected, view.devices[0].state);
        EXPECT_EQ(WorkspaceDeviceState::Disconnected, view.devices[2].state);
    }
}

TEST(RuntimeTopologyAdapterTests, ExpiryMissingInvalidAndNotReadyRemoveOnlineWithoutMovingHistory) {
    for (auto status : {ReadStatus::Stale, ReadStatus::Missing, ReadStatus::Invalid,
                        ReadStatus::WrongOwner, ReadStatus::NotReady}) {
        RuntimeTopologyAdapter adapter; auto s = threeDevices();
        adapter.apply(ReadStatus::Fresh, &s, 2000, pendingSettings());
        if (status == ReadStatus::NotReady) s.ready = false;
        const bool hasEvidence = status == ReadStatus::Stale || status == ReadStatus::NotReady;
        const auto view = adapter.apply(status, hasEvidence ? &s : nullptr, 9000, pendingSettings());
        ASSERT_EQ(3, view.devices.size());
        EXPECT_TRUE(view.historical);
        for (const auto& device : view.devices) EXPECT_NE(WorkspaceDeviceState::Connected, device.state);
        EXPECT_EQ(QPoint(-1,0), view.devices[0].position);
    }
    RuntimeTopologyAdapter adapter; auto s = threeDevices();
    adapter.apply(ReadStatus::Fresh, &s, 2000, pendingSettings());
    const auto expired = adapter.apply(ReadStatus::Fresh, &s, 9000, pendingSettings());
    EXPECT_TRUE(expired.historical); // Re-reading an unchanged sequence cannot extend freshness.
}

TEST(RuntimeTopologyAdapterTests, NewRunAndServerEpochCannotInheritOldTopology) {
    RuntimeTopologyAdapter adapter; auto s = threeDevices();
    adapter.apply(ReadStatus::Fresh, &s, 2000, pendingSettings());
    ++s.run; s.ready = false; s.topology = {}; s.localName = "new-client"; s.localRole = Role::Client;
    auto view = adapter.apply(ReadStatus::NotReady, &s, 2000, pendingSettings());
    ASSERT_EQ(1, view.devices.size());
    EXPECT_EQ(QString("new-client"), view.devices[0].name);
    EXPECT_EQ(WorkspaceDeviceRole::Client, view.devices[0].role);
    EXPECT_FALSE(view.canArrange);
    adapter.reset();
    view = adapter.apply(ReadStatus::Missing, nullptr, 2000, pendingSettings());
    ASSERT_EQ(1, view.devices.size());
    EXPECT_EQ(QString("pending-name"), view.devices[0].name);
    EXPECT_EQ(WorkspaceDeviceState::Configured, view.devices[0].state);
}

TEST(RuntimeTopologyAdapterTests, UnavailableGraphUsesNamedListAndOldPeerShowsOnlyLocalClient) {
    RuntimeTopologyAdapter adapter; auto s = threeDevices("windows");
    s.topology.layout = Layout::Contradictory;
    for (auto& n : s.topology.nodes) n.x = n.y = 0;
    auto view = adapter.apply(ReadStatus::Fresh, &s, 2000, pendingSettings());
    EXPECT_FALSE(view.spatial); EXPECT_FALSE(view.layoutReason.isEmpty());
    ASSERT_EQ(3, view.devices.size());
    s.topology = {}; s.localName = "windows"; s.localRole = Role::Client;
    view = adapter.apply(ReadStatus::NotReady, &s, 2000, pendingSettings());
    ASSERT_EQ(1, view.devices.size());
    EXPECT_FALSE(view.spatial); EXPECT_FALSE(view.canArrange);
    EXPECT_TRUE(view.status.contains("configured.example"));
    EXPECT_NE(WorkspaceDeviceState::Connected, view.devices.front().state);
}

#if !defined(Q_OS_WIN)
TEST(RuntimeTopologyAdapterTests, ProtectedReaderResultDrivesFreshStaleAndMalformedRendering) {
    QTemporaryDir directory; ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(QFile::setPermissions(directory.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    auto s = threeDevices(); s.identity = currentProcessIdentity();
    ASSERT_NE(0u, s.identity.pid); s.updatedMillis = monotonicMillis();
    std::string encoded; ASSERT_TRUE(encodeSnapshot(s, encoded));
    QFile file(directory.filePath(QString::fromStdString(snapshotFilename(s.identity))));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_TRUE(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    ASSERT_EQ(static_cast<qint64>(encoded.size()), file.write(encoded.data(), encoded.size())); file.close();
    Snapshot read; RuntimeTopologyAdapter adapter;
    auto status = readSnapshot(directory.path().toStdString(), s.identity, s.updatedMillis, read);
    ASSERT_EQ(ReadStatus::Fresh, status);
    auto view = adapter.apply(status, &read, s.updatedMillis, pendingSettings());
    ASSERT_EQ(3, view.devices.size());
    EXPECT_EQ(WorkspaceDeviceState::Connected, view.devices[0].state);
    status = readSnapshot(directory.path().toStdString(), s.identity, s.updatedMillis + FreshnessMillis + 1, read);
    ASSERT_EQ(ReadStatus::Stale, status);
    view = adapter.apply(status, &read, s.updatedMillis + FreshnessMillis + 1, pendingSettings());
    EXPECT_EQ(WorkspaceDeviceState::Stale, view.devices[0].state);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(7, file.write("invalid")); file.close();
    status = readSnapshot(directory.path().toStdString(), s.identity, s.updatedMillis, read);
    ASSERT_EQ(ReadStatus::Invalid, status);
    view = adapter.apply(status, nullptr, s.updatedMillis, pendingSettings());
    EXPECT_TRUE(view.historical);
    EXPECT_EQ(WorkspaceDeviceState::Stale, view.devices[0].state);
}
#endif

TEST(RuntimeTopologyAdapterTests, OwnerGenerationAndEpochReplacementDoNotCarryConnectedFlags) {
    for (int replacement = 0; replacement < 3; ++replacement) {
        RuntimeTopologyAdapter adapter; auto s = threeDevices(); s.ownerGeneration = 8;
        adapter.apply(ReadStatus::Fresh, &s, 2000, pendingSettings());
        if (replacement == 0) ++s.identity.birth;
        if (replacement == 1) ++s.ownerGeneration;
        if (replacement == 2) ++s.topology.epoch;
        s.ready = false;
        const auto view = adapter.apply(ReadStatus::NotReady, &s, 2000, pendingSettings());
        for (const auto& device : view.devices) EXPECT_NE(WorkspaceDeviceState::Connected, device.state);
    }
}

TEST(RuntimeTopologyAdapterTests, RuntimeStatusAndListReasonsFollowDisplayLanguage) {
    RuntimeTopologyAdapter adapter; auto s = threeDevices(); s.topology.layout = Layout::Contradictory;
    for (auto& node : s.topology.nodes) node.x = node.y = 0;
    const auto english = adapter.apply(ReadStatus::Fresh, &s, 2000, pendingSettings());
    QTranslator translator; ASSERT_TRUE(translator.load(":/res/lang/gui_zh-CN.qm"));
    QApplication::installTranslator(&translator);
    const auto chinese = adapter.apply(ReadStatus::Fresh, &s, 2000, pendingSettings());
    EXPECT_NE(english.status, chinese.status);
    EXPECT_NE(english.layoutReason, chinese.layoutReason);
    EXPECT_EQ(english.devices[0].name, chinese.devices[0].name);
    QApplication::removeTranslator(&translator);
}
