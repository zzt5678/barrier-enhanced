#include "server/DeviceTopologyConfig.h"
#include "server/Config.h"
#include "test/global/TestEventQueue.h"
#include "test/global/gtest.h"

using namespace weave::topology;
TEST(DeviceTopologyConfigTests, AliasAdmissionAndThreeDeviceCoordinates)
{
    TestEventQueue events;
    Config config(&events);
    config.addScreen("Ubuntu"); config.addScreen("Mac"); config.addScreen("Windows");
    config.addAlias("Mac","mac-alias");
    config.connect("Ubuntu",kLeft,0,1,"mac-alias",0,1);
    config.connect("Ubuntu",kRight,0,1,"Windows",0,1);
    auto t=topologyFromConfig(config,"Ubuntu",{"mac-alias"},1,1);
    ASSERT_EQ(Layout::Available,t.layout);
    ASSERT_EQ(3u,t.nodes.size());
    EXPECT_EQ("Mac",t.nodes[0].name); EXPECT_TRUE(t.nodes[0].online);
    EXPECT_EQ(-1,t.nodes[0].x); EXPECT_EQ(Role::Client,t.nodes[0].role);
    EXPECT_EQ("Ubuntu",t.nodes[1].name); EXPECT_TRUE(t.nodes[1].online);
    EXPECT_EQ(Role::Server,t.nodes[1].role);
    EXPECT_FALSE(t.nodes[2].online); EXPECT_EQ(1,t.nodes[2].x);
    auto gone=topologyFromConfig(config,"Ubuntu",{},1,2);
    EXPECT_FALSE(gone.nodes[0].online); EXPECT_TRUE(gone.nodes[1].online);
    auto back=topologyFromConfig(config,"Ubuntu",{"Mac"},1,3);
    EXPECT_TRUE(back.nodes[0].online);
}

TEST(DeviceTopologyConfigTests, PartialIntervalsAndImpossibleCyclesAreUnavailable)
{
    TestEventQueue events; Config config(&events);
    config.addScreen("a"); config.addScreen("b");
    config.connect("a",kRight,0,0.5,"b",0,1);
    EXPECT_EQ(Layout::PartialEdge,topologyFromConfig(config,"a",{},1,1).layout);
    config.disconnect("a",kRight);
    config.connect("a",kRight,0,1,"b",0,1);
    config.connect("b",kRight,0,1,"a",0,1);
    EXPECT_EQ(Layout::Contradictory,topologyFromConfig(config,"a",{},1,1).layout);
}

#include "barrier/option_types.h"
#include <sstream>

TEST(DeviceClipboardConfigTests, ScreenOptOutSurvivesRoundTripAndAliases)
{
    TestEventQueue events; Config config(&events);
    std::istringstream input("section: screens\nmini:\nbook:\n clipboardSharing = false\nubuntu:\nend\nsection: aliases\nbook:\n laptop\nend\nsection: options\n clipboardSharing = true\nend\n");
    input >> config;
    EXPECT_TRUE(config.isClipboardSharingEnabled("mini"));
    EXPECT_FALSE(config.isClipboardSharingEnabled("book"));
    EXPECT_FALSE(config.isClipboardSharingEnabled("laptop"));
    EXPECT_TRUE(config.isClipboardSharingEnabled("ubuntu"));
    std::stringstream encoded; encoded << config;
    Config decoded(&events); encoded >> decoded;
    EXPECT_FALSE(decoded.isClipboardSharingEnabled("book"));
    EXPECT_TRUE(decoded.isClipboardSharingEnabled("ubuntu"));
}

TEST(DeviceClipboardConfigTests, GlobalDisableCannotBeOverriddenByDevice)
{
    TestEventQueue events; Config config(&events);
    config.addScreen("book");
    config.addOption("book", kOptionClipboardSharing, 1);
    config.addOption("", kOptionClipboardSharing, 0);
    EXPECT_FALSE(config.isClipboardSharingEnabled("book"));
}
