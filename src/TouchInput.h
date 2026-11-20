#pragma once

#include <QCoreApplication>
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

} // namespace vn::touch
