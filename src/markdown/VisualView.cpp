#include "VisualView.h"

#include "../PlainText.h"
#include "../Settings.h"
#include "../Theme.h"
#include "ImageViewer.h"
#include "MarkdownModel.h"
#include "RemoteImages.h"
#include "TableToolbar.h"

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>

#include <algorithm>
#include <climits>
#include <string_view>
#include <optional>
#include <utility>

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
// Typing goes into the rendering of blocks up to this size (source
// characters and lines): each keystroke renders the block again, which
// takes about 1 ms a KiB for a paragraph and more for a list (13 ms for 200
// items). Larger blocks open as source when typed into.
constexpr int liveEditChars = 2000;
constexpr int liveEditLines = 40;

// The ASCII a typographic character is typed as (the typographer makes
// these of quotes, dashes and dots).
QChar typedAs(QChar c)
{
    switch (c.unicode()) {
    case 0x201C: // “ ” „
    case 0x201D:
    case 0x201E:
        return u'"';
    case 0x2018: // ‘ ’ ‚
    case 0x2019:
    case 0x201A:
        return u'\'';
    case 0x2013: // – —
    case 0x2014:
        return u'-';
    case 0x2026: // …
        return u'.';
    default:
        return c;
    }
}

// Whether rendered character a can be source character b.
bool sameChar(QChar a, QChar b)
{
    return a == b || (a.isSpace() && b.isSpace()) || typedAs(a) == b;
}

// The extra source characters a typographic character stands for: "--"
// for –, "..." for …
int typedExtra(QChar c)
{
    switch (c.unicode()) {
    case 0x2013:
        return 1;
    case 0x2014:
    case 0x2026:
        return 2;
    default:
        return 0;
    }
}

bool isLineBreak(QChar c)
{
    return c == u'\n' || c == QChar::ParagraphSeparator || c == QChar::LineSeparator;
}

// The source index of the character rendered character `index` was matched
// with (as sourceOffsetFor matches them), or -1.
int matchOf(const QString &rendered, const QString &source, int index)
{
    int r = 0;
    for (int s = 0; s < source.size() && r <= index; ++s) {
        if (sameChar(rendered.at(r), source.at(s))) {
            if (r == index)
                return s;
            ++r;
        }
    }
    return -1;
}

// Text with whitespace dropped and typographic characters as typed, to
// compare a render with what was expected of it.
QString comparable(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        if (c.isSpace() || isLineBreak(c))
            continue;
        const QChar typed = typedAs(c);
        out += typed;
        for (int i = 0; i < typedExtra(c); ++i)
            out += typed;
    }
    return out;
}

// Characters Markdown would read as syntax anywhere, typed into rendered
// text: inserted with a backslash so they show as typed.
bool escapedAnywhere(QChar c, bool math)
{
    switch (c.unicode()) {
    case '*':
    case '_':
    case '#':
    case '`':
    case '[':
    case ']':
    case '<':
    case '|':
    case '\\':
    case '~':
        return true;
    case '$':
        return math;
    default:
        return false;
    }
}

// `typed` escaped for insertion into a source line after `before` (the
// line's text up to the insertion point); contentStart is where the line's
// text starts (after indentation, list and quote markers).
QString escapeTyped(const QString &typed, const QString &before, int contentStart, bool math)
{
    QString out;
    // What the line holds from its content start, as it grows.
    QString content = before.mid(qMin(contentStart, int(before.size())));
    for (const QChar c : typed) {
        bool escape = escapedAnywhere(c, math);
        const bool atStart = content.trimmed().isEmpty();
        if (atStart && (c == u'-' || c == u'+' || c == u'>' || c == u'='))
            escape = true;
        // "1." or "1)" at the start of a line starts a list.
        if ((c == u'.' || c == u')') && !content.isEmpty()) {
            const QString t = content.trimmed();
            escape = escape || (!t.isEmpty() && std::all_of(t.cbegin(), t.cend(), [](QChar d) { return d.isDigit(); }));
        }
        if (escape)
            out += u'\\';
        out += c;
        content += c;
    }
    return out;
}

bool isBlank(const QString &line)
{
    return line.trimmed().isEmpty();
}

// Where a line's text starts: after indentation, quote and list markers.
int contentStartOf(const QString &line)
{
    const md::ListLine item = md::parseListLine(line);
    if (item.valid)
        return item.contentStart;
    int i = 0;
    while (i < line.size() && line.at(i).isSpace())
        ++i;
    return i;
}

// Whether a block is Markdown text: a paragraph, heading, list or quote
// written in Markdown, not in HTML.
bool isTextual(const core::Block &block, const QString &source)
{
    if (block.kind != core::BlockKind::Html)
        return false;
    for (const QChar c : source) {
        if (c == u'<')
            return false;
        if (!c.isSpace())
            break;
    }
    const std::string_view html(block.html.data(), block.html.size());
    const auto starts = [&](std::string_view prefix) { return html.substr(0, prefix.size()) == prefix; };
    return starts("<p") || starts("<ul") || starts("<ol") || starts("<blockquote")
           || (html.size() > 2 && starts("<h") && html[2] >= '1' && html[2] <= '6');
}

} // namespace

int caretSourceOffset(const QString &rendered, const QString &source, int k)
{
    const int n = int(rendered.size());
    if (n == 0)
        return int(source.size());
    if (k <= 0) {
        const int m = matchOf(rendered, source, 0);
        return m >= 0 ? m : int(source.size());
    }
    k = qMin(k, n);
    const int before = matchOf(rendered, source, k - 1);
    if (before < 0)
        return int(source.size());
    // After a line or paragraph break (a source line break, rendered as one
    // or as a space): before the next character, past the next line's markers.
    if (k < n && (isLineBreak(rendered.at(k - 1)) || source.at(before) == u'\n')) {
        const int m = matchOf(rendered, source, k);
        if (m >= 0)
            return m;
    }
    int after = before + 1;
    const QChar typed = typedAs(rendered.at(k - 1));
    for (int i = 0; i < typedExtra(rendered.at(k - 1)) && after < source.size() && source.at(after) == typed; ++i)
        ++after;
    return after;
}

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
    // A table in source form is realigned when the caret leaves it.
    setRealignTablesOnLeave(true);
    setTextMargins(sideMargin, sideMargin, topMargin);
    viewport()->setMouseTracking(true);
    connect(this, &TextView::scrolled, this, &VisualView::scheduleToolbarUpdate);
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
    connect(model, &MarkdownModel::reparsed, this, [this] {
        // Every block is parsed now: what was live renders from the parse.
        m_liveStart = QTextCursor();
        m_liveEnd = QTextCursor();
        relayout();
    });
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
    scheduleToolbarUpdate();
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
    m_hasFocus = hasFocus();
    scheduleToolbarUpdate();
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
    scheduleToolbarUpdate();
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
    scheduleToolbarUpdate();
}

// ---------------------------------------------------------------------------
// Table toolbar

void VisualView::scheduleToolbarUpdate()
{
    if (m_toolbarUpdatePending)
        return;
    m_toolbarUpdatePending = true;
    QTimer::singleShot(0, this, [this] {
        m_toolbarUpdatePending = false;
        updateTableToolbar();
    });
}

void VisualView::updateTableToolbar()
{
    const auto hideToolbar = [this] {
        if (m_tableToolbar)
            m_tableToolbar->hide();
    };
    // The box over a cell follows its cell (the view scrolled, was resized or
    // re-rendered); one whose cell is gone ends there.
    if (m_cellEditor && !placeCellEditor())
        closeCellEditor(TableCellEditor::Exit::FocusLost);
    if (!isVisible() || isReadOnly() || !(m_hasFocus || m_cellEditor) || cursorCount() != 1 || !markdownEditing())
        return hideToolbar();
    const md::TableInfo info = tableAtCaret();
    if (!info.found)
        return hideToolbar();
    qreal firstTop = 0;
    qreal firstBottom = 0;
    qreal lastTop = 0;
    qreal lastBottom = 0;
    const bool firstShown = lineBounds(info.firstLine, &firstTop, &firstBottom);
    const bool lastShown = lineBounds(info.lastLine, &lastTop, &lastBottom);
    if (!firstShown && !lastShown && textCursor().blockNumber() > info.lastLine)
        return hideToolbar();

    if (!m_tableToolbar) {
        m_tableToolbar = new TableToolbar(viewport());
        connect(m_tableToolbar, &TableToolbar::operation, this, &VisualView::toolbarOperation);
        connect(m_tableToolbar->moreMenu(), &QMenu::aboutToShow, this, [this] {
            m_tableToolbar->moreMenu()->clear();
            populateTableMenu(m_tableToolbar->moreMenu());
        });
    }
    // A press on the toolbar gives the focus to the first widget up from the
    // button that takes it on a click (QApplication), the view: that would end
    // the cell box, and the toolbar with it, before the button is released.
    // The search stops at a widget whose focus proxy has the focus already.
    m_tableToolbar->setFocusProxy(m_cellEditor);
    m_tableToolbar->setState(info);
    m_tableToolbar->adjustSize();
    const QSize size = m_tableToolbar->size();
    constexpr int gap = 4;
    // Above the table; else below it; else (it covers the viewport) at the top.
    int y = 2;
    if (firstShown && firstTop - size.height() - gap >= 0)
        y = int(firstTop) - size.height() - gap;
    else if (lastShown && lastBottom + size.height() + gap <= viewport()->height())
        y = int(lastBottom) + gap;
    const int x = qBound(0, int(textLeft() - activeBarInset), qMax(0, viewport()->width() - size.width()));
    m_tableToolbar->move(x, y);
    m_tableToolbar->show();
    m_tableToolbar->raise();
}

void VisualView::toolbarOperation(md::TableOp op)
{
    // Choosing the alignment a column has goes back to the default.
    const md::TableInfo info = tableAtCaret();
    const struct
    {
        md::TableOp op;
        md::TableAlignment alignment;
    } same[] = {{md::TableOp::AlignLeft, md::TableAlignment::Left},
                {md::TableOp::AlignCenter, md::TableAlignment::Center},
                {md::TableOp::AlignRight, md::TableAlignment::Right}};
    for (const auto &s : same) {
        if (s.op == op && info.found && info.alignment == s.alignment)
            op = md::TableOp::AlignNone;
    }
    editTable(op);
    setFocus();
}

// ---------------------------------------------------------------------------
// Cell editing

bool VisualView::tableCellPressed(const Row &r, const QPointF &point, QMouseEvent *event)
{
    if (isReadOnly() || !markdownEditing() || (event->modifiers() & ~Qt::KeypadModifier))
        return false;
    const Render *render = rendered(r);
    if (!render)
        return false;
    const auto hit = render->block->tableCellAt(point);
    if (!hit)
        return false;
    // Not consumed: the press goes on to place the caret and start a drag;
    // mouseReleaseEvent decides whether it was a click.
    m_pendingCell = {true, r.firstLine, hit->row, hit->col, event->position().toPoint(), textCursor()};
    return false;
}

bool VisualView::openCellEditor(int line, int row, int col)
{
    if (m_cellEditor)
        closeCellEditor(TableCellEditor::Exit::Commit);
    const int index = rowOfLine(line);
    if (index < 0 || index >= rowCount())
        return false;
    const Row r = this->row(index);
    if (!r.rendered || r.firstLine != line)
        return false;
    const Render *render = rendered(r);
    if (!render)
        return false;
    // The block must be one table (and not, say, front matter, shown as a
    // table), the rendered one with the cells the source has: a pipe table,
    // or an HTML block that renders as nothing but its <table>.
    int at = 0;
    md::TableInfo info = md::tableInfo(render->source, 0, 0);
    if (!info.found) {
        static const QRegularExpression tag(QStringLiteral("^ {0,3}<table[\\s>/]"),
                                            QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
        if (const QRegularExpressionMatch m = tag.match(render->source); m.hasMatch()) {
            at = int(QStringView(render->source).first(m.capturedStart()).count(u'\n'));
            info = md::tableInfo(render->source, at, 0);
        }
    }
    const int lines = r.lastLine - r.firstLine;
    const QSize cells = render->block->tableCells();
    if (!info.found || info.firstLine != at || info.lastLine > lines || (!info.html && info.lastLine != lines)
        || cells.height() != info.rows || cells.width() != info.cols || row >= info.rows || col >= info.cols)
        return false;
    const md::TableCell cell = md::tableCell(render->source, at, row, col);
    if (!cell.found)
        return false;

    m_cell = {line, at, row, col, cell.text};
    if (!m_cellEditor) {
        m_cellEditor = new TableCellEditor(viewport());
        connect(m_cellEditor, &TableCellEditor::finished, this, [this](TableCellEditor::Exit how) { closeCellEditor(how); });
    }
    m_cellEditor->setFont(context().font);
    m_cellEditor->open(cell.text);
    if (!placeCellEditor()) {
        TableCellEditor *editor = m_cellEditor;
        m_cellEditor = nullptr;
        m_cell = {};
        delete editor;
        return false;
    }
    setTableTarget(line + at, row, col);
    m_cellEditor->show();
    m_cellEditor->raise();
    m_cellEditor->setFocus(Qt::MouseFocusReason);
    scheduleToolbarUpdate();
    return true;
}

bool VisualView::placeCellEditor()
{
    if (!m_cellEditor || m_cell.line < 0)
        return false;
    const int index = rowOfLine(m_cell.line);
    if (index < 0 || index >= rowCount())
        return false;
    const Row r = row(index);
    if (!r.rendered || r.firstLine != m_cell.line)
        return false;
    const Render *render = rendered(r);
    if (!render)
        return false;
    const md::TableCell cell = md::tableCell(render->source, m_cell.at, m_cell.row, m_cell.col);
    const auto rect = render->block->tableCellRect(m_cell.row, m_cell.col);
    if (!cell.found || !rect || cell.text != m_cell.original)
        return false;
    // The cell's line is where the view scrolls to when the cell is out of view.
    const int sourceLine = m_cell.line + cell.line;
    for (int attempt = 0; attempt < 2; ++attempt) {
        qreal top = 0;
        qreal bottom = 0;
        if (lineBounds(m_cell.line, &top, &bottom)) {
            const qreal y = top + rect->top();
            if (y + rect->height() > 0 && y < viewport()->height()) {
                const int width = qBound(60, int(rect->width()), qMax(60, viewport()->width() - int(textLeft() + rect->left())));
                m_cellEditor->place(QPoint(int(textLeft() + rect->left()), int(y)), width, int(rect->height()));
                return true;
            }
        }
        if (attempt == 0)
            scrollToLine(sourceLine);
    }
    return false;
}

void VisualView::insertTable(int rows, int cols)
{
    const int first = insertTableLines(rows, cols, true);
    if (first < 0)
        return;
    const md::TableInfo info = tableInfoAt(first, 0);
    if (info.found && !isReadOnly()) {
        // The caret below the table (past the blank line after it), so the
        // table is shown as a table, not as its source.
        const int below = qMin(info.lastLine + 2, document()->blockCount() - 1);
        if (below > info.lastLine) {
            setTextCursor(QTextCursor(document()->findBlockByNumber(below)));
            m_model->flush();
            if (openCellEditor(first, 0, 0)) {
                m_cellEditor->selectAll();
                return;
            }
        }
    }
    moveToTableCell(first, 0, 0); // Its source, then: the header selected.
}

void VisualView::tableTargetEditing()
{
    // A command of the table (the toolbar, a menu) is about to change it: the
    // text typed so far goes in first.
    closeCellEditor(TableCellEditor::Exit::Commit);
}

void VisualView::closeCellEditor(TableCellEditor::Exit how)
{
    using Exit = TableCellEditor::Exit;
    if (!m_cellEditor)
        return;
    // Forget the box first: closing it moves the focus, which ends it again.
    TableCellEditor *editor = m_cellEditor;
    m_cellEditor = nullptr;
    const CellTarget cell = m_cell;
    m_cell = {};
    setTableTarget(-1);
    const QString text = editor->toPlainText();
    editor->hide();
    editor->deleteLater();
    if (how != Exit::FocusLost)
        setFocus(Qt::OtherFocusReason);

    bool changed = false;
    if (how != Exit::Cancel && text != cell.original)
        changed = editTableAt(cell.line + cell.at, md::TableOp::SetCell, cell.row, cell.col, text, false);
    if (changed)
        m_model->flush(); // Show the new text (and widths) now.
    scheduleToolbarUpdate();

    // Tab, Shift+Tab and the arrows go on to the next cell.
    if (how == Exit::Commit || how == Exit::Cancel || how == Exit::FocusLost)
        return;
    const md::TableCell now = tableCellOf(cell.line + cell.at, cell.row, cell.col);
    if (!now.found)
        return;
    const md::TableInfo info = tableInfoAt(now.line, now.start);
    int row = cell.row;
    int col = cell.col;
    switch (how) {
    case Exit::Next:
        if (col + 1 < info.cols) {
            ++col;
        } else if (row + 1 < info.rows) {
            ++row;
            col = 0;
        } else {
            // Past the last cell: a new row.
            if (!editTableAt(cell.line + cell.at, md::TableOp::RowBelow, row, 0, {}, false))
                return;
            m_model->flush();
            ++row;
            col = 0;
        }
        break;
    case Exit::Previous:
        if (col > 0) {
            --col;
        } else if (row > 0) {
            --row;
            col = info.cols - 1;
        } else {
            return;
        }
        break;
    case Exit::Up:
        if (row == 0)
            return;
        --row;
        break;
    case Exit::Down:
        if (row + 1 >= info.rows)
            return;
        ++row;
        break;
    default:
        return;
    }
    openCellEditor(cell.line, row, col);
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
    std::optional<Place> keptPlace;
    if (m_standIn) {
        keptPlace = place();
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

    // The open blocks close when the caret moves out of them (a move: no
    // edit since the rows were built).
    if (m_sourceOpen && !m_openLines.empty() && !m_rowsStale && m_rowsRevision == m_model->revision()
        && int(m_lineToRow.size()) == lines) {
        const int caret = textCursor().blockNumber();
        if (std::none_of(m_openLines.begin(), m_openLines.end(),
                         [caret](const auto &range) { return range.first <= caret && caret <= range.second; }))
            m_sourceOpen = false;
    }
    // Line ranges carets touch, widened to whole spans, sorted and merged:
    // the open blocks.
    std::vector<std::pair<int, int>> expanded;
    if (!isReadOnly() && m_sourceOpen) {
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
    m_openLines = expanded;
    if (!hasFocus())
        expanded.clear();
    // A caret on a blank line between blocks (or between the blocks of
    // lines edited in rendered text) shows that line as source.
    std::vector<int> caretGaps;
    if (!isReadOnly() && hasFocus()) {
        for (const QTextCursor &c : cursors()) {
            const int line = c.blockNumber();
            const int at = m_model->spanAt(line);
            if (at < 0 || (spans[at].block < 0 && isBlank(c.block().text())))
                caretGaps.push_back(line);
        }
        std::sort(caretGaps.begin(), caretGaps.end());
    }
    if (!m_rowsStale && m_rowsRevision == m_model->revision() && int(m_lineToRow.size()) == lines
        && expanded == m_rowsExpanded && caretGaps == m_rowsCaretGaps)
        return;

    // Rows that matched the document and now open or close for editing
    // (a caret moved into a table, a selection ended in it) change height and
    // count: the top of the view stays at the same source line, or a click
    // among the last rows of a long table would scroll to its first row.
    if (!keptPlace && !m_rows.empty() && !m_rowsStale && m_rowsRevision == m_model->revision()
        && int(m_lineToRow.size()) == lines)
        keptPlace = place();

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
    int liveFirst = -1;
    int liveLast = -2;
    liveLines(&liveFirst, &liveLast);
    const auto gapRow = [&](int l) {
        m_lineToRow[l] = int(m_rows.size());
        m_rows.push_back({{l, l, !touched(l, l) && !std::binary_search(caretGaps.begin(), caretGaps.end(), l)}, -1});
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
            // Lines edited in rendered text show the blocks a parse of
            // their own source makes, with blank lines between them.
            if (!open && span.block < 0 && line <= liveLast && last >= liveFirst) {
                const rust::Vec<core::Block> &blocks = liveBlocks(m_model->sourceText(line, last));
                int l = line;
                for (const core::Block &b : blocks) {
                    const int first = line + int(b.first_line);
                    const int end = qMin(last, line + int(b.last_line));
                    if (first < l || end < first)
                        continue;
                    while (l < first)
                        gapRow(l++);
                    for (; l <= end; ++l)
                        m_lineToRow[l] = int(m_rows.size());
                    m_rows.push_back({{first, end, true}, int(s)});
                }
                while (l <= last)
                    gapRow(l++);
            } else if (open || (span.block < 0 && span.previous < 0)) {
                // Edited lines with nothing parsed to stand in show as
                // source until the reparse.
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
        gapRow(line++);
    }
    m_rowsStale = false;
    m_rowsRevision = m_model->revision();
    m_rowsExpanded = std::move(expanded);
    m_rowsCaretGaps = std::move(caretGaps);
    if (keptPlace)
        restorePlace(*keptPlace);
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
    // An edited block shows its last parse until the reparse, unless it was
    // edited in rendered text (see rebuildRows).
    const int blockIndex = span.block >= 0 ? span.block : span.previous;
    int liveFirst = -1;
    int liveLast = -2;
    const bool live = span.block < 0 && liveLines(&liveFirst, &liveLast) && span.first <= liveLast
                      && span.last >= liveFirst && !m_partSpans.contains(index);
    if (blockIndex < 0 && !live)
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

    Render memo;
    if (live) {
        const rust::Vec<core::Block> &blocks = liveBlocks(m_model->sourceText(span.first, span.last));
        const auto it = std::find_if(blocks.begin(), blocks.end(),
                                     [&](const core::Block &b) { return int(b.first_line) == r.firstLine - span.first; });
        if (it == blocks.end())
            return nullptr;
        memo = liveRender(*it, m_model->sourceText(r.firstLine, r.lastLine), r.firstLine, ctx);
    } else {
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
    QString key = renderKey(ctx, block.kind, source);
    if (inParts)
        key += QStringLiteral("|part:%1%2%3").arg(int(part.first)).arg(int(part.last)).arg(int(part.wrapAnywhere));
    else if (span.block < 0)
        key += QStringLiteral("|stale:%1:%2").arg(m_model->parseGeneration()).arg(blockIndex);
    const Render *hit = m_cache.object(key);
    if (hit && hit->source == source) {
        memo = *hit;
    } else {
        const std::shared_ptr<RenderedBlock> created(inParts ? RenderedBlock::createPart(source, part, ctx)
                                                             : RenderedBlock::create(block, source, ctx));
        const int cost = created->cost() + int(source.size() / 512);
        ++m_renderCount;
        memo = {source, key, created, block.kind, cost, isTextual(block, source)};
        if (created->isPending())
            m_pendingKeys.insert(key);
        m_cache.insert(key, new Render(memo), cost);
    }
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

QString VisualView::renderKey(const RenderContext &ctx, core::BlockKind kind, const QString &source) const
{
    // Identical blocks render identically, so the source (by hash, checked
    // on a hit) is the key.
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
        .arg(int(ctx.width))
        .arg(ctx.font.pointSizeF())
        .arg(ctx.font.family())
        .arg(ctx.lineHeight)
        .arg(int(kind))
        .arg(ctx.codeFont.family())
        .arg(ctx.devicePixelRatio)
        .arg(source.size())
        .arg(qHash(source));
}

bool VisualView::liveLines(int *first, int *last) const
{
    if (m_liveStart.isNull() || m_liveEnd.isNull())
        return false;
    *first = m_liveStart.blockNumber();
    *last = m_liveEnd.blockNumber();
    return true;
}

const rust::Vec<core::Block> &VisualView::liveBlocks(const QString &source)
{
    for (const LiveParse &parse : m_liveParses) {
        if (parse.source == source)
            return parse.blocks;
    }
    const QByteArray utf8 = source.toUtf8();
    m_liveParses.push_front({source, core::blocks(rust::Str(utf8.constData(), utf8.size()), m_model->options())});
    // A few: the block typed in, before and after the edit, and its rows.
    while (m_liveParses.size() > 4)
        m_liveParses.pop_back();
    return m_liveParses.front().blocks;
}

VisualView::Render VisualView::liveRender(const core::Block &parsed, const QString &source, int firstLine,
                                          const RenderContext &ctx)
{
    // Not the key of the document parse's render: a block parsed alone does
    // not see the link definitions and footnotes of the others.
    QString key = renderKey(ctx, parsed.kind, source) + QLatin1String("|live");
    // Task boxes name their line in the document.
    const std::string_view html(parsed.html.data(), parsed.html.size());
    const bool tasks = html.find("vn-task:") != std::string_view::npos;
    if (tasks)
        key += QStringLiteral("|at:%1").arg(firstLine);
    const Render *hit = m_cache.object(key);
    if (hit && hit->source == source)
        return *hit;
    core::Block block = parsed;
    if (tasks) {
        static const QRegularExpression task(QStringLiteral("vn-task:(\\d+)"));
        QString text = QString::fromUtf8(parsed.html.data(), qsizetype(parsed.html.size()));
        QString out;
        qsizetype at = 0;
        for (auto it = task.globalMatch(text); it.hasNext();) {
            const QRegularExpressionMatch m = it.next();
            out += QStringView(text).mid(at, m.capturedStart(1) - at);
            out += QString::number(m.captured(1).toInt() + firstLine - int(parsed.first_line));
            at = m.capturedEnd(1);
        }
        out += QStringView(text).mid(at);
        const QByteArray utf8 = out.toUtf8();
        block.html = rust::String(utf8.constData(), utf8.size());
    }
    const std::shared_ptr<RenderedBlock> created(RenderedBlock::create(block, source, ctx));
    const int cost = created->cost() + int(source.size() / 512);
    ++m_renderCount;
    Render memo{source, key, created, parsed.kind, cost, isTextual(parsed, source)};
    if (created->isPending())
        m_pendingKeys.insert(key);
    m_cache.insert(key, new Render(memo), cost);
    return memo;
}

QString VisualView::renderedTextAt(int line)
{
    const Row r = row(rowOfLine(line));
    const RenderedBlock *block = r.rendered ? renderedForRow(r) : nullptr;
    return block ? block->renderedText() : QString();
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
    const Render *render = rendered(r);
    if (!render)
        return;
    const RenderedBlock *block = render->block.get();
    block->paint(painter, rect.topLeft(), rect.width(), selectionForRow(r, block, render->source));
    // Carets in rendered text (editable views): where their source position
    // renders.
    if (m_paintCarets.empty())
        return;
    QTextDocument *doc = document();
    const int start = doc->findBlockByNumber(r.firstLine).position();
    const QTextBlock last = doc->findBlockByNumber(r.lastLine);
    const int end = last.position() + last.length() - 1;
    auto it = std::lower_bound(m_paintCarets.begin(), m_paintCarets.end(), start);
    QString text;
    for (; it != m_paintCarets.end() && *it <= end; ++it) {
        int k = *it - start;
        if (block->kind() == RenderedBlock::Kind::Rich) {
            if (text.isNull())
                text = block->renderedText();
            k = renderedOffsetFor(text, render->source, k);
        }
        QRectF caret(0, 0, 2, qMin<qreal>(lineHeight(), block->height()));
        if (const auto at = block->caretRect(k))
            caret = *at;
        painter.fillRect(caret.translated(rect.topLeft()), theme::color(theme::Role::text));
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
    // The carets drawn in rendered text (TextView draws those in source rows).
    m_paintCarets.clear();
    if (hasFocus() && caretBlinkOn() && !isReadOnly()) {
        for (const QTextCursor &c : cursors())
            m_paintCarets.push_back(c.position());
        std::sort(m_paintCarets.begin(), m_paintCarets.end());
    }
    TextView::paintEvent(event);
    m_paintCarets.clear();
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
    const PendingCell pending = std::exchange(m_pendingCell, PendingCell());
    TextView::mouseReleaseEvent(event);
    if (pending.active && event->button() == Qt::LeftButton && !hasSelection() && !isReadOnly()
        && (event->position().toPoint() - pending.pos).manhattanLength() <= QApplication::startDragDistance()) {
        // A click on a cell of a rendered table: its box, not the table's
        // source. The caret goes back to where it was.
        const QTextCursor clicked = textCursor();
        setTextCursor(pending.caret);
        if (openCellEditor(pending.line, pending.row, pending.col))
            return;
        setTextCursor(clicked);
    }
    realignLeftTable();
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

    switch (render->kind) {
    case core::BlockKind::Html: {
        const int pos = block->hitTest(point);
        if (pos < 0)
            return first.position();
        return first.position() + caretSourceOffset(block->renderedText(), render->source, pos);
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
        return openPicture(block, point, event) || tableCellPressed(r, point, event);
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
    m_hasFocus = true;
    scheduleToolbarUpdate();
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
    m_hasFocus = false;
    scheduleToolbarUpdate();
    rebuildRows();
    viewport()->update();
}

void VisualView::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::FontChange)
        clearRenderCache();
    TextView::changeEvent(event);
}

// ---------------------------------------------------------------------------
// Rendered editing
//
// The caret of an editable view sits in rendered text as in a word
// processor: a click places it, typing goes in where it renders and the
// block renders again (from a parse of its own source; see liveRender).
// Every edit is checked before it is made: its block, parsed and rendered
// with it, must show what was typed (whitespace aside). An edit that fails
// that, or that rendered text cannot express (several blocks, joining two,
// several carets), opens the block as source and is made there; pictures
// and raw HTML take no typing until opened with a double click.

namespace {

// What a backslash escapes in Markdown (CommonMark's ASCII punctuation).
bool isAsciiPunctuation(QChar c)
{
    return QLatin1String("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~").contains(c);
}

bool isDelimiter(QChar c)
{
    return c == u'*' || c == u'_' || c == u'~' || c == u'`';
}

// Removes the source of rendered characters [from, to) (escapes included)
// from source; *at is where the first of them was.
QString removeRendered(const QString &rendered, const QString &source, int from, int to, int *at)
{
    std::vector<bool> removed(source.size(), false);
    int r = 0;
    for (int s = 0; s < source.size() && r < to; ++s) {
        if (!sameChar(rendered.at(r), source.at(s)))
            continue;
        if (r >= from) {
            removed[s] = true;
            if (s > 0 && source.at(s - 1) == u'\\' && isAsciiPunctuation(source.at(s)))
                removed[s - 1] = true;
            const QChar typed = typedAs(rendered.at(r));
            for (int i = 0; i < typedExtra(rendered.at(r)) && s + 1 < source.size() && source.at(s + 1) == typed; ++i)
                removed[++s] = true;
        }
        ++r;
    }
    QString out;
    out.reserve(source.size());
    *at = -1;
    for (int s = 0; s < source.size(); ++s) {
        if (removed[s]) {
            if (*at < 0)
                *at = int(out.size());
            continue;
        }
        out += source.at(s);
    }
    if (*at < 0)
        *at = int(out.size());
    return out;
}

// Removes markup left empty at `*at` ("****", "``", "[](url)").
QString removeEmptyMarkup(QString source, int *at)
{
    for (int pass = 0; pass < 4; ++pass) {
        const int p = *at;
        if (p > 0 && p < source.size() && isDelimiter(source.at(p)) && source.at(p - 1) == source.at(p)) {
            int left = p;
            while (left > 0 && source.at(left - 1) == source.at(p))
                --left;
            int right = p;
            while (right < source.size() && source.at(right) == source.at(p))
                ++right;
            if (p - left == right - p) {
                source.remove(p, right - p);
                source.remove(left, p - left);
                *at = left;
                continue;
            }
        }
        if (p > 0 && source.at(p - 1) == u'[' && QStringView(source).mid(p).startsWith(QLatin1String("]("))) {
            int depth = 0;
            int close = -1;
            for (int i = p + 1; i < source.size() && close < 0; ++i) {
                if (source.at(i) == u'\n')
                    break;
                if (source.at(i) == u'(')
                    ++depth;
                else if (source.at(i) == u')' && --depth == 0)
                    close = i;
            }
            if (close > 0) {
                source.remove(p - 1, close - p + 2);
                *at = p - 1;
                continue;
            }
        }
        break;
    }
    return source;
}

// The line of `text` holding offset p: [*start, *end).
void lineAround(const QString &text, int p, int *start, int *end)
{
    *start = p > 0 ? int(text.lastIndexOf(u'\n', p - 1)) + 1 : 0;
    const int next = int(text.indexOf(u'\n', p));
    *end = next < 0 ? int(text.size()) : next;
}

} // namespace

int VisualView::Spot::offsetOf(int position) const
{
    if (block->kind() != RenderedBlock::Kind::Rich)
        return position - start;
    return renderedOffsetFor(text, source, position - start);
}

std::optional<VisualView::Spot> VisualView::spotAt(int position)
{
    const QTextBlock b = document()->findBlock(position);
    if (!b.isValid())
        return std::nullopt;
    const int index = rowOfLine(b.blockNumber());
    if (index < 0 || index >= int(m_rows.size()) || !m_rows[index].row.rendered || m_rows[index].span < 0)
        return std::nullopt;
    const Row r = m_rows[index].row;
    const Render *render = rendered(r);
    if (!render)
        return std::nullopt;
    Spot spot;
    spot.index = index;
    spot.row = r;
    spot.start = document()->findBlockByNumber(r.firstLine).position();
    spot.source = render->source;
    spot.block = render->block;
    if (spot.block->kind() == RenderedBlock::Kind::Rich) {
        spot.text = spot.block->renderedText();
        spot.textual = render->textual && spot.block->tableCells().isEmpty();
        spot.editable = spot.textual && spot.source.size() <= liveEditChars
                        && r.lastLine - r.firstLine < liveEditLines;
    }
    return spot;
}

bool VisualView::rendersAs(const QString &source, int firstLine, const QString &expected)
{
    const RenderContext ctx = context();
    const rust::Vec<core::Block> &blocks = liveBlocks(source);
    // Where each line of the source starts, for the blocks' own sources.
    std::vector<int> starts{0};
    for (int i = 0; i < source.size(); ++i) {
        if (source.at(i) == u'\n')
            starts.push_back(i + 1);
    }
    const auto lineEnd = [&](size_t line) {
        return line + 1 < starts.size() ? starts[line + 1] - 1 : int(source.size());
    };
    QString text;
    for (const core::Block &b : blocks) {
        if (b.kind != core::BlockKind::Html || b.first_line >= starts.size())
            return false;
        const int from = starts[b.first_line];
        const int to = lineEnd(qMin<size_t>(b.last_line, starts.size() - 1));
        const Render render = liveRender(b, source.mid(from, to - from), firstLine + int(b.first_line), ctx);
        if (!render.textual)
            return false;
        text += render.block->renderedText();
        text += u'\n';
    }
    return comparable(text) == comparable(expected);
}

void VisualView::applyEdit(int from, int to, const QString &text, int caret)
{
    const QTextCursor c = textCursor();
    if (c.hasSelection() || c.position() != from || to != from) {
        QTextCursor range(document());
        range.setPosition(from);
        range.setPosition(to, QTextCursor::KeepAnchor);
        setTextCursor(range);
    }
    // As typing does: one undo step with the typing around it.
    insertTextAtCarets(text);
    if (caret >= 0 && textCursor().position() != caret) {
        QTextCursor at(document());
        at.setPosition(qBound(0, caret, document()->characterCount() - 1));
        setTextCursor(at);
    }
}

namespace {

// The change between two versions of a row's source: [*from, *to) of the
// old one becomes *text; the common start goes no further than `hint`.
void changeBetween(const QString &before, const QString &after, int hint, int *from, int *to, QString *text)
{
    int pre = 0;
    const int maxPre = qMin(hint, int(qMin(before.size(), after.size())));
    while (pre < maxPre && before.at(pre) == after.at(pre))
        ++pre;
    int suf = 0;
    while (suf < before.size() - pre && suf < after.size() - pre
           && before.at(before.size() - 1 - suf) == after.at(after.size() - 1 - suf))
        ++suf;
    *from = pre;
    *to = int(before.size()) - suf;
    *text = after.mid(pre, after.size() - suf - pre);
}

} // namespace

bool VisualView::typeRendered(const QString &typed)
{
    const QTextCursor c = textCursor();
    const std::optional<Spot> spot = spotAt(c.position());
    if (!spot || !spot->editable)
        return false;
    const int ka = spot->offsetOf(c.selectionStart());
    const int kb = spot->offsetOf(c.selectionEnd());
    const QString expected = spot->text.left(ka) + typed + spot->text.mid(kb);
    QString base = spot->source;
    // Where the text may go in the source: the caret itself (after a space
    // typed at the end, say, which does not render), else where a caret
    // before rendered character ka goes (past the markers at a line's start).
    QList<int> candidates;
    if (c.hasSelection() && kb > ka) {
        int at = 0;
        base = removeRendered(spot->text, spot->source, ka, kb, &at);
        candidates.append(at);
    } else {
        candidates.append(c.position() - spot->start);
        const int canonical = caretSourceOffset(spot->text, spot->source, ka);
        if (canonical != candidates.first())
            candidates.append(canonical);
    }
    const bool math = m_model->options().math;
    for (const int p : std::as_const(candidates)) {
        int lineStart = 0;
        int lineEnd = 0;
        lineAround(base, p, &lineStart, &lineEnd);
        const QString line = base.mid(lineStart, lineEnd - lineStart);
        const QString escaped = escapeTyped(typed, line.left(p - lineStart), contentStartOf(line), math);
        const QString after = base.left(p) + escaped + base.mid(p);
        if (!rendersAs(after, spot->row.firstLine, expected))
            continue;
        int from = 0;
        int to = 0;
        QString text;
        changeBetween(spot->source, after, c.hasSelection() ? int(spot->source.size()) : p, &from, &to, &text);
        applyEdit(spot->start + from, spot->start + to, text, spot->start + p + int(escaped.size()));
        return true;
    }
    return false;
}

bool VisualView::typeOnBlankLine(const QString &typed)
{
    const QTextCursor c = textCursor();
    const QTextBlock line = c.block();
    if (c.hasSelection() || !isBlank(line.text()))
        return false;
    // A paragraph of its own: blank lines around it.
    const QTextBlock prev = line.previous();
    const QTextBlock next = line.next();
    const bool blankBefore = !prev.isValid() || isBlank(prev.text());
    const bool blankAfter = !next.isValid() || isBlank(next.text());
    const QString escaped = escapeTyped(typed, QString(), 0, m_model->options().math);
    if (!rendersAs(escaped, line.blockNumber() + (blankBefore ? 0 : 1), typed))
        return false;
    const QString lead = blankBefore ? QString() : QStringLiteral("\n");
    applyEdit(line.position(), line.position() + line.length() - 1,
              lead + escaped + (blankAfter ? QString() : QStringLiteral("\n")),
              line.position() + int(lead.size() + escaped.size()));
    return true;
}

bool VisualView::deleteRendered(bool forward)
{
    const QTextCursor c = textCursor();
    const std::optional<Spot> spot = spotAt(c.position());
    if (!spot || !spot->editable)
        return false;
    const QString &text = spot->text;
    int ka = spot->offsetOf(c.selectionStart());
    int kb = spot->offsetOf(c.selectionEnd());
    if (!c.hasSelection() || ka == kb) {
        const int k = spot->offsetOf(c.position());
        if (forward) {
            if (k >= text.size())
                return false;
            ka = k;
            kb = k + (text.at(k).isHighSurrogate() && k + 1 < text.size() ? 2 : 1);
        } else {
            if (k <= 0)
                return false;
            ka = k - (text.at(k - 1).isLowSurrogate() && k >= 2 ? 2 : 1);
            kb = k;
        }
        // Joining the lines, items or paragraphs of a block: as source.
        for (int i = ka; i < kb; ++i) {
            if (isLineBreak(text.at(i)))
                return false;
        }
    }
    const QString expected = text.left(ka) + text.mid(kb);
    int at = 0;
    const QString removed = removeRendered(text, spot->source, ka, kb, &at);
    int cleanAt = at;
    const QString cleaned = removeEmptyMarkup(removed, &cleanAt);
    for (const auto &[after, caret] : {std::pair(cleaned, cleanAt), std::pair(removed, at)}) {
        if (!rendersAs(after, spot->row.firstLine, expected))
            continue;
        int from = 0;
        int to = 0;
        QString change;
        changeBetween(spot->source, after, caret, &from, &to, &change);
        applyEdit(spot->start + from, spot->start + to, change, spot->start + caret);
        return true;
    }
    return false;
}

bool VisualView::enterRendered()
{
    const QTextCursor c = textCursor();
    const std::optional<Spot> spot = spotAt(c.position());
    if (!spot || !spot->editable || c.hasSelection())
        return false;
    const QString &source = spot->source;
    const int firstEnd = source.contains(u'\n') ? int(source.indexOf(u'\n')) : int(source.size());
    static const QRegularExpression atx(QStringLiteral("^ {0,3}#{1,6}(?:[ \\t]|$)"));
    static const QRegularExpression setext(QStringLiteral("\\n {0,3}(?:=+|-+)[ \\t]*$"));
    const bool heading = atx.match(QStringView(source).first(firstEnd)).hasMatch()
                         || (firstEnd < source.size() && !source.startsWith(u'>') && !md::parseListLine(source.left(firstEnd)).valid
                             && setext.match(source).hasMatch());
    const int k = spot->offsetOf(c.position());
    if (heading) {
        // A new paragraph after the heading, wherever the caret is in it.
        const QString after = source + QStringLiteral("\n\n");
        if (!rendersAs(after, spot->row.firstLine, spot->text))
            return false;
        const int end = spot->start + int(source.size());
        applyEdit(end, end, QStringLiteral("\n\n"), end + 2);
        return true;
    }
    QList<int> candidates{c.position() - spot->start};
    if (const int canonical = caretSourceOffset(spot->text, source, k); canonical != candidates.first())
        candidates.append(canonical);
    for (const int p : std::as_const(candidates)) {
        int lineStart = 0;
        int lineEnd = 0;
        lineAround(source, p, &lineStart, &lineEnd);
        // The list item the caret's line is in: its marker line, at or above.
        md::ListLine item;
        for (int at = lineStart; !item.valid || item.isQuote;) {
            int s = 0;
            int e = 0;
            lineAround(source, at, &s, &e);
            item = md::parseListLine(source.mid(s, e - s));
            if (item.valid && (!item.isQuote || s == lineStart))
                break;
            if (s == 0)
                break;
            at = s - 1;
        }
        QString insert;
        QString expected = spot->text;
        if (item.valid && !item.isQuote) {
            if (item.empty)
                return false; // Ending the list: as source.
            insert = QStringLiteral("\n") + item.nextPrefix();
            if (!item.box.isEmpty())
                expected.insert(k, QChar(0x2610)); // The new item's box.
        } else if (item.valid && item.isQuote) {
            // A new paragraph in the quote.
            insert = QStringLiteral("\n") + item.lead.trimmed() + QStringLiteral("\n") + item.lead;
        } else {
            insert = QStringLiteral("\n\n");
        }
        const QString after = source.left(p) + insert + source.mid(p);
        if (!rendersAs(after, spot->row.firstLine, expected))
            continue;
        applyEdit(spot->start + p, spot->start + p, insert, spot->start + p + int(insert.size()));
        return true;
    }
    return false;
}

void VisualView::moveRendered(int key, bool select)
{
    const QTextCursor c = textCursor();
    const std::optional<Spot> spot = spotAt(c.position());
    if (!spot || spot->block->kind() != RenderedBlock::Kind::Rich)
        return;
    const QString &text = spot->text;
    const int n = int(text.size());
    const int k = spot->offsetOf(c.position());
    int target = -1;
    const auto rowEdge = [&](int direction) {
        // The nearest block in that direction (blank lines skipped): its
        // end going back, its start going on.
        for (int i = spot->index + direction; i >= 0 && i < int(m_rows.size()); i += direction) {
            if (m_rows[i].span < 0)
                continue;
            const Row r = m_rows[i].row;
            QTextDocument *doc = document();
            if (!r.rendered) {
                const QTextBlock b = doc->findBlockByNumber(direction < 0 ? r.lastLine : r.firstLine);
                return direction < 0 ? b.position() + b.length() - 1 : b.position();
            }
            const Render *render = rendered(r);
            if (!render)
                continue;
            const int start = doc->findBlockByNumber(r.firstLine).position();
            if (render->block->kind() == RenderedBlock::Kind::Rich) {
                const QString t = render->block->renderedText();
                return start + caretSourceOffset(t, render->source, direction < 0 ? int(t.size()) : 0);
            }
            return direction < 0 ? start + int(render->source.size()) : start;
        }
        return -1;
    };
    switch (key) {
    case Qt::Key_Left:
    case Qt::Key_Right: {
        const bool left = key == Qt::Key_Left;
        if (!select && c.hasSelection()) {
            target = left ? c.selectionStart() : c.selectionEnd();
            break;
        }
        if (left) {
            if (k > 0)
                target = spot->positionOf(k - (k >= 2 && text.at(k - 1).isLowSurrogate() ? 2 : 1));
            else
                target = rowEdge(-1);
        } else {
            if (k < n)
                target = spot->positionOf(k + (k + 1 < n && text.at(k).isHighSurrogate() ? 2 : 1));
            else
                target = rowEdge(1);
        }
        break;
    }
    case Qt::Key_Home:
    case Qt::Key_End: {
        auto [start, end] = spot->block->lineAt(k);
        // A wrapped line ends with the space it wrapped at.
        if (end > start && end < n && !isLineBreak(text.at(end)) && text.at(end - 1).isSpace())
            --end;
        target = spot->positionOf(key == Qt::Key_Home ? start : end);
        break;
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
        const int direction = key == Qt::Key_Up ? -1 : 1;
        const std::optional<QRectF> caret = spot->block->caretRect(k);
        if (!caret)
            return;
        const qreal x = m_verticalPosition == c.position() && m_verticalX >= 0 ? m_verticalX : caret->left();
        const qreal y = direction < 0 ? caret->top() - 1 : caret->bottom() + 1;
        if (y >= 0 && y < spot->block->height()) {
            const int hit = spot->block->hitTest(QPointF(x, y));
            const std::optional<QRectF> to = hit >= 0 ? spot->block->caretRect(hit) : std::nullopt;
            if (to && (direction < 0 ? to->top() < caret->top() - 0.5 : to->top() > caret->top() + 0.5))
                target = spot->positionOf(hit);
        }
        for (int i = spot->index + direction; target < 0 && i >= 0 && i < int(m_rows.size()); i += direction) {
            if (m_rows[i].span < 0)
                continue;
            const Row r = m_rows[i].row;
            const QTextBlock first = document()->findBlockByNumber(r.firstLine);
            if (!r.rendered) {
                target = first.position();
                break;
            }
            const Render *render = rendered(r);
            if (!render)
                continue;
            const RenderedBlock *block = render->block.get();
            if (block->kind() == RenderedBlock::Kind::Rich) {
                const QString t = block->renderedText();
                const std::optional<QRectF> edge = block->caretRect(direction < 0 ? int(t.size()) : 0);
                const qreal ty = edge ? edge->center().y() : 0;
                target = first.position() + caretSourceOffset(t, render->source, qMax(0, block->hitTest(QPointF(x, ty))));
            } else {
                target = hitTestRenderedRow(r, QPointF(x, direction < 0 ? qMax<qreal>(0, block->height() - 1) : 0));
            }
        }
        if (target < 0)
            return;
        QTextCursor moved = c;
        moved.setPosition(target, select ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
        setTextCursor(moved);
        m_verticalX = x;
        m_verticalPosition = textCursor().position();
        return;
    }
    default:
        return;
    }
    if (target < 0)
        return;
    QTextCursor moved = c;
    moved.setPosition(target, select ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    setTextCursor(moved);
}

void VisualView::openBlockAtCaret()
{
    if (isReadOnly())
        return;
    m_sourceOpen = true;
    m_openLines.clear(); // Not a caret move out of them: see rebuildRows.
    if (isVisible())
        rebuildRows();
    scheduleToolbarUpdate();
    viewport()->update();
}

void VisualView::closeBlocks()
{
    if (!m_sourceOpen)
        return;
    m_sourceOpen = false;
    m_openLines.clear();
    if (isVisible())
        rebuildRows();
    scheduleToolbarUpdate();
    viewport()->update();
}

void VisualView::openAndForward(QKeyEvent *event, int caret)
{
    if (caret >= 0) {
        QTextCursor at(document());
        at.setPosition(qBound(0, caret, document()->characterCount() - 1));
        setTextCursor(at);
    }
    openBlockAtCaret();
    TextView::keyPressEvent(event);
}

bool VisualView::renderedKey(QKeyEvent *event)
{
    if (isReadOnly() || m_sourceOpen)
        return false;
    const auto mods = event->modifiers() & ~Qt::KeypadModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const bool ctrl = mods & Qt::ControlModifier;
    const bool alt = mods & Qt::AltModifier;
    const int key = event->key();
    const QString text = event->text();
    // As TextView takes text: AltGr arrives as Ctrl+Alt on Windows.
    const bool typing = !text.isEmpty() && text.at(0).isPrint() && (!ctrl || alt) && !(alt && !ctrl);
    const bool erase = key == Qt::Key_Backspace || (key == Qt::Key_Delete && !(shift && !ctrl));
    const bool enter = key == Qt::Key_Return || key == Qt::Key_Enter;
    const bool tab = (key == Qt::Key_Tab || key == Qt::Key_Backtab) && !ctrl && !alt;
    const bool edit = typing || erase || enter || tab;
    // Several carets: their edits are made in the source.
    if (cursorCount() > 1) {
        if (!edit)
            return false;
        openAndForward(event);
        return true;
    }
    const QTextCursor c = textCursor();
    const std::optional<Spot> spot = spotAt(c.position());
    if (!spot) {
        // A blank line (shown as source for the caret) or source lines.
        if (typing && typeOnBlankLine(text))
            return true;
        if (edit && c.hasSelection() && spotAt(c.anchor())) {
            openAndForward(event);
            return true;
        }
        return false;
    }
    const bool inRow = c.selectionStart() >= spot->start && c.selectionEnd() <= spot->start + spot->source.size();
    const bool rich = spot->block->kind() == RenderedBlock::Kind::Rich;
    if (!ctrl && !alt && rich && inRow
        && (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down
            || key == Qt::Key_Home || key == Qt::Key_End)) {
        moveRendered(key, shift);
        return true;
    }
    if (!edit)
        return false;
    // A selection over more than one block: edited as source.
    if (!inRow) {
        openAndForward(event);
        return true;
    }
    if (!spot->editable) {
        // Tab moves between the cells of a table, as in source.
        if (tab && tableAtCaret().found)
            return false;
        // Pictures (math, diagrams) and raw HTML take no typing until
        // opened (a double click): where it would go is not known.
        const bool table = rich && !spot->block->tableCells().isEmpty();
        if (spot->block->kind() == RenderedBlock::Kind::Image || spot->block->kind() == RenderedBlock::Kind::Empty
            || (rich && !table && !spot->textual))
            return true;
        // Code, front matter, tables and long text: their source.
        openAndForward(event);
        return true;
    }
    if (typing) {
        if (!typeRendered(text))
            openAndForward(event);
        return true;
    }
    if (erase) {
        const bool forward = key == Qt::Key_Delete;
        if (!ctrl && deleteRendered(forward))
            return true;
        // At the start (end) of the block: joined with the one before
        // (after), in source.
        const int k = spot->offsetOf(c.position());
        if (!ctrl && !c.hasSelection() && !forward && k == 0)
            openAndForward(event, spot->start);
        else if (!ctrl && !c.hasSelection() && forward && k >= spot->text.size())
            openAndForward(event, spot->start + int(spot->source.size()));
        else
            openAndForward(event);
        return true;
    }
    if (enter && !ctrl && !shift && enterRendered())
        return true;
    openAndForward(event);
    return true;
}

void VisualView::keyPressEvent(QKeyEvent *event)
{
    const auto mods = event->modifiers() & ~Qt::KeypadModifier;
    if (event->key() == Qt::Key_Escape && mods == Qt::NoModifier && m_sourceOpen && !isReadOnly()) {
        closeBlocks();
        return;
    }
    if (event->key() != Qt::Key_Up && event->key() != Qt::Key_Down)
        m_verticalX = -1;
    if (renderedKey(event))
        return;
    TextView::keyPressEvent(event);
}

void VisualView::inputMethodEvent(QInputMethodEvent *event)
{
    if (!isReadOnly() && !m_sourceOpen && cursorCount() == 1) {
        const std::optional<Spot> spot = spotAt(textCursor().position());
        const QString commit = event->commitString();
        if (event->preeditString().isEmpty() && event->replacementLength() == 0 && !commit.isEmpty()) {
            if ((spot && spot->editable && typeRendered(commit)) || (!spot && typeOnBlankLine(commit))) {
                event->accept();
                return;
            }
        }
        // Composing (or what rendered text cannot take): in the source.
        if (spot && (!commit.isEmpty() || !event->preeditString().isEmpty()))
            openBlockAtCaret();
    }
    TextView::inputMethodEvent(event);
}

bool VisualView::event(QEvent *event)
{
    // Esc closes an open block before any window shortcut sees it.
    if (event->type() == QEvent::ShortcutOverride && m_sourceOpen && !isReadOnly()) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape && (key->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
            event->accept();
            return true;
        }
    }
    return TextView::event(event);
}

void VisualView::editStarting()
{
    // The lines this edit starts in (whole blocks): their blocks render
    // live (see rebuildRows).
    if (!isVisible())
        return;
    const bool spans = m_model->hasUsableSpans() && !m_model->isAwaitingParse();
    QTextDocument *doc = document();
    int first = INT_MAX;
    int last = -1;
    for (const QTextCursor &c : cursors()) {
        int from = doc->findBlock(c.selectionStart()).blockNumber();
        int to = doc->findBlock(c.selectionEnd()).blockNumber();
        if (spans) {
            if (const int s = m_model->spanAt(from); s >= 0)
                from = m_model->spans()[s].first;
            if (const int s = m_model->spanAt(to); s >= 0)
                to = m_model->spans()[s].last;
        }
        first = qMin(first, from);
        last = qMax(last, to);
    }
    if (last < 0)
        return;
    m_liveStart = QTextCursor(doc->findBlockByNumber(first));
    m_liveStart.setKeepPositionOnInsert(true);
    m_liveEnd = QTextCursor(doc->findBlockByNumber(qMin(last, doc->blockCount() - 1)));
    m_liveEnd.movePosition(QTextCursor::EndOfBlock);
}

bool VisualView::blankAt(const QPointF &point)
{
    if (point.x() < textLeft() || point.x() > textLeft() + textWidth())
        return true;
    qreal top = 0;
    const int index = rowAt(point.y(), &top);
    return index < 0 || (index < int(m_rows.size()) && m_rows[index].span < 0);
}

void VisualView::mousePressEvent(QMouseEvent *event)
{
    // A click on blank space shows everything rendered (the caret goes
    // where the click puts it; nothing opens).
    if (event->button() == Qt::LeftButton && m_sourceOpen && blankAt(event->position())) {
        m_sourceOpen = false;
        m_openLines.clear();
    }
    m_verticalX = -1;
    TextView::mousePressEvent(event);
}

void VisualView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // A double click on a rendered block opens it as source, the caret where
    // the click was (in source, a double click selects a word).
    if (event->button() == Qt::LeftButton && !isReadOnly()) {
        qreal top = 0;
        const int index = rowAt(event->position().y(), &top);
        const Row r = index >= 0 ? row(index) : Row();
        if (index >= 0 && r.rendered && spanForRow(r) >= 0) {
            const QPointF point(event->position().x() - textLeft(), event->position().y() - top);
            const RenderedBlock *block = renderedForRow(r);
            // Links and task boxes act on the first click alone.
            if (block && !block->anchorAt(point).isEmpty())
                return;
            const int position = hitTestRenderedRow(r, point);
            m_pendingCell = {};
            closeCellEditor(TableCellEditor::Exit::Commit);
            m_sourceOpen = true;
            m_openLines.clear();
            QTextCursor at(document());
            at.setPosition(qBound(0, position, document()->characterCount() - 1));
            setTextCursor(at);
            rebuildRows();
            ensureCursorVisible();
            return;
        }
    }
    TextView::mouseDoubleClickEvent(event);
}

} // namespace vn
