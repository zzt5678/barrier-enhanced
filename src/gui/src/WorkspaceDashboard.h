#pragma once
#include <QWidget>
#include <QPoint>
#include <QVector>
class QLabel;
class QPushButton;
class QStackedWidget;
class QListWidget;
struct WorkspaceDevice { QString name; QPoint position; bool local = false; };
class WorkspaceDashboard : public QWidget {
    Q_OBJECT
public:
    explicit WorkspaceDashboard(QWidget* settings, QWidget* parent = nullptr);
    void setDevices(const QVector<WorkspaceDevice>& devices);
    void setConnectionState(const QString& status, bool running, bool canArrange);
    void setRecentTransfers(const QStringList& transfers);
    void retranslate();
protected:
    void resizeEvent(QResizeEvent* event) override;
signals:
    void arrangeRequested();
    void sharingRequested();
    void transfersRequested();
    void inboxRequested();
    void preferencesRequested();
private:
    int illustrationSide() const;
    QWidget* m_canvas = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_recentTitle = nullptr;
    QLabel* m_empty = nullptr;
    QPushButton* m_devices = nullptr;
    QPushButton* m_transfers = nullptr;
    QPushButton* m_settings = nullptr;
    QPushButton* m_arrange = nullptr;
    QPushButton* m_sharing = nullptr;
    QPushButton* m_inbox = nullptr;
    QPushButton* m_preferences = nullptr;
    QStackedWidget* m_pages = nullptr;
    QListWidget* m_recent = nullptr;
    bool m_running = false;
    QStringList m_deviceSignature;
    int m_columns = 1;
};
