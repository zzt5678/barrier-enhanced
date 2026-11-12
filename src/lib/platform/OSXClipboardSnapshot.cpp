#include "platform/OSXClipboardSnapshot.h"
#include "barrier/Clipboard.h"
#include "barrier/ClipboardChunk.h"
#include "base/Log.h"
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <array>
#include <vector>
extern char** environ;

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint64_t magic = 0x574556434c495031ULL;
using Header = std::array<uint64_t, 5>; // magic, length, native/conversion/marshal us
std::string utf8(CFStringRef value) {
    if (!value) value = kPasteboardClipboard;
    const auto size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
    std::vector<char> text(size);
    return CFStringGetCString(value, text.data(), size, kCFStringEncodingUTF8) ? text.data() : "";
}
std::string helperPath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    const std::string current(path.data());
    const auto slash = current.find_last_of('/');
    return slash == std::string::npos ? std::string() : current.substr(0, slash + 1) + "weavec";
}
bool writeAll(int fd, const void* bytes, size_t size) {
    const char* p = static_cast<const char*>(bytes);
    while (size) {
        const auto n = ::write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= n;
    }
    return true;
}
struct Child {
    pid_t pid;
    int fd;
    ~Child() {
        ::close(fd);
        // Read-only child: safe to stop even if native cleanup/provider IPC hangs.
        // An unreaped child PID cannot be reused for an unrelated process.
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
};
}

OSXClipboardSnapshotResult OSXClipboardSnapshot::read(CFStringRef name,
    const std::function<bool()>& current, std::chrono::milliseconds timeout,
    const std::string& executable)
{
    OSXClipboardSnapshotResult result;
    if (current && !current()) return result;
    const auto until = Clock::now() + timeout;
    const std::string path = executable.empty() ? helperPath() : executable;
    const std::string board = utf8(name);
    if (path.empty() || board.empty()) return result;
    int pipefd[2];
    if (::pipe(pipefd) != 0) return result;
    if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(pipefd[0], F_SETFL, O_NONBLOCK) < 0) {
        ::close(pipefd[0]); ::close(pipefd[1]); return result;
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        ::close(pipefd[0]); ::close(pipefd[1]); return result;
    }
    int setupError = 0;
    const auto checked = [&](int error) { if (error && !setupError) setupError = error; };
    checked(posix_spawn_file_actions_adddup2(&actions, pipefd[1], 3));
    if (pipefd[0] != 3) checked(posix_spawn_file_actions_addclose(&actions, pipefd[0]));
    if (pipefd[1] != 3) checked(posix_spawn_file_actions_addclose(&actions, pipefd[1]));
    checked(posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0));
    checked(posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0));
    checked(posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0));
    posix_spawnattr_t attrs;
    if (posix_spawnattr_init(&attrs) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        ::close(pipefd[0]); ::close(pipefd[1]); return result;
    }
    checked(posix_spawnattr_setflags(&attrs, POSIX_SPAWN_CLOEXEC_DEFAULT));
    char* args[] = {const_cast<char*>(path.c_str()), const_cast<char*>("--clipboard-snapshot"),
                   const_cast<char*>(board.c_str()), nullptr};
    pid_t pid = 0;
    const int error = setupError ? setupError :
        posix_spawn(&pid, path.c_str(), &actions, &attrs, args, environ);
    posix_spawnattr_destroy(&attrs);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipefd[1]);
    if (error != 0) {
        ::close(pipefd[0]);
        LOG((CLOG_WARN "could not start macOS clipboard reader, error=%d", error));
        return result;
    }
    Child child{pid, pipefd[0]};
    std::string bytes;
    Header header{};
    size_t expected = 0;
    char buffer[65536];
    for (;;) {
        if (current && !current()) return result;
        if (Clock::now() >= until) { result.timedOut = true; return result; }
        const auto n = ::read(child.fd, buffer, sizeof(buffer));
        if (n > 0) {
            if (bytes.size() + size_t(n) > ClipboardChunk::kMaxReceiveSize + sizeof(Header)) return result;
            bytes.append(buffer, size_t(n));
            if (!expected && bytes.size() >= sizeof(Header)) {
                std::memcpy(header.data(), bytes.data(), sizeof(Header));
                if (header[0] != magic || !header[1] || header[1] > ClipboardChunk::kMaxReceiveSize) return result;
                expected = sizeof(Header) + size_t(header[1]);
            }
            if (expected && bytes.size() >= expected) {
                if (bytes.size() != expected) return result;
                String data(bytes.data() + sizeof(Header), size_t(header[1]));
                if (!Clipboard::isValidMarshalled(data)) return result;
                result.data = std::make_shared<const String>(std::move(data));
                result.timings.nativeReadMilliseconds = header[2] / 1000.0;
                result.timings.conversionMilliseconds = header[3] / 1000.0;
                result.marshallingMilliseconds = header[4] / 1000.0;
                return result;
            }
        }
        else if (n == 0) return result;
        else if (errno != EINTR && errno != EAGAIN) return result;
        else {
            pollfd fd{child.fd, POLLIN, 0};
            ::poll(&fd, 1, 10);
        }
    }
}

int OSXClipboardSnapshot::runHelper(const char* name, int output)
{
    if (!name || !*name || fcntl(output, F_GETFD) < 0) return 2;
    // The parent normally enforces two seconds. Keep a slightly wider local
    // deadline too, so a crashed/killed parent cannot orphan a native read.
    sigset_t alarms;
    sigemptyset(&alarms);
    sigaddset(&alarms, SIGALRM);
    signal(SIGALRM, SIG_DFL);
    pthread_sigmask(SIG_UNBLOCK, &alarms, nullptr);
    alarm(3);
    struct Deadline { ~Deadline() { alarm(0); } } deadline;
    CFStringRef board = CFStringCreateWithCString(nullptr, name, kCFStringEncodingUTF8);
    if (!board) return 2;
    struct Release { CFStringRef value; ~Release() { CFRelease(value); } } release{board};
    try {
        OSXClipboard source(board);
        Clipboard destination;
        if (!source.copyTo(&destination) || source.synchronize()) return 1;
        const auto began = Clock::now();
        const String bytes = destination.marshall();
        const auto marshal = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-began).count();
        if (bytes.size() > ClipboardChunk::kMaxReceiveSize) return 1;
        const auto timings = source.lastReadTimings();
        Header header{{magic, bytes.size(), uint64_t(timings.nativeReadMilliseconds*1000),
            uint64_t(timings.conversionMilliseconds*1000), uint64_t(marshal)}};
        return writeAll(output, header.data(), sizeof(header)) &&
            writeAll(output, bytes.data(), bytes.size()) ? 0 : 1;
    }
    catch (...) { return 1; }
}
