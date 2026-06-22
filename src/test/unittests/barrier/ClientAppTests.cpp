#include "barrier/ClientApp.h"

#include "test/mock/barrier/MockEventQueue.h"
#include "test/global/gtest.h"

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
