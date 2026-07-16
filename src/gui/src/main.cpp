/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2008 Volker Lanz (vl@fidra.de)
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

#include "QBarrierApplication.h"
#include "MainWindow.h"
#include "AppConfig.h"
#include "SetupWizard.h"
#include "DisplayIsValid.h"
#include "GuiInstanceCoordinator.h"

#include <QtCore>
#include <QtGui>
#include <QSettings>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QSocketNotifier>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(Q_OS_MAC)
#include <Carbon/Carbon.h>
#endif

#ifdef Q_OS_DARWIN
#include <cstdlib>
#endif

#if defined(Q_OS_MAC)
bool checkMacAssistiveDevices();
#endif

namespace {

#if defined(Q_OS_UNIX)
int signalWriteFd = -1;

void forwardUnixSignal(int)
{
    const int savedErrno = errno;
    const char byte = 1;
    if (signalWriteFd >= 0) {
        const ssize_t result = ::write(signalWriteFd, &byte, sizeof(byte));
        (void)result;
    }
    errno = savedErrno;
}

void installUnixSignalHandlers(QCoreApplication& app)
{
    int signalPipe[2];
    if (::pipe(signalPipe) != 0) {
        return;
    }

    for (const int fd : signalPipe) {
        ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD) | FD_CLOEXEC);
    }
    ::fcntl(signalPipe[1], F_SETFL, ::fcntl(signalPipe[1], F_GETFL) | O_NONBLOCK);
    signalWriteFd = signalPipe[1];

    const int readFd = signalPipe[0];
    auto* notifier = new QSocketNotifier(readFd, QSocketNotifier::Read, &app);
    QObject::connect(
        notifier,
        &QSocketNotifier::activated,
        &app,
        [&app, notifier, readFd]() {
            notifier->setEnabled(false);
            char byte;
            const ssize_t result = ::read(readFd, &byte, sizeof(byte));
            (void)result;
            app.quit();
        });

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = forwardUnixSignal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    ::sigaction(SIGTERM, &action, nullptr);
    ::sigaction(SIGINT, &action, nullptr);
}
#endif

void cleanupStartupArtifacts()
{
    const QDir tempDir = QDir::temp();
    const QStringList tempPatterns = {
        QStringLiteral("Barrier.*"),
        QStringLiteral("Weave.*")
    };
    for (const QString& pattern : tempPatterns) {
        const QStringList entries = tempDir.entryList({pattern}, QDir::Files);
        for (const QString& entry : entries) {
            QFile::remove(tempDir.absoluteFilePath(entry));
        }
    }
}

} // namespace

int main(int argc, char* argv[])
{
#ifdef WINAPI_XWINDOWS
    // QApplication's constructor will call a fscking abort() if
    // DISPLAY is bad. Let's check it first and handle it gracefully
    if (!display_is_valid()) {
        fprintf(stderr, "The Weave GUI requires a display. Quitting...\n");
        return 1;
    }
#endif
#ifdef Q_OS_DARWIN
    /* Workaround for QTBUG-40332 - "High ping when QNetworkAccessManager is instantiated" */
    ::setenv ("QT_BEARER_POLL_TIMEOUT", "-1", 1);
#endif

	QCoreApplication::setOrganizationName("Weave");
	QCoreApplication::setOrganizationDomain("github.com");
	QCoreApplication::setApplicationName("Weave");

	// Enable High DPI scaling for modern displays
	QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
	QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

	QBarrierApplication app(argc, argv);

#if defined(Q_OS_UNIX)
    installUnixSignalHandlers(app);
#endif

	GuiInstanceCoordinator instanceCoordinator(
		GuiInstanceCoordinator::defaultLockPath(),
		GuiInstanceCoordinator::defaultServerName());
	const GuiInstanceCoordinator::StartResult instanceResult =
		instanceCoordinator.start();
	if (instanceResult == GuiInstanceCoordinator::StartResult::ExistingActivated) {
		return 0;
	}
	if (instanceResult != GuiInstanceCoordinator::StartResult::Primary) {
		fprintf(stderr, "Unable to start or activate the Weave GUI.\n");
		return 1;
	}

    cleanupStartupArtifacts();

#if defined(Q_OS_MAC)
	if (app.applicationDirPath().startsWith("/Volumes/")) {
        // macOS preferences track applications allowed assistive access by path
        // Unfortunately, there's no user-friendly way to allow assistive access
        // to applications that are not in default paths (/Applications),
        // especially if an identically named application already exists in
        // /Applications). Thus we require Weave to reside in the /Applications
        // folder
		QMessageBox::information(
			NULL, "Weave",
			"Please drag Weave to the Applications folder, and open it from there.");
		return 1;
	}

	if (!checkMacAssistiveDevices())
	{
		return 1;
	}
#endif

	QApplication::setQuitOnLastWindowClosed(false);

    if (QGuiApplication::platformName() == "wayland") {
        QMessageBox::warning(
        NULL, "Weave",
        "You are using a Wayland session, which is currently not fully supported by Weave.");
    }

	QSettings settings;
	AppConfig appConfig (&settings);

	app.switchTranslator(appConfig.language());

	MainWindow mainWindow(settings, appConfig);
	SetupWizard setupWizard(mainWindow, true);

	if (appConfig.wizardShouldRun())
	{
		setupWizard.show();
	}
	else
	{
		mainWindow.open();
	}

	instanceCoordinator.setActivationHandler([&mainWindow, &setupWizard]() {
		if (setupWizard.isVisible()) {
			setupWizard.showNormal();
			setupWizard.raise();
			setupWizard.activateWindow();
		}
		else {
			mainWindow.activateFromSecondaryInstance();
		}
	});

	return app.exec();
}

#if defined(Q_OS_MAC)
bool checkMacAssistiveDevices()
{
#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 1090 // mavericks

	// new in mavericks, applications are trusted individually
	// with use of the accessibility api. this call will show a
	// prompt which can show the security/privacy/accessibility
	// tab, with a list of allowed applications. barrier should
	// show up there automatically, but will be unchecked.

	if (AXIsProcessTrusted()) {
		return true;
	}

	const void* keys[] = { kAXTrustedCheckOptionPrompt };
	const void* trueValue[] = { kCFBooleanTrue };
	CFDictionaryRef options = CFDictionaryCreate(NULL, keys, trueValue, 1, NULL, NULL);

	bool result = AXIsProcessTrustedWithOptions(options);
	CFRelease(options);
	return result;

#else

	// now deprecated in mavericks.
	bool result = AXAPIEnabled();
	if (!result) {
		QMessageBox::information(
			NULL, "Weave",
			"Please enable access to assistive devices "
			"System Preferences -> Security & Privacy -> "
			"Privacy -> Accessibility, then re-open Weave.");
	}
	return result;

#endif
}
#endif
