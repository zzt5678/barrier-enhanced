/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
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

#include "SetupWizard.h"
#include "MainWindow.h"
#include "QBarrierApplication.h"
#include "QUtility.h"
#include "AppConfig.h"
#include "DisplayIsValid.h"

#include <QMessageBox>
#include <QDesktopServices>
#include <QOperatingSystemVersion>
#include <QUrl>

#if defined(Q_OS_MAC)
#include <ApplicationServices/ApplicationServices.h>
#endif

SetupWizard::SetupWizard(MainWindow& mainWindow, bool startMain) :
    m_MainWindow(mainWindow),
    m_StartMain(startMain)
{
    setupUi(this);

#if defined(Q_OS_MAC)

    // the mac style needs a little more room because of the
    // graphic on the left.
    resize(600, 500);
    setMinimumSize(size());

#elif defined(Q_OS_WIN)

    // when aero is disabled on windows, the next/back buttons
    // are hidden (must be a qt bug) -- resizing the window
    // to +1 of the original height seems to fix this.
    // NOTE: calling setMinimumSize after this will break
    // it again, so don't do that.
    resize(size().width(), size().height() + 1);

#endif

    connect(m_pServerRadioButton, SIGNAL(toggled(bool)), m_MainWindow.m_pGroupServer, SLOT(setChecked(bool)));
    connect(m_pClientRadioButton, SIGNAL(toggled(bool)), m_MainWindow.m_pGroupClient, SLOT(setChecked(bool)));

    m_Locale.fillLanguageComboBox(m_pComboLanguage);
    setIndexFromItemData(m_pComboLanguage, m_MainWindow.appConfig().language());
    updatePermissionsPage();
}

SetupWizard::~SetupWizard()
{
}

bool SetupWizard::validateCurrentPage()
{
    QMessageBox message;
    message.setWindowTitle(tr("Setup Weave"));
    message.setIcon(QMessageBox::Information);

    if (currentPage() == m_pNodePage)
    {
        bool result = m_pClientRadioButton->isChecked() ||
                 m_pServerRadioButton->isChecked();

        if (!result)
        {
            message.setText(tr("Please select an option."));
            message.exec();
            return false;
        }
    }

    return true;
}

void SetupWizard::changeEvent(QEvent* event)
{
    if (event != 0)
    {
        switch (event->type())
        {
        case QEvent::LanguageChange:
            {
                m_pComboLanguage->blockSignals(true);
                retranslateUi(this);
                m_pComboLanguage->blockSignals(false);
                updatePermissionsPage();
                break;
            }

        default:
            QWizard::changeEvent(event);
        }
    }
}

void SetupWizard::accept()
{
    AppConfig& appConfig = m_MainWindow.appConfig();

    appConfig.setLanguage(m_pComboLanguage->itemData(m_pComboLanguage->currentIndex()).toString());

    appConfig.setWizardHasRun();
    appConfig.saveSettings();

    QSettings& settings = m_MainWindow.settings();
    if (m_pServerRadioButton->isChecked())
    {
        settings.setValue("groupServerChecked", true);
        settings.setValue("groupClientChecked", false);
    }
    if (m_pClientRadioButton->isChecked())
    {
        settings.setValue("groupClientChecked", true);
        settings.setValue("groupServerChecked", false);
    }

    QWizard::accept();

    if (m_StartMain)
    {
        m_MainWindow.updateZeroconfService();
        m_MainWindow.open();
    }
}

void SetupWizard::reject()
{
    QBarrierApplication::getInstance()->switchTranslator(m_MainWindow.appConfig().language());

    if (m_StartMain)
    {
        m_MainWindow.open();
    }

    QWizard::reject();
}

void SetupWizard::on_m_pComboLanguage_currentIndexChanged(int index)
{
    QString ietfCode = m_pComboLanguage->itemData(index).toString();
    QBarrierApplication::getInstance()->switchTranslator(ietfCode);
}

void SetupWizard::on_m_pButtonPermissionRefresh_clicked()
{
    updatePermissionsPage();
}

void SetupWizard::on_m_pButtonPermissionAction_clicked()
{
    triggerPermissionAction();
    updatePermissionsPage();
}

void SetupWizard::updatePermissionsPage()
{
    m_pPermissionSummary->setText(permissionSummaryText());
    m_pPermissionDetails->setText(permissionDetailText());

#if defined(Q_OS_MAC)
    m_pButtonPermissionAction->setVisible(true);
    m_pButtonPermissionAction->setText(tr("Prompt Accessibility Access"));
#elif defined(Q_OS_WIN)
    m_pButtonPermissionAction->setVisible(false);
#elif defined(WINAPI_XWINDOWS)
    const bool isWayland = QGuiApplication::platformName() == QStringLiteral("wayland");
    m_pButtonPermissionAction->setVisible(isWayland);
    m_pButtonPermissionAction->setText(tr("Open Wayland Guidance"));
#else
    m_pButtonPermissionAction->setVisible(false);
#endif
}

bool SetupWizard::currentPlatformReady() const
{
#if defined(Q_OS_MAC)
    return AXIsProcessTrusted();
#elif defined(WINAPI_XWINDOWS)
    if (QGuiApplication::platformName() == QStringLiteral("wayland")) {
        return false;
    }
    return display_is_valid();
#else
    return true;
#endif
}

QString SetupWizard::permissionSummaryText() const
{
    return currentPlatformReady()
        ? tr("This device is ready for Weave control.")
        : tr("Weave still needs one platform permission or compatibility step before full control is available.");
}

QString SetupWizard::permissionDetailText() const
{
#if defined(Q_OS_MAC)
    if (AXIsProcessTrusted()) {
        return tr("Accessibility access is already granted. You can finish setup and start sharing immediately.");
    }
    return tr("Weave needs Accessibility access to capture and inject input. Click the button below and macOS will open the authorization prompt for you.");
#elif defined(Q_OS_WIN)
    return tr("Windows works best with Elevate set to As Needed. Weave will prompt for UAC when the desktop context changes, so no separate permissions page is required.");
#elif defined(WINAPI_XWINDOWS)
    if (QGuiApplication::platformName() == QStringLiteral("wayland")) {
        return tr("This session is running on Wayland. Clipboard and discovery may work, but full input control is limited. Use an X11 session for the smoothest setup.");
    }
    return tr("X11 is available and no additional desktop authorization is required. Finish setup and connect your other machine.");
#else
    return tr("No additional platform permission checks are required for this operating system.");
#endif
}

void SetupWizard::triggerPermissionAction()
{
#if defined(Q_OS_MAC)
    const void* keys[] = { kAXTrustedCheckOptionPrompt };
    const void* values[] = { kCFBooleanTrue };
    CFDictionaryRef options = CFDictionaryCreate(NULL, keys, values, 1, NULL, NULL);
    AXIsProcessTrustedWithOptions(options);
    CFRelease(options);
#elif defined(WINAPI_XWINDOWS)
    if (QGuiApplication::platformName() == QStringLiteral("wayland")) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/debauchee/barrier/wiki/FAQ")));
    }
#endif
}
