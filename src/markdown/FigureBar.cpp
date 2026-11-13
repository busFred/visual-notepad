#include "FigureBar.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QToolButton>

namespace vn {

FigureBar::FigureBar(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("FigureBar"));
    setAttribute(Qt::WA_StyledBackground);
    setFocusPolicy(Qt::NoFocus);
    // A child of the viewport, which has the text cursor: the buttons get the
    // ordinary pointer.
    setCursor(Qt::ArrowCursor);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 3, 4, 3);
    layout->setSpacing(2);
    m_smaller = addButton(tr("−"), tr("Make the picture smaller"), Action::Smaller);
    m_percent = new QLabel(QStringLiteral("100%"), this);
    m_percent->setObjectName(QStringLiteral("FigureBarPercent"));
    m_percent->setAlignment(Qt::AlignCenter);
    m_percent->setToolTip(tr("Width as a part of the picture's own"));
    layout->addWidget(m_smaller);
    layout->addWidget(m_percent);
    m_larger = addButton(tr("+"), tr("Make the picture larger"), Action::Larger);
    layout->addWidget(m_larger);
    m_fit = addButton(tr("Fit"), tr("Back to the size that fits the window"), Action::Fit);
    layout->addWidget(m_fit);
}

QToolButton *FigureBar::addButton(const QString &text, const QString &toolTip, Action which)
{
    auto *button = new QToolButton(this);
    button->setObjectName(QStringLiteral("TableToolButton")); // The table toolbar's look.
    button->setText(text);
    button->setToolTip(toolTip);
    button->setFocusPolicy(Qt::NoFocus);
    connect(button, &QToolButton::clicked, this, [this, which] { Q_EMIT action(which); });
    return button;
}

void FigureBar::setFigure(int percent, bool canShrink, bool canGrow, bool fitted)
{
    m_percent->setText(tr("%1%").arg(percent));
    m_smaller->setEnabled(canShrink);
    m_larger->setEnabled(canGrow);
    m_fit->setEnabled(!fitted);
}

void FigureBar::leaveEvent(QEvent *event)
{
    QFrame::leaveEvent(event);
    Q_EMIT pointerLeft();
}

void FigureBar::mousePressEvent(QMouseEvent *event)
{
    event->accept();
}

void FigureBar::mouseReleaseEvent(QMouseEvent *event)
{
    event->accept();
}

void FigureBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
}

} // namespace vn
