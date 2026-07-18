#include "barrier/ClientApp.h"

#include "test/mock/barrier/MockEventQueue.h"
#include "test/global/gtest.h"

namespace {

int successfulStartup(int, char**)
{
    return 0;
}

} // namespace

class ClientAppTestAccess {
public:
    static bool hasRunInnerAddress(const ClientApp& app)
    {
        return app.m_serverAddress != NULL;
    }
};

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
