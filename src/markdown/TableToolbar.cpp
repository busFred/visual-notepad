#include "TableToolbar.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QToolButton>

namespace vn {

namespace {

// Four lines of text, flush left, centered, or flush right.
QIcon alignIcon(md::TableAlignment alignment, const QColor &color, qreal ratio)
{
    constexpr int size = 14;
    QPixmap pixmap(QSize(size, size) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setPen(QPen(color, 1.5));
    const int widths[] = {12, 8, 12, 6};
    for (int i = 0; i < 4; ++i) {
        const int w = widths[i];
        int x = 1;
        if (alignment == md::TableAlignment::Center)
            x = (size - w) / 2;
        else if (alignment == md::TableAlignment::Right)
            x = size - 1 - w;
        else if (alignment == md::TableAlignment::None)
            x = 1;
        const qreal y = 2.5 + i * 3;
        p.drawLine(QPointF(x, y), QPointF(x + w, y));
    }
    return QIcon(pixmap);
}

} // namespace

TableToolbar::TableToolbar(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("TableToolbar"));
    setAttribute(Qt::WA_StyledBackground);
    setFocusPolicy(Qt::NoFocus);
    // A child of the editor's viewport, which has the text cursor: the
    // buttons get the ordinary pointer.
    setCursor(Qt::ArrowCursor);
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 3, 4, 3);
    layout->setSpacing(2);

    using Op = md::TableOp;
    layout->addWidget(addButton(tr("+ Row ↑"), tr("Insert a row above"), Op::RowAbove));
    layout->addWidget(addButton(tr("+ Row ↓"), tr("Insert a row below"), Op::RowBelow));
    layout->addWidget(addButton(tr("+ Col ←"), tr("Insert a column to the left"), Op::ColumnLeft));
    layout->addWidget(addButton(tr("+ Col →"), tr("Insert a column to the right"), Op::ColumnRight));
    layout->addWidget(addButton(tr("− Row"), tr("Delete the row"), Op::DeleteRow));
    layout->addWidget(addButton(tr("− Col"), tr("Delete the column"), Op::DeleteColumn));

    const struct
    {
        Op op;
        const char *tip;
    } aligns[] = {{Op::AlignNone, QT_TR_NOOP("Default alignment of the column")},
                  {Op::AlignLeft, QT_TR_NOOP("Align the column left")},
                  {Op::AlignCenter, QT_TR_NOOP("Center the column")},
                  {Op::AlignRight, QT_TR_NOOP("Align the column right")}};
    for (int i = 0; i < 4; ++i) {
        // The default is left too, only without a colon: it gets no icon of its own.
        if (i == 0)
            continue;
        m_aligns[i] = addButton(QString(), tr(aligns[i].tip), aligns[i].op);
        m_aligns[i]->setCheckable(true);
        layout->addWidget(m_aligns[i]);
    }

    auto *more = new QToolButton(this);
    more->setObjectName(QStringLiteral("TableToolButton"));
    more->setText(tr("More ▾"));
    more->setToolTip(tr("More table commands"));
    more->setFocusPolicy(Qt::NoFocus);
    more->setPopupMode(QToolButton::InstantPopup);
    m_menu = new QMenu(more);
    more->setMenu(m_menu);
    layout->addWidget(more);
    updateIcons();
}

QToolButton *TableToolbar::addButton(const QString &text, const QString &toolTip, md::TableOp op)
{
    auto *button = new QToolButton(this);
    button->setObjectName(QStringLiteral("TableToolButton"));
    button->setText(text);
    button->setToolTip(toolTip);
    button->setFocusPolicy(Qt::NoFocus);
    m_ops.insert(button, op);
    connect(button, &QToolButton::clicked, this, [this, op] { Q_EMIT operation(op); });
    return button;
}

void TableToolbar::setState(const md::TableInfo &info)
{
    for (auto it = m_ops.cbegin(); it != m_ops.cend(); ++it)
        it.key()->setEnabled(md::tableOpAvailable(info, it.value()));
    for (int i = 1; i < 4; ++i) {
        if (!m_aligns[i])
            continue;
        // Clicking the alignment a column has already goes back to the default.
        m_aligns[i]->setChecked(info.found && int(info.alignment) == i);
    }
}

void TableToolbar::changeEvent(QEvent *event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        updateIcons();
}

void TableToolbar::updateIcons()
{
    const qreal ratio = devicePixelRatioF();
    const QColor color = palette().color(QPalette::WindowText);
    const md::TableAlignment kinds[] = {md::TableAlignment::None, md::TableAlignment::Left,
                                        md::TableAlignment::Center, md::TableAlignment::Right};
    for (int i = 1; i < 4; ++i) {
        if (m_aligns[i])
            m_aligns[i]->setIcon(alignIcon(kinds[i], color, ratio));
    }
}

} // namespace vn
