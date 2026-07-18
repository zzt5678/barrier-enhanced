#include "barrier/ServerApp.h"

#include "barrier/ServerArgs.h"
#include "barrier/XBarrier.h"
#include "net/NetworkAddress.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/global/gtest.h"

namespace {

int exitStartup(int, char**)
{
    throw XExitApp(0);
}

} // namespace

TEST(ServerAppTests, runInnerReleasesStateAfterStartupExit)
{
    MockEventQueue events;
    ServerApp app(&events, NULL);
    char executable[] = "weaves";
    char* argv[] = {executable, NULL};

    EXPECT_THROW(app.runInner(1, argv, NULL, &exitStartup), XExitApp);
    EXPECT_EQ(NULL, app.m_barrierAddress);
    EXPECT_EQ(NULL, app.args().m_config);
}
