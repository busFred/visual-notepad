// The hybrid Markdown editor: every top-level block is shown rendered, except
// blocks holding a caret, which show their source for editing (the
// behaviour of VS Code's Markdown editor). Locked (read-only) shows
// everything rendered.
#pragma once

#include "../editor/TextView.h"
#include "BlockRenderer.h"
#include "TableCellEditor.h"

#include <QCache>
#include <QCursor>
#include <QHash>
#include <QSet>
#include <QStringList>

#include <deque>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

class QFrame;
class QLabel;
class QMenu;

namespace vn {

class MarkdownModel;
class TableCellEditor;
class TableToolbar;

// Map between a Rich block's rendered text and its Markdown source by
// matching characters in order (Markdown syntax that is not rendered is
// skipped). sourceOffsetFor: the source offset just after the rendered prefix;
// renderedOffsetFor: the inverse, the number of rendered characters that
// precede a source offset. For any prefix length k,
// renderedOffsetFor(rendered, source, sourceOffsetFor(rendered.left(k), source)) == k.
int sourceOffsetFor(const QString &renderedPrefix, const QString &source);
int renderedOffsetFor(const QString &rendered, const QString &source, int sourceOffset);

class VisualView : public TextView
{
    Q_OBJECT

public:
    VisualView(MarkdownModel *model, QWidget *parent = nullptr);

    void setBaseDirectory(const QString &directory);
    void setHighlightActiveBlock(bool on);
    // Blocks currently shown as source because a caret or selection touches them.
    int expandedBlockCount() const { return int(m_expandedBlocks.size()); }
    // For tests: renders kept in hand besides the cache.
    int memoizedRenderCount() const { return int(m_renders.size()); }
    // For tests: blocks (and parts of blocks) rendered so far.
    int renderCount() const { return m_renderCount; }
    // A new table stays rendered, with its first header's box open (its
    // text selected, to type over), as a word processor does.
    void insertTable(int rows, int cols) override;
    // For tests: where a cell of the rendered table starting at `line` is, in
    // viewport coordinates; null if it is not shown as a table.
    QRect tableCellViewRect(int line, int row, int col);

Q_SIGNALS:
    // A link was clicked in rendered text (not a task checkbox).
    void linkActivated(const QString &url);

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
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
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

private:
    struct RowInfo
    {
        Row row;
        int span = -1; // Index into the model's spans, -1 for gaps.
    };
    // A render and the source it was made from.
    struct Render
    {
        QString source;
        QString key;
        std::shared_ptr<RenderedBlock> block;
        int cost = 0;     // KiB, of the block.
        quint64 used = 0; // The last frame (see m_frame) that asked for it.
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
    RenderContext context() const;
    // The render of a rendered row (a block, or a part of one), or null.
    const Render *rendered(const Row &row);
    const RenderedBlock *renderedForRow(const Row &row);
    // The render kept for a row at the current width, or null; makes none.
    const Render *memoized(const Row &row) const;
    int spanForRow(const Row &row) const;
    LongBox &longBox(int blockIndex);
    // Whether a span shows as a code box in parts.
    bool madeInParts(int span);
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
    // The toolbar over the table being edited: shown while the caret is in a
    // table of an editable view with the focus; the update is made once the
    // event being handled is done.
    void scheduleToolbarUpdate();
    void updateTableToolbar();
    void toolbarOperation(md::TableOp op);

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
    bool m_toolbarUpdatePending = false;
    bool m_hasFocus = false; // Not counting the toolbar's menu taking it.

    MarkdownModel *m_model;
    QString m_baseDirectory;
    bool m_highlightActiveBlock = true;
    std::vector<RowInfo> m_rows;
    std::vector<int> m_lineToRow;
    QSet<int> m_expandedBlocks; // Span indices.
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

    // Renders by size, fonts and source; bounded by cost (KiB).
    QCache<QString, Render> m_cache;
    QSet<QString> m_pendingKeys; // Diagrams still rendering.
    // The render of rendered rows by first line, for the model revision,
    // width and pixel ratio below, so painting doesn't rebuild sources and
    // keys. Only the latest few (see rendered()) are kept: the rest are in
    // m_cache, within its budget, or gone.
    std::unordered_map<int, Render> m_renders;
    std::deque<int> m_renderOrder; // First lines in the order they were set.
    qint64 m_renderKiB = 0;
    int m_rendersRevision = -1;
    qreal m_rendersWidth = -1;
    qreal m_rendersDpr = -1;
    // Bumped by every paint: what a paint shows is kept until the next one.
    quint64 m_frame = 0;
    int m_renderCount = 0;
    // Blocks made in parts, by block index of m_longBoxesGeneration's parse.
    std::unordered_map<int, LongBox> m_longBoxes;
    int m_longBoxesGeneration = -1;

    // The bar above the text asking about remote pictures (see
    // updateRemoteBanner); the remote addresses are scanned once per parse.
    QFrame *m_banner = nullptr;
    QLabel *m_bannerText = nullptr;
    int m_bannerHeight = 0;
    QStringList m_remoteUrls;
    int m_remoteUrlsGeneration = -1;
    QHash<size_t, QStringList> m_remoteScans; // Addresses by hash of a block's HTML.
};

} // namespace vn
