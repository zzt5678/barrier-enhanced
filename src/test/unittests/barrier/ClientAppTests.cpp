#include "barrier/ClientApp.h"
#include "client/Client.h"
#include "barrier/ClientArgs.h"
#include "barrier/XBarrier.h"
#include "barrier/XScreen.h"
#include "base/EventQueue.h"
#include "net/ISocketFactory.h"
#include "net/NetworkAddress.h"
#include "mt/Thread.h"
#include "common/CoreRestartPolicy.h"
#include <functional>

#include "test/mock/barrier/MockEventQueue.h"
#include "test/mock/barrier/MockScreen.h"
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
    static bool inputFailure(const ClientApp& app) { return app.m_inputFailure; }
    static bool permanentFailure(const ClientApp& app) { return app.m_permanentFailure; }
    static bool hasRunInnerAddress(const ClientApp& app)
    {
        return app.m_serverAddress != NULL;
    }
    static bool hasScreen(const ClientApp& app) { return app.m_clientScreen != nullptr; }
    static void address(ClientApp& app, NetworkAddress* value) { app.m_serverAddress = value; }
};

TEST(ClientAppTests, screenFailureRequestsQuitAndPreventsAutomaticRestart)
{
    MockEventQueue events;
    ClientApp app(&events, NULL);
    EXPECT_CALL(events, newOneShotTimer(testing::_, testing::_)).Times(0);
    EXPECT_CALL(events, addEvent(testing::_)).WillOnce(testing::Invoke([](const Event& event) {
        EXPECT_EQ(Event::kQuit, event.getType());
    }));

    app.handleScreenError(Event(Event::kUnknown), nullptr);

    EXPECT_TRUE(ClientAppTestAccess::inputFailure(app));
    EXPECT_TRUE(barrier::coreExitRequiresCorrection(kExitInputUnavailable));
}

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

namespace {
class ClientStartupQueue : public EventQueue {
public:
    int retries = 0, errors = 0, connected = 0;
    void* errorTarget = nullptr;
    EventQueueTimer* newOneShotTimer(double duration, void* target) override {
        ++retries; return EventQueue::newOneShotTimer(duration, target);
    }
    void addEvent(const Event& event) override {
        if (event.getType() == forIScreen().error()) {
            ++errors; errorTarget = event.getTarget();
        }
        if (event.getType() == forClient().connected()) ++connected;
        EventQueue::addEvent(event);
    }
};
class NoConnectionFactory : public ISocketFactory {
public:
    IDataSocket* create(IArchNetwork::EAddressFamily, ConnectionSecurityLevel) const override {
        ADD_FAILURE() << "startup test must not connect"; return nullptr;
    }
    IListenSocket* createListen(IArchNetwork::EAddressFamily, ConnectionSecurityLevel) const override {
        ADD_FAILURE() << "startup test must not listen"; return nullptr;
    }
};
class ClientStartupScreen : public testing::NiceMock<MockScreen> {
public:
    explicit ClientStartupScreen(int& destroyed) : m_destroyed(destroyed) {}
    ~ClientStartupScreen() override { ++m_destroyed; }
    void* getEventTarget() const override { return const_cast<ClientStartupScreen*>(this); }
    bool prepareInputBackend() override {
        if (prepareFailure) prepareFailure();
        return true;
    }
    std::function<void()> prepareFailure;
private:
    int& m_destroyed;
};
class StartupClientInstance : public Client {
public:
    StartupClientInstance(IEventQueue* events, const String& name, const NetworkAddress& address,
                          barrier::Screen* screen, const ClientArgs& args, int& destroyed) :
        Client(events, name, address, new NoConnectionFactory, screen, args), m_destroyed(destroyed) {}
    ~StartupClientInstance() override { ++m_destroyed; }
private:
    int& m_destroyed;
};
class StartupClient : public ClientApp {
public:
    explicit StartupClient(IEventQueue* events) : ClientApp(events, nullptr) {
        setByeFunc([](int code) { throw XExitApp(code); });
    }
    enum Failure { OpenInput, CreateInput, Temporary, Network, None } failure = OpenInput;
    int destroyed = 0;
    int clientsDestroyed = 0;
    std::function<void()> backendFailure;
    barrier::Screen* createScreen() override {
        if (failure == OpenInput) throw XInputUnavailable("accessibility denied");
        if (failure == Temporary) throw XScreenUnavailable(0.5);
        auto* screen = new ClientStartupScreen(destroyed);
        screen->prepareFailure = backendFailure;
        return screen;
    }
    Client* openClient(const String& name, const NetworkAddress& address,
                       barrier::Screen* screen) override {
        if (failure == CreateInput) throw XInputUnavailable("native backend unavailable");
        if (failure == Network) throw XBase("network unavailable");
        return new StartupClientInstance(getEvents(), name, address, screen, args(), clientsDestroyed);
    }
};
int clientStartupExit(ClientApp& app) {
    try { app.startNode(); }
    catch (const XExitApp& e) { return e.getCode(); }
    return -1;
}
void expectAdoptedBackendFailure(const std::function<void()>& failure, bool retry, bool permanent = false)
{
    ClientStartupQueue events; StartupClient app(&events); NetworkAddress address;
    ClientAppTestAccess::address(app, &address);
    app.failure = StartupClient::None;
    app.backendFailure = failure;
    app.args().m_restartable = true;
    EXPECT_EQ(retry, app.prepareClient());
    EXPECT_EQ(1, app.destroyed);
    EXPECT_EQ(1, app.clientsDestroyed);
    EXPECT_FALSE(ClientAppTestAccess::hasScreen(app));
    EXPECT_EQ(nullptr, app.getClientPtr());
    EXPECT_EQ(retry ? 1 : 0, events.retries);
    EXPECT_EQ(permanent, ClientAppTestAccess::inputFailure(app));
    // On RED, the old catch has already freed the screen while leaving both
    // members dangling. Do not dereference that diagnosed UAF in test cleanup.
    if (!ClientAppTestAccess::hasScreen(app) && app.getClientPtr() == nullptr) {
        app.stopClient();
        EXPECT_EQ(1, app.destroyed);
        EXPECT_EQ(1, app.clientsDestroyed);
    }
}
}

TEST(ClientAppTests, adoptedBackendTemporaryFailureCleansOnceAndKeepsRetry)
{
    expectAdoptedBackendFailure([] { throw XScreenUnavailable(0.5); }, true);
}

TEST(ClientAppTests, adoptedBackendOpenFailureCleansOnceWithoutRetry)
{
    expectAdoptedBackendFailure([] { throw XScreenOpenFailure("open failed"); }, false);
}

TEST(ClientAppTests, adoptedBackendGenericFailureCleansOnceWithoutRetry)
{
    expectAdoptedBackendFailure([] { throw XBase("backend failed"); }, false);
}

TEST(ClientAppTests, adoptedBackendInputFailureStillCleansOnceAndStaysPermanent)
{
    expectAdoptedBackendFailure([] { throw XInputUnavailable("input unavailable"); }, false, true);
}

TEST(ClientAppTests, unavailableNativeInputAtOpenStopsWithoutRetry)
{
    ClientStartupQueue events; StartupClient app(&events);
    EXPECT_EQ(kExitInputUnavailable, clientStartupExit(app));
    EXPECT_TRUE(ClientAppTestAccess::inputFailure(app));
    EXPECT_EQ(0, events.retries);
}

TEST(ClientAppTests, unavailableNativeInputAfterScreenOpenClosesItBeforeExit)
{
    ClientStartupQueue events; StartupClient app(&events); NetworkAddress address;
    ClientAppTestAccess::address(app, &address); app.failure = StartupClient::CreateInput;
    EXPECT_EQ(kExitInputUnavailable, clientStartupExit(app));
    EXPECT_TRUE(ClientAppTestAccess::inputFailure(app));
    EXPECT_EQ(1, app.destroyed);
    EXPECT_FALSE(ClientAppTestAccess::hasScreen(app));
    EXPECT_EQ(nullptr, app.getClientPtr());
    EXPECT_EQ(0, events.retries);
}

TEST(ClientAppTests, ordinaryStartupFailureKeepsRetryableExit)
{
    ClientStartupQueue events; StartupClient app(&events); NetworkAddress address;
    ClientAppTestAccess::address(app, &address); app.failure = StartupClient::Network;
    EXPECT_EQ(kExitFailed, clientStartupExit(app));
    EXPECT_FALSE(ClientAppTestAccess::inputFailure(app));
    EXPECT_EQ(1, app.destroyed);
}

TEST(ClientAppTests, temporarilyUnavailableScreenStillSchedulesRetry)
{
    ClientStartupQueue events; StartupClient app(&events);
    app.failure = StartupClient::Temporary; app.args().m_restartable = true;
    EXPECT_TRUE(app.prepareClient());
    EXPECT_EQ(1, events.retries);
    EXPECT_FALSE(ClientAppTestAccess::inputFailure(app));
}

TEST(ClientAppTests, serviceStandbyPreservesNativeInputFailureExit)
{
    ClientStartupQueue events; StartupClient app(&events);
    app.args().m_serviceStandby = true;
    EXPECT_EQ(kExitInputUnavailable, app.mainLoop());
    EXPECT_FALSE(ClientAppTestAccess::hasScreen(app));
}

TEST(ClientAppTests, runInnerCleansPreparedClientOnStartupException)
{
    ClientStartupQueue events; StartupClient app(&events);
    app.failure = StartupClient::None; app.args().m_serviceStandby = true;
    char executable[] = "weavec"; char* argv[] = {executable, nullptr};
    EXPECT_THROW(app.runInner(1, argv, nullptr, [](int, char**) -> int {
        EXPECT_TRUE(ClientApp::instance().prepareClient());
        throw XInputUnavailable("startup aborted");
    }), XInputUnavailable);
    EXPECT_EQ(1, app.destroyed);
    EXPECT_FALSE(ClientAppTestAccess::hasScreen(app));
    EXPECT_EQ(nullptr, app.getClientPtr());
    app.stopClient();
}

TEST(ClientAppTests, handshakeInputFailureQueuesScreenErrorWithoutConnectedNotification)
{
    ClientStartupQueue events; int destroyed = 0;
    ClientStartupScreen screen(destroyed); ClientArgs args;
    Client client(&events, "client", NetworkAddress(), new NoConnectionFactory, &screen, args);
    EXPECT_CALL(screen, enable()).WillOnce(testing::Throw(XInputUnavailable("tap unavailable")));
    EXPECT_NO_THROW(client.handshakeComplete());
    EXPECT_EQ(1, events.errors);
    EXPECT_EQ(screen.getEventTarget(), events.errorTarget);
    EXPECT_EQ(0, events.connected);
    if (events.errors == 1) EXPECT_FALSE(client.canAcceptInputHandoff());
}

TEST(ClientAppTests, businessThreadInputFailureReachesAppShutdownInsteadOfEscapingThread)
{
    ClientStartupQueue events; StartupClient app(&events); NetworkAddress address;
    ClientAppTestAccess::address(app, &address);
    app.failure = StartupClient::None; app.args().m_serviceStandby = true;
    ASSERT_TRUE(app.prepareClient());
    // Configure the actual screen already adopted by prepareClient, not a
    // replacement for the client handshake or its event delivery.
    auto* owned = static_cast<ClientStartupScreen*>(app.getClientPtr()->getEventTarget());
    EXPECT_CALL(*owned, enable()).WillOnce(testing::Throw(XInputUnavailable("tap unavailable")));
    std::atomic<bool> returned{false};
    Thread business([&] { app.getClientPtr()->handshakeComplete(); returned = true; });
    ASSERT_TRUE(business.wait(2));
    EXPECT_TRUE(returned.load());
    EXPECT_EQ(1, events.errors);
    EXPECT_EQ(0, events.connected);
    if (events.errors == 1) events.loop();
    EXPECT_TRUE(ClientAppTestAccess::inputFailure(app));
    app.stopClient();
}
