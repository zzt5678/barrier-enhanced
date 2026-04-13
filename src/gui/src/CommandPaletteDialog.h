#pragma once

#include <QDialog>

class ActionBus;
class QHideEvent;
class QListWidget;
class QLineEdit;
class QShowEvent;
class WorkflowStore;

class CommandPaletteDialog : public QDialog
{
    Q_OBJECT

public:
    CommandPaletteDialog(WorkflowStore& store, ActionBus& actionBus, QWidget* parent = nullptr);

signals:
    void workflowHubRequested();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void refreshCommands();
    void filterCommands(const QString& query);
    void executeSelected();

private:
    void addCommand(const QString& id, const QString& label, bool enabled = true);

private:
    WorkflowStore* m_store;
    ActionBus* m_actionBus;
    QLineEdit* m_filter;
    QListWidget* m_commandList;
};
