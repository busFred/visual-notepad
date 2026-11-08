#include "VisualView.h"

#include "../PlainText.h"
#include "../Settings.h"
#include "../Theme.h"
#include "ImageViewer.h"
#include "MarkdownModel.h"
#include "RemoteImages.h"
#include "TableToolbar.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>

#include <algorithm>
#include <climits>
#include <functional>
#include <string>
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
// takes about 1 ms a KiB for a paragraph. Larger blocks open as source when
// typed into. A long list renders an item at a time (see itemRows): the
// limit is on the item.
constexpr int liveEditChars = 2000;
constexpr int liveEditLines = 40;
// Lists show an item a row from this many items or bytes of HTML on: an
// item renders in about 0.06 ms, a whole list in 0.035 ms an item (16 items:
// 0.55 ms a keystroke), and rows of items lay out only those on screen.
constexpr int itemRowsMinItems = 16;
constexpr int itemRowsMinHtml = 2048;
bool itemRowsEnabled = true; // For tests.

// Pointing at a table's edge: how far the band above (and left of) the table
// reaches out of it and into it, how near a border the quick-insert mark
// shows, and the mark's size and distance from the table.
constexpr qreal tableBandOut = 14;
constexpr qreal tableBandIn = 2;
constexpr qreal insertReach = 6;
constexpr qreal markRadius = 6.5;
constexpr qreal markOffset = 7.5;

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

// Where the line break after a list item's rendered text is in its source
// (the whole list's text has one there): what sourceOffsetFor matches it with.
int itemBreak(const QString &rendered, const QString &source)
{
    const int at = matchOf(rendered + u'\n', source, int(rendered.size()));
    return at >= 0 ? at : int(source.size());
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

// The pointer over a column's top edge (an arrow down) or a row's left edge
// (an arrow right); Qt has no such cursors. Made once for each pixel ratio.
QCursor edgeArrowCursor(bool down, qreal ratio)
{
    static QHash<int, QCursor> cursors;
    const int key = int(ratio * 100) * 2 + (down ? 1 : 0);
    if (const auto found = cursors.constFind(key); found != cursors.cend())
        return *found;
    constexpr int size = 24;
    QPixmap pixmap(QSize(size, size) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainterPath arrow;
    arrow.moveTo(9, 2);
    arrow.lineTo(15, 2);
    arrow.lineTo(15, 12);
    arrow.lineTo(20, 12);
    arrow.lineTo(12, 22);
    arrow.lineTo(4, 12);
    arrow.lineTo(9, 12);
    arrow.closeSubpath();
    if (!down)
        arrow = QTransform(0, 1, 1, 0, 0, 0).map(arrow); // The same, turned to the right.
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Qt::white, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(arrow);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    p.drawPath(arrow);
    p.end();
    return QCursor(pixmap, down ? 12 : 22, down ? 22 : 12);
}

bool startsWith(std::string_view text, size_t at, std::string_view prefix)
{
    return at <= text.size() && text.substr(at, prefix.size()) == prefix;
}

// The number written at `at` (digits), or -1.
int numberAt(std::string_view text, size_t at)
{
    int n = 0;
    size_t i = at;
    for (; i < text.size() && i < at + 9 && text[i] >= '0' && text[i] <= '9'; ++i)
        n = n * 10 + (text[i] - '0');
    return i > at ? n : -1;
}

// Whether an item of a list (the HTML after its <li> and line anchor)
// starts with something Qt puts on the item's first line: text, inline
// markup, a paragraph or heading with some, a one-line code block. Qt drops
// an empty item, starts an item that opens with a list without a marker and
// numbers each line of a code block that opens one (see
// RenderedBlock::isRegularItem): such a list shows whole.
bool startsRegularly(std::string_view s)
{
    for (int steps = 0; steps < 16; ++steps) {
        if (s.empty() || startsWith(s, 0, "</"))
            return false;
        if (s[0] != '<')
            return true;
        if (startsWith(s, 0, "<a name=\"")) {
            const size_t end = s.find("</a>");
            if (end == std::string_view::npos)
                return false;
            s.remove_prefix(end + 4);
            continue;
        }
        if (startsWith(s, 0, "<p>") || startsWith(s, 0, "<p ") || startsWith(s, 0, "<blockquote>")
            || (s.size() > 3 && startsWith(s, 0, "<h") && s[2] >= '1' && s[2] <= '6' && s[3] == '>')) {
            s.remove_prefix(s.find('>') + 1);
            continue;
        }
        if (startsWith(s, 0, "<pre>")) {
            const size_t end = s.find("</pre>");
            return end != std::string_view::npos && end > 5 && s.substr(5, end - 5).find('\n') == std::string_view::npos;
        }
        for (const std::string_view tag : {"<a ", "<strong>", "<em>", "<s>", "<code>", "<img ", "<sup>", "<br", "<hr"}) {
            if (startsWith(s, 0, tag))
                return true;
        }
        return false;
    }
    return false;
}

// Tags the core writes in a list (render.rs), and those of them without an
// end tag. Raw HTML with any other tag, or with these out of balance in an
// item, keeps its list whole.
bool isListTag(std::string_view name)
{
    static constexpr std::string_view names[] = {"ul", "ol", "li", "p", "h1", "h2", "h3", "h4", "h5", "h6",
                                                  "em", "strong", "s", "code", "a", "img", "sup", "br",
                                                  "pre", "blockquote", "hr"};
    return std::find(std::begin(names), std::end(names), name) != std::end(names);
}

bool isVoidTag(std::string_view name)
{
    return name == "br" || name == "img" || name == "hr";
}

} // namespace

// The top-level items of a list's HTML (core/src/render.rs writes
// <ul>/<ol start="N">, then <li> items, each but the first starting with
// the anchor of its line), for showing them one by one.
struct VisualView::ListItems
{
    bool ok = false;
    bool ordered = false;
    int start = 1;
    struct Item
    {
        int from = 0; // <li>...</li> in the HTML.
        int to = 0;
        int line = 0; // First source line, from the block's.
        bool task = false;
    };
    std::vector<Item> items;
    std::vector<int> plainBefore; // Items before item i that are not tasks (one more than items).

    static ListItems split(std::string_view html)
    {
        ListItems result;
        if (!startsWith(html, 0, "<ul") && !startsWith(html, 0, "<ol"))
            return {};
        result.ordered = html[1] == 'o';
        const size_t openEnd = html.find('>');
        if (openEnd == std::string_view::npos)
            return {};
        if (result.ordered) {
            const std::string_view open = html.substr(0, openEnd);
            const size_t at = open.find("start=\"");
            if (at != std::string_view::npos)
                result.start = numberAt(open, at + 7);
            if (result.start < 0)
                return {};
        }
        std::vector<std::string_view> open{html.substr(1, 2)};
        size_t i = openEnd + 1;
        size_t itemFrom = 0;
        while (i < html.size()) {
            const size_t lt = html.find('<', i);
            if (lt == std::string_view::npos)
                return {};
            // Between items: nothing (the core writes no white space there).
            if (open.size() == 1 && lt != i)
                return {};
            size_t j = lt + 1;
            const bool closing = j < html.size() && html[j] == '/';
            if (closing)
                ++j;
            const size_t nameStart = j;
            while (j < html.size() && ((html[j] >= 'a' && html[j] <= 'z') || (html[j] >= '0' && html[j] <= '9')))
                ++j;
            const std::string_view name = html.substr(nameStart, j - nameStart);
            const size_t gt = html.find('>', j);
            if (name.empty() || gt == std::string_view::npos || !isListTag(name))
                return {};
            if (closing) {
                if (open.back() != name)
                    return {};
                open.pop_back();
                if (open.empty()) {
                    // The list's end: the end of the block.
                    if (gt + 1 != html.size() || result.items.empty())
                        return {};
                    result.plainBefore.resize(result.items.size() + 1);
                    for (size_t k = 0; k < result.items.size(); ++k)
                        result.plainBefore[k + 1] = result.plainBefore[k] + !result.items[k].task;
                    result.ok = true;
                    return result;
                }
                if (open.size() == 1) {
                    // An item ends: its line, task box and first content.
                    Item item;
                    item.from = int(itemFrom);
                    item.to = int(gt + 1);
                    std::string_view content = html.substr(itemFrom + 4, gt + 1 - 5 - (itemFrom + 4));
                    static constexpr std::string_view anchor = "<a name=\"vn-line:";
                    if (startsWith(content, 0, anchor)) {
                        item.line = numberAt(content, anchor.size());
                        const size_t end = content.find("</a>");
                        if (item.line <= 0 || end == std::string_view::npos)
                            return {};
                        content.remove_prefix(end + 4);
                    }
                    // Lines rise from item to item (the first is the block's).
                    if (result.items.empty() ? item.line != 0 : item.line <= result.items.back().line)
                        return {};
                    item.task = startsWith(content, 0, "<a class=\"task\"");
                    // (An empty item renders as an empty line with its marker
                    // where Qt would drop it: see RenderedBlock::createItem.)
                    if (!content.empty() && !startsRegularly(content))
                        return {};
                    result.items.push_back(item);
                }
            } else if (!isVoidTag(name) && html[gt - 1] != '/') {
                if (open.size() == 1) {
                    if (!startsWith(html, lt, "<li>"))
                        return {};
                    itemFrom = lt;
                }
                open.push_back(name);
            } else if (open.size() == 1) {
                return {};
            }
            i = gt + 1;
        }
        return {};
    }

    // The HTML of an item alone, as RenderedBlock::createItem takes it: the
    // lines its anchors and task boxes name made relative to its first line
    // (taskBase: the line the HTML's boxes count from, the item's included),
    // its math numbered from 0 (*math gets the TeX of each), so that an item
    // renders the same wherever it is.
    static QString itemHtml(std::string_view html, const Item &item, int taskBase, bool ordered,
                            const std::function<QString(int)> &mathOf, QStringList *math)
    {
        const std::string_view text = html.substr(item.from, item.to - item.from);
        std::string out;
        out.reserve(text.size() + 16);
        out += ordered ? "<ol>" : "<ul>";
        size_t at = 0;
        for (size_t found = text.find("vn-"); found != std::string_view::npos; found = text.find("vn-", found + 3)) {
            static constexpr std::string_view line = "<a name=\"vn-line:";
            static constexpr std::string_view task = "href=\"vn-task:";
            static constexpr std::string_view mathInline = "src=\"vn-math:";
            static constexpr std::string_view mathDisplay = "src=\"vn-math-display:";
            // Which of the four, and where its number is.
            std::string_view prefix;
            for (const std::string_view p : {line, task, mathInline, mathDisplay}) {
                const size_t vn = p.find("vn-");
                if (found >= vn && startsWith(text, found - vn, p)) {
                    prefix = p;
                    break;
                }
            }
            if (prefix.empty())
                continue;
            const size_t tagStart = found - prefix.find("vn-");
            const size_t numberStart = tagStart + prefix.size();
            const int n = numberAt(text, numberStart);
            if (n < 0)
                continue;
            size_t numberEnd = numberStart;
            while (numberEnd < text.size() && text[numberEnd] >= '0' && text[numberEnd] <= '9')
                ++numberEnd;
            if (prefix == line) {
                const int relative = n - item.line;
                if (relative <= 0) {
                    // The item's own first line: none, as in a parse of the item alone.
                    const size_t end = text.find("</a>", numberEnd);
                    if (end == std::string_view::npos)
                        continue;
                    out.append(text.substr(at, tagStart - at));
                    at = end + 4;
                    found = at - 3;
                    continue;
                }
                out.append(text.substr(at, numberStart - at));
                out += std::to_string(relative);
            } else if (prefix == task) {
                out.append(text.substr(at, numberStart - at));
                out += std::to_string(n - taskBase);
            } else {
                out.append(text.substr(at, numberStart - at));
                out += std::to_string(math->size());
                math->append(mathOf(n));
            }
            at = numberEnd;
            found = at - 3;
        }
        out.append(text.substr(at));
        out += ordered ? "</ol>" : "</ul>";
        return QString::fromUtf8(out.data(), qsizetype(out.size()));
    }
};

namespace {

// The marker of a list item line, to compare items: its indentation, and
// its bullet or the delimiter of its number.
struct MarkerKind
{
    bool valid = false;
    int indent = 0;
    QChar marker;
    bool operator==(const MarkerKind &other) const = default;
};

MarkerKind markerKind(const QString &line)
{
    const md::ListLine item = md::parseListLine(line);
    if (!item.valid || item.isQuote || !item.lead.trimmed().isEmpty())
        return {};
    return {true, int(item.lead.size()), item.ordered ? item.delimiter : item.marker.at(0)};
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
    // (The mark of the quick insert is kept in viewport coordinates.)
    connect(this, &TextView::scrolled, this, &VisualView::clearTableHover);
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
        m_editRanges.clear();
        m_editsKnown = true;
        m_editsGeneration = m_model->parseGeneration();
        m_editLines = document()->blockCount();
        relayout();
    });
    // Before the views hear of it (DocumentLayout::changed comes after).
    connect(document(), &QTextDocument::contentsChange, this, &VisualView::noteEdit);
    // Edits made before this view was: only a parse tells which lines they were.
    m_editLines = document()->blockCount();
    m_editsGeneration = model->parseGeneration();
    m_editsKnown = !model->hasPendingEdits();
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
    m_contextValid = false;
    m_itemMemo.clear();
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
    std::erase_if(m_itemMemo, [](const auto &entry) { return entry.second.block->isPending(); });
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

void VisualView::noteEdit(int from, int, int added)
{
    // As MarkdownModel::documentChanged: lines [first, oldLast] became
    // [first, lastNew].
    QTextDocument *doc = document();
    const int count = doc->blockCount();
    const int delta = count - m_editLines;
    m_editLines = count;
    // A parse since (some parse silently): the edits before it are in it.
    if (m_editsGeneration != m_model->parseGeneration()) {
        m_editsGeneration = m_model->parseGeneration();
        m_editRanges.clear();
        m_editsKnown = true;
    }
    if (!m_editsKnown)
        return;
    const int first = qMax(0, doc->findBlock(from).blockNumber());
    const int endPos = qBound(0, from + added, doc->characterCount() - 1);
    const int lastNew = qMax(first, doc->findBlock(endPos).blockNumber());
    const int oldLast = lastNew - delta; // Of the lines before this edit.
    if (oldLast < first) {
        m_editsKnown = false;
        m_editRanges.clear();
        return;
    }
    // The ranges it overlaps (where they are before this edit) become one
    // with it.
    std::vector<EditRange> ranges;
    ranges.reserve(m_editRanges.size() + 1);
    int shift = 0;  // Lines added by the ranges before it.
    int merged = 0; // ... by those it overlaps.
    int unionFirst = first;
    int unionLast = oldLast;
    bool placed = false;
    const auto place = [&] {
        ranges.push_back({unionFirst - shift, unionLast - shift - merged, unionLast + delta - unionFirst + 1});
        placed = true;
    };
    for (const EditRange &r : m_editRanges) {
        const int rangeFirst = r.first + shift + merged;
        const int rangeLast = rangeFirst + r.lines - 1;
        if (rangeLast < first) {
            ranges.push_back(r);
            shift += r.delta();
        } else if (rangeFirst > oldLast) {
            if (!placed)
                place();
            ranges.push_back(r);
        } else {
            unionFirst = qMin(unionFirst, rangeFirst);
            unionLast = qMax(unionLast, rangeLast);
            merged += r.delta();
        }
    }
    if (!placed)
        place();
    m_editRanges = std::move(ranges);
    // Edits all over: the lists edited are parsed again whole.
    if (m_editRanges.size() > 64) {
        m_editsKnown = false;
        m_editRanges.clear();
    }
}

void VisualView::rowsMayHaveChanged()
{
    scheduleToolbarUpdate();
    // An edit ends the selection of cells, and moves what the mark pointed at.
    clearTableHover();
    if (m_tableSel.active && m_tableSel.revision != document()->revision())
        clearTableSelection();
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
    m_tableToolbar->setState(m_tableSel.active ? selectionInfo() : info);
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
    if (m_tableSel.active)
        selectionOperation(op);
    else
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
    if (!hit || !checkTable(r, render).ok)
        return false;
    // Consumed: the caret stays where it is. A click (no drag) opens the box
    // when the button comes up; a drag selects the cells from this one to
    // where the button comes up (see mouseMoveEvent).
    m_pendingCell = {true, r.firstLine, hit->row, hit->col, event->position().toPoint(), point};
    return true;
}

const VisualView::TableCheck &VisualView::checkTable(const Row &r, const Render *render)
{
    const int revision = document()->revision();
    TableCheck &check = m_tableCheck;
    if (check.line == r.firstLine && check.revision == revision && check.block == render->block.get())
        return check;
    check = {};
    check.line = r.firstLine;
    check.revision = revision;
    check.block = render->block.get();
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
    check.ok = info.found && info.firstLine == at && info.lastLine <= lines && (info.html || info.lastLine == lines)
               && cells.height() == info.rows && cells.width() == info.cols;
    check.at = at;
    check.info = info;
    // Not in a table with merged cells, where a row or column is not a row
    // or column of the source.
    check.canInsert = check.ok && md::tableOpAvailable(info, md::TableOp::RowBelow)
                      && md::tableOpAvailable(info, md::TableOp::ColumnRight);
    return check;
}

bool VisualView::openCellEditor(int line, int row, int col)
{
    clearTableSelection();
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
    const TableCheck &check = checkTable(r, render);
    if (!check.ok || row >= check.info.rows || col >= check.info.cols)
        return false;
    const int at = check.at;
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
        m_itemSpans.clear();
        m_itemLists.clear();
        m_itemRows.clear();
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
    m_itemSpans.clear();
    m_itemLists.clear();
    m_itemRows.clear();
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
            if (!open && (span.block >= 0 || span.previous >= 0) && itemRows(int(s), line, last)) {
                // A long list: a row an item.
            } else if (!open && span.block < 0 && line <= liveLast && last >= liveFirst) {
                // Lines edited in rendered text show the blocks a parse of
                // their own source makes, with blank lines between them.
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

const RenderContext &VisualView::renderContext()
{
    const qreal width = textWidth();
    if (!m_contextValid || m_contextFrame != m_frame || m_contextRevision != m_model->revision()
        || m_contextWidth != width || m_context.devicePixelRatio != devicePixelRatioF()) {
        m_context = context();
        m_contextKey = renderKey(m_context, core::BlockKind::Html, QString());
        m_contextFrame = m_frame;
        m_contextRevision = m_model->revision();
        m_contextWidth = width;
        m_contextValid = true;
    }
    return m_context;
}

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

const VisualView::ListItems &VisualView::parsedItems(int blockIndex)
{
    if (m_listItemsGeneration != m_model->parseGeneration()) {
        m_listItems.clear();
        m_irregularLists.clear();
        m_itemMemo.clear();
        m_listItemsGeneration = m_model->parseGeneration();
    }
    std::shared_ptr<const ListItems> &items = m_listItems[blockIndex];
    if (!items) {
        const core::Block &block = m_model->parsedBlock(blockIndex);
        items = std::make_shared<const ListItems>(ListItems::split({block.html.data(), block.html.size()}));
    }
    return *items;
}

void VisualView::irregularItem(int blockIndex)
{
    if (m_listItemsGeneration != m_model->parseGeneration() || m_irregularLists.contains(blockIndex))
        return;
    m_irregularLists.insert(blockIndex);
    // Not under the paint or layout asking for the item: once it is done.
    QTimer::singleShot(0, this, [this] {
        m_rowsStale = true;
        relayout();
    });
}

const VisualView::ItemRow *VisualView::itemOfRow(int index) const
{
    if (index < 0 || index >= int(m_rows.size()) || m_rows[index].item < 0)
        return nullptr;
    return &m_itemRows[m_rows[index].item];
}

int VisualView::neighbourItem(int index, int direction) const
{
    const ItemRow *item = itemOfRow(index);
    const ItemRow *other = itemOfRow(index + direction);
    return item && other && other->list == item->list ? index + direction : -1;
}

void VisualView::setItemRowsEnabled(bool on)
{
    itemRowsEnabled = on;
}

QString VisualView::itemMarkerAt(int line)
{
    const int index = rowOfLine(line);
    const ItemRow *item = itemOfRow(index);
    if (!item || m_rows[index].row.firstLine != line || !m_itemLists[item->list].markers)
        return {};
    const ItemList &list = m_itemLists[item->list];
    return list.ordered ? QString::number(list.start + item->index) + u'.' : QStringLiteral("\u2022");
}

bool VisualView::itemRows(int s, int line, int last)
{
    if (!itemRowsEnabled)
        return false;
    const MarkdownModel::Span &span = m_model->spans()[s];
    const int blockIndex = span.block >= 0 ? span.block : span.previous;
    const core::Block &block = m_model->parsedBlock(blockIndex);
    const std::string_view html(block.html.data(), block.html.size());
    // Quick for the blocks that are not long lists.
    if (block.kind != core::BlockKind::Html || !(startsWith(html, 0, "<ul") || startsWith(html, 0, "<ol"))
        || (int(block.last_line - block.first_line) + 1 < itemRowsMinItems && int(html.size()) < itemRowsMinHtml))
        return false;
    const ListItems &parsed = parsedItems(blockIndex);
    const int n = int(parsed.items.size());
    if (!parsed.ok || n < 2 || (n < itemRowsMinItems && int(html.size()) < itemRowsMinHtml)
        || m_irregularLists.contains(blockIndex))
        return false;

    ItemList list;
    list.ordered = parsed.ordered;
    list.start = parsed.start;
    list.block = blockIndex;
    const int listIndex = int(m_itemLists.size());
    std::vector<ItemRow> items;
    std::vector<int> firsts; // The first line of each item now.
    const int parsedFirst = int(block.first_line);
    const auto addParsed = [&](int k, int shift) {
        const ListItems::Item &item = parsed.items[k];
        items.push_back({listIndex, int(items.size()), item.from, item.to, item.line, parsedFirst + item.line, -1, k});
        firsts.push_back(parsedFirst + item.line + shift);
    };
    int plain = 0;
    if (span.block >= 0) {
        if (parsedFirst != line)
            return false;
        for (int k = 0; k < n; ++k)
            addParsed(k, 0);
        plain = parsed.plainBefore[n];
    } else {
        // Edited since the parse: the runs of items the edits touched are
        // parsed again (each alone, as a list of its own), the others are as
        // parsed, moved by the lines added or taken before them.
        const auto itemEnd = [&](int k) {
            return k + 1 < n ? parsedFirst + parsed.items[k + 1].line - 1 : int(block.last_line);
        };
        const bool known = m_editsKnown && m_editsGeneration == m_model->parseGeneration();
        std::vector<char> dirty(n, !known);
        std::vector<int> shifts(n, 0); // An item's lines now, from the parse's.
        if (known) {
            size_t r = 0;
            int shift = 0;
            for (int k = 0; k < n; ++k) {
                while (r < m_editRanges.size() && m_editRanges[r].last < parsedFirst + parsed.items[k].line)
                    shift += m_editRanges[r++].delta();
                shifts[k] = shift;
                dirty[k] = r < m_editRanges.size() && m_editRanges[r].first <= itemEnd(k);
            }
            // Lines edited next to the list may have joined it.
            if (parsedFirst + shifts[0] != line)
                dirty[0] = true;
            if (itemEnd(n - 1) + shifts[n - 1] != last)
                dirty[n - 1] = true;
        }
        QTextDocument *doc = document();
        const MarkerKind kind = markerKind(doc->findBlockByNumber(line).text());
        if (!kind.valid)
            return false;
        int plainLive = 0;
        for (int k = 0; k < n;) {
            if (!dirty[k]) {
                addParsed(k, shifts[k]);
                ++k;
                continue;
            }
            int end = k;
            while (end + 1 < n && dirty[end + 1])
                ++end;
            const int from = k == 0 ? line : parsedFirst + parsed.items[k].line + shifts[k];
            const int to = end == n - 1 ? last : parsedFirst + parsed.items[end + 1].line + shifts[end + 1] - 1;
            if (from > to || to > last)
                return false;
            // At the list's start, nothing must run into it.
            if (k == 0 && line > 0 && !isBlank(doc->findBlockByNumber(line - 1).text()))
                return false;
            const rust::Vec<core::Block> &blocks = liveBlocks(m_model->sourceText(from, to));
            if (blocks.size() != 1 || blocks[0].first_line != 0)
                return false;
            const core::Block &again = blocks[0];
            const std::string_view liveHtml(again.html.data(), again.html.size());
            const ListItems live = ListItems::split(liveHtml);
            if (!live.ok || (live.ordered != parsed.ordered && !(k == 0 && end == n - 1)))
                return false;
            // They and the item after them must be items of the list: the
            // same bullet (or number delimiter) and indentation.
            for (const ListItems::Item &item : live.items) {
                if (markerKind(doc->findBlockByNumber(from + item.line).text()) != kind)
                    return false;
            }
            if (end < n - 1 && markerKind(doc->findBlockByNumber(to + 1).text()) != kind)
                return false;
            if (k == 0) {
                list.ordered = live.ordered;
                list.start = live.start;
            }
            ItemList::Live group;
            group.html = QByteArray(liveHtml.data(), qsizetype(liveHtml.size()));
            for (const auto &tex : again.inline_math)
                group.math.push_back(QString::fromUtf8(tex.data(), qsizetype(tex.size())));
            for (const ListItems::Item &item : live.items) {
                items.push_back({listIndex, int(items.size()), item.from, item.to, item.line, item.line,
                                 int(list.live.size()), -1});
                firsts.push_back(from + item.line);
            }
            list.live.push_back(std::move(group));
            plainLive += live.plainBefore.back();
            k = end + 1;
        }
        for (int k = 0; k < n; ++k)
            plain += !dirty[k] && !parsed.items[k].task;
        plain += plainLive;
    }
    // The items' lines cover the span's, in order.
    if (firsts.front() != line)
        return false;
    for (size_t k = 1; k < firsts.size(); ++k) {
        if (firsts[k] <= firsts[k - 1] || firsts[k] > last)
            return false;
    }
    list.markers = plain > 0;
    for (size_t k = 0; k < items.size(); ++k) {
        const int from = firsts[k];
        const int to = k + 1 < firsts.size() ? firsts[k + 1] - 1 : last;
        for (int l = from; l <= to; ++l)
            m_lineToRow[l] = int(m_rows.size());
        m_rows.push_back({{from, to, true}, s, int(m_itemRows.size())});
        m_itemRows.push_back(items[k]);
    }
    m_itemLists.push_back(std::move(list));
    m_itemSpans.insert(s);
    return true;
}

VisualView::Render VisualView::itemRender(const ItemRow &item, const QString &source, const RenderContext &ctx)
{
    const ItemList &list = m_itemLists[item.list];
    const ListItems::Item range{item.from, item.to, item.line, false};
    QStringList math;
    QString html;
    if (item.live >= 0) {
        const ItemList::Live &live = list.live[item.live];
        html = ListItems::itemHtml({live.html.constData(), size_t(live.html.size())}, range, item.taskBase, list.ordered,
                                   [&](int i) { return i < int(live.math.size()) ? live.math[i] : QString(); }, &math);
    } else {
        const core::Block &block = m_model->parsedBlock(list.block);
        html = ListItems::itemHtml({block.html.data(), block.html.size()}, range, item.taskBase, list.ordered,
                                   [&](int i) {
                                       return i < int(block.inline_math.size())
                                                  ? QString::fromUtf8(block.inline_math[i].data(),
                                                                      qsizetype(block.inline_math[i].size()))
                                                  : QString();
                                   },
                                   &math);
    }
    return itemRender(html, math, source, ctx);
}

VisualView::Render VisualView::itemRender(const QString &html, const QStringList &math, const QString &source,
                                          const RenderContext &ctx)
{
    // Keyed by both: an item's rendering is its HTML's (which holds what
    // the parse of the whole document resolved), its mapping to the text
    // the source's.
    QString made = html;
    for (const QString &tex : math)
        made += u'\x1f' + tex;
    const QString key = renderKey(ctx, core::BlockKind::Html, source)
                        + QStringLiteral("|item:%1:%2").arg(made.size()).arg(qHash(made));
    const Render *hit = m_cache.object(key);
    if (hit && hit->source == source && hit->html == made)
        return *hit;
    core::Block block{};
    block.kind = core::BlockKind::Html;
    const QByteArray utf8 = html.toUtf8();
    block.html = rust::String(utf8.constData(), size_t(utf8.size()));
    for (const QString &tex : math) {
        const QByteArray bytes = tex.toUtf8();
        block.inline_math.push_back(rust::String(bytes.constData(), size_t(bytes.size())));
    }
    const std::shared_ptr<RenderedBlock> created(RenderedBlock::createItem(block, source, ctx));
    const int cost = created->cost() + int((source.size() + made.size()) / 512);
    ++m_renderCount;
    Render memo{source, key, created, core::BlockKind::Html, cost, true, 0, made};
    if (created->isPending())
        m_pendingKeys.insert(key);
    m_cache.insert(key, new Render(memo), cost);
    return memo;
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
    const ItemRow *item = itemOfRow(rowOfLine(r.firstLine));
    int liveFirst = -1;
    int liveLast = -2;
    const bool live = !item && span.block < 0 && liveLines(&liveFirst, &liveLast) && span.first <= liveLast
                      && span.last >= liveFirst && !m_partSpans.contains(index);
    if (blockIndex < 0 && !live && !item)
        return nullptr;

    const qreal width = textWidth();
    const qreal dpr = devicePixelRatioF();
    if (m_rendersRevision != m_model->revision() || m_rendersWidth != width || m_rendersDpr != dpr) {
        resetRenders();
        m_rendersRevision = m_model->revision();
        m_rendersWidth = width;
        m_rendersDpr = dpr;
    }
    if (const auto it = m_renders.find(r.firstLine); it != m_renders.end()) {
        it->second.used = m_frame;
        return &it->second;
    }

    const RenderContext &ctx = renderContext();
    Render memo;
    if (item) {
        const int listBlock = m_itemLists[item->list].block;
        if (item->parsed >= 0) {
            // An item of the parse not edited since: the same whatever the
            // edits elsewhere (the cache's key would take reading its
            // source and HTML again at every keystroke).
            parsedItems(listBlock); // (A new parse clears the memo.)
            if (m_itemMemoKey != m_contextKey) {
                m_itemMemo.clear();
                m_itemMemoKey = m_contextKey;
            }
            const quint64 id = (quint64(quint32(listBlock)) << 32) | quint32(item->parsed);
            if (const auto it = m_itemMemo.find(id); it != m_itemMemo.end()) {
                memo = it->second;
            } else {
                memo = itemRender(*item, m_model->sourceText(r.firstLine, r.lastLine), ctx);
                if (m_itemMemo.size() >= size_t(renderMax / 2))
                    m_itemMemo.clear(); // (The cache keeps them.)
                m_itemMemo.emplace(id, memo);
            }
        } else {
            memo = itemRender(*item, m_model->sourceText(r.firstLine, r.lastLine), ctx);
        }
        if (memo.block->kind() == RenderedBlock::Kind::Rich && !memo.block->isRegularItem())
            irregularItem(listBlock);
    } else if (live) {
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
    const int index = rowOfLine(r.firstLine);
    const int next = itemOfRow(index) ? neighbourItem(index, 1) : -1;
    if (m_unpainted) {
        const Render *render = memoized(r);
        const Render *after = next >= 0 ? memoized(m_rows[next].row) : nullptr;
        if (render && (!itemOfRow(index) || next < 0 || after))
            return render->block->itemAdvance(after ? after->block.get() : nullptr);
        m_estimated = true;
        return lineHeight() * (r.lastLine - r.firstLine + 1);
    }
    const Render *render = rendered(r);
    if (!render)
        return lineHeight();
    // A list item reaches down to the next one (whose margin counts).
    const std::shared_ptr<RenderedBlock> block = render->block;
    std::shared_ptr<RenderedBlock> after;
    if (next >= 0) {
        if (const Render *nextRender = rendered(m_rows[next].row))
            after = nextRender->block;
    }
    return block->itemAdvance(after.get());
}

void VisualView::paintRenderedRow(QPainter &painter, const Row &r, const QRectF &rect)
{
    const Render *render = rendered(r);
    if (!render)
        return;
    const RenderedBlock *block = render->block.get();
    const RenderedSelection selection = selectionForRow(r, block, render->source);
    block->paint(painter, rect.topLeft(), rect.width(), selection);
    if (const ItemRow *item = itemOfRow(rowOfLine(r.firstLine))) {
        const ItemList &list = m_itemLists[item->list];
        if (list.markers)
            block->paintItemMarker(painter, rect.topLeft(), list.ordered, list.start + item->index, selection.markerSelected);
    }
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
    paintTableOverlay(event);
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
            const int index = rowOfLine(r.firstLine);
            if (!itemOfRow(index))
                break;
            // An item of a long list: what the whole list would show of the
            // line break between items (see RenderedSelection).
            const int next = neighbourItem(index, 1);
            if (next >= 0 && b > rowStart) {
                const int breakAt = rowStart + itemBreak(text, source);
                if (a <= breakAt && b > breakAt)
                    result.runsOn = true;
            }
            const int previous = neighbourItem(index, -1);
            if (previous >= 0 && a < rowStart && to >= 1) {
                // From before the previous item's line break.
                const Row before = m_rows[previous].row;
                const Render *render = memoized(before);
                const int start = doc->findBlockByNumber(before.firstLine).position();
                result.markerSelected
                    = !render || a <= start + itemBreak(render->block->renderedText(), render->source);
            }
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
    if (r.rendered && index < int(m_rows.size())
        && (m_partSpans.contains(m_rows[index].span) || m_itemSpans.contains(m_rows[index].span))) {
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
    if (m_tableDrag.active && event->button() == Qt::LeftButton) {
        // The selection of cells, rows or columns is made as it was dragged.
        m_tableDrag = {};
        return;
    }
    const bool wasSelecting = isMouseSelecting();
    const PendingCell pending = std::exchange(m_pendingCell, PendingCell());
    TextView::mouseReleaseEvent(event);
    if (pending.active && event->button() == Qt::LeftButton && !isReadOnly()
        && (event->position().toPoint() - pending.pos).manhattanLength() <= QApplication::startDragDistance()) {
        // A click on a cell of a rendered table: its box, not the table's
        // source.
        if (openCellEditor(pending.line, pending.row, pending.col))
            return;
        // No box for this table: the click is a click on its text.
        const int index = rowOfLine(pending.line);
        if (index >= 0 && index < rowCount() && row(index).rendered) {
            QTextCursor cursor(document());
            cursor.setPosition(hitTestRenderedRow(row(index), pending.point));
            setTextCursor(cursor);
        }
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
        // A list item's boxes count from its first line.
        const int line = anchor.section(u':', 1).toInt();
        toggleTask(itemOfRow(rowOfLine(r.firstLine)) ? r.firstLine + line : line);
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
    if (m_tableDrag.active) {
        if (event->buttons() & Qt::LeftButton) {
            updateTableDrag(event->position());
            return;
        }
        m_tableDrag = {}; // (The release went elsewhere.)
    }
    if (m_pendingCell.active && (event->buttons() & Qt::LeftButton)
        && (event->position().toPoint() - m_pendingCell.pos).manhattanLength() > QApplication::startDragDistance()) {
        // A drag from a cell: the cells from it to the pointer are selected.
        const PendingCell pending = std::exchange(m_pendingCell, PendingCell());
        const int index = rowOfLine(pending.line);
        if (index >= 0 && index < rowCount() && row(index).rendered) {
            const Row r = row(index);
            const Render *render = memoized(r);
            if (render && render->block && render->block->tableGrid()) {
                const TableCheck &check = checkTable(r, render);
                TableSpot spot;
                spot.line = r.firstLine;
                spot.at = check.at;
                spot.rows = check.info.rows;
                spot.cols = check.info.cols;
                if (check.ok && !m_cellEditor) {
                    selectTableCells(SelectionMode::Cells, spot, pending.row, pending.col, false);
                    m_tableDrag.active = true;
                    updateTableDrag(event->position());
                    return;
                }
            }
        }
    }
    if (event->buttons() == Qt::NoButton) {
        // The edges of a table: arrows to select a column or a row, and the
        // marks to insert one.
        const TableSpot spot = tableSpotAt(event->position());
        setTableHover(spot);
        if (spot.zone != TableZone::None && spot.zone != TableZone::Cell) {
            if (spot.zone == TableZone::Column || spot.zone == TableZone::Row) {
                const bool down = spot.zone == TableZone::Column;
                const qreal ratio = viewport()->devicePixelRatioF();
                const int key = int(ratio * 100) * 2 + (down ? 1 : 0);
                auto cursor = m_edgeCursors.find(key);
                if (cursor == m_edgeCursors.end())
                    cursor = m_edgeCursors.insert(key, edgeArrowCursor(down, ratio));
                viewport()->setCursor(*cursor);
            } else {
                viewport()->setCursor(Qt::PointingHandCursor);
            }
            QToolTip::hideText();
            return;
        }
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

bool VisualView::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::Leave)
        clearTableHover();
    return TextView::viewportEvent(event);
}

// ---------------------------------------------------------------------------
// Selecting cells, rows and columns; quick insert

VisualView::TableSpot VisualView::tableSpotAt(const QPointF &p)
{
    if (isReadOnly() || !markdownEditing())
        return {};
    // The row under the pointer, then the one that starts just below it (the
    // band above a table is in the space before it).
    const auto inRow = [&](int index, qreal top) -> TableSpot {
        if (index < 0)
            return {};
        const Row r = row(index);
        if (!r.rendered)
            return {};
        const Render *render = memoized(r);
        if (!render || !render->block)
            return {};
        const RenderedBlock::TableGrid *grid = render->block->tableGrid();
        if (!grid)
            return {};
        const TableCheck &check = checkTable(r, render);
        if (!check.ok)
            return {};
        TableSpot spot;
        spot.line = r.firstLine;
        spot.at = check.at;
        spot.rows = grid->rows();
        spot.cols = grid->cols();
        spot.top = top;
        spot.grid = grid;
        const qreal lx = p.x() - textLeft();
        const qreal ly = p.y() - top;
        const qreal x0 = grid->xs.front();
        const qreal x1 = grid->xs.back();
        const qreal y0 = grid->ys.front();
        const qreal y1 = grid->ys.back();
        // The border nearest to a coordinate, no nearer the start than
        // `first`, and whether it is near enough (and its cells wide enough)
        // for the mark.
        const auto border = [](const std::vector<qreal> &edges, qreal v, int first, int *index) {
            const int count = int(edges.size());
            int k = int(std::lower_bound(edges.begin() + first, edges.end(), v) - edges.begin());
            if (k >= count || (k > first && v - edges[k - 1] < edges[k] - v))
                --k;
            *index = k;
            qreal room = insertReach;
            if (k > 0)
                room = qMin(room, (edges[k] - edges[k - 1]) / 3);
            if (k + 1 < count)
                room = qMin(room, (edges[k + 1] - edges[k]) / 3);
            return qAbs(v - edges[k]) <= room;
        };
        if (ly >= y0 - tableBandOut && ly < y0 + tableBandIn && lx >= x0 - insertReach && lx <= x1 + insertReach) {
            int k = 0;
            if (check.canInsert && border(grid->xs, lx, 0, &k)) {
                spot.zone = TableZone::InsertColumn;
                spot.index = k;
                return spot;
            }
            if (lx >= x0 && lx < x1) {
                spot.zone = TableZone::Column;
                spot.index = grid->colAt(lx);
                return spot;
            }
        }
        if (lx >= x0 - tableBandOut && lx < x0 + tableBandIn && ly >= y0 - insertReach && ly <= y1 + insertReach) {
            int k = 0;
            if (check.canInsert && border(grid->ys, ly, 1, &k)) {
                spot.zone = TableZone::InsertRow;
                spot.index = k;
                return spot;
            }
            if (ly >= y0 && ly < y1) {
                spot.zone = TableZone::Row;
                spot.index = grid->rowAt(ly);
                return spot;
            }
        }
        if (lx >= x0 && lx < x1 && ly >= y0 && ly < y1) {
            spot.zone = TableZone::Cell;
            spot.row = grid->rowAt(ly);
            spot.col = grid->colAt(lx);
        }
        return spot;
    };
    qreal top = 0;
    const int index = rowAt(p.y(), &top);
    TableSpot spot = inRow(index, top);
    if (spot.zone != TableZone::None)
        return spot;
    qreal below = 0;
    const int next = rowAt(p.y() + tableBandOut, &below);
    if (next < 0 || next == index)
        return {};
    return inRow(next, below);
}

void VisualView::setTableHover(const TableSpot &spot)
{
    const bool insert = spot.zone == TableZone::InsertColumn || spot.zone == TableZone::InsertRow;
    if (!insert) {
        clearTableHover();
        return;
    }
    if (m_tableHover.zone == spot.zone && m_tableHover.line == spot.line && m_tableHover.index == spot.index)
        return;
    const QRect old = m_tableHover.area;
    const bool column = spot.zone == TableZone::InsertColumn;
    const RenderedBlock::TableGrid &g = *spot.grid;
    const qreal left = textLeft();
    TableHover hover;
    hover.zone = spot.zone;
    hover.line = spot.line;
    hover.index = spot.index;
    if (column) {
        const qreal x = left + g.xs[spot.index];
        hover.center = QPointF(x, spot.top + g.ys.front() - markOffset);
        hover.from = QPointF(x, spot.top + g.ys.front() - markOffset);
        hover.to = QPointF(x, spot.top + g.ys.back());
    } else {
        const qreal y = spot.top + g.ys[spot.index];
        hover.center = QPointF(left + g.xs.front() - markOffset, y);
        hover.from = QPointF(left + g.xs.front() - markOffset, y);
        hover.to = QPointF(left + g.xs.back(), y);
    }
    hover.area = QRectF(hover.from, hover.to).normalized().adjusted(-markRadius - 2, -markRadius - 2, markRadius + 2, markRadius + 2).toAlignedRect();
    m_tableHover = hover;
    if (!old.isNull())
        viewport()->update(old);
    viewport()->update(hover.area);
}

void VisualView::clearTableHover()
{
    if (m_tableHover.zone == TableZone::None)
        return;
    const QRect area = m_tableHover.area;
    m_tableHover = {};
    viewport()->update(area);
}

void VisualView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !isReadOnly() && markdownEditing()) {
        TableSpot spot = tableSpotAt(event->position());
        const bool shift = (event->modifiers() & ~Qt::KeypadModifier) == Qt::ShiftModifier;
        const bool onBand = spot.zone != TableZone::None && spot.zone != TableZone::Cell;
        if (onBand && m_cellEditor) {
            // What was typed goes in first, and may change the table's widths.
            closeCellEditor(TableCellEditor::Exit::Commit);
            spot = tableSpotAt(event->position());
        }
        if (spot.zone != TableZone::None && spot.zone != TableZone::Cell) {
            setFocus(Qt::MouseFocusReason);
            event->accept();
            tablePressed(spot, event);
            return;
        }
        if (m_tableSel.active) {
            if (shift && spot.zone == TableZone::Cell && spot.line == m_tableSel.line && tableSelectionValid()) {
                // Shift+click on a cell grows the selection to it.
                setFocus(Qt::MouseFocusReason);
                event->accept();
                selectTableCells(m_tableSel.mode, spot, spot.row, spot.col, true);
                return;
            }
            clearTableSelection();
        }
    }
    typingMousePress(event);
}

void VisualView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // A quick second click on an edge is another click on it, not a word.
    if (event->button() == Qt::LeftButton && !isReadOnly() && markdownEditing()) {
        const TableSpot spot = tableSpotAt(event->position());
        if (spot.zone != TableZone::None && spot.zone != TableZone::Cell) {
            mousePressEvent(event);
            return;
        }
    }
    typingMouseDoubleClick(event);
}

void VisualView::tablePressed(const TableSpot &spot, QMouseEvent *event)
{
    const bool extend = (event->modifiers() & ~Qt::KeypadModifier) == Qt::ShiftModifier;
    switch (spot.zone) {
    case TableZone::InsertColumn:
    case TableZone::InsertRow:
        clearTableSelection();
        insertAtBorder(spot);
        break;
    case TableZone::Column:
        selectTableCells(SelectionMode::Columns, spot, 0, spot.index, extend);
        m_tableDrag.active = true;
        break;
    case TableZone::Row:
        selectTableCells(SelectionMode::Rows, spot, spot.index, 0, extend);
        m_tableDrag.active = true;
        break;
    default:
        break;
    }
}

void VisualView::insertAtBorder(const TableSpot &spot)
{
    clearTableHover();
    const int line = spot.line + spot.at;
    using Op = md::TableOp;
    bool done = false;
    if (spot.zone == TableZone::InsertRow)
        done = editTableAt(line, Op::RowBelow, spot.index - 1, 0, {}, false);
    else if (spot.index < spot.cols)
        done = editTableAt(line, Op::ColumnLeft, 0, spot.index, {}, false);
    else
        done = editTableAt(line, Op::ColumnRight, 0, spot.cols - 1, {}, false);
    if (done)
        m_model->flush(); // Show it now.
    scheduleToolbarUpdate();
}

void VisualView::selectTableCells(SelectionMode mode, const TableSpot &spot, int row, int col, bool extend)
{
    TableSelection s;
    if (extend && m_tableSel.active && m_tableSel.line == spot.line && m_tableSel.mode == mode) {
        s = m_tableSel;
    } else {
        s.active = true;
        s.mode = mode;
        s.line = spot.line;
        s.at = spot.at;
        s.rows = spot.rows;
        s.cols = spot.cols;
        s.anchorRow = row;
        s.anchorCol = col;
    }
    s.row = row;
    s.col = col;
    s.revision = document()->revision();
    setTableSelection(s);
}

void VisualView::updateTableDrag(const QPointF &p)
{
    if (!m_tableSel.active)
        return;
    const int index = rowOfLine(m_tableSel.line);
    if (index < 0 || index >= rowCount())
        return;
    const Row r = row(index);
    qreal top = 0;
    qreal bottom = 0;
    const Render *render = r.rendered ? memoized(r) : nullptr;
    const RenderedBlock::TableGrid *grid = render && render->block ? render->block->tableGrid() : nullptr;
    if (!grid || !lineBounds(m_tableSel.line, &top, &bottom))
        return;
    // (The table's edges clamp: a pointer past the table selects up to its end.)
    TableSelection s = m_tableSel;
    const int col = grid->colAt(p.x() - textLeft());
    const int row = grid->rowAt(p.y() - top);
    if (s.mode != SelectionMode::Rows)
        s.col = col;
    if (s.mode != SelectionMode::Columns)
        s.row = row;
    if (s == m_tableSel)
        return;
    setTableSelection(s);
}

bool VisualView::tableSelectionValid()
{
    if (!m_tableSel.active)
        return false;
    if (m_tableSel.revision != document()->revision()) {
        clearTableSelection();
        return false;
    }
    return true;
}

bool VisualView::selectionViewRect(QRectF *rect)
{
    const TableSelection &s = m_tableSel;
    if (!s.active)
        return false;
    const int index = rowOfLine(s.line);
    if (index < 0 || index >= rowCount())
        return false;
    const Row r = row(index);
    if (!r.rendered || r.firstLine != s.line)
        return false;
    const Render *render = memoized(r);
    const RenderedBlock::TableGrid *grid = render && render->block ? render->block->tableGrid() : nullptr;
    qreal top = 0;
    qreal bottom = 0;
    if (!grid || grid->rows() != s.rows || grid->cols() != s.cols || !lineBounds(s.line, &top, &bottom))
        return false;
    const qreal left = textLeft();
    *rect = QRectF(QPointF(left + grid->xs[s.left()], top + grid->ys[s.top()]),
                   QPointF(left + grid->xs[s.right() + 1], top + grid->ys[s.bottom() + 1]));
    return true;
}

void VisualView::setTableSelection(const TableSelection &selection)
{
    QRectF before;
    QRectF after;
    const bool hadBefore = selectionViewRect(&before);
    m_tableSel = selection;
    const bool hasAfter = selectionViewRect(&after);
    // Only what changes is painted again.
    if (hadBefore && hasAfter)
        viewport()->update(before.united(after).adjusted(-3, -3, 3, 3).toAlignedRect());
    else
        viewport()->update();
    // The table commands (the toolbar, the Table menu) act on the first cell.
    setTableTarget(selection.line + selection.at, selection.top(), selection.left());
    scheduleToolbarUpdate();
}

void VisualView::clearTableSelection()
{
    if (!m_tableSel.active)
        return;
    QRectF rect;
    const bool shown = selectionViewRect(&rect);
    m_tableSel = {};
    m_tableDrag = {};
    if (!m_cellEditor)
        setTableTarget(-1);
    if (shown)
        viewport()->update(rect.adjusted(-3, -3, 3, 3).toAlignedRect());
    else
        viewport()->update();
    scheduleToolbarUpdate();
}

md::TableInfo VisualView::selectionInfo()
{
    md::TableInfo info = tableAtCaret();
    if (!m_tableSel.active || !info.found)
        return info;
    const TableSelection &s = m_tableSel;
    const int line = s.line + s.at;
    using Op = md::TableOp;
    // Whether a command is accepted at a cell of the table.
    const auto allowed = [&](Op op, int row, int col) {
        const md::TableCell cell = tableCellOf(line, row, col);
        return cell.found && md::tableOpAvailable(tableInfoAt(cell.line, cell.start), op);
    };
    const bool allColumns = s.right() - s.left() + 1 >= s.cols;
    md::setTableOpAvailable(info, Op::RowAbove, allowed(Op::RowAbove, s.top(), s.left()));
    md::setTableOpAvailable(info, Op::RowBelow, allowed(Op::RowBelow, s.bottom(), s.left()));
    md::setTableOpAvailable(info, Op::ColumnLeft, allowed(Op::ColumnLeft, s.top(), s.left()));
    md::setTableOpAvailable(info, Op::ColumnRight, allowed(Op::ColumnRight, s.top(), s.right()));
    // (Deleting rows is about the last one: the header cannot be.)
    md::setTableOpAvailable(info, Op::DeleteRow, allowed(Op::DeleteRow, s.bottom(), s.left()));
    md::setTableOpAvailable(info, Op::DeleteColumn, !allColumns && allowed(Op::DeleteColumn, s.top(), s.left()));
    md::setTableOpAvailable(info, Op::SetCell, allowed(Op::SetCell, s.top(), s.left()));
    return info;
}

bool VisualView::selectionOperation(md::TableOp op)
{
    if (!tableSelectionValid())
        return false;
    const TableSelection s = m_tableSel;
    const int line = s.line + s.at;
    const int rows = s.bottom() - s.top() + 1;
    const int cols = s.right() - s.left() + 1;
    using Op = md::TableOp;
    bool done = false;
    switch (op) {
    case Op::RowAbove:
    case Op::ColumnLeft:
    case Op::DeleteRow:
    case Op::DeleteColumn:
    case Op::AlignNone:
    case Op::AlignLeft:
    case Op::AlignCenter:
    case Op::AlignRight:
    case Op::SetCell:
        done = editTableAt(line, op, s.top(), s.left(), {}, false, rows, cols);
        break;
    case Op::RowBelow:
        done = editTableAt(line, op, s.bottom(), s.left(), {}, false, rows, cols);
        break;
    case Op::ColumnRight:
        done = editTableAt(line, op, s.top(), s.right(), {}, false, rows, cols);
        break;
    default:
        done = editTable(op); // On the first cell.
        break;
    }
    if (done)
        m_model->flush(); // Show the new table now.
    clearTableSelection();
    scheduleToolbarUpdate();
    return done;
}

void VisualView::populateSelectionMenu(QMenu *menu)
{
    if (!m_tableSel.active)
        return;
    using Op = md::TableOp;
    const md::TableInfo info = selectionInfo();
    const int rows = m_tableSel.bottom() - m_tableSel.top() + 1;
    const int cols = m_tableSel.right() - m_tableSel.left() + 1;
    const auto add = [&](QMenu *to, const QString &title, Op op) {
        QAction *action = to->addAction(title, this, [this, op] { selectionOperation(op); });
        action->setEnabled(md::tableOpAvailable(info, op));
        return action;
    };
    add(menu, rows == 1 ? tr("Insert Row Above") : tr("Insert %1 Rows Above").arg(rows), Op::RowAbove);
    add(menu, rows == 1 ? tr("Insert Row Below") : tr("Insert %1 Rows Below").arg(rows), Op::RowBelow);
    add(menu, rows == 1 ? tr("Delete Row") : tr("Delete %1 Rows").arg(rows), Op::DeleteRow);
    menu->addSeparator();
    add(menu, cols == 1 ? tr("Insert Column Left") : tr("Insert %1 Columns Left").arg(cols), Op::ColumnLeft);
    add(menu, cols == 1 ? tr("Insert Column Right") : tr("Insert %1 Columns Right").arg(cols), Op::ColumnRight);
    add(menu, cols == 1 ? tr("Delete Column") : tr("Delete %1 Columns").arg(cols), Op::DeleteColumn);
    menu->addSeparator();
    QMenu *align = menu->addMenu(cols == 1 ? tr("Align Column") : tr("Align Columns"));
    align->setEnabled(md::tableOpAvailable(info, Op::AlignLeft));
    const struct
    {
        const char *title;
        Op op;
        md::TableAlignment alignment;
    } alignments[] = {{QT_TR_NOOP("Left"), Op::AlignLeft, md::TableAlignment::Left},
                      {QT_TR_NOOP("Center"), Op::AlignCenter, md::TableAlignment::Center},
                      {QT_TR_NOOP("Right"), Op::AlignRight, md::TableAlignment::Right},
                      {QT_TR_NOOP("None"), Op::AlignNone, md::TableAlignment::None}};
    for (const auto &a : alignments) {
        QAction *action = add(align, tr(a.title), a.op);
        action->setCheckable(true);
        action->setChecked(info.alignment == a.alignment);
    }
    menu->addSeparator();
    add(menu, tr("Clear Cells"), Op::SetCell);
    menu->addSeparator();
    populateTableMenu(menu->addMenu(tr("Table")));
}

void VisualView::contextMenuEvent(QContextMenuEvent *event)
{
    if (m_tableSel.active && tableSelectionValid()) {
        QRectF rect;
        if (selectionViewRect(&rect) && rect.contains(event->pos())) {
            QMenu menu(this);
            populateSelectionMenu(&menu);
            menu.exec(event->globalPos());
            return;
        }
        clearTableSelection();
    }
    TextView::contextMenuEvent(event);
}

void VisualView::keyPressEvent(QKeyEvent *event)
{
    if (m_tableSel.active) {
        const auto mods = event->modifiers() & ~Qt::KeypadModifier;
        switch (event->key()) {
        case Qt::Key_Escape:
            clearTableSelection();
            event->accept();
            return;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (mods == Qt::NoModifier) {
                selectionOperation(md::TableOp::SetCell);
                event->accept();
                return;
            }
            break;
        case Qt::Key_Shift:
        case Qt::Key_Control:
        case Qt::Key_Alt:
        case Qt::Key_AltGr:
        case Qt::Key_Meta:
        case Qt::Key_CapsLock:
            break;
        default:
            // Typing or moving the caret leaves the selection (a shortcut,
            // Ctrl+S, does not).
            if (!(mods & Qt::ControlModifier))
                clearTableSelection();
            break;
        }
    }
    typingKeyPress(event);
}

QRect VisualView::tableCellViewRect(int line, int row, int col)
{
    const int index = rowOfLine(line);
    if (index < 0 || index >= rowCount())
        return {};
    const Row r = this->row(index);
    const Render *render = r.rendered && r.firstLine == line ? memoized(r) : nullptr;
    qreal top = 0;
    qreal bottom = 0;
    if (!render || !render->block || !lineBounds(line, &top, &bottom))
        return {};
    const auto rect = render->block->tableCellRect(row, col);
    if (!rect)
        return {};
    return rect->translated(textLeft(), top).toAlignedRect();
}

void VisualView::paintTableOverlay(QPaintEvent *event)
{
    QRectF selected;
    const bool shade = m_tableSel.active && m_tableSel.revision == document()->revision() && selectionViewRect(&selected)
                       && selected.intersects(event->rect());
    const bool mark = m_tableHover.zone != TableZone::None && m_tableHover.area.intersects(event->rect());
    if (!shade && !mark)
        return;
    QPainter painter(viewport());
    painter.setClipRegion(event->region());
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor accent = theme::color(theme::Role::accent);
    if (shade) {
        QColor tint = theme::color(theme::Role::selection);
        tint.setAlpha(110);
        painter.fillRect(selected, tint);
        painter.setPen(QPen(accent, 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(selected.adjusted(0.75, 0.75, -0.75, -0.75));
    }
    if (mark) {
        painter.setPen(QPen(accent, 2));
        painter.drawLine(m_tableHover.from, m_tableHover.to);
        painter.setPen(Qt::NoPen);
        painter.setBrush(accent);
        painter.drawEllipse(m_tableHover.center, markRadius, markRadius);
        painter.setPen(QPen(theme::color(theme::Role::editor), 1.8, Qt::SolidLine, Qt::RoundCap));
        const QPointF c = m_tableHover.center;
        painter.drawLine(c + QPointF(-3, 0), c + QPointF(3, 0));
        painter.drawLine(c + QPointF(0, -3), c + QPointF(0, 3));
    }
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
    spot.lastIndex = index;
    spot.row = r;
    spot.start = document()->findBlockByNumber(r.firstLine).position();
    spot.source = render->source;
    spot.block = render->block;
    if (spot.block->kind() == RenderedBlock::Kind::Rich) {
        spot.text = spot.block->renderedText();
        spot.textual = render->textual && spot.block->tableCells().isEmpty();
        // (A long list's item is limited alone, as it renders alone.)
        spot.items = itemOfRow(index) != nullptr;
        spot.editable = spot.textual && spot.source.size() <= liveEditChars
                        && r.lastLine - r.firstLine < liveEditLines;
    }
    return spot;
}

std::optional<VisualView::Spot> VisualView::spotOverItems(int first, int last)
{
    const ItemRow *a = itemOfRow(first);
    const ItemRow *b = itemOfRow(last);
    if (!a || !b || a->list != b->list || first > last)
        return std::nullopt;
    Spot spot;
    spot.index = first;
    spot.lastIndex = last;
    spot.row = {m_rows[first].row.firstLine, m_rows[last].row.lastLine, true};
    spot.start = document()->findBlockByNumber(spot.row.firstLine).position();
    spot.source = m_model->sourceText(spot.row.firstLine, spot.row.lastLine);
    for (int i = first; i <= last; ++i) {
        const Render *render = rendered(m_rows[i].row);
        if (!render || render->block->kind() != RenderedBlock::Kind::Rich)
            return std::nullopt;
        // As the whole list's text: a line break between items.
        if (i > first)
            spot.text += u'\n';
        spot.text += render->block->renderedText();
        if (i == first)
            spot.block = render->block;
    }
    // Its cost is that of the items it covers (an edit of a selection).
    spot.textual = true;
    spot.items = true;
    spot.editable = true;
    return spot;
}

std::optional<VisualView::Spot> VisualView::spotFor(const QTextCursor &cursor)
{
    if (cursor.hasSelection()) {
        QTextDocument *doc = document();
        const int first = rowOfLine(doc->findBlock(cursor.selectionStart()).blockNumber());
        const int last = rowOfLine(doc->findBlock(cursor.selectionEnd()).blockNumber());
        if (first != last) {
            if (std::optional<Spot> spot = spotOverItems(first, last))
                return spot;
        }
    }
    return spotAt(cursor.position());
}

QString VisualView::itemsText(const core::Block &block, const QString &source, const RenderContext &ctx)
{
    const std::string_view html(block.html.data(), block.html.size());
    const ListItems items = ListItems::split(html);
    if (!items.ok)
        return {};
    // Each item's lines, as its row will have them (blank lines after it
    // included), from where each line of `source` starts.
    std::vector<int> starts{0};
    for (int i = 0; i < source.size(); ++i) {
        if (source.at(i) == u'\n')
            starts.push_back(i + 1);
    }
    const auto mathOf = [&](int i) {
        return i < int(block.inline_math.size())
                   ? QString::fromUtf8(block.inline_math[i].data(), qsizetype(block.inline_math[i].size()))
                   : QString();
    };
    QString text;
    for (size_t k = 0; k < items.items.size(); ++k) {
        const ListItems::Item &item = items.items[k];
        const size_t first = block.first_line + item.line;
        const size_t next = k + 1 < items.items.size() ? block.first_line + items.items[k + 1].line : starts.size();
        if (first >= starts.size() || next > starts.size())
            return {};
        const int from = starts[first];
        const int to = next < starts.size() ? starts[next] - 1 : int(source.size());
        QStringList math;
        const QString itemHtml = ListItems::itemHtml(html, item, int(block.first_line) + item.line, items.ordered, mathOf, &math);
        const Render render = itemRender(itemHtml, math, source.mid(from, to - from), ctx);
        if (render.block->kind() != RenderedBlock::Kind::Rich || !render.block->isRegularItem())
            return {};
        if (k > 0)
            text += u'\n';
        text += render.block->renderedText();
    }
    return text;
}

bool VisualView::rendersAs(const QString &source, int firstLine, const QString &expected, bool items)
{
    const RenderContext &ctx = renderContext();
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
        // A long list's items render one by one, as their rows will (the
        // last with the blank lines after it).
        if (items && blocks.size() == 1) {
            const QString itemText = itemsText(b, source, ctx);
            if (!itemText.isNull()) {
                text += itemText;
                text += u'\n';
                continue;
            }
        }
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
    const std::optional<Spot> spot = spotFor(c);
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
        if (!rendersAs(after, spot->row.firstLine, expected, spot->items))
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
    const std::optional<Spot> spot = spotFor(c);
    if (!spot || !spot->editable)
        return false;
    const QString &text = spot->text;
    int ka = spot->offsetOf(c.selectionStart());
    int kb = spot->offsetOf(c.selectionEnd());
    if (!c.hasSelection() || ka == kb) {
        const int k = spot->offsetOf(c.position());
        // At the edge of a long list's item: joined with the item before
        // (after), over the line break between them.
        if (spot->items && spot->index == spot->lastIndex && (forward ? k >= text.size() : k <= 0)) {
            const int other = neighbourItem(spot->index, forward ? 1 : -1);
            const std::optional<Spot> both
                = other < 0 ? std::nullopt : spotOverItems(qMin(other, spot->index), qMax(other, spot->index));
            if (!both)
                return false;
            return joinItems(*both, forward ? int(text.size()) : int(both->text.size() - text.size()) - 1);
        }
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
        // Joining list items: the item after goes on the line of the one
        // before; the lines or paragraphs of a block: as source.
        for (int i = ka; i < kb; ++i) {
            if (isLineBreak(text.at(i)))
                return joinItems(*spot, i);
        }
    }
    const QString expected = text.left(ka) + text.mid(kb);
    int at = 0;
    const QString removed = removeRendered(text, spot->source, ka, kb, &at);
    int cleanAt = at;
    const QString cleaned = removeEmptyMarkup(removed, &cleanAt);
    for (const auto &[after, caret] : {std::pair(cleaned, cleanAt), std::pair(removed, at)}) {
        if (!rendersAs(after, spot->row.firstLine, expected, spot->items))
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

bool VisualView::joinItems(const Spot &spot, int breakAt)
{
    const QString &text = spot.text;
    const QString &source = spot.source;
    if (breakAt < 0 || breakAt >= text.size() || !isLineBreak(text.at(breakAt)))
        return false;
    // The break is matched with the end of the first item's line; the next
    // line that is not blank is the second item's, which must start one.
    const int matched = matchOf(text, source, breakAt);
    if (matched < 0)
        return false;
    const int lineBreak = int(source.indexOf(u'\n', matched));
    if (lineBreak < 0)
        return false;
    int next = lineBreak + 1;
    int lineStart = 0;
    int lineEnd = 0;
    for (lineAround(source, next, &lineStart, &lineEnd); isBlank(source.mid(lineStart, lineEnd - lineStart));
         lineAround(source, next, &lineStart, &lineEnd)) {
        if (lineEnd >= source.size())
            return false;
        next = lineEnd + 1;
    }
    const md::ListLine item = md::parseListLine(source.mid(lineStart, lineEnd - lineStart));
    if (!item.valid || item.isQuote)
        return false;
    // From the end of the first item's last line that is not blank to the
    // second item's text: its marker and box go.
    int end = lineStart - 1;
    while (end > 0) {
        const int start = int(source.lastIndexOf(u'\n', end - 1)) + 1;
        if (!isBlank(source.mid(start, end - start)))
            break;
        end = start - 1;
    }
    if (end != lineBreak)
        return false;
    const int contentStart = lineStart + item.contentStart;
    const QString joined = source.left(end) + source.mid(contentStart);
    int skip = 1; // The break, and the box rendered after it.
    if (!item.box.isEmpty() && breakAt + 2 < text.size()
        && (text.at(breakAt + 1) == QChar(0x2610) || text.at(breakAt + 1) == QChar(0x2611)))
        skip += text.at(breakAt + 2).isSpace() ? 2 : 1;
    const QString expected = text.left(breakAt) + text.mid(breakAt + skip);
    if (!rendersAs(joined, spot.row.firstLine, expected, spot.items))
        return false;
    applyEdit(spot.start + end, spot.start + contentStart, QString(), spot.start + end);
    return true;
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
        if (!rendersAs(after, spot->row.firstLine, expected, spot->items))
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
    const std::optional<Spot> spot = spotFor(c);
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
        // (after), in source. (A long list's item that is not its first or
        // last is inside the block.)
        const int k = spot->offsetOf(c.position());
        const bool blockStart = !spot->items || neighbourItem(spot->index, -1) < 0;
        const bool blockEnd = !spot->items || neighbourItem(spot->lastIndex, 1) < 0;
        if (!ctrl && !c.hasSelection() && !forward && k == 0 && blockStart)
            openAndForward(event, spot->start);
        else if (!ctrl && !c.hasSelection() && forward && k >= spot->text.size() && blockEnd)
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

void VisualView::typingKeyPress(QKeyEvent *event)
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

void VisualView::typingMousePress(QMouseEvent *event)
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

void VisualView::typingMouseDoubleClick(QMouseEvent *event)
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
