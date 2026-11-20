// Read mode of the visual view (see VisualView::setReadMode). Which part of
// which row each column shows, and how the moves go, is md::ReadFlow's; here
// are the rows it reads, where the columns go, the painting, and the keys,
// wheel and buttons that move it.
#include "VisualView.h"

#include "../Settings.h"
#include "../Theme.h"
#include "../editor/ReadControls.h"
#include "ImageViewer.h"
#include "MarkdownModel.h"

#include <QAbstractButton>
#include <QApplication>
#include <QFontMetricsF>
#include <QFrame>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScopedValueRollback>
#include <QScreen>
#include <QTextBlock>
#include <QTimer>
#include <QTouchEvent>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace vn {

namespace {

// The room around the columns and the buttons are the source view's columns' too.
constexpr int readTop = columns::topRoom;
constexpr int readBottom = columns::bottomRoom;
constexpr int pageArrowSize = columns::pageArrowSize;
constexpr int pageArrowInset = columns::pageArrowInset;
constexpr int lineButtonSize = columns::lineButtonSize;
constexpr int lineButtonTop = columns::lineButtonTop;
constexpr int lineButtonBottom = columns::lineButtonBottom;
// Room a column's rows have outside it, as in the scrolling view: the
// chevron of a heading sits left of the text (VisualView.cpp's sideMargin).
constexpr qreal readMargin = 26;

using ReadButton = columns::PageButton;

constexpr int previewWidth = 320;
constexpr int previewHeight = 240;
constexpr int previewPadding = 6;
constexpr int glowRoom = 6;

bool isTouchMouse(const QMouseEvent *event)
{
    return event->pointingDevice()->type() == QInputDevice::DeviceType::TouchScreen
           || event->source() == Qt::MouseEventSynthesizedByQt
           || event->source() == Qt::MouseEventSynthesizedBySystem;
}

} // namespace

class VisualView::ReadChipPreview : public QFrame
{
public:
    explicit ReadChipPreview(QWidget *parent)
        : QFrame(parent, Qt::ToolTip | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus)
    {
        setObjectName(QStringLiteral("ReadChipPreview"));
        setAttribute(Qt::WA_StyledBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void showPictures(QList<QImage> pictures, const QRect &chip, const QRect &available)
    {
        m_pictures = std::move(pictures);
        m_boxes.clear();
        const int width = qMin(previewWidth, available.width()) - 2 * previewPadding;
        const int height = qMin(previewHeight, available.height()) - 2 * previewPadding;
        if (width <= 0 || height <= 0)
            return;
        const qreal gap = qMin<qreal>(4, qreal(width) / (4 * m_pictures.size()));
        const qreal slot = (width - gap * (m_pictures.size() - 1)) / m_pictures.size();
        qreal x = previewPadding;
        qreal tallest = 0;
        for (const QImage &image : std::as_const(m_pictures)) {
            QSizeF size = image.deviceIndependentSize();
            if (size.width() > slot || size.height() > height)
                size.scale(slot, height, Qt::KeepAspectRatio);
            m_boxes.append(QRectF(QPointF(x, 0), size));
            tallest = qMax(tallest, size.height());
            x += size.width() + gap;
        }
        for (QRectF &box : m_boxes)
            box.moveTop(previewPadding + (tallest - box.height()) / 2);
        const QSize size(qCeil(x - gap + previewPadding), qCeil(tallest + 2 * previewPadding));
        int left = chip.right() + previewPadding;
        int top = chip.top();
        if (left + size.width() > available.right() + 1) {
            left = chip.left() - previewPadding - size.width();
            if (left < available.left()) {
                left = chip.left();
                top = chip.bottom() + previewPadding;
                if (top + size.height() > available.bottom() + 1)
                    top = chip.top() - previewPadding - size.height();
            }
        }
        left = qBound(available.left(), left, available.right() + 1 - size.width());
        top = qBound(available.top(), top, available.bottom() + 1 - size.height());
        setGeometry(QRect(QPoint(left, top), size));
        show();
        update();
    }

    void clearPictures()
    {
        hide();
        m_pictures.clear();
        m_boxes.clear();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QFrame::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        for (qsizetype i = 0; i < m_boxes.size(); ++i)
            painter.drawImage(m_boxes[i], m_pictures[i]);
    }

private:
    QList<QImage> m_pictures;
    QList<QRectF> m_boxes;
};

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
        if (b->kind() == RenderedBlock::Kind::Chip)
            return md::RowKind::Unbreakable;
        if (b->breaks().table)
            return md::RowKind::Table;
        if (m_view->itemOfRow(row))
            return b->kind() == RenderedBlock::Kind::Image ? md::RowKind::Unbreakable : md::RowKind::Normal;
        const MarkdownModel::Span &span = m_view->m_model->spans()[info.span];
        const int blockIndex = span.block >= 0 ? span.block : span.previous;
        if (blockIndex < 0)
            return b->kind() == RenderedBlock::Kind::Image ? md::RowKind::Unbreakable : md::RowKind::Normal;
        const FoldKind fold = m_view->blockClass(blockIndex).kind;
        // Pictures and diagrams move to the next column if they must (not
        // equations, which stay with their text).
        if (fold == FoldKind::Figure && b->firstPicture())
            return md::RowKind::Picture;
        if (b->kind() == RenderedBlock::Kind::Image)
            return md::RowKind::Unbreakable;
        switch (fold) {
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

    // A table's parts may be one row (or a few lines of a tall one): the
    // header repeated over each part says what it is.
    int minPieceLines(int row) override { return kind(row) == md::RowKind::Table ? 1 : 2; }

    // A chip as a collapsed block's, and a line of small text.
    int chipHeight() override
    {
        const RenderContext &ctx = m_view->renderContext();
        return qCeil(RenderedBlock::createChip({QStringLiteral("Image"), {}}, ctx)->height());
    }
    int noteHeight() override { return qCeil(QFontMetricsF(m_view->readNoteFont()).height()) + 4; }

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

void VisualView::setReadMode(bool on, int startLine)
{
    if (m_readMode == on)
        return;
    const bool wasActive = readActive();
    // The place, as a line position, at the width it was found at.
    const qreal position = wasActive ? m_readLinePosition : startLine >= 0 ? startLine : topLinePosition();
    invalidateReadChips();
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
    const int oldWidth = readActive() ? readLayout().width : 0;
    m_readColumns = columns;
    if (readActive()) {
        if (readLayout().width != oldWidth) {
            hideFigureBar();
            relayout(); // Figures and text lay out at the new width.
        }
        placeReadControls();
        viewport()->update();
        noteReadScreen();
    }
}

void VisualView::setReadChars(int chars)
{
    chars = qBound(20, chars, 100);
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

std::pair<int, int> VisualView::readColumnChars()
{
    return readActive() ? readLayout().shownChars : std::pair{50, 50};
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
    const auto sizing = columns::sizing(size.width(), charWidth, 0, m_readColumns, m_readChars);
    l.width = sizing.textWidth;
    l.fit = sizing.fit;
    l.count = sizing.count;
    l.shownChars = {sizing.chars, sizing.maxChars};
    l.gap = columns::spreadGap(size.width(), l.count, l.width);
    const int total = l.count * l.width + (l.count - 1) * l.gap;
    l.left = (size.width() - total) / 2;
    l.top = readTop;
    l.height = qMax(int(bodyLineHeight() * 2), size.height() - readTop - readBottom);
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
    invalidateReadChips();
    const md::Screen &screen = m_readFlow->screen();
    if (screen.start.floatRow >= 0 && screen.start.floatRow < int(m_rows.size()))
        m_readLinePosition = linePositionAt(screen.start.floatRow, 0);
    else if (screen.start.row < int(m_rows.size()))
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
    invalidateReadChips();
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
        if (readActive()) {
            const ReadLayout &layout = readLayout();
            news.fit = layout.fit;
            news.count = layout.count;
            news.chars = layout.shownChars;
            if (m_readFlow && !m_readFlow->screen().empty()) {
                const md::Screen &screen = m_readFlow->screen();
                const md::LineRange range = m_readFlow->lineRange();
                news.start = screen.start;
                news.end = screen.end;
                news.first = range.first;
                news.last = range.last;
            }
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
    const md::Segment &segment = m_readFlow->screen().columns[hit.column].segments[hit.index];
    if (segment.part != md::Segment::Part::Row)
        return false;
    const int index = segment.row;
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
    if (s.part != md::Segment::Part::Row)
        return {};
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
    const md::Segment &s = m_readFlow->screen().columns[hit.column].segments[hit.index];
    if (s.part != md::Segment::Part::Row)
        return -1; // (A chip, a note: no row's.)
    *origin = readOrigin(hit);
    return s.row;
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
            if (segments[i].row == index && segments[i].part == md::Segment::Part::Row)
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
    m_readChips.resize(screen.columns.size());
    // A thin line down the middle of each gap.
    for (int c = 1; c < int(screen.columns.size()); ++c) {
        painter.fillRect(QRect(columns::dividerX(l.left + c * (l.width + l.gap), l.gap), l.top, 1, l.height),
                         theme::color(theme::Role::divider));
    }
    for (int c = 0; c < int(screen.columns.size()); ++c) {
        const qreal left = l.left + c * (l.width + l.gap);
        const QRectF band(left - readMargin, 0, l.width + 2 * readMargin, viewport()->height());
        if (!band.intersects(event->rect()))
            continue;
        m_readChips[c].clear();
        qreal y = l.top;
        qreal keptBottom = -1;
        const auto &segments = screen.columns[c].segments;
        for (int i = 0; i < int(segments.size()); ++i) {
            const md::Segment &s = segments[i];
            if (s.part != md::Segment::Part::Row) {
                const qreal h = m_readFlow->drawnHeight(s);
                paintReadPart(painter, segments, i, c, QRectF(left, y, l.width, h));
                y += h;
                continue;
            }
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
            if (h > 0 && s.yTo < rowHeight && r.rendered) {
                // A table cut here: its line closes the part.
                const RenderedBlock *block = renderedForRow(r);
                const RenderedBlock::TableGrid *grid = block ? block->tableGrid() : nullptr;
                if (grid && block->breaks().table && block->breaks().lineColor.isValid()) {
                    const qreal slide = readOffset(s.row);
                    const qreal from = qMax<qreal>(left, left + grid->xs.front() - slide);
                    const qreal to = qMin<qreal>(left + l.width, left + grid->xs.back() - slide);
                    if (to > from)
                        painter.fillRect(QRectF(from, y + h - 1, to - from, 1), block->breaks().lineColor);
                }
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
    paintReadPictureGlow(painter, event->rect());
}

QFont VisualView::readNoteFont()
{
    QFont font = renderContext().font;
    font.setPointSizeF(font.pointSizeF() * 0.9);
    return font;
}

QString VisualView::readPictureName(int row)
{
    const Row &r = m_rows[row].row;
    const RenderedBlock *block = r.rendered ? renderedForRow(r) : nullptr;
    return block && block->kind() == RenderedBlock::Kind::Image ? tr("Diagram") : tr("Image");
}

// What a chip says, and the notes over and under moved pictures.
QString VisualView::readPartText(const std::vector<md::Segment> &segments, int index)
{
    const md::Segment &s = segments[index];
    const auto [first, last] = m_readFlow->pictureGroup(s.row);
    int total = 0;
    int k = 0; // The picture's place in its group, from 1.
    for (int r = first; r <= last; ++r) {
        if (m_readRows->kind(r) != md::RowKind::Picture)
            continue;
        ++total;
        if (r == s.row)
            k = total;
    }
    switch (s.part) {
    case md::Segment::Part::FromNote: {
        const QString from = s.page ? tr("from the previous page") : tr("from the previous column");
        int shown = 0;
        for (int i = index + 1; i < int(segments.size()) && segments[i].part == md::Segment::Part::Row; ++i) {
            if (m_readRows->kind(segments[i].row) == md::RowKind::Picture)
                ++shown;
        }
        if (k == 1 && shown == total)
            return total == 1 ? tr("%1 %2").arg(readPictureName(s.row), from) : tr("%1 images %2").arg(total).arg(from);
        if (shown <= 1)
            return tr("Image %1 of %2 %3").arg(k).arg(total).arg(from);
        return tr("Images %1\u2013%2 of %3 %4").arg(k).arg(k + shown - 1).arg(total).arg(from);
    }
    case md::Segment::Part::OnNote: {
        const QString where = s.page ? tr("on the next page") : tr("in the next column");
        if (k == total)
            return tr("Image %1 continues %2 \u203A").arg(k).arg(where);
        return tr("Images %1\u2013%2 continue %3 \u203A").arg(k).arg(total).arg(where);
    }
    case md::Segment::Part::Chip:
    case md::Segment::Part::Row:
        break;
    }
    return {};
}

void VisualView::paintReadPart(QPainter &painter, const std::vector<md::Segment> &segments, int index,
                               int column, const QRectF &rect)
{
    const md::Segment &s = segments[index];
    if (s.part == md::Segment::Part::Chip) {
        int lastRow;
        const auto chip = makeReadChip(s.row, column, &lastRow);
        m_readChips[column].push_back({s.row, lastRow, column, chip->chipRect().translated(rect.topLeft())});
        painter.save();
        chip->paint(painter, rect.topLeft(), rect.width(), {});
        painter.restore();
        return;
    }
    painter.save();
    painter.setFont(readNoteFont());
    RenderedBlock::drawSecondaryText(painter, rect.adjusted(0, 0, 0, -2),
                                     Qt::AlignLeft | Qt::AlignBottom | Qt::TextSingleLine, readPartText(segments, index));
    painter.restore();
}

std::unique_ptr<RenderedBlock> VisualView::makeReadChip(int row, int column, int *lastRow)
{
    *lastRow = m_readFlow->pictureGroup(row).second;
    int count = 0;
    for (int r = row; r <= *lastRow; ++r) {
        if (m_readRows->kind(r) == md::RowKind::Picture)
            ++count;
    }
    const QString text = count == 1 ? readPictureName(row) : tr("%1 images").arg(count);
    const bool lastColumn = column + 1 == int(m_readFlow->screen().columns.size());
    const QString where = lastColumn ? tr("\u00B7 on the next page") : tr("\u00B7 in the next column");
    return RenderedBlock::createChip({text, where}, renderContext());
}

int VisualView::readChipAt(const QPointF &point)
{
    if (!readActive() || ensureReadScreen().empty())
        return -1;
    const ReadHit hit = readHitAt(point, true);
    if (hit.index < 0)
        return -1;
    const md::Segment &s = m_readFlow->screen().columns[hit.column].segments[hit.index];
    if (s.part != md::Segment::Part::Chip)
        return -1;
    // On the chip's box, not beside it.
    const auto chip = RenderedBlock::createChip({QStringLiteral("x"), {}}, renderContext());
    const qreal top = hit.top + chip->chipRect().top();
    if (point.y() < top || point.y() >= top + chip->chipRect().height() + 2 * 2)
        return -1;
    return s.row;
}

const VisualView::ReadChip *VisualView::paintedReadChipAt(const QPointF &point) const
{
    if (!readActive() || !m_readFlow || m_readDirty || m_readNeedsStart
        || m_readFlow->geometry().revision != m_readRevision)
        return nullptr;
    for (const auto &column : m_readChips) {
        for (const ReadChip &chip : column) {
            if (chip.box.contains(point))
                return &chip;
        }
    }
    return nullptr;
}

bool VisualView::readChipHovered(QMouseEvent *event)
{
    const ReadChip *chip = paintedReadChipAt(event->position());
    if (isTouchMouse(event))
        return chip != nullptr;
    if (!chip)
        clearReadChipPreview();
    else if (m_readPreviewRow != chip->row || m_readPreviewTouch)
        showReadChipPreview(*chip, false);
    return chip != nullptr;
}

std::optional<VisualView::ReadChip> VisualView::readChipForPress(const QPointF &point)
{
    if (const ReadChip *chip = paintedReadChipAt(point))
        return *chip;
    // A tap can arrive before the repaint queued by a caret or page change.
    const ReadHit hit = readHitAt(point, true);
    if (hit.index < 0)
        return std::nullopt;
    const md::Segment &segment = m_readFlow->screen().columns[hit.column].segments[hit.index];
    if (segment.part != md::Segment::Part::Chip)
        return std::nullopt;
    int lastRow;
    const auto chip = makeReadChip(segment.row, hit.column, &lastRow);
    const QRectF box = chip->chipRect().translated(readColumnRect(hit.column).left(), hit.top);
    if (!box.contains(point))
        return std::nullopt;
    return ReadChip{segment.row, lastRow, hit.column, box};
}

bool VisualView::readChipPressed(QMouseEvent *event)
{
    if (readActive() && event->button() == Qt::LeftButton) {
        if (isTouchMouse(event)) {
            if (const auto chip = readChipForPress(event->position())) {
                event->accept();
                const int row = chip->row;
                // The second tap, or a first with nothing to show (pictures
                // no longer decoded): what a click does.
                if ((m_readPreviewTouch && m_readPreviewRow == row) || !showReadChipPreview(*chip, true)) {
                    clearReadChipPreview();
                    openMovedPictures(row);
                }
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonPress) {
            const int row = readChipAt(event->position());
            clearReadChipPreview();
            if (row >= 0) {
                event->accept();
                openMovedPictures(row);
                return true;
            }
        }
    }
    clearReadChipPreview();
    return false;
}

bool VisualView::eventFilter(QObject *watched, QEvent *event)
{
    // A tap on a page button or elsewhere in the window also dismisses the preview.
    if (m_readPreviewTouch && watched != viewport() && watched->isWidgetType()) {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)
            clearReadChipPreview();
        if (event->type() == QEvent::TouchBegin) {
            const auto *touch = static_cast<QTouchEvent *>(event);
            if (touch->points().isEmpty())
                return TextView::eventFilter(watched, event);
            const QPointF point = viewport()->mapFromGlobal(touch->points().front().globalPosition());
            // The touch may pass through a parent before Qt synthesizes a mouse press.
            if (!paintedReadChipAt(point))
                clearReadChipPreview();
        }
    }
    return TextView::eventFilter(watched, event);
}

bool VisualView::showReadChipPreview(ReadChip chip, bool touch)
{
    clearReadChipPreview();
    m_readPreviewRow = chip.row;
    m_readPreviewTouch = touch;
    if (touch)
        qApp->installEventFilter(this);

    const auto cachedBlock = [this](int row) -> const RenderedBlock * {
        const RowInfo &info = m_rows[row];
        if (!info.row.rendered || info.span < 0)
            return nullptr;
        if (const Render *render = memoized(info.row))
            return render->block.get();
        // The row may have left the view's recent renders, but the cache can still hold it.
        const auto key = m_renderKeys.find(info.row.firstLine);
        const Render *render = key != m_renderKeys.end() ? m_cache.object(key->second.key) : nullptr;
        return render ? render->block.get() : nullptr;
    };
    const md::Screen &screen = m_readFlow->screen();
    for (int c = chip.column + 1; c < int(screen.columns.size()); ++c) {
        const auto &segments = screen.columns[c].segments;
        if (segments.empty() || segments.front().part != md::Segment::Part::FromNote
            || segments.front().row < chip.row || segments.front().row > chip.lastRow)
            break;
        const QRect column = readColumnRect(c);
        int y = column.top() + m_readFlow->drawnHeight(segments.front());
        for (size_t i = 1; i < segments.size(); ++i) {
            const md::Segment &segment = segments[i];
            if (segment.row > chip.lastRow || segment.part != md::Segment::Part::Row)
                break;
            const int height = segment.yTo - segment.yFrom;
            if (const RenderedBlock *block = cachedBlock(segment.row)) {
                const QRectF piece(column.left(), y, column.width(), height);
                const QPointF origin(piece.left(), piece.top() - segment.yFrom);
                for (const RenderedBlock::Figure &figure : block->figures()) {
                    const QRectF rect = figure.rect.translated(origin).intersected(piece);
                    if (!rect.isEmpty()) {
                        m_readPictureGlow.append(rect);
                        viewport()->update(rect.adjusted(-glowRoom, -glowRoom, glowRoom, glowRoom).toAlignedRect());
                    }
                }
            }
            y += height;
        }
    }

    // A group can continue on the next page after filling the remaining columns.
    const int next = screen.end.floatRow;
    if (next < chip.row || next > chip.lastRow)
        return !m_readPictureGlow.isEmpty();
    QList<QImage> pictures;
    for (int row = next; row <= chip.lastRow; ++row) {
        if (const RenderedBlock *block = cachedBlock(row))
            pictures.append(block->picturePreviews());
    }
    if (pictures.isEmpty())
        return !m_readPictureGlow.isEmpty();
    if (!m_readChipPreview)
        m_readChipPreview = new ReadChipPreview(this);
    const QRect box(viewport()->mapToGlobal(chip.box.topLeft().toPoint()), chip.box.size().toSize());
    const QScreen *screenAtChip = QGuiApplication::screenAt(box.center());
    m_readChipPreview->showPictures(std::move(pictures), box,
                                   (screenAtChip ? screenAtChip : viewport()->screen())->availableGeometry());
    return true;
}

void VisualView::clearReadChipPreview()
{
    if (m_readPreviewRow < 0)
        return;
    if (m_readPreviewTouch)
        qApp->removeEventFilter(this);
    m_readPreviewTouch = false;
    m_readPreviewRow = -1;
    if (m_readChipPreview)
        m_readChipPreview->clearPictures();
    for (const QRectF &rect : std::as_const(m_readPictureGlow))
        viewport()->update(rect.adjusted(-glowRoom, -glowRoom, glowRoom, glowRoom).toAlignedRect());
    m_readPictureGlow.clear();
}

void VisualView::invalidateReadChips()
{
    clearReadChipPreview();
    m_readChips.clear();
}

void VisualView::paintReadPictureGlow(QPainter &painter, const QRect &dirty)
{
    if (m_readPictureGlow.isEmpty())
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    const QColor accent = theme::color(theme::Role::accent);
    for (const QRectF &rect : std::as_const(m_readPictureGlow)) {
        if (!rect.adjusted(-glowRoom, -glowRoom, glowRoom, glowRoom).intersects(dirty))
            continue;
        const QRectF border = rect.adjusted(-1, -1, 1, 1);
        QColor glow = accent;
        for (int width = 10; width >= 4; width -= 2) {
            glow.setAlphaF(accent.alphaF() * (12 - width) / 48);
            painter.setPen(QPen(glow, width));
            painter.drawRoundedRect(border, 3, 3);
        }
        painter.setPen(QPen(accent, 2));
        painter.drawRoundedRect(border, 3, 3);
    }
    painter.restore();
}

// The viewer, for the pictures of a chip's group (from the first of the
// group, so it can step back to those shown in place), at the chip's.
void VisualView::openMovedPictures(int row)
{
    const auto [first, last] = m_readFlow->pictureGroup(row);
    std::vector<ImageViewer::Entry> pictures;
    int index = 0;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (int r = first; r <= last; ++r) {
        if (m_readRows->kind(r) != md::RowKind::Picture)
            continue;
        const RenderedBlock *block = m_rows[r].row.rendered ? renderedForRow(m_rows[r].row) : nullptr;
        const auto picture = block ? block->firstPicture() : std::nullopt;
        if (!picture)
            continue;
        QImage image = block->pictureImage(*picture);
        if (image.isNull())
            continue;
        if (r == row)
            index = int(pictures.size());
        pictures.push_back({std::move(image), picture->title});
    }
    QApplication::restoreOverrideCursor();
    if (pictures.empty())
        return;
    // Closes itself; the window is the parent so it stays on top of this tab.
    (new ImageViewer(std::move(pictures), index, window()))->show();
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

qreal VisualView::touchPageLineHeight()
{
    return readActive() ? readLineHeight() : TextView::touchPageLineHeight();
}

void VisualView::touchTurnPage(bool forward)
{
    if (readActive())
        forward ? readPageForward() : readPageBack();
    else
        TextView::touchTurnPage(forward);
}

bool VisualView::touchCanNudge(int direction)
{
    if (!readActive())
        return TextView::touchCanNudge(direction);
    const md::Screen &screen = ensureReadScreen();
    return !screen.empty() && (direction > 0 ? !screen.atEnd : !screen.atStart);
}

bool VisualView::touchNudge(int lines)
{
    if (!readActive())
        return TextView::touchNudge(lines);
    if (ensureReadScreen().empty())
        return false;
    const auto before = m_readFlow->screen().start;
    readNudge(lines);
    return m_readFlow->screen().start != before;
}

bool VisualView::touchTapActivates(const QPointF &point)
{
    // Picture chips handle each tap through the existing click handler.
    return readActive() && readChipAt(point) >= 0;
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
