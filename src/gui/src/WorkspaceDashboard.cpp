#include "WorkspaceDashboard.h"
#include "ElidingLabel.h"
#include "WorkflowDisplayText.h"
#include <QApplication>
#include <QButtonGroup>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QResizeEvent>
#include <QTextDocument>
#include <QVBoxLayout>
#include <algorithm>

WorkspaceDashboard::WorkspaceDashboard(QWidget* settings, QWidget* parent): QWidget(parent)
{
    setObjectName("workspaceDashboard");
    auto* shell = new QHBoxLayout(this); shell->setContentsMargins(0,0,0,0); shell->setSpacing(0);
    auto* sidebar = new QFrame; sidebar->setObjectName("workspaceSidebar"); sidebar->setFixedWidth(180);
    auto* nav = new QVBoxLayout(sidebar); nav->setContentsMargins(12,28,12,20); nav->setSpacing(8);
    auto* brandRow = new QHBoxLayout;
    auto* brand = new QLabel;
    QPixmap brandImage = QPixmap(":/res/icons/256x256/weave.png").scaled(qRound(32 * devicePixelRatioF()),qRound(32 * devicePixelRatioF()),Qt::KeepAspectRatio,Qt::SmoothTransformation);
    brandImage.setDevicePixelRatio(devicePixelRatioF()); brand->setPixmap(brandImage); brandRow->addWidget(brand);
    auto* name = new QLabel("Weave"); name->setObjectName("workspaceBrand"); brandRow->addWidget(name); brandRow->addStretch(); nav->addLayout(brandRow); nav->addSpacing(36);
    m_devices = new QPushButton; m_transfers = new QPushButton; m_settings = new QPushButton;
    auto* group = new QButtonGroup(this); group->setExclusive(true);
    const QList<QPushButton*> buttons{m_devices,m_transfers,m_settings};
    const QStringList icons{"devices", "transfers", "settings"};
    for(int i=0;i<buttons.size();++i) { auto* button=buttons[i]; button->setProperty("navigation",true); button->setCheckable(true); button->setIcon(QIcon(":/res/icons/workspace/"+icons[i]+".png")); button->setIconSize({22,22}); button->setMinimumHeight(48); group->addButton(button); nav->addWidget(button); }
    m_devices->setChecked(true); nav->addStretch(); shell->addWidget(sidebar);
    m_pages = new QStackedWidget; shell->addWidget(m_pages,1);
    auto* home = new QWidget; home->setObjectName("workspaceHome"); auto* content = new QVBoxLayout(home); content->setContentsMargins(34,30,34,24); content->setSpacing(16);
    m_title = new QLabel; m_title->setObjectName("workspaceTitle"); content->addWidget(m_title);
    m_status = new QLabel; m_status->setTextFormat(Qt::PlainText); m_status->setObjectName("workspaceStatus"); m_status->setWordWrap(true); content->addWidget(m_status);
    m_layoutDescription = new QLabel; m_layoutDescription->setObjectName("deviceLayoutDescription");
    m_layoutDescription->setWordWrap(true); content->addWidget(m_layoutDescription); m_layoutDescription->hide();
    m_canvas = new QWidget; m_canvas->setObjectName("deviceCanvas"); auto* grid = new QGridLayout(m_canvas); grid->setContentsMargins(0,20,0,16); grid->setSpacing(18); content->addWidget(m_canvas);
    auto* actions = new QHBoxLayout; actions->setSpacing(12);
    m_arrange = new QPushButton; m_arrange->setObjectName("arrangeButton"); m_arrange->setProperty("primary",true);
    m_sharing = new QPushButton; m_sharing->setObjectName("sharingButton");
    actions->addStretch(); actions->addWidget(m_arrange); actions->addWidget(m_sharing); actions->addStretch(); content->addLayout(actions);
    m_layoutNotice = new QLabel; m_layoutNotice->setObjectName("layoutReadOnlyNotice");
    m_layoutNotice->setWordWrap(true); m_layoutNotice->setAlignment(Qt::AlignCenter);
    content->addWidget(m_layoutNotice); m_layoutNotice->hide();
    auto* separator = new QFrame; separator->setFrameShape(QFrame::HLine); separator->setObjectName("workspaceSeparator"); content->addSpacing(10); content->addWidget(separator);
    auto* recentHeader = new QHBoxLayout; m_recentTitle = new QLabel; m_recentTitle->setObjectName("workspaceSection"); recentHeader->addWidget(m_recentTitle); recentHeader->addStretch(); m_inbox = new QPushButton; m_inbox->setProperty("quiet",true); m_inbox->setIcon(QIcon(":/res/icons/workspace/folder.png")); recentHeader->addWidget(m_inbox); content->addLayout(recentHeader);
    m_empty = new QLabel; m_empty->setObjectName("emptyTransfers"); m_empty->setWordWrap(true); content->addWidget(m_empty);
    m_recent = new QListWidget; m_recent->setObjectName("recentTransfers"); m_recent->setMinimumHeight(120); m_recent->setMaximumHeight(200); m_recent->setFrameShape(QFrame::NoFrame); content->addWidget(m_recent); m_recent->hide(); content->addStretch();
    m_recent->setWordWrap(true); m_recent->setTextElideMode(Qt::ElideNone);
    m_recent->setItemDelegate(new WorkflowReceiptDelegate(m_recent));
    m_recent->setResizeMode(QListView::Adjust); m_recent->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_recent->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    auto* scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidget(home); m_pages->addWidget(scroll);
    auto* configuration = new QWidget; auto* configLayout = new QVBoxLayout(configuration); configLayout->setContentsMargins(12,12,12,12);
    m_preferences = new QPushButton; m_preferences->setProperty("primary",true); configLayout->addWidget(m_preferences,0,Qt::AlignRight); configLayout->addWidget(settings); m_pages->addWidget(configuration);
    connect(m_devices,&QPushButton::clicked,this,[this]{m_pages->setCurrentIndex(0);});
    connect(m_settings,&QPushButton::clicked,this,[this]{m_pages->setCurrentIndex(1);});
    connect(m_transfers,&QPushButton::clicked,this,[this]{emit transfersRequested(); (m_pages->currentIndex()==0 ? m_devices : m_settings)->setChecked(true);});
    connect(m_arrange,&QPushButton::clicked,this,&WorkspaceDashboard::arrangeRequested);
    connect(m_sharing,&QPushButton::clicked,this,&WorkspaceDashboard::sharingRequested);
    connect(m_inbox,&QPushButton::clicked,this,&WorkspaceDashboard::inboxRequested);
    connect(m_preferences,&QPushButton::clicked,this,&WorkspaceDashboard::preferencesRequested);
    connect(m_recent,&QListWidget::itemActivated,this,[this]{emit transfersRequested();});
    retranslate(); setDevices({});
}
void WorkspaceDashboard::setDevices(const QVector<WorkspaceDevice>& devices, bool spatial, const QString& layoutReason)
{
    m_deviceData = devices;
    m_spatial = spatial; m_layoutReason = layoutReason;
    m_layoutDescription->setText(spatial ? QString() : tr("Device list · %1").arg(layoutReason));
    m_layoutDescription->setVisible(!spatial);
    QStringList signature{tr("This computer"), tr("Configured"), tr("Connected"), tr("Awaiting sync"), QString::number(spatial), layoutReason};
    for (const auto& device : devices) signature.append(QStringLiteral("%1\n%2,%3,%4,%5,%6").arg(device.name).arg(device.position.x()).arg(device.position.y()).arg(device.local).arg(static_cast<int>(device.role)).arg(static_cast<int>(device.state)));
    if (signature == m_deviceSignature) return;
    m_deviceSignature = signature;
    auto* grid = qobject_cast<QGridLayout*>(m_canvas->layout());
    while(auto* item=grid->takeAt(0)) { delete item->widget(); delete item; }
    for (int column = 0; column < grid->columnCount(); ++column) grid->setColumnStretch(column, 0);
    QStringList accessible; QList<int> columns,rows;
    for(const auto& device:devices) { if(!columns.contains(device.position.x())) columns.append(device.position.x()); if(!rows.contains(device.position.y())) rows.append(device.position.y()); }
    std::sort(columns.begin(),columns.end()); std::sort(rows.begin(),rows.end());
    m_columns = spatial ? qMax(1, columns.size()) : 1;
    for (int column = 0; column < m_columns; ++column) grid->setColumnStretch(column, 1);
    int listRow = 0;
    for(const auto& device:devices) {
        auto* card = new QWidget; card->setMinimumWidth(64);
        auto* layout = new QBoxLayout(spatial ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight, card);
        layout->setContentsMargins(0,0,0,0); layout->setSpacing(8); layout->setAlignment(Qt::AlignCenter);
        auto* monitor = new QLabel; monitor->setObjectName("deviceIllustration"); QPixmap picture(":/res/image/device-display.png"); if(picture.isNull()) picture=style()->standardPixmap(QStyle::SP_ComputerIcon); const int side=illustrationSide();
        picture = picture.scaled(qRound(side * devicePixelRatioF()),qRound(side * devicePixelRatioF()),Qt::KeepAspectRatio,Qt::SmoothTransformation);
        picture.setDevicePixelRatio(devicePixelRatioF()); monitor->setPixmap(picture);
        monitor->setFixedSize(side,side); monitor->setAlignment(Qt::AlignCenter); layout->addWidget(monitor,0,Qt::AlignHCenter);
        monitor->setVisible(spatial);
        auto* name = new ElidingLabel; name->setObjectName("deviceName"); name->setAlignment(Qt::AlignCenter); name->setTextFormat(Qt::PlainText); name->setText(device.name); layout->addWidget(name);
        const QString role = device.role == WorkspaceDeviceRole::Server ? QStringLiteral("Server")
            : device.role == WorkspaceDeviceRole::Client ? QStringLiteral("Client") : tr("Role unknown");
        QString status;
        switch (device.state) {
        case WorkspaceDeviceState::Connected: status = tr("Connected"); break;
        case WorkspaceDeviceState::Disconnected: status = tr("Disconnected"); break;
        case WorkspaceDeviceState::Stale: status = tr("Out of date"); break;
        case WorkspaceDeviceState::AwaitingSync: status = tr("Awaiting sync"); break;
        default: status = tr("Configured"); break;
        }
        auto* roleLabel = new QLabel(role); roleLabel->setObjectName("deviceRole");
        roleLabel->setAlignment(Qt::AlignCenter); layout->addWidget(roleLabel);
        auto* local = new QLabel(device.local ? tr("This computer") : QString());
        local->setObjectName("deviceLocal"); local->setAlignment(Qt::AlignCenter);
        local->setMinimumHeight(local->fontMetrics().height() + 6); layout->addWidget(local);
        auto* detail = new QLabel(status); detail->setObjectName("deviceState");
        detail->setAlignment(Qt::AlignCenter); detail->setWordWrap(true); layout->addWidget(detail);
        const QString description = role + (device.local ? " · " + tr("This computer") : QString()) + " · " + status;
        name->setAccessibleDescription(description); card->setAccessibleName(device.name); card->setAccessibleDescription(description);
        grid->addWidget(card,spatial ? rows.indexOf(device.position.y()) : listRow++,spatial ? columns.indexOf(device.position.x()) : 0); accessible.append(device.name+": "+description);
    }
    if(devices.isEmpty()) { auto* empty=new QLabel(tr("Your configured computers will appear here.")); empty->setWordWrap(true); grid->addWidget(empty,0,0); }
    m_canvas->setAccessibleDescription(accessible.join("\n"));
}
int WorkspaceDashboard::illustrationSide() const
{
    const int sidebarWidth = width() < 760 ? 136 : 180;
    const int margin = width() < 760 ? 16 : 28;
    return qBound(64, (width()-sidebarWidth-2*margin-32)/m_columns-32, 160);
}
void WorkspaceDashboard::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    const int width = event->size().width();
    auto* sidebar = findChild<QFrame*>("workspaceSidebar");
    const int sidebarWidth = width < 760 ? 136 : 180;
    sidebar->setFixedWidth(sidebarWidth);
    auto* home = findChild<QWidget*>("workspaceHome");
    const int margin = width < 760 ? 16 : 28;
    home->layout()->setContentsMargins(margin, 28, margin, 24);
    m_arrange->setMinimumWidth(width < 760 ? 0 : 150);
    m_sharing->setMinimumWidth(width < 760 ? 0 : 150);
    // Reserve the scroll bar and cell gaps before choosing an illustration size.
    const int side=illustrationSide();
    QPixmap picture(":/res/image/device-display.png");
    if(picture.isNull()) picture=style()->standardPixmap(QStyle::SP_ComputerIcon);
    picture = picture.scaled(qRound(side * devicePixelRatioF()),qRound(side * devicePixelRatioF()),Qt::KeepAspectRatio,Qt::SmoothTransformation);
    picture.setDevicePixelRatio(devicePixelRatioF());
    for(auto* label:m_canvas->findChildren<QLabel*>("deviceIllustration")) {
        label->setPixmap(picture); label->setFixedSize(side,side);
    }
}
void WorkspaceDashboard::setConnectionState(const QString& status, bool running, bool canArrange)
{ m_status->setText(status); m_running=running; m_canArrange=canArrange; m_arrange->setEnabled(canArrange); m_arrange->setVisible(canArrange); m_layoutNotice->setVisible(!canArrange); m_arrange->setToolTip(tr("Arrange your configured computers.")); m_sharing->setText(running?tr("Pause sharing"):tr("Start sharing")); }
void WorkspaceDashboard::setRecentTransfers(const QStringList& transfers)
{
    QStringList current;
    for (int i = 0; i < m_recent->count(); ++i) current.append(m_recent->item(i)->text());
    if (current != transfers) {
        const int selected = m_recent->currentRow();
        m_recent->clear(); m_recent->addItems(transfers);
        for (int i = 0; i < m_recent->count(); ++i) {
            m_recent->item(i)->setToolTip(Qt::convertFromPlainText(transfers[i]));
            m_recent->item(i)->setData(Qt::AccessibleDescriptionRole, transfers[i]);
        }
        if (selected >= 0) m_recent->setCurrentRow(qMin(selected, m_recent->count() - 1));
    }
    m_recent->setVisible(!transfers.isEmpty()); m_empty->setVisible(transfers.isEmpty());
}
void WorkspaceDashboard::retranslate()
{
    m_recent->setToolTip(tr("Double-click a record or press Enter to open local history."));
    m_title->setText(tr("My devices")); m_devices->setText(tr("Devices")); m_transfers->setText(tr("Local history")); m_settings->setText(tr("Settings")); m_arrange->setText(tr("Arrange layout")); m_sharing->setText(m_running?tr("Pause sharing"):tr("Start sharing")); m_recentTitle->setText(tr("Recent local activity")); m_inbox->setText(tr("Open inbox")); m_preferences->setText(tr("Application preferences")); m_empty->setText(tr("No local activity yet. Open history to review clipboard items and local action records."));
    m_layoutNotice->setText(tr("Read-only layout · managed by Server"));
    m_deviceSignature.clear();
    setDevices(m_deviceData, m_spatial, m_layoutReason);
}
