#include "topology/DeviceTopology.h"
#include "topology/TopologySnapshot.h"
#include "test/global/gtest.h"
#include <chrono>
#include <thread>
#include <filesystem>
#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#endif

using namespace weave::topology;

static Topology trio()
{
    Topology t;
    t.revision = 7;
    t.nodes = {{"Ubuntu", Role::Server, true, 0, 0},
               {"Mac", Role::Client, true, -1, 0},
               {"Windows", Role::Client, false, 1, 0}};
    t.layout = Layout::Available;
    return t;
}

TEST(DeviceTopologyTests, RoundTripRetainsRolesAndIndependentRoster)
{
    std::string bytes;
    ASSERT_TRUE(encodeTopology(trio(), bytes));
    Topology parsed;
    ASSERT_TRUE(decodeTopology(bytes, parsed));
    ASSERT_EQ(3u, parsed.nodes.size());
    EXPECT_EQ(Role::Server, parsed.nodes[0].role);
    EXPECT_EQ(-1, parsed.nodes[1].x);
    EXPECT_FALSE(parsed.nodes[2].online);
}

TEST(DeviceTopologyTests, MalformedAndOversizedInputPreservesCaller)
{
    Topology original = trio();
    std::string bytes;
    ASSERT_TRUE(encodeTopology(original, bytes));
    for (size_t i = 0; i < bytes.size(); ++i) {
        Topology out = original;
        EXPECT_FALSE(decodeTopology(bytes.substr(0, i), out));
        EXPECT_EQ(7u, out.revision);
        EXPECT_EQ(3u, out.nodes.size());
    }
    EXPECT_FALSE(decodeTopology(bytes + "x", original));
    EXPECT_FALSE(decodeTopology(std::string(65537, 'x'), original));
}

TEST(DeviceTopologyTests, RejectsDuplicateMalformedUtf8AndBounds)
{
    std::string out = "preserve";
    Topology t = trio();
    t.nodes[1].name = "ubuntu";
    EXPECT_FALSE(encodeTopology(t, out));
    EXPECT_EQ("preserve", out);
    t = trio(); t.nodes[1].name = std::string("\xc0\xaf", 2);
    EXPECT_FALSE(encodeTopology(t, out));
    t = trio(); t.nodes[1].name.assign(257, 'a');
    EXPECT_FALSE(encodeTopology(t, out));
    t = trio(); t.nodes.resize(65);
    EXPECT_FALSE(encodeTopology(t, out));
}

TEST(DeviceTopologyTests, ResolvesDirectedGraphAndFailsClosedOnContradictions)
{
    Topology t = trio();
    std::vector<Edge> edges = {{"Ubuntu", "Mac", -1, 0, true},
                               {"Ubuntu", "Windows", 1, 0, true}};
    resolveLayout(t, edges);
    EXPECT_EQ(Layout::Available, t.layout);
    EXPECT_EQ(-1, t.nodes[1].x);
    EXPECT_EQ(1, t.nodes[2].x);
    edges.push_back({"Mac", "Windows", 1, 0, true});
    resolveLayout(t, edges);
    EXPECT_EQ(Layout::Contradictory, t.layout);
    edges.pop_back(); edges[0].whole = false;
    resolveLayout(t, edges);
    EXPECT_EQ(Layout::PartialEdge, t.layout);
    edges.clear();
    resolveLayout(t, edges);
    EXPECT_EQ(Layout::Disconnected, t.layout);
}

TEST(DeviceTopologyTests, SnapshotFreshnessAndOwnerAreIndependentOfOnlineFlags)
{
    Snapshot s;
    s.identity = {42, 100, "1000", 2};
    s.run = 99; s.sequence = 3; s.updatedMillis = 10000;
    s.localName = "Mac"; s.ready = true; s.topology = trio();
    EXPECT_EQ(ReadStatus::Fresh, classifySnapshot(s, s.identity, 11000));
    EXPECT_EQ(ReadStatus::Stale, classifySnapshot(s, s.identity, 20000));
    ProcessIdentity foreign = s.identity; foreign.birth++;
    EXPECT_EQ(ReadStatus::WrongOwner, classifySnapshot(s, foreign, 11000));
    foreign = s.identity; foreign.owner = "2000";
    EXPECT_EQ(ReadStatus::WrongOwner, classifySnapshot(s, foreign, 11000));
    EXPECT_EQ(ReadStatus::Stale, classifySnapshot(s, s.identity, 9000));
    s.ready = false;
    EXPECT_EQ(ReadStatus::NotReady, classifySnapshot(s, s.identity, 11000));
}

TEST(DeviceTopologyTests, PublishedProofRejectsStandbyForeignSessionAndExpiredProof)
{
    Snapshot s;
    s.identity = {42, 100, "S-1-5-21-1000", 2};
    PublishedOwner p{s.identity, 3, 10000};
    EXPECT_TRUE(matchesPublishedOwner(p,s,"S-1-5-21-1000",2,11000));
    s.identity.pid = 43;
    EXPECT_FALSE(matchesPublishedOwner(p,s,"S-1-5-21-1000",2,11000));
    s.identity = p.identity;
    EXPECT_FALSE(matchesPublishedOwner(p,s,"S-1-5-21-1000",3,11000));
    EXPECT_FALSE(matchesPublishedOwner(p,s,"S-1-5-21-2000",2,11000));
    EXPECT_FALSE(matchesPublishedOwner(p,s,"S-1-5-21-1000",2,20000));
    p.generation=0;
    EXPECT_FALSE(matchesPublishedOwner(p,s,"S-1-5-21-1000",2,11000));
}

#ifndef _WIN32
TEST(DeviceTopologyTests, AtomicPrivateSnapshotLifecycleAndDisconnect)
{
    char pattern[]="/tmp/weave-topology-test-XXXXXX";
    const char* path=mkdtemp(pattern); ASSERT_NE(nullptr,path);
    const std::string directory(path);
    const auto identity=currentProcessIdentity();
    ASSERT_NE(0u,identity.birth);
    {
        SnapshotPublisher publisher(directory);
        publisher.update(trio(),"Mac",Role::Client,true);
        Snapshot s; ReadStatus state=ReadStatus::Missing;
        for (int n=0;n<100 && state!=ReadStatus::Fresh;++n) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            state=readSnapshot(directory,identity,monotonicMillis(),s);
        }
        ASSERT_EQ(ReadStatus::Fresh,state);
        struct stat st{};
        ASSERT_EQ(0,stat((directory+"/"+snapshotFilename(identity)).c_str(),&st));
        EXPECT_EQ(0600,st.st_mode&0777);
        EXPECT_EQ("Mac",s.localName);
        publisher.update(trio(),"Mac",Role::Client,false);
        for (int n=0;n<100 && state!=ReadStatus::NotReady;++n) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            state=readSnapshot(directory,identity,monotonicMillis(),s);
        }
        EXPECT_EQ(ReadStatus::NotReady,state);
    }
    Snapshot out;
    EXPECT_EQ(ReadStatus::Missing,readSnapshot(directory,identity,monotonicMillis(),out));
    EXPECT_TRUE(std::filesystem::is_empty(directory));
    std::filesystem::remove(directory);
}

TEST(DeviceTopologyTests, PrivatePublisherRejectsPublicDirectoryAndSymlink)
{
    char pattern[]="/tmp/weave-topology-test-XXXXXX";
    const char* path=mkdtemp(pattern); ASSERT_NE(nullptr,path);
    const std::string directory(path);
    ASSERT_EQ(0,chmod(directory.c_str(),0755));
    {
        SnapshotPublisher publisher(directory);
        publisher.update(trio(),"Mac",Role::Client,true);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    EXPECT_TRUE(std::filesystem::is_empty(directory));
    ASSERT_EQ(0,chmod(directory.c_str(),0700));
    const auto link=directory+"-link";
    ASSERT_EQ(0,symlink(directory.c_str(),link.c_str()));
    {
        SnapshotPublisher publisher(link);
        publisher.update(trio(),"Mac",Role::Client,true);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    EXPECT_TRUE(std::filesystem::is_empty(directory));
    std::filesystem::remove(link); std::filesystem::remove(directory);
}
#endif
