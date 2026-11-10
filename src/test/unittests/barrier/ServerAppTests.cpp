#include "barrier/ServerApp.h"
#include "common/CoreRestartPolicy.h"

#include "barrier/ServerArgs.h"
#include "barrier/XBarrier.h"
#include "barrier/XScreen.h"
#include "base/EventQueue.h"
#include "net/NetworkAddress.h"
#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/barrier/MockScreen.h"
#include "test/global/gtest.h"

namespace {

int exitStartup(int, char**)
{
    throw XExitApp(0);
}

} // namespace

class ServerAppTestAccess {
public:
    static bool inputFailure(const ServerApp& app) { return app.m_inputFailure; }
};

TEST(ServerAppTests, screenFailureRequestsQuitAndPreventsAutomaticRestart)
{
    MockEventQueue events;
    ServerApp app(&events, NULL);
    EXPECT_CALL(events, newOneShotTimer(testing::_, testing::_)).Times(0);
    EXPECT_CALL(events, addEvent(testing::_)).WillOnce(testing::Invoke([](const Event& event) {
        EXPECT_EQ(Event::kQuit, event.getType());
    }));

    app.handleScreenError(Event(Event::kUnknown), nullptr);

    EXPECT_TRUE(ServerAppTestAccess::inputFailure(app));
    EXPECT_TRUE(barrier::coreExitRequiresCorrection(kExitInputUnavailable));
}

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

namespace {
class StartupQueue : public EventQueue {
public:
    int retries = 0;
    EventQueueTimer* newOneShotTimer(double duration, void* target) override {
        ++retries;
        return EventQueue::newOneShotTimer(duration, target);
    }
};
class StartupScreen : public testing::NiceMock<MockScreen> {
public:
    explicit StartupScreen(int& destroyed) : m_destroyed(destroyed) {}
    ~StartupScreen() override { ++m_destroyed; }
    void* getEventTarget() const override { return const_cast<StartupScreen*>(this); }
private:
    int& m_destroyed;
};
class StartupServer : public ServerApp {
public:
    explicit StartupServer(IEventQueue* events) : ServerApp(events, nullptr) {
        setByeFunc([](int code) { throw XExitApp(code); });
    }
    enum Failure { OpenInput, EnableInput, Temporary, Network } failure = OpenInput;
    int destroyed = 0;
    barrier::Screen* createScreen() override {
        if (failure == OpenInput) throw XInputUnavailable("accessibility denied");
        if (failure == Temporary) throw XScreenUnavailable(0.5);
        return new StartupScreen(destroyed);
    }
    ClientListener* openClientListener(const NetworkAddress&) override { return nullptr; }
    Server* openServer(Config&, PrimaryClient*) override {
        if (failure == EnableInput) throw XInputUnavailable("tap creation failed");
        throw XBase("network unavailable");
    }
};
void configureStartup(StartupServer& app, Config& config) {
    app.args().m_name = "primary";
    app.args().m_config = &config;
    config.addScreen("primary");
    config.setBarrierAddress(NetworkAddress(24800));
}
int startupExit(ServerApp& app) {
    try { app.startNode(); }
    catch (const XExitApp& e) { return e.getCode(); }
    return -1;
}
}

TEST(ServerAppTests, unavailableNativeInputAtOpenStopsWithoutRetry)
{
    StartupQueue events; StartupServer app(&events); Config config(&events);
    configureStartup(app, config);
    EXPECT_EQ(kExitInputUnavailable, startupExit(app));
    EXPECT_TRUE(ServerAppTestAccess::inputFailure(app));
    EXPECT_EQ(0, events.retries);
    EXPECT_EQ(kUninitialized, app.m_serverState);
}

TEST(ServerAppTests, unavailableNativeInputAtEnableClosesInitializedScreenBeforeExit)
{
    StartupQueue events; StartupServer app(&events); Config config(&events);
    configureStartup(app, config); app.failure = StartupServer::EnableInput;
    EXPECT_EQ(kExitInputUnavailable, startupExit(app));
    EXPECT_TRUE(ServerAppTestAccess::inputFailure(app));
    EXPECT_EQ(1, app.destroyed);
    EXPECT_EQ(nullptr, app.m_primaryClient);
    EXPECT_EQ(nullptr, app.m_serverScreen);
    EXPECT_EQ(0, events.retries);
    app.cleanupServer(); // also clean old behavior when this regression is RED
}

TEST(ServerAppTests, ordinaryStartupFailureKeepsRetryableExitButCleansScreen)
{
    StartupQueue events; StartupServer app(&events); Config config(&events);
    configureStartup(app, config); app.failure = StartupServer::Network;
    EXPECT_EQ(kExitFailed, startupExit(app));
    EXPECT_FALSE(ServerAppTestAccess::inputFailure(app));
    EXPECT_EQ(1, app.destroyed);
    app.cleanupServer();
}

TEST(ServerAppTests, temporarilyUnavailableScreenStillSchedulesRetry)
{
    StartupQueue events; StartupServer app(&events); Config config(&events);
    configureStartup(app, config); app.failure = StartupServer::Temporary;
    app.args().m_restartable = true;
    EXPECT_TRUE(app.startServer());
    EXPECT_EQ(1, events.retries);
    EXPECT_FALSE(ServerAppTestAccess::inputFailure(app));
    app.cleanupServer();
    EXPECT_EQ(nullptr, app.m_timer);
}

TEST(ServerAppTests, serviceStandbyPreservesNativeInputFailureExit)
{
    StartupQueue events; StartupServer app(&events); Config config(&events);
    configureStartup(app, config); NetworkAddress address;
    app.m_barrierAddress = &address;
    app.args().m_serviceStandby = true;
    EXPECT_EQ(kExitInputUnavailable, app.mainLoop());
    EXPECT_EQ(kUninitialized, app.m_serverState);
}

TEST(ServerAppTests, runInnerCleansInitializedScreenOnStartupException)
{
    StartupQueue events; StartupServer app(&events);
    app.failure = StartupServer::EnableInput;
    char executable[] = "weaves"; char* argv[] = {executable, nullptr};
    EXPECT_THROW(app.runInner(1, argv, nullptr, [](int, char**) -> int {
        auto& app = ServerApp::instance();
        app.args().m_name = "primary";
        app.args().m_config->addScreen("primary");
        EXPECT_TRUE(app.initServer());
        throw XInputUnavailable("startup aborted");
    }), XInputUnavailable);
    EXPECT_EQ(1, app.destroyed);
    EXPECT_EQ(nullptr, app.m_serverScreen);
    app.cleanupServer();
}

TEST(ServerAppTests, applicationBoundaryPreservesNativeInputFailureReasonAndExit)
{
    class EscapingStartup : public StartupServer {
    public:
        using StartupServer::StartupServer;
        int standardStartup(int, char**) override { throw XInputUnavailable("tap unavailable"); }
    };
    StartupQueue events; EscapingStartup app(&events);
    char executable[] = "weaves"; char* argv[] = {executable, nullptr};
    EXPECT_STREQ("tap unavailable", XInputUnavailable("tap unavailable").what());
    EXPECT_EQ(kExitInputUnavailable, app.run(1, argv));
}
