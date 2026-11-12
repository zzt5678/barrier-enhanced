#include <future>
#include <fstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
extern char** environ;
#include <cerrno>
#include "barrier/option_types.h"
#include "platform/OSXScreen.h"
#include "barrier/Clipboard.h"
#include "barrier/RemoteFileClipboard.h"
#include "base/EventQueue.h"
#include "test/global/gtest.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <dlfcn.h>
#include <thread>
#include <unistd.h>
#include "lodepng.h"

namespace {
std::atomic<bool> delayPrivateReads{false};
std::atomic<unsigned> callerReads{0};
std::atomic<unsigned> privateReads{0};
std::atomic<bool> injectLocalDuringPreparation{false};
std::atomic<bool> blockPreparation{false};
std::atomic<bool> preparationEntered{false};
std::atomic<bool> countPrivateSynchronizes{false};
std::atomic<unsigned> privateSynchronizes{0};
std::atomic<unsigned> failPrivatePutNumber{0};
std::atomic<bool> failedPrivatePut{false};
std::atomic<bool> blockPrivateRead{false};
std::atomic<bool> privateReadEntered{false};
std::atomic<bool> injectLocalAfterPrivateRead{false};
std::thread::id callerThread;
CFStringRef privateName = CFSTR("org.weave.unittest.async-clipboard");
}

extern "C" PasteboardSyncFlags PasteboardSynchronize(PasteboardRef board)
{
    using Function = PasteboardSyncFlags (*)(PasteboardRef);
    static const auto native = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "PasteboardSynchronize"));
    if (countPrivateSynchronizes.load()) {
        CFStringRef name = nullptr;
        if (PasteboardCopyName(board, &name) == noErr && name) {
            if (CFEqual(name, privateName)) ++privateSynchronizes;
            CFRelease(name);
        }
    }
    return native(board);
}

extern "C" OSStatus PasteboardPutItemFlavor(PasteboardRef board, PasteboardItemID item,
    CFStringRef type, CFDataRef data, PasteboardFlavorFlags flags)
{
    using Function = OSStatus (*)(PasteboardRef, PasteboardItemID, CFStringRef, CFDataRef, PasteboardFlavorFlags);
    static const auto native = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "PasteboardPutItemFlavor"));
    if (failPrivatePutNumber.load()) {
        CFStringRef name = nullptr;
        if (PasteboardCopyName(board, &name) == noErr && name) {
            const bool isolated = CFEqual(name, privateName);
            CFRelease(name);
            if (isolated && --failPrivatePutNumber == 0) {
                failedPrivatePut = true;
                return badPasteboardFlavorErr;
            }
        }
    }
    return native(board, item, type, data, flags);
}

// Delay only this explicitly named private board. Never access live clipboard
// contents, install input taps, or synthesize input in these regressions.
extern "C" OSStatus PasteboardCopyItemFlavorData(PasteboardRef board,
    PasteboardItemID item, CFStringRef flavor, CFDataRef* data)
{
    using Function = OSStatus (*)(PasteboardRef, PasteboardItemID, CFStringRef, CFDataRef*);
    static const auto native = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "PasteboardCopyItemFlavorData"));
    if (delayPrivateReads.load() || injectLocalDuringPreparation.load() || blockPreparation.load() ||
        blockPrivateRead.load() || injectLocalAfterPrivateRead.load()) {
        CFStringRef name = nullptr;
        if (PasteboardCopyName(board, &name) == noErr && name != nullptr) {
            const bool isolated = CFEqual(name, privateName);
            CFRelease(name);
            if (isolated && blockPrivateRead.load()) {
                privateReadEntered = true;
                const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (blockPrivateRead.load() && std::chrono::steady_clock::now() < limit)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (isolated && injectLocalAfterPrivateRead.exchange(false)) {
                OSXClipboard newer(privateName);
                newer.open(0);
                newer.empty();
                newer.add(IClipboard::kText, "local B copied after remote P was queued");
                newer.close();
            }
            if (!isolated && injectLocalDuringPreparation.exchange(false)) {
                // A second native reference changes the destination while
                // publication is reading its private staging board. This is
                // a native modification, not a generation-callback failure.
                OSXClipboard newer(privateName);
                newer.open(0);
                newer.empty();
                newer.add(IClipboard::kText, "new local copy during preparation");
                newer.close();
            }
            if (!isolated && blockPreparation.load()) {
                preparationEntered = true;
                const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (blockPreparation.load() && std::chrono::steady_clock::now() < limit)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (isolated && delayPrivateReads.load()) {
                ++privateReads;
                if (std::this_thread::get_id() == callerThread) ++callerReads;
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
            }
        }
    }
    return native(board, item, flavor, data);
}

TEST(OSXScreenClipboardTests, slowPrivateProviderNeverRunsOnInputThread)
{
    callerThread = std::this_thread::get_id();
    callerReads = 0;
    EventQueue events;
    OSXScreen screen(&events, false, false, privateName, OSXClipboardWorker::readInProcess);
    OSXClipboard writer(privateName);
    ASSERT_TRUE(writer.open(0));
    ASSERT_TRUE(writer.empty());
    writer.add(IClipboard::kText, "private slow provider");
    writer.close();
    delayPrivateReads = true;
    struct Reset { ~Reset() { delayPrivateReads = false; } } reset;

    screen.checkClipboards();
    Clipboard result;
    const auto start = std::chrono::steady_clock::now();
    screen.getClipboard(kClipboardClipboard, &result);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::fprintf(stderr, "private clipboard input-call: %.3fms, native caller reads=%u\n", elapsed * 1000, callerReads.load());
    EXPECT_EQ(0u, callerReads.load());
    EXPECT_LT(elapsed, 0.080);

    bool ready = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        screen.checkClipboards();
        if (screen.getClipboard(kClipboardClipboard, &result)) {
            ASSERT_TRUE(result.open(0));
            ready = result.get(IClipboard::kText) == "private slow provider";
            result.close();
        }
        if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (!ready && std::chrono::steady_clock::now() < deadline);
    EXPECT_TRUE(ready);
    EXPECT_EQ(0u, callerReads.load());
    EXPECT_TRUE(writer.empty());
}

TEST(OSXScreenClipboardTests, immutableRemoteSnapshotPublishesWithoutLocalFeedback)
{
    EventQueue events;
    OSXScreen screen(&events, false, false, privateName, OSXClipboardWorker::readInProcess);
    Clipboard source;
    ASSERT_TRUE(source.open(0));
    ASSERT_TRUE(source.empty());
    source.add(IClipboard::kText, "remote private publication");
    source.close();
    const auto bytes = std::make_shared<const String>(source.marshall());
    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(screen.setClipboardSnapshot(kClipboardClipboard, bytes, 17));
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 0.080);
    bool published = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    OSXClipboard reader(privateName);
    do {
        screen.checkClipboards();
        reader.synchronize();
        ASSERT_TRUE(reader.open(0));
        published = reader.get(IClipboard::kText) == "remote private publication";
        reader.close();
        if (!published) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (!published && std::chrono::steady_clock::now() < deadline);
    EXPECT_TRUE(published);
    // Remote publication must not create an exportable local snapshot.
    std::shared_ptr<const String> snapshot;
    UInt32 sequence = 0;
    EXPECT_FALSE(screen.getClipboardSnapshot(kClipboardClipboard, &snapshot, &sequence));
    EXPECT_TRUE(reader.empty());
}

TEST(OSXScreenClipboardTests, remotePublicationSupersedesInFlightLocalRead)
{
    callerThread = std::this_thread::get_id();
    privateReads = 0;
    EventQueue events;
    OSXScreen screen(&events, false, false, privateName, OSXClipboardWorker::readInProcess);
    OSXClipboard writer(privateName);
    ASSERT_TRUE(writer.open(0));
    ASSERT_TRUE(writer.empty());
    writer.add(IClipboard::kText, "obsolete local snapshot");
    writer.close();
    delayPrivateReads = true;
    struct Reset { ~Reset() { delayPrivateReads = false; } } reset;
    screen.checkClipboards();
    const auto startedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!privateReads.load() && std::chrono::steady_clock::now() < startedDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_GT(privateReads.load(), 0u);

    Clipboard source;
    ASSERT_TRUE(source.open(0));
    ASSERT_TRUE(source.empty());
    source.add(IClipboard::kText, "new remote revision");
    source.close();
    ASSERT_TRUE(screen.setClipboardSnapshot(kClipboardClipboard,
        std::make_shared<const String>(source.marshall()), 27));
    std::shared_ptr<const String> snapshot;
    UInt32 sequence = 0;
    EXPECT_FALSE(screen.getClipboardSnapshot(kClipboardClipboard, &snapshot, &sequence));
    bool published = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        screen.checkClipboards();
        writer.synchronize();
        ASSERT_TRUE(writer.open(0));
        published = writer.get(IClipboard::kText) == "new remote revision";
        writer.close();
        if (!published) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (!published && std::chrono::steady_clock::now() < deadline);
    EXPECT_TRUE(published);
    EXPECT_FALSE(screen.getClipboardSnapshot(kClipboardClipboard, &snapshot, &sequence));
    delayPrivateReads = false;
    EXPECT_TRUE(writer.empty());
}

TEST(OSXClipboardPublicationTests, cancelledPreparedDataDoesNotClearLatestLocalClipboard)
{
    OSXClipboard target(kPasteboardUniqueName);
    ASSERT_TRUE(target.open(0));
    ASSERT_TRUE(target.empty());
    target.add(IClipboard::kText, "keep local revision");
    target.close();
    target.synchronize();
    Clipboard remote;
    ASSERT_TRUE(remote.open(0));
    ASSERT_TRUE(remote.empty());
    remote.add(IClipboard::kText, "cancelled remote revision");
    remote.close();
    unsigned checks = 0;
    EXPECT_EQ(OSXClipboard::PublishResult::Superseded,
        target.publish(&remote, [&] { return ++checks == 1; }));
    EXPECT_EQ(2u, checks);
    ASSERT_TRUE(target.open(0));
    EXPECT_EQ("keep local revision", target.get(IClipboard::kText));
    target.close();
}

TEST(OSXClipboardPublicationTests, invalidImagePreparationPreservesExistingClipboard)
{
    OSXClipboard target(kPasteboardUniqueName);
    ASSERT_TRUE(target.open(0));
    ASSERT_TRUE(target.empty());
    target.add(IClipboard::kText, "preserve on conversion failure");
    target.close();
    target.synchronize();
    Clipboard invalid;
    ASSERT_TRUE(invalid.open(0));
    ASSERT_TRUE(invalid.empty());
    invalid.add(IClipboard::kPNG, "invalid PNG bytes");
    invalid.close();
    EXPECT_EQ(OSXClipboard::PublishResult::Failed,
        target.publish(&invalid, [] { return true; }));
    ASSERT_TRUE(target.open(0));
    EXPECT_EQ("preserve on conversion failure", target.get(IClipboard::kText));
    target.close();
}

TEST(OSXClipboardPublicationTests, localNativeChangeDuringPreparationSupersedesRemote)
{
    OSXClipboard target(privateName);
    ASSERT_TRUE(target.open(0));
    ASSERT_TRUE(target.empty());
    target.add(IClipboard::kText, "original local copy");
    target.close();
    target.synchronize();
    Clipboard remote;
    ASSERT_TRUE(remote.open(0));
    ASSERT_TRUE(remote.empty());
    remote.add(IClipboard::kText, "obsolete remote copy");
    remote.close();
    injectLocalDuringPreparation = true;
    struct Reset { ~Reset() { injectLocalDuringPreparation = false; } } reset;
    EXPECT_EQ(OSXClipboard::PublishResult::Superseded,
        target.publish(&remote, [] { return true; }));
    EXPECT_FALSE(injectLocalDuringPreparation.load());
    ASSERT_TRUE(target.open(0));
    EXPECT_EQ("new local copy during preparation", target.get(IClipboard::kText));
    EXPECT_TRUE(target.empty());
    target.close();
}

TEST(OSXClipboardWorkerTests, cancelDoesNotDeliverPriorLifetimePublicationCompletion)
{
    OSXClipboardWorker worker(privateName, OSXClipboardWorker::readInProcess);
    Clipboard remote;
    ASSERT_TRUE(remote.open(0));
    ASSERT_TRUE(remote.empty());
    remote.add(IClipboard::kText, "cancelled lifetime");
    remote.close();
    preparationEntered = false;
    blockPreparation = true;
    struct Reset { ~Reset() { blockPreparation = false; } } reset;
    ASSERT_TRUE(worker.publish(kClipboardClipboard, std::make_shared<const String>(remote.marshall()), 59));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!preparationEntered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(preparationEntered.load());
    worker.cancel();
    blockPreparation = false;
    worker.poll(1);
    std::shared_ptr<const String> snapshot;
    UInt32 sequence = 0;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!worker.snapshot(&snapshot, &sequence) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(snapshot);
    EXPECT_TRUE(worker.takePublications().empty());
}

TEST(OSXClipboardWorkerTests, completionPollingDoesNotMultiplyIdleNativePolling)
{
    OSXClipboard writer(privateName);
    ASSERT_TRUE(writer.open(0));
    ASSERT_TRUE(writer.empty());
    writer.add(IClipboard::kText, "idle polling sample");
    writer.close();
    OSXClipboardWorker worker(privateName, OSXClipboardWorker::readInProcess);
    privateSynchronizes = 0;
    countPrivateSynchronizes = true;
    struct Reset { ~Reset() { countPrivateSynchronizes = false; } } reset;
    for (unsigned i = 0; i < 50; ++i) {
        worker.poll(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::shared_ptr<const String> snapshot;
    UInt32 sequence = 0;
    ASSERT_TRUE(worker.snapshot(&snapshot, &sequence));
    Clipboard contents;
    contents.unmarshall(*snapshot, 0);
    ASSERT_TRUE(contents.open(0));
    EXPECT_EQ("idle polling sample", contents.get(IClipboard::kText));
    contents.close();
    std::fprintf(stderr, "private clipboard idle sample: 50 poll requests, native synchronizes=%u\n", privateSynchronizes.load());
    EXPECT_LE(privateSynchronizes.load(), 6u);
}

TEST(OSXClipboardWorkerTests, partialRemoteWriteFailureNeverBecomesLocalFeedback)
{
    OSXClipboardWorker worker(privateName, OSXClipboardWorker::readInProcess);
    Clipboard remote;
    ASSERT_TRUE(remote.open(0));
    ASSERT_TRUE(remote.empty());
    remote.add(IClipboard::kText, "partial remote data");
    remote.close();
    failedPrivatePut = false;
    failPrivatePutNumber = 2;
    struct Reset { ~Reset() { failPrivatePutNumber = 0; } } reset;
    ASSERT_TRUE(worker.publish(kClipboardClipboard, std::make_shared<const String>(remote.marshall()), 81));
    std::vector<IScreen::ClipboardPublicationInfo> completions;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        completions = worker.takePublications();
        if (completions.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (completions.empty() && std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(failedPrivatePut.load());
    ASSERT_EQ(1u, completions.size());
    EXPECT_EQ(IScreen::ClipboardPublicationResult::Failed, completions[0].m_result);
    for (unsigned i = 0; i < 20; ++i) {
        worker.poll(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::shared_ptr<const String> snapshot;
    UInt32 sequence = 0;
    EXPECT_FALSE(worker.snapshot(&snapshot, &sequence));
}

TEST(OSXClipboardWorkerTests, staleReadCannotConsumeNewLocalChangeAheadOfQueuedRemote)
{
    OSXClipboard writer(privateName);
    ASSERT_TRUE(writer.open(0));
    ASSERT_TRUE(writer.empty());
    writer.add(IClipboard::kText, "old local A");
    writer.close();
    OSXClipboardWorker worker(privateName, OSXClipboardWorker::readInProcess);
    privateReadEntered = false;
    blockPrivateRead = true;
    struct Reset { ~Reset() { blockPrivateRead = false; injectLocalAfterPrivateRead = false; } } reset;
    worker.poll(1);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!privateReadEntered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(privateReadEntered.load());
    Clipboard remote;
    ASSERT_TRUE(remote.open(0));
    ASSERT_TRUE(remote.empty());
    remote.add(IClipboard::kText, "queued remote P");
    remote.close();
    ASSERT_TRUE(worker.publish(kClipboardClipboard, std::make_shared<const String>(remote.marshall()), 91));
    injectLocalAfterPrivateRead = true;
    blockPrivateRead = false;
    std::vector<IScreen::ClipboardPublicationInfo> completions;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        completions = worker.takePublications();
        if (completions.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (completions.empty() && std::chrono::steady_clock::now() < deadline);
    ASSERT_FALSE(injectLocalAfterPrivateRead.load());
    ASSERT_EQ(1u, completions.size());
    EXPECT_EQ(IScreen::ClipboardPublicationResult::Superseded, completions[0].m_result);
    writer.synchronize();
    ASSERT_TRUE(writer.open(0));
    EXPECT_EQ("local B copied after remote P was queued", writer.get(IClipboard::kText));
    writer.close();
}

TEST(OSXClipboardWorkerTests, publishesNativePngTextHtmlAndMaterializedFilePaths)
{
    OSXClipboardWorker worker(privateName, OSXClipboardWorker::readInProcess);
    const auto publish = [&](Clipboard& source, std::uint64_t token) {
        if (!worker.publish(kClipboardClipboard, std::make_shared<const String>(source.marshall()), token))
            return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        do {
            const auto completions = worker.takePublications();
            if (!completions.empty())
                return completions.size() == 1 && completions[0].m_publicationId == token &&
                    completions[0].m_result == IScreen::ClipboardPublicationResult::Succeeded;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    std::vector<unsigned char> encoded;
    ASSERT_EQ(0u, lodepng::encode(encoded, std::vector<unsigned char>{255, 32, 128, 255}, 1, 1));
    const std::string png(encoded.begin(), encoded.end());
    Clipboard source;
    ASSERT_TRUE(source.open(0));
    ASSERT_TRUE(source.empty());
    source.add(IClipboard::kText, "native image with text");
    source.add(IClipboard::kHTML, "<b>native image with text</b>");
    source.add(IClipboard::kPNG, png);
    source.close();
    ASSERT_TRUE(publish(source, 101));
    OSXClipboard reader(privateName);
    Clipboard actual;
    ASSERT_TRUE(reader.copyTo(&actual));
    ASSERT_TRUE(actual.open(0));
    EXPECT_EQ("native image with text", actual.get(IClipboard::kText));
    EXPECT_EQ("<b>native image with text</b>", actual.get(IClipboard::kHTML));
    EXPECT_EQ(png, actual.get(IClipboard::kPNG));
    EXPECT_FALSE(actual.has(IClipboard::kBitmap));
    actual.close();
    const auto isolatedImage = OSXClipboardSnapshot::read(privateName, [] { return true; });
    ASSERT_TRUE(isolatedImage.data);
    Clipboard copiedImage;
    copiedImage.unmarshall(*isolatedImage.data,0);
    ASSERT_TRUE(copiedImage.open(0));
    EXPECT_EQ(png,copiedImage.get(IClipboard::kPNG));
    EXPECT_EQ("native image with text",copiedImage.get(IClipboard::kText));
    copiedImage.close();

    char path[] = "/tmp/weave-native-clipboard-XXXXXX";
    const int file = mkstemp(path);
    ASSERT_GE(file, 0);
    close(file);
    struct RemoveFile { const char* path; ~RemoveFile() { unlink(path); } } removeFile{path};
    RemoteFileClipboard::Data paths;
    paths.mode = RemoteFileClipboard::Mode::MaterializedPaths;
    paths.sessionId = "00000000000000000000000000000001";
    paths.paths.push_back(barrier::fs::u8path(path));
    ASSERT_TRUE(source.open(0));
    ASSERT_TRUE(source.empty());
    source.add(IClipboard::kFileList, RemoteFileClipboard::serialize(paths));
    source.close();
    ASSERT_TRUE(publish(source, 102));
    reader.synchronize();
    ASSERT_TRUE(reader.copyTo(&actual));
    ASSERT_TRUE(actual.open(0));
    ASSERT_TRUE(actual.has(IClipboard::kFileList));
    RemoteFileClipboard::Data readPaths;
    ASSERT_TRUE(RemoteFileClipboard::parseLocalClipboard(actual.get(IClipboard::kFileList), readPaths));
    ASSERT_EQ(1u, readPaths.paths.size());
    EXPECT_EQ(path, readPaths.paths[0].u8string());
    actual.close();
    const auto isolatedFiles = OSXClipboardSnapshot::read(privateName, [] { return true; });
    ASSERT_TRUE(isolatedFiles.data);
    Clipboard copiedFiles;
    copiedFiles.unmarshall(*isolatedFiles.data,0);
    ASSERT_TRUE(copiedFiles.open(0));
    RemoteFileClipboard::Data processPaths;
    ASSERT_TRUE(RemoteFileClipboard::parseLocalClipboard(copiedFiles.get(IClipboard::kFileList),processPaths));
    ASSERT_EQ(1u,processPaths.paths.size());
    EXPECT_EQ(path,processPaths.paths[0].u8string());
    copiedFiles.close();
    EXPECT_TRUE(reader.empty());
}

TEST(OSXClipboardWorkerTests, cancelledReadRetainsNativeChangeEvidenceForNewLifetimePublication)
{
    OSXClipboard writer(privateName);
    ASSERT_TRUE(writer.open(0));
    ASSERT_TRUE(writer.empty());
    writer.add(IClipboard::kText, "old lifetime A");
    writer.close();
    OSXClipboardWorker worker(privateName, OSXClipboardWorker::readInProcess);
    privateReadEntered = false;
    blockPrivateRead = true;
    struct Reset { ~Reset() { blockPrivateRead = false; injectLocalAfterPrivateRead = false; } } reset;
    worker.poll(1);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!privateReadEntered.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(privateReadEntered.load());
    worker.cancel();
    Clipboard remote;
    ASSERT_TRUE(remote.open(0));
    ASSERT_TRUE(remote.empty());
    remote.add(IClipboard::kText, "new lifetime queued remote P");
    remote.close();
    ASSERT_TRUE(worker.publish(kClipboardClipboard, std::make_shared<const String>(remote.marshall()), 92));
    injectLocalAfterPrivateRead = true;
    blockPrivateRead = false;
    std::vector<IScreen::ClipboardPublicationInfo> completions;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        completions = worker.takePublications();
        if (completions.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (completions.empty() && std::chrono::steady_clock::now() < deadline);
    ASSERT_FALSE(injectLocalAfterPrivateRead.load());
    ASSERT_EQ(1u, completions.size());
    EXPECT_EQ(IScreen::ClipboardPublicationResult::Superseded, completions[0].m_result);
    writer.synchronize();
    ASSERT_TRUE(writer.open(0));
    EXPECT_EQ("local B copied after remote P was queued", writer.get(IClipboard::kText));
    writer.close();
}

TEST(OSXScreenClipboardTests, disablingSharingStopsNativePollingAndRejectsPublication)
{
    EventQueue events;
    OSXScreen screen(&events, false, false, privateName, OSXClipboardWorker::readInProcess);
    privateSynchronizes = 0;
    countPrivateSynchronizes = true;
    struct Reset { ~Reset() { countPrivateSynchronizes = false; } } reset;
    screen.setOptions({kOptionClipboardSharing, 0});
    for (int i=0; i<15; ++i) {
        screen.checkClipboards();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(0u, privateSynchronizes.load());
    Clipboard value;
    ASSERT_TRUE(value.open(0));
    ASSERT_TRUE(value.empty());
    value.add(IClipboard::kText, "must not be published while disabled");
    value.close();
    EXPECT_FALSE(screen.setClipboardSnapshot(kClipboardClipboard,
        std::make_shared<const String>(value.marshall()), 99));
}

TEST(OSXClipboardSnapshotTests, readsTextAndHtmlFromPrivateBoardInSeparateProcess)
{
    OSXClipboard writer(privateName);
    ASSERT_TRUE(writer.open(0));
    ASSERT_TRUE(writer.empty());
    const String text = u8"独立剪贴板 Café";
    writer.add(IClipboard::kText, text);
    writer.add(IClipboard::kHTML, "<b>private</b>");
    writer.close();
    const auto result = OSXClipboardSnapshot::read(privateName, [] { return true; });
    ASSERT_FALSE(result.timedOut);
    ASSERT_TRUE(result.data);
    Clipboard decoded;
    decoded.unmarshall(*result.data, 0);
    ASSERT_TRUE(decoded.open(0));
    EXPECT_EQ(text, decoded.get(IClipboard::kText));
    EXPECT_EQ("<b>private</b>", decoded.get(IClipboard::kHTML));
    decoded.close();
    EXPECT_TRUE(writer.empty());
}

namespace {
struct BlockedClipboardHelper {
    std::string directory;
    std::string executable;
    BlockedClipboardHelper() {
        char root[] = "/tmp/weave-clipboard-helper-test.XXXXXX";
        const char* name = mkdtemp(root);
        if (!name) throw std::runtime_error("failed to create isolated helper directory");
        directory = name;
        executable = directory + "/helper";
        std::ofstream file(executable);
        file << "#!/bin/sh\nif [ \"$1\" = --warmup ]; then exit 0; fi\nprintf '%s\\n' \"$$\" > '" << directory << "/pid'\nexec /bin/sleep 30\n";
        file.close();
        if (chmod(executable.c_str(), 0700) != 0) throw std::runtime_error("helper mode failed");
        // macOS cold script validation can exceed the one-second read test.
        // Warm separately so the test proves a recorded child was reaped.
        char* args[] = {const_cast<char*>(executable.c_str()), const_cast<char*>("--warmup"), nullptr};
        pid_t child = 0;
        if (posix_spawn(&child, executable.c_str(), nullptr, nullptr, args, environ) != 0)
            throw std::runtime_error("helper warmup spawn failed");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        int status = 0;
        pid_t reaped;
        while ((reaped = waitpid(child, &status, WNOHANG)) == 0 || (reaped < 0 && errno == EINTR)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                kill(child, SIGKILL);
                while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
                throw std::runtime_error("helper warmup timed out");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (reaped != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            throw std::runtime_error("helper warmup failed");
    }
    ~BlockedClipboardHelper() { barrier::fs::remove_all(directory); }
    pid_t pid() const { std::ifstream file(directory + "/pid"); pid_t value=0;file>>value;return value; }
};
}

TEST(OSXClipboardSnapshotTests, stalledHelperTimesOutWithoutEmptySnapshotAndIsReaped)
{
    BlockedClipboardHelper helper;
    const auto began = std::chrono::steady_clock::now();
    const auto result = OSXClipboardSnapshot::read(privateName, [] { return true; },
        std::chrono::milliseconds(1000), helper.executable);
    EXPECT_TRUE(result.timedOut);
    EXPECT_FALSE(result.data);
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count(), 3.0);
    const auto pid = helper.pid();
    ASSERT_GT(pid, 0);
    EXPECT_EQ(-1, waitpid(pid, nullptr, WNOHANG));
    EXPECT_EQ(ECHILD, errno);
}

TEST(OSXClipboardSnapshotTests, cancellationStopsStalledHelperBeforeItsDeadline)
{
    BlockedClipboardHelper helper;
    std::atomic<bool> current{true};
    auto read = std::async(std::launch::async, [&] {
        return OSXClipboardSnapshot::read(privateName, [&] { return current.load(); },
            std::chrono::seconds(10), helper.executable);
    });
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!helper.pid() && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto pid = helper.pid();
    current = false;
    EXPECT_EQ(std::future_status::ready, read.wait_for(std::chrono::seconds(1)));
    const auto result = read.get();
    EXPECT_FALSE(result.timedOut);
    EXPECT_FALSE(result.data);
    ASSERT_GT(pid, 0);
    EXPECT_EQ(-1, waitpid(pid, nullptr, WNOHANG));
    EXPECT_EQ(ECHILD, errno);
}

TEST(OSXClipboardWorkerTests, destructionCancelsAndReapsStalledSnapshotReader)
{
    BlockedClipboardHelper helper;
    auto worker = std::make_unique<OSXClipboardWorker>(privateName,
        [&](OSXClipboard&, CFStringRef name, const std::function<bool()>& current) {
            return OSXClipboardSnapshot::read(name, current, std::chrono::seconds(10), helper.executable);
        });
    worker->poll(1);
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (!helper.pid() && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto pid=helper.pid();
    const auto began=std::chrono::steady_clock::now();
    worker.reset();
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count(),1.0);
    ASSERT_GT(pid,0);
    EXPECT_EQ(-1,waitpid(pid,nullptr,WNOHANG));
    EXPECT_EQ(ECHILD,errno);
}

TEST(OSXClipboardWorkerTests, timedOutUnchangedProviderIsNotImmediatelyRetried)
{
    OSXClipboard writer(privateName);
    ASSERT_TRUE(writer.empty());
    writer.add(IClipboard::kText, "unchanged slow provider");
    std::atomic<unsigned> attempts{0};
    OSXClipboardWorker worker(privateName,
        [&](OSXClipboard&, CFStringRef, const std::function<bool()>&) {
            ++attempts;
            OSXClipboardSnapshotResult result;
            result.timedOut = true;
            return result;
        });
    for (int i=0; i<130; ++i) {
        worker.poll(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(1u,attempts.load());
    std::shared_ptr<const String> data;
    EXPECT_FALSE(worker.snapshot(&data,nullptr));
}

TEST(OSXClipboardWorkerTests, defaultProcessReaderPublishesPrivateSnapshot)
{
    OSXClipboard writer(privateName);
    ASSERT_TRUE(writer.empty());
    writer.add(IClipboard::kText, "default isolated reader");
    OSXClipboardWorker worker(privateName);
    worker.poll(73);
    UInt32 sequence=0;
    bool ready=false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
    while (!(ready=worker.takeReady(&sequence)) && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(ready);
    EXPECT_EQ(73u,sequence);
    std::shared_ptr<const String> data;
    ASSERT_TRUE(worker.snapshot(&data,nullptr));
    Clipboard decoded;
    decoded.unmarshall(*data,0);
    ASSERT_TRUE(decoded.open(0));
    EXPECT_EQ("default isolated reader",decoded.get(IClipboard::kText));
    decoded.close();
}

TEST(OSXClipboardSnapshotTests, truncatedHelperReplyIsNotAnEmptyClipboard)
{
    BlockedClipboardHelper helper;
    {
        std::ofstream file(helper.executable);
        file << "#!/bin/sh\nprintf 'broken' >&3\n";
    }
    const auto result=OSXClipboardSnapshot::read(privateName,[]{return true;},
        std::chrono::seconds(2),helper.executable);
    EXPECT_FALSE(result.data);
    EXPECT_FALSE(result.timedOut);
}

static void verifyNativePromisedDataDeadline()
{
    PasteboardRef board=nullptr;
    ASSERT_EQ(noErr,PasteboardCreate(kPasteboardUniqueName,&board));
    struct Board { PasteboardRef p; ~Board(){CFRelease(p);} } owner{board};
    ASSERT_EQ(noErr,PasteboardClear(board));
    struct State { std::atomic<bool> entered{false}, release{false}; } state;
    struct ClearKeeper { PasteboardRef p; ~ClearKeeper(){PasteboardSetPromiseKeeper(p,nullptr,nullptr);} } clearKeeper{board};
    const auto keeper=+[](PasteboardRef p, PasteboardItemID item, CFStringRef flavor, void* raw)->OSStatus {
        auto* s=static_cast<State*>(raw);
        s->entered=true;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while (!s->release && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const UInt8 bytes[]={'l','a','t','e'};
        CFDataRef data=CFDataCreate(nullptr,bytes,sizeof(bytes));
        const auto result=PasteboardPutItemFlavor(p,item,flavor,data,kPasteboardFlavorNoFlags);
        CFRelease(data);
        return result;
    };
    ASSERT_EQ(noErr,PasteboardSetPromiseKeeper(board,keeper,&state));
    ASSERT_EQ(noErr,PasteboardPutItemFlavor(board,reinterpret_cast<PasteboardItemID>(1),
        CFSTR("public.utf8-plain-text"),nullptr,kPasteboardFlavorNoFlags));
    CFStringRef name=nullptr;
    ASSERT_EQ(noErr,PasteboardCopyName(board,&name));
    struct Name { CFStringRef p; ~Name(){CFRelease(p);} } named{name};
    const auto began=std::chrono::steady_clock::now();
    auto pending=std::async(std::launch::async,[&] {
        auto result=OSXClipboardSnapshot::read(name,[]{return true;},std::chrono::seconds(2));
        state.release=true;
        return result;
    });
    while (pending.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.01,false);
    const auto result=pending.get();
    EXPECT_TRUE(state.entered.load());
    EXPECT_TRUE(result.timedOut);
    EXPECT_FALSE(result.data);
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count(),3.5);
    PasteboardSetPromiseKeeper(board,nullptr,nullptr);
}

TEST(OSXClipboardSnapshotTests, nativePromisedDataStallIsBoundedWithoutTouchingGeneralClipboard)
{
    // Cancelling a request while its Carbon promise callback is servicing IPC
    // leaves process-global pasteboard caches unsuitable for later tests on
    // macOS 26. Isolate the deliberately stalled provider in a fresh process,
    // just as the production reader is isolated from its external provider.
    ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
    EXPECT_EXIT({
        verifyNativePromisedDataDeadline();
        _exit(::testing::Test::HasFailure() ? 1 : 0);
    }, ::testing::ExitedWithCode(0), "");
}
