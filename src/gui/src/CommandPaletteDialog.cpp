#include "CommandPaletteDialog.h"

#include "ActionBus.h"
#include "WorkflowStore.h"

#include <QDesktopServices>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QUrl>
#include <QVBoxLayout>

CommandPaletteDialog::CommandPaletteDialog(WorkflowStore& store, ActionBus& actionBus, QWidget* parent) :
    QDialog(parent),
    m_store(&store),
    m_actionBus(&actionBus),
    m_filter(new QLineEdit(this)),
    m_commandList(new QListWidget(this))
{
    setWindowTitle(tr("Barrier Command Palette"));
    resize(520, 360);

    m_filter->setPlaceholderText(tr("Type a command..."));

    auto* root = new QVBoxLayout(this);
    root->addWidget(m_filter);
    root->addWidget(m_commandList, 1);

    connect(m_filter, &QLineEdit::textChanged, this, &CommandPaletteDialog::filterCommands);
    connect(m_filter, &QLineEdit::returnPressed, this, &CommandPaletteDialog::executeSelected);
    connect(m_commandList, &QListWidget::itemDoubleClicked, this, &CommandPaletteDialog::executeSelected);
    connect(m_store, &WorkflowStore::historyChanged, this, &CommandPaletteDialog::refreshCommands);
    connect(m_store, &WorkflowStore::suggestionsChanged, this, &CommandPaletteDialog::refreshCommands);

    refreshCommands();
}

void CommandPaletteDialog::showEvent(QShowEvent* event)
{
    m_store->setInteractiveSessionOpen(true);
    refreshCommands();
    m_filter->clear();
    m_filter->setFocus();
    QDialog::showEvent(event);
}

void CommandPaletteDialog::hideEvent(QHideEvent* event)
{
    m_store->setInteractiveSessionOpen(false);
    QDialog::hideEvent(event);
}

void CommandPaletteDialog::refreshCommands()
{
    m_commandList->clear();

    const bool hasHistory = !m_store->history().isEmpty();
    const bool hasSuggestions = !m_store->suggestions().isEmpty();

    addCommand(QStringLiteral("open_workflow_hub"), tr("Open Workflow Hub"), true);
    addCommand(QStringLiteral("save_latest_to_inbox"), tr("Save latest item to Inbox"), hasHistory);
    addCommand(QStringLiteral("open_latest"), tr("Open latest item"), hasHistory);
    addCommand(QStringLiteral("reveal_latest"), tr("Reveal latest item"), hasHistory);
    addCommand(QStringLiteral("show_latest_suggestion"), tr("Run latest suggestion"), hasSuggestions);
    addCommand(QStringLiteral("capture_screenshot"), tr("Capture screenshot into workflow history"), true);
    addCommand(QStringLiteral("open_inbox"), tr("Open Workflow Inbox"), true);

    filterCommands(m_filter->text());
    if (m_commandList->currentRow() < 0 && m_commandList->count() > 0) {
        m_commandList->setCurrentRow(0);
    }
}

void CommandPaletteDialog::filterCommands(const QString& query)
{
    const QString needle = query.trimmed().toLower();
    for (int i = 0; i < m_commandList->count(); ++i) {
        QListWidgetItem* item = m_commandList->item(i);
        const bool matches = needle.isEmpty() || item->text().toLower().contains(needle);
        item->setHidden(!matches);
    }
}

void CommandPaletteDialog::executeSelected()
{
    QListWidgetItem* item = m_commandList->currentItem();
    if (item == nullptr || item->flags().testFlag(Qt::ItemIsEnabled) == false) {
        return;
    }

    const QString commandId = item->data(Qt::UserRole).toString();
    if (commandId == QStringLiteral("open_workflow_hub")) {
        emit workflowHubRequested();
        accept();
        return;
    }

    if (commandId == QStringLiteral("open_inbox")) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_store->inboxDir()));
        accept();
        return;
    }

    QString errorMessage;
    if (!m_actionBus->executeQuickCommand(commandId, this, &errorMessage) && !errorMessage.isEmpty()) {
        QMessageBox::warning(this, tr("Command Failed"), errorMessage);
        return;
    }

    accept();
}

void CommandPaletteDialog::addCommand(const QString& id, const QString& label, bool enabled)
{
    auto* item = new QListWidgetItem(label, m_commandList);
    item->setData(Qt::UserRole, id);
    if (!enabled) {
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
    }
}
