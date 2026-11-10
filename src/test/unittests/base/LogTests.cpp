#include "base/Log.h"
#include "base/ILogOutputter.h"
#include "test/global/gtest.h"

#include <chrono>
#include <future>
#include <thread>

namespace {
class BlockedLogSink : public ILogOutputter {
public:
    std::promise<void> entered;
    std::shared_future<void> release;
    explicit BlockedLogSink(std::shared_future<void> gate) : release(gate) {}
    void open(const char*) override {}
    void close() override {}
    void show(bool) override {}
    bool write(ELevel, const char*) override {
        entered.set_value();
        release.wait();
        return false;
    }
};
}

TEST(LogTests, disabledDebugMessageDoesNotWaitForBlockedOutput)
{
    auto* log = Log::getInstance();
    const int oldFilter = log->getFilter();
    log->setFilter(kINFO);
    std::promise<void> release;
    BlockedLogSink sink(release.get_future().share());
    auto entered = sink.entered.get_future();
    log->insert(&sink);
    std::thread writer([] { LOG((CLOG_INFO "isolated blocked sink")); });
    const bool outputBlocked = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    auto disabled = std::async(std::launch::async, [] { LOG((CLOG_DEBUG2 "discarded pointer diagnostic")); });
    const bool returnedBeforeRelease = disabled.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
    release.set_value();
    writer.join();
    disabled.get();
    log->remove(&sink);
    log->setFilter(oldFilter);
    EXPECT_TRUE(outputBlocked);
    EXPECT_TRUE(returnedBeforeRelease);
}

TEST(LogTests, filterUpdateDoesNotWaitForBlockedOutput)
{
    auto* log = Log::getInstance();
    const int oldFilter = log->getFilter();
    log->setFilter(kINFO);
    std::promise<void> release;
    BlockedLogSink sink(release.get_future().share());
    auto entered = sink.entered.get_future();
    log->insert(&sink);
    std::thread writer([] { LOG((CLOG_INFO "isolated blocked sink")); });
    const bool outputBlocked = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    auto update = std::async(std::launch::async, [log] {
        log->setFilter(kWARNING);
        return log->getFilter();
    });
    const bool returnedBeforeRelease = update.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
    release.set_value();
    writer.join();
    const int actualFilter = update.get();
    log->remove(&sink);
    log->setFilter(oldFilter);
    EXPECT_TRUE(outputBlocked);
    EXPECT_TRUE(returnedBeforeRelease);
    EXPECT_EQ(kWARNING, actualFilter);
}
