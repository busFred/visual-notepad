// Geometry of a tab drag, as pure functions so it can be tested.
#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>

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

// Where a new editor group goes relative to an existing one (VS Code's
// GroupDirection); None merges into the group itself.
enum class SplitDirection { None, Up, Down, Left, Right };

// Ctrl (Alt on macOS) copies the dragged tabs instead of moving them:
// editorDropTarget.ts isCopyOperation().
inline bool isCopyOperation(Qt::KeyboardModifiers modifiers)
{
#ifdef Q_OS_MACOS
    // Qt maps the Command key to Control; Option is Alt.
    return modifiers & Qt::AltModifier;
#else
    return modifiers & Qt::ControlModifier;
#endif
}

// Alt (Shift on macOS) turns splitting on drop off (it is on by default):
// editorDropTarget.ts isToggleSplitOperation() and its use in onDragOver.
inline bool isToggleSplitOperation(Qt::KeyboardModifiers modifiers)
{
#ifdef Q_OS_MACOS
    return modifiers & Qt::ShiftModifier;
#else
    return modifiers & Qt::AltModifier;
#endif
}

// The drop zone under `pos` in an editor body of `size` while a tab is
// dragged over it. Mirrors editorDropTarget.ts positionOverlay() for
// editors (not whole groups): a 10% band along each edge offers a split;
// with preferSplitVertically (workbench.editor.openSideBySideDirection
// "right", the default) the left and right thirds of that band split
// sideways and its middle splits up or down by the half the cursor is in,
// otherwise the top and bottom thirds split up/down and the rest left/right.
inline SplitDirection dropZone(const QPoint &pos, const QSize &size, bool enableSplitting = true,
                               bool preferSplitVertically = true)
{
    if (!enableSplitting)
        return SplitDirection::None;
    const double width = size.width();
    const double height = size.height();
    const double x = pos.x();
    const double y = pos.y();
    const double edgeWidth = width * 0.1;
    const double edgeHeight = height * 0.1;
    if (x > edgeWidth && x < width - edgeWidth && y > edgeHeight && y < height - edgeHeight)
        return SplitDirection::None;
    const double splitWidth = width / 3;
    const double splitHeight = height / 3;
    if (preferSplitVertically) {
        if (x < splitWidth)
            return SplitDirection::Left;
        if (x > splitWidth * 2)
            return SplitDirection::Right;
        return y < height / 2 ? SplitDirection::Up : SplitDirection::Down;
    }
    if (y < splitHeight)
        return SplitDirection::Up;
    if (y > splitHeight * 2)
        return SplitDirection::Down;
    return x < width / 2 ? SplitDirection::Left : SplitDirection::Right;
}

// The shaded part of `body` for a zone: the half the new group would take,
// or all of it for a merge (editorDropTarget.ts positionOverlay(), the
// switch at its end).
inline QRect overlayRect(SplitDirection zone, const QRect &body)
{
    const int halfWidth = body.width() / 2;
    const int halfHeight = body.height() / 2;
    switch (zone) {
    case SplitDirection::Up:
        return QRect(body.left(), body.top(), body.width(), halfHeight);
    case SplitDirection::Down:
        return QRect(body.left(), body.top() + halfHeight, body.width(), body.height() - halfHeight);
    case SplitDirection::Left:
        return QRect(body.left(), body.top(), halfWidth, body.height());
    case SplitDirection::Right:
        return QRect(body.left() + halfWidth, body.top(), body.width() - halfWidth, body.height());
    case SplitDirection::None:
        break;
    }
    return body;
}

} // namespace vn::dragGeometry
