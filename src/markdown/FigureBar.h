// The small bar over a picture or diagram in the visual view while the
// pointer is on it: − and + to make it smaller or larger, its size as a
// percentage, and Fit to go back to the size it was fitted to. It never takes
// the keyboard focus, and its clicks stay on it (the picture under it opens
// the viewer on a click).
#pragma once

#include <QFrame>

class QLabel;
class QToolButton;

namespace vn {

class FigureBar : public QFrame
{
    Q_OBJECT

public:
    enum class Action { Smaller, Larger, Fit };
    Q_ENUM(Action)

    explicit FigureBar(QWidget *parent = nullptr);

    // Shows the figure's width as a part of its own (per cent) and enables
    // the buttons that can still change it.
    void setFigure(int percent, bool canShrink, bool canGrow, bool fitted);

Q_SIGNALS:
    void action(vn::FigureBar::Action action);
    // The pointer left the bar (it may be on the figure still).
    void pointerLeft();

protected:
    void leaveEvent(QEvent *event) override;
    // A press or release between the buttons would reach the view under the bar.
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QToolButton *addButton(const QString &text, const QString &toolTip, Action action);

    QToolButton *m_smaller = nullptr;
    QToolButton *m_larger = nullptr;
    QToolButton *m_fit = nullptr;
    QLabel *m_percent = nullptr;
};

} // namespace vn
