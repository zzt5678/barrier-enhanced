#include "barrier/ClientApp.h"
#include "client/Client.h"
#include "common/CoreRestartPolicy.h"

#include "test/mock/barrier/MockEventQueue.h"
#include "test/global/gtest.h"

namespace {

int successfulStartup(int, char**)
{
    return 0;
}

} // namespace

TEST(ClientAppTests, permanentExitBlocksOnlyTheFailedCommandGeneration)
{
    EXPECT_TRUE(barrier::coreExitRequiresCorrection(kExitIncompatible));
    EXPECT_FALSE(barrier::coreExitRequiresCorrection(kExitFailed));
    EXPECT_FALSE(barrier::coreGenerationMayRestart(true, 7, 7));
    EXPECT_TRUE(barrier::coreGenerationMayRestart(true, 7, 8));
    EXPECT_TRUE(barrier::coreGenerationMayRestart(false, 0, 0));
}

class ClientAppTestAccess {
public:
    static bool permanentFailure(const ClientApp& app) { return app.m_permanentFailure; }
    static bool hasRunInnerAddress(const ClientApp& app)
    {
        return app.m_serverAddress != NULL;
    }
};

TEST(ClientAppTests, permanentFailureRequestsQuitWithoutSchedulingRetry)
{
    MockEventQueue events;
    ClientApp app(&events, NULL);
    auto* info = new Client::FailInfo("incompatible protocol");
    info->m_retry = false;
    EXPECT_CALL(events, newOneShotTimer(testing::_, testing::_)).Times(0);
    EXPECT_CALL(events, addEvent(testing::_)).WillOnce(testing::Invoke([](const Event& event) {
        EXPECT_EQ(Event::kQuit, event.getType());
    }));
    app.handleClientFailed(Event(Event::kUnknown, nullptr, info, Event::kDontFreeData), nullptr);
    EXPECT_TRUE(ClientAppTestAccess::permanentFailure(app));
}

TEST(ClientAppTests, runInnerReleasesAddressAfterSuccessfulStartup)
{
    MockEventQueue events;
    ClientApp app(&events, NULL);
    char executable[] = "weavec";
    char* argv[] = {executable, NULL};

    EXPECT_EQ(0, app.runInner(1, argv, NULL, &successfulStartup));
    EXPECT_FALSE(ClientAppTestAccess::hasRunInnerAddress(app));
}

TEST(ClientAppTests, nextRestartTimeout_usesShortCappedBackoff)
{
    MockEventQueue events;
    ClientApp app(&events, NULL);

    EXPECT_DOUBLE_EQ(1.0, app.nextRestartTimeout());
    EXPECT_DOUBLE_EQ(1.5, app.nextRestartTimeout());
    EXPECT_DOUBLE_EQ(2.25, app.nextRestartTimeout());
    EXPECT_DOUBLE_EQ(3.0, app.nextRestartTimeout());
    EXPECT_DOUBLE_EQ(3.0, app.nextRestartTimeout());
}

TEST(ClientAppTests, resetRestartTimeout_restoresMinimumRetry)
{
    MockEventQueue events;
    ClientApp app(&events, NULL);

    EXPECT_DOUBLE_EQ(1.0, app.nextRestartTimeout());
    EXPECT_DOUBLE_EQ(1.5, app.nextRestartTimeout());

    app.resetRestartTimeout();

    EXPECT_DOUBLE_EQ(1.0, app.nextRestartTimeout());
}
