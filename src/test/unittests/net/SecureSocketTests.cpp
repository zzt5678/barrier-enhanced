#define BARRIER_TEST_ENV
#include "net/SecureSocket.h"

#include "arch/Arch.h"
#include "base/EventTypes.h"
#include "base/Log.h"
#include "net/ISocketMultiplexerJob.h"
#include "net/NetworkAddress.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPSocketFactory.h"
#include "net/SecureListenSocket.h"
#include "net/SecureUtils.h"
#include "net/FingerprintDatabase.h"
#include "common/DataDirectories.h"
#include "net/XSocket.h"
#include "mt/Lock.h"
#include "test/global/gmock.h"
#include "test/global/gtest.h"
#include "test/mock/barrier/MockEventQueue.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>
#include <algorithm>
#include <ctime>
#include <cstdint>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#if SYSAPI_UNIX
#include <unistd.h>
#endif

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

namespace {

double processCpuSeconds()
{
#if SYSAPI_WIN32
    // MSVC clock() measures elapsed wall time, unlike the Unix CPU clock.
    // GetProcessTimes reports accumulated kernel/user CPU in 100ns units.
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        throw std::runtime_error("cannot read process CPU time");
    }
    const auto ticks = [](const FILETIME& value) {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) |
            static_cast<std::uint64_t>(value.dwLowDateTime);
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10000000.0;
#else
    const auto ticks = std::clock();
    if (ticks == static_cast<std::clock_t>(-1)) {
        throw std::runtime_error("cannot read process CPU time");
    }
    return static_cast<double>(ticks) / CLOCKS_PER_SEC;
#endif
}

class CallbackMultiplexerJob : public ISocketMultiplexerJob {
public:
    CallbackMultiplexerJob(ArchSocket socket,
                           std::function<void()> callback,
                           std::atomic<int>& destructionCount) :
        m_socket(socket),
        m_callback(std::move(callback)),
        m_destructionCount(destructionCount)
    {
    }

    ~CallbackMultiplexerJob() override
    {
        m_destructionCount.fetch_add(1, std::memory_order_release);
    }

    MultiplexerJobStatus run(bool, bool, bool) override
    {
        m_callback();
        return {false, {}};
    }

    ArchSocket getSocket() const override { return m_socket; }
    bool isReadable() const override { return true; }
    bool isWritable() const override { return false; }

private:
    ArchSocket m_socket;
    std::function<void()> m_callback;
    std::atomic<int>& m_destructionCount;
};

class ConnectedSocketPair {
public:
    ConnectedSocketPair()
    {
        ArchSocket listener = nullptr;
        try {
            NetworkAddress address;
            for (int port = 41000; port < 42000; ++port) {
                listener = ARCH->newSocket(
                    IArchNetwork::kINET, IArchNetwork::kSTREAM);
                try {
                    address = NetworkAddress("127.0.0.1", port);
                    address.resolve();
                    ARCH->setReuseAddrOnSocket(listener, true);
                    ARCH->bindSocket(listener, address.getAddress());
                    ARCH->listenOnSocket(listener);
                    break;
                }
                catch (...) {
                    ARCH->closeSocket(listener);
                    listener = nullptr;
                }
            }

            if (listener == nullptr) {
                throw std::runtime_error("cannot bind loopback test socket");
            }

            m_peer = ARCH->newSocket(
                IArchNetwork::kINET, IArchNetwork::kSTREAM);
            ARCH->connectSocket(m_peer, address.getAddress());

            IArchNetwork::PollEntry entry;
            entry.m_socket = listener;
            entry.m_events = IArchNetwork::kPOLLIN;
            entry.m_revents = 0;
            if (ARCH->pollSocket(&entry, 1, 2.0) == 0) {
                throw std::runtime_error("loopback test connection timed out");
            }

            m_socket = ARCH->acceptSocket(listener, nullptr);
            if (m_socket == nullptr) {
                throw std::runtime_error("cannot accept loopback test connection");
            }
            ARCH->closeSocket(listener);
            listener = nullptr;
        }
        catch (...) {
            if (listener != nullptr) {
                ARCH->closeSocket(listener);
            }
            close();
            throw;
        }
    }

    ~ConnectedSocketPair()
    {
        close();
    }

    ArchSocket releaseSocket()
    {
        ArchSocket socket = m_socket;
        m_socket = nullptr;
        return socket;
    }

    ArchSocket peer() const { return m_peer; }

    void signalReadable()
    {
        const char byte = 1;
        const double deadline = ARCH->time() + 2.0;
        size_t written = 0;
        while (written == 0 && ARCH->time() < deadline) {
            written = ARCH->writeSocket(m_peer, &byte, sizeof(byte));
            ARCH->sleep(0.001);
        }
        if (written == 0) {
            throw std::runtime_error("cannot signal loopback test socket");
        }
    }

private:
    void close()
    {
        if (m_socket != nullptr) {
            ARCH->closeSocket(m_socket);
            m_socket = nullptr;
        }
        if (m_peer != nullptr) {
            ARCH->closeSocket(m_peer);
            m_peer = nullptr;
        }
    }

    ArchSocket m_socket = nullptr;
    ArchSocket m_peer = nullptr;
};

class TestableSecureSocket : public SecureSocket {
public:
    TestableSecureSocket(IEventQueue* events,
                         SocketMultiplexer* multiplexer) :
        SecureSocket(events, multiplexer, IArchNetwork::kINET,
                     ConnectionSecurityLevel::ENCRYPTED)
    {
    }

    TestableSecureSocket(IEventQueue* events,
                         SocketMultiplexer* multiplexer,
                         ArchSocket socket) :
        SecureSocket(events, multiplexer, socket,
                     ConnectionSecurityLevel::ENCRYPTED)
    {
    }

    void markConnected()
    {
        m_connected = true;
        m_readable = true;
        m_writable = true;
    }

    void markSecureConnected()
    {
        markConnected();
        testSetSecureReadyNoLock();
    }

    bool readStopsJobWithLock()
    {
        Lock lock(&getMutex());
        return doRead() == kBreak;
    }

    void failTLSWithLock()
    {
        Lock lock(&getMutex());
        testDisconnectTLSFailureNoLock();
    }

    void failPermanentTLSWithLock()
    {
        Lock lock(&getMutex());
        testDisconnectPermanentTLSFailureNoLock();
    }

    void exhaustSecureConnectRetriesWithLock()
    {
        Lock lock(&getMutex());
        testSecureConnectRetryExhaustedNoLock();
    }

    void installFailingJob(std::atomic<bool>& invoked,
                           std::atomic<int>& destructionCount)
    {
        setJob(std::make_unique<CallbackMultiplexerJob>(
            rawSocket(),
            [this, &invoked]() {
                failTLSWithLock();
                invoked.store(true, std::memory_order_release);
            },
            destructionCount));
    }

    bool connected() const { return m_connected; }
    bool readable() const { return m_readable; }
    bool writable() const { return m_writable; }
    ArchSocket rawSocket() { return getSocket(); }
    void pauseService() { removeJob(); }
    void resumeService() { setJob(newJob()); }
};

class DeadlineJob : public ISocketMultiplexerJob {
public:
    DeadlineJob(ArchSocket socket, double deadline, std::atomic<int>& calls) :
        socket_(socket), deadline_(deadline), calls_(calls) {}
    MultiplexerJobStatus run(bool, bool, bool) override
    {
        ++calls_;
        return {false, {}};
    }
    ArchSocket getSocket() const override { return socket_; }
    bool isReadable() const override { return true; }
    bool isWritable() const override { return false; }
    double getDeadline() const override { return deadline_; }
private:
    ArchSocket socket_;
    double deadline_;
    std::atomic<int>& calls_;
};

class BudgetTCPSocket : public TCPSocket {
public:
    BudgetTCPSocket(IEventQueue* events, SocketMultiplexer* mux, ArchSocket socket) :
        TCPSocket(events, mux, socket) { removeJob(); }
    void readOnce() { Lock lock(&getMutex()); doRead(); }
    void resume() { setJob(newJob()); }
};

#if SYSAPI_UNIX
class TemporaryCertificate {
public:
    TemporaryCertificate()
    {
        char path[] = "/tmp/weave-network-cert-XXXXXX";
        const int fd = mkstemp(path);
        if (fd < 0) { throw std::runtime_error("cannot create test certificate"); }
        ::close(fd);
        path_ = path;
        barrier::generate_pem_self_signed_cert(path_);
    }
    ~TemporaryCertificate() { std::remove(path_.c_str()); }
    void enlargeForBackpressure()
    {
        // A real, signed certificate larger than the test transport's buffers
        // forces SSL_accept to suspend while writing its handshake flight.
        std::unique_ptr<FILE, decltype(&std::fclose)> input(std::fopen(path_.c_str(), "r"), std::fclose);
        if (!input) { throw std::runtime_error("cannot open test certificate"); }
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
            PEM_read_PrivateKey(input.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(
            PEM_read_X509(input.get(), nullptr, nullptr, nullptr), X509_free);
        std::unique_ptr<ASN1_OBJECT, decltype(&ASN1_OBJECT_free)> oid(
            OBJ_txt2obj("1.3.6.1.4.1.55555.1", 1), ASN1_OBJECT_free);
        std::unique_ptr<ASN1_OCTET_STRING, decltype(&ASN1_OCTET_STRING_free)> padding(
            ASN1_OCTET_STRING_new(), ASN1_OCTET_STRING_free);
        const std::string bytes(64 * 1024, 'x');
        if (!key || !cert || !oid || !padding ||
            !ASN1_OCTET_STRING_set(padding.get(),
                reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size())) {
            throw std::runtime_error("cannot prepare backpressure certificate");
        }
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
            X509_EXTENSION_create_by_OBJ(nullptr, oid.get(), 0, padding.get()), X509_EXTENSION_free);
        if (!extension || !X509_add_ext(cert.get(), extension.get(), -1) ||
            !X509_sign(cert.get(), key.get(), EVP_sha256())) {
            throw std::runtime_error("cannot sign backpressure certificate");
        }
        input.reset();
        std::unique_ptr<FILE, decltype(&std::fclose)> output(std::fopen(path_.c_str(), "w"), std::fclose);
        if (!output || !PEM_write_PrivateKey(output.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) ||
            !PEM_write_X509(output.get(), cert.get())) {
            throw std::runtime_error("cannot write backpressure certificate");
        }
    }
    std::string path_;
};

class BudgetSecureSocket : public TestableSecureSocket {
public:
    using TestableSecureSocket::TestableSecureSocket;
    void readOnce() { Lock lock(&getMutex()); doRead(); }
    bool secureReady() { Lock lock(&getMutex()); return isSecureReady(); }
    bool delayFirstRead = false;
protected:
    int secureReadForInput(void* buffer, int size, int& bytes) override
    {
        const int result = SecureSocket::secureReadForInput(buffer, size, bytes);
        // Emulate expensive record processing while retaining a real SSL_read.
        if (delayFirstRead && bytes > 0) {
            delayFirstRead = false;
            ARCH->sleep(0.003);
        }
        return result;
    }
};

class TemporaryTlsProfile {
public:
    TemporaryTlsProfile() : saved_(barrier::DataDirectories::profile())
    {
        char path[] = "/tmp/weave-network-profile-XXXXXX";
        if (!mkdtemp(path)) { throw std::runtime_error("cannot create test profile"); }
        root_ = path;
        barrier::DataDirectories::profile(root_);
        barrier::fs::create_directories(barrier::DataDirectories::ssl_fingerprints_path());
        const auto cert = barrier::DataDirectories::ssl_certificate_path();
        barrier::generate_pem_self_signed_cert(cert.string());
        barrier::FingerprintDatabase db;
        db.add_trusted(barrier::get_pem_file_cert_fingerprint(
            cert.string(), barrier::FingerprintType::SHA256));
        db.write(barrier::DataDirectories::trusted_servers_ssl_fingerprints_path());
    }
    ~TemporaryTlsProfile()
    {
        barrier::DataDirectories::profile(saved_);
        std::error_code error;
        barrier::fs::remove_all(root_, error);
    }
private:
    barrier::fs::path saved_;
    barrier::fs::path root_;
};

struct ControlProbeState {
    std::atomic<double> sentAt{0};
    std::atomic<double> maxDelay{0};
    std::atomic<unsigned> received{0};
    std::atomic<bool> outstanding{false};
};

class ControlProbeJob : public ISocketMultiplexerJob {
public:
    ControlProbeJob(ArchSocket socket, std::shared_ptr<ControlProbeState> state) :
        socket_(ARCH->copySocket(socket)), state_(std::move(state)) {}
    ~ControlProbeJob() override { ARCH->closeSocket(socket_); }
    MultiplexerJobStatus run(bool read, bool, bool error) override
    {
        if (error) { return {false, {}}; }
        if (read) {
            char byte;
            if (ARCH->readSocket(socket_, &byte, 1) == 1) {
                const double delay = ARCH->time() - state_->sentAt.load();
                double old = state_->maxDelay.load();
                while (delay > old && !state_->maxDelay.compare_exchange_weak(old, delay)) {}
                ++state_->received;
                state_->outstanding = false;
            }
        }
        return {true, {}};
    }
    ArchSocket getSocket() const override { return socket_; }
    bool isReadable() const override { return true; }
    bool isWritable() const override { return false; }
private:
    ArchSocket socket_;
    std::shared_ptr<ControlProbeState> state_;
};

class ScopedLogFilter {
public:
    explicit ScopedLogFilter(int level) : saved_(CLOG->getFilter()) { CLOG->setFilter(level); }
    ~ScopedLogFilter() { CLOG->setFilter(saved_); }
private:
    int saved_;
};

bool finishTlsClientHandshake(SSL* client, BudgetSecureSocket& socket)
{
    const double end = ARCH->time() + 4;
    int result = 0;
    while (ARCH->time() < end) {
        result = SSL_connect(client);
        if (result == 1) { break; }
        const int error = SSL_get_error(client, result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) { return false; }
        ARCH->sleep(0.001);
    }
    if (result != 1) { return false; }
    while (!socket.secureReady() && ARCH->time() < end) { ARCH->sleep(0.001); }
    return socket.secureReady();
}
#endif

class PayloadThenFatalSecureSocket : public TestableSecureSocket {
public:
    PayloadThenFatalSecureSocket(IEventQueue* events,
                                 SocketMultiplexer* multiplexer) :
        TestableSecureSocket(events, multiplexer)
    {
    }

    int secureReadForInput(void* buffer, int size, int& bytesRead) override
    {
        if (m_readCount++ == 0) {
            const int payloadSize = static_cast<int>(sizeof(kPayload) - 1);
            if (size < payloadSize) {
                throw std::runtime_error("scripted secure read buffer is too small");
            }
            std::memcpy(buffer, kPayload, payloadSize);
            bytesRead = payloadSize;
            return payloadSize;
        }

        isFatal(true);
        bytesRead = -1;
        return -1;
    }

    static constexpr char kPayload[] = "tail";

private:
    int m_readCount = 0;
};

class FatalSecureSocket : public TestableSecureSocket {
public:
    FatalSecureSocket(IEventQueue* events, SocketMultiplexer* multiplexer) :
        TestableSecureSocket(events, multiplexer)
    {
    }

    int secureReadForInput(void*, int, int& bytesRead) override
    {
        isFatal(true);
        bytesRead = -1;
        return -1;
    }
};

struct EventCounts {
    int stopRetry = 0;
    int disconnected = 0;
    int inputShutdown = 0;
};

void setEventDefaults(MockEventQueue& events,
                      ISocketEvents& socketEvents,
                      IStreamEvents& streamEvents,
                      IDataSocketEvents& dataSocketEvents)
{
    socketEvents.setEvents(&events);
    streamEvents.setEvents(&events);
    dataSocketEvents.setEvents(&events);

    ON_CALL(events, forISocket()).WillByDefault(ReturnRef(socketEvents));
    ON_CALL(events, forIStream()).WillByDefault(ReturnRef(streamEvents));
    ON_CALL(events, forIDataSocket()).WillByDefault(ReturnRef(dataSocketEvents));
    ON_CALL(events, removeHandler(_, _)).WillByDefault(Return());

    auto nextEventType = std::make_shared<int>(100);
    ON_CALL(events, registerTypeOnce(_, _))
        .WillByDefault(Invoke([nextEventType](Event::Type& type, const char*) {
            if (type == Event::kUnknown) {
                type = (*nextEventType)++;
            }
            return type;
        }));
}

void countEvent(EventCounts& counts,
                Event::Type stopRetryType,
                Event::Type disconnectedType,
                Event::Type inputShutdownType,
                void* target,
                const Event& event)
{
    if (event.getTarget() != target) {
        return;
    }

    if (event.getType() == stopRetryType) {
        ++counts.stopRetry;
    }
    else if (event.getType() == disconnectedType) {
        ++counts.disconnected;
    }
    else if (event.getType() == inputShutdownType) {
        ++counts.inputShutdown;
    }
}

}

TEST(SocketFairnessTests, deadlineRunsWithoutSocketReadiness)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);
    SocketMultiplexer multiplexer;
    ConnectedSocketPair pair;
    TestableSecureSocket owner(&events, &multiplexer, pair.releaseSocket());
    owner.pauseService();
    std::atomic<int> calls{0};
    multiplexer.addSocket(&owner, std::make_unique<DeadlineJob>(
        owner.rawSocket(), ARCH->time() + 0.05, calls));
    const double end = ARCH->time() + 0.4;
    while (calls.load() == 0 && ARCH->time() < end) {
        ARCH->sleep(0.001);
    }
    owner.pauseService();
    EXPECT_EQ(1, calls.load());
}

TEST(SocketFairnessTests, tcpReadBudgetYieldsAndContinuesWithoutLosingData)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);
    SocketMultiplexer multiplexer;
    ConnectedSocketPair pair;
    BudgetTCPSocket socket(&events, &multiplexer, pair.releaseSocket());
    const std::string payload(512 * 1024, 'p');
    const size_t sent = ARCH->writeSocket(pair.peer(), payload.data(), payload.size());
    ASSERT_GT(sent, 64u * 1024u);
    ARCH->sleep(0.02);
    socket.readOnce();
    const auto firstPass = socket.getSize();
    std::printf("network TCP first callback buffered=%u bytes\n", firstPass);
    EXPECT_GT(firstPass, 0u);
    EXPECT_LE(firstPass, 64u * 1024u);
    socket.resume();
    const double end = ARCH->time() + 2;
    while (socket.getSize() < sent && ARCH->time() < end) {
        ARCH->sleep(0.001);
    }
    std::string received(sent, '\0');
    EXPECT_EQ(sent, socket.read(&received[0], received.size()));
    EXPECT_TRUE(payload.compare(0, sent, received) == 0);
}

TEST(SocketFairnessTests, stalledTlsPeersDoNotDelayHealthySocket)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);
    SocketMultiplexer multiplexer;
    std::vector<std::unique_ptr<ConnectedSocketPair>> peers;
    std::vector<std::unique_ptr<TestableSecureSocket>> stalled;
    for (int i = 0; i < 3; ++i) {
        peers.emplace_back(new ConnectedSocketPair);
        stalled.emplace_back(new TestableSecureSocket(
            &events, &multiplexer, peers.back()->releaseSocket()));
        stalled.back()->initSsl(true);
        stalled.back()->secureAccept();
        const char partialClientHello = 0x16;
        ASSERT_EQ(1u, ARCH->writeSocket(peers.back()->peer(), &partialClientHello, 1));
    }
    ARCH->sleep(0.05);
    ConnectedSocketPair healthyPair;
    TestableSecureSocket healthy(&events, &multiplexer, healthyPair.releaseSocket());
    healthy.pauseService();
    double total = 0;
    double worst = 0;
    for (int trial = 0; trial < 8; ++trial) {
        std::atomic<int> calls{0};
        std::atomic<int> destroyed{0};
        std::atomic<double> callbackAt{0.0};
        multiplexer.addSocket(&healthy, std::make_unique<CallbackMultiplexerJob>(
            healthy.rawSocket(), [&]() {
                callbackAt.store(ARCH->time(), std::memory_order_relaxed);
                calls.fetch_add(1, std::memory_order_release);
            }, destroyed));
        const double start = ARCH->time();
        healthyPair.signalReadable();
        while (calls.load(std::memory_order_acquire) == 0 && ARCH->time() - start < 1) {
            ARCH->sleep(0.0001);
        }
        // Stop the job before any assertion can unwind its captured state.
        healthy.pauseService();
        ASSERT_GT(calls.load(std::memory_order_acquire), 0);
        const double elapsed = callbackAt.load(std::memory_order_relaxed) - start;
        total += elapsed;
        worst = (std::max)(worst, elapsed);
        char byte;
        ARCH->readSocket(healthy.rawSocket(), &byte, 1);
    }
    const double cpuStart = processCpuSeconds();
    ARCH->sleep(0.2);
    const double cpuSeconds = processCpuSeconds() - cpuStart;
    std::printf("network fairness: 3 stalled TLS peers multiplexer callback latency "
                "mean=%.3fms max=%.3fms idleCPU=%.3fms/200ms\n",
                total * 1000 / 8, worst * 1000, cpuSeconds * 1000);
    EXPECT_LT(total / 8, 0.015);
    EXPECT_LT(cpuSeconds, 0.05);
}

#if SYSAPI_UNIX
TEST(SocketFairnessTests, sustainedTlsBulkMeasuresThroughputAndControlLatency)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    ClientListenerEvents listenerEvents;
    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);
    listenerEvents.setEvents(&events);
    ON_CALL(events, forClientListener()).WillByDefault(ReturnRef(listenerEvents));
    SocketMultiplexer multiplexer;
    TemporaryCertificate cert;
    ConnectedSocketPair pair;
    BudgetSecureSocket socket(&events, &multiplexer, pair.releaseSocket());
    socket.initSsl(true);
    ASSERT_TRUE(socket.load_certificates(cert.path_));
    socket.secureAccept();
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx(SSL_CTX_new(TLS_method()), SSL_CTX_free);
    ASSERT_NE(nullptr, ctx.get());
    std::unique_ptr<SSL, decltype(&SSL_free)> client(SSL_new(ctx.get()), SSL_free);
    ASSERT_NE(nullptr, client.get());
    ASSERT_EQ(1, SSL_set_fd(client.get(), pair.peer()->m_fd));
    ASSERT_TRUE(finishTlsClientHandshake(client.get(), socket));

    const auto probe = std::make_shared<ControlProbeState>();
    ConnectedSocketPair controlPair;
    TestableSecureSocket control(&events, &multiplexer, controlPair.releaseSocket());
    control.pauseService();
    multiplexer.addSocket(&control, std::make_unique<ControlProbeJob>(control.rawSocket(), probe));
    // Measure transfer work, without per-record debug formatting dominating it.
    ScopedLogFilter filter(kWARNING);
    const std::string payload(64 * 1024, 'b');
    std::vector<char> received(1024 * 1024);
    size_t sentBytes = 0;
    size_t receivedBytes = 0;
    bool contentMatches = true;
    unsigned sentProbes = 0;
    const double start = ARCH->time();
    double nextProbe = start;
    bool pendingWrite = false;
    while ((ARCH->time() - start < 0.75 || pendingWrite) && ARCH->time() - start < 3) {
        const int written = SSL_write(client.get(), payload.data(), payload.size());
        if (written > 0) {
            sentBytes += written;
            pendingWrite = false;
        }
        else {
            const int error = SSL_get_error(client.get(), written);
            ASSERT_TRUE(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
            pendingWrite = true;
        }
        const auto count = socket.read(received.data(), received.size());
        receivedBytes += count;
        contentMatches = contentMatches && std::all_of(received.begin(), received.begin() + count,
                                                       [](char byte) { return byte == 'b'; });
        const double now = ARCH->time();
        if (now >= nextProbe && !probe->outstanding.load()) {
            probe->sentAt = now;
            probe->outstanding = true;
            const char byte = 1;
            ASSERT_EQ(1u, ARCH->writeSocket(controlPair.peer(), &byte, 1));
            ++sentProbes;
            nextProbe = now + 0.002;
        }
        if (written <= 0 && count == 0) { ARCH->sleep(0.0001); }
    }
    const double drainEnd = ARCH->time() + 2;
    while ((receivedBytes < sentBytes || probe->outstanding.load()) && ARCH->time() < drainEnd) {
        const auto count = socket.read(received.data(), received.size());
        receivedBytes += count;
        contentMatches = contentMatches && std::all_of(received.begin(), received.begin() + count,
                                                       [](char byte) { return byte == 'b'; });
        ARCH->sleep(0.0001);
    }
    const double elapsed = ARCH->time() - start;
    control.pauseService();
    std::printf("network sustained TLS: delivered=%zu bytes elapsed=%.6fs throughput=%.2fMiB/s "
                "control=%u/%u maxDelay=%.3fms\n", receivedBytes, elapsed,
                receivedBytes / (1024.0 * 1024.0 * elapsed), probe->received.load(), sentProbes,
                probe->maxDelay.load() * 1000);
    EXPECT_EQ(sentBytes, receivedBytes);
    EXPECT_FALSE(pendingWrite);
    EXPECT_GE(receivedBytes, 16u * 1024u * 1024u);
    EXPECT_TRUE(contentMatches);
    EXPECT_EQ(sentProbes, probe->received.load());
    EXPECT_GE(probe->received.load(), 20u);
    EXPECT_LT(probe->maxDelay.load(), 0.1);
}

TEST(SocketFairnessTests, tlsHandshakeResumesAfterWriteBackpressure)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    ClientListenerEvents listenerEvents;
    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);
    listenerEvents.setEvents(&events);
    ON_CALL(events, forClientListener()).WillByDefault(ReturnRef(listenerEvents));
    SocketMultiplexer multiplexer;
    TemporaryCertificate cert;
    cert.enlargeForBackpressure();
    ConnectedSocketPair pair;
    BudgetSecureSocket socket(&events, &multiplexer, pair.releaseSocket());
    const int smallBuffer = 4096;
    ASSERT_EQ(0, setsockopt(socket.rawSocket()->m_fd, SOL_SOCKET, SO_SNDBUF,
                            &smallBuffer, sizeof(smallBuffer)));
    ASSERT_EQ(0, setsockopt(pair.peer()->m_fd, SOL_SOCKET, SO_RCVBUF,
                            &smallBuffer, sizeof(smallBuffer)));
    socket.initSsl(true);
    ASSERT_TRUE(socket.load_certificates(cert.path_));
    socket.secureAccept();
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx(SSL_CTX_new(TLS_method()), SSL_CTX_free);
    ASSERT_NE(nullptr, ctx.get());
    std::unique_ptr<SSL, decltype(&SSL_free)> client(SSL_new(ctx.get()), SSL_free);
    ASSERT_NE(nullptr, client.get());
    ASSERT_EQ(1, SSL_set_fd(client.get(), pair.peer()->m_fd));
    const int first = SSL_connect(client.get());
    ASSERT_EQ(SSL_ERROR_WANT_READ, SSL_get_error(client.get(), first));
    // Do not consume the oversized server handshake flight yet. The peer's
    // receive window and server send queue fill, exercising WANT_WRITE.
    ARCH->sleep(0.05);
    IArchNetwork::PollEntry entry{socket.rawSocket(), IArchNetwork::kPOLLOUT, 0};
    EXPECT_EQ(0, ARCH->pollSocket(&entry, 1, 0));
    EXPECT_FALSE(socket.secureReady());
    const double cpuStart = processCpuSeconds();
    ARCH->sleep(0.1);
    EXPECT_LT(processCpuSeconds() - cpuStart, 0.03);
    ASSERT_TRUE(finishTlsClientHandshake(client.get(), socket));
    const char payload[] = "after-write-backpressure";
    ASSERT_EQ(static_cast<int>(sizeof(payload)), SSL_write(client.get(), payload, sizeof(payload)));
    const double end = ARCH->time() + 1;
    while (socket.getSize() < sizeof(payload) && ARCH->time() < end) { ARCH->sleep(0.001); }
    char received[sizeof(payload)] = {};
    ASSERT_EQ(sizeof(payload), socket.read(received, sizeof(received)));
    EXPECT_EQ(0, std::memcmp(payload, received, sizeof(payload)));
}

TEST(SocketFairnessTests, tlsBudgetsYieldAndContinueBufferedPlaintextWithoutSpin)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    ClientListenerEvents listenerEvents;
    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);
    listenerEvents.setEvents(&events);
    ON_CALL(events, forClientListener()).WillByDefault(ReturnRef(listenerEvents));
    SocketMultiplexer multiplexer;
    TemporaryCertificate cert;
    ConnectedSocketPair pair;
    BudgetSecureSocket socket(&events, &multiplexer, pair.releaseSocket());
    socket.initSsl(true);
    ASSERT_TRUE(socket.load_certificates(cert.path_));
    socket.secureAccept();
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx(SSL_CTX_new(TLS_method()), SSL_CTX_free);
    ASSERT_NE(nullptr, ctx.get());
    std::unique_ptr<SSL, decltype(&SSL_free)> client(SSL_new(ctx.get()), SSL_free);
    ASSERT_NE(nullptr, client.get());
    ASSERT_EQ(1, SSL_set_fd(client.get(), pair.peer()->m_fd));
    const double handshakeEnd = ARCH->time() + 2;
    int result = 0;
    while (ARCH->time() < handshakeEnd) {
        result = SSL_connect(client.get());
        if (result == 1) { break; }
        const int error = SSL_get_error(client.get(), result);
        ASSERT_TRUE(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
        ARCH->sleep(0.001);
    }
    ASSERT_EQ(1, result);
    while (!socket.secureReady() && ARCH->time() < handshakeEnd) {
        ARCH->sleep(0.001);
    }
    ASSERT_TRUE(socket.secureReady());
    socket.pauseService();
    const std::string payload(16384, 's');
    ASSERT_EQ(static_cast<int>(payload.size()), SSL_write(client.get(), payload.data(), payload.size()));
    ARCH->sleep(0.01);
    socket.delayFirstRead = true;
    socket.readOnce();
    EXPECT_EQ(4096u, socket.getSize());
    IArchNetwork::PollEntry entry{socket.rawSocket(), IArchNetwork::kPOLLIN, 0};
    EXPECT_EQ(0, ARCH->pollSocket(&entry, 1, 0));
    socket.resumeService();
    const double end = ARCH->time() + 1;
    while (socket.getSize() < payload.size() && ARCH->time() < end) {
        ARCH->sleep(0.001);
    }
    std::string received(payload.size(), '\0');
    EXPECT_EQ(payload.size(), socket.read(&received[0], received.size()));
    EXPECT_TRUE(payload == received);
    const double cpuStart = processCpuSeconds();
    ARCH->sleep(0.1);
    EXPECT_LT(processCpuSeconds() - cpuStart, 0.03);

    socket.pauseService();
    const std::string bulk(256 * 1024, 'b');
    ASSERT_EQ(static_cast<int>(bulk.size()), SSL_write(client.get(), bulk.data(), bulk.size()));
    ARCH->sleep(0.01);
    socket.readOnce();
    EXPECT_GT(socket.getSize(), 0u);
    EXPECT_LE(socket.getSize(), 64u * 1024u);
    socket.resumeService();
    const double bulkEnd = ARCH->time() + 2;
    while (socket.getSize() < bulk.size() && ARCH->time() < bulkEnd) {
        ARCH->sleep(0.001);
    }
    received.assign(bulk.size(), '\0');
    EXPECT_EQ(bulk.size(), socket.read(&received[0], received.size()));
    EXPECT_TRUE(bulk == received);
}
TEST(SocketFairnessTests, clientHandshakeExpiresAndFreshAttemptConnects)
{
    TemporaryTlsProfile profile;
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);
    SocketMultiplexer multiplexer;
    const auto disconnected = socketEvents.disconnected();
    const auto stopRetry = socketEvents.stopRetry();
    const auto connectionFailed = dataSocketEvents.connectionFailed();
    std::atomic<int> disconnects{0};
    std::atomic<int> stops{0};
    ON_CALL(events, addEvent(_)).WillByDefault(Invoke([&](const Event& event) {
        if (event.getType() == disconnected) { ++disconnects; }
        if (event.getType() == stopRetry) { ++stops; }
        if (event.getType() == connectionFailed) {
            delete static_cast<IDataSocket::ConnectionFailedInfo*>(event.getData());
        }
    }));
    {
        ConnectedSocketPair pair;
        BudgetSecureSocket socket(&events, &multiplexer, pair.releaseSocket());
        socket.initSsl(false);
        const double start = ARCH->time();
        socket.secureConnect();
        ARCH->sleep(0.05);
        const double cpuStart = processCpuSeconds();
        ARCH->sleep(0.2);
        EXPECT_LT(processCpuSeconds() - cpuStart, 0.05);
        while (disconnects.load() == 0 && ARCH->time() - start < 16.5) {
            ARCH->sleep(0.005);
        }
        EXPECT_EQ(1, disconnects.load());
        EXPECT_EQ(0, stops.load());
        EXPECT_GE(ARCH->time() - start, 14.9);
        EXPECT_LT(ARCH->time() - start, 16.0);
    }

    ConnectedSocketPair pair;
    BudgetSecureSocket socket(&events, &multiplexer, pair.releaseSocket());
    socket.initSsl(false);
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ctx(SSL_CTX_new(TLS_method()), SSL_CTX_free);
    const auto cert = barrier::DataDirectories::ssl_certificate_path().string();
    ASSERT_EQ(1, SSL_CTX_use_certificate_file(ctx.get(), cert.c_str(), SSL_FILETYPE_PEM));
    ASSERT_EQ(1, SSL_CTX_use_PrivateKey_file(ctx.get(), cert.c_str(), SSL_FILETYPE_PEM));
    std::unique_ptr<SSL, decltype(&SSL_free)> server(SSL_new(ctx.get()), SSL_free);
    ASSERT_EQ(1, SSL_set_fd(server.get(), pair.peer()->m_fd));
    socket.secureConnect();
    const double end = ARCH->time() + 2;
    int result = 0;
    while (ARCH->time() < end) {
        result = SSL_accept(server.get());
        if (result == 1) { break; }
        const int error = SSL_get_error(server.get(), result);
        ASSERT_TRUE(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
        ARCH->sleep(0.001);
    }
    ASSERT_EQ(1, result);
    while (!socket.secureReady() && ARCH->time() < end) { ARCH->sleep(0.001); }
    ASSERT_TRUE(socket.secureReady());
    const char payload[] = "reconnected";
    socket.write(payload, sizeof(payload));
    char received[sizeof(payload)] = {};
    while (ARCH->time() < end) {
        result = SSL_read(server.get(), received, sizeof(received));
        if (result > 0) { break; }
        ARCH->sleep(0.001);
    }
    EXPECT_EQ(sizeof(payload), static_cast<size_t>(result));
    EXPECT_EQ(0, std::memcmp(payload, received, sizeof(payload)));
    EXPECT_EQ(0, stops.load());
}
#endif

TEST(SocketFairnessTests, silentTlsPeerExpiresWithoutReadinessAndAllowsRetry)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);
    SocketMultiplexer multiplexer;
    ConnectedSocketPair pair;
    TestableSecureSocket socket(&events, &multiplexer, pair.releaseSocket());
    socket.initSsl(true);
    const auto disconnected = socketEvents.disconnected();
    const auto stopRetry = socketEvents.stopRetry();
    std::atomic<int> disconnects{0};
    std::atomic<int> stops{0};
    ON_CALL(events, addEvent(_)).WillByDefault(Invoke([&](const Event& event) {
        if (event.getType() == disconnected) { ++disconnects; }
        if (event.getType() == stopRetry) { ++stops; }
    }));
    const double start = ARCH->time();
    socket.secureAccept();
    while (disconnects.load() == 0 && ARCH->time() - start < 6.5) {
        ARCH->sleep(0.005);
    }
    EXPECT_EQ(1, disconnects.load());
    EXPECT_EQ(0, stops.load());
    EXPECT_GE(ARCH->time() - start, 4.9);
    EXPECT_LT(ARCH->time() - start, 6.0);
}

#if OPENSSL_VERSION_NUMBER >= 0x30000000L && GTEST_HAS_DEATH_TEST
TEST(SecureSocketDeathTest, unavailableTlsProviderFailsCleanlyAndNextSocketCanRecover)
{
    // This process-local OpenSSL fault must never affect another test or a
    // running device. Use --gtest_death_test_style=threadsafe for exec isolation.
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT({
      {
        NiceMock<MockEventQueue> events;
        ISocketEvents socketEvents;
        IStreamEvents streamEvents;
        IDataSocketEvents dataSocketEvents;
        SocketMultiplexer multiplexer;
        setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);
        TCPSocketFactory factory(&events, &multiplexer);

        if (EVP_set_default_properties(nullptr, "provider=weave_missing_provider") != 1) {
            std::exit(10);
        }
        for (int attempt = 0; attempt < 8; ++attempt) {
            try {
                std::unique_ptr<IDataSocket> socket(factory.create(
                    IArchNetwork::kINET, ConnectionSecurityLevel::ENCRYPTED_AUTHENTICATED));
                std::exit(11);
            }
            catch (const XSocketCreate&) {
                // Resource/provider failure is recoverable, not a process crash.
            }
            catch (...) {
                std::exit(12);
            }
            try {
                TestableSecureSocket accepted(&events, &multiplexer);
                accepted.initSsl(true);
                std::exit(15);
            }
            catch (const XSocketCreate&) {
            }
        }
        if (EVP_set_default_properties(nullptr, nullptr) != 1) {
            std::exit(13);
        }
        try {
            std::unique_ptr<IDataSocket> socket(factory.create(
                IArchNetwork::kINET, ConnectionSecurityLevel::ENCRYPTED_AUTHENTICATED));
        }
        catch (...) {
            std::exit(14);
        }
      }
        std::exit(0);
    }, ::testing::ExitedWithCode(0), "");
}
#endif

#if OPENSSL_VERSION_NUMBER >= 0x30000000L && GTEST_HAS_DEATH_TEST
TEST(SecureSocketDeathTest, listenerRejectsTlsInitializationFailureWithoutStoppingServer)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT({
      {
        NiceMock<MockEventQueue> events;
        ISocketEvents socketEvents;
        IStreamEvents streamEvents;
        IDataSocketEvents dataSocketEvents;
        IListenSocketEvents listenEvents;
        SocketMultiplexer multiplexer;
        setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);
        listenEvents.setEvents(&events);
        ON_CALL(events, forIListenSocket()).WillByDefault(ReturnRef(listenEvents));

        std::unique_ptr<SecureListenSocket> listener;
        NetworkAddress address;
        for (int port = 41000; port < 42000; ++port) {
            listener = std::make_unique<SecureListenSocket>(
                &events, &multiplexer, IArchNetwork::kINET,
                ConnectionSecurityLevel::ENCRYPTED_AUTHENTICATED);
            address = NetworkAddress("127.0.0.1", port);
            address.resolve();
            try { listener->bind(address); break; }
            catch (const XSocketAddressInUse&) { listener.reset(); }
        }
        if (!listener) { std::exit(20); }
        std::atomic<int> incoming{0};
        const Event::Type incomingType = listenEvents.connecting();
        ON_CALL(events, addEvent(_)).WillByDefault(Invoke([&](const Event& event) {
            if (event.getTarget() == listener.get() && event.getType() == incomingType) {
                incoming.fetch_add(1, std::memory_order_release);
            }
        }));
        if (EVP_set_default_properties(nullptr, "provider=weave_missing_provider") != 1) {
            std::exit(21);
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            ArchSocket peer = ARCH->newSocket(IArchNetwork::kINET, IArchNetwork::kSTREAM);
            ARCH->connectSocket(peer, address.getAddress());
            const double deadline = ARCH->time() + 2.0;
            while (incoming.load(std::memory_order_acquire) < attempt + 1 &&
                   ARCH->time() < deadline) {
                ARCH->sleep(0.001);
            }
            if (incoming.load(std::memory_order_acquire) != attempt + 1) { std::exit(22); }
            try {
                std::unique_ptr<IDataSocket> accepted(listener->accept());
                if (accepted) { std::exit(23); }
            }
            catch (...) { std::exit(24); }
            ARCH->closeSocket(peer);
        }
        if (EVP_set_default_properties(nullptr, nullptr) != 1) { std::exit(25); }
      }
        std::exit(0);
    }, ::testing::ExitedWithCode(0), "");
}
#endif

TEST(SecureSocketTests, payloadBeforeTlsCloseIsReadyBeforeShutdownAndRemainsReadable)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    SocketMultiplexer multiplexer;
    std::vector<Event::Type> eventOrder;

    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);

    PayloadThenFatalSecureSocket socket(&events, &multiplexer);
    const Event::Type inputReadyType = streamEvents.inputReady();
    const Event::Type inputShutdownType = streamEvents.inputShutdown();
    const Event::Type disconnectedType = socketEvents.disconnected();

    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&](const Event& event) {
            if (event.getTarget() == socket.getEventTarget()) {
                eventOrder.push_back(event.getType());
            }
        }));

    socket.markSecureConnected();
    ASSERT_TRUE(socket.readStopsJobWithLock());

    ASSERT_GE(eventOrder.size(), 2u);
    EXPECT_EQ(inputReadyType, eventOrder[0]);
    EXPECT_EQ(inputShutdownType, eventOrder[1]);
    EXPECT_EQ(4u, socket.getSize());
    EXPECT_EQ(4u, socket.getInputBytesReceived());

    char payload[5] = {};
    EXPECT_EQ(2u, socket.read(payload, 2));
    EXPECT_EQ(2u, socket.getSize());
    EXPECT_TRUE(socket.connected());
    EXPECT_NE(nullptr, socket.rawSocket());
    EXPECT_EQ(2u, eventOrder.size());

    EXPECT_EQ(2u, socket.read(payload + 2, 2));
    EXPECT_STREQ(PayloadThenFatalSecureSocket::kPayload, payload);
    EXPECT_EQ(0u, socket.getSize());
    EXPECT_EQ(4u, socket.getInputBytesReceived());

    ASSERT_GE(eventOrder.size(), 3u);
    EXPECT_EQ(disconnectedType, eventOrder[2]);
    EXPECT_FALSE(socket.connected());
    EXPECT_EQ(nullptr, socket.rawSocket());
}

TEST(SecureSocketTests, tlsFailureWithoutPayloadStillDisconnectsImmediately)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    SocketMultiplexer multiplexer;
    std::vector<Event::Type> eventOrder;

    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);

    FatalSecureSocket socket(&events, &multiplexer);
    const Event::Type inputShutdownType = streamEvents.inputShutdown();
    const Event::Type disconnectedType = socketEvents.disconnected();

    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&](const Event& event) {
            if (event.getTarget() == socket.getEventTarget()) {
                eventOrder.push_back(event.getType());
            }
        }));

    socket.markSecureConnected();
    ASSERT_TRUE(socket.readStopsJobWithLock());

    ASSERT_EQ(2u, eventOrder.size());
    EXPECT_EQ(inputShutdownType, eventOrder[0]);
    EXPECT_EQ(disconnectedType, eventOrder[1]);
    EXPECT_EQ(0u, socket.getSize());
    EXPECT_FALSE(socket.connected());
    EXPECT_EQ(nullptr, socket.rawSocket());
}

TEST(SecureSocketTests, transientTlsFailureClosesTransportAndAllowsRetry)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    SocketMultiplexer multiplexer;
    EventCounts counts;

    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);

    TestableSecureSocket socket(&events, &multiplexer);
    const Event::Type stopRetryType = socketEvents.stopRetry();
    const Event::Type disconnectedType = socketEvents.disconnected();
    const Event::Type inputShutdownType = streamEvents.inputShutdown();

    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&](const Event& event) {
            countEvent(counts, stopRetryType, disconnectedType, inputShutdownType,
                       socket.getEventTarget(), event);
        }));

    socket.markConnected();
    ASSERT_NE(nullptr, socket.rawSocket());

    socket.failTLSWithLock();

    EXPECT_FALSE(socket.connected());
    EXPECT_FALSE(socket.readable());
    EXPECT_FALSE(socket.writable());
    EXPECT_EQ(nullptr, socket.rawSocket());
    EXPECT_EQ(nullptr, socket.newJob().get());
    EXPECT_EQ(0, counts.stopRetry);
    EXPECT_EQ(1, counts.disconnected);
    EXPECT_EQ(1, counts.inputShutdown);

    socket.failTLSWithLock();

    EXPECT_EQ(0, counts.stopRetry);
    EXPECT_EQ(1, counts.disconnected);
    EXPECT_EQ(1, counts.inputShutdown);
}

TEST(SecureSocketTests, tlsFailureLetsMultiplexerRetireCurrentJob)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    SocketMultiplexer multiplexer;
    std::atomic<bool> invoked(false);
    std::atomic<int> jobDestructions(0);

    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);

    {
        ConnectedSocketPair sockets;
        TestableSecureSocket socket(&events, &multiplexer,
                                    sockets.releaseSocket());
        socket.installFailingJob(invoked, jobDestructions);
        sockets.signalReadable();

        const double deadline = ARCH->time() + 2.0;
        while ((!invoked.load(std::memory_order_acquire) ||
                jobDestructions.load(std::memory_order_acquire) == 0) &&
               ARCH->time() < deadline) {
            ARCH->sleep(0.001);
        }

        EXPECT_TRUE(invoked.load(std::memory_order_acquire));
        EXPECT_EQ(1, jobDestructions.load(std::memory_order_acquire));
    }

    EXPECT_EQ(1, jobDestructions.load(std::memory_order_acquire));
}

TEST(SecureSocketTests, secureConnectRetryExhaustionAllowsReconnect)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    SocketMultiplexer multiplexer;
    EventCounts counts;

    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);

    TestableSecureSocket socket(&events, &multiplexer);
    const Event::Type stopRetryType = socketEvents.stopRetry();
    const Event::Type disconnectedType = socketEvents.disconnected();
    const Event::Type inputShutdownType = streamEvents.inputShutdown();

    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&](const Event& event) {
            countEvent(counts, stopRetryType, disconnectedType, inputShutdownType,
                       socket.getEventTarget(), event);
        }));

    socket.markConnected();
    socket.exhaustSecureConnectRetriesWithLock();

    EXPECT_EQ(0, counts.stopRetry);
    EXPECT_EQ(1, counts.disconnected);
    EXPECT_EQ(1, counts.inputShutdown);
    EXPECT_FALSE(socket.connected());
    EXPECT_EQ(nullptr, socket.rawSocket());
}

TEST(SecureSocketTests, permanentTlsFailureStopsRetryOnce)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    SocketMultiplexer multiplexer;
    EventCounts counts;

    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);

    TestableSecureSocket socket(&events, &multiplexer);
    const Event::Type stopRetryType = socketEvents.stopRetry();
    const Event::Type disconnectedType = socketEvents.disconnected();
    const Event::Type inputShutdownType = streamEvents.inputShutdown();

    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&](const Event& event) {
            countEvent(counts, stopRetryType, disconnectedType, inputShutdownType,
                       socket.getEventTarget(), event);
        }));

    socket.markConnected();
    socket.failPermanentTLSWithLock();

    EXPECT_EQ(1, counts.stopRetry);
    EXPECT_EQ(1, counts.disconnected);
    EXPECT_EQ(1, counts.inputShutdown);

    socket.failPermanentTLSWithLock();

    EXPECT_EQ(1, counts.stopRetry);
    EXPECT_EQ(1, counts.disconnected);
    EXPECT_EQ(1, counts.inputShutdown);
}

TEST(SecureSocketTests, permanentFailureAfterTransportFailureStillStopsRetry)
{
    NiceMock<MockEventQueue> events;
    ISocketEvents socketEvents;
    IStreamEvents streamEvents;
    IDataSocketEvents dataSocketEvents;
    SocketMultiplexer multiplexer;
    EventCounts counts;

    setEventDefaults(events, socketEvents, streamEvents, dataSocketEvents);

    TestableSecureSocket socket(&events, &multiplexer);
    const Event::Type stopRetryType = socketEvents.stopRetry();
    const Event::Type disconnectedType = socketEvents.disconnected();
    const Event::Type inputShutdownType = streamEvents.inputShutdown();

    ON_CALL(events, addEvent(_))
        .WillByDefault(Invoke([&](const Event& event) {
            countEvent(counts, stopRetryType, disconnectedType, inputShutdownType,
                       socket.getEventTarget(), event);
        }));

    socket.markConnected();
    socket.failTLSWithLock();
    socket.failPermanentTLSWithLock();
    socket.failPermanentTLSWithLock();

    EXPECT_EQ(1, counts.stopRetry);
    EXPECT_EQ(1, counts.disconnected);
    EXPECT_EQ(1, counts.inputShutdown);
}
