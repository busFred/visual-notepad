// Turns one Markdown block into something paintable at a given width:
// rich text, a highlighted code box, a math image or a Mermaid diagram.
#pragma once

#include "vn_core_bridge/lib.h"

#include <QFont>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTextDocument>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

class QPainter;
class QTextList;
class QTextTable;

namespace vn {

struct RenderContext
{
    QString baseDirectory; // For relative image paths.
    QFont font;            // Body text.
    QFont codeFont;
    qreal lineHeight = 1.6; // Multiple of the font height, for text.
    qreal width = 600;
    qreal devicePixelRatio = 1;
    QString mermaidTheme;
    // The user agreed to download the pictures this block names with http(s)
    // addresses; otherwise they show as missing and nothing is requested.
    bool allowRemoteImages = false;
};

// What to highlight in a block: ranges of rendered-text characters (for code,
// source characters), or for images (math, diagrams) the whole picture.
struct RenderedSelection
{
    QList<QPair<int, int>> ranges;
    bool whole = false;
    // A list item rendered on its own (RenderedBlock::createItem): the
    // selection runs on into the next item (Qt marks the end of the item's
    // last line then), and comes from an item before (its marker shows
    // selected).
    bool runsOn = false;
    bool markerSelected = false;
    bool isEmpty() const { return ranges.isEmpty() && !whole && !runsOn && !markerSelected; }
};

// Mermaid diagrams render on a worker thread (they can take a while);
// ready is emitted on the GUI thread when one has finished, after which
// RenderedBlock::create returns it. Blocks created before show the source.
class DiagramNotifier : public QObject
{
    Q_OBJECT

public:
    static DiagramNotifier &instance();

    // For tests: diagrams rendered so far, and the size of the cache holding them.
    static int renderCount();
    static void setCacheLimitKiB(int kib);

Q_SIGNALS:
    void ready();
};

class RenderedBlock
{
public:
    enum class Kind { Rich, Code, Image, Empty };

    static std::unique_ptr<RenderedBlock> create(const core::Block &block, const QString &source,
                                                 const RenderContext &context);

    // A code box too long to highlight (code, front matter, a block too heavy
    // for rich text: see create) can be made in parts of partLines source
    // lines instead, painted one under the other, so that a view lays out
    // and keeps only the parts it shows. Together they look like the box.
    static constexpr int partLines = 200;
    // Whether create() makes such a box of the block with this much source.
    static bool isLongCodeBox(const core::Block &block, qsizetype sourceChars);
    // For tests: off, isLongCodeBox() says no (views make every box whole).
    static void setPartsEnabled(bool on);
    // Whether the box wraps its lines anywhere; decided on its whole source.
    static bool wrapsAnywhere(const QString &source);
    struct Part
    {
        bool first = true; // Has the top of the box.
        bool last = true;  // Has the bottom.
        bool wrapAnywhere = false;
    };
    // A part, from the lines of the source it covers.
    static std::unique_ptr<RenderedBlock> createPart(const QString &source, const Part &part,
                                                     const RenderContext &context);
    // One top-level item of a list, alone: `block` is its HTML in its list's
    // tags (<ul><li>...</li></ul> or <ol>...), and it renders as it does in
    // the whole list, except that its marker (bullet or number) is left to
    // paintItemMarker(), so that items renumber without rendering again.
    // Its height is the item's own: itemAdvance() adds the space down to the
    // next item. A view shows a long list so, an item a row (VisualView).
    static std::unique_ptr<RenderedBlock> createItem(const core::Block &block, const QString &source,
                                                     const RenderContext &context);
    // How far below this item's top the next item starts in the whole list
    // (its margins and the next item's, as Qt's layout spaces them), or for
    // the last item (next null) where the list ends.
    qreal itemAdvance(const RenderedBlock *next) const;
    // Whether the item renders as it does in the whole list: one marker, on
    // its first line (Qt drops an empty item, starts an item that opens
    // with a list without a marker and numbers each line of a code block
    // that opens one). Views render a list with others whole.
    bool isRegularItem() const { return m_item.regular; }
    // Draws the item's marker as Qt draws it in the whole list: a bullet, or
    // `number` of an ordered list; `selected` as when a selection from an
    // item before runs over it.
    void paintItemMarker(QPainter &painter, const QPointF &topLeft, bool ordered, int number, bool selected) const;
    // The distinct http(s) addresses of the pictures an HTML block asks Qt for
    // when it is laid out and painted (entities and styles decoded by Qt
    // itself), whether or not downloads are allowed. Nothing is requested.
    static QStringList remoteImages(const core::Block &block, const RenderContext &context);
    // False if remoteImages() is sure to be empty without laying the block
    // out (it has no picture, background or style at all). Quick.
    static bool mayAskForPictures(const core::Block &block);

    Kind kind() const { return m_kind; }
    qreal height() const { return m_height; }
    void paint(QPainter &painter, const QPointF &topLeft, qreal width, const RenderedSelection &selection = {}) const;

    // Character position in the rendered text at a point relative to the
    // block's top-left, or -1. For code the position is in the source text.
    int hitTest(const QPointF &point) const;
    QString anchorAt(const QPointF &point) const;
    QString renderedText() const;
    // Where a caret before character `position` of the rendered text (code:
    // of the source) is drawn, relative to the block's top-left; none for
    // pictures and empty blocks.
    std::optional<QRectF> caretRect(int position) const;
    // The [start, end) of the laid-out line holding `position`.
    std::pair<int, int> lineAt(int position) const;
    // Something still arriving: a Mermaid diagram rendering (shown as its
    // source for now) or remote pictures downloading (shown as missing).
    bool isPending() const { return m_pending; }

    // A cell of the one pipe table a block of rich text is (see
    // tableCells): its row (0 is the header) and column, and its rectangle
    // relative to the block's top-left.
    struct TableCellHit
    {
        int row = -1;
        int col = -1;
        QRectF rect;
    };
    // The table's size, or (0, 0) if the block is not exactly one table (text
    // around it, two tables, a table in a table).
    QSize tableCells() const;
    // The cell at a point relative to the block's top-left, if it is in the table.
    std::optional<TableCellHit> tableCellAt(const QPointF &point) const;
    // The rectangle of a cell, relative to the block's top-left.
    std::optional<QRectF> tableCellRect(int row, int col) const;
    // Where the cells' edges are, relative to the block's top-left: the left
    // edge of each column and the right edge of the last (cols + 1 values),
    // the top of each row and the bottom of the last (rows + 1 values). Made
    // once, so pointing at a table is cheap; null if the block is not one
    // table.
    struct TableGrid
    {
        std::vector<qreal> xs;
        std::vector<qreal> ys;
        int cols() const { return int(xs.size()) - 1; }
        int rows() const { return int(ys.size()) - 1; }
        // The column or row holding a coordinate, clamped to the table's.
        int colAt(qreal x) const;
        int rowAt(qreal y) const;
    };
    const TableGrid *tableGrid() const;

    // A picture the viewer can enlarge: a local file (path), a downloaded
    // one (url), or a generated one (math, diagram; both empty).
    struct Picture
    {
        QString title;
        QString path;
        QString url;
    };
    // The picture at a point relative to the block's top-left; cheap.
    std::optional<Picture> pictureAt(const QPointF &point) const;
    // It at full size; null if it cannot be read.
    QImage pictureImage(const Picture &picture) const;

    // For tests: picture files read (and decoded) from disk so far.
    static int pictureFileReads();
    // How many KiB of decoded picture files are kept for later widths (and
    // forgets the ones kept). Set from key::pictureCacheMiB by MainWindow.
    static void setDecodedPictureLimitKiB(int kib);
    // For tests: blocks shown as their source because rich text would take
    // too long to lay them out, and a switch for those limits (and the
    // code box's: no highlighting of huge text, wrapping anywhere).
    static int guardedBlockCount();
    static void setLayoutGuardEnabled(bool on);

    // Memory use in KiB (about, but not under), for caches.
    int cost() const;

    // Where source lines start in the rendering: (line offset from the
    // block's first line, y from the block's top), ascending by line. Code
    // has every line; rich text has the table rows, list items, paragraphs
    // and headings the renderer marked with "vn-line:N" anchors. Line 0 (the
    // top) is left out; the rest is interpolated.
    using LineAnchor = std::pair<int, qreal>;
    const std::vector<LineAnchor> &lineAnchors() const { return m_lineAnchors; }

private:
    static std::unique_ptr<RenderedBlock> create(const core::Block &block, const QString &source,
                                                 const RenderContext &context, bool item);
    static std::unique_ptr<RenderedBlock> codeBox(std::unique_ptr<RenderedBlock> result, const QString &source,
                                                  const RenderContext &context);
    QPointF contentOffset() const;
    QTextTable *theTable() const;
    void collectLineAnchors(int sourceLines);

    Kind m_kind = Kind::Empty;
    std::unique_ptr<QTextDocument> m_doc;
    QImage m_image;
    QString m_error;
    qreal m_height = 0;
    qreal m_width = 0; // Of the column it was made for.
    qsizetype m_pictureBytes = 0; // Of the pictures in the rich text.
    QString m_diagramCode; // Mermaid source; null for other blocks.
    QString m_diagramTheme;
    bool m_centered = false;
    bool m_pending = false;
    Part m_part; // Of a code box; the whole box by default.
    // Of an item made by createItem: its list (in m_doc), whose marker style
    // was taken off for paintItemMarker, and its outer margins.
    struct Item
    {
        bool made = false;
        bool regular = false;
        QTextList *list = nullptr;
        int style = 0; // QTextListFormat::Style
        qreal topMargin = 0;
        qreal bottomMargin = 0;
    } m_item;
    std::vector<LineAnchor> m_lineAnchors;
    mutable QString m_renderedText; // Cached; null until asked for.
    mutable std::optional<std::optional<TableGrid>> m_tableGrid; // Empty until asked for.
};

// The picture in downloaded bytes, or null: also for an SVG that names a
// file on another machine, which the SVG plugin would open by itself, or that
// would take too long to draw (the rules for local SVG files). Any thread.
QImage decodePicture(const QByteArray &data);

} // namespace vn
