#include "topology/TopologySnapshot.h"
#include "test/global/gtest.h"
#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <filesystem>
#include <thread>
#include <chrono>

using namespace weave::topology;
namespace {
struct PrivateFixture {
    std::filesystem::path path;
    PrivateFixture() {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH,temp);
        path=std::filesystem::path(temp)/(L"weave-topology-test-"+std::to_wstring(newRunId()));
    }
    ~PrivateFixture() { std::error_code error; std::filesystem::remove_all(path,error); }
};
Topology fixtureTopology() {
    Topology t; t.revision=1; t.viewerName="client"; t.layout=Layout::Available;
    t.nodes={{"server",Role::Server,true,0,0},{"client",Role::Client,true,1,0}};
    return t;
}
ReadStatus poll(const std::string& dir,Snapshot& out) {
    ReadStatus status=ReadStatus::Missing;
    for (int i=0;i<100 && status!=ReadStatus::Fresh;++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        status=readSnapshot(dir,currentProcessIdentity(),monotonicMillis(),out);
    }
    return status;
}
}

TEST(MSWindowsTopologySnapshotTests, RealPrivateAclAtomicReadAndRetirement)
{
    PrivateFixture fixture;
    const auto dir=fixture.path.u8string();
    Snapshot out;
    {
        SnapshotPublisher publisher(dir);
        publisher.update(fixtureTopology(),"client",Role::Client,true);
        ASSERT_EQ(ReadStatus::Fresh,poll(dir,out));
        EXPECT_EQ(currentProcessIdentity().birth,out.identity.birth);
        EXPECT_EQ("client",out.localName);
        ProcessIdentity foreign=out.identity; foreign.birth++;
        EXPECT_EQ(ReadStatus::WrongOwner,readSnapshot(dir,foreign,monotonicMillis(),out));
    }
    EXPECT_EQ(ReadStatus::Missing,readSnapshot(dir,currentProcessIdentity(),monotonicMillis(),out));
    EXPECT_TRUE(std::filesystem::is_empty(fixture.path));
}

TEST(MSWindowsTopologySnapshotTests, RejectsWorldWritableDirectoryWithoutRepairingAcl)
{
    PrivateFixture fixture;
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;OICI;FA;;;WD)",SDDL_REVISION_1,&descriptor,nullptr));
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
    const bool created=CreateDirectoryW(fixture.path.c_str(),&attributes)!=FALSE;
    LocalFree(descriptor); ASSERT_TRUE(created);
    {
        SnapshotPublisher publisher(fixture.path.u8string());
        publisher.update(fixtureTopology(),"client",Role::Client,true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(std::filesystem::is_empty(fixture.path));
}

TEST(MSWindowsTopologySnapshotTests, RejectsNullDaclSnapshotWithoutTrustingContents)
{
    PrivateFixture fixture; const auto dir=fixture.path.u8string(); Snapshot out;
    SnapshotPublisher publisher(dir);
    publisher.update(fixtureTopology(),"client",Role::Client,true);
    ASSERT_EQ(ReadStatus::Fresh,poll(dir,out));
    const auto file=fixture.path/std::filesystem::u8path(snapshotFilename(out.identity));
    ASSERT_EQ(ERROR_SUCCESS,SetNamedSecurityInfoW(const_cast<LPWSTR>(file.c_str()),SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,nullptr,nullptr));
    EXPECT_EQ(ReadStatus::Missing,readSnapshot(dir,out.identity,monotonicMillis(),out));
}
