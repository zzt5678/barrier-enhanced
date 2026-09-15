#include "CommandPaletteDialog.h"

#include "ActionBus.h"
#include "WorkflowStore.h"
#include "DialogPresentation.h"

#include <QDesktopServices>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
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
    m_commandList(new QListWidget(this)),
    m_empty(new QLabel(this))
{
    setWindowTitle(tr("Weave Command Palette"));
    resize(520, 360);

    m_filter->setPlaceholderText(tr("Type a command..."));

    auto* root = new QVBoxLayout(this);
    root->addWidget(m_filter);
    root->addWidget(m_commandList, 1);
    m_empty->setObjectName("paletteEmptyState");
    m_empty->setWordWrap(true);
    root->addWidget(m_empty);

    connect(m_filter, &QLineEdit::textChanged, this, &CommandPaletteDialog::filterCommands);
    connect(m_filter, &QLineEdit::returnPressed, this, &CommandPaletteDialog::executeSelected);
    connect(m_commandList, &QListWidget::itemDoubleClicked, this, &CommandPaletteDialog::executeSelected);
    connect(m_store, &WorkflowStore::historyChanged, this, &CommandPaletteDialog::refreshCommands);
    connect(m_store, &WorkflowStore::suggestionsChanged, this, &CommandPaletteDialog::refreshCommands);

    refreshCommands();
    DialogPresentation::fit(this, QSize(540, 400));
}

void CommandPaletteDialog::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        const QVariant selected = m_commandList->currentItem()
            ? m_commandList->currentItem()->data(Qt::UserRole) : QVariant();
        setWindowTitle(tr("Weave Command Palette"));
        m_filter->setPlaceholderText(tr("Type a command..."));
        refreshCommands();
        for (int i = 0; i < m_commandList->count(); ++i)
            if (m_commandList->item(i)->data(Qt::UserRole) == selected &&
                !m_commandList->item(i)->isHidden() &&
                m_commandList->item(i)->flags().testFlag(Qt::ItemIsEnabled))
                m_commandList->setCurrentRow(i);
    }
    QDialog::changeEvent(event);
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

    addCommand(QStringLiteral("open_workflow_hub"), tr("Open local history"), true);
    addCommand(QStringLiteral("save_latest_to_inbox"), tr("Save latest item to inbox"), hasHistory);
    addCommand(QStringLiteral("open_latest"), tr("Open latest item"), hasHistory);
    addCommand(QStringLiteral("reveal_latest"), tr("Reveal latest item"), hasHistory);
    addCommand(QStringLiteral("show_latest_suggestion"), tr("Run latest suggestion"), hasSuggestions);
    addCommand(QStringLiteral("capture_screenshot"), tr("Capture screenshot into local history"), true);
    addCommand(QStringLiteral("open_inbox"), tr("Open inbox folder"), true);

    filterCommands(m_filter->text());
}

void CommandPaletteDialog::filterCommands(const QString& query)
{
    const QString needle = query.trimmed().toLower();
    bool hasAvailable = false;
    for (int i = 0; i < m_commandList->count(); ++i) {
        QListWidgetItem* item = m_commandList->item(i);
        const bool matches = needle.isEmpty() || item->text().toLower().contains(needle);
        item->setHidden(!matches);
        hasAvailable |= matches && item->flags().testFlag(Qt::ItemIsEnabled);
    }
    m_empty->setText(tr("No available commands match. Clear the search or add an item to local history."));
    m_empty->setVisible(!hasAvailable);
    const auto* selected = m_commandList->currentItem();
    if (selected && !selected->isHidden() && selected->flags().testFlag(Qt::ItemIsEnabled)) return;
    m_commandList->setCurrentRow(-1);
    for (int i = 0; i < m_commandList->count(); ++i) {
        const auto* item = m_commandList->item(i);
        if (!item->isHidden() && item->flags().testFlag(Qt::ItemIsEnabled)) {
            m_commandList->setCurrentRow(i);
            break;
        }
    }
}

void CommandPaletteDialog::executeSelected()
{
    QListWidgetItem* item = m_commandList->currentItem();
    if (item == nullptr || item->isHidden() || item->flags().testFlag(Qt::ItemIsEnabled) == false) {
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
        const QString reason = id == "show_latest_suggestion"
            ? tr("No suggestions available yet") : tr("No item in local history yet");
        item->setText(label + " · " + reason);
        item->setToolTip(reason);
    }
}
