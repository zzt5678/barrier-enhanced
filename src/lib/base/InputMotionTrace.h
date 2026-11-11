/* Optional, bounded pointer-only diagnostics. No key or clipboard contents. */
#pragma once

#if defined(__APPLE__) || defined(__linux__)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <fcntl.h>
#include <unistd.h>

namespace barrier {
// Called only by the protocol/event owner. Disabled unless explicitly enabled.
// Each process gets a new private file; the trace expires after ten minutes.
class InputMotionTrace {
public:
    InputMotionTrace() {
        const char* prefix = std::getenv("WEAVE_INPUT_TRACE_PREFIX");
        if (!prefix || !*prefix) return;
        const std::string path = std::string(prefix) + "." + std::to_string(getpid()) + ".csv";
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) return;
        file = fdopen(fd, "w");
        if (!file) { ::close(fd); return; }
        setvbuf(file, buffer, _IOFBF, sizeof(buffer));
        std::fputs("stage,epoch,sequence,x,y,wall_ns,mono_ns\n", file);
        until = Clock::now() + std::chrono::minutes(10);
    }
    ~InputMotionTrace() { if (file) std::fclose(file); }
    void record(char stage, unsigned epoch, unsigned sequence, int x, int y) {
        if (!file) return;
        const auto now = Clock::now();
        if (now >= until || rows >= 300000) {
            std::fclose(file); file = nullptr; return;
        }
        const auto wall = std::chrono::system_clock::now();
        std::fprintf(file, "%c,%u,%u,%d,%d,%lld,%lld\n", stage, epoch, sequence, x, y,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(wall.time_since_epoch()).count()),
            static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()));
        if (++rows % 64 == 0) std::fflush(file);
    }
private:
    using Clock = std::chrono::steady_clock;
    FILE* file = nullptr;
    char buffer[16384];
    unsigned rows = 0;
    Clock::time_point until;
};
inline void traceInputMotion(char stage, unsigned epoch, unsigned sequence, int x, int y) {
    static InputMotionTrace trace;
    trace.record(stage, epoch, sequence, x, y);
}
}
#else
namespace barrier {
inline void traceInputMotion(char, unsigned, unsigned, int, int) {}
}
#endif
