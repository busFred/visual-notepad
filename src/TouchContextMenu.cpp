#include "TouchContextMenu.h"

#include "TouchInput.h"

#include <QAbstractSpinBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTextEdit>

namespace vn::touch {

namespace {

class HoldForMenu : public QObject
{
public:
    explicit HoldForMenu(QWidget *widget)
        : QObject(widget)
        , m_widget(widget)
    {
        widget->setAttribute(Qt::WA_AcceptTouchEvents);
        widget->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        const auto *finger = screenEvent(event);
        if (!finger)
            return false;

        const auto type = event->type();
        const auto &points = finger->points();
        if (type == QEvent::TouchBegin) {
            if (points.isEmpty() || m_widget->childAt(points.first().position().toPoint()))
                return false;
            m_held.start();
            m_start = m_last = points.first().globalPosition();
            m_dragging = false;
            m_ignored = false;
        }
        if (!m_held.isValid())
            return false;
        event->accept();

        if (type == QEvent::TouchCancel || points.size() != 1) {
            if (m_dragging)
                sendMouse(m_widget, QEvent::MouseButtonRelease, m_last, finger);
            m_dragging = false;
            m_ignored = true;
        }
        if (m_ignored) {
            if (type == QEvent::TouchEnd || type == QEvent::TouchCancel)
                m_held.invalidate();
            return true;
        }

        const QPointF at = points.first().globalPosition();
        const QPointer<QWidget> target = m_widget;
        if (!m_dragging && (at - m_start).manhattanLength() >= QApplication::startDragDistance()) {
            m_dragging = true;
            // A drag still goes through the usual tab or text field handling.
            sendMouse(target, QEvent::MouseButtonPress, m_start, finger);
        }
        m_last = at;
        if (type == QEvent::TouchEnd) {
            const bool held = m_held.elapsed() >= holdTime;
            m_held.invalidate();
            if (target && m_dragging) {
                sendMouse(target, QEvent::MouseButtonRelease, at, finger);
            } else if (target && held) {
                postContextMenu(target, at);
            } else if (target) {
                // A second tap is a double click, as a text field selects a word with.
                sendMouse(target, m_tap.tap(at) ? QEvent::MouseButtonDblClick : QEvent::MouseButtonPress, at, finger);
                if (target)
                    sendMouse(target, QEvent::MouseButtonRelease, at, finger);
            }
        } else if (target && m_dragging) {
            sendMouse(target, QEvent::MouseMove, at, finger);
        }
        return true;
    }

private:
    QWidget *m_widget;
    QElapsedTimer m_held;
    QPointF m_start, m_last;
    bool m_dragging = false;
    bool m_ignored = false;
    DoubleTap m_tap;
};

// Qt's text controls supply their own right-click menus. Treat them like
// the app's custom menus, including text editors made inside other widgets.
class ContextMenus : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::Polish)
            return false;
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget)
            return false;

        if (auto *edit = qobject_cast<QTextEdit *>(widget))
            enableContextMenu(edit->viewport());
        else if (auto *edit = qobject_cast<QPlainTextEdit *>(widget))
            enableContextMenu(edit->viewport());
        // (Not spin boxes: a finger held on their arrows repeats the step.)
        else if (qobject_cast<QAbstractSpinBox *>(widget))
            return false;
        else if (qobject_cast<QLineEdit *>(widget) || widget->contextMenuPolicy() == Qt::ActionsContextMenu
                 || widget->contextMenuPolicy() == Qt::CustomContextMenu)
            enableContextMenu(widget);
        return false;
    }
};

} // namespace

void enableContextMenu(QWidget *widget)
{
    // Some custom menus, such as the one on the tabs, can be used in tests
    // or in an embedded window before the application filter is installed.
    if (!widget->property("vnTouchContextMenu").toBool()) {
        widget->setProperty("vnTouchContextMenu", true);
        new HoldForMenu(widget);
    }
}

void installContextMenus(QApplication &app)
{
    app.installEventFilter(new ContextMenus(&app));
}

} // namespace vn::touch
