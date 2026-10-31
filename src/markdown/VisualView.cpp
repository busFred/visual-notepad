#include "VisualView.h"

#include "../PlainText.h"
#include "../Settings.h"
#include "../Theme.h"
#include "ImageViewer.h"
#include "MarkdownModel.h"
#include "RemoteImages.h"

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolTip>

#include <algorithm>
#include <optional>

namespace vn {

namespace {

constexpr qreal sideMargin = 26;
constexpr qreal topMargin = 12;
constexpr qreal gapRowFactor = 0.6;
constexpr qreal activeBarInset = 10;
// Render cache budget per view, in KiB (see RenderedBlock::cost).
constexpr int renderCacheKiB = 48 * 1024;
// Renders the view keeps in hand besides the cache (see rendered()): a screen
// shows a few dozen blocks, at most a few hundred. What the last paint showed
// is kept even past the budget (one block alone may be over it), up to a
// hard limit on their number.
constexpr int renderMax = 512;
constexpr int renderKiB = 16 * 1024;
constexpr int renderHardMax = 2 * renderMax;

bool sameChar(QChar a, QChar b)
{
    return a == b || (a.isSpace() && b.isSpace());
}

} // namespace

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

int renderedOffsetFor(const QString &rendered, const QString &source, int sourceOffset)
{
    int r = 0;
    const int end = qMin(sourceOffset, int(source.size()));
    for (int s = 0; s < end && r < rendered.size(); ++s) {
        if (sameChar(rendered.at(r), source.at(s)))
            ++r;
    }
    return r;
}

VisualView::VisualView(MarkdownModel *model, QWidget *parent)
    : TextView(model->document(), parent)
    , m_model(model)
    , m_cache(renderCacheKiB)
{
    setLineNumbersVisible(false);
    // The edited block has its own shading instead.
    setHighlightCurrentLine(false);
    setTextMargins(sideMargin, sideMargin, topMargin);
    viewport()->setMouseTracking(true);
    connect(model, &MarkdownModel::optionsChanged, this, [this] {
        clearRenderCache();
        relayout();
    });
    // Rendered blocks have the theme's colours baked in.
    connect(&theme::notifier(), &theme::Notifier::changed, this, [this] {
        clearRenderCache();
        relayout();
    });
    // Typing settled: one visible view asks for the reparse (off the GUI
    // thread for a large document), every view then rebuilds its rows
    // (hidden ones when shown).
    connect(model, &MarkdownModel::reparseDue, this, [this] {
        if (isVisible())
            m_model->flushInBackground();
    });
    connect(model, &MarkdownModel::reparsed, this, &VisualView::relayout);
    connect(model, &MarkdownModel::reparsed, this, &VisualView::updateRemoteBanner);
    connect(&DiagramNotifier::instance(), &DiagramNotifier::ready, this, &VisualView::asyncRenderReady);
    connect(&RemoteImages::instance(), &RemoteImages::ready, this, &VisualView::asyncRenderReady);
    // Pictures are only requested once the user agreed (the bar asks).
    connect(model, &MarkdownModel::remoteConsentChanged, this, [this] {
        clearRenderCache();
        relayout();
        updateRemoteBanner();
    });
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString &changedKey) {
        if (changedKey != QLatin1String(key::remoteImages))
            return;
        clearRenderCache();
        relayout();
        updateRemoteBanner();
    });
    // Rows are built when the view is first shown (see showEvent).
}

void VisualView::setBaseDirectory(const QString &directory)
{
    if (m_baseDirectory == directory)
        return;
    m_baseDirectory = directory;
    clearRenderCache();
    relayout();
}

void VisualView::clearRenderCache()
{
    m_cache.clear();
    m_pendingKeys.clear();
    resetRenders();
}

void VisualView::resetRenders()
{
    m_renders.clear();
    m_renderOrder.clear();
    m_renderKiB = 0;
}

void VisualView::asyncRenderReady()
{
    // Drop the source stand-ins of diagrams and the blocks with pictures on
    // their way so they render again, now with the results.
    if (m_pendingKeys.isEmpty())
        return;
    for (const QString &key : std::as_const(m_pendingKeys))
        m_cache.remove(key);
    m_pendingKeys.clear();
    std::erase_if(m_renderOrder, [this](int line) {
        const auto it = m_renders.find(line);
        if (!it->second.block->isPending())
            return false;
        m_renderKiB -= it->second.cost;
        m_renders.erase(it);
        return true;
    });
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
    // A hidden view (Source mode) waits until it is shown, so edits don't
    // re-parse the document for nothing.
    if (!isVisible()) {
        m_rowsStale = true;
        return;
    }
    rebuildRows();
}

void VisualView::showEvent(QShowEvent *event)
{
    TextView::showEvent(event);
    // Edits made while hidden are parsed once now rather than shown stale,
    // but a large document with nothing usable parsed is parsed on a worker
    // (its source shows meanwhile).
    if (m_model->hasUsableSpans() || !m_model->parseInBackground())
        m_model->flush();
    if (m_rowsStale)
        relayout();
    updateRemoteBanner();
}

void VisualView::hideEvent(QHideEvent *event)
{
    TextView::hideEvent(event);
    m_showsParse = false;
    m_unpainted = true;
}

// ---------------------------------------------------------------------------
// Remote pictures

bool VisualView::remoteImagesAllowed() const
{
    return Settings::instance().text(key::remoteImages) != QLatin1String("never")
           && m_model->remoteConsent() == MarkdownModel::RemoteConsent::Allow;
}

void VisualView::updateRemoteBanner()
{
    QStringList urls;
    if (isVisible() && Settings::instance().text(key::remoteImages) != QLatin1String("never")
        && m_model->remoteConsent() == MarkdownModel::RemoteConsent::Undecided) {
        if (m_remoteUrlsGeneration != m_model->parseGeneration()) {
            // The last parse as it is (blocks() would parse a large document
            // again here, on the GUI thread). Blocks are scanned by their
            // HTML, so after an edit only the changed ones are laid out again.
            m_remoteUrls.clear();
            QSet<QString> seen;
            QHash<size_t, QStringList> scans;
            const RenderContext ctx = context();
            for (int i = 0, n = m_model->parsedBlockCount(); i < n; ++i) {
                const core::Block &block = m_model->parsedBlock(i);
                // (Most blocks can't ask for any: no need to keep their scans.)
                if (!RenderedBlock::mayAskForPictures(block))
                    continue;
                const size_t hash = qHash(QByteArrayView(block.html.data(), qsizetype(block.html.size())));
                const auto known = m_remoteScans.constFind(hash);
                const QStringList urls = known != m_remoteScans.constEnd() ? *known : RenderedBlock::remoteImages(block, ctx);
                scans.insert(hash, urls);
                for (const QString &url : urls) {
                    if (!seen.contains(url)) {
                        seen.insert(url);
                        m_remoteUrls.append(url);
                    }
                }
            }
            m_remoteScans = std::move(scans);
            m_remoteUrlsGeneration = m_model->parseGeneration();
        }
        urls = m_remoteUrls;
    }
    if (urls.isEmpty()) {
        if (m_banner && !m_banner->isHidden()) {
            m_banner->hide();
            layoutBanner();
        }
        return;
    }

    if (!m_banner) {
        m_banner = new QFrame(this);
        m_banner->setObjectName(QStringLiteral("RemoteImageBanner"));
        m_banner->setAttribute(Qt::WA_StyledBackground);
        auto *layout = new QHBoxLayout(m_banner);
        // The right edge is left to the lock/pencil pill floating over it.
        layout->setContentsMargins(int(sideMargin), 6, 84, 6);
        layout->setSpacing(8);
        m_bannerText = new QLabel(m_banner);
        m_bannerText->setWordWrap(true);
        m_bannerText->setTextFormat(Qt::PlainText); // Host names come from the document.
        layout->addWidget(m_bannerText, 1);
        const auto addButton = [&](const QString &text, MarkdownModel::RemoteConsent consent) {
            auto *button = new QPushButton(text, m_banner);
            button->setObjectName(QStringLiteral("BannerButton"));
            layout->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, consent] {
                // Choosing to load is also the way to try failed pictures again.
                if (consent == MarkdownModel::RemoteConsent::Allow)
                    RemoteImages::instance().clearFailures();
                m_model->setRemoteConsent(consent);
                setFocus();
            });
        };
        addButton(tr("Load images"), MarkdownModel::RemoteConsent::Allow);
        addButton(tr("Don't load"), MarkdownModel::RemoteConsent::Deny);
    }
    // Hosts in order of appearance, with how many pictures each serves.
    QStringList hosts;
    QHash<QString, int> counts;
    for (const QString &url : urls) {
        const QUrl parsed(url);
        const QString host = parsed.port() > 0 ? QStringLiteral("%1:%2").arg(parsed.host()).arg(parsed.port()) : parsed.host();
        if (counts[host]++ == 0)
            hosts.append(host);
    }
    QStringList shown;
    for (const QString &host : hosts)
        shown.append(QStringLiteral("%1 (%2)").arg(host).arg(counts[host]));
    constexpr int maxShown = 4;
    QString text = shown.mid(0, maxShown).join(QLatin1String(", "));
    if (shown.size() > maxShown)
        text += tr(" and %n more host(s)", "", shown.size() - maxShown);
    m_bannerText->setText(tr("%n remote image(s) from %1. Load them only if you trust the links in this file.", "",
                             int(urls.size())).arg(text));
    m_bannerText->setToolTip(plainToolTip(shown.join(u'\n') + QLatin1String("\n\n")
                                          + tr("Pictures on this computer or the local network are never loaded."),
                                          4096));
    m_banner->show();
    layoutBanner();
}

void VisualView::layoutBanner()
{
    int height = 0;
    if (m_banner && !m_banner->isHidden()) {
        const int width = viewport()->width();
        height = m_banner->layout()->totalHeightForWidth(width);
        m_banner->setGeometry(0, 0, width, height);
    }
    if (height != m_bannerHeight) {
        m_bannerHeight = height;
        setViewportMargins(0, height, 0, 0);
    }
}

void VisualView::resizeEvent(QResizeEvent *event)
{
    TextView::resizeEvent(event);
    layoutBanner();
}

void VisualView::rebuildRows()
{
    const int lines = document()->blockCount();
    // Shown before a large document is parsed: its source stands in, line
    // by line, until the worker is done (see MarkdownModel::parseInBackground).
    if (m_model->isAwaitingParse()
        || (!m_model->hasUsableSpans() && !m_showsParse && m_model->parseInBackground())) {
        if (m_standIn && int(m_lineToRow.size()) == lines)
            return;
        m_rows.clear();
        m_rows.reserve(lines);
        m_lineToRow.resize(lines);
        for (int line = 0; line < lines; ++line) {
            m_lineToRow[line] = line;
            m_rows.push_back({{line, line, false}, -1});
        }
        m_expandedBlocks.clear();
        m_partSpans.clear();
        m_rowsExpanded.clear();
        m_rowsStale = false;
        m_rowsRevision = -1;
        m_standIn = true;
        return;
    }
    // The rendered rows that replace the stand-in keep its place.
    std::optional<Place> standInPlace;
    if (m_standIn) {
        standInPlace = place();
        m_standIn = false;
    }
    m_showsParse = true;
    const auto &spans = m_model->spans();

    // Blocks open for editing only while this view has focus, so a document
    // opens (and stays, when you look elsewhere) fully rendered.
    // While the mouse is selecting, rows stay as they are: blocks must not
    // swap to source and shift under the pointer mid-drag. mouseReleaseEvent
    // applies the expansion once the selection is made.
    if (isMouseSelecting() && !m_rows.empty() && !m_rowsStale && int(m_lineToRow.size()) == lines
        && m_rowsRevision == m_model->revision())
        return;

    // Line ranges carets touch, widened to whole spans, sorted and merged.
    std::vector<std::pair<int, int>> expanded;
    if (!isReadOnly() && hasFocus()) {
        for (const QTextCursor &c : cursors()) {
            int from = document()->findBlock(c.selectionStart()).blockNumber();
            int to = document()->findBlock(c.selectionEnd()).blockNumber();
            if (const int s = m_model->spanAt(from); s >= 0)
                from = spans[s].first;
            if (const int s = m_model->spanAt(to); s >= 0)
                to = spans[s].last;
            expanded.push_back({from, to});
        }
        std::sort(expanded.begin(), expanded.end());
        std::vector<std::pair<int, int>> merged;
        for (const auto &range : expanded) {
            if (!merged.empty() && range.first <= merged.back().second)
                merged.back().second = qMax(merged.back().second, range.second);
            else
                merged.push_back(range);
        }
        expanded = std::move(merged);
    }
    if (!m_rowsStale && m_rowsRevision == m_model->revision() && int(m_lineToRow.size()) == lines
        && expanded == m_rowsExpanded)
        return;

    m_rows.clear();
    m_rows.reserve(spans.size() * 2 + 16);
    m_expandedBlocks.clear();
    m_partSpans.clear();
    m_lineToRow.assign(lines, 0);
    // Queries come in ascending line order, so one pass over the ranges.
    size_t next = 0;
    const auto touched = [&](int first, int last) {
        while (next < expanded.size() && expanded[next].second < first)
            ++next;
        return next < expanded.size() && expanded[next].first <= last;
    };
    size_t s = 0;
    int line = 0;
    while (line < lines) {
        while (s < spans.size() && spans[s].first < line)
            ++s;
        if (s < spans.size() && spans[s].first == line) {
            const MarkdownModel::Span &span = spans[s];
            const int last = qMin(span.last, lines - 1);
            const bool open = touched(line, last);
            if (open)
                m_expandedBlocks.insert(int(s));
            // Edited lines with nothing parsed to stand in show as source
            // until the reparse.
            if (open || (span.block < 0 && span.previous < 0)) {
                for (int l = line; l <= last; ++l) {
                    m_lineToRow[l] = int(m_rows.size());
                    m_rows.push_back({{l, l, false}, int(s)});
                }
            } else if (madeInParts(int(s))) {
                // A row a part, so that only the parts on screen are laid out.
                m_partSpans.insert(int(s));
                for (int from = line; from <= last; from += RenderedBlock::partLines) {
                    const int to = qMin(last, from + RenderedBlock::partLines - 1);
                    for (int l = from; l <= to; ++l)
                        m_lineToRow[l] = int(m_rows.size());
                    m_rows.push_back({{from, to, true}, int(s)});
                }
            } else {
                for (int l = line; l <= last; ++l)
                    m_lineToRow[l] = int(m_rows.size());
                m_rows.push_back({{line, last, true}, int(s)});
            }
            line = last + 1;
            ++s;
            continue;
        }
        // A line outside every block (blank): a small gap unless edited.
        m_lineToRow[line] = int(m_rows.size());
        m_rows.push_back({{line, line, !touched(line, line)}, -1});
        ++line;
    }
    m_rowsStale = false;
    m_rowsRevision = m_model->revision();
    m_rowsExpanded = std::move(expanded);
    if (standInPlace)
        restorePlace(*standInPlace);
}

VisualView::Place VisualView::place()
{
    qreal top = 0;
    const int bottom = rowAt(viewport()->height() - 1, &top);
    const int caret = textCursor().blockNumber();
    return {topLinePosition(), caret >= topLine() && (bottom < 0 || caret <= row(bottom).lastLine)};
}

void VisualView::restorePlace(const Place &place)
{
    scrollToLinePosition(place.top);
    if (place.caretShown)
        ensureCursorVisible();
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

int VisualView::spanForRow(const Row &r) const
{
    const int index = rowOfLine(r.firstLine);
    return index < int(m_rows.size()) ? m_rows[index].span : -1;
}

// ---------------------------------------------------------------------------
// Rendering

RenderContext VisualView::context() const
{
    RenderContext ctx;
    ctx.baseDirectory = m_baseDirectory;
    ctx.codeFont = font();
    // Body text follows the "text font" setting, scaled with the editor's zoom.
    const qreal zoom = font().pointSizeF() / theme::editorFont().pointSizeF();
    ctx.font = theme::textFont();
    ctx.font.setPointSizeF(ctx.font.pointSizeF() * zoom);
    ctx.lineHeight = Settings::instance().value(key::textLineHeight).toDouble();
    ctx.width = textWidth();
    ctx.devicePixelRatio = devicePixelRatioF();
    ctx.mermaidTheme = Settings::instance().text(key::mermaidTheme);
    ctx.allowRemoteImages = remoteImagesAllowed();
    return ctx;
}

VisualView::LongBox &VisualView::longBox(int blockIndex)
{
    if (m_longBoxesGeneration != m_model->parseGeneration()) {
        m_longBoxes.clear();
        m_longBoxesGeneration = m_model->parseGeneration();
    }
    return m_longBoxes[blockIndex];
}

bool VisualView::madeInParts(int s)
{
    const MarkdownModel::Span &span = m_model->spans()[s];
    const int blockIndex = span.block >= 0 ? span.block : span.previous;
    if (blockIndex < 0 || span.last - span.first < RenderedBlock::partLines)
        return false;
    QTextDocument *doc = document();
    const QTextBlock last = doc->findBlockByNumber(qMin(span.last, doc->blockCount() - 1));
    // As long as sourceText(span.first, span.last).
    const qsizetype chars = last.position() + last.length() - 1 - doc->findBlockByNumber(span.first).position();
    const core::Block &block = m_model->parsedBlock(blockIndex);
    // A parsed block's lines are its own until the next parse; a stand-in's
    // were edited since.
    if (span.block < 0)
        return RenderedBlock::isLongCodeBox(block, chars);
    LongBox &box = longBox(blockIndex);
    if (box.parts == -1)
        box.parts = RenderedBlock::isLongCodeBox(block, chars);
    return box.parts == 1;
}

const VisualView::Render *VisualView::rendered(const Row &r)
{
    const int index = spanForRow(r);
    const auto &spans = m_model->spans();
    if (index < 0 || index >= int(spans.size()))
        return nullptr;
    const MarkdownModel::Span &span = spans[index];
    // An edited block shows its last parse until the reparse.
    const int blockIndex = span.block >= 0 ? span.block : span.previous;
    if (blockIndex < 0)
        return nullptr;

    const RenderContext ctx = context();
    if (m_rendersRevision != m_model->revision() || m_rendersWidth != ctx.width
        || m_rendersDpr != ctx.devicePixelRatio) {
        resetRenders();
        m_rendersRevision = m_model->revision();
        m_rendersWidth = ctx.width;
        m_rendersDpr = ctx.devicePixelRatio;
    }
    if (const auto it = m_renders.find(r.firstLine); it != m_renders.end()) {
        it->second.used = m_frame;
        return &it->second;
    }

    const core::Block &block = m_model->parsedBlock(blockIndex);
    // A part of a long code box: the lines of the row, and where it sits in the box.
    const bool inParts = m_partSpans.contains(index);
    RenderedBlock::Part part;
    QString source;
    if (inParts) {
        source = m_model->sourceText(r.firstLine, r.lastLine);
        part.first = r.firstLine == span.first;
        part.last = r.lastLine >= qMin(span.last, document()->blockCount() - 1);
        int wrap = -1;
        LongBox *box = span.block >= 0 ? &longBox(blockIndex) : nullptr;
        if (box)
            wrap = box->wrap;
        if (wrap == -1) {
            wrap = RenderedBlock::wrapsAnywhere(m_model->sourceText(span.first, span.last));
            if (box)
                box->wrap = wrap;
        }
        part.wrapAnywhere = wrap == 1;
    } else {
        source = m_model->sourceText(span.first, span.last);
    }
    // Identical blocks render identically, so the source (by hash, checked
    // on a hit) is the key. A stand-in pairs an old parse with new source:
    // never mistake it for the real render of that source.
    QString key = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
                      .arg(int(ctx.width))
                      .arg(ctx.font.pointSizeF())
                      .arg(ctx.font.family())
                      .arg(ctx.lineHeight)
                      .arg(int(block.kind))
                      .arg(ctx.codeFont.family())
                      .arg(ctx.devicePixelRatio)
                      .arg(source.size())
                      .arg(qHash(source));
    if (inParts)
        key += QStringLiteral("|part:%1%2%3").arg(int(part.first)).arg(int(part.last)).arg(int(part.wrapAnywhere));
    else if (span.block < 0)
        key += QStringLiteral("|stale:%1:%2").arg(m_model->parseGeneration()).arg(blockIndex);
    Render memo;
    const Render *hit = m_cache.object(key);
    if (hit && hit->source == source) {
        memo = *hit;
    } else {
        const std::shared_ptr<RenderedBlock> created(inParts ? RenderedBlock::createPart(source, part, ctx)
                                                             : RenderedBlock::create(block, source, ctx));
        const int cost = created->cost() + int(source.size() / 512);
        ++m_renderCount;
        memo = {source, key, created, cost};
        if (created->isPending())
            m_pendingKeys.insert(key);
        m_cache.insert(key, new Render(memo), cost);
    }
    memo.used = m_frame;
    Render &kept = m_renders[r.firstLine] = std::move(memo);
    // Keep the memos of the latest renders only: the cache holds the rest
    // within its budget, while memos on every block seen would keep them all
    // alive, however far the view has scrolled on. Those the paint in
    // progress asked for stay, though, or one block over the budget would be
    // made again for every other block shown with it.
    m_renderOrder.push_back(r.firstLine);
    m_renderKiB += kept.cost;
    for (size_t rotated = 0; rotated < m_renderOrder.size()
                             && (int(m_renders.size()) > renderMax || m_renderKiB > renderKiB);) {
        const int oldest = m_renderOrder.front();
        m_renderOrder.pop_front();
        const auto it = m_renders.find(oldest);
        if (oldest == r.firstLine || (it->second.used == m_frame && int(m_renders.size()) <= renderHardMax)) {
            m_renderOrder.push_back(oldest);
            ++rotated;
            continue;
        }
        m_renderKiB -= it->second.cost;
        m_renders.erase(it);
    }
    return &kept;
}

const RenderedBlock *VisualView::renderedForRow(const Row &r)
{
    const Render *render = rendered(r);
    return render ? render->block.get() : nullptr;
}

const VisualView::Render *VisualView::memoized(const Row &r) const
{
    if (m_rendersRevision != m_model->revision() || m_rendersWidth != textWidth()
        || m_rendersDpr != devicePixelRatioF())
        return nullptr;
    const auto it = m_renders.find(r.firstLine);
    return it != m_renders.end() ? &it->second : nullptr;
}

qreal VisualView::renderedRowHeight(const Row &r, qreal)
{
    if (spanForRow(r) < 0)
        return lineHeight() * gapRowFactor;
    // Before the first paint the width may change yet (a splitter lays out
    // a view it shows after showing it, and a mode switch shows it in a
    // half first): blocks not rendered at this width are measured roughly,
    // and rendered at the width the paint finds (see paintEvent).
    if (m_unpainted) {
        if (const Render *render = memoized(r))
            return render->block->height();
        m_estimated = true;
        return lineHeight() * (r.lastLine - r.firstLine + 1);
    }
    const RenderedBlock *block = renderedForRow(r);
    return block ? block->height() : lineHeight();
}

void VisualView::paintRenderedRow(QPainter &painter, const Row &r, const QRectF &rect)
{
    if (const Render *render = rendered(r)) {
        const RenderedBlock *block = render->block.get();
        block->paint(painter, rect.topLeft(), rect.width(), selectionForRow(r, block, render->source));
    }
}

void VisualView::paintEvent(QPaintEvent *event)
{
    if (m_unpainted) {
        // The rough heights give way to the real ones, at the same place:
        // the same source line at the top, and the caret in view if it was.
        std::optional<Place> estimatedPlace;
        if (m_estimated)
            estimatedPlace = place();
        m_unpainted = false;
        m_estimated = false;
        if (estimatedPlace) {
            relayout();
            restorePlace(*estimatedPlace);
        }
    }
    ++m_frame;
    TextView::paintEvent(event);
}

RenderedSelection VisualView::selectionForRow(const Row &r, const RenderedBlock *block, const QString &source) const
{
    RenderedSelection result;
    if (!block || block->kind() == RenderedBlock::Kind::Empty || spanForRow(r) < 0 || !hasSelection())
        return result;
    QTextDocument *doc = document();
    const QTextBlock first = doc->findBlockByNumber(r.firstLine);
    const QTextBlock last = doc->findBlockByNumber(r.lastLine);
    const int rowStart = first.position();
    const int rowEnd = last.position() + last.length() - 1; // Without the line break.

    QString text;
    for (const QTextCursor &c : cursors()) {
        if (!c.hasSelection())
            continue;
        const int a = c.selectionStart();
        const int b = c.selectionEnd();
        if (b <= rowStart || a >= rowEnd)
            continue;
        switch (block->kind()) {
        case RenderedBlock::Kind::Image: {
            // Highlight the picture when the selection spans its content.
            const int contentStart = r.lastLine > r.firstLine ? first.next().position() : rowStart;
            const int contentEnd = r.lastLine > r.firstLine ? last.position() : rowEnd;
            if (a <= contentStart && b >= contentEnd)
                result.whole = true;
            break;
        }
        case RenderedBlock::Kind::Code:
            // The code box is a plain-text copy of the source (a part of one
            // has the line break after it too).
            result.ranges.append({qMax(a, rowStart) - rowStart, qMin(b, rowEnd + 1) - rowStart});
            break;
        case RenderedBlock::Kind::Rich: {
            // Only a selection end inside the block needs the (linear) mapping.
            if (text.isNull())
                text = block->renderedText();
            const int from = a <= rowStart ? 0 : renderedOffsetFor(text, source, a - rowStart);
            const int to = b >= rowEnd ? int(text.size()) : renderedOffsetFor(text, source, b - rowStart);
            if (to > from)
                result.ranges.append({from, to});
            break;
        }
        case RenderedBlock::Kind::Empty:
            break;
        }
    }
    return result;
}

QTextCursor VisualView::lineUnitAt(int position) const
{
    // A rendered block is one paragraph however many source lines it has
    // (and rows it is made in).
    const QTextBlock b = document()->findBlock(position);
    const int index = rowOfLine(b.blockNumber());
    Row r = row(index);
    if (r.rendered && index < int(m_rows.size()) && m_partSpans.contains(m_rows[index].span)) {
        int first = index;
        int last = index;
        while (first > 0 && m_rows[first - 1].span == m_rows[index].span)
            --first;
        while (last + 1 < int(m_rows.size()) && m_rows[last + 1].span == m_rows[index].span)
            ++last;
        r = {m_rows[first].row.firstLine, m_rows[last].row.lastLine, true};
    }
    if (!r.rendered || r.lastLine <= r.firstLine)
        return TextView::lineUnitAt(position);
    const QTextBlock last = document()->findBlockByNumber(r.lastLine);
    QTextCursor cursor(document());
    cursor.setPosition(document()->findBlockByNumber(r.firstLine).position());
    cursor.setPosition(last.next().isValid() ? last.next().position() : last.position() + last.length() - 1,
                       QTextCursor::KeepAnchor);
    return cursor;
}

void VisualView::mouseReleaseEvent(QMouseEvent *event)
{
    const bool wasSelecting = isMouseSelecting();
    TextView::mouseReleaseEvent(event);
    // Editable: the blocks the selection touches open for editing now.
    if (wasSelecting && !isReadOnly())
        relayout();
}

void VisualView::paintLineBackground(QPainter &painter, int line, const QRectF &rect)
{
    // Source lines of the block being edited get the active-block shade and
    // a navy bar, as in the mockup.
    if (!m_highlightActiveBlock)
        return;
    const int index = rowOfLine(line);
    const int s = index < int(m_rows.size()) ? m_rows[index].span : -1;
    if (s < 0 || !m_expandedBlocks.contains(s))
        return;
    const qreal left = textLeft() - activeBarInset;
    const QRectF area(left, rect.top(), viewport()->width() - left - sideMargin + activeBarInset, rect.height());
    painter.fillRect(area, theme::color(theme::Role::activeBlock));
    painter.fillRect(QRectF(left, rect.top(), 2, rect.height()), theme::color(theme::Role::accent));
}

int VisualView::hitTestRenderedRow(const Row &r, const QPointF &point)
{
    QTextDocument *doc = document();
    const QTextBlock first = doc->findBlockByNumber(r.firstLine);
    const int s = spanForRow(r);
    const Render *render = rendered(r);
    if (s < 0 || !render)
        return first.position();
    const RenderedBlock *block = render->block.get();

    const MarkdownModel::Span &span = m_model->spans()[s];
    const auto &info = m_model->parsedBlock(span.block >= 0 ? span.block : span.previous);
    switch (info.kind) {
    case core::BlockKind::Html: {
        const int pos = block->hitTest(point);
        if (pos < 0)
            return first.position();
        return first.position() + sourceOffsetFor(block->renderedText().left(pos), render->source);
    }
    case core::BlockKind::Code:
    case core::BlockKind::FrontMatter: {
        const int pos = block->hitTest(point);
        if (block->kind() == RenderedBlock::Kind::Code && pos >= 0)
            return qMin(first.position() + pos, doc->characterCount() - 1);
        [[fallthrough]];
    }
    default: {
        // Images (math, diagrams): the lower half is the end of the block, so
        // a drag past the picture selects all of it; else the first content line.
        if (block->kind() == RenderedBlock::Kind::Image && point.y() > block->height() / 2) {
            const QTextBlock last = doc->findBlockByNumber(r.lastLine);
            return last.position() + last.length() - 1;
        }
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
        return openPicture(block, point, event);
    if (anchor.startsWith(QLatin1String("vn-task:"))) {
        toggleTask(anchor.section(u':', 1).toInt());
        return true;
    }
    Q_EMIT linkActivated(anchor);
    return true;
}

bool VisualView::zoomClick(const QMouseEvent *event) const
{
    // Read-only, a click is free to enlarge; editable, a click opens the
    // block's source, so it takes the link modifier (as for links in source).
    const auto mods = event->modifiers() & ~Qt::KeypadModifier;
    return isReadOnly() ? mods == Qt::NoModifier : mods == linkModifier();
}

bool VisualView::openPicture(const RenderedBlock *block, const QPointF &point, const QMouseEvent *event)
{
    if (!zoomClick(event))
        return false;
    const auto picture = block->pictureAt(point);
    if (!picture)
        return false;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QImage image = block->pictureImage(*picture);
    QApplication::restoreOverrideCursor();
    if (image.isNull())
        return false;
    // Closes itself; the window is the parent so it stays on top of this tab.
    (new ImageViewer(image, picture->title, window()))->show();
    return true;
}

std::vector<TextView::LineAnchor> VisualView::rowLineAnchors(const Row &r, qreal)
{
    // Gap and source rows are one line each; rendered blocks know where
    // their table rows, list items and code lines are.
    std::vector<LineAnchor> anchors;
    if (!r.rendered || r.lastLine == r.firstLine)
        return anchors;
    // (Rough heights spread the lines evenly; see renderedRowHeight.)
    if (m_unpainted && !memoized(r))
        return anchors;
    if (const RenderedBlock *block = renderedForRow(r)) {
        for (const auto &[offset, y] : block->lineAnchors())
            anchors.push_back({qreal(r.firstLine + offset), y});
    }
    return anchors;
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
    // A click is one edit, not typing: show the new state now.
    m_model->flush();
}

void VisualView::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() == Qt::NoButton) {
        // Pointing hand and URL tooltip over links in rendered text.
        qreal top = 0;
        const int index = rowAt(event->position().y(), &top);
        QString anchor;
        bool zoomable = false;
        if (index >= 0 && row(index).rendered) {
            if (const RenderedBlock *block = renderedForRow(row(index))) {
                const QPointF point(event->position().x() - textLeft(), event->position().y() - top);
                anchor = block->anchorAt(point);
                zoomable = anchor.isEmpty() && zoomClick(event) && block->pictureAt(point);
            }
        }
        viewport()->setCursor(anchor.isEmpty() && !zoomable ? Qt::IBeamCursor : Qt::PointingHandCursor);
        if (!anchor.isEmpty() && !anchor.startsWith(QLatin1String("vn-task:")))
            QToolTip::showText(event->globalPosition().toPoint(), plainToolTip(anchor), viewport());
        else
            QToolTip::hideText();
    }
    TextView::mouseMoveEvent(event);
}

void VisualView::focusInEvent(QFocusEvent *event)
{
    TextView::focusInEvent(event);
    rebuildRows();
    viewport()->update();
}

void VisualView::focusOutEvent(QFocusEvent *event)
{
    TextView::focusOutEvent(event);
    // Context menus and the find bar take focus briefly; keep the block open
    // for those, close it when focus really moves elsewhere.
    if (event->reason() == Qt::PopupFocusReason)
        return;
    rebuildRows();
    viewport()->update();
}

void VisualView::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::FontChange)
        clearRenderCache();
    TextView::changeEvent(event);
}

} // namespace vn
