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
#include <QString>
#include <QStringList>
#include <QTextDocument>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

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
    bool isEmpty() const { return ranges.isEmpty() && !whole; }
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
    // The distinct http(s) addresses of the pictures an HTML block asks Qt for
    // when it is laid out and painted (entities and styles decoded by Qt
    // itself), whether or not downloads are allowed. Nothing is requested.
    static QStringList remoteImages(const core::Block &block, const RenderContext &context);

    Kind kind() const { return m_kind; }
    qreal height() const { return m_height; }
    void paint(QPainter &painter, const QPointF &topLeft, qreal width, const RenderedSelection &selection = {}) const;

    // Character position in the rendered text at a point relative to the
    // block's top-left, or -1. For code the position is in the source text.
    int hitTest(const QPointF &point) const;
    QString anchorAt(const QPointF &point) const;
    QString renderedText() const;
    // Something still arriving: a Mermaid diagram rendering (shown as its
    // source for now) or remote pictures downloading (shown as missing).
    bool isPending() const { return m_pending; }

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
    static std::unique_ptr<RenderedBlock> codeBox(std::unique_ptr<RenderedBlock> result, const QString &source,
                                                  const RenderContext &context);
    QPointF contentOffset() const;
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
    std::vector<LineAnchor> m_lineAnchors;
    mutable QString m_renderedText; // Cached; null until asked for.
};

// The picture in downloaded bytes, or null: also for an SVG that names a
// file on another machine, which the SVG plugin would open by itself (the
// rule for local SVG files). Any thread.
QImage decodePicture(const QByteArray &data);

} // namespace vn
