#include "TouchScrollArea.h"

#include "TouchInput.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPointer>
#include <QScroller>
#include <QScrollerProperties>

namespace vn::touch {

namespace {

bool takesDrag(QWidget *target, QWidget *viewport)
{
    for (QWidget *widget = target; widget && widget != viewport; widget = widget->parentWidget()) {
        const auto *button = qobject_cast<QAbstractButton *>(widget);
        if ((button && button->isCheckable()) || qobject_cast<QAbstractSlider *>(widget)
            || qobject_cast<QComboBox *>(widget) || qobject_cast<QAbstractSpinBox *>(widget)
            || qobject_cast<QLineEdit *>(widget)) {
            return true;
        }
    }
    return false;
}

class ScrollTouch : public QObject
{
public:
    explicit ScrollTouch(QAbstractScrollArea *area)
        : QObject(area->viewport())
        , m_view(area->viewport())
        , m_items(qobject_cast<QAbstractItemView *>(area))
        , m_scroller(QScroller::scroller(m_view))
    {
        m_view->setAttribute(Qt::WA_AcceptTouchEvents);
        m_view->installEventFilter(this);
        // Keep the viewport in place, including when it contains controls.
        auto properties = m_scroller->scrollerProperties();
        properties.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
        properties.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy, QScrollerProperties::OvershootAlwaysOff);
        m_scroller->setScrollerProperties(properties);
        connect(m_scroller, &QScroller::stateChanged, this, [this](QScroller::State state) {
            if (state == QScroller::Inactive && !m_tracking)
                restoreScrollMode();
        });
        m_clock.start();
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Hide || event->type() == QEvent::Wheel
            || (event->type() == QEvent::MouseButtonPress && !m_tracking)) {
            stop();
        }
        if (event->type() == QEvent::Gesture && m_tracking)
            return true; // The raw touch already drives the scroller.
        const auto *finger = screenEvent(event);
        if (!finger)
            return false;

        const auto type = event->type();
        const auto &points = finger->points();
        const qint64 now = m_clock.elapsed();
        if (type == QEvent::TouchBegin) {
            if (points.isEmpty())
                return false;
            m_start = points.first().position();
            QWidget *child = m_view->childAt(m_start.toPoint());
            const bool wasMoving = m_scroller->state() == QScroller::Scrolling;
            stop();
            if (takesDrag(child, m_view))
                return false; // Qt's usual touchscreen click and drag still go to the control.
            m_target = child ? child : m_view;
            m_started = now;
            m_tracking = true;
            m_dragging = false;
            m_skipTap = wasMoving;
            m_ignored = false;
        }
        if (!m_tracking)
            return false;
        event->accept();

        if (type == QEvent::TouchCancel) {
            stop();
            return true;
        }
        if (points.size() != 1) {
            // A second finger cancels scrolling and the pending tap.
            m_ignored = true;
            m_scroller->stop();
        }
        if (m_ignored) {
            if (type == QEvent::TouchEnd)
                stop();
            return true;
        }

        const auto &point = points.first();
        const QPointF at = point.position();
        if (!m_dragging && (at - m_start).manhattanLength() >= QApplication::startDragDistance()) {
            m_dragging = true;
            usePixelScrolling();
            m_scroller->handleInput(QScroller::InputPress, m_start, m_started);
        }
        if (m_dragging && type != QEvent::TouchBegin) {
            m_scroller->handleInput(QScroller::InputMove, at, now);
            if (type == QEvent::TouchEnd)
                m_scroller->handleInput(QScroller::InputRelease, at, now);
        }
        if (type == QEvent::TouchEnd) {
            m_tracking = false;
            if (m_scroller->state() == QScroller::Inactive)
                restoreScrollMode();
            if (!m_dragging && !m_skipTap && m_target) {
                if (now - m_started >= holdTime) {
                    postContextMenu(m_target, point.globalPosition());
                } else {
                    const QPointer<QWidget> target = m_target;
                    sendMouse(target, QEvent::MouseButtonPress, point.globalPosition(), finger);
                    if (target)
                        sendMouse(target, QEvent::MouseButtonRelease, point.globalPosition(), finger);
                }
            }
        }
        return true;
    }

private:
    void stop()
    {
        m_tracking = false;
        if (m_scroller->state() != QScroller::Inactive)
            m_scroller->stop();
        restoreScrollMode();
    }

    void usePixelScrolling()
    {
        if (!m_items || m_pixelScrolling)
            return;
        // The scroller measures pixels. Restore the list's usual wheel and
        // scrollbar units as soon as the finger and its momentum stop.
        m_vertical = m_items->verticalScrollMode();
        m_horizontal = m_items->horizontalScrollMode();
        m_pixelScrolling = true;
        if (m_vertical != QAbstractItemView::ScrollPerPixel)
            m_items->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        if (m_horizontal != QAbstractItemView::ScrollPerPixel)
            m_items->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    }

    void restoreScrollMode()
    {
        if (!m_pixelScrolling || !m_items)
            return;
        m_pixelScrolling = false;
        if (m_vertical != QAbstractItemView::ScrollPerPixel)
            m_items->setVerticalScrollMode(m_vertical);
        if (m_horizontal != QAbstractItemView::ScrollPerPixel)
            m_items->setHorizontalScrollMode(m_horizontal);
    }

    QWidget *m_view;
    QPointer<QAbstractItemView> m_items;
    QScroller *m_scroller;
    QPointer<QWidget> m_target;
    QElapsedTimer m_clock;
    QPointF m_start;
    qint64 m_started = 0;
    QAbstractItemView::ScrollMode m_vertical = QAbstractItemView::ScrollPerItem;
    QAbstractItemView::ScrollMode m_horizontal = QAbstractItemView::ScrollPerItem;
    bool m_pixelScrolling = false;
    bool m_tracking = false;
    bool m_dragging = false;
    bool m_ignored = false;
    bool m_skipTap = false;
};

} // namespace

void enableKineticScrolling(QAbstractScrollArea *area)
{
    new ScrollTouch(area);
}

} // namespace vn::touch
