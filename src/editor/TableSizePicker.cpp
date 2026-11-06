#include "TableSizePicker.h"

#include "../Theme.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSpinBox>
#include <QTimer>
#include <QWidgetAction>

namespace vn {

namespace {

constexpr int cell = 16;  // A square, with its gap.
constexpr int gap = 2;
constexpr int margin = 6;

} // namespace

TableSizePicker::TableSizePicker(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    setToolTip(tr("Rows include the header row"));
}

int TableSizePicker::labelHeight() const
{
    return fontMetrics().height() + 6;
}

QSize TableSizePicker::sizeHint() const
{
    return {2 * margin + gridColumns * cell - gap, 2 * margin + gridRows * cell - gap + labelHeight()};
}

QRect TableSizePicker::cellRect(int row, int col) const
{
    return {margin + col * cell, margin + labelHeight() + row * cell, cell - gap, cell - gap};
}

void TableSizePicker::setSelection(int rows, int columns)
{
    rows = qBound(0, rows, gridRows);
    columns = qBound(0, columns, gridColumns);
    if (rows == m_rows && columns == m_cols)
        return;
    m_rows = rows;
    m_cols = columns;
    update();
}

void TableSizePicker::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QColor edge = theme::color(theme::Role::divider);
    const QColor fill = theme::color(theme::Role::editor);
    const QColor on = theme::color(theme::Role::selection);
    const QColor onEdge = theme::color(theme::Role::accent);
    for (int r = 0; r < gridRows; ++r) {
        for (int c = 0; c < gridColumns; ++c) {
            const bool chosen = r < m_rows && c < m_cols;
            p.setPen(chosen ? onEdge : edge);
            p.setBrush(chosen ? on : fill);
            p.drawRect(QRectF(cellRect(r, c)).adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }
    // The first row is the header: a line under it.
    p.setPen(QPen(edge, 1));
    const int y = margin + labelHeight() + cell - gap / 2 - 1;
    p.drawLine(margin, y, margin + gridColumns * cell - gap, y);

    p.setPen(palette().color(QPalette::WindowText));
    const QString label = m_rows > 0 && m_cols > 0 ? tr("%1 × %2 (rows × columns)").arg(m_rows).arg(m_cols)
                                                   : tr("Point at a size");
    p.drawText(QRect(margin, margin, width() - 2 * margin, labelHeight() - 4), Qt::AlignLeft | Qt::AlignVCenter, label);
}

void TableSizePicker::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint at = event->position().toPoint() - QPoint(margin, margin + labelHeight());
    if (at.x() < 0 || at.y() < 0 || at.x() >= gridColumns * cell || at.y() >= gridRows * cell)
        return; // Keep the last size while crossing the margin.
    // At least the header and one more row.
    setSelection(qMax(2, at.y() / cell + 1), at.x() / cell + 1);
}

void TableSizePicker::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_rows > 0 && m_cols > 0)
        Q_EMIT picked(m_rows, m_cols);
}

void TableSizePicker::keyPressEvent(QKeyEvent *event)
{
    const int rows = qMax(2, m_rows);
    const int cols = qMax(1, m_cols);
    switch (event->key()) {
    case Qt::Key_Right:
        setSelection(rows, m_cols == 0 ? 1 : cols + 1);
        break;
    case Qt::Key_Left:
        setSelection(rows, qMax(1, cols - 1));
        break;
    case Qt::Key_Down:
        setSelection(m_rows == 0 ? 2 : rows + 1, cols);
        break;
    case Qt::Key_Up:
        setSelection(qMax(2, rows - 1), cols);
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        if (m_rows > 0 && m_cols > 0)
            Q_EMIT picked(m_rows, m_cols);
        break;
    default:
        QWidget::keyPressEvent(event); // Escape closes the menu.
        return;
    }
    event->accept();
}

bool askTableSize(QWidget *parent, int *rows, int *columns)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Insert Table"));
    auto *form = new QFormLayout(&dialog);
    auto *rowBox = new QSpinBox(&dialog);
    rowBox->setRange(2, 200);
    rowBox->setValue(qBound(2, *rows, 200));
    rowBox->setToolTip(QObject::tr("Including the header row"));
    auto *columnBox = new QSpinBox(&dialog);
    columnBox->setRange(1, 50);
    columnBox->setValue(qBound(1, *columns, 50));
    form->addRow(QObject::tr("&Rows:"), rowBox);
    form->addRow(QObject::tr("&Columns:"), columnBox);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    rowBox->selectAll();
    if (dialog.exec() != QDialog::Accepted)
        return false;
    *rows = rowBox->value();
    *columns = columnBox->value();
    return true;
}

QMenu *addInsertTableMenu(QMenu *menu, const std::function<void(int, int)> &insert, QAction *custom)
{
    QMenu *sub = menu->addMenu(QObject::tr("Insert &Table"));
    auto *grid = new QWidgetAction(sub);
    auto *picker = new TableSizePicker(sub);
    grid->setDefaultWidget(picker);
    sub->addAction(grid);
    // Pointing at the grid again starts from nothing.
    QObject::connect(sub, &QMenu::aboutToShow, picker, [picker] { picker->setSelection(0, 0); });
    QObject::connect(picker, &TableSizePicker::picked, sub, [sub, insert](int rows, int columns) {
        // Close the menus as a chosen item does; the table goes in once
        // they are gone (and the focus is back).
        for (QMenu *m = sub; m; m = qobject_cast<QMenu *>(m->parentWidget()))
            m->hide();
        QTimer::singleShot(0, [insert, rows, columns] { insert(rows, columns); });
    });
    sub->addSeparator();
    if (!custom) {
        custom = sub->addAction(QObject::tr("Custom &Size…"));
        QObject::connect(custom, &QAction::triggered, sub, [sub, insert] {
            int rows = 3;
            int columns = 3;
            if (askTableSize(sub->parentWidget(), &rows, &columns))
                insert(rows, columns);
        });
    } else {
        sub->addAction(custom);
    }
    return sub;
}

} // namespace vn
