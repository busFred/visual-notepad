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
    // QAbstractScrollArea also recognizes a two-finger pan. The raw
    // touchscreen sequence already decides what the fingers do here.
    if (event->type() == QEvent::Gesture && m_touchMode != TouchMode::Idle)
        return true;
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
        m_touchPageLineHeight = touchPageLineHeight();
        m_touchLineRemainder = 0;
    }
    if (m_touchMode == TouchMode::Idle)
        return false;
    if (type == QEvent::TouchCancel || points.size() != 1) {
        if (m_touchMode == TouchMode::Select) {
            m_dragging = false;
            m_autoScroll.stop();
        }
        m_touchTap.reset();
        // Keep consuming the sequence after any second finger.
        m_touchMode = type == QEvent::TouchCancel || type == QEvent::TouchEnd ? TouchMode::Idle : TouchMode::Ignore;
        return true;
    }

    if (m_touchMode != TouchMode::Ignore && type != QEvent::TouchBegin) {
        const QEventPoint &point = points.first();
        const QPointF at = point.position();
        const QPointF movement = at - m_touchStart;
        const qint64 now = m_touchTime.elapsed();
        if (m_touchMode == TouchMode::Tap && movement.manhattanLength() >= QApplication::startDragDistance()) {
            m_touchTap.reset();
            if (now >= touch::holdTime) {
                m_touchMode = TouchMode::Select;
                setFocus(Qt::MouseFocusReason);
                m_dragUnit = Unit::Word;
                m_dragOrigin = wordAt(positionAt(m_touchStart));
                m_dragging = true;
                setCaretRanges({m_dragOrigin}, 0);
            } else if (m_touchPageLineHeight > 0 && qAbs(movement.x()) > qAbs(movement.y())) {
                m_touchMode = TouchMode::Swipe;
            } else {
                m_touchMode = TouchMode::Scroll;
            }
        }

        if (m_touchMode == TouchMode::Select)
            touch::sendMouse(viewport(), QEvent::MouseMove, point.globalPosition(), event);
        if (m_touchMode == TouchMode::Scroll && at != m_touchLast) {
            const QPointF delta = m_touchLast - at;
            const qreal speed = delta.y() / qMax<qint64>(8, now - m_touchLastMove);
            m_touchVelocity = qBound(-maxVelocity, speed, maxVelocity);
            m_touchLastMove = now;
            m_touchLast = at;
            if (m_touchPageLineHeight == 0 && !m_wordWrap)
                horizontalScrollBar()->setValue(horizontalScrollBar()->value() + qRound(delta.x()));
            if (delta.y() != 0)
                touchScrollBy(delta.y());
        }
        if (type == QEvent::TouchEnd) {
            const TouchMode mode = m_touchMode;
            m_touchMode = TouchMode::Idle;
            if (mode == TouchMode::Tap) {
                const QPointF global = point.globalPosition();
                if (now >= touch::holdTime) {
                    m_touchTap.reset();
                    touch::postContextMenu(viewport(), global);
                } else {
                    if (touchTapActivates(at))
                        m_touchTap.reset();
                    else if (m_touchTap.tap(global)) {
                        setFocus(Qt::MouseFocusReason);
                        setCaretRanges({wordAt(positionAt(at))}, 0);
                        return true;
                    }
                    const QPointer<QWidget> target = viewport();
                    touch::sendMouse(target, QEvent::MouseButtonPress, global, event);
                    if (target)
                        touch::sendMouse(target, QEvent::MouseButtonRelease, global, event);
                }
            } else if (mode == TouchMode::Select) {
                touch::sendMouse(viewport(), QEvent::MouseButtonRelease, point.globalPosition(), event);
            } else if (mode == TouchMode::Swipe) {
                if (qAbs(movement.x()) >= qMax(36, 4 * QApplication::startDragDistance())
                    && qAbs(movement.x()) > qAbs(movement.y())) {
                    touchTurnPage(movement.x() < 0);
                }
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

bool TextView::touchTapActivates(const QPointF &)
{
    return false;
}

bool TextView::touchScrollBy(qreal pixels)
{
    if (m_touchPageLineHeight == 0)
        return scrollByPixels(pixels);

    m_touchLineRemainder += pixels;
    const int lines = int(m_touchLineRemainder / m_touchPageLineHeight);
    m_touchLineRemainder -= lines * m_touchPageLineHeight;
    if (lines != 0)
        touchNudge(lines);
    return true; // An animation tick can be smaller than a line.
}

void TextView::touchFlickTick()
{
    const qreal dt = m_touchTime.restart();
    const qreal decay = std::exp(-dt / flickTime);
    const bool moved = touchScrollBy(m_touchVelocity * flickTime * (1 - decay));
    m_touchVelocity *= decay;
    if (!moved || qAbs(m_touchVelocity) < minVelocity)
        m_touchFlick.stop();
}

} // namespace vn
