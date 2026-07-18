#define BARRIER_TEST_ENV

#include "server/Server.h"

#include "barrier/option_types.h"
#include "test/global/gtest.h"

namespace {

UInt32 optionValue(const OptionsList& optionsList, OptionID option)
{
    for (UInt32 i = 0; i + 1 < optionsList.size(); i += 2) {
        if (optionsList[i] == option) {
            return optionsList[i + 1];
        }
    }
    return 0;
}

UInt32 optionCount(const OptionsList& optionsList, OptionID option)
{
    UInt32 count = 0;
    for (UInt32 i = 0; i + 1 < optionsList.size(); i += 2) {
        if (optionsList[i] == option) {
            ++count;
        }
    }
    return count;
}

}

TEST(ServerConnectionOptionsTests, addDefaultConnectionOptions_missingHeartbeat_addsDefault)
{
    OptionsList optionsList;

    Server::addDefaultConnectionOptionsForTest(optionsList);

    ASSERT_EQ(2u, optionsList.size());
    EXPECT_EQ(1u, optionCount(optionsList, kOptionHeartbeat));
    EXPECT_EQ(10000u, optionValue(optionsList, kOptionHeartbeat));
}

TEST(ServerConnectionOptionsTests, addDefaultConnectionOptions_existingHeartbeat_keepsConfiguredValue)
{
    OptionsList optionsList;
    optionsList.push_back(kOptionHeartbeat);
    optionsList.push_back(5000);

    Server::addDefaultConnectionOptionsForTest(optionsList);

    ASSERT_EQ(2u, optionsList.size());
    EXPECT_EQ(1u, optionCount(optionsList, kOptionHeartbeat));
    EXPECT_EQ(5000u, optionValue(optionsList, kOptionHeartbeat));
}

TEST(ServerFileReceiveRouteTests, staleSourceCannotAbortCurrentReceive)
{
    Server server;
    BaseClientProxy* source = reinterpret_cast<BaseClientProxy*>(1);
    BaseClientProxy* staleSource = reinterpret_cast<BaseClientProxy*>(2);
    barrier::BulkChannel* route =
        reinterpret_cast<barrier::BulkChannel*>(3);
    barrier::BulkChannel* staleRoute =
        reinterpret_cast<barrier::BulkChannel*>(4);

    ASSERT_TRUE(server.getFileReceiveSession().begin(8, 8, 8));
    server.bindFileReceiveClipboardRevision(source, route);

    EXPECT_TRUE(server.canReceiveFileChunk(source, route));
    EXPECT_FALSE(server.canReceiveFileChunk(source, staleRoute));
    EXPECT_FALSE(server.canReceiveFileChunk(staleSource, route));

    server.abortFileReceiveRoute(staleSource, staleRoute);
    EXPECT_EQ(FileReceiveSession::kReceiving,
              server.getFileReceiveSession().state());

    server.abortFileReceiveRoute(source, route);
    EXPECT_EQ(FileReceiveSession::kIdle,
              server.getFileReceiveSession().state());
    EXPECT_TRUE(server.canReceiveFileChunk(staleSource, staleRoute));
}
