#include "TextView.h"

#include "../TouchInput.h"

#include <QApplication>
#include <QScrollBar>

#include <cmath>

namespace vn {

namespace {
constexpr qreal flickTime = 250; // Milliseconds over which the velocity falls to about a third.
constexpr qreal minVelocity = 0.025; // Pixels per millisecond.
constexpr qreal maxVelocity = 3;
constexpr qint64 flickPause = 80; // A finger held still before lifting should not start a flick.
}

bool TextView::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Wheel)
        m_touchFlick.stop();
    if (auto *finger = touch::screenEvent(event)) {
        if (handleTouch(finger)) {
            event->accept();
            return true;
        }
    }
    return QAbstractScrollArea::viewportEvent(event);
}

bool TextView::handleTouch(QTouchEvent *event)
{
    const auto type = event->type();
    const auto &points = event->points();
    if (type == QEvent::TouchBegin) {
        if (points.isEmpty() || viewport()->childAt(points.first().position().toPoint()))
            return false; // Let a button or an inline editor handle the contact.
        m_touchFlick.stop();
        m_touchMode = TouchMode::Tap;
        m_touchStart = m_touchLast = points.first().position();
        m_touchTime.start();
        m_touchLastMove = 0;
        m_touchVelocity = 0;
    }
    if (m_touchMode == TouchMode::Idle)
        return false;
    if (type == QEvent::TouchCancel) {
        m_touchMode = TouchMode::Idle;
        return true;
    }
    if (points.size() != 1)
        m_touchMode = TouchMode::Ignore; // Keep consuming the sequence after any second finger.

    if (m_touchMode != TouchMode::Ignore && type != QEvent::TouchBegin) {
        const QEventPoint &point = points.first();
        const QPointF at = point.position();
        const QPointF movement = at - m_touchStart;
        const qint64 now = m_touchTime.elapsed();
        if (m_touchMode == TouchMode::Tap && movement.manhattanLength() >= QApplication::startDragDistance())
            m_touchMode = TouchMode::Scroll;

        if (m_touchMode == TouchMode::Scroll && at != m_touchLast) {
            const QPointF delta = m_touchLast - at;
            const qreal speed = delta.y() / qMax<qint64>(8, now - m_touchLastMove);
            m_touchVelocity = qBound(-maxVelocity, speed, maxVelocity);
            m_touchLastMove = now;
            m_touchLast = at;
            if (!m_wordWrap)
                horizontalScrollBar()->setValue(horizontalScrollBar()->value() + qRound(delta.x()));
            scrollByPixels(delta.y());
        }
        if (type == QEvent::TouchEnd) {
            const TouchMode mode = m_touchMode;
            m_touchMode = TouchMode::Idle;
            if (mode == TouchMode::Tap) {
                const QPointF global = point.globalPosition();
                const QPointer<QWidget> target = viewport();
                touch::sendMouse(target, QEvent::MouseButtonPress, global, event);
                if (target)
                    touch::sendMouse(target, QEvent::MouseButtonRelease, global, event);
            } else if (mode == TouchMode::Scroll && now - m_touchLastMove < flickPause
                       && qAbs(m_touchVelocity) >= minVelocity) {
                m_touchTime.restart();
                m_touchFlick.start(16, Qt::PreciseTimer, this);
            }
        }
    } else if (type == QEvent::TouchEnd) {
        m_touchMode = TouchMode::Idle;
    }
    return true;
}

void TextView::touchFlickTick()
{
    const qreal dt = m_touchTime.restart();
    const qreal decay = std::exp(-dt / flickTime);
    const bool moved = scrollByPixels(m_touchVelocity * flickTime * (1 - decay));
    m_touchVelocity *= decay;
    if (!moved || qAbs(m_touchVelocity) < minVelocity)
        m_touchFlick.stop();
}

} // namespace vn
