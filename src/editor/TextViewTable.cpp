// Pipe table commands of TextView: the table around the caret is rewritten
// by the Rust core (see core/src/table.rs), moving between cells with Tab,
// adding rows with Enter, and inserting a new table. Each command is one undo
// step and does nothing in a read-only view or outside a table.
#include "TextView.h"

#include <QMenu>
#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>
#include <optional>

namespace vn {

namespace {

// Whether a line can be a row of a pipe table: a pipe after list or quote
// markers at most, and a pipe at the end. Takes everything the core takes
// for a row (format.rs, row_parts) and a little more, which is all that is
// needed: the core decides on the lines handed to it.
bool rowLike(const QString &text)
{
    const qsizetype bar = text.indexOf(u'|');
    if (bar < 0)
        return false;
    for (qsizetype i = 0; i < bar; ++i) {
        const QChar c = text.at(i);
        if (!(c == u' ' || c == u'>' || c == u'-' || c == u'*' || c == u'+' || c == u'.' || c == u')' || c.isDigit()))
            return false;
    }
    qsizetype end = text.size();
    while (end > bar + 1 && text.at(end - 1).isSpace())
        --end;
    return end > bar + 1 && text.at(end - 1) == u'|';
}

} // namespace

std::optional<TextView::FenceMark> TextView::fenceMarkOf(int line, const QString &text)
{
    qsizetype i = 0;
    while (i < text.size() && (text.at(i) == u' ' || text.at(i) == u'>'))
        ++i;
    if (i >= text.size())
        return std::nullopt;
    const QChar ch = text.at(i);
    if (ch != u'`' && ch != u'~')
        return std::nullopt;
    qsizetype run = i;
    while (run < text.size() && text.at(run) == ch)
        ++run;
    if (run - i < 3)
        return std::nullopt;
    bool closes = true;
    for (qsizetype k = run; k < text.size() && closes; ++k)
        closes = text.at(k).isSpace();
    return FenceMark{line, ch, int(run - i), closes};
}

void TextView::rebuildFenceMarks()
{
    m_fenceMarks.clear();
    for (QTextBlock block = m_doc->begin(); block.isValid(); block = block.next()) {
        if (const auto mark = fenceMarkOf(block.blockNumber(), block.text()))
            m_fenceMarks.push_back(*mark);
    }
    m_fenceMarksValid = true;
}

void TextView::spliceFenceMarks(int first, int oldAffected, int newAffected)
{
    // Lines [first, first + oldAffected) were replaced by newAffected lines.
    if (!m_fenceMarksValid)
        return;
    const auto byLine = [](const FenceMark &mark, int line) { return mark.line < line; };
    const auto lo = std::lower_bound(m_fenceMarks.begin(), m_fenceMarks.end(), first, byLine);
    const auto hi = std::lower_bound(lo, m_fenceMarks.end(), first + oldAffected, byLine);
    const auto at = m_fenceMarks.erase(lo, hi);
    for (auto it = at; it != m_fenceMarks.end(); ++it)
        it->line += newAffected - oldAffected;
    std::vector<FenceMark> fresh;
    QTextBlock block = m_doc->findBlockByNumber(first);
    for (int n = 0; n < newAffected && block.isValid(); ++n, block = block.next()) {
        if (const auto mark = fenceMarkOf(block.blockNumber(), block.text()))
            fresh.push_back(*mark);
    }
    m_fenceMarks.insert(at, fresh.begin(), fresh.end());
}

bool TextView::inFencedCode(int line)
{
    if (!m_fenceMarksValid)
        rebuildFenceMarks();
    // The core's rules (format.rs): a fence opens at a mark and closes at
    // the next mark of the same character, at least as long, on its own.
    const FenceMark *open = nullptr;
    for (const FenceMark &mark : m_fenceMarks) {
        if (mark.line >= line)
            break;
        if (!open)
            open = &mark;
        else if (mark.ch == open->ch && mark.run >= open->run && mark.closes)
            open = nullptr;
    }
    return open != nullptr;
}

TextView::TableWindow TextView::tableWindow(int line)
{
    TableRun &run = m_tableRun;
    if (run.firstLine < 0 || line < run.firstLine || line >= run.firstLine + run.lines.size()) {
        run = {};
        const QTextBlock block = m_doc->findBlockByNumber(line);
        if (!block.isValid() || !rowLike(block.text()))
            return {};
        // (Each line's text is made once: that is most of the cost.)
        for (QTextBlock b = block.previous(); b.isValid(); b = b.previous()) {
            const QString text = b.text();
            if (!rowLike(text))
                break;
            run.lines.prepend(text);
        }
        run.firstLine = line - int(run.lines.size());
        for (QTextBlock b = block; b.isValid(); b = b.next()) {
            const QString text = b.text();
            if (!rowLike(text))
                break;
            run.lines.append(text);
        }
        run.text = run.lines.join(u'\n');
    }
    if (inFencedCode(run.firstLine))
        return {};
    return {run.text, run.firstLine};
}

void TextView::spliceTableRun(int first, int oldAffected, int newAffected)
{
    // Lines [first, first + oldAffected) were replaced by newAffected lines.
    TableRun &run = m_tableRun;
    if (run.firstLine < 0)
        return;
    const int count = int(run.lines.size());
    const int end = first + oldAffected;
    if (end < run.firstLine) {
        run.firstLine += newAffected - oldAffected; // Above it: it moved.
        return;
    }
    if (first > run.firstLine + count)
        return; // Below it.
    // Inside it (not at the lines next to it, which may have joined it):
    // the changed lines take the place of the old ones, if they are still
    // rows. Else the run is made again when next asked for.
    if (first < run.firstLine || end > run.firstLine + count) {
        run = {};
        return;
    }
    QStringList fresh;
    QTextBlock block = m_doc->findBlockByNumber(first);
    for (int n = 0; n < newAffected && block.isValid(); ++n, block = block.next()) {
        const QString text = block.text();
        if (!rowLike(text)) {
            run = {};
            return;
        }
        fresh.append(text);
    }
    const int at = first - run.firstLine;
    run.lines.remove(at, oldAffected);
    for (int n = 0; n < fresh.size(); ++n)
        run.lines.insert(at + n, fresh.at(n));
    run.text = run.lines.join(u'\n');
}

void TextView::setTableTarget(int line, int row, int col)
{
    m_tableTarget = {line, row, col};
}

md::TableCell TextView::tableCellOf(int line, int row, int col)
{
    const TableWindow window = tableWindow(line);
    if (window.firstLine < 0)
        return {};
    md::TableCell cell = md::tableCell(window.text, line - window.firstLine, row, col);
    if (cell.found)
        cell.line += window.firstLine;
    return cell;
}

md::TableInfo TextView::tableInfoAt(int line, int column)
{
    const TableWindow window = tableWindow(line);
    if (window.firstLine < 0)
        return {};
    md::TableInfo info = md::tableInfo(window.text, line - window.firstLine, column);
    if (info.found) {
        info.firstLine += window.firstLine;
        info.lastLine += window.firstLine;
    }
    return info;
}

md::TableInfo TextView::tableAtCaret()
{
    if (m_tableTarget.line >= 0 && m_markdown) {
        const md::TableCell cell = tableCellOf(m_tableTarget.line, m_tableTarget.row, m_tableTarget.col);
        return cell.found ? tableInfoAt(cell.line, cell.start) : md::TableInfo();
    }
    return caretTableInfo();
}

md::TableInfo TextView::caretTableInfo()
{
    if (!m_markdown || m_carets.empty())
        return {};
    const QTextCursor c = textCursor();
    if (!rowLike(c.block().text()))
        return {};
    // (Asked after every caret move, and the same several times.)
    const int revision = m_doc->revision();
    if (m_tableInfoRevision != revision || m_tableInfoPosition != c.position()) {
        m_tableInfo = tableInfoAt(c.blockNumber(), c.positionInBlock());
        m_tableInfoRevision = revision;
        m_tableInfoPosition = c.position();
    }
    return m_tableInfo;
}

bool TextView::editTable(md::TableOp op, const QString &arg)
{
    if (!canEdit())
        return false;
    if (m_tableTarget.line >= 0) {
        const TableTarget target = m_tableTarget;
        tableTargetEditing();
        return editTableAt(target.line, op, target.row, target.col, arg, false);
    }
    const md::TableInfo info = tableAtCaret();
    if (!info.found)
        return false;
    return editTableAt(textCursor().blockNumber(), op, info.row, info.col, arg);
}

bool TextView::editTableAt(int line, md::TableOp op, int row, int col, const QString &arg, bool moveCaret)
{
    if (!m_markdown || !canEdit())
        return false;
    const TableWindow window = tableWindow(line);
    if (window.firstLine < 0)
        return false;
    md::TableEdit edit = md::tableEdit(window.text, line - window.firstLine, op, row, col, arg);
    if (!edit.ok)
        return false;
    edit.firstLine += window.firstLine;
    edit.lastLine += window.firstLine;
    edit.caretLine += window.firstLine;
    const QTextBlock first = m_doc->findBlockByNumber(edit.firstLine);
    const QTextBlock last = m_doc->findBlockByNumber(edit.lastLine);
    if (!first.isValid() || !last.isValid())
        return false;

    int from = first.position();
    int to = last.position() + last.length() - 1;
    if (edit.remove) {
        // Take a line break along: the one after, else the one before.
        if (last.next().isValid())
            to = last.next().position();
        else if (first.previous().isValid())
            from = first.previous().position() + first.previous().length() - 1;
    }
    if (edit.changed) {
        beginEdit();
        replaceRange(from, to, edit.remove ? QString() : edit.replacement);
        endEdit();
    }

    if (!moveCaret)
        return true;
    QTextBlock target = m_doc->findBlockByNumber(edit.caretLine);
    if (!target.isValid())
        target = m_doc->lastBlock();
    QTextCursor cursor(m_doc);
    cursor.setPosition(target.position() + qMin(edit.caretColumn, target.length() - 1));
    setTextCursor(cursor);
    ensureCursorVisible();
    return true;
}

void TextView::setRealignTablesOnLeave(bool on)
{
    m_realignTables = on;
    m_tableTracked = false;
}

void TextView::realignLeftTable()
{
    if (!m_realignTables || !m_markdown || !canEdit() || m_dragging)
        return;
    const md::TableInfo now = caretTableInfo();
    if (m_tableTracked) {
        const int line = m_tableCursor.blockNumber();
        if (now.found && line >= now.firstLine && line <= now.lastLine) {
            m_tableCursor = textCursor();
            return; // Still in it.
        }
        m_tableTracked = false;
        // Left it. Realign if it was edited meanwhile; the caret stays put.
        if (m_doc->revision() != m_tableEnterRevision)
            editTableAt(line, md::TableOp::Realign, 0, 0, {}, false);
    }
    if (now.found) {
        m_tableCursor = textCursor();
        m_tableTracked = true;
        m_tableEnterRevision = m_doc->revision();
    }
}

bool TextView::moveToTableCell(int line, int row, int col)
{
    const md::TableCell cell = tableCellOf(line, row, col);
    QTextCursor cursor(m_doc);
    if (cell.found) {
        const QTextBlock block = m_doc->findBlockByNumber(cell.line);
        cursor.setPosition(block.position() + cell.start);
        // The text of a cell is selected, so typing replaces it.
        cursor.setPosition(block.position() + cell.end, QTextCursor::KeepAnchor);
    } else {
        // A short row has no such cell: go to the end of its line.
        const md::TableInfo info = tableInfoAt(line, 0);
        if (!info.found)
            return false;
        const QTextBlock block = m_doc->findBlockByNumber(info.firstLine + (row == 0 ? 0 : row + 1));
        if (!block.isValid())
            return false;
        cursor.setPosition(block.position() + block.length() - 1);
    }
    setTextCursor(cursor);
    ensureCursorVisible();
    return true;
}

// One caret, with no selection or one inside a line (a cell Tab selected).
static bool onOneLine(const QTextCursor &c)
{
    return c.document()->findBlock(c.selectionStart()) == c.document()->findBlock(c.selectionEnd());
}

bool TextView::tableTab(bool forward)
{
    if (!m_markdown || !canEdit() || m_carets.size() != 1 || !onOneLine(textCursor()))
        return false;
    const md::TableInfo info = caretTableInfo();
    if (!info.found)
        return false;
    const int line = textCursor().blockNumber();
    int row = info.row;
    int col = info.col;
    if (forward) {
        if (col + 1 < info.cols) {
            ++col;
        } else if (row + 1 < info.rows) {
            ++row;
            col = 0;
        } else {
            // Past the last cell: a new row, as in a word processor.
            editTableAt(line, md::TableOp::RowBelow, row, 0);
            return true;
        }
    } else if (col > 0) {
        --col;
    } else if (row > 0) {
        --row;
        col = info.cols - 1;
    } else {
        return true; // Already in the first cell.
    }
    moveToTableCell(line, row, col);
    return true;
}

bool TextView::tableEnter()
{
    if (!m_markdown || !canEdit() || m_carets.size() != 1 || !onOneLine(textCursor()))
        return false;
    const md::TableInfo info = caretTableInfo();
    if (!info.found)
        return false;
    return editTableAt(textCursor().blockNumber(), md::TableOp::RowBelow, info.row, info.col);
}

void TextView::insertTable(int rows, int cols)
{
    if (!m_markdown || !canEdit())
        return;
    rows = qBound(2, rows, 200);
    cols = qBound(1, cols, 50);

    QStringList names;
    int width = 0;
    for (int i = 1; i <= cols; ++i) {
        names << tr("Column %1").arg(i);
        width = qMax(width, int(names.last().size()));
    }
    const auto line = [&](const QStringList &cells) { return QStringLiteral("| ") + cells.join(QStringLiteral(" | ")) + QStringLiteral(" |"); };
    QStringList header;
    QStringList delimiter;
    QStringList blank;
    for (const QString &name : std::as_const(names)) {
        header << name.leftJustified(width);
        delimiter << QString(width, u'-');
        blank << QString(width, u' ');
    }
    QStringList lines{line(header), line(delimiter)};
    for (int i = 1; i < rows; ++i)
        lines << line(blank);

    beginEdit();
    QTextCursor c = textCursor();
    const QTextBlock block = c.block();
    QString text = lines.join(u'\n');
    int start;
    if (block.text().trimmed().isEmpty()) {
        start = block.position();
        c.setPosition(start);
        c.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
    } else {
        // After the line, set apart from it: a table cannot follow text directly.
        c.setPosition(block.position() + block.length() - 1);
        text.prepend(QStringLiteral("\n\n"));
        start = c.position() + 2;
    }
    const QTextBlock next = block.next();
    if (next.isValid() && !next.text().trimmed().isEmpty())
        text.append(u'\n');
    c.insertText(text);
    endEdit();

    // The first header, selected for typing over.
    QTextCursor selection(m_doc);
    selection.setPosition(start + 2);
    selection.setPosition(start + 2 + int(names.first().size()), QTextCursor::KeepAnchor);
    setTextCursor(selection);
    ensureCursorVisible();
}

void TextView::addTableMenu(QMenu *menu)
{
    populateTableMenu(menu->addMenu(tr("Table")));
}

void TextView::populateTableMenu(QMenu *table)
{
    using Op = md::TableOp;
    const md::TableInfo info = tableAtCaret();
    const bool editable = canEdit();
    const auto add = [&](QMenu *to, const QString &title, Op op) {
        QAction *action = to->addAction(title, this, [this, op] { editTable(op); });
        action->setEnabled(editable && md::tableOpAvailable(info, op));
    };
    add(table, tr("Insert Row Above"), Op::RowAbove);
    add(table, tr("Insert Row Below"), Op::RowBelow);
    add(table, tr("Insert Column Left"), Op::ColumnLeft);
    add(table, tr("Insert Column Right"), Op::ColumnRight);
    table->addSeparator();
    add(table, tr("Delete Row"), Op::DeleteRow);
    add(table, tr("Delete Column"), Op::DeleteColumn);
    add(table, tr("Delete Table"), Op::DeleteTable);
    table->addSeparator();
    add(table, tr("Move Row Up"), Op::MoveRowUp);
    add(table, tr("Move Row Down"), Op::MoveRowDown);
    add(table, tr("Move Column Left"), Op::MoveColumnLeft);
    add(table, tr("Move Column Right"), Op::MoveColumnRight);
    table->addSeparator();
    QMenu *align = table->addMenu(tr("Align Column"));
    align->setEnabled(editable && info.found);
    const struct
    {
        const char *title;
        Op op;
        md::TableAlignment alignment;
    } alignments[] = {{QT_TR_NOOP("Default"), Op::AlignNone, md::TableAlignment::None},
                      {QT_TR_NOOP("Left"), Op::AlignLeft, md::TableAlignment::Left},
                      {QT_TR_NOOP("Center"), Op::AlignCenter, md::TableAlignment::Center},
                      {QT_TR_NOOP("Right"), Op::AlignRight, md::TableAlignment::Right}};
    for (const auto &a : alignments) {
        add(align, tr(a.title), a.op);
        align->actions().last()->setCheckable(true);
        align->actions().last()->setChecked(info.found && info.alignment == a.alignment);
    }
    add(table, tr("Sort Ascending"), Op::SortAscending);
    add(table, tr("Sort Descending"), Op::SortDescending);
    add(table, tr("Transpose"), Op::Transpose);
    add(table, tr("Realign"), Op::Realign);
}

} // namespace vn
