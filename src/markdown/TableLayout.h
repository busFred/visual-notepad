// The layout of a rendered block that is one table of plain cells, made as
// Qt's own document layout makes it, but kept from one width to the next.
#pragma once

#include <QAbstractTextDocumentLayout>

#include <memory>
#include <vector>

class QTextTable;

namespace vn {

// Qt's layout (QTextDocumentLayout) lays every cell of a table out twice at
// every width, once unbounded to learn the cell's least and widest widths
// and once in its column, and each time shapes the cell's text again. This
// layout places the paragraphs, lines and cells exactly where Qt's would
// (its steps, in Qt's fixed point, are Qt's: layoutFrame, layoutTable,
// layoutCell, layoutFlow and layoutBlock of qtextdocumentlayout.cpp, Qt
// 6.12), and draws and hit-tests them as it does, but it keeps what does not
// depend on the width: each cell's least and widest widths, its shaped text
// and its lines. A new width breaks the lines again only in the cells whose
// column changed width, without shaping the text again (see breakLines in
// the source). Queries go through the usual QAbstractTextDocumentLayout
// calls, so the rest of the renderer does not tell the two apart.
//
// It takes documents of one table (RenderedBlock::tableCells) of single
// cells aligned to the top, with no borders or backgrounds of its own (see
// RenderedBlock::OwnGrid, which draws them), no objects in the text
// (pictures, formulas), no lists and no side margins in the cells (see
// suits). Their text is not edited once laid out: a change of the
// document's length starts it over, as do changes of the default font, text
// option (but its wrap mode), indent width, root frame or table format (but
// its column widths); the widths and those column widths only lay it out
// again.
class TableLayout : public QAbstractTextDocumentLayout
{
    Q_OBJECT

public:
    // Whether this layout can take `doc`, which is `table` and empty
    // paragraphs around it.
    static bool suits(const QTextDocument &doc, const QTextTable &table);
    // For tests: off, suits() says no (Qt lays tables out).
    static void setEnabled(bool on);

    TableLayout(QTextDocument *doc, QTextTable *table);
    ~TableLayout() override;

    void draw(QPainter *painter, const PaintContext &context) override;
    int hitTest(const QPointF &point, Qt::HitTestAccuracy accuracy) const override;
    int pageCount() const override;
    QSizeF documentSize() const override;
    QRectF frameBoundingRect(QTextFrame *frame) const override;
    QRectF blockBoundingRect(const QTextBlock &block) const override;

    // What the table would be at a text width (QTextDocument::textWidth),
    // without laying it all out: QTextDocument::idealWidth() as Qt's layout
    // gives it, and the left edge and width of the first paragraph of each
    // cell of the first row (its blockBoundingRect: as wide as the column,
    // less the padding, or as its widest line).
    struct Columns
    {
        qreal idealWidth = 0;
        std::vector<qreal> textLeft;
        std::vector<qreal> textWidth;
    };
    Columns columnsAt(qreal textWidth) const;
    // The least width of a cell's text, in Qt's fixed point (1/64 px): the
    // widest of its paragraphs' QTextLayout::minimumWidth, as they have it
    // after a layout in the document's wrap mode.
    int leastWidth(int row, int column) const;

    // For tests: cells whose lines were broken again so far.
    static int cellsBroken();

protected:
    void documentChanged(int from, int charsRemoved, int charsAdded) override;

private:
    struct Data;
    std::unique_ptr<Data> d;
};

} // namespace vn
