#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QLabel>
#include <QEvent>
#include <QTimer>
#include <QPushButton>
#include <QGroupBox>

namespace DialogPresentation {
inline void reflowPanels(QDialog* dialog)
{
    for (auto* row : dialog->findChildren<QHBoxLayout*>()) {
        bool panelsOnly = row->count() > 1;
        int required = 0;
        for (int i = 0; i < row->count(); ++i) {
            auto* panel = qobject_cast<QGroupBox*>(row->itemAt(i)->widget());
            if (!panel) { panelsOnly = false; break; }
            required += panel->minimumSizeHint().width() + qMax(0, row->spacing());
        }
        if (panelsOnly) row->setDirection(required > dialog->width() - 72
            ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    }
}

inline void updateButtons(QDialog* dialog)
{
    for (auto* box : dialog->findChildren<QDialogButtonBox*>()) {
        for (auto* button : box->buttons()) button->setIcon(QIcon());
        if (box->button(QDialogButtonBox::Ok))
            box->button(QDialogButtonBox::Ok)->setText(QCoreApplication::translate("DialogButtons", "OK"));
        if (box->button(QDialogButtonBox::Cancel))
            box->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("DialogButtons", "Cancel"));
    }
}

class InitialPlacement : public QObject {
public:
    explicit InitialPlacement(QDialog* dialog) : QObject(dialog), m_dialog(dialog) { dialog->installEventFilter(this); }
protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Resize) reflowPanels(m_dialog);
        if (event->type() == QEvent::LanguageChange)
            QTimer::singleShot(0, m_dialog, [this] { updateButtons(m_dialog); reflowPanels(m_dialog); });
        if (event->type() == QEvent::Show && !m_shown) {
            m_shown = true;
            QTimer::singleShot(0, m_dialog, [this] {
                const QRect available = m_dialog->screen()->availableGeometry();
                const QRect frame = m_dialog->frameGeometry();
                const QPoint target(qBound(available.left(), frame.left(), qMax(available.left(), available.right() - frame.width() + 1)),
                                    qBound(available.top(), frame.top(), qMax(available.top(), available.bottom() - frame.height() + 1)));
                m_dialog->move(m_dialog->pos() + target - frame.topLeft());
                updateButtons(m_dialog);
                reflowPanels(m_dialog);
            });
        }
        return false;
    }
private:
    QDialog* m_dialog;
    bool m_shown = false;
};

inline void fit(QDialog* dialog, QSize preferred)
{
    const QScreen* screen = dialog->screen();
    const QSize available = screen ? screen->availableGeometry().size() - QSize(40, 60) : QSize(800, 600);
    dialog->resize(preferred.boundedTo(available));
    if (!dialog->property("initialPlacementInstalled").toBool()) {
        new InitialPlacement(dialog);
        dialog->setProperty("initialPlacementInstalled", true);
    }
    updateButtons(dialog);
}

// Keep confirmation controls outside the scrolling editor on short displays.
inline void scrollEditor(QDialog* dialog, QSize preferred)
{
    auto* oldLayout = dialog->layout();
    auto* buttons = dialog->findChild<QDialogButtonBox*>();
    if (buttons) oldLayout->removeWidget(buttons);
    auto* content = new QWidget;
    content->setObjectName("dialogEditorContent");
    content->setLayout(oldLayout);
    auto* scroll = new QScrollArea(dialog);
    scroll->setObjectName("dialogEditorScroll");
    scroll->setStyleSheet("QScrollArea#dialogEditorScroll, QWidget#dialogEditorContent { background: #faf9f6; }");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    auto* root = new QVBoxLayout(dialog);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);
    root->addWidget(scroll, 1);
    if (buttons) root->addWidget(buttons);
    for (auto* label : content->findChildren<QLabel*>()) label->setWordWrap(true);
    dialog->setMinimumSize(320, 240);
    fit(dialog, preferred);
}
}
