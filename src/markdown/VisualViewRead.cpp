// Read mode of the visual view (see VisualView::setReadMode). Which part of
// which row each column shows, and how the moves go, is md::ReadFlow's; here
// are the rows it reads, where the columns go, the painting, and the keys,
// wheel and buttons that move it.
#include "VisualView.h"

#include "../Settings.h"
#include "../Theme.h"
#include "../editor/ReadControls.h"
#include "MarkdownModel.h"

#include <QAbstractButton>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScopedValueRollback>
#include <QTextBlock>
#include <QTimer>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace vn {

namespace {

// The room around the columns and the buttons are the source view's columns' too.
constexpr int readTop = columns::topRoom;
constexpr int readBottom = columns::bottomRoom;
constexpr int readSideRoom = columns::sideRoom;
constexpr int readGap = columns::gap;
constexpr int pageArrowSize = columns::pageArrowSize;
constexpr int pageArrowInset = columns::pageArrowInset;
constexpr int lineButtonSize = columns::lineButtonSize;
constexpr int lineButtonTop = columns::lineButtonTop;
constexpr int lineButtonBottom = columns::lineButtonBottom;
// Room a column's rows have outside it, as in the scrolling view: the
// chevron of a heading sits left of the text (VisualView.cpp's sideMargin).
constexpr qreal readMargin = 26;

using ReadButton = columns::PageButton;

} // namespace

// ---------------------------------------------------------------------------
// The rows as the flow reads them

class VisualView::ReadRows : public md::RowSource
{
public:
    explicit ReadRows(VisualView *view)
        : m_view(view)
    {
    }

    int count() override { return int(m_view->m_rows.size()); }

    int height(int row) override
    {
        const Row r = m_view->m_rows[row].row;
        return qCeil(r.rendered ? m_view->renderedRowHeight(r, m_view->textWidth()) : m_view->lineHeight());
    }

    int firstLine(int row) override { return m_view->m_rows[row].row.firstLine; }
    int lastLine(int row) override { return m_view->m_rows[row].row.lastLine; }
    int rowForLine(int line) override
    {
        return m_view->rowOfLine(qBound(0, line, m_view->document()->blockCount() - 1));
    }

    md::RowKind kind(int row) override
    {
        const RowInfo &info = m_view->m_rows[row];
        if (info.span < 0)
            return md::RowKind::Gap;
        if (!info.row.rendered)
            return md::RowKind::Normal;
        if (info.chip)
            return md::RowKind::Unbreakable;
        const RenderedBlock *b = block(row);
        if (!b)
            return md::RowKind::Normal;
        if (b->kind() == RenderedBlock::Kind::Image || b->kind() == RenderedBlock::Kind::Chip)
            return md::RowKind::Unbreakable;
        if (b->breaks().table)
            return md::RowKind::Table;
        if (m_view->itemOfRow(row))
            return md::RowKind::Normal;
        const MarkdownModel::Span &span = m_view->m_model->spans()[info.span];
        const int blockIndex = span.block >= 0 ? span.block : span.previous;
        if (blockIndex < 0)
            return md::RowKind::Normal;
        switch (m_view->blockClass(blockIndex).kind) {
        case FoldKind::Heading:
            return md::RowKind::Heading;
        case FoldKind::Table: // (One without a grid: merged cells.)
        case FoldKind::Figure:
        case FoldKind::Equation:
            return md::RowKind::Unbreakable;
        default:
            return md::RowKind::Normal;
        }
    }

    QList<int> cuts(int row) override
    {
        const RenderedBlock *b = block(row);
        return b ? b->breaks().cuts : QList<int>();
    }

    // The space above a block's first line (the strip, a box's padding)
    // goes with that line: a nudge does not stop on it, and the header of a
    // table is its first line (repeated only over the parts after it).
    QList<int> lineTops(int row) override
    {
        const RenderedBlock *b = block(row);
        if (!b || b->breaks().lineTops.isEmpty())
            return {0};
        QList<int> tops = b->breaks().lineTops;
        tops.front() = 0;
        return tops;
    }

    int headerHeight(int row) override
    {
        const RenderedBlock *b = block(row);
        return b && b->breaks().table ? b->breaks().headerHeight : 0;
    }

    // Lines through the rows' anchors (table rows, list items, code lines),
    // as the scrolling view places them. A visual line that has the start of
    // a source line shows that line (a table row's top is its border, a
    // little above where its line starts); else the line it goes on with.
    int sourceLineAt(int row, int y) override
    {
        const Row r = m_view->m_rows[row].row;
        const QList<int> tops = lineTops(row);
        const auto next = std::upper_bound(tops.begin(), tops.end(), y);
        const int end = next != tops.end() ? *next : qMax(y + 1, height(row));
        const qreal from = m_view->linePositionAt(row, y) + 1e-6;
        const qreal to = m_view->linePositionAt(row, end - 1) + 1e-6;
        const int line = std::floor(to) > std::floor(from) ? int(std::ceil(from - 2e-6)) : int(std::floor(from));
        return qBound(r.firstLine, line, r.lastLine);
    }

    int yOfLine(int row, int line) override
    {
        const auto [index, y] = m_view->placeOfLinePosition(line);
        if (index != row)
            return 0;
        // The top of the visual line that has the line's start.
        const QList<int> tops = lineTops(row);
        int top = 0;
        for (const int t : tops) {
            if (t > y + 0.5)
                break;
            top = t;
        }
        return top;
    }

private:
    const RenderedBlock *block(int row)
    {
        const RowInfo &info = m_view->m_rows[row];
        if (!info.row.rendered || info.span < 0)
            return nullptr;
        return m_view->renderedForRow(info.row);
    }

    VisualView *m_view;
};

// ---------------------------------------------------------------------------
// Mode and layout

void VisualView::setReadMode(bool on)
{
    if (m_readMode == on)
        return;
    const bool wasActive = readActive();
    // The place, as a line position, at the width it was found at.
    const qreal position = wasActive ? m_readLinePosition : topLinePosition();
    m_readMode = on;
    m_readWheel = 0;
    m_readOffsets.clear();
    m_readBarDrag = {};
    if (readActive()) {
        m_readLinePosition = position;
        m_readNeedsStart = true;
    }
    hideFigureBar();
    clearTableHover();
    // Rows lay out at another width: the column's, or the view's again.
    relayout();
    if (wasActive && !readActive())
        scrollToLinePosition(position);
    setScrollBarsHidden(readActive());
    placeReadControls();
    noteReadScreen();
}

void VisualView::setReadColumns(int columns)
{
    columns = qMax(0, columns);
    if (m_readColumns == columns)
        return;
    m_readColumns = columns;
    if (readActive()) {
        placeReadControls();
        viewport()->update();
        noteReadScreen();
    }
}

void VisualView::setReadChars(int chars)
{
    chars = qBound(20, chars, 400);
    if (m_readChars == chars)
        return;
    m_readChars = chars;
    if (readActive()) {
        hideFigureBar();
        relayout(); // (Figures fit the column: renders at the new width.)
        placeReadControls();
        noteReadScreen();
    }
}

int VisualView::readMaxColumns()
{
    return readActive() ? readLayout().fit : 1;
}

int VisualView::readColumnCount()
{
    return readActive() ? readLayout().count : 1;
}

const VisualView::ReadLayout &VisualView::readLayout() const
{
    ReadLayout &l = m_readLayout;
    const QSize size = viewport()->size();
    const qreal fontSize = font().pointSizeF();
    if (l.viewport == size && l.fontSize == fontSize && l.chars == m_readChars && l.columns == m_readColumns
        && l.serial == m_readLayoutSerial)
        return l;
    l = {};
    l.viewport = size;
    l.fontSize = fontSize;
    l.chars = m_readChars;
    l.columns = m_readColumns;
    l.serial = m_readLayoutSerial;
    // Characters of body text as context() sets it: the text font, zoomed.
    QFont body = theme::textFont();
    body.setPointSizeF(body.pointSizeF() * fontSize / theme::editorFont().pointSizeF());
    const qreal charWidth = QFontMetricsF(body).averageCharWidth();
    const int room = qMax(1, size.width() - 2 * readSideRoom);
    l.gap = readGap;
    l.width = qBound(1, qRound(m_readChars * charWidth), room);
    l.fit = qMax(1, (room + l.gap) / (l.width + l.gap));
    l.count = m_readColumns > 0 ? qMin(m_readColumns, l.fit) : l.fit;
    const int total = l.count * l.width + (l.count - 1) * l.gap;
    l.left = (size.width() - total) / 2;
    l.top = readTop;
    l.height = qMax(lineHeight() * 2, size.height() - readTop - readBottom);
    return l;
}

qreal VisualView::textWidth() const
{
    return readActive() ? readLayout().width : TextView::textWidth();
}

qreal VisualView::readLineHeight()
{
    const RenderContext &ctx = renderContext();
    return QFontMetricsF(ctx.font).height() * ctx.lineHeight;
}

// ---------------------------------------------------------------------------
// The flow

const md::Screen &VisualView::ensureReadScreen()
{
    if (!m_readFlow) {
        m_readRows = std::make_unique<ReadRows>(this);
        m_readFlow = std::make_unique<md::ReadFlow>(m_readRows.get());
        m_readFlow->setKeptLines(Settings::instance().number(key::readKeptLines));
    }
    static const md::Screen none;
    // (Hidden, the rows wait for the view to be shown.)
    if (!readActive() || m_rowsStale || m_rows.empty())
        return none;
    const ReadLayout &l = readLayout();
    md::Geometry geometry;
    geometry.columns = l.count;
    geometry.height = l.height;
    geometry.width = l.width;
    geometry.gap = l.gap;
    geometry.revision = m_readRevision;
    if (m_readNeedsStart) {
        m_readNeedsStart = false;
        m_readDirty = false;
        if (!(m_readFlow->geometry().key() == geometry.key())) {
            m_readFlow->setGeometry(geometry);
            m_readOffsets.clear();
        }
        // Where the line position is drawn, at the top of its visual line.
        const auto [row, y] = placeOfLinePosition(m_readLinePosition);
        const QList<int> tops = m_readRows->lineTops(row);
        int top = 0;
        for (const int t : tops) {
            if (t > y + 0.5)
                break;
            top = t;
        }
        m_readFlow->goTo({row, top});
    } else if (!(m_readFlow->geometry().key() == geometry.key())) {
        m_readDirty = false;
        m_readFlow->setGeometry(geometry);
        m_readOffsets.clear(); // (The layout changed: so did what is wide.)
    } else if (m_readDirty) {
        m_readDirty = false;
        m_readFlow->reflow();
    } else {
        return m_readFlow->screen();
    }
    const md::Screen &screen = m_readFlow->screen();
    if (screen.start.row < int(m_rows.size()))
        m_readLinePosition = linePositionAt(screen.start.row, screen.start.y);
    noteReadScreen();
    return screen;
}

const md::Screen &VisualView::readScreen()
{
    return ensureReadScreen();
}

void VisualView::readMoved()
{
    const md::Screen &screen = m_readFlow->screen();
    if (screen.start.row < int(m_rows.size()))
        m_readLinePosition = linePositionAt(screen.start.row, screen.start.y);
    m_readDirty = false;
    updateReadControls();
    viewport()->update();
    Q_EMIT scrolled();
    noteReadScreen();
}

void VisualView::readPageForward()
{
    if (!readActive() || ensureReadScreen().empty())
        return;
    m_readFlow->pageForward();
    readMoved();
}

void VisualView::readPageBack()
{
    if (!readActive() || ensureReadScreen().empty())
        return;
    m_readFlow->pageBack();
    readMoved();
}

void VisualView::readNudge(int lines)
{
    if (!readActive() || lines == 0 || ensureReadScreen().empty())
        return;
    m_readFlow->nudge(lines);
    readMoved();
}

void VisualView::readToStart()
{
    if (!readActive() || ensureReadScreen().empty())
        return;
    m_readFlow->toStart();
    readMoved();
}

void VisualView::readToEnd()
{
    if (!readActive() || ensureReadScreen().empty())
        return;
    m_readFlow->toEnd();
    readMoved();
}

std::pair<int, int> VisualView::readLineRange()
{
    if (!readActive() || ensureReadScreen().empty())
        return {-1, -1};
    const md::LineRange range = m_readFlow->lineRange();
    return {range.first, range.last};
}

// What the status bar and the bar over the view show changes after a move or
// a new layout; they hear of it once the event (or paint) is done.
void VisualView::noteReadScreen()
{
    if (m_readNewsPending)
        return;
    m_readNewsPending = true;
    QTimer::singleShot(0, this, [this] {
        m_readNewsPending = false;
        ReadNews news;
        if (readActive() && m_readFlow && !m_readFlow->screen().empty()) {
            const md::Screen &screen = m_readFlow->screen();
            const md::LineRange range = m_readFlow->lineRange();
            news = {screen.start, screen.end, range.first, range.last, readLayout().fit};
        } else if (readActive()) {
            news.fit = readLayout().fit;
        }
        placeReadControls(); // (The columns may have moved: a zoom, a font.)
        if (news == m_readNews)
            return;
        m_readNews = news;
        Q_EMIT readChanged();
    });
}

void VisualView::ensureCursorVisible()
{
    if (!readActive())
        return TextView::ensureCursorVisible();
    // The screen that starts with the caret's line, if neither it nor its
    // selection is on this one (find, go to line, the outline put it there;
    // Select All does not turn to the end).
    if (m_readPointing) {
        viewport()->update();
        return;
    }
    const QTextCursor caret = textCursor();
    const int from = document()->findBlock(caret.selectionStart()).blockNumber();
    const int to = document()->findBlock(caret.selectionEnd()).blockNumber();
    const std::pair<int, int> range = readLineRange();
    if (range.first < 0 || (from <= range.second && to >= range.first)) {
        viewport()->update();
        return;
    }
    m_readFlow->goToLine(caret.blockNumber());
    readMoved();
}

void VisualView::centerCursor()
{
    if (!readActive())
        return TextView::centerCursor();
    ensureCursorVisible();
}

void VisualView::scrollPage(bool forward)
{
    if (!readActive())
        return TextView::scrollPage(forward);
    forward ? readPageForward() : readPageBack();
}

// ---------------------------------------------------------------------------
// Where things are

QRect VisualView::readColumnRect(int column)
{
    if (!readActive())
        return {};
    const ReadLayout &l = readLayout();
    return QRect(l.left + column * (l.width + l.gap), l.top, l.width, l.height);
}

QRect VisualView::readSegmentRect(int column, int index)
{
    const md::Screen &screen = ensureReadScreen();
    if (column < 0 || column >= int(screen.columns.size()))
        return {};
    const auto &segments = screen.columns[column].segments;
    if (index < 0 || index >= int(segments.size()))
        return {};
    const QRect columnRect = readColumnRect(column);
    int y = columnRect.top();
    for (int i = 0; i < index; ++i)
        y += m_readFlow->drawnHeight(segments[i]);
    const md::Segment &s = segments[index];
    if (s.repeatHeader)
        y += m_readRows->headerHeight(s.row);
    return QRect(columnRect.left(), y, columnRect.width(), s.yTo - s.yFrom);
}

VisualView::ReadHit VisualView::readHitAt(const QPointF &point, bool exact)
{
    const md::Screen &screen = ensureReadScreen();
    if (screen.empty())
        return {};
    const ReadLayout &l = readLayout();
    // The column: the nearest, the gaps shared half and half.
    int c = 0;
    while (c + 1 < l.count && point.x() >= l.left + (c + 1) * (l.width + l.gap) - l.gap / 2.0)
        ++c;
    const qreal left = l.left + c * (l.width + l.gap);
    if (exact && (point.x() < left - readMargin || point.x() >= left + l.width + readMargin))
        return {};
    const auto &segments = screen.columns[c].segments;
    qreal y = l.top;
    for (int i = 0; i < int(segments.size()); ++i) {
        const md::Segment &s = segments[i];
        const qreal bottom = y + m_readFlow->drawnHeight(s);
        if (point.y() < bottom || (!exact && i + 1 == int(segments.size()))) {
            if (exact && point.y() < y)
                return {};
            ReadHit hit{c, i, y, false};
            hit.header = s.repeatHeader && point.y() >= y && point.y() < y + m_readRows->headerHeight(s.row);
            return hit;
        }
        y = bottom;
    }
    return {};
}

QPointF VisualView::readOrigin(const ReadHit &hit)
{
    const ReadLayout &l = readLayout();
    const md::Segment &s = m_readFlow->screen().columns[hit.column].segments[hit.index];
    // (A wide table or equation is slid sideways.)
    const qreal left = l.left + hit.column * (l.width + l.gap) - readOffset(s.row);
    if (hit.header) {
        const RenderedBlock *block = renderedForRow(m_rows[s.row].row);
        return QPointF(left, hit.top - (block ? block->breaks().headerTop : 0));
    }
    const int header = s.repeatHeader ? m_readRows->headerHeight(s.row) : 0;
    return QPointF(left, hit.top + header - s.yFrom);
}

qreal VisualView::readOffset(int rowIndex)
{
    if (!readActive() || rowIndex < 0 || rowIndex >= int(m_rows.size()) || !m_rows[rowIndex].row.rendered)
        return 0;
    const Row r = m_rows[rowIndex].row;
    const RenderedBlock *block = renderedForRow(r);
    const qreal range = block ? block->contentWidth() - readLayout().width : 0;
    if (range <= 0)
        return 0;
    const auto it = m_readOffsets.find(r.firstLine);
    return it == m_readOffsets.end() ? 0 : qBound<qreal>(0, it->second, range);
}

void VisualView::setReadOffset(int rowIndex, qreal offset)
{
    if (!readActive() || rowIndex < 0 || rowIndex >= int(m_rows.size()))
        return;
    const RenderedBlock *block = renderedForRow(m_rows[rowIndex].row);
    const qreal range = block ? block->contentWidth() - readLayout().width : 0;
    if (range <= 0)
        return;
    offset = qBound<qreal>(0, offset, range);
    qreal &kept = m_readOffsets[m_rows[rowIndex].row.firstLine];
    if (kept == offset)
        return;
    kept = offset;
    hideFigureBar();
    viewport()->update();
}

const RenderedBlock *VisualView::renderedBlockOfLine(int line)
{
    const Row r = row(rowOfLine(line));
    return r.rendered ? renderedForRow(r) : nullptr;
}

qreal VisualView::readOffsetOfLine(int line)
{
    return readOffset(rowOfLine(line));
}

bool VisualView::readScrollAt(const QPointF &point, qreal dx)
{
    if (!readActive() || ensureReadScreen().empty())
        return false;
    const ReadHit hit = readHitAt(point, true);
    if (hit.index < 0)
        return false;
    const int index = m_readFlow->screen().columns[hit.column].segments[hit.index].row;
    const RenderedBlock *block = m_rows[index].row.rendered ? renderedForRow(m_rows[index].row) : nullptr;
    if (!block || block->contentWidth() <= readLayout().width)
        return false;
    setReadOffset(index, readOffset(index) + dx);
    return true;
}

// ---------------------------------------------------------------------------
// The scrollbar of a wide block

QRectF VisualView::readBarOf(const ReadHit &hit)
{
    if (hit.index < 0)
        return {};
    const md::Segment &s = m_readFlow->screen().columns[hit.column].segments[hit.index];
    const Row r = m_rows[s.row].row;
    const RenderedBlock *block = r.rendered ? renderedForRow(r) : nullptr;
    const ReadLayout &l = readLayout();
    if (!block || block->contentWidth() <= l.width)
        return {};
    // (Only the part of a split table that has the block's bottom shows it.)
    const QRectF track = block->scrollTrack(l.width);
    if (track.isEmpty() || track.top() < s.yFrom || track.bottom() > s.yTo)
        return {};
    ReadHit body = hit;
    body.header = false;
    const qreal left = l.left + hit.column * (l.width + l.gap);
    return track.translated(left, readOrigin(body).y());
}

QRect VisualView::readBarRect(int column, int index)
{
    const md::Screen &screen = ensureReadScreen();
    if (column < 0 || column >= int(screen.columns.size()))
        return {};
    qreal y = readLayout().top;
    const auto &segments = screen.columns[column].segments;
    for (int i = 0; i < int(segments.size()) && i <= index; ++i) {
        if (i == index)
            return readBarOf({column, i, y, false}).toAlignedRect();
        y += m_readFlow->drawnHeight(segments[i]);
    }
    return {};
}

std::optional<VisualView::ReadHit> VisualView::readBarAt(const QPointF &point)
{
    if (!readActive() || ensureReadScreen().empty())
        return std::nullopt;
    const ReadHit hit = readHitAt(point, true);
    if (hit.index < 0 || !readBarOf(hit).contains(point))
        return std::nullopt;
    return hit;
}

bool VisualView::readBarPressed(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return false;
    const std::optional<ReadHit> hit = readBarAt(event->position());
    if (!hit)
        return false;
    const int index = m_readFlow->screen().columns[hit->column].segments[hit->index].row;
    const RenderedBlock *block = renderedForRow(m_rows[index].row);
    const QRectF bar = readBarOf(*hit);
    const QRectF thumb = RenderedBlock::scrollThumb(bar, block->contentWidth(), readOffset(index));
    const qreal x = event->position().x();
    // A press on the thumb holds it where it is; elsewhere on the track the
    // thumb comes to the pointer.
    const bool onThumb = x >= thumb.left() && x < thumb.right();
    m_readBarDrag = {true, m_rows[index].row.firstLine, onThumb ? x - thumb.left() : thumb.width() / 2};
    setFocus(Qt::MouseFocusReason);
    event->accept();
    if (!onThumb)
        readBarMoved(event);
    viewport()->update();
    return true;
}

bool VisualView::readBarMoved(QMouseEvent *event)
{
    if (!m_readBarDrag.active)
        return false;
    if (!(event->buttons() & Qt::LeftButton)) {
        m_readBarDrag = {}; // (The release went elsewhere.)
        viewport()->update();
        return false;
    }
    // The bar of the block's row, where it shows now.
    const md::Screen &screen = ensureReadScreen();
    for (int c = 0; c < int(screen.columns.size()); ++c) {
        qreal y = readLayout().top;
        const auto &segments = screen.columns[c].segments;
        for (int i = 0; i < int(segments.size()); ++i) {
            const md::Segment &s = segments[i];
            const ReadHit hit{c, i, y, false};
            y += m_readFlow->drawnHeight(s);
            if (m_rows[s.row].row.firstLine != m_readBarDrag.line)
                continue;
            const QRectF bar = readBarOf(hit);
            if (bar.isEmpty())
                continue;
            const RenderedBlock *block = renderedForRow(m_rows[s.row].row);
            const qreal thumbWidth = RenderedBlock::scrollThumb(bar, block->contentWidth(), 0).width();
            const qreal room = bar.width() - thumbWidth;
            if (room > 0) {
                const qreal along = qBound<qreal>(0, event->position().x() - m_readBarDrag.grab - bar.left(), room);
                setReadOffset(s.row, along / room * (block->contentWidth() - bar.width()));
            }
            event->accept();
            return true;
        }
    }
    return true;
}

int VisualView::rowAtPoint(const QPointF &point, QPointF *origin)
{
    if (!readActive())
        return TextView::rowAtPoint(point, origin);
    const ReadHit hit = readHitAt(point, true);
    if (hit.index < 0)
        return -1;
    *origin = readOrigin(hit);
    return m_readFlow->screen().columns[hit.column].segments[hit.index].row;
}

int VisualView::positionAt(const QPointF &point)
{
    if (!readActive())
        return TextView::positionAt(point);
    const ReadHit hit = readHitAt(point, false);
    if (hit.index < 0)
        return 0;
    const md::Segment &s = m_readFlow->screen().columns[hit.column].segments[hit.index];
    const QPointF origin = readOrigin(hit);
    QPointF local = point - origin;
    // Off the text (a margin, above or below it): the nearest place the
    // column shows (a table's repeated header is its header).
    if (!hit.header)
        local.setY(qBound<qreal>(s.yFrom, local.y(), qMax(s.yFrom, s.yTo - 1)));
    const Row r = m_rows[s.row].row;
    return r.rendered ? hitTestRenderedRow(r, local) : document()->findBlockByNumber(r.firstLine).position();
}

std::optional<QPointF> VisualView::rowOrigin(int line)
{
    if (!readActive()) {
        qreal top = 0;
        qreal bottom = 0;
        if (!lineBounds(line, &top, &bottom))
            return std::nullopt;
        return QPointF(textLeft(), top);
    }
    const md::Screen &screen = ensureReadScreen();
    const int index = rowOfLine(line);
    const ReadLayout &l = readLayout();
    for (int c = 0; c < int(screen.columns.size()); ++c) {
        qreal y = l.top;
        const auto &segments = screen.columns[c].segments;
        for (int i = 0; i < int(segments.size()); ++i) {
            if (segments[i].row == index)
                return readOrigin({c, i, y, false});
            y += m_readFlow->drawnHeight(segments[i]);
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Painting

void VisualView::paintRead(QPaintEvent *event)
{
    QPainter painter(viewport());
    const QColor background = theme::color(theme::Role::editor);
    painter.fillRect(event->rect(), background);
    const md::Screen &screen = ensureReadScreen();
    const ReadLayout &l = readLayout();
    for (int c = 0; c < int(screen.columns.size()); ++c) {
        const qreal left = l.left + c * (l.width + l.gap);
        const QRectF band(left - readMargin, 0, l.width + 2 * readMargin, viewport()->height());
        if (!band.intersects(event->rect()))
            continue;
        qreal y = l.top;
        qreal keptBottom = -1;
        for (const md::Segment &s : screen.columns[c].segments) {
            const Row r = m_rows[s.row].row;
            const qreal rowHeight = m_readRows->height(s.row);
            if (s.repeatHeader) {
                // The header row of a table that goes on here, from its own rendering.
                const int header = m_readRows->headerHeight(s.row);
                const RenderedBlock *block = renderedForRow(r);
                painter.save();
                painter.setClipRect(QRectF(left - readMargin, y, l.width + 2 * readMargin, header));
                if (block && block->contentWidth() > l.width)
                    painter.setClipRect(QRectF(left, y, l.width, header), Qt::IntersectClip);
                const QScopedValueRollback<qreal> slide(m_readSlide, readOffset(s.row));
                paintRenderedRow(painter, r, QRectF(left, y - (block ? block->breaks().headerTop : 0), l.width, rowHeight));
                painter.restore();
                y += header;
            }
            const qreal h = s.yTo - s.yFrom;
            if (h > 0 && QRectF(band.left(), y, band.width(), h).intersects(event->rect())) {
                painter.save();
                painter.setClipRect(QRectF(left - readMargin, y, l.width + readMargin + 2, h));
                if (r.rendered) {
                    const RenderedBlock *block = renderedForRow(r);
                    const QRectF rowRect(left, y - s.yFrom, l.width, rowHeight);
                    if (block && block->contentWidth() > l.width) {
                        // A wide table or equation is cut at the column's
                        // edges and slides under its strip; its scrollbar stays.
                        painter.setClipRect(QRectF(left, y, l.width, h), Qt::IntersectClip);
                        {
                            const QScopedValueRollback<qreal> slide(m_readSlide, readOffset(s.row));
                            paintRenderedRow(painter, r, rowRect);
                        }
                        const QRectF track = block->scrollTrack(l.width);
                        if (track.top() >= s.yFrom && track.bottom() <= s.yTo) {
                            RenderedBlock::paintScrollBar(painter, track.translated(rowRect.topLeft()),
                                                          block->contentWidth(), readOffset(s.row),
                                                          m_readBarDrag.active && m_readBarDrag.line == r.firstLine);
                        }
                    } else {
                        paintRenderedRow(painter, r, rowRect);
                    }
                } else {
                    // Source standing in for a block not parsed yet.
                    painter.setPen(theme::color(theme::Role::text));
                    painter.drawText(QRectF(left, y - s.yFrom, l.width, rowHeight), Qt::AlignLeft | Qt::AlignTop,
                                     document()->findBlockByNumber(r.firstLine).text());
                }
                painter.restore();
            }
            if (s.keptTo > s.yFrom) {
                // Lines kept from the page before are shown as any line: the
                // thin line under them is what marks them (greyed, they read
                // as disabled).
                keptBottom = y + qMin(s.keptTo, s.yTo) - s.yFrom;
            }
            y += h;
        }
        if (keptBottom >= 0) {
            // A thin line between them and the new page, in the gap above its first line.
            painter.fillRect(QRectF(left, std::floor(keptBottom) - 1, l.width, 1),
                             theme::color(theme::Role::divider));
        }
    }
}

// ---------------------------------------------------------------------------
// Controls

void VisualView::placeReadControls()
{
    const bool shown = readActive();
    if (!shown && !m_readBack)
        return;
    if (!m_readBack) {
        const auto make = [this](ReadButton::Direction direction, bool round, const QString &name,
                                 const QString &tip, void (VisualView::*move)()) {
            auto *button = new ReadButton(direction, round, viewport());
            button->setObjectName(name);
            button->setToolTip(tip);
            connect(button, &QAbstractButton::clicked, this, move);
            return button;
        };
        m_readBack = make(ReadButton::Direction::Left, true, QStringLiteral("ReadPageBack"),
                          tr("Previous page (Page Up, Left, Shift+Space)"), &VisualView::readPageBack);
        m_readForward = make(ReadButton::Direction::Right, true, QStringLiteral("ReadPageForward"),
                             tr("Next page (Page Down, Right, Space)"), &VisualView::readPageForward);
        m_readUp = new ReadButton(ReadButton::Direction::Up, false, viewport());
        m_readUp->setObjectName(QStringLiteral("ReadLineUp"));
        m_readUp->setToolTip(tr("Up one line (Up, Ctrl+Up)"));
        connect(m_readUp, &QAbstractButton::clicked, this, [this] { readNudge(-1); });
        m_readDown = new ReadButton(ReadButton::Direction::Down, false, viewport());
        m_readDown->setObjectName(QStringLiteral("ReadLineDown"));
        m_readDown->setToolTip(tr("Down one line (Down, Ctrl+Down)"));
        connect(m_readDown, &QAbstractButton::clicked, this, [this] { readNudge(1); });
    }
    for (QAbstractButton *button : {m_readBack, m_readForward, m_readUp, m_readDown})
        button->setVisible(shown);
    if (!shown)
        return;
    const ReadLayout &l = readLayout();
    const int w = viewport()->width();
    const int h = viewport()->height();
    const int arrowTop = (h - pageArrowSize) / 2;
    m_readBack->move(pageArrowInset, arrowTop);
    m_readForward->move(w - pageArrowInset - pageArrowSize, arrowTop);
    // Over the first column's top-left corner and under the last's bottom-right.
    const int right = l.left + l.count * l.width + (l.count - 1) * l.gap;
    m_readUp->move(l.left - 4, lineButtonTop);
    m_readDown->move(right + 4 - lineButtonSize, h - lineButtonBottom);
    for (QAbstractButton *button : {m_readBack, m_readForward, m_readUp, m_readDown})
        button->raise();
    updateReadControls();
}

void VisualView::updateReadControls()
{
    if (!m_readBack || !readActive() || !m_readFlow)
        return;
    const md::Screen &screen = m_readFlow->screen();
    m_readBack->setEnabled(!screen.empty() && !screen.atStart);
    m_readUp->setEnabled(!screen.empty() && !screen.atStart);
    m_readForward->setEnabled(!screen.empty() && !screen.atEnd);
    m_readDown->setEnabled(!screen.empty() && !screen.atEnd);
}

// ---------------------------------------------------------------------------
// Keys and wheel

bool VisualView::readKey(QKeyEvent *event)
{
    const auto mods = event->modifiers() & ~Qt::KeypadModifier;
    const bool plain = mods == Qt::NoModifier;
    switch (event->key()) {
    case Qt::Key_PageDown:
        if (!plain && mods != Qt::AltModifier)
            return false;
        readPageForward();
        return true;
    case Qt::Key_PageUp:
        if (!plain && mods != Qt::AltModifier)
            return false;
        readPageBack();
        return true;
    case Qt::Key_Space:
        if (plain)
            readPageForward();
        else if (mods == Qt::ShiftModifier)
            readPageBack();
        else
            return false;
        return true;
    case Qt::Key_Right:
    case Qt::Key_Left:
        if (!plain)
            return false;
        event->key() == Qt::Key_Right ? readPageForward() : readPageBack();
        return true;
    case Qt::Key_Down:
    case Qt::Key_Up:
        if (!plain && mods != Qt::ControlModifier)
            return false;
        readNudge(event->key() == Qt::Key_Down ? 1 : -1);
        return true;
    case Qt::Key_Home:
    case Qt::Key_End:
        if (mods != Qt::ControlModifier)
            return false;
        event->key() == Qt::Key_Home ? readToStart() : readToEnd();
        return true;
    default:
        return false;
    }
}

void VisualView::wheelEvent(QWheelEvent *event)
{
    // Ctrl+wheel zooms as anywhere.
    if (!readActive() || (event->modifiers() & Qt::ControlModifier))
        return TextView::wheelEvent(event);
    event->accept();
    const bool pixels = !event->pixelDelta().isNull();
    const QPoint delta = pixels ? event->pixelDelta() : event->angleDelta();
    // Sideways (a horizontal wheel or touchpad swipe, or Shift with the wheel)
    // scrolls the wide block under the pointer, and nothing else.
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    if (shift || qAbs(delta.x()) > qAbs(delta.y())) {
        const int along = shift && delta.x() == 0 ? delta.y() : delta.x();
        // A notch is three lines of text, a touchpad's pixels are pixels.
        const qreal dx = pixels ? along : along / 120.0 * 3 * readLineHeight();
        readScrollAt(event->position(), -dx);
        return;
    }
    // A line a notch; a touchpad's pixels add up to lines of body text.
    if (pixels != m_readWheelPixels) {
        m_readWheelPixels = pixels;
        m_readWheel = 0;
    }
    const int step = pixels ? qMax(1, qRound(readLineHeight())) : 120;
    m_readWheel += delta.y();
    const int notches = m_readWheel / step;
    m_readWheel -= notches * step;
    readNudge(-notches);
}

} // namespace vn
