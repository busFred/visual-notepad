#pragma once

#include <QApplication>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QStyleHints>
#include <QMouseEvent>
#include <QTouchEvent>
#include <QWidget>

namespace vn::touch {

inline QTouchEvent *screenEvent(QEvent *event)
{
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
        auto *touch = static_cast<QTouchEvent *>(event);
        return touch->pointingDevice()->type() == QInputDevice::DeviceType::TouchScreen ? touch : nullptr;
    }
    default:
        return nullptr;
    }
}

// Reuse the widget's click behavior and keep the touchscreen as the source.
inline void sendMouse(QWidget *widget, QEvent::Type type, const QPointF &global, const QTouchEvent *touch)
{
    const QPointF local = widget->mapFromGlobal(global);
    const QPointF scene = widget->window()->mapFromGlobal(global);
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent mouse(type, local, scene, global, button, buttons, touch->modifiers(),
                      Qt::MouseEventSynthesizedByApplication, touch->pointingDevice());
    mouse.setTimestamp(touch->timestamp());
    QCoreApplication::sendEvent(widget, &mouse);
}

inline constexpr qint64 holdTime = 500;

class DoubleTap
{
public:
    bool tap(const QPointF &position)
    {
        const auto *hints = QApplication::styleHints();
        const bool second = m_time.isValid() && m_time.elapsed() <= hints->mouseDoubleClickInterval()
                            && (position - m_last).manhattanLength() <= hints->touchDoubleTapDistance();
        if (second) {
            reset();
        } else {
            m_last = position;
            m_time.start();
        }
        return second;
    }

    void reset() { m_time.invalidate(); }

private:
    QElapsedTimer m_time;
    QPointF m_last;
};

inline void postContextMenu(QWidget *target, const QPointF &global)
{
    const QPoint at = global.toPoint();
    // Context menus may run a nested event loop. Deliver after touch end.
    QCoreApplication::postEvent(target, new QContextMenuEvent(QContextMenuEvent::Other, target->mapFromGlobal(at), at));
}

} // namespace vn::touch
