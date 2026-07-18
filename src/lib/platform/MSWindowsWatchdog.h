/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2009 Chris Schoeneman
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

#include "ipc/DesktopSwitchPolicy.h"
#include "platform/MSWindowsSession.h"
#include "barrier/ServiceLaunchState.h"
#include "barrier/XBarrier.h"
#include "arch/IArchMultithread.h"
#include "common/basic_types.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <list>
#include <mutex>

class Thread;
class IpcLogOutputter;
class IpcServer;
class FileLogOutputter;

class MSWindowsWatchdog {
public:
    typedef std::function<ServiceLaunchCommitResult (
        const ServiceLaunchCandidate&)>
        LaunchReadyCallback;
    typedef std::function<void (unsigned long long)>
        StopCompletedCallback;
    typedef std::function<bool (unsigned char*, std::size_t)>
        RandomBytesProvider;

    struct LaunchProfile {
        bool authenticated = false;
        UInt32 sessionId = 0;
        std::string userSid;
        std::string profileDirectory;
        std::string generation;
        std::string digest;
    };

    // The service-only standby flag is derived for CreateProcess and must
    // never become part of the externally supplied or durable command.
    static bool         containsInternalStandbyOption(
                            const std::string& command);
    static bool         isExternalCommandAccepted(
                            const std::string& command);
    static std::string  makeStandbyLaunchCommand(
                            const std::string& command);
    static bool         isSameAuthenticatedLaunchOwner(
                            const LaunchProfile& left,
                            const LaunchProfile& right);
    static bool         shouldAbortPendingActivation(
                            bool monitoring,
                            bool commandEmpty,
                            bool ownerMatches,
                            bool sessionChanged);
    static bool         shouldDiscardFailedActivation(
                            bool monitoring,
                            bool commandEmpty,
                            bool ownerMatches);
    static double       boundedShutdownWaitSeconds(
                            double deadlineSeconds,
                            double nowSeconds,
                            double maximumWaitSeconds);
    static bool         stopConfirmationReady(
                            bool commandEmpty,
                            bool publishedProcessExists,
                            bool pendingProcessExists);
    static bool         generateReadinessNonce(
                            std::uint64_t& nonce,
                            const RandomBytesProvider& provider);

    MSWindowsWatchdog(
        bool daemonized,
        bool autoDetectCommand,
        IpcServer& ipcServer,
        IpcLogOutputter& ipcLogOutputter);
    ~MSWindowsWatchdog();

    void                startAsync();
    std::string            getCommand() const;
    bool                setCommand(const std::string& command, UInt8 elevateMode);
    bool                setCommand(const std::string& command, UInt8 elevateMode,
                                  const LaunchProfile& launchProfile);
    bool                setCommand(const std::string& command, UInt8 elevateMode,
                                  const LaunchProfile& launchProfile,
                                  const ServiceLaunchCandidate& candidate);
    bool                requestStop(unsigned long long& commandGeneration);
    void                setLaunchReadyCallback(
                                  const LaunchReadyCallback& callback);
    void                setStopCompletedCallback(
                                  const StopCompletedCallback& callback);
    bool                isStopConfirmed(
                                  unsigned long long commandGeneration) const;
    void                stop();
    bool                isProcessActive();
    void                setFileLogOutputter(FileLogOutputter* outputter);

private:
    enum class LaunchOwnerState {
        Ready,
        TemporarilyInactive,
        Invalid
    };

    struct CommandState {
        std::string command;
        UInt8 elevateMode;
        bool commandChanged;
        int processFailures;
        unsigned long long generation;
        std::string lastDesktopName;
        LaunchProfile launchProfile;
        bool hasLaunchCandidate;
        ServiceLaunchCandidate launchCandidate;
    };

    void main_loop();
    void output_loop();
    bool shutdownProcess(HANDLE handle, DWORD pid, int timeout,
                         bool notifyIpc = true, UInt32 notifyProcessId = 0);
    bool shutdownAndCloseProcess(PROCESS_INFORMATION& processInfo, int timeout,
                                 bool notifyIpc = true);
    bool shutdownOwnedProcessBeforeDeadline(
                            PROCESS_INFORMATION& processInfo,
                            double deadlineSeconds);
    bool shutdownManagedProcess(int timeout = 20);
    bool shutdownPendingProcess(int timeout = 3, bool notifyIpc = true);
    void discardPendingProcessOrFailFast(const char* reason, int timeout = 3,
                                         bool notifyIpc = true);
    void closeProcessInfoHandles();
    void createManagedProcessJob();
    void closeManagedProcessJob();
    void failFastOwnedProcesses(const char* reason);
    bool assignPendingProcessToJob();
    bool resumePendingProcess();
    bool waitForPendingInputReadiness(
        const PROCESS_INFORMATION& processInfo,
        UInt32 expectedSessionId,
        const std::string& expectedDesktopName,
        bool expectedDesktopKnown,
        std::string& reportedDesktopName,
        const char* readinessPhase,
        const LaunchProfile* activationOwner = nullptr);
    bool activatePendingProcess(
        const PROCESS_INFORMATION& processInfo,
        UInt32 expectedSessionId,
        const std::string& expectedDesktopName,
        bool expectedDesktopKnown,
        std::string& reportedDesktopName,
        const LaunchProfile& activationOwner);
    bool pendingActivationShouldAbort(
        const LaunchProfile& activationOwner,
        std::string& reason);
    void createOutputPipeHandles();
    void closeOutputPipeHandles();
    HANDLE duplicateProcessToken(HANDLE process, LPSECURITY_ATTRIBUTES security);
    HANDLE                getUserToken(LPSECURITY_ATTRIBUTES security, UInt8 elevateMode, bool autoElevated);
    bool                shouldElevateProcess(UInt8 elevateMode) const;
    bool                shouldAutoElevate(UInt8 elevateMode, const std::string& desktopName) const;
    bool                startProcess();
    bool                setCommandInternal(
                            const std::string& command, UInt8 elevateMode,
                            const LaunchProfile& launchProfile,
                            const ServiceLaunchCandidate* candidate,
                            unsigned long long* acceptedGeneration = nullptr);
    void                confirmStoppedGenerationIfReady(
                            unsigned long long commandGeneration);
    std::string         autoDetectedCommand() const;
    CommandState        commandState() const;
    void                incrementProcessFailures();
    void                deferLaunchForGeneration(unsigned long long generation);
    LaunchOwnerState    validateLaunchOwner(const LaunchProfile& launchProfile,
                                             std::string& reason);
    void                rememberDesktopName(const std::string& desktopName);
    bool                shouldRelaunchForDesktopChange(const std::string& oldDesktop,
                                                       const std::string& newDesktop);
    BOOL doStartProcessAsUser(std::string& command, HANDLE userToken, LPSECURITY_ATTRIBUTES sa,
                              const std::string& desktop, PROCESS_INFORMATION& processInfo);
    BOOL doStartProcessAsSelf(std::string& command, const std::string& desktop,
                              PROCESS_INFORMATION& processInfo);

private:
    Thread*                m_thread;
    bool                m_autoDetectCommand;
    std::string            m_command;
    std::atomic<bool>   m_monitoring;
    bool                m_commandChanged;
    unsigned long long  m_commandGeneration;
    HANDLE                m_stdOutWrite;
    HANDLE                m_stdOutRead;
    Thread*                m_outputThread;
    IpcServer&            m_ipcServer;
    IpcLogOutputter&    m_ipcLogOutputter;
    UInt8               m_elevateMode;
    LaunchProfile       m_launchProfile;
    bool                m_hasLaunchCandidate;
    ServiceLaunchCandidate m_launchCandidate;
    LaunchReadyCallback m_launchReadyCallback;
    StopCompletedCallback m_stopCompletedCallback;
    MSWindowsSession    m_session;
    // Only the watchdog thread moves a ready candidate into the published slot.
    PROCESS_INFORMATION m_processInfo;
    PROCESS_INFORMATION m_pendingProcessInfo;
    HANDLE              m_processJob;
    int                    m_processFailures;
    std::atomic<bool>   m_processRunning;
    std::atomic<unsigned long long> m_confirmedStopGeneration;
    std::atomic<bool>   m_hasConfirmedStopGeneration;
    FileLogOutputter*    m_fileLogOutputter;
    bool                m_daemonized;
    mutable std::mutex  m_commandMutex;
    mutable std::mutex  m_outputPipeMutex;
    mutable std::mutex  m_fileLogMutex;
    std::string         m_lastDesktopName;
    DesktopSwitchPolicy::RelaunchState m_desktopRelaunchState;
};

//! Relauncher error
/*!
An error occurred in the process watchdog.
*/
class XMSWindowsWatchdogError : public XBarrier {
public:
    XMSWindowsWatchdogError(const std::string& msg) : XBarrier(msg) { }

    // XBase overrides
    virtual std::string getWhat() const noexcept { return what(); }
};
