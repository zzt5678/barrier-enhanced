#include "WorkflowHubDialog.h"

#include "ActionBus.h"
#include "WorkflowStore.h"
#include "ElidingLabel.h"
#include "DialogPresentation.h"
#include "WorkflowDisplayText.h"
#include <QEvent>

#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QString htmlEscape(const QString& text)
{
    QString escaped = text;
    escaped.replace('&', QStringLiteral("&amp;"));
    escaped.replace('<', QStringLiteral("&lt;"));
    escaped.replace('>', QStringLiteral("&gt;"));
    escaped.replace('"', QStringLiteral("&quot;"));
    return escaped;
}

QString formatDateTime(const QDateTime& value)
{
    return value.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

} // namespace

WorkflowHubDialog::WorkflowHubDialog(WorkflowStore& store, ActionBus& actionBus, QWidget* parent) :
    QDialog(parent),
    m_store(&store),
    m_actionBus(&actionBus),
    m_runtimeLabel(new ElidingLabel(this)),
    m_capabilityLabel(new QLabel(this)),
    m_previewLabel(new QLabel(this)),
    m_tabs(new QTabWidget(this)),
    m_historyList(new QListWidget(this)),
    m_suggestionList(new QListWidget(this)),
    m_receiptList(new QListWidget(this)),
    m_detail(new QTextBrowser(this)),
    m_executeSuggestionButton(new QPushButton(tr("Execute Suggestion"), this)),
    m_openButton(new QPushButton(tr("Open"), this)),
    m_saveButton(new QPushButton(tr("Save to Inbox"), this)),
    m_revealButton(new QPushButton(tr("Reveal"), this))
{
    setWindowTitle(tr("Weave Workflow Hub"));
    resize(860, 620);
    setObjectName(QStringLiteral("WorkflowHubDialog"));
    m_runtimeLabel->setObjectName(QStringLiteral("workflowRuntime"));
    m_capabilityLabel->setWordWrap(true);
    m_runtimeLabel->setTextFormat(Qt::PlainText);
    m_capabilityLabel->setTextFormat(Qt::PlainText);

    m_previewLabel->setAlignment(Qt::AlignCenter);
    m_previewLabel->setMinimumHeight(140);
    m_previewLabel->setText(tr("No preview"));
    m_previewLabel->setObjectName(QStringLiteral("m_pWorkflowPreview"));

    m_tabs->addTab(m_historyList, tr("History"));
    m_tabs->addTab(m_suggestionList, tr("Suggestions"));
    m_tabs->addTab(m_receiptList, tr("Receipts"));
    m_tabs->setObjectName(QStringLiteral("m_pWorkflowTabs"));

    auto* titleLabel = new QLabel(tr("Workflow Hub"), this);
    titleLabel->setObjectName(QStringLiteral("m_pLabelWorkflowSectionTitle"));
    auto* subtitleLabel = new QLabel(
        tr("Review transferred context, suggested follow-up actions, and every receipt Weave records while you move content between devices."),
        this);
    subtitleLabel->setObjectName(QStringLiteral("m_pLabelReceiptSummary"));
    subtitleLabel->setWordWrap(true);

    auto* headerLayout = new QVBoxLayout;
    headerLayout->addWidget(titleLabel);
    headerLayout->addWidget(subtitleLabel);
    headerLayout->addWidget(m_runtimeLabel);
    headerLayout->addWidget(m_capabilityLabel);

    auto* contentLayout = new QHBoxLayout;
    contentLayout->setObjectName("historyContentLayout");
    contentLayout->setSpacing(18);
    auto* leftCard = new QFrame(this);
    leftCard->setObjectName(QStringLiteral("workflowCard"));
    leftCard->setFrameShape(QFrame::StyledPanel);
    leftCard->setFrameShadow(QFrame::Raised);
    auto* leftLayout = new QVBoxLayout(leftCard);
    auto* leftTitle = new QLabel(tr("Collected Context"), leftCard);
    leftTitle->setObjectName(QStringLiteral("m_pLabelWorkflowSectionTitle"));
    auto* leftSubtitle = new QLabel(tr("Switch between history, suggestions, and receipts without leaving the desktop."), leftCard);
    leftSubtitle->setObjectName(QStringLiteral("m_pLabelReceiptSummary"));
    leftSubtitle->setWordWrap(true);
    leftLayout->addWidget(leftTitle);
    leftLayout->addWidget(leftSubtitle);
    leftLayout->addWidget(m_tabs, 1);
    contentLayout->addWidget(leftCard, 2);

    auto* rightCard = new QFrame(this);
    rightCard->setObjectName(QStringLiteral("workflowCard"));
    rightCard->setFrameShape(QFrame::StyledPanel);
    rightCard->setFrameShadow(QFrame::Raised);
    auto* detailLayout = new QVBoxLayout;
    auto* detailTitle = new QLabel(tr("Inspector"), rightCard);
    detailTitle->setObjectName(QStringLiteral("m_pLabelWorkflowSectionTitle"));
    auto* detailSubtitle = new QLabel(tr("Preview payloads, inspect transfer metadata, and execute the next step from one place."), rightCard);
    detailSubtitle->setObjectName(QStringLiteral("m_pLabelReceiptSummary"));
    detailSubtitle->setWordWrap(true);
    detailLayout->addWidget(detailTitle);
    detailLayout->addWidget(detailSubtitle);
    detailLayout->addWidget(m_previewLabel);
    detailLayout->addWidget(m_detail, 1);
    rightCard->setLayout(detailLayout);
    contentLayout->addWidget(rightCard, 3);

    auto* actionLayout = new QGridLayout;
    actionLayout->setObjectName("historyActionsLayout");
    actionLayout->setProperty("columns", 4);
    auto* screenshotButton = new QPushButton(tr("Capture Screenshot"), this);
    auto* inboxButton = new QPushButton(tr("Open Inbox"), this);
    auto* cancelSaveButton = new QPushButton(tr("Cancel Save"), this);
    cancelSaveButton->setToolTip(tr("Cancel the active inbox save; completed files are kept."));
    connect(cancelSaveButton, &QPushButton::clicked, m_actionBus, &ActionBus::cancelInboxSave);
    auto* closeButton = new QPushButton(tr("Close"), this);
    const QList<QPair<QWidget*, const char*>> translatedWidgets {
        {titleLabel, "Workflow Hub"},
        {subtitleLabel, "Review transferred context, suggested follow-up actions, and every receipt Weave records while you move content between devices."},
        {leftTitle, "Collected Context"},
        {leftSubtitle, "Switch between history, suggestions, and receipts without leaving the desktop."},
        {detailTitle, "Inspector"},
        {detailSubtitle, "Preview payloads, inspect transfer metadata, and execute the next step from one place."},
        {m_executeSuggestionButton, "Execute Suggestion"}, {m_openButton, "Open"},
        {m_saveButton, "Save to Inbox"}, {m_revealButton, "Reveal"},
        {screenshotButton, "Capture Screenshot"}, {inboxButton, "Open Inbox"},
        {cancelSaveButton, "Cancel Save"}, {closeButton, "Close"}
    };
    for (const auto& entry : translatedWidgets) entry.first->setProperty("translationSource", entry.second);
    const QList<QPushButton*> actions{m_executeSuggestionButton, m_openButton,
        m_saveButton, m_revealButton, screenshotButton, inboxButton, cancelSaveButton, closeButton};
    for (int index = 0; index < actions.size(); ++index) {
        actions[index]->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        actionLayout->addWidget(actions[index], index / 4, index % 4);
    }
    actionLayout->setHorizontalSpacing(10);
    actionLayout->setVerticalSpacing(10);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(18);
    auto* body = new QWidget(this);
    body->setObjectName("historyBody");
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->addLayout(headerLayout);
    bodyLayout->addLayout(contentLayout, 1);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName("historyBodyScroll");
    scroll->setStyleSheet("QScrollArea#historyBodyScroll, QWidget#historyBody { background: #faf9f6; }");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setMinimumHeight(120);
    scroll->setWidget(body);
    root->addWidget(scroll, 1);
    root->addLayout(actionLayout);

    connect(m_tabs, &QTabWidget::currentChanged, this, &WorkflowHubDialog::updateDetails);
    connect(m_historyList, &QListWidget::currentItemChanged, this, &WorkflowHubDialog::updateDetails);
    connect(m_suggestionList, &QListWidget::currentItemChanged, this, &WorkflowHubDialog::updateDetails);
    connect(m_receiptList, &QListWidget::currentItemChanged, this, &WorkflowHubDialog::updateDetails);
    connect(m_suggestionList, &QListWidget::itemDoubleClicked, this, &WorkflowHubDialog::executeSuggestion);
    connect(m_historyList, &QListWidget::itemDoubleClicked, this, &WorkflowHubDialog::openSelected);
    connect(m_executeSuggestionButton, &QPushButton::clicked, this, &WorkflowHubDialog::executeSuggestion);
    connect(m_openButton, &QPushButton::clicked, this, &WorkflowHubDialog::openSelected);
    connect(m_saveButton, &QPushButton::clicked, this, &WorkflowHubDialog::saveSelected);
    connect(m_revealButton, &QPushButton::clicked, this, &WorkflowHubDialog::revealSelected);
    connect(screenshotButton, &QPushButton::clicked, this, &WorkflowHubDialog::captureScreenshot);
    connect(inboxButton, &QPushButton::clicked, this, &WorkflowHubDialog::openInbox);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

    connect(m_store, &WorkflowStore::historyChanged, this, &WorkflowHubDialog::refreshAll);
    connect(m_store, &WorkflowStore::suggestionsChanged, this, &WorkflowHubDialog::refreshAll);
    connect(m_store, &WorkflowStore::receiptsChanged, this, &WorkflowHubDialog::refreshAll);
    connect(m_store, &WorkflowStore::runtimeModeChanged, this, &WorkflowHubDialog::refreshAll);

    refreshAll();
    DialogPresentation::fit(this, QSize(920, 700));
}

void WorkflowHubDialog::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        setWindowTitle(tr("Weave Workflow Hub"));
        m_tabs->setTabText(0, tr("History"));
        m_tabs->setTabText(1, tr("Suggestions"));
        m_tabs->setTabText(2, tr("Receipts"));
        for (auto* widget : findChildren<QWidget*>()) {
            const QByteArray source = widget->property("translationSource").toByteArray();
            if (source.isEmpty()) continue;
            if (auto* label = qobject_cast<QLabel*>(widget)) label->setText(tr(source.constData()));
            if (auto* button = qobject_cast<QPushButton*>(widget)) button->setText(tr(source.constData()));
        }
        refreshAll();
    }
    QDialog::changeEvent(event);
}

void WorkflowHubDialog::resizeEvent(QResizeEvent* event)
{
    auto* content = findChild<QHBoxLayout*>("historyContentLayout");
    if (content) content->setDirection(width() < 720 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    auto* actions = findChild<QGridLayout*>("historyActionsLayout");
    const int columns = width() < 720 ? 2 : 4;
    if (actions && actions->property("columns").toInt() != columns) {
        QList<QWidget*> buttons;
        while (auto* item = actions->takeAt(0)) { buttons.append(item->widget()); delete item; }
        for (int i = 0; i < buttons.size(); ++i) actions->addWidget(buttons[i], i / columns, i % columns);
        actions->setProperty("columns", columns);
    }
    QDialog::resizeEvent(event);
}

void WorkflowHubDialog::showEvent(QShowEvent* event)
{
    m_store->setInteractiveSessionOpen(true);
    QDialog::showEvent(event);
    refreshAll();
}

void WorkflowHubDialog::hideEvent(QHideEvent* event)
{
    m_store->setInteractiveSessionOpen(false);
    QDialog::hideEvent(event);
}

void WorkflowHubDialog::refreshAll()
{
    const QString selectedHistoryId = m_historyList->currentItem()
        ? m_historyList->currentItem()->data(Qt::UserRole).toString()
        : QString();
    const QString selectedSuggestionId = m_suggestionList->currentItem()
        ? m_suggestionList->currentItem()->data(Qt::UserRole).toString()
        : QString();
    const QString selectedReceiptId = m_receiptList->currentItem()
        ? m_receiptList->currentItem()->data(Qt::UserRole).toString() : QString();

    m_runtimeLabel->setText(tr("Runtime: %1 | Inbox: %2")
        .arg(workflowDisplayText(m_store->runtimeModeText()), m_store->inboxDir()));
    QStringList capabilities;
    for (const auto& capability : m_store->capabilities()) capabilities << workflowDisplayText(capability);
    m_capabilityLabel->setText(tr("Capabilities: %1").arg(capabilities.join(QStringLiteral(", "))));
    m_capabilityLabel->setToolTip(m_store->capabilities().join(QStringLiteral(", ")));

    m_historyList->clear();
    for (const ContextItem& item : m_store->history()) {
        auto* listItem = new QListWidgetItem(
            QStringLiteral("[%1] %2").arg(workflowDisplayText(contextKindText(item.kind)), item.summary),
            m_historyList);
        listItem->setData(Qt::UserRole, item.id);
        if (item.sensitivity == SensitivityLevel::Sensitive) {
            listItem->setToolTip(tr("Sensitive content. Confirmation is required for risky actions."));
        }
        if (item.id == selectedHistoryId) {
            m_historyList->setCurrentItem(listItem);
        }
    }

    m_suggestionList->clear();
    for (const SuggestionCard& card : m_store->suggestions()) {
        auto* listItem = new QListWidgetItem(
            QStringLiteral("%1: %2").arg(m_actionBus->labelForAction(card.actionId), card.previewText),
            m_suggestionList);
        listItem->setData(Qt::UserRole, card.id);
        if (card.requiresConfirmation) {
            listItem->setToolTip(tr("Requires confirmation."));
        }
        if (card.id == selectedSuggestionId) {
            m_suggestionList->setCurrentItem(listItem);
        }
    }

    m_receiptList->clear();
    for (const TransferReceipt& receipt : m_store->receipts()) {
        auto* listItem = new QListWidgetItem(
            QStringLiteral("[%1] %2").arg(workflowDisplayText(receipt.status), receipt.detail),
            m_receiptList);
        listItem->setData(Qt::UserRole, receipt.id);
        if (receipt.id == selectedReceiptId) m_receiptList->setCurrentItem(listItem);
    }

    updateDetails();
}

void WorkflowHubDialog::updateDetails()
{
    QString contextId = selectedContextId();
    QString previewPath;
    QString html;

    if (!contextId.isEmpty()) {
        const ContextItem* item = m_store->contextById(contextId);
        if (item != nullptr) {
            previewPath = item->previewRef;
            html = tr(
                "<h3>%1</h3>"
                "<p><b>Type:</b> %2<br>"
                "<b>Source:</b> %3<br>"
                "<b>Size:</b> %4 bytes<br>"
                "<b>Sensitivity:</b> %5<br>"
                "<b>Created:</b> %6<br>"
                "<b>Actions:</b> %7</p>")
                .arg(htmlEscape(item->summary),
                     workflowDisplayText(contextKindText(item->kind)),
                     htmlEscape(item->sourceDevice),
                     QString::number(item->byteSize),
                     workflowDisplayText(sensitivityText(item->sensitivity)),
                     formatDateTime(item->createdAt),
                     htmlEscape(item->suggestedActions.join(QStringLiteral(", "))));
        }
    }

    if (html.isEmpty() && m_tabs->currentWidget() == m_receiptList && m_receiptList->currentItem()) {
        const QString receiptId = m_receiptList->currentItem()->data(Qt::UserRole).toString();
        for (const TransferReceipt& receipt : m_store->receipts()) {
            if (receipt.id == receiptId) {
                html = tr(
                    "<h3>%1</h3><p><b>Action:</b> %2<br><b>Detail:</b> %3<br><b>Time:</b> %4</p>")
                    .arg(htmlEscape(workflowDisplayText(receipt.status)),
                         htmlEscape(receipt.actionId),
                         htmlEscape(receipt.detail),
                         formatDateTime(receipt.createdAt));
                break;
            }
        }
    }

    if (html.isEmpty()) {
        html = tr("<p>Select a history item, suggestion, or receipt.</p>");
    }

    setDetailHtml(html, previewPath);

    const bool hasContext = !contextId.isEmpty();
    m_openButton->setEnabled(hasContext);
    m_saveButton->setEnabled(hasContext);
    m_revealButton->setEnabled(hasContext);
    m_executeSuggestionButton->setEnabled(m_tabs->currentWidget() == m_suggestionList &&
                                          m_suggestionList->currentItem() != nullptr);
}

void WorkflowHubDialog::executeSuggestion()
{
    const QString actionId = selectedSuggestionActionId();
    const QString contextId = selectedContextId();
    if (!actionId.isEmpty() && !contextId.isEmpty()) {
        executeAction(actionId, contextId);
    }
}

void WorkflowHubDialog::openSelected()
{
    const QString contextId = selectedContextId();
    if (contextId.isEmpty()) {
        return;
    }

    const ContextItem* item = m_store->contextById(contextId);
    const QString actionId = item && item->kind == ContextKind::Link
        ? QStringLiteral("open_link")
        : QStringLiteral("open_file");
    executeAction(actionId, contextId);
}

void WorkflowHubDialog::saveSelected()
{
    const QString contextId = selectedContextId();
    if (!contextId.isEmpty()) {
        executeAction(QStringLiteral("save_to_inbox"), contextId);
    }
}

void WorkflowHubDialog::revealSelected()
{
    const QString contextId = selectedContextId();
    if (!contextId.isEmpty()) {
        executeAction(QStringLiteral("reveal_in_folder"), contextId);
    }
}

void WorkflowHubDialog::captureScreenshot()
{
    executeAction(QStringLiteral("capture_screenshot"), QString());
}

void WorkflowHubDialog::openInbox()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_store->inboxDir()));
}

QString WorkflowHubDialog::selectedContextId() const
{
    if (m_tabs->currentWidget() == m_historyList && m_historyList->currentItem()) {
        return m_historyList->currentItem()->data(Qt::UserRole).toString();
    }

    if (m_tabs->currentWidget() == m_suggestionList && m_suggestionList->currentItem()) {
        const QString suggestionId = m_suggestionList->currentItem()->data(Qt::UserRole).toString();
        const SuggestionCard* card = m_store->suggestionById(suggestionId);
        return card ? card->contextId : QString();
    }

    if (m_tabs->currentWidget() == m_receiptList && m_receiptList->currentItem()) {
        const QString receiptId = m_receiptList->currentItem()->data(Qt::UserRole).toString();
        for (const TransferReceipt& receipt : m_store->receipts()) {
            if (receipt.id == receiptId) {
                return receipt.contextId;
            }
        }
    }

    return QString();
}

QString WorkflowHubDialog::selectedSuggestionActionId() const
{
    if (m_tabs->currentWidget() != m_suggestionList || m_suggestionList->currentItem() == nullptr) {
        return QString();
    }

    const QString suggestionId = m_suggestionList->currentItem()->data(Qt::UserRole).toString();
    const SuggestionCard* card = m_store->suggestionById(suggestionId);
    return card ? card->actionId : QString();
}

void WorkflowHubDialog::setDetailHtml(const QString& html, const QString& previewPath)
{
    m_detail->setHtml(html);
    if (!previewPath.isEmpty() && QFileInfo::exists(previewPath)) {
        QPixmap preview(previewPath);
        m_previewLabel->setPixmap(preview.scaled(
            m_previewLabel->width(),
            qMax(96, m_previewLabel->height()),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation));
    }
    else {
        m_previewLabel->setPixmap(QPixmap());
        m_previewLabel->setText(tr("No preview"));
    }
}

bool WorkflowHubDialog::executeAction(const QString& actionId, const QString& contextId)
{
    QString errorMessage;
    const bool ok = m_actionBus->execute(actionId, contextId, this, &errorMessage);
    if (!ok && !errorMessage.isEmpty()) {
        QMessageBox::warning(this, tr("Workflow Action Failed"), errorMessage);
    }
    refreshAll();
    return ok;
}
