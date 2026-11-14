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
inline constexpr int gap = 40;
inline constexpr int pageArrowSize = 36;
inline constexpr int pageArrowInset = 10;
inline constexpr int lineButtonSize = 20;
inline constexpr int lineButtonTop = 18;
inline constexpr int lineButtonBottom = 24; // From the bottom edge to its top.

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
