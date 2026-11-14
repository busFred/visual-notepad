// A window of its own to look at a wide table or equation of read mode in
// full: the block itself, laid out at its natural width and painted as the
// view paints it (the same fonts and colours; formulas as shapes), not a
// picture of it. It scrolls when it is larger than the window, zooms by
// painting at the new size (so it is sharp at any zoom), and its text can be
// selected and copied. It does not block the window it came from: it can be
// moved aside, and several can be open.
#pragma once

#include "BlockRenderer.h"

#include <QDialog>
#include <QPointF>
#include <QSizeF>

#include <memory>

namespace vn {

class ZoomBar;

class BlockViewer : public QDialog
{
    Q_OBJECT

public:
    // Shows `block`, which is made at its natural width (RenderContext::width
    // the block's contentWidth with overflow on); it is kept by the window.
    BlockViewer(std::unique_ptr<RenderedBlock> block, const QString &title, QWidget *parent = nullptr);
    ~BlockViewer() override;

    // Display scale: logical pixels per pixel of the block's natural layout.
    qreal scale() const { return m_scale; }
    qreal fitScale() const;
    // What the scrollbars cover: the block at the current scale.
    QSizeF scaledSize() const;
    // The window's area for the block, without the scrollbars.
    QWidget *viewport() const;
    // Where a point of the block (relative to its top-left, as the
    // RenderedBlock's own points are) is in the viewport.
    QPointF toViewport(const QPointF &blockPoint) const;
    void zoomAt(const QPointF &anchor, qreal factor); // `anchor` in viewport coordinates.
    void fit();
    void actualSize(const QPointF &anchor);

    // The selected text (empty: none), and selecting all of the block's.
    QString selectedText() const;
    void selectAll();
    void copy() const; // The selection, to the clipboard.
    const RenderedBlock &block() const { return *m_block; }

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    class Canvas;

    void setScale(qreal scale, const QPointF &anchor);
    QPointF viewCentre() const;
    qreal lowestScale() const;
    void refresh(); // Repaint, and bring the controls up to date.

    std::unique_ptr<RenderedBlock> m_block;
    Canvas *m_canvas = nullptr;
    ZoomBar *m_bar = nullptr;
    qreal m_scale = 1;
    bool m_fitted = false;
};

} // namespace vn
