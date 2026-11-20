#include "ViewerTouch.h"

#include <QLineF>

#include <utility>

namespace vn {

ViewerTouch::ViewerTouch(QWidget *canvas, Pan pan, Zoom zoom, Toggle toggle)
    : QObject(canvas)
    , m_canvas(canvas)
    , m_pan(std::move(pan))
    , m_zoom(std::move(zoom))
    , m_toggle(std::move(toggle))
{
    canvas->setAttribute(Qt::WA_AcceptTouchEvents);
    canvas->installEventFilter(this);
}

bool ViewerTouch::eventFilter(QObject *, QEvent *event)
{
    if (event->type() == QEvent::Hide) {
        m_active = false;
        m_taps.reset();
    }
    if (event->type() == QEvent::Gesture && m_active)
        return true; // A scroll area can also recognize pan from these points.
    const auto *finger = touch::screenEvent(event);
    if (!finger)
        return false;
    const auto &points = finger->points();
    if (event->type() == QEvent::TouchBegin) {
        if (points.isEmpty() || m_canvas->childAt(points.first().position().toPoint()))
            return false; // The zoom bar and navigation buttons still take taps.
        m_active = true;
        m_moved = false;
        m_first = m_second = -1;
        m_contact.start();
    }
    if (!m_active)
        return false;
    event->accept();

    if (event->type() == QEvent::TouchCancel) {
        m_active = false;
        m_taps.reset();
        return true;
    }
    if (event->type() == QEvent::TouchEnd) {
        m_active = false;
        if (points.size() == 1 && m_second == -1 && points.first().id() == m_first) {
            const auto &point = points.first();
            const QPointF delta = point.position() - m_last;
            if (m_moved || delta.manhattanLength() >= QApplication::startDragDistance()) {
                m_taps.reset();
                if (!delta.isNull())
                    m_pan(delta);
                return true;
            }
            if (m_contact.elapsed() < touch::holdTime) {
                if (m_taps.tap(point.globalPosition()) && m_toggle)
                    m_toggle(point.position());
                return true;
            }
        }
        m_taps.reset();
        return true;
    }

    const QEventPoint *first = nullptr;
    const QEventPoint *second = nullptr;
    int count = 0;
    for (const QEventPoint &point : points) {
        if (point.state() == QEventPoint::State::Released)
            continue;
        if (++count == 1)
            first = &point;
        else if (count == 2)
            second = &point;
    }
    if (count == 2) {
        if (first->id() > second->id())
            std::swap(first, second);
        const QPointF centre = (first->position() + second->position()) / 2;
        const qreal distance = QLineF(first->position(), second->position()).length();
        if (m_zoom && m_first == first->id() && m_second == second->id() && m_distance > 0 && distance > 0)
            m_zoom(distance / m_distance, m_last, centre);
        m_first = first->id();
        m_second = second->id();
        m_last = centre;
        m_distance = distance;
        m_moved = true;
        m_taps.reset();
    } else if (count == 1) {
        const QPointF at = first->position();
        // After lifting one finger, start panning from the remaining one.
        if (m_first != first->id() || m_second != -1) {
            m_first = first->id();
            m_second = -1;
            m_last = at;
        } else if (m_moved || (at - m_last).manhattanLength() >= QApplication::startDragDistance()) {
            m_pan(at - m_last);
            m_last = at;
            m_moved = true;
            m_taps.reset();
        }
    } else {
        m_first = m_second = -1;
        m_moved = true;
        m_taps.reset();
    }
    return true;
}

} // namespace vn
