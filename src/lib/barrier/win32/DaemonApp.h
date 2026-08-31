/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2012 Nick Bolton
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

#pragma once

#include "arch/Arch.h"
#include "barrier/ServiceLaunchState.h"
#include "base/Event.h"
#include "ipc/IpcServer.h"

#include <cstdint>
#include <atomic>
#include <string>
#include <vector>

class IpcLogOutputter;
class FileLogOutputter;

class MSWindowsWatchdog;

class DaemonApp {

public:
    DaemonApp();
    virtual ~DaemonApp();
    int run(int argc, char** argv);
    void mainLoop(bool daemonized);

private:
    void daemonize();
    void foregroundError(const char* message);
    std::string            logFilename();
    void                initializeTrustedExecutables();
    bool                prepareWatchdogCommand(std::string& command,
                                               UInt8 requestedElevateMode,
                                               UInt8& sanitizedElevateMode,
                                               std::string& reason) const;
    void                handleIpcMessage(const Event&, void*);
    ServiceLaunchCommitResult commitWatchdogLaunchReady(
                            const ServiceLaunchCandidate& candidate);
    void                handleWatchdogLaunchReady(const Event&, void*);
    void                notifyWatchdogStopConfirmed(
                            unsigned long long commandGeneration);
    void                handleWatchdogStopConfirmed(const Event&, void*);
    void                acknowledgeConfirmedStops(
                            unsigned long long commandGeneration);
    ServiceLaunchCommitResult promoteWatchdogLaunchReady(
                            const ServiceLaunchCandidate& candidate);

public:
    static DaemonApp* s_instance;

    MSWindowsWatchdog*    m_watchdog;

private:
    IpcServer*            m_ipcServer;
    IpcLogOutputter*    m_ipcLogOutputter;
    IEventQueue*        m_events;
    FileLogOutputter*    m_fileLogOutputter;
    bool                m_daemonized;
    Event::Type         m_launchReadyEvent;
    Event::Type         m_stopConfirmedEvent;
    std::uint64_t       m_launchRevision;
    std::atomic<bool>   m_acceptLaunchCommits;
    std::atomic<bool>   m_acceptStopConfirmations;
    struct PendingStopAck {
        std::uint64_t requestId;
        UInt32 processId;
        unsigned long long commandGeneration;
    };
    std::vector<PendingStopAck> m_pendingStopAcks;
    std::string         m_trustedServerExecutable;
    std::string         m_trustedClientExecutable;
};

#define LOG_FILENAME "weaved.log"
