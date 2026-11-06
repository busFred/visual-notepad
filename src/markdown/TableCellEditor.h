// The editor shown over one cell of a rendered table: the cell's Markdown
// source in a small text box, which grows with the text. It ends with a key
// (Enter, Tab, Esc, Up, Down) or by losing the focus, and says how.
#pragma once

#include <QPlainTextEdit>

namespace vn {

class TableCellEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    enum class Exit {
        Commit,    // Enter
        Cancel,    // Esc
        Next,      // Tab
        Previous,  // Shift+Tab
        Up,        // Up on the first line
        Down,      // Down on the last line
        FocusLost,
    };
    explicit TableCellEditor(QWidget *parent = nullptr);

    // Puts the text in, caret at the end.
    void open(const QString &text);
    // Sets the width and the top-left; the height follows the text (at least
    // `minHeight`).
    void place(const QPoint &topLeft, int width, int minHeight);

Q_SIGNALS:
    void finished(vn::TableCellEditor::Exit how);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    void fit();

    int m_minHeight = 0;
    bool m_done = false;
};

} // namespace vn
