#include "WorkspaceDashboard.h"
#include "ElidingLabel.h"
#include <QApplication>
#include <QButtonGroup>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
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
    auto* titleRow = new QHBoxLayout;
    m_title = new QLabel; m_title->setObjectName("workspaceTitle"); titleRow->addWidget(m_title); titleRow->addStretch();
    m_arrange = new QPushButton; m_arrange->setObjectName("arrangeButton"); m_arrange->setProperty("quiet",true);
    titleRow->addWidget(m_arrange); content->addLayout(titleRow);
    m_status = new QLabel; m_status->setTextFormat(Qt::PlainText); m_status->setObjectName("workspaceStatus"); m_status->setWordWrap(true); content->addWidget(m_status);
    m_layoutDescription = new QLabel; m_layoutDescription->setObjectName("deviceLayoutDescription");
    m_layoutDescription->setWordWrap(true); content->addWidget(m_layoutDescription); m_layoutDescription->hide();
    m_canvas = new QWidget; m_canvas->setObjectName("deviceCanvas"); auto* grid = new QGridLayout(m_canvas); grid->setContentsMargins(0,12,0,12); grid->setSpacing(12); content->addWidget(m_canvas);
    m_layoutNotice = new QLabel; m_layoutNotice->setObjectName("layoutReadOnlyNotice");
    m_layoutNotice->setWordWrap(true); m_layoutNotice->setAlignment(Qt::AlignCenter);
    content->addWidget(m_layoutNotice); m_layoutNotice->hide();
    auto* separator = new QFrame; separator->setFrameShape(QFrame::HLine); separator->setObjectName("workspaceSeparator"); content->addSpacing(10); content->addWidget(separator);
    auto* controls = new QWidget; controls->setObjectName("sharingControl");
    auto* sharingLayout = new QVBoxLayout(controls); sharingLayout->setContentsMargins(0,0,0,0); sharingLayout->setSpacing(10);
    m_sharingTitle = new QLabel; m_sharingTitle->setObjectName("workspaceSection"); sharingLayout->addWidget(m_sharingTitle);
    m_keyboardState = new QLabel; m_keyboardState->setObjectName("keyboardSharingState"); m_keyboardState->setWordWrap(true); sharingLayout->addWidget(m_keyboardState);
    m_clipboardStateLabel = new QLabel; m_clipboardStateLabel->setObjectName("clipboardSharingState"); m_clipboardStateLabel->setTextFormat(Qt::PlainText); m_clipboardStateLabel->setWordWrap(true); sharingLayout->addWidget(m_clipboardStateLabel);
    auto* sharingActions = new QHBoxLayout;
    m_sharing = new QPushButton; m_sharing->setObjectName("sharingButton"); m_sharing->setProperty("primary",true);
    m_sharingSettings = new QPushButton; m_sharingSettings->setObjectName("sharingSettingsButton");
    sharingActions->addWidget(m_sharing); sharingActions->addWidget(m_sharingSettings); sharingActions->addStretch();
    sharingLayout->addLayout(sharingActions); content->addWidget(controls); content->addStretch();
    auto* scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidget(home); m_pages->addWidget(scroll);
    auto* configuration = new QWidget; auto* configLayout = new QVBoxLayout(configuration); configLayout->setContentsMargins(12,12,12,12);
    m_preferences = new QPushButton; m_preferences->setProperty("primary",true); configLayout->addWidget(m_preferences,0,Qt::AlignRight); configLayout->addWidget(settings); m_pages->addWidget(configuration);
    connect(m_devices,&QPushButton::clicked,this,[this]{m_pages->setCurrentIndex(0);});
    connect(m_settings,&QPushButton::clicked,this,[this]{m_pages->setCurrentIndex(1);});
    connect(m_transfers,&QPushButton::clicked,this,[this]{emit transfersRequested(); (m_pages->currentIndex()==0 ? m_devices : m_settings)->setChecked(true);});
    connect(m_arrange,&QPushButton::clicked,this,&WorkspaceDashboard::arrangeRequested);
    connect(m_sharing,&QPushButton::clicked,this,&WorkspaceDashboard::sharingRequested);
    connect(m_sharingSettings,&QPushButton::clicked,this,&WorkspaceDashboard::preferencesRequested);
    connect(m_preferences,&QPushButton::clicked,this,&WorkspaceDashboard::preferencesRequested);
    retranslate(); setDevices({});
}
void WorkspaceDashboard::setDevices(const QVector<WorkspaceDevice>& devices, bool spatial, const QString& layoutReason)
{
    m_deviceData = devices;
    m_spatial = spatial; m_layoutReason = layoutReason;
    m_layoutDescription->setText(spatial ? QString() : tr("Device list · %1").arg(layoutReason));
    m_layoutDescription->setVisible(!spatial);
    QStringList signature{tr("This computer"), tr("Configured"), tr("Connected"), tr("Awaiting sync"), QString::number(spatial), layoutReason};
    for (const auto& device : devices) signature.append(QStringLiteral("%1\n%2,%3,%4,%5,%6,%7").arg(device.name).arg(device.position.x()).arg(device.position.y()).arg(device.local).arg(static_cast<int>(device.role)).arg(static_cast<int>(device.state)).arg(device.platform));
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
        auto* card = new QFrame; card->setObjectName("deviceCard"); card->setProperty("local",device.local); card->setMinimumWidth(48);
        auto* layout = new QBoxLayout(spatial ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight, card);
        layout->setContentsMargins(4,10,4,10); layout->setSpacing(8); layout->setAlignment(Qt::AlignCenter);
        const QString platform = QStringList{"ubuntu","windows","macos"}.contains(device.platform) ? device.platform : QStringLiteral("unknown");
        auto* monitor = new QLabel; monitor->setObjectName("deviceIllustration"); monitor->setProperty("platform",platform); const int side=illustrationSide();
        monitor->setPixmap(illustration(platform, side));
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
        auto* detailRow = new QWidget; detailRow->setObjectName("deviceDetailRow");
        auto* detailLayout = new QHBoxLayout(detailRow); detailLayout->setContentsMargins(0,0,0,0); detailLayout->setSpacing(3); detailLayout->addStretch();
        auto* roleLabel = new QLabel(role); roleLabel->setObjectName("deviceRole");
        roleLabel->setAlignment(Qt::AlignCenter); detailLayout->addWidget(roleLabel);
        auto* dot = new QLabel(QString::fromUtf8("·")); dot->setObjectName("deviceStatusSeparator"); detailLayout->addWidget(dot);
        auto* detail = new QLabel(status); detail->setObjectName("deviceState");
        detail->setAlignment(Qt::AlignCenter); detail->setWordWrap(true); detailLayout->addWidget(detail); detailLayout->addStretch(); layout->addWidget(detailRow);
        const QString description = role + (device.local ? " · " + tr("This computer") : QString()) + " · " + status;
        name->setAccessibleDescription(description); card->setAccessibleName(device.name); card->setAccessibleDescription(description);
        const QString system = platform == "ubuntu" ? QStringLiteral("Ubuntu")
            : platform == "windows" ? QStringLiteral("Windows")
            : platform == "macos" ? QStringLiteral("macOS") : tr("Operating system unknown");
        card->setToolTip(Qt::convertFromPlainText(device.name + "\n" + system + "\n" + description));
        grid->addWidget(card,spatial ? rows.indexOf(device.position.y()) : listRow++,spatial ? columns.indexOf(device.position.x()) : 0); accessible.append(device.name+": "+description);
    }
    if(devices.isEmpty()) { auto* empty=new QLabel(tr("Your configured computers will appear here.")); empty->setWordWrap(true); grid->addWidget(empty,0,0); }
    m_canvas->setAccessibleDescription(accessible.join("\n"));
}
int WorkspaceDashboard::illustrationSide() const
{
    const int sidebarWidth = width() < 760 ? 160 : 180;
    const int margin = width() < 760 ? 16 : 28;
    return qBound(64, (width()-sidebarWidth-2*margin-32)/m_columns-32, 160);
}
QPixmap WorkspaceDashboard::illustration(const QString& platform, int side) const
{
    const int physicalSide = qRound(side * devicePixelRatioF());
    QPixmap picture(":/res/image/device-display.png");
    if(picture.isNull()) picture=style()->standardPixmap(QStyle::SP_ComputerIcon);
    picture = picture.scaled(physicalSide,physicalSide,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    if (QStringList{"ubuntu","windows","macos"}.contains(platform)) {
        QPainter painter(&picture);
        const int iconSide = qRound(physicalSide * 0.26);
        const QRect screenIcon(qRound(physicalSide * 0.5) - iconSide / 2,
                               qRound(physicalSide * 0.42) - iconSide / 2, iconSide, iconSide);
        QIcon(":/res/icons/platform/" + platform + ".svg").paint(&painter, screenIcon);
    }
    picture.setDevicePixelRatio(devicePixelRatioF());
    return picture;
}
void WorkspaceDashboard::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    const int width = event->size().width();
    auto* sidebar = findChild<QFrame*>("workspaceSidebar");
    const int sidebarWidth = width < 760 ? 160 : 180;
    sidebar->setFixedWidth(sidebarWidth);
    auto* home = findChild<QWidget*>("workspaceHome");
    const int margin = width < 760 ? 16 : 28;
    home->layout()->setContentsMargins(margin, 28, margin, 24);
    m_arrange->setMinimumWidth(0);
    m_sharing->setMinimumWidth(width < 760 ? 0 : 150);
    // Reserve the scroll bar and cell gaps before choosing an illustration size.
    const int side=illustrationSide();
    for(auto* label:m_canvas->findChildren<QLabel*>("deviceIllustration")) {
        label->setPixmap(illustration(label->property("platform").toString(), side)); label->setFixedSize(side,side);
    }
}
void WorkspaceDashboard::setConnectionState(const QString& status, bool running, bool canArrange)
{ m_status->setText(status); m_running=running; m_canArrange=canArrange; m_arrange->setEnabled(canArrange); m_arrange->setVisible(canArrange); m_layoutNotice->setVisible(!canArrange); m_arrange->setToolTip(tr("Arrange your configured computers.")); updateSharingLabels(); }
void WorkspaceDashboard::setClipboardSharingState(const QString& state)
{
    m_clipboardState = state;
    updateSharingLabels();
}
void WorkspaceDashboard::updateSharingLabels()
{
    m_sharing->setText(m_running ? tr("Pause sharing") : tr("Start sharing"));
    m_keyboardState->setText(m_running ? tr("Keyboard and mouse: sharing enabled") : tr("Keyboard and mouse: sharing paused"));
    m_clipboardStateLabel->setText(tr("Clipboard: %1").arg(m_clipboardState.isEmpty() ? tr("State unknown") : m_clipboardState));
}
void WorkspaceDashboard::retranslate()
{
    m_title->setText(tr("My devices")); m_devices->setText(tr("Devices")); m_transfers->setText(tr("Local history")); m_settings->setText(tr("Settings")); m_arrange->setText(tr("Arrange layout")); m_preferences->setText(tr("Application preferences"));
    m_sharingTitle->setText(tr("Sharing control")); m_sharingSettings->setText(tr("Sharing settings")); updateSharingLabels();
    m_layoutNotice->setText(tr("Read-only layout · managed by Server"));
    m_deviceSignature.clear();
    setDevices(m_deviceData, m_spatial, m_layoutReason);
}
