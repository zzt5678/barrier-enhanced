#pragma once

#include <QDialog>

class ActionBus;
class QLabel;
class QListWidget;
class QPushButton;
class QShowEvent;
class QHideEvent;
class QTabWidget;
class QTextBrowser;
class WorkflowStore;

class WorkflowHubDialog : public QDialog
{
    Q_OBJECT

public:
    WorkflowHubDialog(WorkflowStore& store, ActionBus& actionBus, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void refreshAll();
    void updateDetails();
    void executeSuggestion();
    void openSelected();
    void saveSelected();
    void revealSelected();
    void captureScreenshot();
    void openInbox();

private:
    QString selectedContextId() const;
    QString selectedSuggestionActionId() const;
    void setDetailHtml(const QString& html, const QString& previewPath);
    bool executeAction(const QString& actionId, const QString& contextId);

private:
    WorkflowStore* m_store;
    ActionBus* m_actionBus;
    QLabel* m_runtimeLabel;
    QLabel* m_capabilityLabel;
    QLabel* m_previewLabel;
    QTabWidget* m_tabs;
    QListWidget* m_historyList;
    QListWidget* m_suggestionList;
    QListWidget* m_receiptList;
    QTextBrowser* m_detail;
    QPushButton* m_executeSuggestionButton;
    QPushButton* m_openButton;
    QPushButton* m_saveButton;
    QPushButton* m_revealButton;
};
