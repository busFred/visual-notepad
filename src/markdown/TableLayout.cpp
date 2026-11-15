#include "TableLayout.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QStyle>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextLayout>
#include <QTextList>
#include <QTextTable>

#include <algorithm>
#include <atomic>
#include <climits>

namespace vn {

namespace {

std::atomic<bool> enabled{true};
std::atomic<int> brokenCells{0};
// Whether this Qt lets a layout's lines be made again without beginLayout()
// (see breakLines); a build of Qt that checks says no once.
std::atomic<bool> rebreaks{true};

// Qt's fixed point (QFixed, qfixed_p.h): 1/64 px in an int, with its
// conversions and roundings, so that the sums below come out as Qt's.
struct Fixed
{
    int v = 0;
    constexpr Fixed() = default;
    constexpr Fixed(int pixels) : v(pixels * 64) {}
    static constexpr Fixed raw(int value)
    {
        Fixed f;
        f.v = value;
        return f;
    }
    static constexpr Fixed fromReal(qreal r) { return raw(int(r * qreal(64))); }
    constexpr qreal toReal() const { return qreal(v) / qreal(64); }
    constexpr Fixed round() const { return raw((v + 32) & -64); }
    constexpr Fixed ceil() const { return raw((v + 63) & -64); }
    constexpr Fixed operator+(Fixed o) const { return raw(v + o.v); }
    constexpr Fixed operator-(Fixed o) const { return raw(v - o.v); }
    Fixed &operator+=(Fixed o)
    {
        v += o.v;
        return *this;
    }
    Fixed &operator-=(Fixed o)
    {
        v -= o.v;
        return *this;
    }
    constexpr Fixed operator*(int i) const { return raw(v * i); }
    constexpr Fixed operator/(int i) const { return raw(v / i); }
    Fixed operator*(Fixed o) const
    {
        bool negative = false;
        qint64 a = v, b = o.v;
        if (a < 0) {
            a = -a;
            negative = true;
        }
        if (b < 0) {
            b = -b;
            negative = !negative;
        }
        const int result = int((a * b + 0x20L) >> 6);
        return raw(negative ? -result : result);
    }
    Fixed operator/(Fixed o) const
    {
        if (o.v == 0)
            return raw(0x7FFFFFFF);
        bool negative = false;
        qint64 a = v, b = o.v;
        if (a < 0) {
            a = -a;
            negative = true;
        }
        if (b < 0) {
            b = -b;
            negative = !negative;
        }
        const int result = int(((a << 6) + (b >> 1)) / b);
        return raw(negative ? -result : result);
    }
    friend constexpr bool operator==(Fixed a, Fixed b) = default;
    friend constexpr auto operator<=>(Fixed a, Fixed b) { return a.v <=> b.v; }
};

constexpr Fixed fixedMax = Fixed::raw(INT_MAX); // QFIXED_MAX

// Where a paragraph goes in its frame or cell (QTextLayoutStruct, as much of
// it as matters without floats or pages), and what laying it out found.
struct Flow
{
    Fixed left, right; // x_left, x_right
    Fixed y;
    Fixed textWidth;      // contentsWidth: the widest line, from the frame's left.
    Fixed least;          // minimumWidth
    Fixed blockWidest;    // maximumWidth of the last paragraph alone (0: none)
};

// isEmptyBlockBeforeTable and isEmptyBlockAfterTable of Qt's layout: the
// empty paragraphs Qt keeps in front of and after a table are hidden on its
// edges.
bool emptyBeforeTable(const QTextBlock &block, const QTextBlockFormat &format, const QTextFrame::iterator &next)
{
    return !next.atEnd() && qobject_cast<QTextTable *>(next.currentFrame()) && block.isValid() && block.length() == 1
           && !format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)
           && !format.hasProperty(QTextFormat::BackgroundBrush)
           && next.currentFrame()->firstPosition() == block.position() + 1;
}

bool emptyAfterTable(const QTextBlock &block, const QTextFrame *previous)
{
    return qobject_cast<const QTextTable *>(previous) && block.isValid() && block.length() == 1
           && previous->lastPosition() == block.position() - 1;
}

// getLineHeightParams of Qt's layout, without a paint device (no scaling).
void lineHeights(const QTextBlockFormat &format, const QTextLine &line, Fixed *adjustment, Fixed *height, Fixed *bottom)
{
    const qreal lineHeight = line.height();
    const int type = format.lineHeightType();
    const qreal raw = qCeil(line.ascent() + line.descent() + line.leading());
    *height = Fixed::fromReal(format.lineHeight(raw, 1));
    *bottom = Fixed::fromReal(format.lineHeight(lineHeight, 1));
    if (type == QTextBlockFormat::FixedHeight)
        *adjustment = Fixed::fromReal(line.ascent() + qMax(line.leading(), qreal(0))) - (*height * 4) / 5;
    else if (type == QTextBlockFormat::MinimumHeight)
        *adjustment = Fixed::fromReal(lineHeight) - *height;
    else
        *adjustment = 0;
}

// The first line of a paragraph's new layout. Qt's beginLayout() drops the
// shaped text (QTextEngine::invalidate) and shapes it again as the lines are
// made, which is most of the time a table's layout takes; a layout whose
// shaped text is kept (QTextLayout::setCacheEnabled) can instead have its
// lines dropped (clearLayout) and made again from that text, which gives the
// same lines (only the layout's minimumWidth and maximumWidth then keep
// growing, so they are read after a fresh layout only). Qt allows that in
// its release builds; a build that checks refuses the line, and then it is
// beginLayout() from then on.
QTextLine breakLines(QTextLayout *layout, bool fresh)
{
    layout->setCacheEnabled(true);
    if (!fresh && rebreaks) {
        // (Makes the layout's items again if Qt dropped them, as it does
        // when the text or its formats change.)
        layout->isValidCursorPosition(0);
        layout->clearLayout();
        const QTextLine line = layout->createLine();
        if (line.isValid())
            return line;
        rebreaks = false;
    }
    layout->beginLayout();
    return layout->createLine();
}

} // namespace

struct TableLayout::Data
{
    QTextDocument *doc = nullptr;
    QTextTable *table = nullptr;

    struct Cell
    {
        std::vector<QTextBlock> blocks;
        Fixed pad[4]; // Top, right, bottom, left (topPadding() and so on).
        // The least and widest width of its text by wrap mode (and again
        // with lines not broken, without a text width), from a layout as
        // wide as Qt makes it to find them (widest fixedMax: none).
        struct Measure
        {
            bool made = false;
            Fixed least;
            Fixed widest;
        } measures[10];
        int laidMode = -1; // The wrap mode and width its lines are broken
        Fixed laidWidth;   // at now; -1: other lines.
        Fixed height;      // Of its text.
    };
    int rows = 0;
    int columns = 0;
    std::vector<Cell> cells; // Row by row.
    int firstBlockNumber = 0;
    std::vector<int> cellOfBlock; // By block number from firstBlockNumber; -1: not in the table.

    bool dirty = true;     // To be laid out (again).
    bool restart = true;   // The text changed: everything to be found again.
    bool unchecked = true; // The settings below may have changed.
    int length = -1;       // Of the document, in characters.
    // What the cells' measures and lines depend on besides the widths.
    QFont font;
    QTextOption option;
    qreal indentWidth = 0;
    QTextFrameFormat rootFormat;
    QTextFormat tableFormat; // Without its column widths.
    // The document has no text width (lines are not broken), as far as the
    // layout being made goes.
    bool manual = false;

    // The layout.
    bool laid = false;
    Fixed rootWidth, rootHeight, rootLeftMargin, rootRightMargin;
    Fixed rootTextWidth;
    Fixed tableX, tableY, tableWidth, tableHeight;
    Fixed tableBorder;
    std::vector<Fixed> columnPositions, widths, rowPositions, heights;
    // Qt's check points of the root frame (y, position): where drawing and
    // hit testing start from and drawing stops.
    std::vector<std::pair<Fixed, int>> checkPoints;

    Cell &cell(int row, int column) { return cells[size_t(row * columns + column)]; }
    const Cell *cellOf(const QTextBlock &block) const
    {
        const int i = block.blockNumber() - firstBlockNumber;
        if (i < 0 || i >= int(cellOfBlock.size()) || cellOfBlock[size_t(i)] < 0)
            return nullptr;
        return &cells[size_t(cellOfBlock[size_t(i)])];
    }
    int indexOf(const Cell *cell) const { return int(cell - cells.data()); }

    bool sameSettings() const;
    void keepSettings();
    void startOver();
    void prepare();
    void ensure();
    void layoutRoot();
    void layoutTable(Fixed parentWidth, bool linesToo);
    const Cell::Measure &measure(Cell &cell, int mode);
    Fixed layoutCell(Cell &cell, Fixed width, int mode);
    void layoutBlock(const QTextBlock &block, const QTextBlockFormat &format, const QTextBlockFormat *previous,
                     Flow &flow, bool fresh, int mode);
    Fixed blockIndent(const QTextBlockFormat &format) const;
    // Drawing, as drawFrame, drawTableCell and drawBlock draw.
    QTextFrame::iterator startAt(Fixed y) const;
    void drawTable(const QPointF &offset, QPainter *painter, const PaintContext &context, const QRectF &clipRect) const;
    void drawBlock(const QPointF &offset, QPainter *painter, const PaintContext &context, const QTextBlock &block,
                   bool beforeTable, const QRectF &clipRect) const;
    // Hit testing, as Qt's hitTest overloads.
    enum Hit { PointBefore, PointAfter, PointInside, PointExact };
    Hit hitBlock(const QTextBlock &block, QPointF point, int *position, QTextLayout **layout,
                 Qt::HitTestAccuracy accuracy) const;
    Hit hitTable(QPointF point, int *position, QTextLayout **layout, Qt::HitTestAccuracy accuracy) const;
};

bool TableLayout::Data::sameSettings() const
{
    QTextOption now = doc->defaultTextOption();
    QTextFormat format = table->format();
    format.clearProperty(QTextFormat::TableColumnWidthConstraints);
    return doc->defaultFont() == font && now.alignment() == option.alignment() && now.flags() == option.flags()
           && now.tabStopDistance() == option.tabStopDistance() && now.tabs() == option.tabs()
           && now.textDirection() == option.textDirection() && doc->indentWidth() == indentWidth
           && doc->rootFrame()->frameFormat() == rootFormat && format == tableFormat;
}

void TableLayout::Data::keepSettings()
{
    font = doc->defaultFont();
    option = doc->defaultTextOption();
    indentWidth = doc->indentWidth();
    rootFormat = doc->rootFrame()->frameFormat();
    tableFormat = table->format();
    tableFormat.clearProperty(QTextFormat::TableColumnWidthConstraints);
}

// Finds the cells and their paragraphs again, and forgets what was measured.
void TableLayout::Data::startOver()
{
    rows = table->rows();
    columns = table->columns();
    cells.assign(size_t(rows * columns), Cell());
    firstBlockNumber = doc->findBlock(table->firstPosition()).blockNumber();
    cellOfBlock.assign(size_t(doc->findBlock(table->lastPosition()).blockNumber() - firstBlockNumber + 1), -1);
    const QTextTableFormat format = table->format();
    const Fixed padding = Fixed::fromReal(format.cellPadding());
    static const int paddings[4] = {QTextFormat::TableCellTopPadding, QTextFormat::TableCellRightPadding,
                                    QTextFormat::TableCellBottomPadding, QTextFormat::TableCellLeftPadding};
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const QTextTableCell tableCell = table->cellAt(row, column);
            Cell &c = cell(row, column);
            // QTextTableData::paddingProperty (and no border, see suits()).
            const QTextCharFormat cellFormat = tableCell.format();
            for (int edge = 0; edge < 4; ++edge) {
                const QVariant v = cellFormat.property(paddings[edge]);
                c.pad[edge] = v.isNull() ? padding : Fixed::fromReal(v.toReal());
            }
            // (Not by firstCursorPosition(): a cursor asks the layout where it is.)
            for (QTextBlock b = doc->findBlock(tableCell.firstPosition());
                 b.isValid() && b.position() <= tableCell.lastPosition(); b = b.next()) {
                c.blocks.push_back(b);
                cellOfBlock[size_t(b.blockNumber() - firstBlockNumber)] = row * columns + column;
            }
        }
    }
}

// The cells, and their measures still good for the document's settings.
void TableLayout::Data::prepare()
{
    const bool again = restart || (unchecked && !sameSettings());
    restart = false;
    unchecked = false;
    if (again) {
        startOver();
        keepSettings();
    }
}

void TableLayout::Data::ensure()
{
    prepare();
    if (!dirty || !doc->isLayoutEnabled() || doc->pageSize().isNull())
        return;
    dirty = false;
    manual = doc->pageSize().width() < 0;
    layoutRoot();
    laid = true;
}

// layoutFrame and layoutFlow of the root frame: the empty paragraphs and the
// table.
void TableLayout::Data::layoutRoot()
{
    const QTextFrameFormat format = doc->rootFrame()->frameFormat();
    const Fixed top = Fixed::fromReal(format.topMargin()).round();
    const Fixed bottom = Fixed::fromReal(format.bottomMargin()).round();
    rootLeftMargin = Fixed::fromReal(format.leftMargin()).round();
    rootRightMargin = Fixed::fromReal(format.rightMargin()).round();
    const Fixed border = Fixed::fromReal(format.border()).round();
    const Fixed padding = Fixed::fromReal(format.padding()).round();
    const qreal maximumWidth = qMax(qreal(0), doc->pageSize().width());
    const Fixed width = Fixed::fromReal(format.width().value(maximumWidth));
    const Fixed contentsWidth = width - (border + padding) * 2 - rootLeftMargin - rootRightMargin;

    Flow flow;
    flow.left = rootLeftMargin + border + padding;
    flow.right = flow.left + contentsWidth;
    flow.y = top + border + padding;
    checkPoints.assign(1, {flow.y, 0});
    QTextFrame::iterator previous;
    QTextBlockFormat previousFormat;
    for (QTextFrame::iterator it = doc->rootFrame()->begin(); !it.atEnd();) {
        if (qAbs((flow.y - checkPoints.back().first).v) > Fixed(2000).v)
            checkPoints.push_back({flow.y, it.currentFrame() ? it.currentFrame()->firstPosition() : it.currentBlock().position()});
        if (it.currentFrame()) {
            Fixed x = flow.left;
            layoutTable(contentsWidth, true);
            const Qt::Alignment align = table->format().alignment() & Qt::AlignHorizontal_Mask;
            if (flow.right - flow.left > tableWidth) {
                if (align & Qt::AlignRight)
                    x += flow.right - tableWidth;
                else if (align & Qt::AlignHCenter)
                    x += (flow.right - tableWidth) / 2;
            }
            tableX = x;
            tableY = flow.y;
            flow.y += tableHeight;
            previous = it;
            ++it;
            continue;
        }
        QTextFrame::iterator last;
        if (!previous.atEnd() && previous != it)
            last = previous;
        previous = it;
        const QTextBlock block = it.currentBlock();
        ++it;
        const QTextBlockFormat blockFormat = block.blockFormat();
        const Fixed y = flow.y;
        layoutBlock(block, blockFormat, last.currentBlock().isValid() ? &previousFormat : nullptr, flow, true,
                    doc->defaultTextOption().wrapMode());
        if (emptyBeforeTable(block, blockFormat, it)) {
            const QTextBlock lastBlock = last.currentBlock();
            const qreal lastBottom = lastBlock.isValid() ? lastBlock.blockFormat().bottomMargin() : 0.0;
            flow.y = y + Fixed::fromReal(qMax(lastBottom, blockFormat.topMargin()));
        } else if (emptyAfterTable(block, last.currentFrame())) {
            QTextLayout *layout = block.layout();
            layout->setPosition(QPointF((tableX + tableWidth).toReal(),
                                        (tableY + tableHeight).toReal() - layout->boundingRect().height()));
            flow.y = y;
        }
        previousFormat = blockFormat;
    }
    checkPoints.push_back({flow.y, doc->characterCount()});
    rootTextWidth = flow.textWidth;
    const Fixed marginWidth = (border + padding) * 2 + rootLeftMargin + rootRightMargin;
    rootHeight = flow.y + border + padding + bottom;
    rootWidth = std::max(contentsWidth, std::max(tableWidth, flow.textWidth)) + marginWidth;
}

// layoutFrame and layoutTable of the table in a frame `parentWidth` wide:
// the columns, and with linesToo the cells' lines and the rows.
void TableLayout::Data::layoutTable(Fixed parentWidth, bool linesToo)
{
    const QTextTableFormat format = table->format();
    const Fixed width = Fixed::fromReal(format.width().value(qMax(qreal(0), parentWidth.toReal())));
    const Fixed topMargin = Fixed::fromReal(format.topMargin()).round();
    const Fixed bottomMargin = Fixed::fromReal(format.bottomMargin()).round();
    const Fixed leftMargin = Fixed::fromReal(format.leftMargin()).round();
    const Fixed rightMargin = Fixed::fromReal(format.rightMargin()).round();
    const Fixed border = tableBorder = Fixed::fromReal(format.border()).round();
    const Fixed padding = Fixed::fromReal(format.padding()).round();
    Fixed contentsWidth = width - (border + padding) * 2 - leftMargin - rightMargin;

    const Fixed borderCell = border; // (No collapsed borders.)
    const Fixed spacing = Fixed::fromReal(format.cellSpacing()).round();
    const Fixed left = leftMargin + padding + border;
    const Fixed top = topMargin + padding + border;
    QList<QTextLength> constraints = format.columnWidthConstraints();
    if (constraints.size() != columns)
        constraints.resize(columns);

    const int wrap = doc->defaultTextOption().wrapMode();
    int mode = wrap;
    std::vector<Fixed> minWidths, maxWidths;
    for (;;) {
        Fixed remaining = contentsWidth;
        remaining -= borderCell * (columns * 2);
        remaining -= spacing * (columns - 1);
        remaining -= spacing * 2;
        const Fixed initialTotal = remaining;
        widths.assign(size_t(columns), Fixed());
        minWidths.assign(size_t(columns), Fixed(1));
        maxWidths.assign(size_t(columns), fixedMax);
        for (int i = 0; i < columns; ++i) {
            for (int row = 0; row < rows; ++row) {
                Cell &c = cell(row, i);
                const Cell::Measure &m = measure(c, mode);
                const Fixed widthPadding = c.pad[3] + c.pad[1];
                minWidths[size_t(i)] = std::max(minWidths[size_t(i)], m.least + widthPadding).ceil();
                Fixed maxW = maxWidths[size_t(i)];
                if (m.widest != fixedMax)
                    maxW = maxW == fixedMax ? m.widest + widthPadding : std::max(maxW, m.widest + widthPadding);
                if (maxW == fixedMax)
                    continue;
                maxW = maxW.ceil();
                Fixed w = maxW;
                if (maxWidths[size_t(i)] != fixedMax)
                    w = std::max(maxWidths[size_t(i)], w);
                maxWidths[size_t(i)] = std::max(minWidths[size_t(i)], w);
            }
        }
        Fixed totalPercentage;
        int variableColumns = 0;
        Fixed totalMinWidth;
        for (int i = 0; i < columns; ++i) {
            const QTextLength &length = constraints.at(i);
            if (length.type() == QTextLength::FixedLength) {
                minWidths[size_t(i)] = widths[size_t(i)] =
                    std::max(Fixed::fromReal(length.rawValue()), minWidths[size_t(i)]);
                remaining -= widths[size_t(i)];
            } else if (length.type() == QTextLength::PercentageLength) {
                totalPercentage += Fixed::fromReal(length.rawValue());
            } else if (length.type() == QTextLength::VariableLength) {
                ++variableColumns;
                widths[size_t(i)] = minWidths[size_t(i)];
                remaining -= minWidths[size_t(i)];
            }
            totalMinWidth += minWidths[size_t(i)];
        }
        {
            const Fixed totalPercentagedWidth = initialTotal * totalPercentage / 100;
            Fixed remainingMinWidths = totalMinWidth;
            for (int i = 0; i < columns; ++i) {
                remainingMinWidths -= minWidths[size_t(i)];
                if (constraints.at(i).type() != QTextLength::PercentageLength)
                    continue;
                const Fixed allotted = Fixed::fromReal(constraints.at(i).rawValue());
                const Fixed percentWidth = totalPercentagedWidth * allotted / totalPercentage;
                const Fixed most = remaining - remainingMinWidths;
                if (percentWidth >= minWidths[size_t(i)] && most > minWidths[size_t(i)])
                    widths[size_t(i)] = std::clamp(percentWidth, minWidths[size_t(i)], std::max(minWidths[size_t(i)], most));
                else
                    widths[size_t(i)] = minWidths[size_t(i)];
                remaining -= widths[size_t(i)];
            }
        }
        if (variableColumns > 0 && remaining > 0) {
            std::vector<int> bounded;
            for (int i = 0; i < columns; ++i) {
                if (constraints.at(i).type() == QTextLength::VariableLength && maxWidths[size_t(i)] != fixedMax)
                    bounded.push_back(i);
            }
            Fixed lastRemaining = remaining;
            while (remaining > 0) {
                for (size_t k = 0; k < bounded.size(); ++k) {
                    const size_t col = size_t(bounded[k]);
                    const int left = int(bounded.size() - k);
                    const Fixed w = std::min(maxWidths[col] - widths[col], remaining / left);
                    widths[col] += w;
                    remaining -= w;
                }
                if (remaining == lastRemaining)
                    break;
                lastRemaining = remaining;
            }
            if (remaining > 0 && format.width().type() != QTextLength::VariableLength) {
                const Fixed share = remaining / variableColumns;
                for (int col = 0; col < columns; ++col) {
                    if (constraints.at(col).type() == QTextLength::VariableLength)
                        widths[size_t(col)] += share;
                }
            }
        }
        // Whole pixels, the rounding error carried on to the next column.
        Fixed error;
        for (int i = 0; i < columns; ++i) {
            const Fixed original = widths[size_t(i)];
            widths[size_t(i)] = (widths[size_t(i)] - error).round();
            error = widths[size_t(i)] - original;
        }
        columnPositions.assign(size_t(columns), Fixed());
        columnPositions[0] = left + spacing + border;
        for (int i = 1; i < columns; ++i)
            columnPositions[size_t(i)] = columnPositions[size_t(i - 1)] + widths[size_t(i - 1)] + borderCell * 2 + spacing;
        const Fixed newWidth = columnPositions.back() + widths.back() + padding + border + spacing - left;
        // Too wide even so: Qt measures again with words broken anywhere.
        if (mode == QTextOption::WrapAtWordBoundaryOrAnywhere && newWidth > contentsWidth) {
            mode = QTextOption::WrapAnywhere;
            continue;
        }
        contentsWidth = newWidth;
        break;
    }
    tableWidth = leftMargin + border + padding + contentsWidth + rightMargin + border + padding;
    if (!linesToo)
        return;

    heights.assign(size_t(rows), Fixed());
    rowPositions.assign(size_t(rows), Fixed());
    rowPositions[0] = top + spacing + border;
    for (int r = 0; r < rows; ++r) {
        if (r > 0)
            rowPositions[size_t(r)] = rowPositions[size_t(r - 1)] + heights[size_t(r - 1)] + borderCell + spacing + borderCell;
        for (int c = 0; c < columns; ++c) {
            Cell &one = cell(r, c);
            const Fixed text = layoutCell(one, widths[size_t(c)] - (one.pad[3] + one.pad[1]), wrap);
            heights[size_t(r)] = std::max(heights[size_t(r)], (text + one.pad[2] + one.pad[0]).round());
        }
    }
    tableHeight = rowPositions.back() + heights.back() + padding + border + spacing + bottomMargin + border + padding;
}

// The first pass of layoutTable over a cell (layoutCell at QFIXED_MAX):
// its least and widest widths, kept.
const TableLayout::Data::Cell::Measure &TableLayout::Data::measure(Cell &cell, int mode)
{
    Cell::Measure &m = cell.measures[mode + (manual ? 5 : 0)];
    if (m.made)
        return m;
    Flow flow;
    flow.left = 0;
    flow.right = fixedMax;
    Fixed widest;
    QTextBlockFormat previousFormat;
    bool first = true;
    for (const QTextBlock &block : cell.blocks) {
        const QTextBlockFormat format = block.blockFormat();
        flow.blockWidest = 0;
        layoutBlock(block, format, first ? nullptr : &previousFormat, flow, true, mode);
        widest = std::max(widest, flow.blockWidest);
        previousFormat = format;
        first = false;
    }
    m.made = true;
    m.least = flow.least;
    m.widest = widest > 0 ? widest : fixedMax;
    cell.laidMode = -1; // Its lines are the unbounded ones now,
    dirty = true;       // to be broken again.
    return m;
}

// The second pass (layoutCell at the column's width): the cell's lines,
// broken again only when the width or the wrap mode changed. Returns the
// height of its text.
Fixed TableLayout::Data::layoutCell(Cell &cell, Fixed width, int mode)
{
    mode += manual ? 5 : 0;
    if (cell.laidMode == mode && cell.laidWidth == width)
        return cell.height;
    Flow flow;
    flow.left = 0;
    flow.right = width;
    QTextBlockFormat previousFormat;
    bool first = true;
    for (const QTextBlock &block : cell.blocks) {
        const QTextBlockFormat format = block.blockFormat();
        layoutBlock(block, format, first ? nullptr : &previousFormat, flow, false, mode % 5);
        previousFormat = format;
        first = false;
    }
    cell.laidMode = mode;
    cell.laidWidth = width;
    cell.height = flow.y;
    ++brokenCells;
    return cell.height;
}

Fixed TableLayout::Data::blockIndent(const QTextBlockFormat &format) const
{
    qreal indent = format.indent();
    if (QTextObject *object = doc->objectForFormat(format))
        indent += object->format().toListFormat().indent();
    if (qIsNull(indent))
        return 0;
    return Fixed::fromReal(indent * doc->indentWidth());
}

// layoutBlock of Qt's layout, without floats, pages or a paint device.
void TableLayout::Data::layoutBlock(const QTextBlock &block, const QTextBlockFormat &format,
                                    const QTextBlockFormat *previous, Flow &flow, bool fresh, int mode)
{
    if (!block.isVisible())
        return;
    QTextLayout *layout = block.layout();
    if (previous)
        flow.y += Fixed::fromReal(qMax(format.topMargin(), previous->bottomMargin()));
    const Qt::LayoutDirection direction = block.textDirection();
    const QTextOption &documentOption = doc->defaultTextOption();
    Fixed extraMargin;
    if (documentOption.flags() & QTextOption::AddSpaceForLineAndParagraphSeparators)
        extraMargin = Fixed::fromReal(QFontMetricsF(block.charFormat().font()).horizontalAdvance(u'\x21B5'));
    const Fixed indent = blockIndent(format);
    const bool rtl = direction == Qt::RightToLeft;
    const Fixed totalLeft = Fixed::fromReal(format.leftMargin()) + (rtl ? extraMargin : indent);
    const Fixed totalRight = Fixed::fromReal(format.rightMargin()) + (rtl ? indent : extraMargin);
    layout->setPosition(QPointF(flow.left.toReal(), flow.y.toReal()));

    QTextOption option = documentOption;
    option.setWrapMode(QTextOption::WrapMode(mode));
    option.setTextDirection(direction);
    option.setTabs(format.tabPositions());
    Qt::Alignment align = documentOption.alignment();
    if (format.hasProperty(QTextFormat::BlockAlignment))
        align = format.alignment();
    option.setAlignment(QStyle::visualAlignment(direction, align));
    if (format.nonBreakableLines() || manual)
        option.setWrapMode(QTextOption::ManualWrap);
    layout->setTextOption(option);
    const bool anyWrap = option.wrapMode() == QTextOption::WrapAtWordBoundaryOrAnywhere;

    const Fixed cy = flow.y;
    const Fixed l = flow.left + totalLeft;
    const Fixed r = flow.right - totalRight;
    Fixed bottom;
    bool firstLine = true;
    for (QTextLine line = breakLines(layout, fresh); line.isValid(); line = layout->createLine()) {
        line.setLeadingIncluded(true);
        Fixed left = std::max(flow.left, l), right = std::min(flow.right, r);
        Fixed textIndent;
        if (firstLine) {
            textIndent = Fixed::fromReal(format.textIndent());
            if (rtl)
                right -= textIndent;
            else
                left += textIndent;
            firstLine = false;
        }
        line.setLineWidth((right - left).toReal());
        left = std::max(flow.left, l);
        right = std::min(flow.right, r);
        if (rtl)
            right -= textIndent;
        else
            left += textIndent;
        if (Fixed::fromReal(line.naturalTextWidth()) > right - left) {
            line.setLineWidth((right - left).toReal());
            if (Fixed::fromReal(line.naturalTextWidth()) > right - left) {
                if (anyWrap) {
                    option.setWrapMode(QTextOption::WrapAnywhere);
                    layout->setTextOption(option);
                }
                line.setLineWidth(qMax<qreal>(line.naturalTextWidth(), (right - left).toReal()));
                if (anyWrap) {
                    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
                    layout->setTextOption(option);
                }
            }
        }
        Fixed adjustment, height, lineBottom;
        lineHeights(format, line, &adjustment, &height, &lineBottom);
        line.setPosition(QPointF((left - flow.left).toReal(), (flow.y - cy - adjustment).toReal()));
        bottom = flow.y + lineBottom;
        flow.y += height;
        flow.textWidth = std::max(flow.textWidth, Fixed::fromReal(line.x() + line.naturalTextWidth()) + totalRight);
    }
    flow.y = std::max(flow.y, bottom);
    layout->endLayout();

    const Fixed margins = totalLeft + totalRight;
    flow.least = std::max(flow.least, Fixed::fromReal(layout->minimumWidth()) + margins);
    const Fixed widest = Fixed::fromReal(layout->maximumWidth()) + margins;
    flow.blockWidest = widest > 0 ? widest : Fixed();
}

bool TableLayout::suits(const QTextDocument &doc, const QTextTable &table)
{
    if (!enabled)
        return false;
    const QTextFrameFormat root = doc.rootFrame()->frameFormat();
    if (root.border() != 0 || root.background().style() != Qt::NoBrush
        || root.height().type() != QTextLength::VariableLength
        || (doc.defaultTextOption().flags() & QTextOption::AddSpaceForLineAndParagraphSeparators))
        return false;
    const QTextTableFormat format = table.format();
    const int rows = table.rows(), columns = table.columns();
    if (rows <= 0 || columns <= 0 || table.parentFrame() != doc.rootFrame() || format.borderCollapse()
        || format.border() != 0 || format.background().style() != Qt::NoBrush
        || format.height().type() != QTextLength::VariableLength || !table.childFrames().isEmpty())
        return false;
    static const int borders[8] = {QTextFormat::TableCellTopBorder,        QTextFormat::TableCellRightBorder,
                                   QTextFormat::TableCellBottomBorder,     QTextFormat::TableCellLeftBorder,
                                   QTextFormat::TableCellTopBorderStyle,   QTextFormat::TableCellRightBorderStyle,
                                   QTextFormat::TableCellBottomBorderStyle, QTextFormat::TableCellLeftBorderStyle};
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            const QTextTableCell cell = table.cellAt(row, column);
            const QTextCharFormat f = cell.format();
            if (cell.rowSpan() != 1 || cell.columnSpan() != 1 || f.verticalAlignment() == QTextCharFormat::AlignMiddle
                || f.verticalAlignment() == QTextCharFormat::AlignBottom || f.background().style() != Qt::NoBrush)
                return false;
            for (const int property : borders) {
                if (f.hasProperty(property))
                    return false;
            }
        }
    }
    // Paragraphs: no list, background or rule (drawn by Qt's layout), no
    // side margins (see fitTable) and nothing but text.
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        const QTextBlockFormat f = b.blockFormat();
        if (b.textList() || f.background().style() != Qt::NoBrush
            || f.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth) || f.leftMargin() != 0
            || f.rightMargin() != 0 || f.indent() != 0 || f.textIndent() != 0
            || b.text().contains(QChar::ObjectReplacementCharacter))
            return false;
    }
    return true;
}

void TableLayout::setEnabled(bool on)
{
    enabled = on;
}

int TableLayout::cellsBroken()
{
    return brokenCells;
}

TableLayout::TableLayout(QTextDocument *doc, QTextTable *table)
    : QAbstractTextDocumentLayout(doc)
    , d(std::make_unique<Data>())
{
    d->doc = doc;
    d->table = table;
}

TableLayout::~TableLayout() = default;

void TableLayout::documentChanged(int from, int charsRemoved, int charsAdded)
{
    // Qt drops the shaped text of paragraphs whose text or formats change;
    // an edit that changes the length starts the cells over. (A new text
    // width, default text option or font comes as the whole document added.)
    const int length = d->doc->characterCount();
    if (length != d->length || (charsRemoved != charsAdded && !(from == 0 && charsRemoved == 0 && charsAdded == length)))
        d->restart = true;
    d->length = length;
    d->dirty = true;
    d->unchecked = true;
}

QSizeF TableLayout::documentSize() const
{
    d->ensure();
    return QSizeF(d->rootWidth.toReal(), d->rootHeight.toReal());
}

int TableLayout::pageCount() const
{
    d->ensure();
    const QSizeF page = d->doc->pageSize();
    if (page.height() < 0)
        return 1;
    return qCeil(d->rootHeight.toReal() / page.height());
}

QRectF TableLayout::frameBoundingRect(QTextFrame *frame) const
{
    if (!d->doc->isLayoutEnabled() || d->doc->pageSize().isNull())
        return QRectF();
    d->ensure();
    if (frame == d->doc->rootFrame())
        return QRectF(0, 0, d->rootWidth.toReal(), d->rootHeight.toReal());
    if (frame != d->table)
        return QRectF();
    // As Qt's: the table's position, plus that of the cell its first
    // position is in (the first cell's).
    const Data::Cell &first = d->cells.front();
    const QPointF cell((d->columnPositions[0] + first.pad[3]).toReal(), (d->rowPositions[0] + first.pad[0]).toReal());
    return QRectF(QPointF(d->tableX.toReal(), d->tableY.toReal()) + cell,
                  QSizeF(d->tableWidth.toReal(), d->tableHeight.toReal()));
}

QRectF TableLayout::blockBoundingRect(const QTextBlock &block) const
{
    if (!d->doc->isLayoutEnabled() || d->doc->pageSize().isNull() || !block.isValid() || !block.isVisible())
        return QRectF();
    d->ensure();
    QPointF offset;
    if (const Data::Cell *cell = d->cellOf(block)) {
        const int i = d->indexOf(cell);
        const int row = i / d->columns, column = i % d->columns;
        offset = QPointF(d->tableX.toReal(), d->tableY.toReal())
                 + QPointF((d->columnPositions[size_t(column)] + cell->pad[3]).toReal(),
                           (d->rowPositions[size_t(row)] + cell->pad[0]).toReal());
    }
    const QTextLayout *layout = block.layout();
    QRectF rect = layout->boundingRect();
    rect.moveTopLeft(layout->position() + offset);
    return rect;
}

TableLayout::Columns TableLayout::columnsAt(qreal textWidth) const
{
    d->prepare();
    Columns result;
    const QTextFrameFormat format = d->doc->rootFrame()->frameFormat();
    const Fixed border = Fixed::fromReal(format.border()).round();
    const Fixed padding = Fixed::fromReal(format.padding()).round();
    const Fixed left = Fixed::fromReal(format.leftMargin()).round();
    const Fixed right = Fixed::fromReal(format.rightMargin()).round();
    const Fixed width = Fixed::fromReal(format.width().value(qMax(qreal(0), textWidth)));
    const Fixed contentsWidth = width - (border + padding) * 2 - left - right;
    // The columns at that width, the lines left as they are (the table is
    // laid out again for the width it is given next).
    const std::vector<Fixed> positions = d->columnPositions, widths = d->widths;
    const Fixed tableWidth = d->tableWidth;
    d->manual = false;
    d->layoutTable(contentsWidth, false);
    const Fixed marginWidth = (border + padding) * 2 + left + right;
    result.idealWidth = std::max(d->tableWidth, d->rootTextWidth).toReal() + marginWidth.toReal();
    // The table's place in the root frame then (see layoutRoot).
    const Fixed xLeft = left + border + padding, xRight = xLeft + contentsWidth;
    Fixed x = xLeft;
    const Qt::Alignment align = d->table->format().alignment() & Qt::AlignHorizontal_Mask;
    if (xRight - xLeft > d->tableWidth) {
        if (align & Qt::AlignRight)
            x += xRight - d->tableWidth;
        else if (align & Qt::AlignHCenter)
            x += (xRight - d->tableWidth) / 2;
    }
    const int mode = d->doc->defaultTextOption().wrapMode();
    for (int column = 0; column < d->columns; ++column) {
        // The first paragraph of the cell in the first row, broken for its
        // column (its width is the widest of its lines').
        Data::Cell &cell = d->cell(0, column);
        const Fixed text = d->widths[size_t(column)] - (cell.pad[3] + cell.pad[1]);
        qreal textWidth = 0;
        if (!cell.blocks.empty() && cell.blocks.front().isVisible() && cell.laidMode == mode && cell.laidWidth == text) {
            textWidth = cell.blocks.front().layout()->boundingRect().width(); // (Broken so already.)
        } else if (!cell.blocks.empty() && cell.blocks.front().isVisible()) {
            Flow flow;
            flow.left = 0;
            flow.right = text;
            const QTextBlock &first = cell.blocks.front();
            d->layoutBlock(first, first.blockFormat(), nullptr, flow, false, mode);
            textWidth = first.layout()->boundingRect().width();
            cell.laidMode = -1;
        }
        result.textLeft.push_back(x.toReal() + (d->columnPositions[size_t(column)] + cell.pad[3]).toReal());
        result.textWidth.push_back(textWidth);
    }
    d->columnPositions = positions;
    d->widths = widths;
    d->tableWidth = tableWidth;
    d->dirty = true;
    return result;
}

int TableLayout::leastWidth(int row, int column) const
{
    d->prepare();
    d->manual = false; // (Measured as laid out at a text width.)
    return d->measure(d->cell(row, column), d->doc->defaultTextOption().wrapMode()).least.v;
}

void TableLayout::draw(QPainter *painter, const PaintContext &context)
{
    d->ensure();
    if (!d->laid)
        return;
    const QRectF clipRect = QRectF(0, 0, d->rootWidth.toReal(), d->rootHeight.toReal())
                                .adjusted(d->rootLeftMargin.toReal(), 0, -d->rootRightMargin.toReal(), 0);
    const QRectF &clip = context.clip;
    // drawFrame of the root frame (no decoration, see suits()).
    if (clip.isValid()
        && (0 > clip.bottom() || d->rootHeight.toReal() < clip.top() || 0 > clip.right() || d->rootWidth.toReal() < clip.left()))
        return;
    // drawFlow stops at the first check point at or below the clip's bottom.
    int stop = INT_MAX;
    if (clip.isValid()) {
        const Fixed bottom = Fixed::fromReal(clip.bottom());
        const auto point = std::find_if(d->checkPoints.begin(), d->checkPoints.end(),
                                        [&](const std::pair<Fixed, int> &p) { return p.first >= bottom; });
        if (point != d->checkPoints.end())
            stop = point->second;
    }
    QTextBlock cursorBlock;
    QTextFrame *previousFrame = nullptr;
    QTextBlock previousBlock;
    for (QTextFrame::iterator it = d->startAt(Fixed::fromReal(clip.top())); !it.atEnd(); ++it) {
        QTextFrame *frame = it.currentFrame();
        const int position = frame ? frame->firstPosition() : it.currentBlock().position();
        if (position >= stop)
            break;
        QTextFrame::iterator next = it;
        ++next;
        if (frame) {
            d->drawTable(QPointF(), painter, context, clipRect);
        } else {
            const QTextBlock block = it.currentBlock();
            const bool before = emptyBeforeTable(block, block.blockFormat(), next);
            if (emptyAfterTable(block, previousFrame)) {
                PaintContext without = context;
                without.selections.clear();
                d->drawBlock(QPointF(), painter, without, block, before, clipRect);
            } else {
                d->drawBlock(QPointF(), painter, context, block, before, clipRect);
            }
        }
        // The cursor in the empty paragraph before the table is drawn after it.
        if (previousBlock.isValid() && emptyBeforeTable(previousBlock, previousBlock.blockFormat(), it)
            && previousBlock.contains(context.cursorPosition))
            cursorBlock = previousBlock;
        previousBlock = it.currentBlock();
        previousFrame = frame;
    }
    if (cursorBlock.isValid()) {
        const QPen pen = painter->pen();
        painter->setPen(context.palette.color(QPalette::Text));
        cursorBlock.layout()->drawCursor(painter, QPointF(), context.cursorPosition - cursorBlock.position(), 1);
        painter->setPen(pen);
    }
}

// frameIteratorForYPosition of Qt's layout: the item of the root frame at
// the check point before y.
QTextFrame::iterator TableLayout::Data::startAt(Fixed y) const
{
    QTextFrame::iterator it = doc->rootFrame()->begin();
    if (checkPoints.empty() || y < 0 || y > rootHeight)
        return it;
    auto point = std::find_if(checkPoints.begin(), checkPoints.end(),
                              [&](const std::pair<Fixed, int> &p) { return p.first >= y; });
    if (point == checkPoints.end())
        return it;
    if (point != checkPoints.begin())
        --point;
    for (; !it.atEnd(); ++it) {
        if ((it.currentFrame() ? it.currentFrame()->firstPosition() : it.currentBlock().position()) >= point->second)
            break;
    }
    return it;
}

void TableLayout::Data::drawTable(const QPointF &offset, QPainter *painter, const PaintContext &context,
                                  const QRectF &clipRect) const
{
    const QPointF off = QPointF((offset + QPointF(tableX.toReal(), tableY.toReal())).toPoint());
    const QRectF &clip = context.clip;
    if (clip.isValid()
        && (off.y() > clip.bottom() || off.y() + tableHeight.toReal() < clip.top() || off.x() > clip.right()
            || off.x() + tableWidth.toReal() < clip.left()))
        return;
    const qsizetype count = context.selections.size();
    std::vector<int> selected(size_t(count * 4), -1);
    for (qsizetype i = 0; i < count; ++i) {
        const Selection &s = context.selections.at(i);
        int rowStart = -1, columnStart = -1, rowCount = -1, columnCount = -1;
        if (s.cursor.currentTable() == table)
            s.cursor.selectedTableCells(&rowStart, &rowCount, &columnStart, &columnCount);
        selected[size_t(i * 4)] = rowStart;
        selected[size_t(i * 4 + 1)] = columnStart;
        selected[size_t(i * 4 + 2)] = rowCount;
        selected[size_t(i * 4 + 3)] = columnCount;
    }
    int firstRow = 0, lastRow = rows;
    if (clip.isValid()) {
        auto it = std::lower_bound(rowPositions.begin(), rowPositions.end(), Fixed::fromReal(clip.top() - off.y()));
        if (it != rowPositions.end() && it != rowPositions.begin())
            firstRow = int(--it - rowPositions.begin());
        it = std::upper_bound(rowPositions.begin(), rowPositions.end(), Fixed::fromReal(clip.bottom() - off.y()));
        if (it != rowPositions.end())
            lastRow = int(++it - rowPositions.begin());
    }
    const qreal border = tableBorder.toReal();
    for (int r = firstRow; r < lastRow; ++r) {
        for (int c = 0; c < columns; ++c) {
            const Cell &one = cells[size_t(r * columns + c)];
            QRectF cellRect(columnPositions[size_t(c)].toReal(), rowPositions[size_t(r)].toReal(),
                            widths[size_t(c)].toReal(), heights[size_t(r)].toReal());
            cellRect.translate(off);
            if (clip.isValid() && !cellRect.adjusted(-border, -border, border, border).intersects(clip))
                continue;
            // adjustContextSelectionsForCell
            PaintContext cellContext = context;
            if (count > 0) {
                const QTextTableCell tableCell = table->cellAt(r, c);
                for (qsizetype i = 0; i < count; ++i) {
                    const int rowStart = selected[size_t(i * 4)], columnStart = selected[size_t(i * 4 + 1)];
                    const int rowCount = selected[size_t(i * 4 + 2)], columnCount = selected[size_t(i * 4 + 3)];
                    Selection &s = cellContext.selections[i];
                    if (rowStart != -1) {
                        if (r >= rowStart && r < rowStart + rowCount && c >= columnStart && c < columnStart + columnCount) {
                            const int firstPosition = tableCell.firstPosition();
                            int lastPosition = tableCell.lastPosition();
                            if (firstPosition == lastPosition)
                                ++lastPosition;
                            s.cursor.setPosition(firstPosition);
                            s.cursor.setPosition(lastPosition, QTextCursor::KeepAnchor);
                        } else {
                            s.cursor.clearSelection();
                        }
                    }
                    s.format.clearProperty(QTextFormat::FullWidthSelection);
                }
            }
            // drawTableCell (no border, background or vertical offset, see
            // suits()) and drawFlow of the cell.
            const QPointF cellPosition(cellRect.left() + one.pad[3].toReal(), cellRect.top() + one.pad[0].toReal());
            for (const QTextBlock &block : one.blocks)
                drawBlock(cellPosition, painter, cellContext, block, false, clipRect);
        }
    }
}

void TableLayout::Data::drawBlock(const QPointF &offset, QPainter *painter, const PaintContext &context,
                                  const QTextBlock &block, bool beforeTable, const QRectF &clipRect) const
{
    const QTextLayout *layout = block.layout();
    QRectF r = layout->boundingRect();
    r.translate(offset + layout->position());
    const QRectF &clip = context.clip;
    if (!block.isVisible() || (clip.isValid() && (r.bottom() < clip.y() || r.top() > clip.bottom())))
        return;
    QList<QTextLayout::FormatRange> selections;
    const int position = block.position();
    const int length = block.length();
    for (const Selection &range : context.selections) {
        const int start = range.cursor.selectionStart() - position;
        const int end = range.cursor.selectionEnd() - position;
        if (start < length && end > 0 && end > start) {
            QTextLayout::FormatRange o;
            o.start = start;
            o.length = end - start;
            o.format = range.format;
            selections.append(o);
        } else if (!range.cursor.hasSelection() && range.format.hasProperty(QTextFormat::FullWidthSelection)
                   && block.contains(range.cursor.position())) {
            QTextLayout::FormatRange o;
            const QTextLine line = layout->lineForTextPosition(range.cursor.position() - position);
            o.start = line.textStart();
            o.length = line.textLength();
            if (o.start + o.length == length - 1)
                ++o.length;
            o.format = range.format;
            selections.append(o);
        }
    }
    const QPen pen = painter->pen();
    painter->setPen(context.palette.color(QPalette::Text));
    layout->draw(painter, offset, selections, clip.isValid() ? (clip & clipRect) : clipRect);
    if (!beforeTable
        && ((context.cursorPosition >= position && context.cursorPosition < position + length)
            || (context.cursorPosition < -1 && !layout->preeditAreaText().isEmpty()))) {
        int cursor = context.cursorPosition;
        if (cursor < -1)
            cursor = layout->preeditAreaPosition() - (cursor + 2);
        else
            cursor -= position;
        layout->drawCursor(painter, offset, cursor, 1);
    }
    painter->setPen(pen);
}

TableLayout::Data::Hit TableLayout::Data::hitBlock(const QTextBlock &block, QPointF point, int *position,
                                                   QTextLayout **result, Qt::HitTestAccuracy accuracy) const
{
    QTextLayout *layout = block.layout();
    QRectF text = layout->boundingRect();
    text.translate(layout->position());
    *position = block.position();
    if (point.y() < text.top() - block.blockFormat().topMargin())
        return PointBefore;
    if (point.y() > text.bottom()) {
        *position += block.length();
        return PointAfter;
    }
    const QPointF at = point - layout->position();
    Hit hit = PointInside;
    *result = layout;
    int offset = 0;
    for (int i = 0; i < layout->lineCount(); ++i) {
        const QTextLine line = layout->lineAt(i);
        const QRectF rect = line.naturalTextRect();
        if (rect.top() > at.y()) {
            offset = qMin(offset, line.textStart());
        } else if (rect.bottom() <= at.y()) {
            offset = qMax(offset, line.textStart() + line.textLength());
        } else {
            if (rect.left() <= at.x() && rect.right() >= at.x())
                hit = PointExact;
            offset = line.xToCursor(at.x(), accuracy == Qt::ExactHit ? QTextLine::CursorOnCharacter
                                                                     : QTextLine::CursorBetweenCharacters);
            break;
        }
    }
    *position += offset;
    return hit;
}

TableLayout::Data::Hit TableLayout::Data::hitTable(QPointF point, int *position, QTextLayout **layout,
                                                   Qt::HitTestAccuracy accuracy) const
{
    const Fixed x = Fixed::fromReal(point.x()), y = Fixed::fromReal(point.y());
    auto rowIt = std::lower_bound(rowPositions.begin(), rowPositions.end(), y);
    if (rowIt == rowPositions.end())
        rowIt = rowPositions.end() - 1;
    else if (rowIt != rowPositions.begin())
        --rowIt;
    auto columnIt = std::lower_bound(columnPositions.begin(), columnPositions.end(), x);
    if (columnIt == columnPositions.end())
        columnIt = columnPositions.end() - 1;
    else if (columnIt != columnPositions.begin())
        --columnIt;
    const int row = int(rowIt - rowPositions.begin()), column = int(columnIt - columnPositions.begin());
    const QTextTableCell tableCell = table->cellAt(row, column);
    if (!tableCell.isValid())
        return PointBefore;
    *position = tableCell.firstPosition();
    const Cell &one = cells[size_t(row * columns + column)];
    // Points relative to the cell's text, in Qt's fixed point.
    const Fixed cx = x - (columnPositions[size_t(column)] + one.pad[3]);
    const Fixed cy = y - (rowPositions[size_t(row)] + one.pad[0]);
    const QPointF inCell(cx.toReal(), cy.toReal());
    // hitTest over the cell's flow, from PointInside.
    Hit hit = PointInside;
    for (const QTextBlock &block : one.blocks) {
        int pos = -1;
        const Hit h = hitBlock(block, inCell, &pos, layout, accuracy);
        if (h >= PointInside) {
            hit = h;
            *position = pos;
            break;
        }
        if (h == PointBefore && pos < *position) {
            *position = pos;
            hit = h;
        } else if (h == PointAfter && pos > *position) {
            *position = pos;
            hit = h;
        }
    }
    if (hit == PointExact)
        return hit;
    if (hit == PointAfter)
        *position = tableCell.lastPosition();
    return PointInside;
}

int TableLayout::hitTest(const QPointF &point, Qt::HitTestAccuracy accuracy) const
{
    d->ensure();
    QTextFrame *root = d->doc->rootFrame();
    if (!d->laid)
        return accuracy == Qt::ExactHit ? -1 : 0;
    // Qt's hitTest of the root frame (at 0, 0) and of its flow, in its
    // fixed point.
    const Fixed x = Fixed::fromReal(point.x()), y = Fixed::fromReal(point.y());
    const QPointF p(x.toReal(), y.toReal());
    QTextLayout *layout = nullptr;
    QTextFrame::iterator it = d->startAt(y);
    int position = it.currentFrame() ? it.currentFrame()->firstPosition() : it.currentBlock().position();
    Data::Hit hit = Data::PointBefore;
    for (; !it.atEnd(); ++it) {
        Data::Hit h;
        int pos = -1;
        if (it.currentFrame()) {
            // hitTest of the table's frame.
            const Fixed rx = x - d->tableX, ry = y - d->tableY;
            if (ry < 0 || rx < 0) {
                pos = d->table->firstPosition() - 1;
                h = Data::PointBefore;
            } else if (ry > d->tableHeight || rx > d->tableWidth) {
                pos = d->table->lastPosition() + 1;
                h = Data::PointAfter;
            } else {
                h = d->hitTable(QPointF(rx.toReal(), ry.toReal()), &pos, &layout, accuracy);
            }
        } else {
            h = d->hitBlock(it.currentBlock(), p, &pos, &layout, accuracy);
        }
        if (h >= Data::PointInside) {
            QTextFrame::iterator next = it;
            ++next;
            if (!it.currentFrame() && emptyBeforeTable(it.currentBlock(), it.currentBlock().blockFormat(), next))
                continue;
            hit = h;
            position = pos;
            break;
        }
        if (h == Data::PointBefore && pos < position) {
            position = pos;
            hit = h;
        } else if (h == Data::PointAfter && pos > position) {
            position = pos;
            hit = h;
        }
    }
    if (accuracy == Qt::ExactHit && hit < Data::PointExact)
        return -1;
    int lastPosition = root->lastPosition();
    if (layout && !layout->preeditAreaText().isEmpty())
        lastPosition += int(layout->preeditAreaText().size());
    return qBound(0, position, lastPosition);
}

} // namespace vn
