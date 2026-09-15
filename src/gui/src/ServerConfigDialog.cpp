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

#include "ServerConfigDialog.h"
#include "ServerConfig.h"
#include "HotkeyDialog.h"
#include "ActionDialog.h"
#include "DialogPresentation.h"
#include <QPushButton>
#include <QStyle>

#include <QtCore>
#include <QtGui>
#include <QMessageBox>

ServerConfigDialog::ServerConfigDialog(QWidget* parent, ServerConfig& config, const QString& defaultScreenName) :
    QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
    Ui::ServerConfigDialogBase(),
    m_OrigServerConfig(config),
    m_ServerConfig(config),
    m_ScreenSetupModel(serverConfig().screens(), serverConfig().numColumns(), serverConfig().numRows()),
    m_Message("")
{
    setupUi(this);
    setObjectName(QStringLiteral("ServerConfigDialogBase"));
    m_pScreenSetupView->setMinimumHeight(240);
    m_pScreenSetupView->setMaximumHeight(QWIDGETSIZE_MAX);
    m_pScreenSetupView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QPixmap trash = QIcon(":/res/icons/workspace/trash.png").pixmap(qRound(24 * devicePixelRatioF()), qRound(24 * devicePixelRatioF()));
    trash.setDevicePixelRatio(devicePixelRatioF());
    m_pTrashScreenWidget->setPixmap(trash);
    m_pLabelNewScreenWidget->setPixmap(QIcon(":/res/icons/workspace/devices.png").pixmap(32, 32));
    auto* screenLayout = qobject_cast<QVBoxLayout*>(m_pTabScreens->layout());
    screenLayout->setStretch(1, 1);
    auto* controls = new QHBoxLayout;
    auto* edit = new QPushButton(this);
    edit->setObjectName("editScreenButton");
    edit->setIcon(QIcon(":/res/icons/workspace/settings.png"));
    auto* remove = new QPushButton(this);
    remove->setObjectName("removeScreenButton");
    remove->setIcon(QIcon(":/res/icons/workspace/trash.png"));
    controls->addWidget(edit);
    controls->addWidget(remove);
    controls->addStretch();
    screenLayout->insertLayout(2, controls);
    connect(edit, &QPushButton::clicked, this, [this] {
        QKeyEvent key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(m_pScreenSetupView, &key);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        QKeyEvent key(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(m_pScreenSetupView, &key);
    });
    DialogPresentation::scrollEditor(this, QSize(940, 700));

    m_pCheckBoxHeartbeat->setChecked(true);
    m_pCheckBoxHeartbeat->setEnabled(false);
    m_pCheckBoxHeartbeat->setToolTip(tr("Heartbeat is always enabled to keep reconnects responsive."));
    m_pSpinBoxHeartbeat->setValue(serverConfig().heartbeat());

    m_pCheckBoxRelativeMouseMoves->setChecked(serverConfig().relativeMouseMoves());
    m_pCheckBoxScreenSaverSync->setChecked(serverConfig().screenSaverSync());
    m_pCheckBoxWin32KeepForeground->setChecked(serverConfig().win32KeepForeground());

    m_pCheckBoxSwitchDelay->setChecked(serverConfig().hasSwitchDelay());
    m_pSpinBoxSwitchDelay->setValue(serverConfig().switchDelay());

    m_pCheckBoxSwitchDoubleTap->setChecked(serverConfig().hasSwitchDoubleTap());
    m_pSpinBoxSwitchDoubleTap->setValue(serverConfig().switchDoubleTap());

    m_pCheckBoxCornerTopLeft->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::TopLeft));
    m_pCheckBoxCornerTopRight->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::TopRight));
    m_pCheckBoxCornerBottomLeft->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::BottomLeft));
    m_pCheckBoxCornerBottomRight->setChecked(serverConfig().switchCorner(BaseConfig::SwitchCorner::BottomRight));
    m_pSpinBoxSwitchCornerSize->setValue(serverConfig().switchCornerSize());

    m_pCheckBoxIgnoreAutoConfigClient->setChecked(serverConfig().ignoreAutoConfigClient());

    m_pCheckBoxEnableDragAndDrop->setChecked(serverConfig().enableDragAndDrop());

    m_pCheckBoxEnableClipboard->setChecked(serverConfig().clipboardSharing());

    for (const Hotkey& hotkey : serverConfig().hotkeys()) {
        m_pListHotkeys->addItem(hotkey.text());
    }

    m_pScreenSetupView->setModel(&m_ScreenSetupModel);
    m_pScreenSetupView->setCurrentIndex(m_ScreenSetupModel.index(serverConfig().numRows() / 2, serverConfig().numColumns() / 2));

    if (serverConfig().numScreens() == 0)
        model().screen(serverConfig().numColumns() / 2, serverConfig().numRows() / 2) = Screen(defaultScreenName);

    retranslateDynamicUi();
}

void ServerConfigDialog::retranslateDynamicUi()
{
    label_2->setText(tr("Arrange devices to match their positions on your desk."));
    label_3->setText(tr("Drag the device icon into an empty cell to add a screen. Select a cell and press Enter to edit; use Ctrl + arrow keys to move a device."));
    findChild<QPushButton*>("editScreenButton")->setText(tr("Add / Edit screen"));
    findChild<QPushButton*>("removeScreenButton")->setText(tr("Remove screen"));
    m_pLabelServerConfigSummary->setText(tr("Configured screens: %1 · Hotkeys: %2 · Clipboard sharing: %3")
        .arg(serverConfig().numScreens())
        .arg(serverConfig().hotkeys().size())
        .arg(serverConfig().clipboardSharing() ? tr("On") : tr("Off")));
}

void ServerConfigDialog::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi(this);
        retranslateDynamicUi();
    }
    QDialog::changeEvent(event);
}

void ServerConfigDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);

    if (!m_Message.isEmpty())
    {
        // TODO: ideally this message box should pop up after the dialog is shown
        QMessageBox::information(this, tr("Configure server"), m_Message);
    }
}

void ServerConfigDialog::accept()
{
    serverConfig().haveHeartbeat(true);
    serverConfig().setHeartbeat(m_pSpinBoxHeartbeat->value());

    serverConfig().setRelativeMouseMoves(m_pCheckBoxRelativeMouseMoves->isChecked());
    serverConfig().setScreenSaverSync(m_pCheckBoxScreenSaverSync->isChecked());
    serverConfig().setWin32KeepForeground(m_pCheckBoxWin32KeepForeground->isChecked());

    serverConfig().haveSwitchDelay(m_pCheckBoxSwitchDelay->isChecked());
    serverConfig().setSwitchDelay(m_pSpinBoxSwitchDelay->value());

    serverConfig().haveSwitchDoubleTap(m_pCheckBoxSwitchDoubleTap->isChecked());
    serverConfig().setSwitchDoubleTap(m_pSpinBoxSwitchDoubleTap->value());

    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::TopLeft,
                                   m_pCheckBoxCornerTopLeft->isChecked());
    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::TopRight,
                                   m_pCheckBoxCornerTopRight->isChecked());
    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::BottomLeft,
                                   m_pCheckBoxCornerBottomLeft->isChecked());
    serverConfig().setSwitchCorner(BaseConfig::SwitchCorner::BottomRight,
                                   m_pCheckBoxCornerBottomRight->isChecked());
    serverConfig().setSwitchCornerSize(m_pSpinBoxSwitchCornerSize->value());
    serverConfig().setIgnoreAutoConfigClient(m_pCheckBoxIgnoreAutoConfigClient->isChecked());
    serverConfig().setEnableDragAndDrop(m_pCheckBoxEnableDragAndDrop->isChecked());
    serverConfig().setClipboardSharing(m_pCheckBoxEnableClipboard->isChecked());

    // now that the dialog has been accepted, copy the new server config to the original one,
    // which is a reference to the one in MainWindow.
    setOrigServerConfig(serverConfig());

    QDialog::accept();
}

void ServerConfigDialog::on_m_pButtonNewHotkey_clicked()
{
    Hotkey hotkey;
    HotkeyDialog dlg(this, hotkey);
    if (dlg.exec() == QDialog::Accepted)
    {
        serverConfig().hotkeys().push_back(hotkey);
        m_pListHotkeys->addItem(hotkey.text());
    }
}

void ServerConfigDialog::on_m_pButtonEditHotkey_clicked()
{
    int idx = m_pListHotkeys->currentRow();
    Q_ASSERT(idx >= 0 && idx < serverConfig().hotkeys().size());
    Hotkey& hotkey = serverConfig().hotkeys()[idx];
    HotkeyDialog dlg(this, hotkey);
    if (dlg.exec() == QDialog::Accepted)
        m_pListHotkeys->currentItem()->setText(hotkey.text());
}

void ServerConfigDialog::on_m_pButtonRemoveHotkey_clicked()
{
    int idx = m_pListHotkeys->currentRow();
    Q_ASSERT(idx >= 0 && idx < serverConfig().hotkeys().size());
    serverConfig().hotkeys().erase(serverConfig().hotkeys().begin() + idx);
    m_pListActions->clear();
    delete m_pListHotkeys->item(idx);
}

void ServerConfigDialog::on_m_pListHotkeys_itemSelectionChanged()
{
    bool itemsSelected = !m_pListHotkeys->selectedItems().isEmpty();
    m_pButtonEditHotkey->setEnabled(itemsSelected);
    m_pButtonRemoveHotkey->setEnabled(itemsSelected);
    m_pButtonNewAction->setEnabled(itemsSelected);

    if (itemsSelected && serverConfig().hotkeys().size() > 0)
    {
        m_pListActions->clear();

        int idx = m_pListHotkeys->row(m_pListHotkeys->selectedItems()[0]);

        // There's a bug somewhere around here: We get idx == 1 right after we deleted the next to last item, so idx can
        // only possibly be 0. GDB shows we got called indirectly from the delete line in
        // on_m_pButtonRemoveHotkey_clicked() above, but the delete is of course necessary and seems correct.
        // The while() is a generalized workaround for all that and shouldn't be required.
        while (idx >= 0 && idx >= serverConfig().hotkeys().size())
            idx--;

        Q_ASSERT(idx >= 0 && idx < serverConfig().hotkeys().size());

        const Hotkey& hotkey = serverConfig().hotkeys()[idx];
        for (const Action& action : hotkey.actions()) {
            m_pListActions->addItem(action.text());
        }
    }
}

void ServerConfigDialog::on_m_pButtonNewAction_clicked()
{
    int idx = m_pListHotkeys->currentRow();
    Q_ASSERT(idx >= 0 && idx < serverConfig().hotkeys().size());
    Hotkey& hotkey = serverConfig().hotkeys()[idx];

    Action action;
    ActionDialog dlg(this, serverConfig(), hotkey, action);
    if (dlg.exec() == QDialog::Accepted)
    {
        hotkey.appendAction(action);
        m_pListActions->addItem(action.text());
    }
}

void ServerConfigDialog::on_m_pButtonEditAction_clicked()
{
    int idxHotkey = m_pListHotkeys->currentRow();
    Q_ASSERT(idxHotkey >= 0 && idxHotkey < serverConfig().hotkeys().size());
    Hotkey& hotkey = serverConfig().hotkeys()[idxHotkey];

    int idxAction = m_pListActions->currentRow();
    Q_ASSERT(idxAction >= 0 && idxAction < hotkey.actions().size());
    Action action = hotkey.actions()[idxAction];

    ActionDialog dlg(this, serverConfig(), hotkey, action);
    if (dlg.exec() == QDialog::Accepted) {
        hotkey.setAction(idxAction, action);
        m_pListActions->currentItem()->setText(action.text());
    }
}

void ServerConfigDialog::on_m_pButtonRemoveAction_clicked()
{
    int idxHotkey = m_pListHotkeys->currentRow();
    Q_ASSERT(idxHotkey >= 0 && idxHotkey < serverConfig().hotkeys().size());
    Hotkey& hotkey = serverConfig().hotkeys()[idxHotkey];

    int idxAction = m_pListActions->currentRow();
    Q_ASSERT(idxAction >= 0 && idxAction < hotkey.actions().size());

    hotkey.removeAction(idxAction);
    delete m_pListActions->currentItem();
}

void ServerConfigDialog::on_m_pListActions_itemSelectionChanged()
{
    m_pButtonEditAction->setEnabled(!m_pListActions->selectedItems().isEmpty());
    m_pButtonRemoveAction->setEnabled(!m_pListActions->selectedItems().isEmpty());
}
