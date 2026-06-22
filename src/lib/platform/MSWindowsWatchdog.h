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
#include "barrier/XBarrier.h"
#include "arch/IArchMultithread.h"
#include "common/basic_types.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <atomic>
#include <string>
#include <list>
#include <mutex>

class Thread;
class IpcLogOutputter;
class IpcServer;
class FileLogOutputter;

class MSWindowsWatchdog {
public:
    MSWindowsWatchdog(
        bool daemonized,
        bool autoDetectCommand,
        IpcServer& ipcServer,
        IpcLogOutputter& ipcLogOutputter);

    void                startAsync();
    std::string            getCommand() const;
    void                setCommand(const std::string& command, UInt8 elevateMode);
    void                stop();
    bool                isProcessActive();
    void                setFileLogOutputter(FileLogOutputter* outputter);

private:
    struct CommandState {
        std::string command;
        UInt8 elevateMode;
        bool commandChanged;
        int processFailures;
        unsigned long long generation;
        std::string lastDesktopName;
    };

    void main_loop();
    void output_loop();
	void                shutdownProcess(HANDLE handle, DWORD pid, int timeout,
                                        bool notifyIpc = true, UInt32 notifyProcessId = 0);
	void                shutdownExistingProcesses();
	void                closeProcessInfoHandles();
	void                createOutputPipeHandles();
	void                closeOutputPipeHandles();
	HANDLE                duplicateProcessToken(HANDLE process, LPSECURITY_ATTRIBUTES security);
    HANDLE                getUserToken(LPSECURITY_ATTRIBUTES security, UInt8 elevateMode, bool autoElevated);
    bool                shouldElevateProcess(UInt8 elevateMode) const;
    bool                shouldAutoElevate(UInt8 elevateMode, const std::string& desktopName) const;
    void                startProcess();
    std::string         autoDetectedCommand() const;
    CommandState        commandState() const;
    void                incrementProcessFailures();
    void                clearLaunchStateForGeneration(unsigned long long generation);
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
    MSWindowsSession    m_session;
    PROCESS_INFORMATION m_processInfo;
    int                    m_processFailures;
    bool                m_processRunning;
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
