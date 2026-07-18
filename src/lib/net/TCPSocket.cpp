/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "net/TCPSocket.h"

#include "net/NetworkAddress.h"
#include "net/SocketMultiplexer.h"
#include "net/TSocketMultiplexerMethodJob.h"
#include "net/XSocket.h"
#include "mt/Lock.h"
#include "arch/Arch.h"
#include "arch/XArch.h"
#include "base/Log.h"
#include "base/IEventQueue.h"
#include "base/IEventJob.h"

#include <cstring>
#include <cstdlib>
#include <memory>

static const std::size_t MAX_INPUT_BUFFER_SIZE = 32 * 1024 * 1024;
static const std::size_t kResumeInputBufferSize = MAX_INPUT_BUFFER_SIZE / 2;
static const UInt32 kMaxHighPriorityOutputBufferSize = 16 * 1024 * 1024;
static const UInt32 kMaxLowPriorityOutputBufferSize = 8 * 1024 * 1024;
static const UInt32 kMaxTotalOutputBufferSize = 24 * 1024 * 1024;
static const UInt32 kLowPriorityWriteWindowSize = 128 * 1024;

TCPSocket::TCPSocket(IEventQueue* events, SocketMultiplexer* socketMultiplexer, IArchNetwork::EAddressFamily family) :
    IDataSocket(events),
    m_events(events),
    m_mutex(),
    m_flushed(&m_mutex, true),
    m_socketMultiplexer(socketMultiplexer),
    m_outputStatsWindow(true),
    m_windowHighPriorityWrites(0),
    m_windowLowPriorityWrites(0),
    m_windowHighPriorityBytes(0),
    m_windowLowPriorityBytes(0),
    m_windowMaxHighPriorityBuffered(0),
    m_windowMaxLowPriorityBuffered(0),
    m_outputBytesWritten(0),
    m_inputBytesReceived(0)
{
    try {
        m_socket = ARCH->newSocket(family, IArchNetwork::kSTREAM);
    }
    catch (XArchNetwork& e) {
        throw XSocketCreate(e.what());
    }

    LOG((CLOG_DEBUG "Opening new socket: %08X", m_socket));

    init();
}

TCPSocket::TCPSocket(IEventQueue* events, SocketMultiplexer* socketMultiplexer, ArchSocket socket) :
    IDataSocket(events),
    m_events(events),
    m_mutex(),
    m_socket(socket),
    m_flushed(&m_mutex, true),
    m_socketMultiplexer(socketMultiplexer),
    m_outputStatsWindow(true),
    m_windowHighPriorityWrites(0),
    m_windowLowPriorityWrites(0),
    m_windowHighPriorityBytes(0),
    m_windowLowPriorityBytes(0),
    m_windowMaxHighPriorityBuffered(0),
    m_windowMaxLowPriorityBuffered(0),
    m_outputBytesWritten(0),
    m_inputBytesReceived(0)
{
    assert(m_socket != NULL);

    LOG((CLOG_DEBUG "Opening new socket: %08X", m_socket));

    // socket starts in connected state
    init();
    onConnected();
    setJob(newJob());
}

TCPSocket::~TCPSocket()
{
    try {
        close();
    }
    catch (...) {
        // ignore
    }
}

void
TCPSocket::bind(const NetworkAddress& addr)
{
    try {
        ARCH->bindSocket(m_socket, addr.getAddress());
    }
    catch (XArchNetworkAddressInUse& e) {
        throw XSocketAddressInUse(e.what());
    }
    catch (XArchNetwork& e) {
        throw XSocketBind(e.what());
    }
}

void
TCPSocket::close()
{
    LOG((CLOG_DEBUG "Closing socket: %08X", m_socket));

    // remove ourself from the multiplexer
    setJob(NULL);

    Lock lock(&m_mutex);

    // clear buffers and enter disconnected state
    if (m_connected) {
        sendEvent(m_events->forISocket().disconnected());
    }
    onDisconnected();

    // close the socket
    if (m_socket != NULL) {
        ArchSocket socket = m_socket;
        m_socket = NULL;
        try {
            ARCH->closeSocket(socket);
        }
        catch (XArchNetwork& e) {
            // ignore, there's not much we can do
            LOG((CLOG_WARN "error closing socket: %s", e.what()));
        }
    }
}

void*
TCPSocket::getEventTarget() const
{
    return const_cast<void*>(static_cast<const void*>(this));
}

UInt32
TCPSocket::read(void* buffer, UInt32 n)
{
    // copy data directly from our input buffer
    std::unique_ptr<ISocketMultiplexerJob> job;
    {
        Lock lock(&m_mutex);
        UInt32 size = m_inputBuffer.getSize();
        const UInt32 previousSize = size;
        if (n > size) {
            n = size;
        }
        if (buffer != NULL && n != 0) {
            memcpy(buffer, m_inputBuffer.peek(n), n);
        }
        m_inputBuffer.pop(n);

        // if no more data and we cannot read or write then send disconnected
        if (n > 0 && m_inputBuffer.getSize() == 0 && !m_readable && !m_writable) {
            sendEvent(m_events->forISocket().disconnected());
            m_connected = false;
        }

        if (shouldResumeInputNoLock(previousSize)) {
            job = newJob();
        }
    }

    if (job) {
        setJob(std::move(job));
    }

    return n;
}

void
TCPSocket::write(const void* buffer, UInt32 n)
{
    writeToBuffer(m_outputBuffer, buffer, n);
}

void
TCPSocket::writeLowPriority(const void* buffer, UInt32 n)
{
    // Raw byte priority cannot preserve message boundaries across partial
    // writes.  Use the primary FIFO so reliable data is never interleaved or
    // silently discarded when the low-priority budget is exhausted.
    writeToBuffer(m_outputBuffer, buffer, n);
}

void
TCPSocket::writeToBuffer(StreamBuffer& outputBuffer, const void* buffer, UInt32 n)
{
    std::unique_ptr<ISocketMultiplexerJob> job;
    {
        Lock lock(&m_mutex);

        // must not have shutdown output
        if (!m_writable) {
            sendEvent(m_events->forIStream().outputError());
            return;
        }

        // ignore empty writes
        if (n == 0) {
            return;
        }

        const bool lowPriority = (&outputBuffer == &m_lowPriorityOutputBuffer);
        if (!canQueueOutputNoLock(lowPriority, n)) {
            if (lowPriority) {
                LOG((CLOG_WARN
                    "socket low-priority output backlog exceeded; dropping %u bytes "
                    "without disconnecting (high=%u low=%u total=%u)",
                    n,
                    m_outputBuffer.getSize(),
                    m_lowPriorityOutputBuffer.getSize(),
                    m_outputBuffer.getSize() + m_lowPriorityOutputBuffer.getSize()));
                return;
            }

            LOG((CLOG_WARN
                "socket output backlog exceeded; dropping connection before buffering %u %s-priority bytes "
                "(high=%u low=%u total=%u)",
                n,
                lowPriority ? "low" : "high",
                m_outputBuffer.getSize(),
                m_lowPriorityOutputBuffer.getSize(),
                m_outputBuffer.getSize() + m_lowPriorityOutputBuffer.getSize()));
            onDisconnected();
            sendEvent(m_events->forIStream().outputError());
            sendEvent(m_events->forISocket().disconnected());
            return;
        }

        // copy data to the output buffer
        const bool wasEmpty = !hasBufferedOutputNoLock();
        outputBuffer.write(buffer, n);
        noteQueuedBytes(lowPriority, n);

        // there's data to write
        m_flushed = false;
        if (wasEmpty) {
            job = newJob();
        }
    }

    // make sure we're waiting to write
    if (job) {
        setJob(std::move(job));
    }
}

void
TCPSocket::flush()
{
    Lock lock(&m_mutex);
    while (m_flushed == false) {
        m_flushed.wait();
    }
}

void
TCPSocket::shutdownInput()
{
    bool updateJob = false;
    std::unique_ptr<ISocketMultiplexerJob> job;
    {
        Lock lock(&m_mutex);

        // shutdown socket for reading
        try {
            ARCH->closeSocketForRead(m_socket);
        }
        catch (XArchNetwork&) {
            // ignore
        }

        // shutdown buffer for reading
        if (m_readable) {
            sendEvent(m_events->forIStream().inputShutdown());
            onInputShutdown();
            updateJob = true;
            job = newJob();
        }
    }
    if (updateJob) {
        setJob(std::move(job));
    }
}

void
TCPSocket::shutdownOutput()
{
    bool updateJob = false;
    std::unique_ptr<ISocketMultiplexerJob> job;
    {
        Lock lock(&m_mutex);

        // shutdown socket for writing
        try {
            ARCH->closeSocketForWrite(m_socket);
        }
        catch (XArchNetwork&) {
            // ignore
        }

        // shutdown buffer for writing
        if (m_writable) {
            sendEvent(m_events->forIStream().outputShutdown());
            onOutputShutdown();
            updateJob = true;
            job = newJob();
        }
    }
    if (updateJob) {
        setJob(std::move(job));
    }
}

bool
TCPSocket::isReady() const
{
    Lock lock(&m_mutex);
    return (m_inputBuffer.getSize() > 0);
}

bool
TCPSocket::isFatal() const
{
    // TCP sockets aren't ever left in a fatal state.
    LOG((CLOG_ERR "isFatal() not valid for non-secure connections"));
    return false;
}

UInt32
TCPSocket::getSize() const
{
    Lock lock(&m_mutex);
    return m_inputBuffer.getSize();
}

UInt32
TCPSocket::getBufferedOutputSize() const
{
    Lock lock(&m_mutex);
    return m_outputBuffer.getSize() + m_lowPriorityOutputBuffer.getSize();
}

std::uint64_t
TCPSocket::getOutputBytesWritten() const
{
    Lock lock(&m_mutex);
    return m_outputBytesWritten;
}

std::uint64_t
TCPSocket::getInputBytesReceived() const
{
    Lock lock(&m_mutex);
    return m_inputBytesReceived;
}

void
TCPSocket::connect(const NetworkAddress& addr)
{
    std::unique_ptr<ISocketMultiplexerJob> job;
    {
        Lock lock(&m_mutex);

        // fail on attempts to reconnect
        if (m_socket == NULL || m_connected) {
            sendConnectionFailedEvent("busy");
            return;
        }

        try {
            if (ARCH->connectSocket(m_socket, addr.getAddress())) {
                sendEvent(m_events->forIDataSocket().connected());
                onConnected();
            }
            else {
                // connection is in progress
                m_writable = true;
            }
            job = newJob();
        }
        catch (XArchNetwork& e) {
            throw XSocketConnect(e.what());
        }
    }
    setJob(std::move(job));
}

void
TCPSocket::init()
{
    // default state
    m_connected = false;
    m_readable  = false;
    m_writable  = false;

    try {
        // turn off Nagle algorithm.  we send lots of very short messages
        // that should be sent without (much) delay.  for example, the
        // mouse motion messages are much less useful if they're delayed.
        ARCH->setNoDelayOnSocket(m_socket, true);
    }
    catch (XArchNetwork& e) {
        try {
            ARCH->closeSocket(m_socket);
            m_socket = NULL;
        }
        catch (XArchNetwork&) {
            // ignore
        }
        throw XSocketCreate(e.what());
    }
}

TCPSocket::EJobResult
TCPSocket::doRead()
{
    if (!canReadInputNoLock()) {
        LOG((CLOG_DEBUG1 "socket input backlog full; pausing reads until buffered input is drained"));
        return kNew;
    }

    // Increased from 4096 to 65536 for better throughput on large transfers
    // (e.g., clipboard images). This reduces syscall overhead.
    UInt8 buffer[65536];
    const bool wasEmpty = (m_inputBuffer.getSize() == 0);
    bool readAny = false;

    while (true) {
        const size_t readSize = getInputReadSizeNoLock(sizeof(buffer));
        if (readSize == 0) {
            LOG((CLOG_DEBUG1 "socket input backlog full; pausing reads until buffered input is drained"));
            if (readAny && wasEmpty) {
                sendEvent(m_events->forIStream().inputReady());
            }
            return kNew;
        }

        size_t bytesRead = 0;
        try {
            bytesRead = readSocketNoLock(buffer, readSize);
        }
        catch (XArchNetworkInterrupted& e) {
            // A non-blocking read can race with the readiness notification.
            // Preserve any bytes already drained in this pass and keep the
            // connection open; zero is reserved for an orderly EOF.
            if (readAny && wasEmpty) {
                sendEvent(m_events->forIStream().inputReady());
            }
            LOG((CLOG_DEBUG2 "socket read temporarily unavailable: %s", e.what()));
            return kRetry;
        }

        if (bytesRead == 0) {
            // Deliver buffered data before forwarding shutdown. Filters use
            // this ordering to drain the last complete packet before EOF.
            LOG((CLOG_DEBUG1 "socket peer closed its write side"));
            if (readAny && wasEmpty) {
                sendEvent(m_events->forIStream().inputReady());
            }
            sendEvent(m_events->forIStream().inputShutdown());
            if (!m_writable && m_inputBuffer.getSize() == 0) {
                sendEvent(m_events->forISocket().disconnected());
                m_connected = false;
            }
            m_readable = false;
            return kNew;
        }

        if (!queueInputOrDisconnectNoLock(buffer, static_cast<UInt32>(bytesRead))) {
            return kNew;
        }
        readAny = true;

        if (!canReadInputNoLock()) {
            if (wasEmpty) {
                sendEvent(m_events->forIStream().inputReady());
            }
            return kNew;
        }
    }
}

size_t
TCPSocket::readSocketNoLock(void* buffer, size_t size)
{
    return ARCH->readSocket(m_socket, buffer, size);
}

bool
TCPSocket::queueInputOrDisconnectNoLock(const void* buffer, UInt32 n)
{
    const UInt32 currentSize = m_inputBuffer.getSize();
    if (n > MAX_INPUT_BUFFER_SIZE ||
        currentSize > MAX_INPUT_BUFFER_SIZE - n) {
        LOG((CLOG_WARN
            "socket input backlog exceeded; dropping connection before buffering %u bytes "
            "(input=%u limit=%u)",
            n,
            currentSize,
            static_cast<UInt32>(MAX_INPUT_BUFFER_SIZE)));
        onDisconnected();
        sendEvent(m_events->forIStream().inputShutdown());
        sendEvent(m_events->forISocket().disconnected());
        return false;
    }

    m_inputBuffer.write(buffer, n);
    m_inputBytesReceived += static_cast<std::uint64_t>(n);
    return true;
}

size_t
TCPSocket::getInputReadSizeNoLock(size_t maxReadSize) const
{
    const size_t currentSize = m_inputBuffer.getSize();
    if (currentSize >= MAX_INPUT_BUFFER_SIZE) {
        return 0;
    }

    const size_t remainingSize = MAX_INPUT_BUFFER_SIZE - currentSize;
    return remainingSize < maxReadSize ? remainingSize : maxReadSize;
}

UInt32
TCPSocket::getOutputWriteSizeNoLock(const StreamBuffer& outputBuffer) const
{
    UInt32 bufferSize = outputBuffer.getSize();
    if (&outputBuffer == &m_lowPriorityOutputBuffer &&
        bufferSize > kLowPriorityWriteWindowSize) {
        bufferSize = kLowPriorityWriteWindowSize;
    }
    return bufferSize;
}

TCPSocket::EJobResult
TCPSocket::doWrite()
{
    // write data
    UInt32 bufferSize = 0;
    int bytesWrote = 0;

    StreamBuffer* outputBuffer = nullptr;
    if (m_outputBuffer.getSize() > 0) {
        outputBuffer = &m_outputBuffer;
    }
    else if (m_lowPriorityOutputBuffer.getSize() > 0) {
        outputBuffer = &m_lowPriorityOutputBuffer;
    }
    else {
        return kRetry;
    }

    bufferSize = getOutputWriteSizeNoLock(*outputBuffer);
    if (bufferSize == 0) {
        return kRetry;
    }
    const void* buffer = outputBuffer->peek(bufferSize);
    bytesWrote = (UInt32)ARCH->writeSocket(m_socket, buffer, bufferSize);

    if (bytesWrote > 0) {
        discardWrittenData(*outputBuffer, bytesWrote);
        return kNew;
    }

    return kRetry;
}

void TCPSocket::removeJob()
{
    // multiplexer will delete the old job
    m_socketMultiplexer->removeSocket(this);
}

void TCPSocket::setJob(std::unique_ptr<ISocketMultiplexerJob>&& job)
{
    if (job.get() == nullptr) {
        removeJob();
    } else {
        m_socketMultiplexer->addSocket(this, std::move(job));
    }
}

MultiplexerJobStatus TCPSocket::newJobOrStopServicing()
{
    auto new_job = newJob();
    if (new_job)
        return {true, std::move(new_job)};
    else
        return {false, {}};
}

std::unique_ptr<ISocketMultiplexerJob> TCPSocket::newJob()
{
    // note -- must have m_mutex locked on entry

    if (m_socket == NULL) {
        return {};
    }
    else if (!m_connected) {
        assert(!m_readable);
        if (!(m_readable || m_writable)) {
            return {};
        }
        return std::make_unique<TSocketMultiplexerMethodJob>(
                    [this](auto j, auto r, auto w, auto e)
                    { return serviceConnecting(j, r, w, e); },
                    m_socket, m_readable, m_writable);
    }
    else {
        auto writable = m_writable && hasBufferedOutputNoLock();
        auto readable = m_readable && canReadInputNoLock();
        if (!(readable || writable)) {
            return {};
        }
        return std::make_unique<TSocketMultiplexerMethodJob>(
                    [this](auto j, auto r, auto w, auto e)
                    { return serviceConnected(j, r, w, e); },
                    m_socket, readable, writable);
    }
}

void
TCPSocket::sendConnectionFailedEvent(const char* msg)
{
    ConnectionFailedInfo* info = new ConnectionFailedInfo(msg);
    m_events->addEvent(Event(m_events->forIDataSocket().connectionFailed(),
                            getEventTarget(), info, Event::kDontFreeData));
}

void
TCPSocket::sendEvent(Event::Type type)
{
    m_events->addEvent(Event(type, getEventTarget(), NULL));
}

void
TCPSocket::discardWrittenData(StreamBuffer& outputBuffer, int bytesWrote)
{
    if (bytesWrote <= 0) {
        return;
    }
    outputBuffer.pop(bytesWrote);
    m_outputBytesWritten += static_cast<std::uint64_t>(bytesWrote);
    if (!hasBufferedOutputNoLock()) {
        logOutputWindowStatsIfNeeded();
        sendEvent(m_events->forIStream().outputFlushed());
        m_flushed = true;
        m_flushed.broadcast();
    }
}

void
TCPSocket::onConnected()
{
    m_connected = true;
    m_readable  = true;
    m_writable  = true;
}

void
TCPSocket::onInputShutdown()
{
    m_inputBuffer.pop(m_inputBuffer.getSize());
    m_readable = false;
}

void
TCPSocket::onOutputShutdown()
{
    m_outputBuffer.pop(m_outputBuffer.getSize());
    m_lowPriorityOutputBuffer.pop(m_lowPriorityOutputBuffer.getSize());
    m_writable = false;

    // we're now flushed
    m_flushed = true;
    m_flushed.broadcast();
}

bool
TCPSocket::hasBufferedOutputNoLock() const
{
    return hasHighPriorityOutputNoLock() ||
           (m_lowPriorityOutputBuffer.getSize() > 0);
}

bool
TCPSocket::hasHighPriorityOutputNoLock() const
{
    return (m_outputBuffer.getSize() > 0);
}

void
TCPSocket::onDisconnected()
{
    // disconnected
    onInputShutdown();
    onOutputShutdown();
    m_connected = false;
}

void
TCPSocket::disconnectSocketNoLock(bool notifyInputShutdown, bool notifyOutputError)
{
    const bool wasConnected = m_connected;
    const bool wasReadable = m_readable;
    const bool wasWritable = m_writable;

    if (notifyInputShutdown && wasReadable) {
        sendEvent(m_events->forIStream().inputShutdown());
    }
    if (notifyOutputError && wasWritable) {
        sendEvent(m_events->forIStream().outputError());
    }
    if (wasConnected || wasReadable || wasWritable) {
        sendEvent(m_events->forISocket().disconnected());
    }

    onDisconnected();

    if (m_socket != NULL) {
        ArchSocket socket = m_socket;
        m_socket = NULL;
        try {
            ARCH->closeSocket(socket);
        }
        catch (XArchNetwork& e) {
            LOG((CLOG_WARN "error closing socket: %s", e.what()));
        }
    }
}

bool
TCPSocket::canQueueOutputNoLock(bool lowPriority, UInt32 n) const
{
    const UInt32 highSize = m_outputBuffer.getSize();
    const UInt32 lowSize = m_lowPriorityOutputBuffer.getSize();
    const UInt32 totalSize = highSize + lowSize;

    if (n > kMaxTotalOutputBufferSize || totalSize > kMaxTotalOutputBufferSize - n) {
        return false;
    }

    if (lowPriority) {
        return n <= kMaxLowPriorityOutputBufferSize &&
            lowSize <= kMaxLowPriorityOutputBufferSize - n;
    }

    return n <= kMaxHighPriorityOutputBufferSize &&
        highSize <= kMaxHighPriorityOutputBufferSize - n;
}

bool
TCPSocket::canReadInputNoLock() const
{
    return m_inputBuffer.getSize() < MAX_INPUT_BUFFER_SIZE;
}

bool
TCPSocket::shouldResumeInputNoLock(UInt32 previousSize) const
{
    return m_readable &&
        previousSize >= MAX_INPUT_BUFFER_SIZE &&
        m_inputBuffer.getSize() <= kResumeInputBufferSize;
}

void
TCPSocket::noteQueuedBytes(bool lowPriority, UInt32 n)
{
    if (lowPriority) {
        ++m_windowLowPriorityWrites;
        m_windowLowPriorityBytes += n;
        const UInt32 buffered = m_lowPriorityOutputBuffer.getSize();
        if (buffered > m_windowMaxLowPriorityBuffered) {
            m_windowMaxLowPriorityBuffered = buffered;
            if (buffered >= 256 * 1024) {
                LOG((CLOG_DEBUG1 "low-priority output backlog=%u bytes", buffered));
            }
        }
    }
    else {
        ++m_windowHighPriorityWrites;
        m_windowHighPriorityBytes += n;
        const UInt32 buffered = m_outputBuffer.getSize();
        if (buffered > m_windowMaxHighPriorityBuffered) {
            m_windowMaxHighPriorityBuffered = buffered;
        }
    }
}

void
TCPSocket::logOutputWindowStatsIfNeeded()
{
    const bool hadLowPriorityTraffic =
        (m_windowLowPriorityWrites > 0 || m_windowLowPriorityBytes > 0);
    const bool hadMeaningfulHighPriorityTraffic =
        (m_windowHighPriorityWrites > 32 || m_windowHighPriorityBytes > 16 * 1024);
    if (!(hadLowPriorityTraffic || hadMeaningfulHighPriorityTraffic)) {
        m_outputStatsWindow.reset();
        m_windowHighPriorityWrites = 0;
        m_windowLowPriorityWrites = 0;
        m_windowHighPriorityBytes = 0;
        m_windowLowPriorityBytes = 0;
        m_windowMaxHighPriorityBuffered = 0;
        m_windowMaxLowPriorityBuffered = 0;
        return;
    }

    LOG((CLOG_DEBUG1
        "socket output window: high writes=%u bytes=%u maxBuffered=%u, low writes=%u bytes=%u maxBuffered=%u, window=%.3fs",
        m_windowHighPriorityWrites,
        m_windowHighPriorityBytes,
        m_windowMaxHighPriorityBuffered,
        m_windowLowPriorityWrites,
        m_windowLowPriorityBytes,
        m_windowMaxLowPriorityBuffered,
        m_outputStatsWindow.getTime()));

    m_outputStatsWindow.reset();
    m_windowHighPriorityWrites = 0;
    m_windowLowPriorityWrites = 0;
    m_windowHighPriorityBytes = 0;
    m_windowLowPriorityBytes = 0;
    m_windowMaxHighPriorityBuffered = 0;
    m_windowMaxLowPriorityBuffered = 0;
}

MultiplexerJobStatus TCPSocket::serviceConnecting(ISocketMultiplexerJob* job, bool, bool write, bool error)
{
    Lock lock(&m_mutex);

    // should only check for errors if error is true but checking a new
    // socket (and a socket that's connecting should be new) for errors
    // should be safe and Mac OS X appears to have a bug where a
    // non-blocking stream socket that fails to connect immediately is
    // reported by select as being writable (i.e. connected) even when
    // the connection has failed.  this is easily demonstrated on OS X
    // 10.3.4 by starting a barrier client and telling to connect to
    // another system that's not running a barrier server.  it will
    // claim to have connected then quickly disconnect (i guess because
    // read returns 0 bytes).  unfortunately, barrier attempts to
    // reconnect immediately, the process repeats and we end up
    // spinning the CPU.  luckily, OS X does set SO_ERROR on the
    // socket correctly when the connection has failed so checking for
    // errors works.  (curiously, sometimes OS X doesn't report
    // connection refused.  when that happens it at least doesn't
    // report the socket as being writable so barrier is able to time
    // out the attempt.)
    if (error || true) {
        try {
            // connection may have failed or succeeded
            ARCH->throwErrorOnSocket(m_socket);
        }
        catch (XArchNetwork& e) {
            sendConnectionFailedEvent(e.what());
            onDisconnected();
            return newJobOrStopServicing();
        }
    }

    if (write) {
        sendEvent(m_events->forIDataSocket().connected());
        onConnected();
        return newJobOrStopServicing();
    }

    return {true, {}};
}

MultiplexerJobStatus TCPSocket::serviceConnected(ISocketMultiplexerJob* job,
                                                 bool read, bool write, bool error)
{
    Lock lock(&m_mutex);

    if (error) {
        sendEvent(m_events->forISocket().disconnected());
        onDisconnected();
        return newJobOrStopServicing();
    }

    EJobResult writeResult = kRetry;
    EJobResult readResult = kRetry;
    if (write) {
        try {
            writeResult = doWrite();
        }
        catch (XArchNetworkShutdown&) {
            // remote read end of stream hungup.  our output side
            // has therefore shutdown.
            onOutputShutdown();
            sendEvent(m_events->forIStream().outputShutdown());
            if (!m_readable && m_inputBuffer.getSize() == 0) {
                sendEvent(m_events->forISocket().disconnected());
                m_connected = false;
            }
            writeResult = kNew;
        }
        catch (XArchNetworkDisconnected&) {
            // stream hungup
            onDisconnected();
            sendEvent(m_events->forISocket().disconnected());
            writeResult = kNew;
        }
        catch (XArchNetworkTimedOut& e) {
            LOG((CLOG_DEBUG1 "timed out writing socket, retrying: %s", e.what()));
            writeResult = kRetry;
        }
        catch (XArchNetwork& e) {
            // other write error
            LOG((CLOG_WARN "error writing socket: %s", e.what()));
            onDisconnected();
            sendEvent(m_events->forIStream().outputError());
            sendEvent(m_events->forISocket().disconnected());
            writeResult = kNew;
        }
    }

    if (read && m_readable) {
        try {
            readResult = doRead();
        }
        catch (XArchNetworkDisconnected&) {
            // stream hungup
            sendEvent(m_events->forISocket().disconnected());
            onDisconnected();
            readResult = kNew;
        }
        catch (XArchNetworkInterrupted& e) {
            LOG((CLOG_DEBUG1 "interrupted reading socket: %s", e.what()));
            readResult = kRetry;
        }
        catch (XArchNetworkTimedOut& e) {
            LOG((CLOG_DEBUG1 "timed out reading socket, retrying: %s", e.what()));
            readResult = kRetry;
        }
        catch (XArchNetwork& e) {
            LOG((CLOG_WARN "error reading socket: %s", e.what()));
            onDisconnected();
            sendEvent(m_events->forIStream().inputShutdown());
            sendEvent(m_events->forISocket().disconnected());
            readResult = kNew;
        }
    }

    if (writeResult == kBreak || readResult == kBreak) {
        return {false, {}};
    } else if (writeResult == kNew || readResult == kNew) {
        return newJobOrStopServicing();
    } else {
        return {true, {}};
    }
}
