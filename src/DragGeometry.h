// Geometry of a tab drag, as pure functions so it can be tested.
#pragma once

#include <QPoint>
#include <QRect>

namespace vn::dragGeometry {

// Where the drag pixmap is held: the point of the tab that was pressed,
// relative to the tab's top-left, kept inside the pixmap. The drag only
// starts once the cursor has left the tab strip, so the cursor position at
// that moment must not be used: it can be far outside the tab, and the
// pixmap would then be drawn that far away from the cursor.
inline QPoint hotSpot(const QPoint &pressInTabBar, const QRect &tabRect)
{
    const QPoint offset = pressInTabBar - tabRect.topLeft();
    return QPoint(qBound(0, offset.x(), qMax(0, tabRect.width() - 1)), qBound(0, offset.y(), qMax(0, tabRect.height() - 1)));
}

} // namespace vn::dragGeometry
