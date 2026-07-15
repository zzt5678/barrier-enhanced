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

#include "barrier/win32/DaemonApp.h"

#include "barrier/App.h"
#include "barrier/ArgParser.h"
#include "barrier/ServerArgs.h"
#include "barrier/ClientArgs.h"
#include "ipc/IpcClientProxy.h"
#include "ipc/IpcCommandValidator.h"
#include "ipc/ElevationPolicy.h"
#include "ipc/IpcMessage.h"
#include "ipc/IpcLogOutputter.h"
#include "net/SocketMultiplexer.h"
#include "arch/XArch.h"
#include "base/Log.h"
#include "base/TMethodEventJob.h"
#include "base/EventQueue.h"
#include "base/log_outputters.h"
#include "base/Log.h"
#include "common/DataDirectories.h"

#include "arch/win32/ArchMiscWindows.h"
#include "arch/win32/XArchWindows.h"
#include "barrier/Screen.h"
#include "platform/MSWindowsScreen.h"
#include "platform/MSWindowsDebugOutputter.h"
#include "platform/MSWindowsWatchdog.h"
#include "platform/MSWindowsEventQueueBuffer.h"
#include "platform/MSWindowsUtil.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <stdexcept>
#include <vector>

using namespace std;

DaemonApp* DaemonApp::s_instance = NULL;

static UInt8
readElevateModeSetting()
{
    return ElevationPolicy::modeFromSettings(
        ARCH->setting("ElevateMode"),
        ARCH->setting("Elevate"));
}

static std::string
normalizedWindowsPath(std::string path)
{
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char ch) {
        if (ch == '/') {
            return '\\';
        }
        return static_cast<char>(std::tolower(ch));
    });
    while (path.size() > 3 && path.back() == '\\') {
        path.pop_back();
    }
    return path;
}

static std::string
environmentPath(const char* name)
{
    const DWORD required = GetEnvironmentVariableA(name, nullptr, 0);
    if (required == 0) {
        return std::string();
    }

    std::vector<char> buffer(required);
    const DWORD written = GetEnvironmentVariableA(name, buffer.data(), required);
    if (written == 0 || written >= required) {
        return std::string();
    }
    return std::string(buffer.data(), written);
}

static bool
isBelowDirectory(const std::string& path, const std::string& directory)
{
    const std::string normalizedPath = normalizedWindowsPath(path);
    const std::string normalizedDirectory = normalizedWindowsPath(directory);
    return !normalizedDirectory.empty() &&
           normalizedPath.size() > normalizedDirectory.size() &&
           normalizedPath.compare(0, normalizedDirectory.size(), normalizedDirectory) == 0 &&
           normalizedPath[normalizedDirectory.size()] == '\\';
}

static bool
isProtectedProgramFilesPath(const std::string& path)
{
    static const char* kRoots[] = {
        "ProgramFiles",
        "ProgramW6432",
        "ProgramFiles(x86)"
    };
    for (const char* rootName : kRoots) {
        if (isBelowDirectory(path, environmentPath(rootName))) {
            return true;
        }
    }
    return false;
}

int
mainLoopStatic()
{
    DaemonApp::s_instance->mainLoop(true);
    return kExitSuccess;
}

int
mainLoopStatic(int, const char**)
{
    return ArchMiscWindows::runDaemon(mainLoopStatic);
}

DaemonApp::DaemonApp() :
    m_ipcServer(nullptr),
    m_ipcLogOutputter(nullptr),
    m_watchdog(nullptr),
    m_events(nullptr),
    m_fileLogOutputter(nullptr),
    m_daemonized(false)
{
    s_instance = this;
}

DaemonApp::~DaemonApp()
{
}

int
DaemonApp::run(int argc, char** argv)
{
    // win32 instance needed for threading, etc.
    ArchMiscWindows::setInstanceWin32(GetModuleHandle(NULL));

    Arch arch;
    arch.init();

    Log log;
    EventQueue events;
    m_events = &events;

    bool uninstall = false;
    try
    {
        // sends debug messages to visual studio console window.
        log.insert(new MSWindowsDebugOutputter());

        // default log level to system setting.
        string logLevel = arch.setting("LogLevel");
        if (logLevel != "")
            log.setFilter(logLevel.c_str());

        bool foreground = false;

        for (int i = 1; i < argc; ++i) {
            string arg(argv[i]);

            if (arg == "/f" || arg == "-f") {
                foreground = true;
            }
            else if (arg == "/install") {
                uninstall = true;
                arch.installDaemon();
                return kExitSuccess;
            }
            else if (arg == "/uninstall") {
                arch.uninstallDaemon();
                return kExitSuccess;
            }
            else {
                stringstream ss;
                ss << "Unrecognized argument: " << arg;
                foregroundError(ss.str().c_str());
                return kExitArgs;
            }
        }

        if (foreground) {
            // add a console to catch Ctrl+C and run process in foreground
            // instead of daemonizing. useful for debugging.
            if (IsDebuggerPresent())
                AllocConsole();
            mainLoop(false);
        }
        else {
            arch.daemonize("Barrier", mainLoopStatic);
        }

        return kExitSuccess;
    }
    catch (XArch& e) {
        String message = e.what();
        if (uninstall && (message.find("The service has not been started") != String::npos)) {
            // TODO: if we're keeping this use error code instead (what is it?!).
            // HACK: this message happens intermittently, not sure where from but
            // it's quite misleading for the user. they thing something has gone
            // horribly wrong, but it's just the service manager reporting a false
            // positive (the service has actually shut down in most cases).
        }
        else {
            foregroundError(message.c_str());
        }
        return kExitFailed;
    }
    catch (std::exception& e) {
        foregroundError(e.what());
        return kExitFailed;
    }
    catch (...) {
        foregroundError("Unrecognized error.");
        return kExitFailed;
    }
}

void
DaemonApp::mainLoop(bool daemonized)
{
    try
    {
        m_daemonized = daemonized;
        DAEMON_RUNNING(true);

        if (daemonized) {
            m_fileLogOutputter = new FileLogOutputter(logFilename().c_str());
            CLOG->insert(m_fileLogOutputter);
        }

        if (daemonized) {
            initializeTrustedExecutables();
        }

        // create socket multiplexer.  this must happen after daemonization
        // on unix because threads evaporate across a fork().
        SocketMultiplexer multiplexer;

        // uses event queue, must be created here.
        m_ipcServer = new IpcServer(m_events, &multiplexer);

        // send logging to gui via ipc, log system adopts outputter.
        m_ipcLogOutputter = new IpcLogOutputter(*m_ipcServer, kIpcClientGui, true);
        CLOG->insert(m_ipcLogOutputter);

        m_watchdog = new MSWindowsWatchdog(daemonized, false, *m_ipcServer, *m_ipcLogOutputter);
        m_watchdog->setFileLogOutputter(m_fileLogOutputter);

        m_events->adoptHandler(
            m_events->forIpcServer().messageReceived(), m_ipcServer,
            new TMethodEventJob<DaemonApp>(this, &DaemonApp::handleIpcMessage));

        m_ipcServer->listen();

        // install the platform event queue to handle service stop events.
        m_events->adoptBuffer(new MSWindowsEventQueueBuffer(m_events));

        String command = ARCH->setting("Command");
        UInt8 elevateMode = readElevateModeSetting();
        if (command != "") {
            std::string rejectReason;
            const String requestedCommand = command;
            if (prepareWatchdogCommand(command, rejectReason)) {
                if (command != requestedCommand) {
                    LOG((CLOG_WARN "replaced persisted executable with protected sibling: %s",
                         command.c_str()));
                    ARCH->setting("Command", command);
                }
                LOG((CLOG_INFO "using last known command: %s", command.c_str()));
                m_watchdog->setCommand(command, elevateMode);
            }
            else {
                LOG((CLOG_ERR "ignoring invalid last known command: %s", rejectReason.c_str()));
            }
        }

        m_watchdog->startAsync();

        m_events->loop();

        m_watchdog->stop();
        delete m_watchdog;

        m_events->removeHandler(
            m_events->forIpcServer().messageReceived(), m_ipcServer);

        CLOG->remove(m_ipcLogOutputter);
        delete m_ipcLogOutputter;
        delete m_ipcServer;

        DAEMON_RUNNING(false);
    }
    catch (std::exception& e) {
        LOG((CLOG_CRIT "An error occurred: %s", e.what()));
    }
    catch (...) {
        LOG((CLOG_CRIT "An unknown error occurred.\n"));
    }
}

void
DaemonApp::initializeTrustedExecutables()
{
    std::vector<char> modulePath(32768);
    const DWORD length = GetModuleFileNameA(nullptr, modulePath.data(),
                                            static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) {
        throw std::runtime_error("unable to resolve the daemon executable path");
    }

    const std::string daemonPath(modulePath.data(), length);
    if (!isProtectedProgramFilesPath(daemonPath)) {
        throw std::runtime_error(
            "refusing to run the SYSTEM service from outside Program Files");
    }

    const std::string::size_type separator = daemonPath.find_last_of("/\\");
    if (separator == std::string::npos) {
        throw std::runtime_error("daemon executable has no parent directory");
    }

    const std::string installDirectory = daemonPath.substr(0, separator + 1);
    m_trustedServerExecutable = installDirectory + "weaves.exe";
    m_trustedClientExecutable = installDirectory + "weavec.exe";
    LOG((CLOG_INFO "protected runtime directory: %s", installDirectory.c_str()));
}

bool
DaemonApp::prepareWatchdogCommand(std::string& command,
                                  std::string& reason) const
{
    if (!IpcCommandValidator::isAllowedDaemonCommand(command, &reason)) {
        return false;
    }
    if (!m_daemonized || command.empty() || command == "\"\"") {
        return true;
    }

    std::string rewritten;
    if (!IpcCommandValidator::rewriteDaemonExecutable(
            command,
            m_trustedServerExecutable,
            m_trustedClientExecutable,
            rewritten,
            &reason)) {
        return false;
    }
    command = rewritten;
    return true;
}

void
DaemonApp::foregroundError(const char* message)
{
    MessageBox(NULL, message, "Barrier Service", MB_OK | MB_ICONERROR);
}

std::string
DaemonApp::logFilename()
{
    string logFilename = ARCH->setting("LogFilename");
    if (logFilename.empty())
        logFilename = (barrier::DataDirectories::global() / LOG_FILENAME).u8string();
    MSWindowsUtil::createDirectory(logFilename, true);
    return logFilename;
}

void
DaemonApp::handleIpcMessage(const Event& e, void*)
{
    IpcMessage* m = static_cast<IpcMessage*>(e.getDataObject());
    if (m == NULL) {
        LOG((CLOG_WARN "ignoring empty ipc message"));
        return;
    }

    switch (m->type()) {
        case kIpcCommand: {
            IpcCommandMessage* cm = static_cast<IpcCommandMessage*>(m);
            String command = cm->command();

            // if empty quotes, clear.
            if (command == "\"\"") {
                command.clear();
            }

            if (!command.empty()) {
                std::string rejectReason;
                const String requestedCommand = command;
                if (!prepareWatchdogCommand(command, rejectReason)) {
                    LOG((CLOG_ERR "rejecting ipc command: %s", rejectReason.c_str()));
                    break;
                }
                if (command != requestedCommand) {
                    LOG((CLOG_WARN "replaced ipc executable with protected sibling: %s",
                         command.c_str()));
                }

                LOG((CLOG_DEBUG "new command, elevateMode=%d command=%s", cm->elevateMode(), command.c_str()));

                std::vector<String> argsArray;
                ArgParser::splitCommandString(command, argsArray);
                if (argsArray.empty()) {
                    LOG((CLOG_ERR "rejecting ipc command: command parsed to no arguments"));
                    break;
                }

                ArgParser argParser(NULL);
                const char** argv = argParser.getArgv(argsArray);
                ServerArgs serverArgs;
                ClientArgs clientArgs;
                int argc = static_cast<int>(argsArray.size());
                bool server = IpcCommandValidator::isServerCommand(command);
                ArgsBase* argBase = NULL;

                if (server) {
                    argParser.parseServerArgs(serverArgs, argc, argv);
                    argBase = &serverArgs;
                }
                else {
                    argParser.parseClientArgs(clientArgs, argc, argv);
                    argBase = &clientArgs;
                }

                delete[] argv;

                String logLevel(argBase->m_logFilter);
                if (!logLevel.empty()) {
                    try {
                        // change log level based on that in the command string
                        // and change to that log level now.
                        ARCH->setting("LogLevel", logLevel);
                        CLOG->setFilter(logLevel.c_str());
                    }
                    catch (XArch& e) {
                        LOG((CLOG_ERR "failed to save LogLevel setting, %s", e.what()));
                    }
                }

                // eg. no log-to-file while running in foreground
                if (m_fileLogOutputter != nullptr) {
                    String logFilename;
                    if (argBase->m_logFile != NULL) {
                        logFilename = String(argBase->m_logFile);
                        ARCH->setting("LogFilename", logFilename);
                        m_watchdog->setFileLogOutputter(m_fileLogOutputter);
                        command = ArgParser::assembleCommand(argsArray, "--log", 1);
                        LOG((CLOG_DEBUG "removed log file argument and filename %s from command ", logFilename.c_str()));
                        LOG((CLOG_DEBUG "new command, elevateMode=%d command=%s", cm->elevateMode(), command.c_str()));
                    } else {
                        m_watchdog->setFileLogOutputter(NULL);
                    }
                    m_fileLogOutputter->setLogFilename(logFilename.c_str());
                }
            }
            else {
                LOG((CLOG_DEBUG "empty command, elevateMode=%d", cm->elevateMode()));
            }

            try {
                // store command in system settings. this is used when the daemon
                // next starts.
                ARCH->setting("Command", command);

                // TODO: it would be nice to store bools/ints...
                ARCH->setting("Elevate", String(cm->elevate() ? "1" : "0"));
                ARCH->setting("ElevateMode", String(std::to_string(cm->elevateMode())));
            }
            catch (XArch& e) {
                LOG((CLOG_ERR "failed to save settings, %s", e.what()));
            }

            // tell the relauncher about the new command. this causes the
            // relauncher to stop the existing command and start the new
            // command.
            m_watchdog->setCommand(command, cm->elevateMode());

            break;
        }

        case kIpcHello: {
            IpcHelloMessage* hm = static_cast<IpcHelloMessage*>(m);
            String type;
            switch (hm->clientType()) {
                case kIpcClientGui: type = "gui"; break;
                case kIpcClientNode: type = "node"; break;
                default: type = "unknown"; break;
            }

            LOG((CLOG_DEBUG "ipc hello, type=%s", type.c_str()));

            const char * serverstatus = m_watchdog->isProcessActive() ? "active" : "not active";

            // using CLOG_PRINT here allows the GUI to see that the server status
            // regardless of which log level is set
            LOG((CLOG_PRINT "server status: %s", serverstatus));

            m_ipcLogOutputter->notifyBuffer();
            break;
        }

        case kIpcReady:
            LOG((CLOG_DEBUG "ipc node reported ready"));
            break;
    }
}
