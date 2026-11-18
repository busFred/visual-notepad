// The hybrid Markdown editor: every top-level block is shown rendered, and
// typing goes into rendered text as in a word processor (see keyPressEvent).
// A double click (or an edit rendered text cannot take) opens the block as
// its source until the caret leaves it, Esc or a click on blank space.
// Locked (read-only) shows everything rendered.
#pragma once

#include "../editor/TextView.h"
#include "BlockRenderer.h"
#include "Folding.h"
#include "ReadFlow.h"
#include "TableCellEditor.h"

#include <QCache>
#include <QCursor>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QTextCursor>

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

class QAbstractButton;
class QFrame;
class QLabel;
class QMenu;

namespace vn {

class MarkdownModel;
class TableCellEditor;
class FigureBar;
class TableToolbar;

// Map between a Rich block's rendered text and its Markdown source by
// matching characters in order (Markdown syntax that is not rendered is
// skipped). sourceOffsetFor: the source offset just after the rendered prefix;
// renderedOffsetFor: the inverse, the number of rendered characters that
// precede a source offset. For any prefix length k,
// renderedOffsetFor(rendered, source, sourceOffsetFor(rendered.left(k), source)) == k.
int sourceOffsetFor(const QString &renderedPrefix, const QString &source);
int renderedOffsetFor(const QString &rendered, const QString &source, int sourceOffset);
// Where a caret placed before rendered character k (after k characters)
// goes in the source: after the source of character k - 1, so typing there
// takes its formatting (a word processor's rule); at the start of the block
// or of a line of it, before the source of character k (after list markers).
int caretSourceOffset(const QString &rendered, const QString &source, int k);

class VisualView : public TextView
{
    Q_OBJECT

public:
    VisualView(MarkdownModel *model, QWidget *parent = nullptr);
    ~VisualView() override;

    void setBaseDirectory(const QString &directory);
    void setHighlightActiveBlock(bool on);
    // Blocks currently shown as source (opened, and the view has the focus).
    int expandedBlockCount() const { return int(m_expandedBlocks.size()); }
    // Whether the blocks holding the carets are open as source (they show
    // so while the view has the focus).
    bool isBlockOpen() const { return m_sourceOpen; }
    // Opens the blocks holding the carets as source, as a double click does;
    // closing shows everything rendered again (as Esc does).
    void openBlockAtCaret();
    void closeBlocks();
    // For tests: renders kept in hand besides the cache.
    int memoizedRenderCount() const { return int(m_renders.size()); }
    // For tests: whether a diagram is still being drawn; its picture, when it
    // comes, repaints the whole view.
    bool hasPendingDiagrams() const { return !m_pendingKeys.isEmpty(); }
    // For tests: blocks (and parts of blocks) rendered so far.
    int renderCount() const { return m_renderCount; }
    // For tests: the rendered text of the row showing a line (null for
    // source rows).
    QString renderedTextAt(int line);
    // For tests: the figures of the row showing a line, relative to the row's top-left.
    std::vector<RenderedBlock::Figure> figuresAtLine(int line);
    // For tests: off, long lists render whole as short ones do (see itemRows).
    static void setItemRowsEnabled(bool on);
    // For tests: off, an edit builds all the rows again, as it did before
    // they were patched (see patchRows).
    void setRowPatching(bool on) { m_patchRows = on; }
    // For tests: the rows built again from nothing, as they are after any
    // other change than an edit.
    void rebuildRowsFully()
    {
        m_rowsStale = true;
        rebuildRows();
    }
    // For tests: every row and what the rows hold (spans, item lists, lines
    // to rows) as text, to compare a patched build with a whole one.
    QString rowsDigest();
    // For tests: whether the last edit patched the rows.
    bool lastEditPatchedRows() const { return m_rowsPatched; }
    // For tests: the marker drawn for the long list's item that starts on
    // `line` ("12." or a bullet), or null if none is (not such an item, a
    // list of tasks).
    QString itemMarkerAt(int line);
    // A new table stays rendered, with its first header's box open (its
    // text selected, to type over), as a word processor does.
    void insertTable(int rows, int cols) override;
    // Collapsing. A table, figure, equation or code block shows as a chip
    // (RenderedBlock::createChip), a Markdown section as its heading with a
    // hint of what is hidden under it. Which are collapsed is kept for this
    // view only, in memory, as cursors on the first lines, so that it follows
    // edits; a fold whose block is gone (or is not of its kind any more) is
    // dropped.
    using FoldKind = folding::FoldKind;
    // (First line, kind) of every fold, to give another view of the document.
    using FoldState = std::vector<std::pair<int, FoldKind>>;
    FoldState foldState() const;
    void setFoldState(const FoldState &state);
    // Collapses every table, figure, equation and code block (not sections).
    void collapseAllBlocks();
    // Opens every collapsed block and section.
    void expandAll();
    // Collapses (or opens) the block, or the section under the heading, that
    // starts on `line`; false if there is none, or it is not collapsible.
    bool setFolded(int line, bool folded);
    bool isFolded(int line) const;
    int foldCount() const { return int(m_folds.size()); }
    // Opens what hides the carets (on purpose: find, go to line, the outline).
    void revealCaret() override;
    // The text was replaced wholesale (a file opened or read again): nothing
    // is collapsed but what the preference says.
    void documentReset();
    // For tests: the hint after the folded heading that starts on `line`
    // ("· 3 paragraphs hidden"), the label and detail of the chip of a
    // collapsed block (null for a row that is not a chip).
    QString foldHintAt(int line);
    // For tests: the room reserved over the block starting on `line` for its
    // strip (0 if it has none).
    qreal stripHeightAt(int line);
    // For tests: whether a click at this viewport point folds or opens something.
    bool isFoldSpot(const QPoint &point) { return foldSpotAt(point).where != FoldSpot::None; }
    QString chipTextAt(int line);
    // For tests: the pointer is over the row showing `line` (what the strip
    // and the heading's chevron wait for); -1: nowhere.
    void setHoverLine(int line);
    // For tests: where a click collapses (open) or opens (collapsed) the
    // block, or folds a section, that starts on `line`, in viewport coordinates.
    QRect foldSpotRect(int line);
    // For tests: where a cell of the rendered table starting at `line` is, in
    // viewport coordinates; null if it is not shown as a table.
    QRect tableCellViewRect(int line, int row, int col);

    // Read mode: what a locked Visual mode shows in Columns (DocumentTab
    // turns it on). The document flows through columns of `chars` characters of body text
    // (at the font and zoom of the moment), as many as fit or the number
    // asked for if fewer, centred, a screen at a time (md::ReadFlow): page
    // turns, nudges of a line, no scroll bar. Where the view is, is a source
    // line, which every change of layout keeps, and so does leaving read mode.
    // Before a large document's first parse (its source shows line by line)
    // the view scrolls as usual until the parse arrives. Turned on, the first
    // screen starts at startLine (-1: where the view is scrolled to).
    void setReadMode(bool on, int startLine = -1);
    bool isReadMode() const { return m_readMode; }
    // The source line the screen starts on in read mode (where leaving it
    // scrolls to).
    int readTopLine() const { return int(m_readLinePosition); }
    // 0: as many columns as fit.
    void setReadColumns(int columns);
    int readColumns() const { return m_readColumns; }
    void setReadChars(int chars);
    int readChars() const { return m_readChars; }
    // How many columns fit (what Auto uses), and how many show; 1 out of
    // read mode.
    int readMaxColumns();
    int readColumnCount();
    // The moves. Out of read mode they do nothing.
    void readPageForward();
    void readPageBack();
    void readNudge(int lines); // Negative: up.
    void readToStart();
    void readToEnd();
    // The source lines on screen (0-based, both included); (-1, -1) out of
    // read mode or before the first parse.
    std::pair<int, int> readLineRange();
    // Read mode shows columns (not the source standing in for a parse).
    bool showsReadColumns() const { return readActive(); }
    // For tests: the screen shown, where a column is, and where a segment's
    // part of its row is (without a repeated header), in the viewport.
    const md::Screen &readScreen();
    QRect readColumnRect(int column);
    QRect readSegmentRect(int column, int index);
    // For tests: how far the table or equation that starts on `line` is
    // scrolled sideways (0 if it is not wider than its column), the
    // scrollbar under a segment's part of one, and its Open button.
    qreal readOffsetOfLine(int line);
    QRect readBarRect(int column, int index);
    QRect readOpenRect(int line);
    // For tests: whether the render context asks for blocks that overflow.
    bool rendersOverflow() { return renderContext().overflow; }
    // For tests: the rendered block of the row holding `line`, or null.
    const RenderedBlock *renderedBlockOfLine(int line);
    // The page turns follow the lock, a page back or on, and the nudges the
    // keys and wheel do.
    void ensureCursorVisible() override;
    void centerCursor() override;
    void scrollPage(bool forward) override;

Q_SIGNALS:
    // A link was clicked in rendered text (not a task checkbox).
    void linkActivated(const QString &url);
    // Read mode: the screen, or the number of columns that fit, changed.
    void readChanged();

protected:
    int rowCount() const override;
    Row row(int index) const override;
    int rowOfLine(int line) const override;
    qreal renderedRowHeight(const Row &row, qreal width) override;
    void paintRenderedRow(QPainter &painter, const Row &row, const QRectF &rect) override;
    int hitTestRenderedRow(const Row &row, const QPointF &point) override;
    bool renderedRowPressed(const Row &row, const QPointF &point, QMouseEvent *event) override;
    std::vector<LineAnchor> rowLineAnchors(const Row &row, qreal height) override;
    void paintLineBackground(QPainter &painter, int line, const QRectF &rect) override;
    void rowsMayHaveChanged() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    bool event(QEvent *event) override;
    void editStarting() override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    // The commands on the selected cells, rows or columns (the context menu
    // of a selection); the labels count what is selected.
    void populateSelectionMenu(QMenu *menu);
    bool hasTableSelection() const { return m_tableSel.active; }
    QTextCursor lineUnitAt(int position) const override;
    void changeEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    qreal textWidth() const override;
    int rowAtPoint(const QPointF &point, QPointF *origin) override;
    int positionAt(const QPointF &point) override;
    bool caretRectsKnown() const override { return !readActive(); }

private:
    struct RowInfo
    {
        Row row;
        int span = -1; // Index into the model's spans, -1 for gaps.
        int item = -1; // Index into m_itemRows: a row of a long list's item.
        bool chip = false;   // A collapsed block: the row shows its chip.
        bool folded = false; // A heading whose section is folded: the row runs on over it.
        QString hint;        // ... "· 3 paragraphs hidden", shown after the heading.
    };
    // Long lists show an item a row (see itemRows): each item renders on its
    // own (RenderedBlock::createItem) and the view draws its marker, so that
    // typing in an item, or adding one, renders that item alone, however
    // long the list.
    struct ListItems; // The items of a list's HTML (VisualView.cpp).
    struct ItemList
    {
        bool ordered = false;
        bool markers = true; // Not a list of tasks only.
        int start = 1;
        int block = -1;      // The parsed list; its HTML holds the items not edited since.
        // Runs of items edited since the parse, each parsed again on its own.
        struct Live
        {
            QByteArray html;
            std::vector<QString> math;
        };
        std::vector<Live> live;
    };
    struct ItemRow
    {
        int list = -1;  // In m_itemLists.
        int index = 0;  // In the list.
        int from = 0;   // The item's <li>...</li> in its HTML.
        int to = 0;
        int line = 0;   // Its first line, from the first line of its HTML.
        int taskBase = 0; // The line its HTML's task boxes count from.
        int live = -1;  // In ItemList::live, or -1: in the parsed list.
        int parsed = -1; // Its index in the parsed list, if it is from it.
    };
    // A render and the source it was made from.
    struct Render
    {
        QString source;
        QString key;
        std::shared_ptr<RenderedBlock> block;
        core::BlockKind kind = core::BlockKind::Html;
        int cost = 0;     // KiB, of the block.
        // Markdown text (paragraphs, headings, lists, quotes) rather than
        // raw HTML, code or pictures: typing can go into its rendering.
        bool textual = false;
        quint64 used = 0; // The last frame (see m_frame) that asked for it.
        QString html; // A list item's (RenderedBlock::createItem): what it was made from.
        // What a table, figure, equation or code block says of itself (see
        // folding::chipText), for its strip and chip.
        QString chipLabel;
        QString chipDetail;
    };
    // What the view knows of a block it may show in parts (see
    // RenderedBlock::createPart), for one parse.
    struct LongBox
    {
        int parts = -1; // RenderedBlock::isLongCodeBox, -1 until asked.
        int wrap = -1;  // RenderedBlock::wrapsAnywhere, -1 until asked.
    };

    // Where the view is in the source: the line position at the top, and
    // whether the caret shows.
    struct Place
    {
        qreal top = 0;
        bool caretShown = false;
    };
    Place place();
    void restorePlace(const Place &place);
    void rebuildRows();
    // The rows of the one span an edit made, and the gaps around it, built
    // again in place of those they replace; the rows after move by the
    // lines and spans the edit added. False (nothing changed) when what the
    // rows are made from is not all at hand (see rebuildRows).
    bool patchRows(const std::vector<std::pair<int, int>> &expanded, const std::vector<int> &caretGaps,
                   const std::vector<int> &foldCarets, bool revealing, std::unordered_map<int, size_t> &foldAt,
                   std::vector<bool> &keepFold, const std::function<bool(int, int, size_t)> &build);
    // The kind of a block of the last parse, and the section a heading of it
    // heads: up to (not including) the block `end`, and what it holds as the
    // hint after the heading. Kept for one parse.
    folding::Fold blockClass(int blockIndex);
    struct Section
    {
        int end = 0;
        QString hint;
    };
    const Section &sectionOf(int blockIndex);
    // The fold on a line, or -1.
    int foldIndexAt(int line) const;
    void addFold(int line, FoldKind kind);
    void removeFold(int line);
    // The rows were laid out with other folds: forget what they showed.
    void foldsChanged();
    // The fold at `line` was clicked (the chevron, the chip, the strip).
    void toggleFoldAt(int line, FoldKind kind);
    struct FoldSpot
    {
        enum Where { None, Chevron, Chip, Strip, Open } where = None;
        int line = -1;
        qreal top = 0; // Of the row, in the viewport.
    };
    FoldSpot foldSpotAt(const QPointF &point);
    bool foldPressed(QMouseEvent *event);
    // The Open button at the right end of the strip of a block wider than
    // its column, in a row whose top-left corner is `rect`'s (read mode).
    QRectF openRect(const RenderedBlock *block, const QRectF &rect);
    // Shows the whole of a wide block in the picture viewer.
    bool openWideBlock(const Row &row);
    // The title of the heading the block that starts on `line` is under.
    QString headingBefore(int line);
    void paintStrip(QPainter &painter, const RenderedBlock *block, const Render *render, const QRectF &rect);
    void paintHeadingMarks(QPainter &painter, const RowInfo &info, const RenderedBlock *block, const QRectF &rect);
    RenderContext context() const;
    QFont bodyFont() const;
    // One line of rendered body text (blank-line gaps and rows not rendered yet are sized from it).
    qreal bodyLineHeight() const;
    // context(), made once for a frame (it reads fonts and settings).
    const RenderContext &renderContext();
    // The render of a rendered row (a block, or a part of one), or null.
    const Render *rendered(const Row &row);
    const RenderedBlock *renderedForRow(const Row &row);
    // The render kept for a row at the current width, or null; makes none.
    const Render *memoized(const Row &row) const;
    int spanForRow(const Row &row) const;
    LongBox &longBox(int blockIndex);
    bool mayBeLongList(int blockIndex);
    // Whether a span shows as a code box in parts.
    bool madeInParts(int span);
    // Adds rows for the items of the span (lines line..last) if it is a long
    // list that can be shown so; false (and nothing added) otherwise.
    bool itemRows(int span, int line, int last);
    // The items of a list of the last parse (blocks of either size), once a parse.
    const ListItems &parsedItems(int blockIndex);
    // The render of an item row (see ItemRow), made once per source and HTML.
    Render itemRender(const ItemRow &item, const QString &source, const RenderContext &ctx);
    Render itemRender(const QString &html, const QStringList &math, const QString &source, const RenderContext &ctx);
    // Renders the items of a list block of a parse made apart (see rendersAs)
    // as rows would: the block's text, or null if it is not a list shown so.
    QString itemsText(const core::Block &block, const QString &source, const RenderContext &ctx);
    const ItemRow *itemOfRow(int index) const;
    // The row index of an item next to the one in row `index` (direction -1
    // or 1), or -1 if there is none in its list.
    int neighbourItem(int index, int direction) const;
    // An item of the parsed list `blockIndex` renders irregularly
    // (RenderedBlock::isRegularItem): the list shows whole, this parse.
    void irregularItem(int blockIndex);
    // Notes an edit's lines (see m_editRanges).
    void noteEdit(int from, int removed, int added);
    void toggleTask(int line);
    bool zoomClick(const QMouseEvent *event) const;
    bool openPicture(const RenderedBlock *block, const QPointF &point, const QMouseEvent *event);
    RenderedSelection selectionForRow(const Row &row, const RenderedBlock *block, const QString &source) const;
    void clearRenderCache();
    // Diagrams or downloads finished: blocks that waited for them render again.
    void asyncRenderReady();
    bool remoteImagesAllowed() const;
    // Shows the bar asking whether to load the document's remote pictures
    // while that is undecided and the document has any.
    void updateRemoteBanner();
    void layoutBanner();
    void resetRenders();
    void reflowRenders();
    // The toolbar over the table being edited: shown while the caret is in a
    // table of an editable view with the focus; the update is made once the
    // event being handled is done.
    void scheduleToolbarUpdate();
    void updateTableToolbar();
    void toolbarOperation(md::TableOp op);

    // Pictures and diagrams fit a box (RenderContext::figureWidth and
    // figureHeight); pointing at one shows a bar to make it smaller or
    // larger, kept in memory for this view (m_figureScales).
    //
    // The most a picture's height may be: three quarters of what the view
    // shows, in steps, so that a window being resized does not render the
    // blocks with figures at every pixel.
    qreal figureBoxHeight() const;
    // Forgets the renders of blocks that may have figures; false if none was kept.
    bool dropFigureRenders();
    // Shows the bar over the figure under the point (viewport coordinates), or hides it.
    void updateFigureBar(const QPoint &point);
    void placeFigureBar(int line, const RenderedBlock::Figure &figure, const QPointF &rowOrigin);
    void hideFigureBar();
    void figureAction(int action); // A FigureBar::Action.

    // Cell editing in rendered tables: a click on a cell of a rendered
    // table (editable view) opens a text box over it instead of the table's
    // source; see TableCellEditor. The table commands act on that cell then.
    bool tableCellPressed(const Row &row, const QPointF &point, QMouseEvent *event);
    // `line` is the table's first line.
    bool openCellEditor(int line, int row, int col);
    void closeCellEditor(TableCellEditor::Exit how);
    // Puts the box over its cell; false if the cell is gone or has changed.
    bool placeCellEditor();
    void tableTargetEditing() override;

    // Editing in rendered text (VisualView.cpp, "Rendered editing").
    // Blocks edited in rendered text render from a parse of their own source
    // at once (a "live" render), not from the last parse of the document.
    // The live lines are those of the last edit made through this view.
    bool liveLines(int *first, int *last) const;
    // The blocks of a source parsed alone; the last few are kept.
    const rust::Vec<core::Block> &liveBlocks(const QString &source);
    // The render of a block of a live parse (lines firstLine.. of the
    // document); made once per source, kept in m_cache.
    Render liveRender(const core::Block &block, const QString &source, int firstLine, const RenderContext &ctx);
    QString renderKey(const RenderContext &ctx, core::BlockKind kind, const QString &source) const;
    // The rendered row holding a document position, with its render.
    struct Spot
    {
        int index = -1; // Row.
        int lastIndex = -1; // Several rows: the items of a long list.
        Row row;        // Of several rows: from the first's first line to the last's last.
        bool items = false; // Rows of a long list's items: parses of it render so (see rendersAs).
        int start = 0;  // Document position of the row.
        QString source;
        QString text;   // Rendered.
        std::shared_ptr<RenderedBlock> block;
        // Markdown text: not a table, raw HTML, a picture.
        bool textual = false;
        // Text that typing goes into (not too long to render per keystroke).
        bool editable = false;
        int offsetOf(int position) const; // Rendered offset of a document position.
        int positionOf(int k) const { return start + caretSourceOffset(text, source, k); }
    };
    std::optional<Spot> spotAt(int position);
    // The items in rows [first, last] of a long list as one spot (their
    // texts with a line break between them, as the whole list's).
    std::optional<Spot> spotOverItems(int first, int last);
    // The spot of a caret: its row, or the items its selection covers.
    std::optional<Spot> spotFor(const QTextCursor &cursor);
    // Whether `source` (lines from firstLine on) renders as `expected`
    // (whitespace aside); its renders are kept for the paint. `items`: a
    // long list renders an item at a time, as its rows will.
    bool rendersAs(const QString &source, int firstLine, const QString &expected, bool items = false);
    // Backspace (or Delete) over the line break in rendered character
    // `breakAt` of the spot's text: joins a list item with the one before
    // (after). False if it is not between two list items.
    bool joinItems(const Spot &spot, int breakAt);
    // Replaces [from, to) of the row's source (Spot::start on) as one edit
    // made by typing, and puts the caret at `caret` (a document position).
    void applyEdit(int from, int to, const QString &text, int caret);
    // Key handlers for a caret in rendered text; false: not handled.
    bool renderedKey(QKeyEvent *event);
    bool typeRendered(const QString &text);
    bool typeOnBlankLine(const QString &text);
    bool deleteRendered(bool forward);
    bool enterRendered();
    void moveRendered(int key, bool select);
    // An edit rendered text cannot take: open the caret's block as source
    // (at `caret` if not -1) and let the source editor make it.
    void openAndForward(QKeyEvent *event, int caret = -1);
    // Whether a viewport point is off the text: the side margins, a blank
    // line between blocks, below the end.
    bool blankAt(const QPointF &point);
    // The handlers for rendered text, after the table's (selection, edges).
    void typingKeyPress(QKeyEvent *event);
    void typingMousePress(QMouseEvent *event);
    void typingMouseDoubleClick(QMouseEvent *event);

    struct CellTarget
    {
        int line = -1; // First line of the table's block.
        int at = 0;    // The table's first line, in the block (an HTML
                       // table can be inside a <div>).
        int row = 0;
        int col = 0;
        QString original; // The cell's source when the box opened.
    };
    TableCellEditor *m_cellEditor = nullptr;
    CellTarget m_cell;
    // A press on a cell: a click (no drag) opens the box when the button
    // comes up; a drag selects text as anywhere. The caret is where it was
    // before the press moved it.
    struct PendingCell
    {
        bool active = false;
        int line = 0;
        int row = 0;
        int col = 0;
        QPoint pos;
        QPointF point; // In the block.
    } m_pendingCell;

    // Cells, rows and columns selected in a rendered table (editable view):
    // by dragging from one cell to another, or along the band just above the
    // table (columns) or left of it (rows). The selection is painted over the
    // rendered table and goes with the first edit of the document.
    enum class SelectionMode { Cells, Columns, Rows };
    struct TableSelection
    {
        bool active = false;
        SelectionMode mode = SelectionMode::Cells;
        int line = -1; // First line of the table's block.
        int at = 0;    // The table's first line, in the block.
        int rows = 0;  // Size of the table.
        int cols = 0;
        int anchorRow = 0;
        int anchorCol = 0;
        int row = 0; // The far corner.
        int col = 0;
        int revision = -1; // Of the document.
        int top() const { return mode == SelectionMode::Columns ? 0 : qMin(anchorRow, row); }
        int bottom() const { return mode == SelectionMode::Columns ? rows - 1 : qMax(anchorRow, row); }
        int left() const { return mode == SelectionMode::Rows ? 0 : qMin(anchorCol, col); }
        int right() const { return mode == SelectionMode::Rows ? cols - 1 : qMax(anchorCol, col); }
        bool operator==(const TableSelection &other) const
        {
            return active == other.active && mode == other.mode && line == other.line && anchorRow == other.anchorRow
                   && anchorCol == other.anchorCol && row == other.row && col == other.col;
        }
    };
    // What a rendered table is, as far as the source says (see checkTable).
    struct TableCheck
    {
        int line = -1;
        int revision = -1;
        const RenderedBlock *block = nullptr;
        bool ok = false; // One table, with the cells the source has.
        bool canInsert = false;
        int at = 0;
        md::TableInfo info;
    };
    // Where the pointer is on a table: on a cell, on the band above a column
    // or left of a row, or at a border between two columns or rows.
    enum class TableZone { None, Cell, Column, Row, InsertColumn, InsertRow };
    struct TableSpot
    {
        TableZone zone = TableZone::None;
        int line = -1;
        int at = 0;
        int rows = 0;
        int cols = 0;
        int index = -1; // The column or row, or the border (0 is before the first).
        int row = -1;   // For a cell.
        int col = -1;
        qreal top = 0;  // Of the table's block, in the viewport.
        const RenderedBlock::TableGrid *grid = nullptr;
    };
    // The quick-insert mark shown: a line along a border and a plus on it.
    struct TableHover
    {
        TableZone zone = TableZone::None;
        int line = -1;
        int index = -1;
        QPointF center;   // Of the plus, in the viewport.
        QPointF from;     // The ends of the line.
        QPointF to;
        QRect area;       // To repaint.
    };
    struct TableDrag
    {
        bool active = false;
    };
    const TableCheck &checkTable(const Row &row, const Render *render);
    TableSpot tableSpotAt(const QPointF &point);
    void setTableHover(const TableSpot &spot);
    void clearTableHover();
    void tablePressed(const TableSpot &spot, QMouseEvent *event);
    void insertAtBorder(const TableSpot &spot);
    // Starts (or with `extend`, grows) a selection on the table of `spot`.
    void selectTableCells(SelectionMode mode, const TableSpot &spot, int row, int col, bool extend);
    void updateTableDrag(const QPointF &point);
    void setTableSelection(const TableSelection &selection);
    void clearTableSelection();
    // False (and forgets the selection) if the document changed since.
    bool tableSelectionValid();
    // Where the selection is, in the viewport; false if not in view.
    bool selectionViewRect(QRectF *rect);
    void paintTableOverlay(QPaintEvent *event);
    // Runs a table command on the selected rows, columns or cells.
    bool selectionOperation(md::TableOp op);
    // The table info of the selection's first cell, with what each command
    // does on the selection (not on the cell alone) in `available`.
    md::TableInfo selectionInfo();
    bool dragStartedOnCell(QMouseEvent *event);
    TableSelection m_tableSel;
    TableHover m_tableHover;
    TableDrag m_tableDrag;
    TableCheck m_tableCheck;
    QHash<int, QCursor> m_edgeCursors; // The arrows at a table's edges, by pixel ratio.

    TableToolbar *m_tableToolbar = nullptr;
    FigureBar *m_figureBar = nullptr;
    // The figure the bar is over: the first line of its block's row, and its
    // number in the block.
    struct FigureHover
    {
        int line = -1;
        int index = -1;
    } m_figureHover;
    // How large the user made figures, by RenderedBlock::figureId(); not saved.
    QHash<QString, QList<qreal>> m_figureScales;
    int m_figureRevision = 0; // Bumped when m_figureScales changes.
    bool m_figuresRendered = false; // A block that may have figures was rendered.
    bool m_showsPictures = false;   // Counted in picturedViews (see the destructor).
    bool m_toolbarUpdatePending = false;
    bool m_hasFocus = false; // Not counting the toolbar's menu taking it.

    MarkdownModel *m_model;
    QString m_baseDirectory;
    bool m_highlightActiveBlock = true;
    std::vector<RowInfo> m_rows;
    std::vector<int> m_lineToRow;
    QSet<int> m_expandedBlocks; // Span indices.
    struct Fold
    {
        QTextCursor anchor; // At the first line of the block, or the heading.
        FoldKind kind = FoldKind::Other;
    };
    std::vector<Fold> m_folds;
    // The document was just opened: its blocks start as the preference says,
    // once its first parse is shown.
    bool m_startFoldsDue = true;
    bool m_revealPending = false; // revealCaret() waits for the rows.
    std::vector<int> m_rowsFoldCarets; // What the rows' folds were opened for.
    int m_hoverLine = -1; // First line of the row the pointer is over.
    bool m_hoverFold = false; // ... and it is on something that folds.
    void updateHover(const QPoint &point);
    std::unordered_map<int, folding::Fold> m_classes;
    std::unordered_map<int, Section> m_sections;
    int m_classesGeneration = -1;
    // Blocks holding the carets show as source (while focused): set by a
    // double click or a fallback edit, cleared by Esc, a click on blank space
    // or the caret moving out of them (see rebuildRows).
    bool m_sourceOpen = false;
    std::vector<std::pair<int, int>> m_openLines; // The open blocks' lines, last built.
    // The lines of the last edit made through this view (see liveLines).
    QTextCursor m_liveStart;
    QTextCursor m_liveEnd;
    struct LiveParse
    {
        QString source;
        rust::Vec<core::Block> blocks;
    };
    std::deque<LiveParse> m_liveParses;
    // The lines edited since the last parse (see noteEdit): lines
    // [first, last] of the parse are now `lines` lines; the others moved by
    // the lines added or taken before them. Sorted, apart. Unknown: edits
    // the view did not see (it was made after them), or too many ranges.
    struct EditRange
    {
        int first = 0;
        int last = 0;
        int lines = 0;
        int delta() const { return lines - (last - first + 1); }
    };
    std::vector<EditRange> m_editRanges;
    bool m_editsKnown = false;
    int m_editLines = 0; // The document's lines after the last edit noted.
    int m_editsGeneration = -1; // The parse the edits are counted from.
    // Rows of long lists' items (see itemRows), as the rows were last built.
    std::vector<ItemList> m_itemLists;
    std::vector<ItemRow> m_itemRows;
    QSet<int> m_itemSpans; // Spans shown an item a row.
    // The items of parsed lists (by block index), for m_listItemsGeneration's parse.
    std::unordered_map<int, std::shared_ptr<const ListItems>> m_listItems;
    int m_listItemsGeneration = -1;
    QSet<int> m_irregularLists; // Parsed blocks shown whole (see irregularItem), this parse.
    // The renders of items of parsed lists not edited since (by block index
    // and item), which edits elsewhere leave as they are; for the render
    // context of m_itemMemoKey.
    std::unordered_map<quint64, Render> m_itemMemo;
    QString m_itemMemoKey;
    // renderContext()'s, for the frame, revision and width it was made at.
    RenderContext m_context;
    quint64 m_contextFrame = 0;
    int m_contextRevision = -1;
    qreal m_contextWidth = -1;
    bool m_contextOverflow = false;
    qreal m_contextFigureHeight = -1;
    int m_contextFigureRevision = -1;
    bool m_contextValid = false;
    QString m_contextKey; // renderKey() of it.
    // Up and Down in rendered text keep the x they started from.
    qreal m_verticalX = -1;
    int m_verticalPosition = -1;
    // Carets of the paint in progress (see paintRenderedRow).
    std::vector<int> m_paintCarets;
    QSet<int> m_partSpans;      // Spans shown in parts (see RenderedBlock::createPart).
    bool m_rowsStale = true;
    // The rows are the source, line by line, while the model parses the
    // document for the first time (see rebuildRows).
    bool m_standIn = false;
    // Rows were built from a parse since the view was last shown.
    bool m_showsParse = false;
    // Not painted since shown; rough heights were handed out meanwhile.
    bool m_unpainted = true;
    bool m_estimated = false;
    // What m_rows were built from, so caret moves that open or close
    // nothing don't rebuild them.
    int m_rowsRevision = -1;
    std::vector<std::pair<int, int>> m_rowsExpanded; // Line ranges shown as source.
    std::vector<int> m_rowsCaretGaps; // Blank lines shown as source for a caret.
    std::vector<int> m_rowsFoldCaretLines; // The lines of m_rowsFoldCarets.
    // Item rows and lists that no row points to since patches (see patchRows).
    int m_itemGarbage = 0;
    bool m_rowsPatched = false;
    bool m_patchRows = true;

    // Renders by size, fonts and source; bounded by cost (KiB).
    QCache<QString, Render> m_cache;
    QSet<QString> m_pendingKeys; // Diagrams still rendering.
    // The render of rendered rows by first line, for the model revision,
    // width and pixel ratio below, so painting doesn't rebuild sources and
    // keys. Only the latest few (see rendered()) are kept: the rest are in
    // m_cache, within its budget, or gone.
    std::unordered_map<int, Render> m_renders;
    std::deque<int> m_renderOrder; // First lines in the order they were set.
    // The cache keys of the blocks rendered since the renders were reset, by
    // first line, with what else made the key (see rendered()).
    struct RenderKey
    {
        QString key;
        int lastLine = -1;
        int blockIndex = -1;
        bool chip = false;
        bool inParts = false;
    };
    std::unordered_map<int, RenderKey> m_renderKeys;
    qint64 m_renderKiB = 0;
    int m_rendersRevision = -1;
    qreal m_rendersWidth = -1;
    bool m_rendersOverflow = false;
    qreal m_rendersDpr = -1;
    qreal m_rendersFigureHeight = -1;
    int m_rendersFigureRevision = -1;
    // Bumped by every paint: what a paint shows is kept until the next one.
    quint64 m_frame = 0;
    int m_renderCount = 0;
    // Blocks made in parts, by block index of m_longBoxesGeneration's parse.
    std::unordered_map<int, LongBox> m_longBoxes;
    int m_longBoxesGeneration = -1;
    // Whether each block of m_mayBeListsGeneration's parse may show as a long
    // list (see mayBeLongList): -1 until asked.
    std::vector<signed char> m_mayBeLists;
    int m_mayBeListsGeneration = -1;

    // The bar above the text asking about remote pictures (see
    // updateRemoteBanner); the remote addresses are scanned once per parse.
    QFrame *m_banner = nullptr;
    QLabel *m_bannerText = nullptr;
    int m_bannerHeight = 0;
    QStringList m_remoteUrls;
    int m_remoteUrlsGeneration = -1;
    QHash<size_t, QStringList> m_remoteScans; // Addresses by hash of a block's HTML.

    // Read mode (VisualViewRead.cpp).
    class ReadRows; // m_rows as md::ReadFlow reads them.
    // Where the columns go, for a viewport size, font and the column settings.
    struct ReadLayout
    {
        QSize viewport;
        qreal fontSize = 0;
        int chars = 0;
        int columns = 0; // Asked for.
        int serial = -1; // m_readLayoutSerial.
        int count = 1;   // Shown.
        int fit = 1;     // That fit.
        int width = 0;   // Of a column.
        int height = 0;
        int gap = 0;
        int left = 0;    // Of the first column.
        int top = 0;
    };
    bool readActive() const { return m_readMode && !m_standIn; }
    const ReadLayout &readLayout() const;
    qreal readLineHeight();
    // The screen as the rows and layout are now (reflowed if they changed).
    const md::Screen &ensureReadScreen();
    // Shows a screen a move made: the place it keeps, the controls, the news.
    void readMoved();
    void noteReadScreen();
    void placeReadControls();
    void updateReadControls();
    void paintRead(QPaintEvent *event);
    // The segment under a viewport point (the nearest column, the nearest
    // segment in it unless `exact`), with its top in the viewport; the
    // repeated header above a part of a table counts as that part's.
    struct ReadHit
    {
        int column = -1;
        int index = -1;  // In the column.
        qreal top = 0;   // Of the segment, its header included.
        bool header = false; // The point is on the repeated header.
    };
    ReadHit readHitAt(const QPointF &point, bool exact);
    // Pictures that moved to the next column: their chip and notes.
    QFont readNoteFont();
    QString readPartText(const std::vector<md::Segment> &segments, int index);
    void paintReadPart(QPainter &painter, const std::vector<md::Segment> &segments, int index, bool lastColumn,
                       const QRectF &rect);
    // The row of the chip under a point (-1: none), and the viewer for it.
    int readChipAt(const QPointF &point);
    void openMovedPictures(int row);
    // Where the row's top-left corner is for a hit (rowAtPoint's origin).
    QPointF readOrigin(const ReadHit &hit);
    bool readKey(QKeyEvent *event);
    // A table or equation wider than its column slides sideways under its
    // strip; each keeps its own offset (by its first line) until the layout
    // changes or read mode ends. The offset of a row, clamped.
    qreal readOffset(int rowIndex);
    void setReadOffset(int rowIndex, qreal offset);
    // Scrolls the wide block under a viewport point by `dx` pixels; false if
    // there is none.
    bool readScrollAt(const QPointF &point, qreal dx);
    // The scrollbar of a segment, in the viewport (null if it shows no part
    // of it), and the one at a point.
    QRectF readBarOf(const ReadHit &hit);
    std::optional<ReadHit> readBarAt(const QPointF &point);
    bool readBarPressed(QMouseEvent *event);
    bool readBarMoved(QMouseEvent *event);
    // The viewport origin of the row showing `line` (its first line), if on screen.
    std::optional<QPointF> rowOrigin(int line);

    bool m_readMode = false;
    int m_readColumns = 0;
    int m_readChars = 70;
    std::unique_ptr<md::RowSource> m_readRows; // A ReadRows.
    std::unique_ptr<md::ReadFlow> m_readFlow;
    int m_readRevision = 0;     // Of m_rows, for md::Geometry.
    bool m_readDirty = true;    // Rows or heights may have changed since the last fill.
    bool m_readNeedsStart = true; // The flow starts at m_readLinePosition.
    qreal m_readLinePosition = 0; // Where the screen starts, as a line position.
    int m_readLayoutSerial = 0; // Bumped when the text font may have changed.
    mutable ReadLayout m_readLayout;
    int m_readWheel = 0;        // Wheel movement not yet worth a line (eighths of a degree or pixels).
    bool m_readWheelPixels = false;
    // A mouse event is being handled: the caret it places is on screen.
    bool m_readPointing = false;
    std::unordered_map<int, qreal> m_readOffsets; // By first line of the block.
    // How far the block being painted is slid (see paintRenderedRow).
    qreal m_readSlide = 0;
    // A drag of a scrollbar's thumb: the block's row, and where in the thumb it holds.
    struct BarDrag
    {
        bool active = false;
        int line = -1;
        qreal grab = 0;
    } m_readBarDrag;
    // What readChanged() last told: the screen's start and end, the lines, and the fit.
    struct ReadNews
    {
        md::Position start;
        md::Position end;
        int first = -1;
        int last = -1;
        int fit = 0;
        bool operator==(const ReadNews &) const = default;
    } m_readNews;
    bool m_readNewsPending = false;
    QAbstractButton *m_readBack = nullptr;
    QAbstractButton *m_readForward = nullptr;
    QAbstractButton *m_readUp = nullptr;
    QAbstractButton *m_readDown = nullptr;
};

} // namespace vn
