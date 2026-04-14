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

#include "SettingsDialog.h"

#include "BarrierLocale.h"
#include "QBarrierApplication.h"
#include "QUtility.h"
#include "AppConfig.h"
#include "SslCertificate.h"
#include "MainWindow.h"

#include <QtCore>
#include <QtGui>
#include <QMessageBox>
#include <QFileDialog>
#include <QDir>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QDesktopServices>
#include <QUrl>

#if defined(Q_OS_MAC)
#include <ApplicationServices/ApplicationServices.h>
#endif

SettingsDialog::SettingsDialog(QWidget* parent, AppConfig& config) :
    QDialog(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint),
    Ui::SettingsDialogBase(),
    m_appConfig(config),
    m_pCheckBoxWorkflowEnabled(new QCheckBox(tr("Enable lightweight workflow handoff"), this)),
    m_pCheckBoxWorkflowSuggestions(new QCheckBox(tr("Enable suggestion cards"), this)),
    m_pSpinBoxWorkflowHistoryLimit(new QSpinBox(this)),
    m_pSpinBoxWorkflowDormantSeconds(new QSpinBox(this)),
    m_pLabelPlatformStatus(new QLabel(this)),
    m_pLabelPlatformDetail(new QLabel(this)),
    m_pButtonPlatformAction(new QPushButton(this))
{
    setupUi(this);

    m_Locale.fillLanguageComboBox(m_pComboLanguage);

    m_pLineEditScreenName->setText(appConfig().screenName());
    m_pSpinBoxPort->setValue(appConfig().port());
    m_pLineEditInterface->setText(appConfig().networkInterface());
    m_pComboLogLevel->setCurrentIndex(appConfig().logLevel());
    m_pCheckBoxLogToFile->setChecked(appConfig().logToFile());
    m_pLineEditLogFilename->setText(appConfig().logFilename());
    setIndexFromItemData(m_pComboLanguage, appConfig().language());
    m_pCheckBoxAutoHide->setChecked(appConfig().getAutoHide());
    m_pCheckBoxAutoStart->setChecked(appConfig().getAutoStart());
    m_pCheckBoxMinimizeToTray->setChecked(appConfig().getMinimizeToTray());
    m_pCheckBoxEnableDragDrop->setChecked(appConfig().getEnableDragDrop());
    m_pCheckBoxGameMode->setChecked(appConfig().getGameMode());
    m_pCheckBoxLowLatencyMode->setChecked(appConfig().getLowLatencyMode());
    m_pCheckBoxNestedRemoteMode->setChecked(appConfig().getNestedRemoteMode());
    m_pCheckBoxWorkflowEnabled->setChecked(appConfig().getWorkflowEnabled());
    m_pCheckBoxWorkflowSuggestions->setChecked(appConfig().getSuggestionsEnabled());
    m_pSpinBoxWorkflowHistoryLimit->setRange(10, 500);
    m_pSpinBoxWorkflowHistoryLimit->setValue(appConfig().getWorkflowHistoryLimit());
    m_pSpinBoxWorkflowDormantSeconds->setRange(10, 600);
    m_pSpinBoxWorkflowDormantSeconds->setValue(appConfig().getWorkflowDormantSeconds());
    m_pCheckBoxEnableCrypto->setChecked(m_appConfig.getCryptoEnabled());
    checkbox_require_client_certificate->setChecked(m_appConfig.getRequireClientCertificate());

    auto* historyRow = new QHBoxLayout;
    historyRow->addWidget(new QLabel(tr("Workflow history limit:"), this));
    historyRow->addWidget(m_pSpinBoxWorkflowHistoryLimit);
    historyRow->addStretch();

    auto* dormantRow = new QHBoxLayout;
    dormantRow->addWidget(new QLabel(tr("Dormant after seconds:"), this));
    dormantRow->addWidget(m_pSpinBoxWorkflowDormantSeconds);
    dormantRow->addStretch();

    verticalLayout_2->addWidget(m_pCheckBoxWorkflowEnabled);
    verticalLayout_2->addWidget(m_pCheckBoxWorkflowSuggestions);
    verticalLayout_2->addLayout(historyRow);
    verticalLayout_2->addLayout(dormantRow);

    auto* platformGroup = new QGroupBox(tr("Platform Readiness"), this);
    auto* platformLayout = new QVBoxLayout(platformGroup);
    m_pLabelPlatformDetail->setWordWrap(true);
    platformLayout->addWidget(m_pLabelPlatformStatus);
    platformLayout->addWidget(m_pLabelPlatformDetail);
    platformLayout->addWidget(m_pButtonPlatformAction, 0, Qt::AlignLeft);
    verticalLayout->insertWidget(3, platformGroup);
    connect(m_pButtonPlatformAction, &QPushButton::clicked, this, &SettingsDialog::onPlatformActionClicked);
    updatePlatformReadiness();

#if defined(Q_OS_WIN)
    m_pComboElevate->setCurrentIndex(static_cast<int>(appConfig().elevateMode()));
#else
    // elevate checkbox is only useful on ms windows.
    m_pLabelElevate->hide();
    m_pComboElevate->hide();
#endif
}

void SettingsDialog::accept()
{
    m_appConfig.setScreenName(m_pLineEditScreenName->text());
    m_appConfig.setPort(m_pSpinBoxPort->value());
    m_appConfig.setNetworkInterface(m_pLineEditInterface->text());
    m_appConfig.setCryptoEnabled(m_pCheckBoxEnableCrypto->isChecked());
    m_appConfig.setRequireClientCertificate(checkbox_require_client_certificate->isChecked());
    m_appConfig.setLogLevel(m_pComboLogLevel->currentIndex());
    m_appConfig.setLogToFile(m_pCheckBoxLogToFile->isChecked());
    m_appConfig.setLogFilename(m_pLineEditLogFilename->text());
    m_appConfig.setLanguage(m_pComboLanguage->itemData(m_pComboLanguage->currentIndex()).toString());
    m_appConfig.setElevateMode(static_cast<ElevateMode>(m_pComboElevate->currentIndex()));
    m_appConfig.setAutoHide(m_pCheckBoxAutoHide->isChecked());
    m_appConfig.setAutoStart(m_pCheckBoxAutoStart->isChecked());
    m_appConfig.setMinimizeToTray(m_pCheckBoxMinimizeToTray->isChecked());
    m_appConfig.setEnableDragDrop(m_pCheckBoxEnableDragDrop->isChecked());
    m_appConfig.setGameMode(m_pCheckBoxGameMode->isChecked());
    m_appConfig.setLowLatencyMode(m_pCheckBoxLowLatencyMode->isChecked());
    m_appConfig.setNestedRemoteMode(m_pCheckBoxNestedRemoteMode->isChecked());
    m_appConfig.setWorkflowEnabled(m_pCheckBoxWorkflowEnabled->isChecked());
    m_appConfig.setSuggestionsEnabled(m_pCheckBoxWorkflowSuggestions->isChecked());
    m_appConfig.setWorkflowHistoryLimit(m_pSpinBoxWorkflowHistoryLimit->value());
    m_appConfig.setWorkflowDormantSeconds(m_pSpinBoxWorkflowDormantSeconds->value());
    m_appConfig.saveSettings();
    QDialog::accept();
}

void SettingsDialog::reject()
{
    if (m_appConfig.language() != m_pComboLanguage->itemData(m_pComboLanguage->currentIndex()).toString()) {
        QBarrierApplication::getInstance()->switchTranslator(m_appConfig.language());
    }
    QDialog::reject();
}

void SettingsDialog::changeEvent(QEvent* event)
{
    if (event != 0)
    {
        switch (event->type())
        {
        case QEvent::LanguageChange:
            {
                int logLevelIndex = m_pComboLogLevel->currentIndex();

                m_pComboLanguage->blockSignals(true);
                retranslateUi(this);
                m_pComboLanguage->blockSignals(false);

                m_pComboLogLevel->setCurrentIndex(logLevelIndex);
                break;
            }

        default:
            QDialog::changeEvent(event);
        }
    }
}

void SettingsDialog::on_m_pCheckBoxLogToFile_stateChanged(int i)
{
    bool checked = i == 2;

    m_pLineEditLogFilename->setEnabled(checked);
    m_pButtonBrowseLog->setEnabled(checked);
}

void SettingsDialog::on_m_pButtonBrowseLog_clicked()
{
    QString fileName = QFileDialog::getSaveFileName(
        this, tr("Save log file to..."),
        m_pLineEditLogFilename->text(),
        "Logs (*.log *.txt)");

    if (!fileName.isEmpty())
    {
        m_pLineEditLogFilename->setText(fileName);
    }
}

void SettingsDialog::on_m_pComboLanguage_currentIndexChanged(int index)
{
    QString ietfCode = m_pComboLanguage->itemData(index).toString();
    QBarrierApplication::getInstance()->switchTranslator(ietfCode);
}

void SettingsDialog::updatePlatformReadiness()
{
#if defined(Q_OS_MAC)
    const bool trusted = AXIsProcessTrusted();
    m_pLabelPlatformStatus->setText(trusted
        ? tr("Accessibility access is ready")
        : tr("Accessibility access is still required"));
    m_pLabelPlatformDetail->setText(trusted
        ? tr("Weave can capture and inject input without any extra setup on this Mac.")
        : tr("Click below and macOS will open the Accessibility authorization prompt for Weave."));
    m_pButtonPlatformAction->setVisible(true);
    m_pButtonPlatformAction->setText(tr("Prompt Accessibility Access"));
#elif defined(Q_OS_WIN)
    m_pLabelPlatformStatus->setText(tr("Windows permission flow is handled in-app"));
    m_pLabelPlatformDetail->setText(tr("Keep Elevate set to As Needed so Weave can prompt when UAC or desktop switching requires it."));
    m_pButtonPlatformAction->setVisible(false);
#elif defined(WINAPI_XWINDOWS)
    const bool isWayland = QGuiApplication::platformName() == QStringLiteral("wayland");
    m_pLabelPlatformStatus->setText(isWayland
        ? tr("Wayland session detected")
        : tr("X11 session detected"));
    m_pLabelPlatformDetail->setText(isWayland
        ? tr("Wayland still limits input control. Use an X11 session for full Weave support.")
        : tr("X11 is ready. Weave does not need extra desktop authorization on this session."));
    m_pButtonPlatformAction->setVisible(isWayland);
    m_pButtonPlatformAction->setText(tr("Open Compatibility Guide"));
#else
    m_pLabelPlatformStatus->setText(tr("No additional readiness checks"));
    m_pLabelPlatformDetail->setText(tr("This platform does not require a separate permission prompt here."));
    m_pButtonPlatformAction->setVisible(false);
#endif
}

void SettingsDialog::onPlatformActionClicked()
{
#if defined(Q_OS_MAC)
    const void* keys[] = { kAXTrustedCheckOptionPrompt };
    const void* values[] = { kCFBooleanTrue };
    CFDictionaryRef options = CFDictionaryCreate(NULL, keys, values, 1, NULL, NULL);
    AXIsProcessTrustedWithOptions(options);
    CFRelease(options);
#elif defined(WINAPI_XWINDOWS)
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/debauchee/barrier/wiki/FAQ")));
#endif
    updatePlatformReadiness();
}
