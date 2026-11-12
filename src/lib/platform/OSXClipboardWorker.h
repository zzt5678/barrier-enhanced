#pragma once

#include "platform/OSXClipboard.h"
#include "platform/OSXClipboardSnapshot.h"
#include "barrier/IScreen.h"
#include <memory>
#include <thread>
#include <vector>

// Owns all screen clipboard I/O. No native API or format conversion runs
// while holding the short-lived result/request mutex used by the input loop.
class OSXClipboardWorker {
public:
    using SnapshotReader = std::function<OSXClipboardSnapshotResult(
        OSXClipboard&, CFStringRef, const std::function<bool()>&)>;
    explicit OSXClipboardWorker(CFStringRef name, SnapshotReader reader = {});
    // Explicit injection for private-board/native-conversion tests.
    static OSXClipboardSnapshotResult readInProcess(OSXClipboard&, CFStringRef,
                                                   const std::function<bool()>&);
    ~OSXClipboardWorker();
    void poll(UInt32 sequence);
    // Cancels queued/preparing work and all notifications for this lifetime.
    // A native commit already admitted by its final check is not interruptible:
    // interrupting after Clear would leave an empty/partially filled board.
    void cancel();
    bool publish(ClipboardID id, const std::shared_ptr<const String>& data,
                 std::uint64_t publicationId);
    bool snapshot(std::shared_ptr<const String>* data, UInt32* sequence) const;
    bool takeReady(UInt32* sequence);
    std::vector<IScreen::ClipboardPublicationInfo> takePublications();

private:
    struct Context;
    void start();
    static void run(const std::shared_ptr<Context>& context);
    std::shared_ptr<Context> m_context;
    std::thread m_thread;
};
