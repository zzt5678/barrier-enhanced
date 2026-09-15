#pragma once
#include <QWidget>
#include <QPoint>
#include <QVector>
class QLabel;
class QPushButton;
class QStackedWidget;
class QPixmap;
enum class WorkspaceDeviceRole { Unknown, Server, Client };
enum class WorkspaceDeviceState { Configured, AwaitingSync, Connected, Disconnected, Stale };
struct WorkspaceDevice {
    QString name;
    QPoint position;
    bool local = false;
    WorkspaceDeviceRole role = WorkspaceDeviceRole::Unknown;
    WorkspaceDeviceState state = WorkspaceDeviceState::Configured;
    QString platform = "unknown";
};
class WorkspaceDashboard : public QWidget {
    Q_OBJECT
public:
    explicit WorkspaceDashboard(QWidget* settings, QWidget* parent = nullptr);
    void setDevices(const QVector<WorkspaceDevice>& devices, bool spatial = true,
                    const QString& layoutReason = QString());
    void setConnectionState(const QString& status, bool running, bool canArrange);
    void setClipboardSharingState(const QString& state);
    void retranslate();
protected:
    void resizeEvent(QResizeEvent* event) override;
signals:
    void arrangeRequested();
    void sharingRequested();
    void transfersRequested();
    void preferencesRequested();
private:
    int illustrationSide() const;
    QPixmap illustration(const QString& platform, int side) const;
    void updateSharingLabels();
    QWidget* m_canvas = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_sharingTitle = nullptr;
    QLabel* m_keyboardState = nullptr;
    QLabel* m_clipboardStateLabel = nullptr;
    QLabel* m_layoutNotice = nullptr;
    QLabel* m_layoutDescription = nullptr;
    QPushButton* m_devices = nullptr;
    QPushButton* m_transfers = nullptr;
    QPushButton* m_settings = nullptr;
    QPushButton* m_arrange = nullptr;
    QPushButton* m_sharing = nullptr;
    QPushButton* m_sharingSettings = nullptr;
    QPushButton* m_preferences = nullptr;
    QStackedWidget* m_pages = nullptr;
    QString m_clipboardState;
    bool m_running = false;
    bool m_canArrange = true;
    QVector<WorkspaceDevice> m_deviceData;
    bool m_spatial = true;
    QString m_layoutReason;
    QStringList m_deviceSignature;
    int m_columns = 1;
};
