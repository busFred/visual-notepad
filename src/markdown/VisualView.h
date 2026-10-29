// The hybrid Markdown editor: every top-level block is shown rendered, except
// blocks holding a caret, which show their source for editing (the
// behaviour of VS Code's Markdown editor). Locked (read-only) shows
// everything rendered.
#pragma once

#include "../editor/TextView.h"
#include "BlockRenderer.h"

#include <QHash>

#include <memory>

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
    void paintLineBackground(QPainter &painter, int line, const QRectF &rect) override;
    void rowsMayHaveChanged() override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    QTextCursor lineUnitAt(int position) const override;
    void changeEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    struct RowInfo
    {
        Row row;
        int block = -1; // Block index for rendered blocks, -1 for gaps.
    };

    void rebuildRows();
    RenderContext context() const;
    const RenderedBlock *rendered(int blockIndex);
    const RenderedBlock *renderedForRow(const Row &row);
    int blockForRow(const Row &row) const;
    void toggleTask(int line);
    RenderedSelection selectionForRow(const Row &row, const RenderedBlock *block) const;

    MarkdownModel *m_model;
    QString m_baseDirectory;
    bool m_highlightActiveBlock = true;
    std::vector<RowInfo> m_rows;
    std::vector<int> m_lineToRow;
    QSet<int> m_expandedBlocks;
    bool m_rowsStale = true;
    QHash<QString, std::shared_ptr<RenderedBlock>> m_cache;
};

} // namespace vn
