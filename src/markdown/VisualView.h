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

class VisualView : public TextView
{
    Q_OBJECT

public:
    VisualView(MarkdownModel *model, QWidget *parent = nullptr);

    void setBaseDirectory(const QString &directory);
    void setHighlightActiveBlock(bool on);

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
    void changeEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
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

    MarkdownModel *m_model;
    QString m_baseDirectory;
    bool m_highlightActiveBlock = true;
    std::vector<RowInfo> m_rows;
    std::vector<int> m_lineToRow;
    QSet<int> m_expandedBlocks;
    QHash<QString, std::shared_ptr<RenderedBlock>> m_cache;
};

} // namespace vn
