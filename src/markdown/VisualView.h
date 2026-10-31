// The hybrid Markdown editor: every top-level block is shown rendered, except
// blocks holding a caret, which show their source for editing (the
// behaviour of VS Code's Markdown editor). Locked (read-only) shows
// everything rendered.
#pragma once

#include "../editor/TextView.h"
#include "BlockRenderer.h"

#include <QCache>
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

namespace vn {

class MarkdownModel;

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
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
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
