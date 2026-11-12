#pragma once
#include "platform/OSXClipboard.h"
#include <chrono>
#include <functional>
#include <memory>

struct OSXClipboardSnapshotResult {
    std::shared_ptr<const String> data;
    OSXClipboard::ReadTimings timings;
    double marshallingMilliseconds = 0;
    bool timedOut = false;
};

// Foreign pasteboard providers may block inside an uninterruptible native API.
// Run data reads in a disposable child; only bounded, validated bytes cross back.
class OSXClipboardSnapshot {
public:
    static OSXClipboardSnapshotResult read(CFStringRef name,
        const std::function<bool()>& current,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(2000),
        const std::string& executable = {});
    static int runHelper(const char* name, int output = 3);
};
