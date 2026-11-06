#include "TableCellEditor.h"

#include <QAbstractTextDocumentLayout>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QTextBlock>
#include <QTextLayout>

namespace vn {

TableCellEditor::TableCellEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setObjectName(QStringLiteral("TableCellEditor"));
    setFrameShape(QFrame::Box);
    setLineWrapMode(WidgetWidth);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTabChangesFocus(false);
    document()->setDocumentMargin(4);
    connect(this, &QPlainTextEdit::textChanged, this, &TableCellEditor::fit);
}

void TableCellEditor::open(const QString &text)
{
    m_done = false;
    setPlainText(text);
    moveCursor(QTextCursor::End);
}

void TableCellEditor::place(const QPoint &topLeft, int width, int minHeight)
{
    m_minHeight = minHeight;
    move(topLeft);
    resize(width, minHeight);
    fit();
}

void TableCellEditor::fit()
{
    // The text's height, found with the layout of the width now: lines are
    // not counted before they are laid out.
    document()->setTextWidth(viewport()->width());
    const int height = int(document()->size().height()) + 2 * frameWidth() + 2;
    resize(width(), qMax(m_minHeight, height));
}

void TableCellEditor::keyPressEvent(QKeyEvent *event)
{
    const auto mods = event->modifiers() & ~Qt::KeypadModifier;
    const auto finish = [this, event](Exit how) {
        event->accept();
        if (m_done)
            return;
        m_done = true;
        Q_EMIT finished(how);
    };
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (mods == Qt::NoModifier)
            return finish(Exit::Commit);
        break;
    case Qt::Key_Escape:
        return finish(Exit::Cancel);
    case Qt::Key_Tab:
        if (mods == Qt::NoModifier)
            return finish(Exit::Next);
        break;
    case Qt::Key_Backtab:
        return finish(Exit::Previous);
    case Qt::Key_Up:
    case Qt::Key_Down: {
        if (mods != Qt::NoModifier)
            break;
        // Only from the first (last) line of the text, wrapped lines included.
        const QTextCursor c = textCursor();
        const QTextLayout *layout = c.block().layout();
        const QTextLine line = layout->lineForTextPosition(c.positionInBlock());
        const bool up = event->key() == Qt::Key_Up;
        const bool edge = up ? (!c.block().previous().isValid() && line.lineNumber() == 0)
                             : (!c.block().next().isValid() && line.lineNumber() == layout->lineCount() - 1);
        if (edge)
            return finish(up ? Exit::Up : Exit::Down);
        break;
    }
    default:
        break;
    }
    QPlainTextEdit::keyPressEvent(event);
}

void TableCellEditor::focusOutEvent(QFocusEvent *event)
{
    QPlainTextEdit::focusOutEvent(event);
    // A menu of this box (copy, paste) takes the focus for a moment.
    if (event->reason() == Qt::PopupFocusReason || m_done)
        return;
    m_done = true;
    Q_EMIT finished(Exit::FocusLost);
}

} // namespace vn
