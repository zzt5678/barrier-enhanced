#include "platform/OSXClipboardWorker.h"
#include "barrier/Clipboard.h"
#include "base/Log.h"
#include "mt/ThreadShutdown.h"
#include <chrono>
#include <condition_variable>
#include <mutex>

struct OSXClipboardWorker::Context {
    Context(CFStringRef value, SnapshotReader read) : name(value), reader(std::move(read)) {
        if (name) CFRetain(name);
        if (!reader) reader = [](OSXClipboard&, CFStringRef name, const std::function<bool()>& current) {
            return OSXClipboardSnapshot::read(name, current);
        };
    }
    ~Context() { if (name) CFRelease(name); }
    struct Publication {
        ClipboardID id = kClipboardClipboard;
        std::uint64_t token = 0;
        std::shared_ptr<const String> data;
    };
    CFStringRef name;
    SnapshotReader reader;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false, exited = false, poll = false, ready = false;
    std::uint64_t generation = 0;
    std::uint64_t lifetime = 0;
    UInt32 sequence = 0, readySequence = 0;
    std::chrono::steady_clock::time_point nextNativePoll{};
    std::shared_ptr<const String> snapshot;
    Publication pending;
    std::vector<IScreen::ClipboardPublicationInfo> completions;

    void completed(const Publication& request, IScreen::ClipboardPublicationResult result) {
        if (request.token != 0)
            completions.push_back({request.id, request.token, result, 0});
    }
};

OSXClipboardWorker::OSXClipboardWorker(CFStringRef name, SnapshotReader reader) :
    m_context(std::make_shared<Context>(name, std::move(reader))) {}

OSXClipboardSnapshotResult OSXClipboardWorker::readInProcess(OSXClipboard& clipboard,
    CFStringRef, const std::function<bool()>& current)
{
    OSXClipboardSnapshotResult result;
    if (current && !current()) return result;
    Clipboard candidate;
    if (clipboard.copyTo(&candidate)) {
        const auto began = std::chrono::steady_clock::now();
        result.data = std::make_shared<const String>(candidate.marshall());
        result.marshallingMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now()-began).count();
    }
    result.timings = clipboard.lastReadTimings();
    return result;
}

OSXClipboardWorker::~OSXClipboardWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_context->mutex);
        m_context->stopping = true;
        ++m_context->generation;
        m_context->pending = {};
    }
    m_context->wake.notify_all();
    if (m_thread.joinable()) {
        barrier::waitForFinalThreadShutdown("macOS clipboard worker",
            barrier::kFinalThreadShutdownDeadlineSeconds, [this](double seconds) {
                std::unique_lock<std::mutex> lock(m_context->mutex);
                return m_context->wake.wait_for(lock, std::chrono::duration<double>(seconds),
                    [this] { return m_context->exited; });
            });
        m_thread.join();
    }
}

void OSXClipboardWorker::start()
{
    if (!m_thread.joinable()) {
        const auto context = m_context;
        m_thread = std::thread([context] { run(context); });
    }
}

void OSXClipboardWorker::poll(UInt32 sequence)
{
    start();
    {
        std::lock_guard<std::mutex> lock(m_context->mutex);
        if (m_context->stopping) return;
        m_context->sequence = sequence;
        const auto now = std::chrono::steady_clock::now();
        if (now < m_context->nextNativePoll) return;
        m_context->nextNativePoll = now + std::chrono::seconds(1);
        m_context->poll = true;
    }
    m_context->wake.notify_one();
}

void OSXClipboardWorker::cancel()
{
    std::lock_guard<std::mutex> lock(m_context->mutex);
    ++m_context->generation;
    ++m_context->lifetime;
    m_context->pending = {};
    m_context->snapshot.reset();
    m_context->ready = false;
    m_context->poll = false;
    m_context->nextNativePoll = {};
    m_context->completions.clear();
}

bool OSXClipboardWorker::publish(ClipboardID id, const std::shared_ptr<const String>& data,
                                  std::uint64_t publicationId)
{
    if (!data || id >= kClipboardEnd || !Clipboard::isValidMarshalled(*data)) return false;
    start();
    {
        std::lock_guard<std::mutex> lock(m_context->mutex);
        if (m_context->stopping) return false;
        m_context->completed(m_context->pending, IScreen::ClipboardPublicationResult::Superseded);
        ++m_context->generation;
        m_context->snapshot.reset();
        m_context->ready = false;
        m_context->pending = {id, publicationId, data};
    }
    m_context->wake.notify_one();
    return true;
}

bool OSXClipboardWorker::snapshot(std::shared_ptr<const String>* data, UInt32* sequence) const
{
    std::lock_guard<std::mutex> lock(m_context->mutex);
    if (!m_context->snapshot) return false;
    *data = m_context->snapshot;
    if (sequence) *sequence = m_context->readySequence;
    return true;
}

bool OSXClipboardWorker::takeReady(UInt32* sequence)
{
    std::lock_guard<std::mutex> lock(m_context->mutex);
    if (!m_context->ready || !m_context->snapshot) return false;
    *sequence = m_context->readySequence;
    m_context->ready = false;
    return true;
}

std::vector<IScreen::ClipboardPublicationInfo> OSXClipboardWorker::takePublications()
{
    std::lock_guard<std::mutex> lock(m_context->mutex);
    std::vector<IScreen::ClipboardPublicationInfo> results;
    results.swap(m_context->completions);
    return results;
}

void OSXClipboardWorker::run(const std::shared_ptr<Context>& context)
{
    // Completion runs after the native reference and converters are destroyed.
    struct Finished {
        std::shared_ptr<Context> context;
        ~Finished() {
            std::lock_guard<std::mutex> lock(context->mutex);
            context->exited = true;
            context->wake.notify_all();
        }
    } finished{context};
    try {
        OSXClipboard clipboard(context->name);
        bool localContent = true;
        unsigned failures = 0;
        std::chrono::steady_clock::time_point retryAfter{};
        auto lastSlowLog = std::chrono::steady_clock::now() - std::chrono::seconds(5);
        const auto reportSlow = [&](double milliseconds) {
            const auto now = std::chrono::steady_clock::now();
            if (milliseconds < 50 || now - lastSlowLog < std::chrono::seconds(5)) return false;
            lastSlowLog = now;
            return true;
        };
        for (;;) {
            Context::Publication publication;
            std::uint64_t generation, lifetime;
            UInt32 sequence;
            bool hasSnapshot;
            {
                std::unique_lock<std::mutex> lock(context->mutex);
                context->wake.wait(lock, [&] { return context->stopping || context->poll || context->pending.data; });
                if (context->stopping) return;
                publication = std::move(context->pending);
                context->pending = {};
                generation = context->generation;
                lifetime = context->lifetime;
                sequence = context->sequence;
                hasSnapshot = bool(context->snapshot);
                context->poll = false;
            }
            const auto current = [&] {
                std::lock_guard<std::mutex> lock(context->mutex);
                return !context->stopping && generation == context->generation;
            };
            const auto observedLocalChange = [&] {
                std::lock_guard<std::mutex> lock(context->mutex);
                if (context->stopping) return;
                // The native board survives cancel/re-enable. Its physical
                // modification is authoritative even if the reader belongs to
                // an older lifetime; only that reader's completion is stale.
                const bool requestRead = lifetime == context->lifetime || bool(context->pending.data);
                // Synchronize consumes the native modification flag. Keep that
                // evidence even when this read's generation is obsolete: a
                // queued remote job must not overwrite the newly observed copy.
                if (context->pending.data) {
                    context->completed(context->pending, IScreen::ClipboardPublicationResult::Superseded);
                    context->pending = {};
                    ++context->generation;
                }
                context->snapshot.reset();
                context->ready = false;
                if (requestRead) context->poll = true;
            };
            if (publication.data) {
                auto result = OSXClipboard::PublishResult::Failed;
                OSXClipboard::PublishTimings timings;
                bool nativeChange = false;
                const auto began = std::chrono::steady_clock::now();
                try {
                    Clipboard source;
                    source.unmarshall(*publication.data, 0);
                    result = clipboard.publish(&source, current, &timings, &nativeChange);
                }
                catch (const std::exception&) { result = OSXClipboard::PublishResult::Failed; }
                if (nativeChange) {
                    localContent = true;
                    observedLocalChange();
                }
                else if (result == OSXClipboard::PublishResult::Succeeded ||
                         result == OSXClipboard::PublishResult::FailedAfterCommit) {
                    localContent = false;
                }
                const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
                if (reportSlow(elapsed)) {
                    LOG((CLOG_INFO "slow macOS clipboard publication: total=%.1fms preparation=%.1fms commit=%.1fms result=%d",
                        elapsed, timings.preparationMilliseconds, timings.commitMilliseconds, int(result)));
                }
                else {
                    LOG((CLOG_DEBUG "macOS clipboard publication: total=%.1fms preparation=%.1fms commit=%.1fms result=%d",
                        elapsed, timings.preparationMilliseconds, timings.commitMilliseconds, int(result)));
                }
                std::lock_guard<std::mutex> lock(context->mutex);
                if (context->stopping) return;
                if (lifetime != context->lifetime) continue;
                const auto completion = generation != context->generation || result == OSXClipboard::PublishResult::Superseded
                    ? IScreen::ClipboardPublicationResult::Superseded
                    : result == OSXClipboard::PublishResult::Succeeded
                        ? IScreen::ClipboardPublicationResult::Succeeded : IScreen::ClipboardPublicationResult::Failed;
                context->completed(publication, completion);
                if (localContent) context->poll = true;
                continue;
            }
            const bool changed = clipboard.synchronize();
            if (changed) {
                localContent = true;
                observedLocalChange();
            }
            if (changed) retryAfter = {};
            if (!changed && (hasSnapshot || !localContent ||
                std::chrono::steady_clock::now() < retryAfter)) continue;
            {
                std::lock_guard<std::mutex> lock(context->mutex);
                if (context->stopping) return;
                if (generation != context->generation) continue;
                context->snapshot.reset();
                context->ready = false;
            }
            const auto began = std::chrono::steady_clock::now();
            OSXClipboardSnapshotResult read;
            try { read = context->reader(clipboard, context->name, current); }
            catch (const std::exception&) { }
            auto snapshot = std::move(read.data);
            const double marshallingMilliseconds = read.marshallingMilliseconds;
            if (read.timedOut) {
                retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                LOG((CLOG_WARN "macOS clipboard provider read timed out; reader stopped, unchanged content retry paused for 30s"));
            }
            else if (!snapshot && current()) {
                retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            }
            const bool changedDuringRead = clipboard.synchronize();
            if (changedDuringRead) {
                retryAfter = {};
                observedLocalChange();
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            const auto timings = read.timings;
            if (reportSlow(elapsed)) {
                LOG((CLOG_INFO "slow macOS clipboard snapshot: total=%.1fms native-read=%.1fms conversion=%.1fms marshal=%.1fms valid=%d",
                    elapsed, timings.nativeReadMilliseconds, timings.conversionMilliseconds, marshallingMilliseconds,
                    int(bool(snapshot) && !changedDuringRead)));
            }
            else if (snapshot || failures++ == 0) {
                LOG((CLOG_DEBUG "macOS clipboard snapshot: total=%.1fms native-read=%.1fms conversion=%.1fms marshal=%.1fms valid=%d",
                    elapsed, timings.nativeReadMilliseconds, timings.conversionMilliseconds, marshallingMilliseconds,
                    int(bool(snapshot) && !changedDuringRead)));
            }
            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->stopping) return;
            if (generation != context->generation || changedDuringRead || !snapshot) continue;
            failures = 0;
            context->snapshot = std::move(snapshot);
            context->readySequence = sequence;
            context->ready = true;
        }
    }
    catch (const std::exception& exception) {
        LOG((CLOG_ERR "macOS clipboard worker failed: %s", exception.what()));
    }
}
