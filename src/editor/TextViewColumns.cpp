// Columns of the source view (see TextView::setColumnsShown). Which part of
// which line each column shows, and how the moves go, is md::ReadFlow's, as
// in read mode; here are the rows it reads, where the columns go, the
// painting, and the keys, wheel, buttons and scroll bars that move them.
//
// The text edits in columns as anywhere: an edit renumbers the rows
// (columnsRowsChanged), which fills the screen again from the same place, so
// a new or deleted line moves text over the column breaks. The caret going
// past the last column moves the view on by one column (columnsShift), not a
// page: the lines just above stay in sight. Each column keeps its own
// sideways offset (Columns::offsets), which goes along with its text.
#include "TextViewColumns.h"

#include "../Settings.h"
#include "../Theme.h"
#include "../markdown/BlockRenderer.h"
#include "ReadControls.h"

#include <QFontInfo>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <optional>

namespace vn {

namespace {

// Room under the lines for the scroll bar of a column with a line too long
// for it (Word Wrap off): the app's scroll bar height and a little air.
constexpr int scrollBarHeight = 12;
constexpr int scrollBarRoom = scrollBarHeight + 4;
constexpr int gutterRightPad = 14; // As TextView.cpp's.

// The line top before a place (a wrapped row of the same line, or the last
// row of the line before), if there is one.
std::optional<md::Position> lineBefore(md::RowSource &rows, md::Position position)
{
    if (position.y > 0 && position.row < rows.count()) {
        const QList<int> tops = rows.lineTops(position.row);
        const auto it = std::lower_bound(tops.begin(), tops.end(), position.y);
        return md::Position{position.row, it != tops.begin() ? *(it - 1) : 0};
    }
    if (position.row <= 0)
        return std::nullopt;
    const int row = qMin(position.row, rows.count()) - 1;
    const QList<int> tops = rows.lineTops(row);
    return md::Position{row, tops.isEmpty() ? 0 : tops.last()};
}

} // namespace

// ---------------------------------------------------------------------------
// The rows as the flow reads them

int TextView::ColumnRows::count()
{
    return m_view->m_doc->blockCount();
}

int TextView::ColumnRows::height(int row)
{
    QTextLayout *layout = m_view->layoutForLine(row);
    return (layout ? qMax(1, layout->lineCount()) : 1) * m_view->m_lineHeight;
}

int TextView::ColumnRows::rowForLine(int line)
{
    return qBound(0, line, count() - 1);
}

QList<int> TextView::ColumnRows::lineTops(int row)
{
    QTextLayout *layout = m_view->layoutForLine(row);
    const int lines = layout ? qMax(1, layout->lineCount()) : 1;
    QList<int> tops;
    tops.reserve(lines);
    for (int i = 0; i < lines; ++i)
        tops.append(i * m_view->m_lineHeight);
    return tops;
}

QList<int> TextView::ColumnRows::cuts(int row)
{
    QList<int> tops = lineTops(row);
    tops.removeFirst();
    return tops;
}

// ---------------------------------------------------------------------------
// Mode and layout

TextView::~TextView() = default;

TextView::Columns &TextView::columnState()
{
    if (!m_columns) {
        m_columns = std::make_unique<Columns>(this);
        const auto keep = [this] { m_columns->flow.setKeptLines(Settings::instance().number(key::readKeptLines)); };
        keep();
        connect(&Settings::instance(), &Settings::changed, this, [this, keep](const QString &changed) {
            if (changed == QLatin1String(key::readKeptLines)) {
                keep();
                viewport()->update();
            }
        });
    }
    return *m_columns;
}

void TextView::setColumnsShown(bool on, int startLine)
{
    if (m_columnsOn == on) {
        if (on && startLine >= 0) {
            m_columns->startLine = startLine;
            m_columns->needsStart = true;
            viewport()->update();
        }
        return;
    }
    int top = 0;
    if (on) {
        columnState();
        top = qBound(0, startLine >= 0 ? startLine : topLine(), m_doc->blockCount() - 1);
        m_columns->startLine = top;
        m_columns->needsStart = true;
        m_columnsFirst = m_columnsLast = top;
    } else {
        top = m_columnsFirst;
    }
    m_columnsOn = on;
    m_columns->offsets.clear();
    m_columns->barDrag = {};
    m_columns->wheel = 0;
    m_dragging = false;
    m_columnDragging = false;
    m_autoScroll.stop();
    horizontalScrollBar()->setValue(0);
    // Lines wrap at another width: the column's, or the view's again.
    relayout();
    setScrollBarsHidden(on);
    if (!on)
        scrollToLine(top);
    placeColumnControls();
    noteColumns();
}

void TextView::setColumnCount(int columns)
{
    columns = qMax(0, columns);
    columnState();
    if (m_columns->wanted == columns)
        return;
    m_columns->wanted = columns;
    if (m_columnsOn) {
        placeColumnControls();
        viewport()->update();
        noteColumns();
    }
}

void TextView::setColumnChars(int chars)
{
    chars = qBound(20, chars, 400);
    columnState();
    if (m_columns->chars == chars)
        return;
    m_columns->chars = chars;
    if (m_columnsOn) {
        placeColumnControls();
        viewport()->update();
        noteColumns();
    }
}

int TextView::shownColumnCount()
{
    return m_columnsOn ? columnLayout().count : 1;
}

int TextView::maxColumnCount()
{
    return m_columnsOn ? columnLayout().fit : 1;
}

// A character of text: the editor font's (one width for every character in a
// monospaced font, the average otherwise).
qreal TextView::columnsCharWidth() const
{
    if (QFontInfo(font()).fixedPitch())
        return m_charWidth;
    return QFontMetricsF(font()).averageCharWidth();
}

const TextView::ColumnLayout &TextView::columnLayout()
{
    ColumnLayout &l = m_columns->layout;
    const QSize size = viewport()->size();
    const qreal charWidth = columnsCharWidth();
    l = {};
    const int room = qMax(1, size.width() - 2 * columns::sideRoom);
    l.gap = columns::gap;
    l.textLeft = gutterWidth() + qRound(m_leftMargin);
    l.width = qBound(1, qRound(m_columns->chars * charWidth), qMax(1, room - l.textLeft));
    l.fit = qMax(1, (room + l.gap) / (l.columnWidth() + l.gap));
    l.count = m_columns->wanted > 0 ? qMin(m_columns->wanted, l.fit) : l.fit;
    const int total = l.count * l.columnWidth() + (l.count - 1) * l.gap;
    l.left = (size.width() - total) / 2;
    l.top = columns::topRoom;
    l.height = qMax(m_lineHeight * 2,
                    size.height() - columns::topRoom - columns::bottomRoom - (m_wordWrap ? 0 : scrollBarRoom));
    return l;
}

void TextView::columnsRowsChanged(bool renumbered)
{
    if (!m_columns)
        return;
    if (renumbered)
        ++m_columns->revision;
    m_columns->dirty = true;
}

// ---------------------------------------------------------------------------
// The flow

const md::Screen &TextView::columnsScreen()
{
    static const md::Screen none;
    if (!m_columnsOn)
        return none;
    Columns &c = *m_columns;
    const ColumnLayout &l = columnLayout();
    md::Geometry geometry;
    geometry.columns = l.count;
    geometry.height = l.height;
    geometry.width = l.width;
    geometry.gap = l.gap;
    geometry.revision = c.revision;
    const md::Geometry &was = c.flow.geometry();
    // Another layout (not just other rows): what was wide may not be.
    const bool layoutChanged = was.columns != geometry.columns || was.width != geometry.width
                               || was.height != geometry.height;
    if (c.needsStart) {
        c.needsStart = false;
        c.dirty = false;
        if (!(was.key() == geometry.key()))
            c.flow.setGeometry(geometry);
        c.flow.goToLine(c.startLine);
        c.offsets.clear();
    } else if (!(was.key() == geometry.key())) {
        c.dirty = false;
        const md::Position start = c.flow.screen().start;
        c.flow.setGeometry(geometry);
        if (layoutChanged) {
            c.offsets.clear();
        } else if (start.y > 0 && c.flow.screen().start == md::Position{start.row, 0}
                   && start.y < c.rows.height(start.row)) {
            // An edit, and the first column starts part-way into a wrapped
            // line (the view moved on by a column): it still does there, or
            // the caret's line could drop off the end.
            c.flow.goTo(start);
        }
    } else if (c.dirty) {
        c.dirty = false;
        c.flow.reflow();
    } else {
        return c.flow.screen();
    }
    noteColumns();
    return c.flow.screen();
}

void TextView::columnsMoved(bool keepOffsets)
{
    if (!keepOffsets)
        m_columns->offsets.clear();
    noteColumns();
    viewport()->update();
    Q_EMIT scrolled();
}

// Keeps what the const queries read in step with the screen, and tells the
// tab (once the event or paint that changed it is done).
void TextView::noteColumns()
{
    if (!m_columns)
        return;
    Columns &c = *m_columns;
    if (m_columnsOn && !c.flow.screen().empty() && !c.needsStart) {
        const md::LineRange range = c.flow.lineRange();
        m_columnsFirst = range.first;
        m_columnsLast = range.last;
        // The scrolling view starts here (and a saved session has it).
        m_topRow = qBound(0, range.first, qMax(0, rowCount() - 1));
        m_topOffset = 0;
    }
    updateColumnControls();
    if (c.newsPending)
        return;
    c.newsPending = true;
    QTimer::singleShot(0, this, [this] {
        Columns &c = *m_columns;
        Columns::News news;
        news.shown = m_columnsOn;
        if (m_columnsOn) {
            // (Brings the screen up to date first: this is the news.)
            const std::pair<int, int> range = columnLineRange();
            news.first = range.first;
            news.last = range.second;
            news.fit = columnLayout().fit;
            news.count = columnLayout().count;
        }
        c.newsPending = false;
        placeColumnControls(); // (The columns may have moved: a zoom, a font.)
        if (news == c.news)
            return;
        c.news = news;
        Q_EMIT columnsChanged();
    });
}

std::pair<int, int> TextView::columnLineRange()
{
    if (!m_columnsOn || columnsScreen().empty())
        return {-1, -1};
    const md::LineRange range = m_columns->flow.lineRange();
    return {range.first, range.last};
}

void TextView::columnsPageForward()
{
    columnsPage(true, false, false);
}

void TextView::columnsPageBack()
{
    columnsPage(false, false, false);
}

// A page turn; with carry, the carets go along by as many lines as the
// screen did (Page Up and Page Down move the caret, as when scrolling).
void TextView::columnsPage(bool forward, bool carry, bool select)
{
    if (!m_columnsOn || columnsScreen().empty())
        return;
    Columns &c = *m_columns;
    const int firstBefore = c.flow.lineRange().first;
    const bool caretShown = !columnsCaretRect(textCursor()).isNull();
    const md::Position startBefore = c.flow.screen().start;
    forward ? c.flow.pageForward() : c.flow.pageBack();
    columnsMoved(false);
    if (!carry)
        return;
    if (c.flow.screen().start == startBefore) {
        // Nowhere to turn to: to the end (the start), as in a scrolling view.
        moveCarets(forward ? QTextCursor::End : QTextCursor::Start, select);
        return;
    }
    if (!caretShown)
        return; // A caret elsewhere stays there.
    const md::LineRange after = c.flow.lineRange();
    const int delta = after.first - firstBefore;
    QList<QTextCursor> moved;
    for (const QTextCursor &caret : cursors()) {
        const int line = qBound(after.first, caret.blockNumber() + delta, after.last);
        const QTextBlock block = m_doc->findBlockByNumber(line);
        QTextCursor cursor = caret;
        if (!select)
            cursor.clearSelection();
        cursor.setPosition(block.position() + qMin(caret.positionInBlock(), block.length() - 1),
                           select ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
        moved.append(cursor);
    }
    setCursors(moved, m_primary);
}

void TextView::columnsNudge(int lines)
{
    if (!m_columnsOn || lines == 0 || columnsScreen().empty())
        return;
    m_columns->flow.nudge(lines);
    columnsMoved(true);
}

void TextView::columnsToStart()
{
    if (!m_columnsOn || columnsScreen().empty())
        return;
    m_columns->flow.toStart();
    columnsMoved(false);
}

void TextView::columnsToEnd()
{
    if (!m_columnsOn || columnsScreen().empty())
        return;
    m_columns->flow.toEnd();
    columnsMoved(false);
}

// The caret's line on screen. The line after the screen moves the view on by
// one column (column 2's lines go into column 1, the caret's line is at the top
// of the last column), the line before it back by one: the lines around the
// caret stay in sight, as they would in a scrolling view. A caret farther
// away brings the screen that starts with its line (the last screen, if that
// one ends the text). Then, with Word Wrap off, its column scrolls sideways
// to it.
void TextView::columnsEnsureCursorVisible()
{
    if (columnsScreen().empty()) {
        viewport()->update();
        return;
    }
    Columns &c = *m_columns;
    const QTextCursor caret = textCursor();
    if (columnsCaretRect(caret).isNull()) {
        QTextLayout *layout = layoutForLine(caret.blockNumber());
        const QTextLine line = layout ? layout->lineForTextPosition(caret.positionInBlock()) : QTextLine();
        const md::Position place{caret.blockNumber(), line.isValid() ? line.lineNumber() * m_lineHeight : 0};
        const md::Screen &screen = c.flow.screen();
        bool shifted = false;
        if (place == screen.end)
            shifted = columnsShift(1);
        else if (place == lineBefore(c.rows, screen.start))
            shifted = columnsShift(-1);
        if (!shifted || columnsCaretRect(caret).isNull()) {
            c.flow.goTo(place);
            if (c.flow.screen().atEnd) // (Not a screen with its end empty.)
                c.flow.toEnd();
            columnsMoved(false);
        }
    }
    if (!m_wordWrap) {
        int column = 0;
        if (!columnsCaretRect(caret, &column).isNull()) {
            const ColumnLayout &l = columnLayout();
            const qreal x = cursorX(caret);
            const qreal offset = columnOffset(column);
            const qreal charWidth = columnsCharWidth();
            if (x < offset)
                setColumnOffset(column, qMax<qreal>(0, x - charWidth * 4));
            else if (x > offset + l.width - charWidth)
                setColumnOffset(column, x - l.width + charWidth * 4);
        }
    }
    viewport()->update();
}

// Column 2's lines go into column 1 (forward), or column 1's into column 2,
// and each column's sideways offset goes along with its lines; a column that
// comes in starts at the lines' start. With one column, a line: a column
// would be a page, and the page turn is what this is not.
bool TextView::columnsShift(int direction)
{
    if (!m_columnsOn || direction == 0 || columnsScreen().empty())
        return false;
    Columns &c = *m_columns;
    const md::Screen &screen = c.flow.screen();
    const int count = int(screen.columns.size());
    if (direction > 0 ? screen.atEnd : screen.atStart)
        return false;
    std::vector<qreal> &offsets = c.offsets;
    if (count < 2) {
        const md::Position before = screen.start;
        if (c.flow.nudge(direction).start == before)
            return false;
    } else if (direction > 0) {
        const std::vector<md::Segment> &next = screen.columns[1].segments;
        if (next.empty())
            return false;
        c.flow.goTo({next.front().row, next.front().yFrom});
        if (!offsets.empty())
            offsets.erase(offsets.begin());
    } else {
        // A column back is as many line tops as a column holds (every row is
        // whole lines tall, and code can be cut after any of them).
        const int lines = qMax(1, columnLayout().height / qMax(1, m_lineHeight));
        md::Position start = screen.start;
        int back = 0;
        for (; back < lines; ++back) {
            const std::optional<md::Position> before = lineBefore(c.rows, start);
            if (!before)
                break;
            start = *before;
        }
        c.flow.goTo(start);
        // Near the top, less than a column: the lines mostly stay in their
        // columns, and so do the offsets.
        if (back == lines && !offsets.empty()) {
            offsets.insert(offsets.begin(), 0);
            if (int(offsets.size()) > count)
                offsets.resize(count);
        }
    }
    columnsMoved(true);
    return true;
}

// ---------------------------------------------------------------------------
// Where things are

QRect TextView::columnRect(int column)
{
    if (!m_columnsOn || column < 0 || column >= columnLayout().count)
        return {};
    const ColumnLayout &l = columnLayout();
    return QRect(l.columnLeft(column), l.top, l.columnWidth(), l.height);
}

int TextView::columnAtX(qreal x)
{
    const ColumnLayout &l = columnLayout();
    int c = 0;
    while (c + 1 < l.count && x >= l.columnLeft(c + 1) - l.gap / 2.0)
        ++c;
    return c;
}

qreal TextView::columnContentWidth(int column)
{
    const md::Screen &screen = columnsScreen();
    if (m_wordWrap || column < 0 || column >= int(screen.columns.size()))
        return 0;
    qreal widest = 0;
    for (const md::Segment &s : screen.columns[column].segments) {
        QTextLayout *layout = layoutForLine(s.row);
        if (layout && layout->lineCount() > 0)
            widest = qMax(widest, layout->lineAt(0).naturalTextWidth());
    }
    return widest;
}

// How far a column can scroll: past its widest line by two characters, as
// the scrolling view does; nothing if every line fits.
static qreal offsetRange(qreal content, int width, qreal charWidth)
{
    return content > width ? content + charWidth * 2 - width : 0;
}

qreal TextView::columnOffset(int column)
{
    if (!m_columnsOn || m_wordWrap || column < 0 || column >= int(m_columns->offsets.size()))
        return 0;
    const qreal range = offsetRange(columnContentWidth(column), columnLayout().width, columnsCharWidth());
    return qBound<qreal>(0, m_columns->offsets[column], range);
}

void TextView::setColumnOffset(int column, qreal offset)
{
    if (!m_columnsOn || m_wordWrap || column < 0 || column >= columnLayout().count)
        return;
    const qreal range = offsetRange(columnContentWidth(column), columnLayout().width, columnsCharWidth());
    offset = qBound<qreal>(0, offset, range);
    std::vector<qreal> &offsets = m_columns->offsets;
    if (int(offsets.size()) <= column)
        offsets.resize(column + 1, 0);
    if (offsets[column] == offset)
        return;
    offsets[column] = offset;
    viewport()->update();
}

QRect TextView::columnBarRect(int column)
{
    if (!m_columnsOn || m_wordWrap || column < 0 || column >= columnLayout().count)
        return {};
    const ColumnLayout &l = columnLayout();
    if (offsetRange(columnContentWidth(column), l.width, columnsCharWidth()) <= 0)
        return {};
    return QRect(l.columnLeft(column) + l.textLeft, l.top + l.height + 2, l.width, scrollBarHeight);
}

// The segment of the screen at a point: its column, index and top.
namespace {

struct SegmentHit
{
    int column = 0;
    int index = -1;
    qreal top = 0;
};

SegmentHit segmentAt(const md::Screen &screen, int column, qreal top, qreal y, bool exact)
{
    const auto &segments = screen.columns[column].segments;
    for (int i = 0; i < int(segments.size()); ++i) {
        const md::Segment &s = segments[i];
        const qreal bottom = top + (s.yTo - s.yFrom);
        if (y < bottom || (!exact && i + 1 == int(segments.size()))) {
            if (exact && y < top)
                return {};
            return {column, i, top};
        }
        top = bottom;
    }
    return {};
}

} // namespace

int TextView::columnsPositionAt(const QPointF &point)
{
    const md::Screen &screen = columnsScreen();
    if (screen.empty())
        return 0;
    const ColumnLayout &l = columnLayout();
    const int column = columnAtX(point.x());
    const SegmentHit hit = segmentAt(screen, column, l.top, point.y(), false);
    if (hit.index < 0) // A column past the end of the text.
        return m_doc->characterCount() - 1;
    const md::Segment &s = screen.columns[column].segments[hit.index];
    // Off the text (above or below it): the nearest place the column shows.
    const qreal y = s.yFrom + qBound<qreal>(0, point.y() - hit.top, qMax(0, s.yTo - s.yFrom - 1));
    const QTextBlock block = m_doc->findBlockByNumber(s.row);
    QTextLayout *layout = layoutForLine(s.row);
    const QTextLine line = layout->lineAt(qBound(0, int(y / m_lineHeight), layout->lineCount() - 1));
    const qreal x = point.x() - (l.columnLeft(column) + l.textLeft) + columnOffset(column);
    return block.position() + line.xToCursor(x);
}

int TextView::columnsRowAt(const QPointF &point, QPointF *origin)
{
    const md::Screen &screen = columnsScreen();
    if (screen.empty())
        return -1;
    const ColumnLayout &l = columnLayout();
    const int column = columnAtX(point.x());
    const qreal left = l.columnLeft(column);
    if (point.x() < left || point.x() >= left + l.columnWidth())
        return -1;
    const SegmentHit hit = segmentAt(screen, column, l.top, point.y(), true);
    if (hit.index < 0)
        return -1;
    const md::Segment &s = screen.columns[column].segments[hit.index];
    *origin = QPointF(left + l.textLeft - columnOffset(column), hit.top - s.yFrom);
    return s.row;
}

QRectF TextView::columnsCaretRect(const QTextCursor &cursor, int *column)
{
    const md::Screen &screen = columnsScreen();
    if (screen.empty())
        return {};
    const int lineNo = cursor.blockNumber();
    QTextLayout *layout = layoutForLine(lineNo);
    if (!layout)
        return {};
    const int pos = cursor.positionInBlock();
    const QTextLine line = layout->lineForTextPosition(pos);
    const int top = line.isValid() ? line.lineNumber() * m_lineHeight : 0;
    const ColumnLayout &l = columnLayout();
    for (int c = 0; c < int(screen.columns.size()); ++c) {
        qreal y = l.top;
        for (const md::Segment &s : screen.columns[c].segments) {
            if (s.row == lineNo && top >= s.yFrom && top < s.yTo) {
                const qreal x = line.isValid() ? line.cursorToX(pos) : 0;
                if (column)
                    *column = c;
                return QRectF(l.columnLeft(c) + l.textLeft - columnOffset(c) + x, y + top - s.yFrom, 2, m_lineHeight);
            }
            y += s.yTo - s.yFrom;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Painting

void TextView::paintColumns(QPaintEvent *event)
{
    QPainter painter(viewport());
    painter.fillRect(event->rect(), theme::color(theme::Role::editor));
    const md::Screen &screen = columnsScreen();
    const ColumnLayout &l = columnLayout();
    const LinePaintState state = linePaintState();
    const int gutter = gutterWidth();
    const QFontMetricsF fm(font());
    painter.setFont(font());
    int paintedFirst = -1;
    int paintedLast = -1;
    for (int c = 0; c < int(screen.columns.size()); ++c) {
        const int left = l.columnLeft(c);
        const qreal offset = columnOffset(c);
        const QPointF text(left + l.textLeft - offset, 0);
        qreal y = l.top;
        qreal keptBottom = -1;
        for (const md::Segment &s : screen.columns[c].segments) {
            const qreal h = s.yTo - s.yFrom;
            if (h > 0 && QRectF(left, y, l.columnWidth(), h).intersects(event->rect())) {
                if (paintedFirst < 0)
                    paintedFirst = s.row;
                paintedLast = qMax(paintedLast, s.row);
                const qreal rowTop = y - s.yFrom;
                const qreal rowHeight = m_columns->rows.height(s.row);
                painter.save();
                // The part of the line this column shows, cut at its edges (a
                // long line scrolls in it); room for a caret at the end.
                painter.setClipRect(QRectF(left + gutter, y, l.columnWidth() - gutter + 2, h));
                paintSourceLine(painter, s.row, QPointF(text.x(), rowTop),
                                QRectF(left + gutter, rowTop, l.columnWidth() - gutter, rowHeight), state);
                painter.restore();
                // The number goes on a line's first row: a wrapped row, or a
                // line going on from the column before, has none.
                if (gutter > 0 && s.yFrom == 0) {
                    painter.setPen(state.currentLines.contains(s.row) ? theme::color(theme::Role::text)
                                                                      : theme::color(theme::Role::gutterText));
                    painter.drawText(QRectF(left, y + m_linePad, gutter - gutterRightPad, fm.height()),
                                     Qt::AlignRight | Qt::AlignVCenter, QString::number(s.row + 1));
                }
            }
            if (s.keptTo > s.yFrom)
                keptBottom = y + qMin(s.keptTo, s.yTo) - s.yFrom;
            y += h;
        }
        if (keptBottom >= 0) {
            // Lines kept from the page before: a thin line under them, as in read mode.
            painter.fillRect(QRectF(left + l.textLeft, std::floor(keptBottom) - 1, l.width, 1),
                             theme::color(theme::Role::divider));
        }
        const QRect bar = columnBarRect(c);
        if (!bar.isEmpty() && bar.intersects(event->rect())) {
            RenderedBlock::paintScrollBar(painter, bar, l.width + offsetRange(columnContentWidth(c), l.width,
                                                                              columnsCharWidth()),
                                          offset, m_columns->barDrag.active && m_columns->barDrag.column == c);
        }
    }
    if (paintedFirst >= 0)
        trimLayouts(paintedFirst, paintedLast);
}

// ---------------------------------------------------------------------------
// Controls

void TextView::placeColumnControls()
{
    if (!m_columns)
        return;
    Columns &c = *m_columns;
    const bool shown = m_columnsOn;
    if (!shown && !c.back)
        return;
    if (!c.back) {
        using columns::PageButton;
        const auto make = [this](PageButton::Direction direction, bool round, const QString &name, const QString &tip,
                                 auto move) {
            auto *button = new PageButton(direction, round, viewport());
            button->setObjectName(name);
            button->setToolTip(tip);
            connect(button, &QAbstractButton::clicked, this, move);
            return button;
        };
        c.back = make(PageButton::Direction::Left, true, QStringLiteral("ColumnsPageBack"),
                      tr("Previous page (Page Up)"), [this] { columnsPageBack(); });
        c.forward = make(PageButton::Direction::Right, true, QStringLiteral("ColumnsPageForward"),
                         tr("Next page (Page Down)"), [this] { columnsPageForward(); });
        c.up = make(PageButton::Direction::Up, false, QStringLiteral("ColumnsLineUp"), tr("Up one line (Ctrl+Up)"),
                    [this] { columnsNudge(-1); });
        c.down = make(PageButton::Direction::Down, false, QStringLiteral("ColumnsLineDown"),
                      tr("Down one line (Ctrl+Down)"), [this] { columnsNudge(1); });
    }
    for (QAbstractButton *button : {c.back.get(), c.forward.get(), c.up.get(), c.down.get()})
        button->setVisible(shown);
    if (!shown)
        return;
    const ColumnLayout &l = columnLayout();
    const int w = viewport()->width();
    const int h = viewport()->height();
    const int arrowTop = (h - columns::pageArrowSize) / 2;
    c.back->move(columns::pageArrowInset, arrowTop);
    c.forward->move(w - columns::pageArrowInset - columns::pageArrowSize, arrowTop);
    // Over the first column's top-left corner and under the last's bottom-right.
    const int right = l.columnLeft(l.count - 1) + l.columnWidth();
    c.up->move(l.left - 4, columns::lineButtonTop);
    c.down->move(right + 4 - columns::lineButtonSize, h - columns::lineButtonBottom);
    for (QAbstractButton *button : {c.back.get(), c.forward.get(), c.up.get(), c.down.get()})
        button->raise();
    updateColumnControls();
}

void TextView::updateColumnControls()
{
    if (!m_columns || !m_columns->back || !m_columnsOn)
        return;
    const md::Screen &screen = m_columns->flow.screen();
    m_columns->back->setEnabled(!screen.empty() && !screen.atStart);
    m_columns->up->setEnabled(!screen.empty() && !screen.atStart);
    m_columns->forward->setEnabled(!screen.empty() && !screen.atEnd);
    m_columns->down->setEnabled(!screen.empty() && !screen.atEnd);
}

// ---------------------------------------------------------------------------
// Keys, wheel and scroll bars

bool TextView::columnsKey(QKeyEvent *event)
{
    const auto mods = event->modifiers() & ~Qt::KeypadModifier;
    switch (event->key()) {
    case Qt::Key_PageDown:
    case Qt::Key_PageUp: {
        const bool forward = event->key() == Qt::Key_PageDown;
        if (mods == Qt::NoModifier || mods == Qt::ShiftModifier)
            columnsPage(forward, true, mods == Qt::ShiftModifier);
        else if (mods == Qt::AltModifier)
            columnsPage(forward, false, false);
        else
            return false;
        return true;
    }
    case Qt::Key_Up:
    case Qt::Key_Down:
        // A line without moving the caret, as Ctrl+Up / Ctrl+Down scroll.
        if (mods != Qt::ControlModifier)
            return false;
        columnsNudge(event->key() == Qt::Key_Down ? 1 : -1);
        return true;
    default:
        return false;
    }
}

void TextView::columnsWheel(QWheelEvent *event)
{
    Columns &c = *m_columns;
    event->accept();
    const bool pixels = !event->pixelDelta().isNull();
    const QPoint delta = pixels ? event->pixelDelta() : event->angleDelta();
    // Sideways (a horizontal wheel or touchpad swipe, or Shift with the
    // wheel) scrolls the column under the pointer, and nothing else.
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    if (shift || qAbs(delta.x()) > qAbs(delta.y())) {
        const int along = shift && delta.x() == 0 ? delta.y() : delta.x();
        // A notch is six characters, a touchpad's pixels are pixels.
        const qreal dx = pixels ? along : along / 120.0 * 6 * columnsCharWidth();
        const int column = columnAtX(event->position().x());
        setColumnOffset(column, columnOffset(column) - dx);
        return;
    }
    // A line a notch; a touchpad's pixels add up to lines.
    if (pixels != c.wheelPixels) {
        c.wheelPixels = pixels;
        c.wheel = 0;
    }
    const int step = pixels ? qMax(1, m_lineHeight) : 120;
    c.wheel += delta.y();
    const int notches = c.wheel / step;
    c.wheel -= notches * step;
    columnsNudge(-notches);
}

bool TextView::columnBarPressed(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return false;
    const ColumnLayout &l = columnLayout();
    for (int c = 0; c < l.count; ++c) {
        const QRect bar = columnBarRect(c);
        if (bar.isEmpty() || !bar.contains(event->position().toPoint()))
            continue;
        const qreal content = l.width + offsetRange(columnContentWidth(c), l.width, columnsCharWidth());
        const QRectF thumb = RenderedBlock::scrollThumb(bar, content, columnOffset(c));
        const qreal x = event->position().x();
        // A press on the thumb holds it where it is; elsewhere on the track
        // the thumb comes to the pointer.
        const bool onThumb = x >= thumb.left() && x < thumb.right();
        m_columns->barDrag = {true, c, onThumb ? x - thumb.left() : thumb.width() / 2};
        setFocus(Qt::MouseFocusReason);
        event->accept();
        if (!onThumb)
            columnBarMoved(event);
        viewport()->update();
        return true;
    }
    return false;
}

bool TextView::columnBarMoved(QMouseEvent *event)
{
    Columns::BarDrag &drag = m_columns->barDrag;
    if (!drag.active)
        return false;
    if (!(event->buttons() & Qt::LeftButton)) {
        drag = {}; // (The release went elsewhere.)
        viewport()->update();
        return false;
    }
    const QRect bar = columnBarRect(drag.column);
    if (bar.isEmpty())
        return true;
    const ColumnLayout &l = columnLayout();
    const qreal range = offsetRange(columnContentWidth(drag.column), l.width, columnsCharWidth());
    const qreal thumbWidth = RenderedBlock::scrollThumb(bar, l.width + range, 0).width();
    const qreal room = bar.width() - thumbWidth;
    if (room > 0) {
        const qreal along = qBound<qreal>(0, event->position().x() - drag.grab - bar.left(), room);
        setColumnOffset(drag.column, along / room * range);
    }
    event->accept();
    return true;
}

} // namespace vn
