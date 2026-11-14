#include "ReadControls.h"

#include "../Theme.h"

#include <QPainter>
#include <QPainterPath>

namespace vn::columns {

PageButton::PageButton(Direction direction, bool round, QWidget *parent)
    : QAbstractButton(parent)
    , m_direction(direction)
    , m_round(round)
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
    setAttribute(Qt::WA_Hover);
    const int size = round ? pageArrowSize : lineButtonSize;
    setFixedSize(size, size);
}

void PageButton::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool hot = isEnabled() && underMouse();
    if (m_round) {
        QPainterPath circle;
        circle.addEllipse(r);
        painter.fillPath(circle, theme::color(isDown() ? theme::Role::panelTitle
                                              : hot     ? theme::Role::sideBarHover
                                                        : theme::Role::sideBar));
        painter.setPen(theme::color(theme::Role::divider));
        painter.drawPath(circle);
    } else if (hot) {
        QPainterPath box;
        box.addRoundedRect(r, 3, 3);
        painter.fillPath(box, theme::color(isDown() ? theme::Role::panelTitle : theme::Role::sideBarHover));
    }
    // A chevron 10 px across, as in the mockups' 8-unit glyph.
    const qreal s = 1.25;
    const QPointF c = QRectF(rect()).center();
    QPolygonF chevron;
    switch (m_direction) {
    case Direction::Left:
        chevron << c + QPointF(1.5 * s, -3 * s) << c + QPointF(-1.5 * s, 0) << c + QPointF(1.5 * s, 3 * s);
        break;
    case Direction::Right:
        chevron << c + QPointF(-1.5 * s, -3 * s) << c + QPointF(1.5 * s, 0) << c + QPointF(-1.5 * s, 3 * s);
        break;
    case Direction::Up:
        chevron << c + QPointF(-3 * s, 1.5 * s) << c + QPointF(0, -1.5 * s) << c + QPointF(3 * s, 1.5 * s);
        break;
    case Direction::Down:
        chevron << c + QPointF(-3 * s, -1.5 * s) << c + QPointF(0, 1.5 * s) << c + QPointF(3 * s, -1.5 * s);
        break;
    }
    painter.setPen(QPen(theme::color(isEnabled() ? theme::Role::text : theme::Role::mutedText), 1.5, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.drawPolyline(chevron);
}

} // namespace vn::columns
