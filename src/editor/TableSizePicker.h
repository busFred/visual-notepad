// "Insert Table" as in a word processor: a grid of squares in a menu, where
// pointing at a square picks that many rows and columns, plus "Custom
// Size..." for a table larger than the grid (a dialog with two numbers).
#pragma once

#include <QWidget>

#include <functional>

class QMenu;

namespace vn {

class TableSizePicker : public QWidget
{
    Q_OBJECT

public:
    // Rows count the header row, as in the dialog.
    static constexpr int gridRows = 8;
    static constexpr int gridColumns = 10;

    explicit TableSizePicker(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    // The size under the pointer (or chosen with the arrow keys); 0 x 0
    // when there is none.
    int rows() const { return m_rows; }
    int columns() const { return m_cols; }
    void setSelection(int rows, int columns);

Q_SIGNALS:
    void picked(int rows, int columns);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QRect cellRect(int row, int col) const;
    int labelHeight() const;

    int m_rows = 0;
    int m_cols = 0;
};

// Asks for a table size with a dialog; false when cancelled.
bool askTableSize(QWidget *parent, int *rows, int *columns);

// Adds an "Insert Table" submenu to `menu`: the grid, then "Custom Size...".
// `insert` gets the size picked. `custom` is the action that asks for the
// size (the menu bar's command, so its shortcut shows); null makes one.
QMenu *addInsertTableMenu(QMenu *menu, const std::function<void(int rows, int columns)> &insert,
                          QAction *custom = nullptr);

} // namespace vn
