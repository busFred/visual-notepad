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
    int memoizedRenderCount() const { return m_spanRenderCount; }

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
    void focusOutEvent(QFocusEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

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
        int cost = 0; // KiB, of the block.
    };

    void rebuildRows();
    RenderContext context() const;
    const RenderedBlock *rendered(int span);
    const RenderedBlock *renderedForRow(const Row &row);
    int spanForRow(const Row &row) const;
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
    void resetSpanRenders(size_t spans);
    void dropSpanRender(int span);

    MarkdownModel *m_model;
    QString m_baseDirectory;
    bool m_highlightActiveBlock = true;
    std::vector<RowInfo> m_rows;
    std::vector<int> m_lineToRow;
    QSet<int> m_expandedBlocks; // Span indices.
    bool m_rowsStale = true;
    // What m_rows were built from, so caret moves that open or close
    // nothing don't rebuild them.
    int m_rowsRevision = -1;
    std::vector<std::pair<int, int>> m_rowsExpanded; // Line ranges shown as source.

    // Renders by size, fonts and source; bounded by cost (KiB).
    QCache<QString, Render> m_cache;
    QSet<QString> m_pendingKeys; // Diagrams still rendering.
    // The render of each span, for the model revision, width and pixel
    // ratio below, so painting doesn't rebuild sources and keys. Only the
    // latest few (see rendered()) are kept: the rest are in m_cache, within
    // its budget, or gone.
    std::vector<Render> m_spanRenders;
    std::deque<int> m_spanRenderOrder; // Spans in the order they were set.
    int m_spanRenderCount = 0;
    qint64 m_spanRenderKiB = 0;
    int m_spanRendersRevision = -1;
    qreal m_spanRendersWidth = -1;
    qreal m_spanRendersDpr = -1;

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
