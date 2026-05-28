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

#define TRAY_RETRY_COUNT 5
#define TRAY_RETRY_WAIT 2000

#include "QBarrierApplication.h"
#include "MainWindow.h"
#include "AppConfig.h"
#include "SetupWizard.h"
#include "DisplayIsValid.h"

#include <QtCore>
#include <QtGui>
#include <QSettings>
#include <QMessageBox>
#include <QLockFile>
#include <QDir>
#include <QFileInfo>

#if defined(Q_OS_MAC)
#include <Carbon/Carbon.h>
#endif

#ifdef Q_OS_DARWIN
#include <cstdlib>
#endif

class QThreadImpl : public QThread
{
public:
	static void msleep(unsigned long msecs)
	{
		QThread::msleep(msecs);
	}
};

int waitForTray();

#if defined(Q_OS_MAC)
bool checkMacAssistiveDevices();
#endif

namespace {

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

	// Single instance lock - prevent multiple barrier GUI instances
	// This fixes the tray icon duplication issue when restarting barrier
	QLockFile lockFile(QDir::temp().absoluteFilePath("weave-gui.lock"));
	lockFile.setStaleLockTime(30000);
    bool locked = false;
    for (int attempt = 0; attempt < 20 && !locked; ++attempt) {
        locked = lockFile.tryLock(100);
        if (!locked) {
            QThreadImpl::msleep(100);
        }
    }
	if (!locked) {
		QMessageBox::warning(nullptr, "Weave",
			"Weave is already running.\n\n"
			"If you need to restart, please quit the existing instance first.");
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

	int trayAvailable = waitForTray();

	QApplication::setQuitOnLastWindowClosed(false);

    if (QGuiApplication::platformName() == "wayland") {
        QMessageBox::warning(
        NULL, "Weave",
        "You are using a Wayland session, which is currently not fully supported by Weave.");
    }

	QSettings settings;
	AppConfig appConfig (&settings);

	if (appConfig.getAutoHide() && !trayAvailable)
	{
		// force auto hide to false - otherwise there is no way to get the GUI back
		fprintf(stdout, "System tray not available, force disabling auto hide!\n");
		appConfig.setAutoHide(false);
	}

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

	return app.exec();
}

int waitForTray()
{
	// on linux, the system tray may not be available immediately after logging in,
	// so keep retrying but give up after a short time.
	int trayAttempts = 0;
	while (true)
	{
		if (QSystemTrayIcon::isSystemTrayAvailable())
		{
			break;
		}

		if (++trayAttempts > TRAY_RETRY_COUNT)
		{
			fprintf(stdout, "System tray is unavailable.\n");
			return false;
		}

		QThreadImpl::msleep(TRAY_RETRY_WAIT);
	}
	return true;
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
