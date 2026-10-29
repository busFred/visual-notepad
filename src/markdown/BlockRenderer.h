// Turns one Markdown block into something paintable at a given width:
// rich text, a highlighted code box, a math image or a Mermaid diagram.
#pragma once

#include "vn_core_bridge/lib.h"

#include <QFont>
#include <QImage>
#include <QList>
#include <QPair>
#include <QPointF>
#include <QString>
#include <QTextDocument>

#include <memory>

class QPainter;

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
};

// What to highlight in a block: ranges of rendered-text characters (for code,
// source characters), or for images (math, diagrams) the whole picture.
struct RenderedSelection
{
    QList<QPair<int, int>> ranges;
    bool whole = false;
    bool isEmpty() const { return ranges.isEmpty() && !whole; }
};

class RenderedBlock
{
public:
    enum class Kind { Rich, Code, Image, Empty };

    static std::unique_ptr<RenderedBlock> create(const core::Block &block, const QString &source,
                                                 const RenderContext &context);

    Kind kind() const { return m_kind; }
    qreal height() const { return m_height; }
    void paint(QPainter &painter, const QPointF &topLeft, qreal width, const RenderedSelection &selection = {}) const;

    // Character position in the rendered text at a point relative to the
    // block's top-left, or -1. For code the position is in the source text.
    int hitTest(const QPointF &point) const;
    QString anchorAt(const QPointF &point) const;
    QString renderedText() const;

private:
    QPointF contentOffset() const;

    Kind m_kind = Kind::Empty;
    std::unique_ptr<QTextDocument> m_doc;
    QImage m_image;
    QString m_error;
    qreal m_height = 0;
    bool m_centered = false;
};

} // namespace vn
