#include "VisualView.h"

#include "../Settings.h"
#include "../Theme.h"
#include "MarkdownModel.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolTip>

namespace vn {

namespace {

constexpr qreal sideMargin = 26;
constexpr qreal gapRowFactor = 0.6;
constexpr qreal activeBarInset = 10;

bool sameChar(QChar a, QChar b)
{
    return a == b || (a.isSpace() && b.isSpace());
}

// Maps a prefix of the rendered text onto the block's source by walking the
// source and matching rendered characters in order; Markdown syntax that is
// not rendered (#, *, [ ], URLs) is skipped over.
int sourceOffsetFor(const QString &renderedPrefix, const QString &source)
{
    int r = 0;
    for (int s = 0; s < source.size(); ++s) {
        if (r >= renderedPrefix.size())
            return s;
        if (sameChar(renderedPrefix.at(r), source.at(s)))
            ++r;
    }
    return int(source.size());
}

} // namespace

VisualView::VisualView(MarkdownModel *model, QWidget *parent)
    : TextView(model->document(), parent)
    , m_model(model)
{
    setLineNumbersVisible(false);
    // The edited block has its own shading instead.
    setHighlightCurrentLine(false);
    setTextMargins(sideMargin, sideMargin);
    viewport()->setMouseTracking(true);
    connect(model, &MarkdownModel::optionsChanged, this, [this] {
        m_cache.clear();
        relayout();
    });
    rebuildRows();
}

void VisualView::setBaseDirectory(const QString &directory)
{
    if (m_baseDirectory == directory)
        return;
    m_baseDirectory = directory;
    m_cache.clear();
    relayout();
}

void VisualView::setHighlightActiveBlock(bool on)
{
    m_highlightActiveBlock = on;
    viewport()->update();
}

// ---------------------------------------------------------------------------
// Rows

void VisualView::rowsMayHaveChanged()
{
    rebuildRows();
}

void VisualView::rebuildRows()
{
    const int lines = document()->blockCount();
    const auto &blocks = m_model->blocks();

    QSet<int> caretLines;
    if (!isReadOnly()) {
        for (const QTextCursor &c : cursors()) {
            const int from = document()->findBlock(c.selectionStart()).blockNumber();
            const int to = document()->findBlock(c.selectionEnd()).blockNumber();
            for (int line = from; line <= to; ++line)
                caretLines.insert(line);
        }
    }

    m_rows.clear();
    m_expandedBlocks.clear();
    m_lineToRow.assign(lines, 0);
    int line = 0;
    while (line < lines) {
        const int b = m_model->blockAt(line);
        if (b >= 0) {
            const int first = int(blocks[b].first_line);
            const int last = qMin(int(blocks[b].last_line), lines - 1);
            bool expanded = false;
            for (int l = first; l <= last && !expanded; ++l)
                expanded = caretLines.contains(l);
            if (expanded) {
                m_expandedBlocks.insert(b);
                for (int l = first; l <= last; ++l) {
                    m_lineToRow[l] = int(m_rows.size());
                    m_rows.push_back({{l, l, false}, b});
                }
            } else {
                for (int l = first; l <= last; ++l)
                    m_lineToRow[l] = int(m_rows.size());
                m_rows.push_back({{first, last, true}, b});
            }
            line = last + 1;
            continue;
        }
        // A line outside every block (blank): a small gap unless edited.
        m_lineToRow[line] = int(m_rows.size());
        m_rows.push_back({{line, line, !caretLines.contains(line)}, -1});
        ++line;
    }

    // Drop renders for blocks that no longer exist.
    if (m_cache.size() > int(blocks.size()) * 2 + 64)
        m_cache.clear();
}

int VisualView::rowCount() const
{
    return qMax(1, int(m_rows.size()));
}

TextView::Row VisualView::row(int index) const
{
    if (index < 0 || index >= int(m_rows.size()))
        return {0, 0, false};
    return m_rows[index].row;
}

int VisualView::rowOfLine(int line) const
{
    return line >= 0 && line < int(m_lineToRow.size()) ? m_lineToRow[line] : 0;
}

int VisualView::blockForRow(const Row &r) const
{
    const int index = rowOfLine(r.firstLine);
    return index < int(m_rows.size()) ? m_rows[index].block : -1;
}

// ---------------------------------------------------------------------------
// Rendering

RenderContext VisualView::context() const
{
    RenderContext ctx;
    ctx.baseDirectory = m_baseDirectory;
    ctx.codeFont = font();
    // Body text uses the UI face, scaled with the editor's zoom.
    const qreal zoom = font().pointSizeF() / theme::editorFont().pointSizeF();
    ctx.font = QApplication::font();
    ctx.font.setPointSizeF(ctx.font.pointSizeF() * 1.15 * zoom);
    ctx.width = textWidth();
    ctx.devicePixelRatio = devicePixelRatioF();
    ctx.mermaidTheme = Settings::instance().text(key::mermaidTheme);
    return ctx;
}

const RenderedBlock *VisualView::rendered(int blockIndex)
{
    const auto &blocks = m_model->blocks();
    if (blockIndex < 0 || blockIndex >= int(blocks.size()))
        return nullptr;
    const core::Block &block = blocks[blockIndex];
    const QString source = m_model->sourceText(int(block.first_line), int(block.last_line));
    const RenderContext ctx = context();
    // Identical blocks render identically, so the source is the cache key.
    const QString key = QStringLiteral("%1|%2|%3|").arg(int(ctx.width)).arg(ctx.font.pointSizeF()).arg(int(block.kind))
                        + source;
    auto it = m_cache.find(key);
    if (it == m_cache.end())
        it = m_cache.insert(key, std::shared_ptr<RenderedBlock>(RenderedBlock::create(block, source, ctx)));
    return it->get();
}

const RenderedBlock *VisualView::renderedForRow(const Row &r)
{
    return rendered(blockForRow(r));
}

qreal VisualView::renderedRowHeight(const Row &r, qreal)
{
    const int b = blockForRow(r);
    if (b < 0)
        return lineHeight() * gapRowFactor;
    const RenderedBlock *block = rendered(b);
    return block ? block->height() : lineHeight();
}

void VisualView::paintRenderedRow(QPainter &painter, const Row &r, const QRectF &rect)
{
    if (const RenderedBlock *block = renderedForRow(r))
        block->paint(painter, rect.topLeft(), rect.width());
}

void VisualView::paintLineBackground(QPainter &painter, int line, const QRectF &rect)
{
    // Source lines of the block being edited get the active-block shade and
    // a navy bar, as in the mockup.
    if (!m_highlightActiveBlock)
        return;
    const int b = m_model->blockAt(line);
    if (b < 0 || !m_expandedBlocks.contains(b))
        return;
    const qreal left = textLeft() - activeBarInset;
    const QRectF area(left, rect.top(), viewport()->width() - left - sideMargin + activeBarInset, rect.height());
    painter.fillRect(area, theme::activeBlock);
    painter.fillRect(QRectF(left, rect.top(), 2, rect.height()), theme::accent);
}

int VisualView::hitTestRenderedRow(const Row &r, const QPointF &point)
{
    QTextDocument *doc = document();
    const QTextBlock first = doc->findBlockByNumber(r.firstLine);
    const int b = blockForRow(r);
    const RenderedBlock *block = rendered(b);
    if (b < 0 || !block)
        return first.position();

    const auto &info = m_model->blocks()[b];
    switch (info.kind) {
    case core::BlockKind::Html: {
        const int pos = block->hitTest(point);
        if (pos < 0)
            return first.position();
        const QString source = m_model->sourceText(r.firstLine, r.lastLine);
        return first.position() + sourceOffsetFor(block->renderedText().left(pos), source);
    }
    case core::BlockKind::Code:
    case core::BlockKind::FrontMatter: {
        const int pos = block->hitTest(point);
        if (block->kind() == RenderedBlock::Kind::Code && pos >= 0)
            return qMin(first.position() + pos, doc->characterCount() - 1);
        [[fallthrough]];
    }
    default: {
        // Images (math, diagrams): the first content line.
        const QTextBlock content = r.lastLine > r.firstLine ? first.next() : first;
        return content.position();
    }
    }
}

bool VisualView::renderedRowPressed(const Row &r, const QPointF &point, QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return false;
    const RenderedBlock *block = renderedForRow(r);
    if (!block)
        return false;
    const QString anchor = block->anchorAt(point);
    if (anchor.isEmpty())
        return false;
    if (anchor.startsWith(QLatin1String("vn-task:"))) {
        toggleTask(anchor.section(u':', 1).toInt());
        return true;
    }
    Q_EMIT linkActivated(anchor);
    return true;
}

void VisualView::toggleTask(int line)
{
    if (isReadOnly())
        return;
    const QTextBlock block = document()->findBlockByNumber(line);
    const QString text = block.text();
    static const QRegularExpression box(QStringLiteral("^\\s*(?:[-*+]|\\d+[.)])\\s+\\[([ xX])\\]"));
    const QRegularExpressionMatch m = box.match(text);
    if (!m.hasMatch())
        return;
    QTextCursor c(block);
    c.setPosition(block.position() + m.capturedStart(1));
    c.setPosition(block.position() + m.capturedEnd(1), QTextCursor::KeepAnchor);
    c.insertText(m.captured(1) == QLatin1String(" ") ? QStringLiteral("x") : QStringLiteral(" "));
}

void VisualView::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() == Qt::NoButton) {
        // Pointing hand and URL tooltip over links in rendered text.
        qreal top = 0;
        const int index = rowAt(event->position().y(), &top);
        QString anchor;
        if (index >= 0 && row(index).rendered) {
            if (const RenderedBlock *block = renderedForRow(row(index)))
                anchor = block->anchorAt(QPointF(event->position().x() - textLeft(), event->position().y() - top));
        }
        viewport()->setCursor(anchor.isEmpty() ? Qt::IBeamCursor : Qt::PointingHandCursor);
        if (!anchor.isEmpty() && !anchor.startsWith(QLatin1String("vn-task:")))
            QToolTip::showText(event->globalPosition().toPoint(), anchor, viewport());
        else
            QToolTip::hideText();
    }
    TextView::mouseMoveEvent(event);
}

void VisualView::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::FontChange)
        m_cache.clear();
    TextView::changeEvent(event);
}

} // namespace vn
