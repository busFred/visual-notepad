// What the views that show columns share (read mode of the visual view, and
// the source view's columns): the room around the columns and the buttons
// that move a screen.
#pragma once

#include <QAbstractButton>

namespace vn::columns {

// Room around the columns: above them for the floating bar and the button
// for a line up, below for the one for a line down, and at the sides at
// least enough for the page arrows. The columns are centred in the rest.
inline constexpr int topRoom = 44;
inline constexpr int bottomRoom = 28;
inline constexpr int sideRoom = 56;
inline constexpr int gap = 40; // The least between two columns.
inline constexpr int pageArrowSize = 36;
inline constexpr int pageArrowInset = 10;
inline constexpr int lineButtonSize = 20;
inline constexpr int lineButtonTop = 18;
inline constexpr int lineButtonBottom = 24; // From the bottom edge to its top.

struct Sizing
{
    int count;
    int fit;
    int chars;
    int maxChars;
    int textWidth;
};

// Both views fit columns the same way. Source also needs room for its gutter.
Sizing sizing(int viewWidth, qreal charWidth, int textInset, int wanted, int chars);

// The gap between count columns of columnWidth in a view viewWidth wide:
// the room the columns leave, shared evenly between the gaps and the two
// sides, so a gap is never narrower than a side; but the sides keep sideRoom
// (for the page arrows), the gaps taking less if they must (never under gap).
int spreadGap(int viewWidth, int count, int columnWidth);
// Where the thin line between two columns goes: the middle of the gap left of
// a column that starts at columnLeft.
inline int dividerX(int columnLeft, int gapWidth) { return columnLeft - (gapWidth + 1) / 2; }

// A round page arrow at a side of the view, or one of the small buttons for
// a line up or down: a chevron, greyed when there is nowhere to go. They
// never take the focus, which stays on the text.
class PageButton : public QAbstractButton
{
public:
    enum class Direction { Left, Right, Up, Down };

    PageButton(Direction direction, bool round, QWidget *parent);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Direction m_direction;
    bool m_round;
};

} // namespace vn::columns
