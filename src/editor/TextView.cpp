#include "TextView.h"

#include "../PlainText.h"
#include "../Settings.h"
#include "../Theme.h"
#include "DocumentLayout.h"
#include "TableSizePicker.h"
#include "TextViewColumns.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHelpEvent>
#include <QImage>
#include <QMessageBox>
#include <QMenu>
#include <QPointer>
#include <QMimeData>
#include <QPainter>
#include <QPen>
#include <QScrollBar>
#include <QStyleHints>
#include <QToolTip>
#include <QUrl>
#include <QTextBlock>
#include <QTextDocument>

#include <QSet>
#include <QElapsedTimer>
#include <QScopedValueRollback>
#include <QtMath>

#include <algorithm>
#include <climits>
#include <cmath>
#include <tuple>

namespace vn {

namespace {

constexpr int gutterLeftPad = 6;
constexpr int gutterRightPad = 14;
constexpr qreal hugeWidth = 1e7;
// Marks clipboard text copied as whole lines (copy with no selection).
const QString wholeLinesMime = QStringLiteral("application/x-visnotepad-lines");
// Carries the path of the Markdown file a text was copied from.
const QString markdownSourceMime = QStringLiteral("application/x-visnotepad-markdown-source");

QColor diagnosticColor()
{
    return QColor(0xCD, 0x31, 0x31);
}

// Local files in the mime data, if it holds nothing but local file URLs.
QStringList localFiles(const QMimeData *mime)
{
    QStringList files;
    if (!mime || !mime->hasUrls())
        return files;
    const QList<QUrl> urls = mime->urls();
    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            return {};
        files.append(url.toLocalFile());
    }
    return files;
}

QColor searchColor()
{
    return QColor(0xF2, 0xDF, 0xA6);
}

QColor searchCurrentColor()
{
    return QColor(0xE8, 0xC2, 0x5C);
}

int leadingWhitespace(const QString &text)
{
    int i = 0;
    while (i < text.size() && (text.at(i) == u' ' || text.at(i) == u'\t'))
        ++i;
    return i;
}

} // namespace

namespace {

// Where a position goes when `removed` characters at `at` go and then `added`
// come there, as QTextCursorPrivate::adjustPosition() moves a cursor's
// position and anchor for each (with MoveCursor, as text inserted and removed
// through a cursor has it): one at `at` goes after the new text, unless it
// keeps its place there (a cursor's position with keepPositionOnInsert).
int moved(int p, int at, int removed, int added, bool keepAtInsert = false)
{
    if (p < at)
        return p;
    if (p > at)
        p = p >= at + removed ? p - removed : at;
    if (p == at && keepAtInsert)
        return p;
    return p + added;
}

} // namespace

// See the declaration. One per document: a child of it, so it goes with it
// (the views may outlive it: a tab's document goes before its views).
class TextView::Journal : public QObject
{
public:
    static Journal *of(QTextDocument *document, bool make)
    {
        // (Asked at every edit, mostly of the same document.)
        static QPointer<Journal> last;
        if (last && last->parent() == document)
            return last;
        for (QObject *child : document->children()) {
            if (auto *journal = dynamic_cast<Journal *>(child))
                return last = journal;
        }
        return make ? (last = new Journal(document)) : nullptr;
    }

    std::vector<TextView *> views;

    // A change through insertText() or removeSelectedText() is about to be
    // made.
    void starting()
    {
        if (!m_inBlock)
            m_stepsBefore = static_cast<QTextDocument *>(parent())->availableUndoSteps();
        m_inBlock = true;
    }

    // A change made through insertText() or removeSelectedText(), within an
    // edit block (the document tells of the block once it ends): `removed`
    // characters at `at` went (through a cursor whose position was before its
    // anchor if keepAtUndo: put back, they come after a cursor there), then
    // `added` came there.
    void report(int at, int removed, int added, bool keepAtUndo)
    {
        if (!m_undoing)
            m_block.push_back({at, removed, added, keepAtUndo});
        tell(at, removed, added, false);
    }

    // The views' undo (or redo), as TextView::undoRedo() asks for it: the
    // other positions kept go along with its changes, which the document
    // makes without reports. For a few, cursors are put in (runLive());
    // else the changes the undo step was made of, if they were all reported
    // (and none was made since without a report), are told again backwards.
    void undoRedo(const TextView *except, bool undo)
    {
        auto *document = static_cast<QTextDocument *>(parent());
        m_undoing = true;
        if (!runLive(except, [document, undo] { undo ? document->undo() : document->redo(); })) {
            const int before = document->availableUndoSteps();
            QTextCursor block(document);
            block.beginEditBlock();
            undo ? document->undo() : document->redo();
            const auto found = m_steps.find(undo ? before : document->availableUndoSteps());
            if (found != m_steps.end()) {
                const std::vector<Change> &changes = found->second;
                if (undo) {
                    for (auto it = changes.crbegin(); it != changes.crend(); ++it) {
                        tell(it->at, it->added, 0, false);
                        tell(it->at, 0, it->removed, it->keepAtUndo);
                    }
                } else {
                    for (const Change &c : changes)
                        tell(c.at, c.removed, c.added, false);
                }
            }
            block.endEditBlock();
        }
        m_undoing = false;
    }

    // Whether the change the document told of last was the reported ones.
    bool matched() const { return m_matched; }

private:
    // Runs `edit`, which changes the document some other way (its undo),
    // with a cursor in it for every position the views keep but the carets
    // of `except` (it puts its own), so that they move as cursors do, and
    // reads them back. Not with very many (false, nothing done): a document
    // that has held thousands of cursors at once stays slower to edit (Qt
    // goes through a set as large as it once was at every change).
    bool runLive(const TextView *except, const std::function<void()> &edit)
    {
        constexpr qsizetype mostCursors = 256;
        qsizetype count = 0;
        for (const TextView *view : views) {
            count += (view == except ? 0 : view->m_carets.size()) + 2 * view->m_autoClosed.size() + 2;
            for (const CaretState &state : view->m_expandHistory)
                count += state.ranges.size();
        }
        if (count > mostCursors)
            return false;
        auto *document = static_cast<QTextDocument *>(parent());
        const int end = document->characterCount() - 1;
        // (Placed inside an edit block, where Qt works out no x for them.)
        QTextCursor block(document);
        block.beginEditBlock();
        struct Live
        {
            QTextCursor cursor;
            int *anchor;
            int *position;
        };
        std::vector<Live> live;
        const auto track = [&](int *anchor, int *position, bool keepAtInsert) {
            QTextCursor cursor(document);
            cursor.setPosition(qBound(0, anchor ? *anchor : *position, end));
            cursor.setPosition(qBound(0, *position, end), QTextCursor::KeepAnchor);
            cursor.setKeepPositionOnInsert(keepAtInsert);
            live.push_back({cursor, anchor, position});
        };
        for (TextView *view : views) {
            if (view != except) {
                for (Caret &caret : view->m_carets)
                    track(&caret.anchor, &caret.position, false);
            }
            for (AutoClosed &pair : view->m_autoClosed) {
                track(nullptr, &pair.inner, true);
                track(&pair.closer.anchor, &pair.closer.position, false);
            }
            for (CaretState &state : view->m_expandHistory) {
                for (auto &range : state.ranges)
                    track(&range.first, &range.second, false);
            }
            track(&view->m_dragOrigin.anchor, &view->m_dragOrigin.position, false);
            track(nullptr, &view->m_tablePosition, false);
        }
        edit();
        for (Live &l : live) {
            if (l.anchor)
                *l.anchor = l.cursor.anchor();
            *l.position = l.cursor.position();
        }
        live.clear();
        m_live = true;
        block.endEditBlock();
        m_live = false;
        return true;
    }

    // Moves the views' positions by a change.
    void tell(int at, int removed, int added, bool keepAtInsert)
    {
        if (removed > 0)
            merge(at, -removed);
        if (added > 0)
            merge(at, added);
        for (TextView *view : views) {
            // A view not editing goes back to these if the reports do not add
            // up; one editing places its carets itself.
            if (view->m_editDepth == 0 && !view->m_keptBefore)
                view->m_keptBefore = view->keptPositions();
            view->m_reported = true;
            view->adjustPositions(at, removed, added, keepAtInsert);
        }
    }

    explicit Journal(QTextDocument *document)
        : QObject(document)
    {
        connect(document, &QTextDocument::contentsChange, this, [this, document](int from, int removed, int added) {
            m_matched = m_live || (from == m_from && removed == m_removed && added == m_added);
            if (!m_undoing) {
                // The reports make an undo step, keyed as TextView::
                // m_undoEntries are; a change not reported, one made in
                // ways unknown: the steps recorded are not those any more.
                const int steps = document->availableUndoSteps();
                if (!m_matched) {
                    m_steps.clear();
                } else if (steps > m_stepsBefore) {
                    m_steps.erase(m_steps.upper_bound(m_stepsBefore), m_steps.end());
                    m_steps[steps] = std::move(m_block);
                } else {
                    m_steps.erase(m_steps.lower_bound(steps), m_steps.end());
                }
            }
            m_block.clear();
            m_inBlock = false;
            m_from = -1;
            m_removed = 0;
            m_added = 0;
        });
    }

    // Adds a change to the reported ones as QTextDocumentPrivate::
    // adjustDocumentChangesAndCursors() adds it to what it will tell.
    void merge(int from, int addedOrRemoved)
    {
        if (m_from < 0) {
            m_from = from;
            m_removed = qMax(0, -addedOrRemoved);
            m_added = qMax(0, addedOrRemoved);
            return;
        }
        const int added = qMax(0, addedOrRemoved);
        int removed = qMax(0, -addedOrRemoved);
        int diff = 0;
        if (from + removed < m_from)
            diff = m_from - from - removed;
        else if (from > m_from + m_added)
            diff = from - (m_from + m_added);
        const int overlapStart = qMax(from, m_from);
        const int overlapEnd = qMin(from + removed, m_from + m_added);
        const int removedInside = qMax(0, overlapEnd - overlapStart);
        removed -= removedInside;
        m_from = qMin(m_from, from);
        m_removed += removed + diff;
        m_added += added - removedInside + diff;
    }

    int m_from = -1;
    int m_removed = 0;
    int m_added = 0;
    bool m_matched = false;
    bool m_live = false;
    bool m_undoing = false;

    // The changes reported, by undo step.
    struct Change
    {
        int at;
        int removed;
        int added;
        bool keepAtUndo;
    };
    std::map<int, std::vector<Change>> m_steps;
    std::vector<Change> m_block; // This edit block's so far.
    int m_stepsBefore = 0;
    bool m_inBlock = false;
};

// The carets while an edit goes through all of them in turn (see
// editAtCarets()). Every change moved every caret, N changes times N carets:
// 0.4 s for a key at 5,000 carets. Here the carets are edited in order: those
// before the one being edited are done, and those after it wait, all moved by
// `m_shift` unless a change reached into them; a change moves only the few it
// reaches, each as moved() has it.
class TextView::CaretSpans
{
public:
    explicit CaretSpans(std::vector<CaretRange> spans)
        : m_spans(std::move(spans))
    {
    }

    int size() const { return int(m_spans.size()); }

    // The next caret, where the document has it now.
    CaretRange next()
    {
        const CaretRange &s = m_spans[m_next++];
        return {s.anchor + m_shift, s.position + m_shift};
    }

    // A change while the caret taken last is edited: `removed` characters at
    // `at` went, then `added` came there.
    void change(int at, int removed, int added)
    {
        // Those done are in order: only the last few can be at or after it.
        for (int i = m_next - 2; i >= 0 && m_spans[i].end() >= at; --i) {
            m_spans[i].anchor = moved(m_spans[i].anchor, at, removed, added);
            m_spans[i].position = moved(m_spans[i].position, at, removed, added);
        }
        // Those waiting that start before the change ends; the rest all move
        // by the same.
        const int newShift = m_shift + added - removed;
        for (int i = m_next; i < size(); ++i) {
            CaretRange &s = m_spans[i];
            if (s.start() + m_shift >= at + removed)
                break;
            s.anchor = moved(s.anchor + m_shift, at, removed, added) - newShift;
            s.position = moved(s.position + m_shift, at, removed, added) - newShift;
        }
        m_shift = newShift;
    }

    // The caret taken last ends as `result`.
    void finish(CaretRange result) { m_spans[m_next - 1] = result; }

    // All the carets once each is done.
    const std::vector<CaretRange> &spans() const { return m_spans; }

private:
    std::vector<CaretRange> m_spans;
    int m_next = 0;
    int m_shift = 0;
};

TextView::TextView(QTextDocument *document, QWidget *parent)
    : QAbstractScrollArea(parent)
    , m_doc(document)
{
    Q_ASSERT(qobject_cast<DocumentLayout *>(document->documentLayout()));
    connect(static_cast<DocumentLayout *>(document->documentLayout()), &DocumentLayout::changed, this,
            &TextView::onDocumentChanged);

    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setAttribute(Qt::WA_AcceptTouchEvents);
    viewport()->setAutoFillBackground(false);
    setFont(theme::editorFont());

    m_lines.resize(m_doc->blockCount());
    noteLongLines(0, m_doc->blockCount() - 1);
    m_carets.push_back(Caret());
    m_journal = Journal::of(m_doc, true);
    m_journal->views.push_back(this);
    resetCursorHistory();

    updateMetrics();
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        if (m_updatingScrollBars)
            return;
        m_topRow = qBound(0, value, qMax(0, rowCount() - 1));
        m_topOffset = 0;
        viewport()->update();
        Q_EMIT scrolled();
    });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { viewport()->update(); });
    updateScrollBars();
}

TextView::~TextView()
{
    if (m_journal)
        std::erase(m_journal->views, this);
}

// ---------------------------------------------------------------------------
// Options

void TextView::setWordWrap(bool on)
{
    if (m_wordWrap == on)
        return;
    m_wordWrap = on;
    horizontalScrollBar()->setValue(0);
    requestRelayout();
}

void TextView::setLineNumbersVisible(bool on)
{
    if (m_lineNumbers == on)
        return;
    m_lineNumbers = on;
    requestRelayout();
}

void TextView::setShowWhitespace(bool on)
{
    if (m_showWhitespace == on)
        return;
    m_showWhitespace = on;
    requestRelayout();
}

void TextView::setHighlightCurrentLine(bool on)
{
    m_highlightCurrentLine = on;
    viewport()->update();
}

void TextView::setTabWidth(int columns)
{
    columns = qMax(1, columns);
    if (m_tabWidth == columns)
        return;
    m_tabWidth = columns;
    requestRelayout();
}

void TextView::setMarkdownEditing(bool on)
{
    if (m_markdown == on)
        return;
    m_markdown = on;
    // Link hover needs moves without buttons; only Markdown accepts drops so
    // the window keeps its "open dropped files" behaviour for other text.
    viewport()->setMouseTracking(on);
    viewport()->setAcceptDrops(on);
    if (!on)
        viewport()->setCursor(Qt::IBeamCursor);
}

void TextView::setDocumentPath(const QString &path)
{
    m_documentPath = path;
}

void TextView::setDiagnostics(const QList<Diagnostic> &diagnostics)
{
    // Cursors keep the ranges in place while the text is edited.
    m_diagnostics.clear();
    for (const Diagnostic &d : diagnostics) {
        QTextCursor start(m_doc);
        start.setPosition(d.start);
        QTextCursor end(m_doc);
        end.setPosition(d.end);
        m_diagnostics.append({start, end, d.message});
    }
    viewport()->update();
}

QList<TextView::Diagnostic> TextView::diagnostics() const
{
    QList<Diagnostic> result;
    for (const DiagnosticRange &d : m_diagnostics) {
        if (d.start.position() < d.end.position())
            result.append({d.start.position(), d.end.position(), d.message});
    }
    return result;
}

QString TextView::diagnosticAt(int position) const
{
    for (const DiagnosticRange &d : m_diagnostics) {
        if (position >= d.start.position() && position < d.end.position())
            return d.message;
    }
    return {};
}

void TextView::setInsertSpaces(bool on)
{
    m_insertSpaces = on;
}

void TextView::setLineSpacing(qreal factor)
{
    factor = qBound(1.0, factor, 3.0);
    if (m_lineSpacing == factor)
        return;
    m_lineSpacing = factor;
    updateMetrics();
    requestRelayout();
}

void TextView::beginSettingsUpdate()
{
    Q_ASSERT(!m_settingsUpdate);
    m_settingsUpdate = true;
}

void TextView::endSettingsUpdate()
{
    Q_ASSERT(m_settingsUpdate);
    m_settingsUpdate = false;
    if (std::exchange(m_relayoutPending, false))
        relayout();
}

void TextView::requestRelayout()
{
    if (m_settingsUpdate)
        m_relayoutPending = true;
    else
        relayout();
}

void TextView::setCommentMarker(const QString &marker)
{
    m_commentMarker = marker;
}

void TextView::setReadOnly(bool on)
{
    m_readOnly = on;
    rowsMayHaveChanged();
    updateScrollBars();
    viewport()->update();
}

// ---------------------------------------------------------------------------
// Carets

void TextView::insertText(QTextCursor &cursor, const QString &text)
{
    Journal *journal = Journal::of(cursor.document(), false);
    if (journal)
        journal->starting();
    const int at = cursor.selectionStart();
    const int removed = cursor.selectionEnd() - at;
    // (As QTextCursorPrivate::remove() tells the document to undo it.)
    const bool keepAtUndo = cursor.position() < cursor.anchor();
    cursor.insertText(text);
    const int added = cursor.position() - at;
    if (journal && (removed > 0 || added > 0))
        journal->report(at, removed, added, keepAtUndo);
}

void TextView::removeSelectedText(QTextCursor &cursor)
{
    Journal *journal = Journal::of(cursor.document(), false);
    if (journal)
        journal->starting();
    const int at = cursor.selectionStart();
    const int removed = cursor.selectionEnd() - at;
    const bool keepAtUndo = cursor.position() < cursor.anchor();
    cursor.removeSelectedText();
    if (journal && removed > 0)
        journal->report(at, removed, 0, keepAtUndo);
}

TextView::KeptPositions TextView::keptPositions() const
{
    return {m_carets, m_autoClosed, m_expandHistory, m_dragOrigin, m_tablePosition};
}

void TextView::adjustPositions(int at, int removed, int added, bool keepAtInsert)
{
    const auto move = [&](int &p) { p = moved(p, at, removed, added, keepAtInsert); };
    if (m_spans) {
        Q_ASSERT(!keepAtInsert);
        m_spans->change(at, removed, added);
    } else {
        for (Caret &caret : m_carets) {
            move(caret.anchor);
            move(caret.position);
        }
    }
    for (AutoClosed &pair : m_autoClosed) {
        pair.inner = moved(pair.inner, at, removed, added, true);
        move(pair.closer.anchor);
        move(pair.closer.position);
    }
    for (CaretState &state : m_expandHistory) {
        for (auto &range : state.ranges) {
            move(range.first);
            move(range.second);
        }
    }
    move(m_dragOrigin.anchor);
    move(m_dragOrigin.position);
    move(m_tablePosition);
}

QTextCursor TextView::cursorAt(const CaretRange &range) const
{
    const int end = m_doc->characterCount() - 1;
    QTextCursor cursor(m_doc);
    cursor.setPosition(qBound(0, range.anchor, end));
    cursor.setPosition(qBound(0, range.position, end), QTextCursor::KeepAnchor);
    return cursor;
}

QString TextView::textBetween(int from, int to) const
{
    QString text;
    if (to <= from)
        return text;
    if (to - from <= 4096) {
        // (A line of a few megabytes is not copied for a word of it.)
        text.resize(to - from);
        for (int i = from; i < to; ++i)
            text[i - from] = m_doc->characterAt(i);
        return text;
    }
    for (QTextBlock block = m_doc->findBlock(from); block.isValid() && block.position() < to; block = block.next()) {
        const int start = block.position();
        const int separator = start + block.length() - 1;
        if (separator > from)
            text += QStringView(block.text()).mid(qMax(from, start) - start, qMin(to, separator) - qMax(from, start));
        if (separator >= from && separator < to)
            text += QChar::ParagraphSeparator;
    }
    return text;
}

int TextView::blockNumberOf(int position) const
{
    return m_doc->findBlock(position).blockNumber();
}

TextView::CaretRange TextView::primaryCaret() const
{
    return m_carets[m_primary].range();
}

QList<TextView::CaretRange> TextView::caretRanges() const
{
    QList<CaretRange> ranges;
    ranges.reserve(qsizetype(m_carets.size()));
    for (const Caret &caret : m_carets)
        ranges.append(caret.range());
    return ranges;
}

void TextView::setCaretRanges(const QList<CaretRange> &ranges, int primary)
{
    if (ranges.isEmpty())
        return;
    const int end = m_doc->characterCount() - 1;
    m_carets.clear();
    m_carets.reserve(ranges.size());
    for (const CaretRange &range : ranges) {
        Caret caret;
        caret.anchor = qBound(0, range.anchor, end);
        caret.position = qBound(0, range.position, end);
        m_carets.push_back(caret);
    }
    m_primary = qBound(0, primary, int(m_carets.size()) - 1);
    m_expandHistory.clear();
    caretsChanged();
}

QTextCursor TextView::textCursor() const
{
    return cursorAt(m_carets[m_primary]);
}

void TextView::setTextCursor(const QTextCursor &cursor)
{
    setCursors({cursor}, 0);
}

bool TextView::hasSelection() const
{
    for (const Caret &caret : m_carets) {
        if (caret.hasSelection())
            return true;
    }
    return false;
}

void TextView::mirrorCursorsFrom(const TextView *other, bool reveal)
{
    if (!other || other == this || other->m_doc != m_doc)
        return;
    m_carets.clear();
    for (const Caret &caret : other->m_carets) {
        Caret copy;
        copy.anchor = caret.anchor;
        copy.position = caret.position;
        m_carets.push_back(copy);
    }
    m_primary = qBound(0, other->m_primary, int(m_carets.size()) - 1);
    m_expandHistory.clear();
    // Like caretsChanged(), but silent: the owner of the selection announces it.
    normalizeCarets();
    pruneAutoClosed();
    m_cursorLast = captureCarets();
    m_cursorLastRevision = m_doc->revision();
    rowsMayHaveChanged();
    updateScrollBars();
    if (reveal)
        ensureCursorVisible();
    viewport()->update();
}

QList<QTextCursor> TextView::cursors() const
{
    QList<QTextCursor> result;
    for (const Caret &caret : m_carets)
        result.append(cursorAt(caret));
    return result;
}

void TextView::setCursors(const QList<QTextCursor> &cursors, int primary)
{
    QList<CaretRange> ranges;
    ranges.reserve(cursors.size());
    for (const QTextCursor &cursor : cursors)
        ranges.append({cursor.anchor(), cursor.position()});
    setCaretRanges(ranges, primary);
}

QString TextView::selectedText() const
{
    std::vector<CaretRange> sorted;
    for (const Caret &caret : m_carets)
        sorted.push_back(caret.range());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const CaretRange &a, const CaretRange &b) { return a.start() < b.start(); });
    QStringList parts;
    for (const CaretRange &range : sorted) {
        if (range.hasSelection())
            parts.append(selectedTextOf(range).replace(QChar::ParagraphSeparator, u'\n'));
    }
    return parts.join(u'\n');
}

void TextView::normalizeCarets()
{
    // Sort by position and merge carets whose selections touch or overlap.
    const int primary = m_carets[m_primary].position;
    std::stable_sort(m_carets.begin(), m_carets.end(), [](const Caret &a, const Caret &b) { return a.start() < b.start(); });
    std::vector<Caret> merged;
    merged.reserve(m_carets.size());
    for (Caret &caret : m_carets) {
        if (!merged.empty()) {
            Caret &last = merged.back();
            const int lastEnd = last.end();
            const int start = caret.start();
            const bool bothEmpty = !last.hasSelection() && !caret.hasSelection();
            if (start < lastEnd || (bothEmpty && start == lastEnd)
                || (start == lastEnd && (!caret.hasSelection() || !last.hasSelection()))) {
                const int newStart = last.start();
                const int newEnd = qMax(lastEnd, caret.end());
                const bool forward = caret.position >= caret.anchor;
                last.anchor = forward ? newStart : newEnd;
                last.position = forward ? newEnd : newStart;
                continue;
            }
        }
        merged.push_back(caret);
    }
    m_carets = std::move(merged);
    m_primary = 0;
    for (int i = 0; i < int(m_carets.size()); ++i) {
        const Caret &c = m_carets[i];
        if (c.start() <= primary && primary <= c.end())
            m_primary = i;
    }
}

void TextView::caretsChanged()
{
    normalizeCarets();
    realignLeftTable();
    recordCursorHistory();
    rowsMayHaveChanged();
    restartBlink();
    updateScrollBars();
    ensureCursorVisible();
    viewport()->update();
    Q_EMIT cursorPositionChanged();
    Q_EMIT selectionChanged();
}

void TextView::collapseToPrimary()
{
    Caret primary = m_carets[m_primary];
    if (m_carets.size() == 1 && primary.hasSelection())
        primary.clearSelection();
    setCaretRanges({primary.range()}, 0);
}

void TextView::restartBlink()
{
    m_caretOn = true;
    const int interval = QApplication::cursorFlashTime() / 2;
    if (interval > 0 && hasFocus())
        m_blink.start(interval, this);
    else
        m_blink.stop();
}

// ---------------------------------------------------------------------------
// Layout

void TextView::onDocumentChanged(int from, int charsRemoved, int charsAdded)
{
    // The kept positions moved with the changes reported as they were made
    // (see Journal); changes made otherwise move them as a whole now.
    const DocumentLayout::Change edit = static_cast<DocumentLayout *>(m_doc->documentLayout())->textChange();
    if (edit.from >= 0 && !(m_journal && m_journal->matched())) {
        if (m_editDepth == 0 && m_keptBefore) {
            const KeptPositions &kept = *m_keptBefore;
            m_carets = kept.carets;
            m_autoClosed = kept.autoClosed;
            m_expandHistory = kept.expandHistory;
            m_dragOrigin = kept.dragOrigin;
            m_tablePosition = kept.tablePosition;
        }
        // (A view editing places its carets itself; it only has to have them
        // where the document is now if nothing it did was reported.)
        if (m_editDepth == 0 || !m_reported) {
            adjustPositions(edit.from, edit.charsRemoved, edit.charsAdded);
            // QTextDocument::setPlainText() tells of one character more than
            // it puts in (it counts the line break a cleared document starts
            // with): a cursor at its end is at the end of the text.
            const int end = m_doc->characterCount() - 1;
            const auto clamp = [end](int &p) { p = qBound(0, p, end); };
            for (Caret &caret : m_carets) {
                clamp(caret.anchor);
                clamp(caret.position);
            }
            for (AutoClosed &pair : m_autoClosed) {
                clamp(pair.inner);
                clamp(pair.closer.anchor);
                clamp(pair.closer.position);
            }
            for (CaretState &state : m_expandHistory) {
                for (auto &range : state.ranges) {
                    clamp(range.first);
                    clamp(range.second);
                }
            }
            clamp(m_dragOrigin.anchor);
            clamp(m_dragOrigin.position);
            clamp(m_tablePosition);
        }
    }
    m_keptBefore.reset();
    m_reported = false;
    shiftSearchRanges(from, charsRemoved, charsAdded);
    // Splice the line cache: [first, first + oldAffected) is replaced by the
    // newAffected lines now covering the change.
    const int newCount = m_doc->blockCount();
    const int oldCount = int(m_lines.size());
    const int first = qMax(0, m_doc->findBlock(from).blockNumber());
    const int endPos = qBound(0, from + charsAdded, m_doc->characterCount() - 1);
    const int lastNew = qMax(first, m_doc->findBlock(endPos).blockNumber());
    const int newAffected = lastNew - first + 1;
    const int oldAffected = newAffected - (newCount - oldCount);
    if (oldAffected < 0 || first + oldAffected > oldCount) {
        m_lines.clear();
        m_lines.resize(newCount);
        m_hasLongLines = false;
        noteLongLines(0, newCount - 1);
        m_liveLayouts = 0;
        invalidateFenceMarks();
        m_tableRun = {};
        m_htmlRun = {};
    } else {
        // Lines laid out in pieces keep the ones the edit did not reach.
        std::vector<std::unique_ptr<LineLayout>> pieced;
        for (int i = first; i < first + oldAffected; ++i) {
            LineCache &line = m_lines[i];
            if (line.layout && line.layout->inPieces()) {
                pieced.resize(oldAffected);
                pieced[i - first] = std::move(line.layout);
                --m_liveLayouts;
            }
        }
        if (newAffected == oldAffected) {
            // The lines keep their places: reset them where they are rather
            // than shifting everything after them down and up again.
            for (int i = first; i < first + newAffected; ++i)
                m_lines[i] = LineCache();
        } else {
            m_lines.erase(m_lines.begin() + first, m_lines.begin() + first + oldAffected);
            std::vector<LineCache> fresh(newAffected);
            m_lines.insert(m_lines.begin() + first, std::make_move_iterator(fresh.begin()),
                           std::make_move_iterator(fresh.end()));
        }
        noteLongLines(first, first + newAffected - 1);
        spliceFenceMarks(first, oldAffected, newAffected);
        spliceTableRun(first, oldAffected, newAffected);
        if (!pieced.empty())
            keepPieces(first, newAffected, std::move(pieced));
    }
    m_topRow = qBound(0, m_topRow, qMax(0, rowCount() - 1));
    columnsRowsChanged(true);
    rowsMayHaveChanged();
    updateScrollBars();
    viewport()->update();
}

void TextView::keepPieces(int first, int newAffected, std::vector<std::unique_ptr<LineLayout>> old)
{
    // A line that starts before the edit has the start of the line that
    // was there; one that ends after it, the end of the line that was where
    // it is now, lines added or removed apart.
    const DocumentLayout::Change edit = static_cast<DocumentLayout *>(m_doc->documentLayout())->textChange();
    const int oldAffected = int(old.size());
    const int delta = newAffected - oldAffected;
    const qreal wrapWidth = m_wordWrap ? textWidth() : hugeWidth;
    QTextBlock block = m_doc->findBlockByNumber(first);
    for (int line = first; line < first + newAffected && block.isValid(); ++line, block = block.next()) {
        const int length = block.length() - 1;
        if (length <= LineLayout::piecesFrom)
            continue;
        const int i = line - first;
        const int j = line - delta - first;
        LineLayout *before = i < oldAffected ? old[i].get() : nullptr;
        LineLayout *after = j >= 0 && j < oldAffected ? old[j].get() : nullptr;
        int prefix = 0;
        int suffix = 0;
        if (edit.from >= 0) {
            const int start = block.position();
            if (before && start <= edit.from)
                prefix = std::min({edit.from - start, length, before->length()});
            else
                before = nullptr;
            if (after && start + length >= edit.from + edit.charsAdded) {
                suffix = std::min({start + length - (edit.from + edit.charsAdded), length - prefix, after->length()});
                if (after == before)
                    suffix = qMin(suffix, after->length() - prefix);
            } else {
                after = nullptr;
            }
        } else if (delta == 0 && before) {
            // Not known what changed (only colours, most likely): compared.
            after = before;
            std::tie(prefix, suffix) = before->common(block.text());
        } else {
            continue;
        }
        if (!before && !after)
            continue;
        // Pieces laid out for another width or font (a hidden view zoomed)
        // stay so, marked stale: when shown, layoutForLine() lays them out
        // again keeping the text at the top, as it would without the edit.
        const LineLayout *old = before ? before : after;
        const LineLayout::Setup setup = layoutSetup(wrapWidth);
        const bool stale = !old->hasSetup(setup);
        auto layout = std::make_unique<LineLayout>();
        if (!layout->rebuild(block, stale ? LineLayout::Setup(old->setup()) : setup, before, prefix, after, suffix))
            continue;
        if (!m_wordWrap && !stale)
            m_maxLineWidth = qMax(m_maxLineWidth, layout->width());
        LineCache &cache = m_lines[line];
        if (!cache.layout)
            ++m_liveLayouts;
        cache.layout = std::move(layout);
        cache.wrapWidth = stale ? -2 : wrapWidth;
        listenToLayout(line);
    }
}

void TextView::updateMetrics()
{
    const QFontMetricsF fm(font());
    m_lineHeight = qCeil(fm.height() * m_lineSpacing);
    m_linePad = (m_lineHeight - fm.height()) / 2;
    m_charWidth = fm.horizontalAdvance(QLatin1Char(' '));
}

void TextView::invalidateLayouts()
{
    m_liveLayouts = 0;
    for (LineCache &line : m_lines) {
        if (line.layout && line.layout->inPieces()) {
            line.wrapWidth = -2;
            ++m_liveLayouts;
        } else {
            line.layout.reset();
        }
    }
    m_maxLineWidth = 0;
    restartSettling();
}

void TextView::noteLongLines(int first, int last)
{
    if (m_hasLongLines || first > last || m_doc->characterCount() <= LineLayout::piecesFrom)
        return;
    QTextBlock block = m_doc->findBlockByNumber(first);
    for (int i = first; i <= last && block.isValid(); ++i, block = block.next()) {
        if (block.length() - 1 > LineLayout::piecesFrom) {
            m_hasLongLines = true;
            return;
        }
    }
}

void TextView::restartSettling()
{
    m_settleTimer.start(300, this);
}

void TextView::listenToLayout(int line)
{
    const QTextBlock block = m_doc->findBlockByNumber(line);
    m_lazyBlocks.removeIf([](const QTextBlock &b) { return !b.isValid(); });
    if (!m_lazyBlocks.contains(block)) m_lazyBlocks.append(block);
    m_lines[line].layout->setListener([this, block](LineLayout *layout, int from, int delta) {
        if (block.isValid()) rowsMoved(block.blockNumber(), layout, from, delta);
    });
    restartSettling();
}

void TextView::rowsMoved(int line, LineLayout *layout, int from, int delta)
{
    Q_ASSERT(!m_painting);
    if (!m_wordWrap) m_maxLineWidth = qMax(m_maxLineWidth, layout->width());
    if (!delta) return;
    const auto shift = [&](int topRow, qreal &offset) {
        if (row(topRow).firstLine == line && offset >= from * m_lineHeight)
            offset += delta * m_lineHeight;
    };
    if (!m_columnsOn) shift(m_topRow, m_topOffset);
    for (auto *history : {&m_cursorUndo, &m_cursorRedo})
        for (CursorHistoryEntry &entry : *history) shift(entry.topRow, entry.topOffset);
    if (m_columnAnchorValid && m_columnAnchor.block == line && m_columnAnchor.subLine >= from)
        m_columnAnchor.subLine += delta;
    if (m_columns) {
        const auto adjust = [&](md::Position &p) {
            if (p.row == line && p.y >= from * m_lineHeight) p.y += delta * m_lineHeight;
        };
        m_columns->flow.adjustPositions(adjust); // Also invalidates cached tops/cuts.
        adjust(m_columns->key.start);
        if (!m_columns->dirty) m_columns->settled = true;
        m_columns->dirty = true;
    }
}

void TextView::ensureShownMeasured(bool bottom)
{
    if (m_settling || m_painting || !m_hasLongLines) return;
    QScopedValueRollback settling(m_settling, true);
    if (m_columnsOn) { columnsScreen(); return; }
    qreal y = firstRowY();
    for (int r = m_topRow; r < rowCount() && y < viewport()->height(); ++r) {
        const Row info = row(r);
        if (info.rendered) { y += rowHeight(r); continue; }
        LineLayout *layout = layoutForLine(info.firstLine);
        if (!layout || !layout->inPieces()) { y += rowHeight(r); continue; }
        if (!m_wordWrap) {
            layout->settleTo(horizontalScrollBar()->value() + viewport()->width() - textLeft());
        } else {
            y = r == m_topRow ? firstRowY() : y;
            const int anchor = bottom ? INT_MAX : r == m_topRow
                ? layout->windowAnchor(qMax(0, qFloor(-y / m_lineHeight)),
                                       qCeil((viewport()->height() - y) / m_lineHeight)) : -2;
            for (;;) {
                y = r == m_topRow ? firstRowY() : y;
                const int top = qMax(0, qFloor(-y / m_lineHeight));
                const int end = qCeil((viewport()->height() - y) / m_lineHeight);
                const auto step = layout->settleWindowStep(top, end, anchor);
                if (!step.measured) break;
                if (r == m_topRow) m_topOffset += step.extraTopRows * m_lineHeight;
            }
        }
        y = r == m_topRow ? firstRowY() : y;
        y += qMax(1, layout->lineCount()) * m_lineHeight;
    }
}

void TextView::trimLayouts(int firstLine, int lastLine)
{
    // Every painted or measured line keeps a QTextLayout; without a bound a
    // long scroll through a big file retains one per line. Layouts are
    // rebuilt on demand, so keep a generous band around the viewport.
    constexpr int maxLive = 2000;
    constexpr int keepAround = 500;
    if (m_liveLayouts <= maxLive)
        return;
    const int keepFrom = firstLine - keepAround;
    const int keepTo = lastLine + keepAround;
    int live = 0;
    for (int i = 0; i < int(m_lines.size()); ++i) {
        LineCache &line = m_lines[i];
        if (!line.layout)
            continue;
        if (i < keepFrom || i > keepTo)
            line.layout.reset();
        else
            ++live;
    }
    m_liveLayouts = live;
}

void TextView::shiftSearchRanges(int from, int charsRemoved, int charsAdded)
{
    // Keep the highlights in place until the find bar searches again: ranges
    // after the edit move, ranges the edit touched are dropped.
    if (m_searchRanges.isEmpty())
        return;
    const auto firstAffected = std::lower_bound(m_searchRanges.begin(), m_searchRanges.end(), from,
                                                [](const search::Range &r, int pos) { return r.end <= pos; });
    const int delta = charsAdded - charsRemoved;
    const int removedEnd = from + charsRemoved;
    int index = int(firstAffected - m_searchRanges.begin());
    int current = m_searchCurrent;
    int out = index;
    for (int i = index; i < m_searchRanges.size(); ++i) {
        search::Range r = m_searchRanges.at(i);
        if (r.start < removedEnd) {
            // Touched by the edit.
            if (i == m_searchCurrent)
                current = -1;
            else if (i < m_searchCurrent && current >= 0)
                --current;
            continue;
        }
        r.start += delta;
        r.end += delta;
        m_searchRanges[out++] = r;
    }
    m_searchRanges.resize(out);
    m_searchCurrent = current;
}

void TextView::relayout()
{
    invalidateLayouts();
    columnsRowsChanged(true);
    rowsMayHaveChanged();
    updateScrollBars();
    viewport()->update();
}

void TextView::formatsChanged(int firstLine, int lastLine)
{
    // Highlighting in slices reports every few milliseconds: only the lines
    // it coloured lose their layouts, and only lines laid out (painted or
    // measured) need a repaint. Bold text may wrap sooner, so the scroll
    // range is measured again; which rows there are does not change.
    firstLine = qMax(0, firstLine);
    lastLine = qMin(lastLine, int(m_lines.size()) - 1);
    bool laidOut = false;
    for (int i = firstLine; i <= lastLine; ++i) {
        LineCache &line = m_lines[i];
        if (!line.layout)
            continue;
        laidOut = true;
        if (line.layout->inPieces()) {
            // (Colours reach only the start of a long line: see Highlighter.)
            line.layout->formatsChanged(m_doc->findBlockByNumber(i));
            restartSettling();
            continue;
        }
        line.layout.reset();
        --m_liveLayouts;
    }
    if (!laidOut)
        return;
    columnsRowsChanged(false);
    updateScrollBars();
    viewport()->update();
}

QTextOption TextView::textOption() const
{
    QTextOption option;
    option.setWrapMode(m_wordWrap ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
    option.setTabStopDistance(m_charWidth * m_tabWidth);
    if (m_showWhitespace)
        option.setFlags(QTextOption::ShowTabsAndSpaces);
    return option;
}

LineLayout::Setup TextView::layoutSetup(qreal wrapWidth) const
{
    return {font(), textOption(), m_wordWrap, wrapWidth, qreal(m_lineHeight), m_linePad};
}

qreal TextView::textLeft() const
{
    return gutterWidth() + m_leftMargin;
}

qreal TextView::textWidth() const
{
    if (m_columnsOn) // A column's.
        return const_cast<TextView *>(this)->columnLayout().width;
    return qMax<qreal>(m_charWidth * 4, viewport()->width() - textLeft() - m_rightMargin);
}

void TextView::setTextMargins(qreal left, qreal right, qreal top)
{
    m_leftMargin = left;
    m_rightMargin = right;
    m_topMargin = top;
    relayout();
}

int TextView::topLine() const
{
    if (m_columnsOn)
        return m_columnsFirst;
    return row(m_topRow).firstLine;
}

int TextView::bottomLine() const
{
    if (m_columnsOn)
        return m_columnsLast;
    return qMin(m_doc->blockCount() - 1, topLine() + viewport()->height() / qMax(1, m_lineHeight) + 1);
}

bool TextView::isLineShown(int line)
{
    if (m_columnsOn) {
        const std::pair<int, int> range = columnLineRange();
        return range.first >= 0 && line >= range.first && line <= range.second;
    }
    qreal top = 0;
    qreal bottom = 0;
    return lineBounds(line, &top, &bottom) && bottom > 0 && top < viewport()->height();
}

void TextView::scrollToLine(int line)
{
    if (m_columnsOn) {
        // The screen that starts with it, at the next paint.
        m_columns->startLine = qBound(0, line, m_doc->blockCount() - 1);
        m_columns->needsStart = true;
        viewport()->update();
        return;
    }
    const int target = qBound(0, rowOfLine(qBound(0, line, m_doc->blockCount() - 1)), qMax(0, rowCount() - 1));
    if (target == m_topRow && m_topOffset == 0)
        return;
    m_topRow = target;
    m_topOffset = 0;
    updateScrollBars();
    viewport()->update();
}

std::vector<TextView::LineAnchor> TextView::rowLineAnchors(const Row &, qreal)
{
    return {};
}

std::vector<TextView::LineAnchor> TextView::lineAnchors(int index, qreal height)
{
    const Row r = row(index);
    const qreal end = r.lastLine + 1;
    std::vector<LineAnchor> anchors{{qreal(r.firstLine), 0}};
    for (const LineAnchor &a : rowLineAnchors(r, height)) {
        const LineAnchor &last = anchors.back();
        if (a.line >= last.line && a.line <= end && a.y >= last.y && a.y <= height)
            anchors.push_back(a);
    }
    anchors.push_back({end, height});
    return anchors;
}

qreal TextView::topLinePosition()
{
    return linePositionAt(m_topRow, m_topOffset);
}

qreal TextView::linePositionAt(int index, qreal y)
{
    index = qBound(0, index, rowCount() - 1);
    const qreal height = rowHeight(index);
    const std::vector<LineAnchor> anchors = lineAnchors(index, height);
    y = qBound<qreal>(0, y, height);
    // The first anchor below y and the one before it.
    for (size_t i = 1; i < anchors.size(); ++i) {
        const LineAnchor &a = anchors[i - 1];
        const LineAnchor &b = anchors[i];
        if (y < b.y)
            return a.line + (b.line - a.line) * (y - a.y) / (b.y - a.y);
    }
    return anchors.back().line;
}

qreal TextView::yOfLinePosition(const std::vector<LineAnchor> &anchors, qreal position)
{
    for (size_t i = 1; i < anchors.size(); ++i) {
        const LineAnchor &a = anchors[i - 1];
        const LineAnchor &b = anchors[i];
        if (position < b.line)
            return a.y + (b.y - a.y) * qMax<qreal>(0, position - a.line) / (b.line - a.line);
    }
    return anchors.back().y;
}

std::pair<int, qreal> TextView::placeOfLinePosition(qreal position)
{
    const int lines = m_doc->blockCount();
    position = qBound<qreal>(0, position, lines);
    const int index = qBound(0, rowOfLine(qMin(int(position), lines - 1)), qMax(0, rowCount() - 1));
    const qreal height = rowHeight(index);
    return {index, yOfLinePosition(lineAnchors(index, height), position)};
}

void TextView::scrollToLinePosition(qreal position)
{
    std::tie(m_topRow, m_topOffset) = placeOfLinePosition(position);
    normalizeScroll();
    ensureShownMeasured();
    updateScrollBars();
    viewport()->update();
}

qreal TextView::maxLastRowOffset()
{
    return qMax<qreal>(0, rowHeight(rowCount() - 1) - m_lineHeight);
}

void TextView::normalizeScroll()
{
    const int rows = rowCount();
    m_topRow = qBound(0, m_topRow, rows - 1);
    while (m_topOffset < 0 && m_topRow > 0) {
        --m_topRow;
        m_topOffset += rowHeight(m_topRow);
    }
    if (m_topOffset < 0)
        m_topOffset = 0;
    while (m_topRow < rows - 1 && m_topOffset >= rowHeight(m_topRow)) {
        m_topOffset -= rowHeight(m_topRow);
        ++m_topRow;
    }
    if (m_topRow >= rows - 1)
        m_topOffset = qMin(m_topOffset, maxLastRowOffset());
}

bool TextView::isAtScrollEnd()
{
    return m_topRow >= rowCount() - 1 && m_topOffset >= maxLastRowOffset() - 0.5;
}

void TextView::scrollToEnd()
{
    const Row last = row(rowCount() - 1);
    if (!last.rendered) {
        if (LineLayout *layout = layoutForLine(last.firstLine)) layout->settleAround(layout->length());
    }
    m_topRow = rowCount() - 1;
    m_topOffset = maxLastRowOffset();
    ensureShownMeasured(true);
    updateScrollBars();
    viewport()->update();
}

void TextView::followScroll(TextView &leader)
{
    if (leader.isAtScrollEnd() && leader.m_topRow > 0)
        scrollToEnd();
    else
        scrollToLinePosition(leader.topLinePosition());
}

qreal TextView::scrollPixels()
{
    qreal y = m_topOffset;
    for (int r = 0; r < m_topRow; ++r)
        y += rowHeight(r);
    return y;
}

int TextView::rowAtPoint(const QPointF &point, QPointF *origin)
{
    if (m_columnsOn)
        return columnsRowAt(point, origin);
    qreal top = 0;
    const int index = rowAt(point.y(), &top);
    *origin = QPointF(textLeft(), top);
    return index;
}

int TextView::rowAt(qreal y, qreal *top)
{
    ensureShownMeasured();
    qreal rowY = firstRowY();
    const int rows = rowCount();
    for (int r = m_topRow; r < rows; ++r) {
        const qreal h = rowHeight(r);
        if (y < rowY + h) {
            *top = rowY;
            return r;
        }
        rowY += h;
    }
    return -1;
}

bool TextView::renderedRowPressed(const Row &, const QPointF &, QMouseEvent *)
{
    return false;
}

LineLayout *TextView::layoutForLine(int line)
{
    if (line < 0 || line >= int(m_lines.size()))
        return nullptr;
    LineCache &cache = m_lines[line];
    const qreal wrapWidth = m_wordWrap ? textWidth() : hugeWidth;
    const QTextBlock block = m_doc->findBlockByNumber(line);
    const int caretInBlock = m_carets[m_primary].position - block.position();
    const bool hasPreedit = !m_preedit.isEmpty() && caretInBlock >= 0 && caretInBlock < block.length();
    const auto preeditFormats = [&] {
        QList<QTextLayout::FormatRange> formats;
        for (const QInputMethodEvent::Attribute &a : std::as_const(m_preeditAttributes)) {
            if (a.type != QInputMethodEvent::TextFormat)
                continue;
            const QTextCharFormat f = qvariant_cast<QTextFormat>(a.value).toCharFormat();
            if (f.isValid())
                formats.append({caretInBlock + a.start, a.length, f});
        }
        return formats;
    };
    if (cache.layout && cache.layout->inPieces() && cache.wrapWidth != wrapWidth) {
        LineLayout *layout = cache.layout.get();
        const bool keepTop = !m_columnsOn && m_wordWrap && layout->wraps()
            && row(m_topRow).firstLine == line;
        int position = 0;
        qreal remainder = 0;
        if (keepTop) {
            const qreal oldHeight = layout->lineHeight();
            const int oldRow = qBound(0, int(m_topOffset / oldHeight), layout->lineCount() - 1);
            // Measuring the containing piece may adjust places after it.
            position = layout->lineAt(oldRow).textStart();
            remainder = std::fmod(m_topOffset, oldHeight);
        }
        layout->setListener({});
        if (layout->resetup(block, layoutSetup(wrapWidth))) {
            cache.wrapWidth = wrapWidth;
            if (keepTop) {
                layout->settleAround(position);
                m_topOffset = layout->lineForTextPosition(position).lineNumber() * m_lineHeight + remainder;
            }
            if (m_columnAnchorValid && m_columnAnchor.block == line) {
                // Its row was counted with the old width's rows.
                const LineLayout::Line anchorLine = layout->lineForTextPosition(m_columnAnchor.position);
                m_columnAnchor.subLine = anchorLine.isValid() ? anchorLine.lineNumber() : 0;
            }
            listenToLayout(line);
            restartSettling();
        }
    }
    if (cache.layout && cache.layout->inPieces() && cache.wrapWidth == wrapWidth) {
        // In pieces: only the piece being composed in is laid out again.
        if (hasPreedit || cache.layout->hasPreedit()) {
            cache.layout->setPreedit(caretInBlock, hasPreedit ? m_preedit : QString(),
                                     hasPreedit ? preeditFormats() : QList<QTextLayout::FormatRange>());
        }
        if (!m_wordWrap) m_maxLineWidth = qMax(m_maxLineWidth, cache.layout->width());
        return cache.layout.get();
    }
    if (cache.layout && cache.wrapWidth == wrapWidth && !hasPreedit)
        return cache.layout.get();

    if (!cache.layout) {
        cache.layout = std::make_unique<LineLayout>();
        ++m_liveLayouts;
    }
    if (block.length() - 1 > LineLayout::piecesFrom && cache.layout->build(block, layoutSetup(wrapWidth))) {
        LineLayout *layout = cache.layout.get();
        if (hasPreedit)
            layout->setPreedit(caretInBlock, m_preedit, preeditFormats());
        if (!m_wordWrap)
            m_maxLineWidth = qMax(m_maxLineWidth, layout->width());
        cache.wrapWidth = wrapWidth;
        listenToLayout(line);
        restartSettling();
        return layout;
    }
    QTextLayout *layout = cache.layout->whole();
    layout->clearLayout();
    layout->setText(block.text());
    layout->setFont(font());
    layout->setTextOption(textOption());
    layout->setCacheEnabled(true);
    layout->setFormats(block.layout()->formats());
    if (hasPreedit) {
        layout->setPreeditArea(caretInBlock, m_preedit);
        layout->setFormats(layout->formats() + preeditFormats());
    } else {
        layout->setPreeditArea(-1, QString());
    }

    layout->beginLayout();
    qreal y = m_linePad;
    for (QTextLine textLine = layout->createLine(); textLine.isValid(); textLine = layout->createLine()) {
        textLine.setLineWidth(wrapWidth);
        textLine.setPosition({0, y});
        y += m_lineHeight;
        if (!m_wordWrap)
            m_maxLineWidth = qMax(m_maxLineWidth, textLine.naturalTextWidth());
    }
    layout->endLayout();
    cache.wrapWidth = hasPreedit ? -1 : wrapWidth; // Preedit layouts are rebuilt each time.
    return cache.layout.get();
}

qreal TextView::rowHeight(int index)
{
    const Row r = row(index);
    if (r.rendered)
        return renderedRowHeight(r, textWidth());
    LineLayout *layout = layoutForLine(r.firstLine);
    return layout ? qMax(1, layout->lineCount()) * m_lineHeight : m_lineHeight;
}

int TextView::gutterWidth() const
{
    if (!m_lineNumbers)
        return 0;
    int digits = 1;
    for (int max = qMax(1, m_doc->blockCount()); max >= 10; max /= 10)
        ++digits;
    return gutterLeftPad + int(m_charWidth * qMax(digits, 2)) + gutterRightPad;
}

void TextView::updateScrollBars()
{
    m_updatingScrollBars = true;
    QScrollBar *v = verticalScrollBar();
    const int rows = rowCount();
    const int pageRows = qMax(1, viewport()->height() / qMax(1, m_lineHeight));
    // Like VS Code (editor.scrollBeyondLastLine), the last line can scroll
    // to the top, so even a short document has a scroll range. Show the bar
    // only when the document doesn't fit; the wheel still scrolls past the end.
    // Rows differ in height in the rendered view, so measure until full.
    bool fits = true;
    qreal contentHeight = m_topMargin;
    for (int i = 0; i < rows && fits; ++i) {
        contentHeight += rowHeight(i);
        fits = contentHeight <= viewport()->height();
    }
    const Qt::ScrollBarPolicy policy = fits || m_scrollBarsHidden ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded;
    v->setRange(0, qMax(0, rows - 1));
    v->setPageStep(pageRows);
    v->setSingleStep(1);
    v->setValue(m_topRow);

    QScrollBar *h = horizontalScrollBar();
    if (m_wordWrap || m_columnsOn) { // (Each column scrolls on its own.)
        h->setRange(0, 0);
    } else {
        h->setRange(0, qMax(0, int(m_maxLineWidth - textWidth() + m_charWidth * 2)));
        h->setPageStep(int(textWidth()));
        h->setSingleStep(int(m_charWidth * 2));
    }
    m_updatingScrollBars = false;
    // Last: a policy change resizes the viewport, which re-enters here.
    if (verticalScrollBarPolicy() != policy)
        setVerticalScrollBarPolicy(policy);
}

void TextView::setScrollBarsHidden(bool hidden)
{
    if (m_scrollBarsHidden == hidden)
        return;
    m_scrollBarsHidden = hidden;
    updateScrollBars();
}

// Tab and Shift+Tab indent and outdent; they never move focus out of the
// editor, even read-only (QWidget::event offers them to focusNextPrevChild
// before keyPressEvent sees them).
bool TextView::focusNextPrevChild(bool)
{
    return false;
}

void TextView::scrollContentsBy(int, int)
{
    viewport()->update();
}

// ---------------------------------------------------------------------------
// Row model defaults

int TextView::rowCount() const
{
    return m_doc->blockCount();
}

TextView::Row TextView::row(int index) const
{
    return {index, index, false};
}

int TextView::rowOfLine(int line) const
{
    return line;
}

qreal TextView::renderedRowHeight(const Row &, qreal)
{
    return m_lineHeight;
}

void TextView::paintRenderedRow(QPainter &, const Row &, const QRectF &)
{
}

int TextView::hitTestRenderedRow(const Row &row, const QPointF &)
{
    return m_doc->findBlockByNumber(row.firstLine).position();
}

void TextView::paintLineBackground(QPainter &, int, const QRectF &)
{
}

QTextCursor TextView::lineUnitAt(int position) const
{
    QTextCursor target(m_doc);
    target.setPosition(position);
    target.movePosition(QTextCursor::StartOfBlock);
    target.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor);
    if (!target.hasSelection())
        target.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    return target;
}

// ---------------------------------------------------------------------------
// Geometry

bool TextView::rowTop(int index, qreal *top)
{
    if (index < m_topRow)
        return false;
    qreal y = firstRowY();
    const int height = viewport()->height();
    for (int r = m_topRow; r < index; ++r) {
        y += rowHeight(r);
        if (y > height)
            return false;
    }
    if (y > height)
        return false;
    *top = y;
    return true;
}

bool TextView::lineBounds(int line, qreal *top, qreal *bottom)
{
    ensureShownMeasured();
    const int index = rowOfLine(line);
    qreal y = 0;
    if (!rowTop(index, &y))
        return false;
    *top = y;
    *bottom = y + rowHeight(index);
    return true;
}

QRectF TextView::caretRect(int position)
{
    ensureShownMeasured();
    if (m_columnsOn)
        return columnsCaretRect(position);
    const QTextBlock block = m_doc->findBlock(position);
    const int rowIndex = rowOfLine(block.blockNumber());
    qreal top = 0;
    if (!rowTop(rowIndex, &top))
        return {};
    const Row r = row(rowIndex);
    if (r.rendered)
        return QRectF(textLeft(), top, 2, rowHeight(rowIndex));
    LineLayout *layout = layoutForLine(block.blockNumber());
    const int pos = position - block.position();
    layout->settleAround(pos);
    ensureShownMeasured();
    if (!rowTop(rowIndex, &top)) return {};
    const LineLayout::Line line = layout->lineForTextPosition(pos);
    if (!line.isValid())
        return QRectF(textLeft(), top, 2, m_lineHeight);
    const qreal x = line.cursorToX(pos) + textLeft() - horizontalScrollBar()->value();
    return QRectF(x, top + line.y() - m_linePad, 2, m_lineHeight);
}

qreal TextView::cursorX(int position)
{
    const QTextBlock block = m_doc->findBlock(position);
    const int pos = position - block.position();
    LineLayout *layout = layoutForLine(block.blockNumber());
    const LineLayout::Line line = layout ? layout->lineForTextPosition(pos) : LineLayout::Line();
    return line.isValid() ? line.cursorToX(pos) : 0;
}

int TextView::positionAt(const QPointF &point)
{
    ensureShownMeasured();
    if (m_columnsOn)
        return columnsPositionAt(point);
    qreal y = firstRowY();
    const int rows = rowCount();
    int r = m_topRow;
    if (point.y() < 0) {
        // Above the viewport: the start of the top row's first line.
        r = m_topRow;
    } else {
        while (r < rows) {
            const qreal h = rowHeight(r);
            if (point.y() < y + h)
                break;
            y += h;
            ++r;
        }
    }
    if (r >= rows)
        return m_doc->characterCount() - 1;

    const Row info = row(r);
    if (info.rendered)
        return hitTestRenderedRow(info, QPointF(point.x() - textLeft(), point.y() - y));

    const QTextBlock block = m_doc->findBlockByNumber(info.firstLine);
    LineLayout *layout = layoutForLine(info.firstLine);
    const qreal localY = qMax<qreal>(0, point.y() - y);
    const int lineIndex = qBound(0, int(localY / m_lineHeight), layout->lineCount() - 1);
    const LineLayout::Line line = layout->lineAt(lineIndex);
    const qreal x = point.x() - textLeft() + horizontalScrollBar()->value();
    return block.position() + line.xToCursor(x);
}

void TextView::ensureCursorVisible()
{
    if (m_columnsOn)
        return columnsEnsureCursorVisible();
    if (viewport()->height() <= 0)
        return;
    const int position = m_carets[m_primary].position;
    const QTextBlock block = m_doc->findBlock(position);
    const int line = block.blockNumber();
    const int rowIndex = rowOfLine(line);
    const Row r = row(rowIndex);

    qreal caretTop = 0;
    qreal caretBottom = m_lineHeight;
    if (!r.rendered) {
        LineLayout *layout = layoutForLine(line);
        const LineLayout::Line textLine = layout->lineForTextPosition(position - block.position());
        if (textLine.isValid())
            caretTop = textLine.y() - m_linePad;
        caretBottom = caretTop + m_lineHeight;
    } else {
        // A rendered row (a table, a list, a code box) can be many screens
        // tall: the caret is where its source line is drawn, not at the
        // row's top, or selecting in the last rows of a long table would
        // scroll back to the first.
        const qreal rowH = rowHeight(rowIndex);
        if (r.lastLine == r.firstLine) {
            caretBottom = qMax<qreal>(m_lineHeight, rowH);
        } else {
            const std::vector<LineAnchor> anchors = lineAnchors(rowIndex, rowH);
            caretTop = yOfLinePosition(anchors, line);
            caretBottom = qMax(caretTop + m_lineHeight, yOfLinePosition(anchors, line + 1));
        }
    }
    const qreal height = viewport()->height();
    // More than a screenful of the row is the caret's: showing any of it will do.
    const bool tall = caretBottom - caretTop > height;

    qreal top = 0;
    const bool visible = rowTop(rowIndex, &top)
        && (tall ? top + caretBottom > 0 && top + caretTop < height : top + caretTop >= 0 && top + caretBottom <= height);
    if (!visible) {
        if (rowIndex < m_topRow || (rowIndex == m_topRow && caretTop < m_topOffset)) {
            m_topRow = rowIndex;
            m_topOffset = caretTop;
        } else {
            // Scroll so the caret's bottom sits at the viewport's bottom
            // (its top at the viewport's top when it is taller).
            int topRow = rowIndex;
            qreal offset = tall ? caretTop : caretBottom - height;
            while (offset < 0 && topRow > 0) {
                --topRow;
                offset += rowHeight(topRow);
            }
            m_topRow = topRow;
            m_topOffset = qMax<qreal>(0, offset);
        }
        updateScrollBars();
        Q_EMIT scrolled();
    }

    if (!m_wordWrap) {
        QScrollBar *h = horizontalScrollBar();
        const qreal x = cursorX(position);
        const qreal width = textWidth();
        if (x < h->value())
            h->setValue(int(qMax<qreal>(0, x - m_charWidth * 4)));
        else if (x > h->value() + width - m_charWidth)
            h->setValue(int(x - width + m_charWidth * 4));
    }
    ensureShownMeasured();
    viewport()->update();
}

void TextView::centerCursor()
{
    if (m_columnsOn)
        return columnsEnsureCursorVisible(); // (The screen that starts with it, if it is not on this one.)
    const int rowIndex = rowOfLine(blockNumberOf(m_carets[m_primary].position));
    qreal remaining = viewport()->height() / 2.0;
    int topRow = rowIndex;
    while (topRow > 0 && remaining > 0) {
        remaining -= rowHeight(topRow - 1);
        if (remaining < 0)
            break;
        --topRow;
    }
    m_topRow = topRow;
    m_topOffset = 0;
    ensureShownMeasured();
    updateScrollBars();
    viewport()->update();
}

void TextView::setSearchHighlights(const QList<search::Range> &ranges, int current)
{
    m_searchRanges = ranges;
    m_searchCurrent = current;
    viewport()->update();
}

// ---------------------------------------------------------------------------
// Painting

TextView::LinePaintState TextView::linePaintState() const
{
    LinePaintState state;
    // Lines that hold a caret with no selection get the current-line shade.
    for (const Caret &caret : m_carets) {
        if (!caret.hasSelection())
            state.currentLines.insert(blockNumberOf(caret.position));
    }
    // Diagnostics by start, with the running maximum of their ends, so each
    // line finds the ones reaching it by binary search.
    state.diagnosticSpans.reserve(m_diagnostics.size());
    for (int i = 0; i < m_diagnostics.size(); ++i)
        state.diagnosticSpans.push_back({m_diagnostics.at(i).start.position(), m_diagnostics.at(i).end.position(), i});
    std::stable_sort(state.diagnosticSpans.begin(), state.diagnosticSpans.end(),
                     [](const LinePaintState::DiagnosticSpan &a, const LinePaintState::DiagnosticSpan &b) {
                         return a.start < b.start;
                     });
    int runningEnd = 0;
    for (const LinePaintState::DiagnosticSpan &d : state.diagnosticSpans)
        state.diagnosticMaxEnd.push_back(runningEnd = qMax(runningEnd, d.end));
    // A selection text colour other than the normal text (Legacy's white on
    // navy) replaces the syntax colours inside the selection.
    state.selectionText = theme::color(theme::Role::selectionText);
    state.recolorSelection = state.selectionText != theme::color(theme::Role::text);
    return state;
}

void TextView::paintSourceLine(QPainter &painter, int lineNo, const QPointF &origin, const QRectF &shade,
                               const QRectF &visible, const LinePaintState &state)
{
    const QTextBlock block = m_doc->findBlockByNumber(lineNo);
    LineLayout *layout = layoutForLine(lineNo);
    const qreal y = origin.y();
    paintLineBackground(painter, lineNo, shade);
    if (m_highlightCurrentLine && state.currentLines.contains(lineNo))
        painter.fillRect(shade, theme::color(theme::Role::currentLine));

    const int blockStart = block.position();
    const int blockEnd = blockStart + block.length(); // Includes the separator.

    // The wrapped rows that can reach `visible` (a line of a few hundred
    // thousand characters has some thousands of rows, a screenful shows a few
    // dozen): only these are filled, outlined and drawn. A row is
    // m_lineHeight high and rows follow one another from `y`; one more each
    // way for the rounding.
    const int rowCount = layout->lineCount();
    const int shownFirst = qBound(0, int(std::floor((visible.top() - y) / m_lineHeight)) - 1, rowCount);
    const int shownEnd = qBound(0, int(std::ceil((visible.bottom() - y) / m_lineHeight)) + 2, rowCount);
    // A line in pieces narrows that to the pieces in sight (with Word Wrap
    // off, its row is the whole line); a line laid out whole does not.
    const auto [shownStart, shownStop] =
        layout->visibleRange(shownFirst, shownEnd, visible.left() - origin.x(), visible.right() - origin.x());

    // Backgrounds are filled by hand so they span the full line height
    // (QTextLayout's own selection only covers the font height).
    const auto fillRange = [&](int start, int end, const QColor &color, bool newline) {
        for (int i = shownFirst; i < shownEnd; ++i) {
            const LineLayout::Line textLine = layout->lineAt(i);
            const int ls = textLine.textStart();
            const int le = ls + textLine.textLength();
            const bool last = i == rowCount - 1;
            const int s = qMax(start, ls);
            const int e = qMin(end, le);
            if (s > e || (s == e && !(newline && last && e == le)))
                continue;
            const int vs = qMax(s, shownStart);
            const int ve = qMin(e, shownStop);
            if (vs > ve)
                continue;
            qreal x1 = textLine.cursorToX(vs);
            qreal x2 = textLine.cursorToX(ve);
            if (x1 > x2)
                std::swap(x1, x2);
            if (newline && last && end > le && ve == e)
                x2 += m_charWidth; // Show the selected line break.
            painter.fillRect(QRectF(origin.x() + x1, y + textLine.y() - m_linePad, x2 - x1, m_lineHeight), color);
        }
    };
    // The ranges are sorted, so start at the first one reaching this line
    // (or the part of it in sight).
    const int matchesFrom = blockStart + shownStart;
    const int matchesTo = shownStop == INT_MAX ? blockEnd : qMin(blockEnd, blockStart + shownStop + 1);
    const auto firstMatch = std::lower_bound(m_searchRanges.cbegin(), m_searchRanges.cend(), matchesFrom,
                                             [](const search::Range &r, int pos) { return r.end <= pos; });
    for (int i = int(firstMatch - m_searchRanges.cbegin()); i < m_searchRanges.size(); ++i) {
        const search::Range &match = m_searchRanges.at(i);
        if (match.start >= matchesTo)
            break;
        fillRange(match.start - blockStart, match.end - blockStart,
                  i == m_searchCurrent ? searchCurrentColor() : searchColor(), false);
    }
    QList<QTextLayout::FormatRange> selectedText;
    // Carets are sorted and disjoint: only the ones reaching this line.
    const auto firstCaret = std::lower_bound(m_carets.begin(), m_carets.end(), blockStart,
                                             [](const Caret &c, int pos) { return c.end() < pos; });
    const auto endCaret = std::find_if(firstCaret, m_carets.end(), [blockEnd](const Caret &c) {
        return c.start() >= blockEnd;
    });
    for (auto it = firstCaret; it != endCaret; ++it) {
        const Caret &c = *it;
        if (!c.hasSelection())
            continue;
        fillRange(c.start() - blockStart, c.end() - blockStart, theme::color(theme::Role::selection),
                  c.end() >= blockEnd);
        if (state.recolorSelection) {
            QTextLayout::FormatRange range;
            range.start = qMax(c.start(), blockStart) - blockStart;
            range.length = qMin(c.end(), blockEnd) - blockStart - range.start;
            range.format.setForeground(state.selectionText);
            selectedText.append(range);
        }
    }

    painter.setPen(theme::color(theme::Role::text));
    // (The clip is a line taller each way: it is only to skip rows.)
    layout->draw(&painter, origin, selectedText, visible.adjusted(0, -m_lineHeight, 0, m_lineHeight));

    const auto &spans = state.diagnosticSpans;
    const auto &maxEnd = state.diagnosticMaxEnd;
    for (auto span = spans.begin() + (std::upper_bound(maxEnd.begin(), maxEnd.end(), blockStart) - maxEnd.begin());
         span != spans.end() && span->start < blockEnd; ++span) {
        const int rangeStart = span->start;
        const int rangeEnd = span->end;
        if (rangeEnd <= blockStart)
            continue;
        painter.save();
        painter.setPen(QPen(diagnosticColor(), 1));
        painter.setRenderHint(QPainter::Antialiasing, true);
        const int ds = qMax(qMax(rangeStart, blockStart) - blockStart, shownStart);
        const int de = qMin(qMin(rangeEnd, blockEnd - 1) - blockStart, shownStop);
        for (int i = shownFirst; i < shownEnd; ++i) {
            const LineLayout::Line textLine = layout->lineAt(i);
            const int s = qMax(ds, textLine.textStart());
            const int e = qMin(de, textLine.textStart() + textLine.textLength());
            if (s >= e)
                continue;
            const qreal x1 = origin.x() + textLine.cursorToX(s);
            const qreal x2 = origin.x() + textLine.cursorToX(e);
            const qreal base = y + textLine.y() + textLine.ascent() + 2.5;
            QPolygonF wave;
            bool up = false;
            for (qreal x = x1; x < x2; x += 2, up = !up)
                wave << QPointF(x, base + (up ? -1.0 : 1.0));
            wave << QPointF(x2, base);
            painter.drawPolyline(wave);
        }
        painter.restore();
    }

    if (hasFocus() && m_caretOn) {
        painter.setPen(isReadOnly() ? theme::color(theme::Role::mutedText) : theme::color(theme::Role::text));
        for (auto it = firstCaret; it != endCaret; ++it) {
            const Caret &caret = *it;
            if (caret.position < blockStart || caret.position >= blockEnd)
                continue;
            int pos = caret.position - blockStart;
            if (!m_preedit.isEmpty() && &caret == &m_carets[m_primary])
                pos += m_preedit.size();
            if (pos >= shownStart && (pos < shownStop || (pos == shownStop && pos >= layout->length())))
                layout->drawCursor(&painter, origin, pos, 2);
        }
    }
}

void TextView::paintEvent(QPaintEvent *event)
{
    ensureShownMeasured();
    if (m_columnsOn)
        return paintColumns(event);
    QScopedValueRollback painting(m_painting, true);
    QPainter painter(viewport());
    const QRect area = event->rect();
    painter.fillRect(area, theme::color(theme::Role::editor));

    const qreal left = textLeft();
    const qreal hscroll = horizontalScrollBar()->value();
    const int gutter = gutterWidth();
    const int height = viewport()->height();
    const int rows = rowCount();
    const LinePaintState state = linePaintState();

    struct VisibleLine
    {
        int line;
        qreal top;
        qreal height;
    };
    QList<VisibleLine> visibleLines;
    int paintedFirst = -1;
    int paintedLast = -1;

    painter.save();
    painter.setClipRect(QRectF(gutter, 0, viewport()->width() - gutter, height));
    qreal y = firstRowY();
    for (int r = m_topRow; r < rows && y < height; ++r) {
        // Rows below what is being painted (a caret blinking is a strip of the
        // view) change nothing in it, and a table or a picture takes a while to draw.
        if (y > area.bottom())
            break;
        const Row info = row(r);
        const qreal h = rowHeight(r);
        if (y + h < area.top()) {
            y += h;
            continue;
        }
        if (paintedFirst < 0)
            paintedFirst = info.firstLine;
        paintedLast = info.lastLine;
        if (info.rendered) {
            paintRenderedRow(painter, info, QRectF(left, y, textWidth(), h));
            y += h;
            continue;
        }
        const int lineNo = info.firstLine;
        paintSourceLine(painter, lineNo, QPointF(left - hscroll, y), QRectF(gutter, y, viewport()->width() - gutter, h),
                        QRectF(area), state);
        visibleLines.append({lineNo, y, h});
        y += h;
    }
    painter.restore();

    if (gutter > 0) {
        painter.fillRect(QRect(0, 0, gutter, height), theme::color(theme::Role::editor));
        painter.setFont(font());
        const QFontMetricsF fm(font());
        for (const VisibleLine &line : std::as_const(visibleLines)) {
            painter.setPen(state.currentLines.contains(line.line) ? theme::color(theme::Role::text)
                                                                  : theme::color(theme::Role::gutterText));
            painter.drawText(QRectF(0, line.top + m_linePad, gutter - gutterRightPad, fm.height()),
                             Qt::AlignRight | Qt::AlignVCenter, QString::number(line.line + 1));
        }
    }
    if (paintedFirst >= 0)
        trimLayouts(paintedFirst, paintedLast);
}

// ---------------------------------------------------------------------------
// Events

bool TextView::event(QEvent *event)
{
    if (event->type() == QEvent::Show) restartSettling();
    if (event->type() == QEvent::Hide) {
        m_settleTimer.stop();
        m_touchFlick.stop();
        if (m_touchMode == TouchMode::Select) {
            m_dragging = false;
            m_autoScroll.stop();
        }
        m_touchMode = TouchMode::Idle;
        m_touchTap.reset();
    }
    if (event->type() == QEvent::ShortcutOverride) {
        // Keep plain typing, Tab and editing keys in the editor even if a
        // window shortcut uses them.
        auto *key = static_cast<QKeyEvent *>(event);
        const auto mods = key->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier);
        const bool typing = !key->text().isEmpty() && key->text().at(0).isPrint() && mods == Qt::NoModifier;
        const bool editing = mods == Qt::NoModifier
                             && (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab
                                 || key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete
                                 || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter
                                 || key->key() == Qt::Key_Home || key->key() == Qt::Key_End
                                 || key->key() == Qt::Key_Left || key->key() == Qt::Key_Right
                                 || key->key() == Qt::Key_Up || key->key() == Qt::Key_Down
                                 || key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown
                                 || (key->key() == Qt::Key_Escape && m_carets.size() > 1));
        if (typing || editing) {
            event->accept();
            return true;
        }
    }
    if (event->type() == QEvent::ToolTip && !m_diagnostics.isEmpty()) {
        const auto *help = static_cast<QHelpEvent *>(event);
        qreal top = 0;
        const int index = rowAt(help->pos().y(), &top);
        if (index >= 0 && !row(index).rendered) {
            const QString message = diagnosticAt(positionAt(help->pos()));
            if (!message.isEmpty()) {
                QToolTip::showText(help->globalPos(), plainToolTip(message), viewport()); // Names link targets.
                return true;
            }
        }
        QToolTip::hideText();
        event->ignore();
        return true;
    }
    if (event->type() == QEvent::KeyPress && m_columnsOn) {
        // In columns, a key that takes the caret to another column says so.
        columnsKeyStarting(static_cast<QKeyEvent *>(event));
        const bool done = QAbstractScrollArea::event(event);
        columnsKeyDone();
        return done;
    }
    return QAbstractScrollArea::event(event);
}

void TextView::keyPressEvent(QKeyEvent *event)
{
    if (m_columnsOn && columnsKey(event))
        return;
    const auto mods = event->modifiers() & ~Qt::KeypadModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const bool ctrl = mods & Qt::ControlModifier;
    const bool alt = mods & Qt::AltModifier;
    const auto noShift = mods & ~Qt::ShiftModifier;

    if (event == QKeySequence::Copy) {
        copy();
        return;
    }
    if (event == QKeySequence::Cut) {
        cut();
        return;
    }
    if (event == QKeySequence::Paste) {
        paste();
        return;
    }
    if (event == QKeySequence::Undo) {
        undo();
        return;
    }
    if (event == QKeySequence::Redo) {
        redo();
        return;
    }
    if (event == QKeySequence::SelectAll) {
        selectAll();
        return;
    }

    switch (event->key()) {
    case Qt::Key_Left:
    case Qt::Key_Right: {
        if (alt)
            break;
        const bool left = event->key() == Qt::Key_Left;
        if (!shift && !ctrl) {
            // Collapse selections to their edge first.
            bool any = false;
            for (Caret &caret : m_carets) {
                if (caret.hasSelection()) {
                    caret.setPosition(left ? caret.start() : caret.end());
                    caret.desiredX = -1;
                    any = true;
                }
            }
            if (any) {
                caretsChanged();
                return;
            }
        }
        const auto op = ctrl ? (left ? QTextCursor::PreviousWord : QTextCursor::NextWord)
                             : (left ? QTextCursor::PreviousCharacter : QTextCursor::NextCharacter);
        moveCarets(op, shift);
        return;
    }
    case Qt::Key_Up:
    case Qt::Key_Down:
        if (alt)
            break;
        if (ctrl && noShift == Qt::ControlModifier) {
            // Scroll without moving the caret.
            verticalScrollBar()->setValue(verticalScrollBar()->value() + (event->key() == Qt::Key_Up ? -1 : 1));
            return;
        }
        moveCaretsVertically(event->key() == Qt::Key_Up ? -1 : 1, shift);
        return;
    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        const int lines = qMax(1, viewport()->height() / m_lineHeight - 1);
        moveCaretsVertically(event->key() == Qt::Key_PageUp ? -lines : lines, shift);
        return;
    }
    case Qt::Key_Home:
        if (ctrl)
            moveCarets(QTextCursor::Start, shift);
        else
            moveCaretsHome(shift);
        return;
    case Qt::Key_End:
        moveCarets(ctrl ? QTextCursor::End : QTextCursor::EndOfBlock, shift);
        return;
    case Qt::Key_Backspace:
        deleteAtCarets(ctrl ? QTextCursor::PreviousWord : QTextCursor::PreviousCharacter);
        return;
    case Qt::Key_Delete:
        if (shift && !ctrl) {
            cut();
            return;
        }
        deleteAtCarets(ctrl ? QTextCursor::NextWord : QTextCursor::NextCharacter);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (ctrl) {
            if (shift)
                insertLineAbove();
            else
                insertLineBelow();
        } else if (shift || !tableEnter()) {
            insertNewline();
        }
        return;
    case Qt::Key_Tab:
        if (!ctrl && !alt) {
            if (!tableTab(true))
                insertTab();
            return;
        }
        break;
    case Qt::Key_Backtab:
        if (!ctrl && !alt) {
            if (!tableTab(false))
                outdent();
            return;
        }
        break;
    case Qt::Key_Escape:
        if (m_carets.size() > 1 || m_carets[m_primary].hasSelection()) {
            collapseToPrimary();
            return;
        }
        break;
    case Qt::Key_Insert:
        if (shift && !ctrl) {
            paste();
            return;
        }
        if (ctrl && !shift) {
            copy();
            return;
        }
        break;
    default:
        break;
    }

    // Text input. AltGr arrives as Ctrl+Alt on Windows, so allow that pair.
    const QString text = event->text();
    const bool altGr = ctrl && alt;
    if (!text.isEmpty() && text.at(0).isPrint() && (!ctrl || altGr) && !(alt && !ctrl)) {
        if (!typeCharacter(text))
            insertTextAtCarets(text);
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void TextView::inputMethodEvent(QInputMethodEvent *event)
{
    if (!canEdit()) {
        event->ignore();
        return;
    }
    // An input method (IBus, fcitx) says "nothing composed" now and then, as
    // focus comes and goes. That changes nothing, and must not take the view
    // back to the caret from wherever it was scrolled.
    if (event->commitString().isEmpty() && event->replacementLength() == 0 && event->preeditString().isEmpty()
        && m_preedit.isEmpty())
        return;
    const int line = blockNumberOf(m_carets[m_primary].position);
    const bool typed = event->replacementLength() == 0 && typeCharacter(event->commitString());
    if (!typed && (!event->commitString().isEmpty() || event->replacementLength() > 0)) {
        beginEdit();
        const int end = m_doc->characterCount() - 1;
        editAtCarets([&](Caret &caret, QTextCursor &c) {
            if (event->replacementLength() > 0) {
                // (As QTextCursor::setPosition(): nothing outside the text.)
                if (const int from = caret.position + event->replacementStart(); from >= 0 && from <= end)
                    caret.setPosition(from);
                if (const int to = caret.position + event->replacementLength(); to >= 0 && to <= end)
                    caret.setPosition(to, QTextCursor::KeepAnchor);
                c.setPosition(caret.anchor);
                c.setPosition(caret.position, QTextCursor::KeepAnchor);
            }
            insertText(c, event->commitString());
            caret.setPosition(c.position());
        });
        endEdit();
    }
    m_preedit = event->preeditString();
    m_preeditAttributes = event->attributes();
    if (line < int(m_lines.size()))
        m_lines[line].layout.reset();
    caretsChanged();
}

QVariant TextView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    auto *self = const_cast<TextView *>(this);
    const Caret &caret = m_carets[m_primary];
    switch (query) {
    case Qt::ImEnabled:
        return !isReadOnly();
    case Qt::ImCursorRectangle:
        return self->caretRect(caret.position).toAlignedRect().translated(viewport()->pos());
    case Qt::ImFont:
        return font();
    case Qt::ImCursorPosition:
        return caret.position - m_doc->findBlock(caret.position).position();
    case Qt::ImAnchorPosition:
        return caret.anchor - m_doc->findBlock(caret.position).position();
    case Qt::ImSurroundingText:
        return m_doc->findBlock(caret.position).text();
    case Qt::ImCurrentSelection:
        return selectedTextOf(caret);
    case Qt::ImHints:
        return int(Qt::ImhMultiLine);
    default:
        return QAbstractScrollArea::inputMethodQuery(query);
    }
}

void TextView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MiddleButton && QApplication::clipboard()->supportsSelection()) {
        // Linux: paste the primary selection where the click lands.
        setFocus(Qt::MouseFocusReason);
        pasteTextAt(positionAt(event->position()), QApplication::clipboard()->text(QClipboard::Selection));
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    if (m_columnsOn) {
        hideColumnsNote(); // (A click shows where the caret is.)
        m_columns->dragShift.invalidate();
        if (columnBarPressed(event))
            return;
    }
    QPointF origin;
    const int pressedRow = rowAtPoint(event->position(), &origin);
    if (pressedRow >= 0) {
        const Row info = row(pressedRow);
        if (info.rendered && renderedRowPressed(info, event->position() - origin, event))
            return;
    }
    // Third click in a row: select the line (the paragraph in rendered text).
    if (event->pointingDevice()->type() != QInputDevice::DeviceType::TouchScreen && m_doubleClickTime.isValid()
        && m_doubleClickTime.elapsed() <= QApplication::doubleClickInterval()
        && (event->position() - m_doubleClickPos).manhattanLength() <= QApplication::startDragDistance() * 2) {
        m_doubleClickTime.invalidate();
        m_dragUnit = Unit::Line;
        m_dragging = true;
        selectByUnit(event->position(), false);
        return;
    }
    const int pos = positionAt(event->position());
    const auto mods = event->modifiers();

    if (m_markdown && pressedRow >= 0 && !row(pressedRow).rendered
        && (mods & ~Qt::KeypadModifier) == linkModifier()) {
        const QString dest = linkAtPosition(pos);
        if (!dest.isEmpty()) {
            Q_EMIT linkRequested(dest);
            return;
        }
    }

    const auto others = mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    if ((mods & Qt::ShiftModifier) && others == m_multiCursorModifier && pressedRow >= 0
        && !row(pressedRow).rendered && !m_columnsOn) { // (Not across columns.)
        // Shift+Alt+drag (Shift+Ctrl+drag): box selection. It starts from the
        // primary caret, or from where the previous box selection started.
        const int anchor = m_carets[m_primary].anchor;
        if (!(m_columnAnchorValid && captureCarets() == m_columnResult)) {
            const QTextBlock block = m_doc->findBlock(anchor);
            LineLayout *layout = layoutForLine(block.blockNumber());
            const LineLayout::Line line = layout->lineForTextPosition(anchor - block.position());
            m_columnAnchor = {block.blockNumber(), line.isValid() ? line.lineNumber() : 0, cursorX(anchor),
                              anchor - block.position()};
            m_columnAnchorValid = true;
        }
        m_columnDragging = true;
        m_dragging = true;
        m_dragUnit = Unit::Char;
        m_lastMousePos = event->position();
        columnSelectTo(event->position());
        return;
    }
    if (mods & m_multiCursorModifier && !(mods & Qt::ShiftModifier)) {
        // Alt+click (or Ctrl+click) adds a caret, or removes one already there.
        for (int i = 0; i < int(m_carets.size()); ++i) {
            if (m_carets[i].position == pos && m_carets.size() > 1) {
                m_carets.erase(m_carets.begin() + i);
                m_primary = qMin(m_primary, int(m_carets.size()) - 1);
                caretsChanged();
                return;
            }
        }
        Caret caret;
        caret.setPosition(pos);
        m_carets.push_back(caret);
        m_primary = int(m_carets.size()) - 1;
        m_dragOrigin = caret.range();
        m_dragUnit = Unit::Char;
        m_dragging = true;
        caretsChanged();
        return;
    }

    Caret caret = m_carets[m_primary];
    if (mods & Qt::ShiftModifier) {
        caret.setPosition(pos, QTextCursor::KeepAnchor);
        m_dragOrigin = {caret.anchor, caret.anchor};
    } else {
        caret.setPosition(pos);
        m_dragOrigin = caret.range();
    }
    m_dragUnit = Unit::Char;
    m_dragging = true;
    setCaretRanges({caret.range()}, 0);
}

void TextView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_dragUnit = Unit::Word;
    m_dragging = true;
    m_doubleClickTime.start();
    m_doubleClickPos = event->position();
    selectByUnit(event->position(), false);
}

void TextView::selectByUnit(const QPointF &point, bool extend)
{
    const int pos = positionAt(point);
    CaretRange target{pos, pos};
    if (m_dragUnit == Unit::Word) {
        target = wordAt(pos);
    } else if (m_dragUnit == Unit::Line) {
        const QTextCursor unit = lineUnitAt(pos);
        target = {unit.anchor(), unit.position()};
    }
    Caret &caret = m_carets[m_primary];
    if (!extend) {
        m_dragOrigin = target;
        caret.anchor = target.anchor;
        caret.position = target.position;
        caretsChanged();
        return;
    }
    // Extend from the origin unit to the unit under the mouse.
    const int start = qMin(m_dragOrigin.start(), target.start());
    const int end = qMax(m_dragOrigin.end(), target.end());
    const bool back = target.start() < m_dragOrigin.start();
    caret.anchor = back ? end : start;
    caret.position = back ? start : end;
    caretsChanged();
}

bool TextView::sourceLineAt(const QPointF &point, int *block, int *subLine)
{
    ensureShownMeasured();
    qreal y = firstRowY();
    const int rows = rowCount();
    int r = m_topRow;
    if (point.y() >= 0) {
        while (r < rows) {
            const qreal h = rowHeight(r);
            if (point.y() < y + h)
                break;
            y += h;
            ++r;
        }
    }
    if (r >= rows) {
        // Below the text: the last wrapped line of the last row.
        r = rows - 1;
        const Row last = row(r);
        if (last.rendered)
            return false;
        *block = last.lastLine;
        *subLine = layoutForLine(*block)->lineCount() - 1;
        return true;
    }
    const Row info = row(r);
    if (info.rendered)
        return false;
    LineLayout *layout = layoutForLine(info.firstLine);
    *block = info.firstLine;
    *subLine = qBound(0, int(qMax<qreal>(0, point.y() - y) / m_lineHeight), layout->lineCount() - 1);
    return true;
}

// Selects the same horizontal span on every line between the box anchor and
// the point, like VS Code: positions are the nearest to the x offsets, lines
// too short to reach the span's left edge get no selection, and if no line
// reaches it every line gets a caret at its end.
bool TextView::columnSelectTo(const QPointF &point)
{
    int toBlock = 0;
    int toSubLine = 0;
    if (!sourceLineAt(point, &toBlock, &toSubLine))
        return false;
    const qreal fromX = m_columnAnchor.x;
    const qreal toX = qMax<qreal>(0, point.x() - textLeft() + horizontalScrollBar()->value());
    const int fromBlock = m_columnAnchor.block;
    const int step = toBlock >= fromBlock ? 1 : -1;
    constexpr qreal eps = 0.01;

    struct Span
    {
        int start;
        int end;
        int block;
    };
    QList<Span> spans;
    QList<Span> lineEnds;
    for (int b = fromBlock;; b += step) {
        const QTextBlock block = m_doc->findBlockByNumber(b);
        if (!block.isValid())
            break;
        LineLayout *layout = layoutForLine(b);
        // The ends of the box sit on the wrapped lines they were made on.
        const int fromLine = b == fromBlock ? qMin(m_columnAnchor.subLine, layout->lineCount() - 1) : 0;
        const int toLine = b == toBlock ? qMin(toSubLine, layout->lineCount() - 1) : 0;
        const LineLayout::Line startLine = layout->lineAt(fromLine);
        const LineLayout::Line endLine = layout->lineAt(toLine);
        const int start = startLine.xToCursor(fromX);
        const int end = endLine.xToCursor(toX);
        const qreal visibleStart = startLine.cursorToX(start);
        const qreal visibleEnd = endLine.cursorToX(end);
        lineEnds.append({block.position() + block.length() - 1, block.position() + block.length() - 1, b});
        bool skip = false;
        if (fromX < toX - eps)
            skip = visibleStart > toX + eps || visibleEnd < fromX - eps;
        else if (fromX > toX + eps)
            skip = visibleEnd > fromX + eps || visibleStart < toX - eps;
        if (!skip)
            spans.append({block.position() + start, block.position() + end, b});
        if (b == toBlock)
            break;
    }
    const QList<Span> &chosen = spans.isEmpty() ? lineEnds : spans;
    m_carets.clear();
    for (const Span &span : chosen) {
        Caret caret;
        caret.anchor = span.start;
        caret.position = span.end;
        m_carets.push_back(caret);
    }
    // The caret on the line under the mouse is the primary one.
    m_primary = int(m_carets.size()) - 1;
    for (int i = 0; i < int(chosen.size()); ++i) {
        if (chosen[i].block == toBlock)
            m_primary = i;
    }
    m_expandHistory.clear();
    caretsChanged();
    m_columnResult = captureCarets();
    return true;
}

void TextView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_columnsOn && columnBarMoved(event))
        return;
    if (m_markdown && !m_dragging && event->buttons() == Qt::NoButton) {
        // Rendered rows are the subclass's business.
        qreal top = 0;
        const int index = rowAt(event->position().y(), &top);
        if (index >= 0 && !row(index).rendered) {
            const bool over = (event->modifiers() & ~Qt::KeypadModifier) == linkModifier()
                              && !linkAtPosition(positionAt(event->position())).isEmpty();
            viewport()->setCursor(over ? Qt::PointingHandCursor : Qt::IBeamCursor);
        }
    }
    if (!m_dragging || !(event->buttons() & Qt::LeftButton))
        return;
    m_lastMousePos = event->position();
    const QRectF bounds = viewport()->rect();
    // (Columns move under a drag past the last one from inside the viewport.)
    if (!bounds.contains(m_lastMousePos) || columnsDragDirection(m_lastMousePos) != 0) {
        if (!m_autoScroll.isActive())
            m_autoScroll.start(50, this);
    } else {
        m_autoScroll.stop();
    }
    if (m_columnDragging) {
        columnSelectTo(m_lastMousePos);
        return;
    }
    if (m_dragUnit != Unit::Char) {
        selectByUnit(m_lastMousePos, true);
        return;
    }
    const int pos = positionAt(m_lastMousePos);
    Caret &caret = m_carets[m_primary];
    caret.anchor = m_dragOrigin.position;
    caret.position = pos;
    caret.desiredX = -1;
    caretsChanged();
}

void TextView::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    if (m_columns && m_columns->barDrag.active) {
        m_columns->barDrag = {};
        viewport()->update();
    }
    m_dragging = false;
    m_columnDragging = false;
    m_autoScroll.stop();
    if (m_carets[m_primary].hasSelection() && QApplication::clipboard()->supportsSelection())
        QApplication::clipboard()->setText(selectedText(), QClipboard::Selection);
}

void TextView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        // Zoom, in whole notches: smooth wheels and touchpads send small
        // deltas that add up. Which half zooms is the tab's call, not the
        // one under the pointer.
        m_zoomDelta += event->angleDelta().y();
        const int notches = m_zoomDelta / 120;
        m_zoomDelta -= notches * 120;
        if (notches != 0)
            Q_EMIT zoomRequested(notches);
        event->accept();
        return;
    }
    if (m_columnsOn)
        return columnsWheel(event);
    if (event->angleDelta().x() != 0 && !m_wordWrap) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - event->angleDelta().x() / 2);
    }
    qreal dy = event->pixelDelta().y();
    if (dy == 0)
        dy = event->angleDelta().y() / 120.0 * 3 * m_lineHeight;
    event->accept();
    scrollByPixels(-dy);
}

bool TextView::scrollByPixels(qreal pixels)
{
    const int oldRow = m_topRow;
    const qreal oldOffset = m_topOffset;
    // Scroll by pixels across rows of different heights.
    m_topOffset += pixels;
    normalizeScroll();
    ensureShownMeasured();
    updateScrollBars();
    viewport()->update();
    const bool moved = m_topRow != oldRow || m_topOffset != oldOffset;
    Q_EMIT scrolled();
    return moved;
}

void TextView::focusInEvent(QFocusEvent *event)
{
    QAbstractScrollArea::focusInEvent(event);
    restartBlink();
    viewport()->update();
}

void TextView::focusOutEvent(QFocusEvent *event)
{
    QAbstractScrollArea::focusOutEvent(event);
    m_blink.stop();
    viewport()->update();
}

void TextView::changeEvent(QEvent *event)
{
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        updateMetrics();
        requestRelayout();
    }
}

void TextView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    // Idle measuring waits for the resizing to stop.
    if (m_settleTimer.isActive()) restartSettling();
    if (m_wordWrap)
        rowsMayHaveChanged();
    updateScrollBars();
    if (m_columnsOn) {
        hideColumnsNote();
        placeColumnControls();
        noteColumns();
    }
}

void TextView::updateBlinkingCarets()
{
    // A blink changes the carets and nothing else: repaint a strip around
    // each (the whole row where it is drawn by the row's own painter) instead of
    // the whole view, which with tables on screen is a few dozen milliseconds
    // ten times a second. Many carets, or a caret being composed in an input
    // method (drawn after its text), are not worth working out.
    constexpr int mostCarets = 32;
    if (m_carets.size() > mostCarets || !m_preedit.isEmpty() || !caretRectsKnown()) {
        viewport()->update();
        return;
    }
    QRegion region;
    for (const Caret &caret : m_carets) {
        QRectF rect;
        qreal top = 0;
        if (m_columnsOn) {
            rect = columnsCaretRect(caret.position);
        } else if (const int index = rowOfLine(blockNumberOf(caret.position)); !rowTop(index, &top)) {
            continue; // Not on the screen.
        } else if (row(index).rendered) {
            rect = QRectF(0, top, viewport()->width(), rowHeight(index));
        } else {
            rect = caretRect(caret.position);
        }
        // (A caret is a bar two pixels wide, a pixel taller than its line.)
        if (!rect.isEmpty())
            region += rect.adjusted(-2, -2, 2, 2).toAlignedRect();
    }
    if (!region.isEmpty())
        viewport()->update(region);
}

void TextView::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_settleTimer.timerId()) {
        m_settleTimer.stop();
        if (!isVisible()) return;
        ensureShownMeasured();
        QElapsedTimer slice;
        slice.start();
        std::vector<int> lines;
        for (const QTextBlock &block : std::as_const(m_lazyBlocks)) {
            if (block.isValid()) lines.push_back(block.blockNumber());
        }
        // Where the reader is: the top of the view, or the column screen's
        // start (its end too); Word Wrap off, the horizontal scroll.
        md::Position start{topLine(), int(m_topOffset)};
        md::Position end = start;
        int pageRows = 0;
        if (m_columnsOn) {
            const md::Screen &screen = m_columns->flow.screen();
            start = screen.start;
            end = screen.end;
            pageRows = m_columns->flow.geometry().columns * (m_columns->flow.geometry().height / m_lineHeight + 1);
        }
        const int near = start.row;
        std::sort(lines.begin(), lines.end(), [near](int a, int b) { return qAbs(a - near) < qAbs(b - near); });
        const auto nearRow = [&](int line) {
            if (!m_wordWrap)
                return m_columnsOn ? 0 : horizontalScrollBar()->value();
            return line == start.row ? start.y / m_lineHeight : line == end.row ? end.y / m_lineHeight : 0;
        };
        // Columns: the next and the previous page first, so turning to them
        // does not measure on the spot.
        const auto neighbourPages = [&] {
            if (!m_columnsOn || !m_wordWrap || pageRows <= 0)
                return false;
            if (LineLayout *layout = layoutForLine(end.row); layout && layout->inPieces()) {
                const int top = end.y / m_lineHeight;
                if (layout->settleWindowStep(top, top + pageRows, -2).measured)
                    return true;
            }
            if (LineLayout *layout = layoutForLine(start.row); layout && layout->inPieces()) {
                const int bottom = start.y / m_lineHeight;
                if (layout->settleWindowStep(qMax(0, bottom - pageRows), bottom, -2).measured)
                    return true;
            }
            return false;
        };
        bool pending = false;
        do {
            bool measured = neighbourPages();
            for (int i : lines) {
                if (measured) break;
                if (i < 0 || i >= int(m_lines.size())) continue;
                LineCache &cache = m_lines[i];
                if (!cache.layout || cache.wrapWidth < 0 || !cache.layout->unmeasuredCount()) continue;
                measured = cache.layout->settleOne(nearRow(i));
            }
            pending = measured;
            if (!measured) break;
        } while (slice.elapsed() < 5);
        if (pending) m_settleTimer.start(10, this);
        if (m_columnsOn && m_columns->dirty) columnsScreen();
        updateScrollBars();
    } else if (event->timerId() == m_touchFlick.timerId()) {
        touchFlickTick();
    } else if (event->timerId() == m_blink.timerId()) {
        m_caretOn = !m_caretOn;
        updateBlinkingCarets();
    } else if (event->timerId() == m_autoScroll.timerId()) {
        const qreal h = viewport()->height();
        QScrollBar *v = verticalScrollBar();
        if (m_columnsOn)
            columnsDragTick();
        else if (m_lastMousePos.y() < 0)
            v->setValue(v->value() - 1);
        else if (m_lastMousePos.y() > h)
            v->setValue(v->value() + 1);
        if (m_columnDragging) {
            columnSelectTo(QPointF(m_lastMousePos.x(), qBound<qreal>(0, m_lastMousePos.y(), h - 1)));
            return;
        }
        const int pos = positionAt(QPointF(m_lastMousePos.x(), qBound<qreal>(0, m_lastMousePos.y(), h - 1)));
        Caret &caret = m_carets[m_primary];
        caret.anchor = m_dragOrigin.position;
        caret.position = pos;
        viewport()->update();
        Q_EMIT cursorPositionChanged();
    } else {
        QAbstractScrollArea::timerEvent(event);
    }
}

void TextView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    menu.addAction(tr("Cut"), this, &TextView::cut)->setEnabled(canEdit());
    menu.addAction(tr("Copy"), this, &TextView::copy);
    menu.addAction(tr("Paste"), this, &TextView::paste)->setEnabled(canEdit());
    menu.addSeparator();
    menu.addAction(tr("Select All"), this, &TextView::selectAll);
    if (m_markdown && canEdit()) {
        menu.addSeparator();
        addInsertTableMenu(&menu, [view = QPointer<TextView>(this)](int rows, int columns) {
            if (view)
                view->insertTable(rows, columns);
        });
    }
    if (tableAtCaret().found) {
        if (!(m_markdown && canEdit()))
            menu.addSeparator();
        addTableMenu(&menu);
    }
    menu.exec(event->globalPos());
}

// ---------------------------------------------------------------------------
// Movement

void TextView::moveCarets(QTextCursor::MoveOperation op, bool select)
{
    for (Caret &caret : m_carets) {
        moveCaret(caret, op, select ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
        caret.desiredX = -1;
    }
    caretsChanged();
}

void TextView::moveCaret(Caret &caret, QTextCursor::MoveOperation op, QTextCursor::MoveMode mode) const
{
    const QTextBlock block = m_doc->findBlock(caret.position);
    const int length = block.length() - 1;
    // As QTextCursorPrivate::movePosition() has these, without a cursor (which
    // Qt places in its line first: a pass over a long line after an edit).
    switch (op) {
    case QTextCursor::Start:
        caret.setPosition(0, mode);
        return;
    case QTextCursor::End:
        caret.setPosition(m_doc->characterCount() - 1, mode);
        return;
    case QTextCursor::StartOfBlock:
        caret.setPosition(block.position(), mode);
        return;
    case QTextCursor::EndOfBlock:
        caret.setPosition(block.position() + length, mode);
        return;
    default:
        break;
    }
    const bool back = op == QTextCursor::PreviousCharacter || op == QTextCursor::PreviousWord;
    const bool words = op == QTextCursor::PreviousWord || op == QTextCursor::NextWord;
    if (!(back || words || op == QTextCursor::NextCharacter) || length <= LineLayout::piecesFrom) {
        QTextCursor cursor = cursorAt(caret);
        cursor.movePosition(op, mode);
        caret.anchor = cursor.anchor();
        caret.position = cursor.position();
        return;
    }
    // As QTextDocumentPrivate::previous/nextCursorPosition(): across the
    // line break at either end, else the block layout's answer, here a
    // layout of the text around. One that lands on an end of that text
    // (a long word) asks again with more around.
    const int pos = caret.position - block.position();
    int target = pos;
    if (back && pos == 0) {
        target = -1;
    } else if (!back && pos == length) {
        target = length + 1;
    } else {
        const auto mode = words ? QTextLayout::SkipWords : QTextLayout::SkipCharacters;
        for (int reach = 1024;; reach *= 4) {
            const int from = qMax(0, pos - reach);
            const int to = qMin(length, pos + reach);
            QString text(to - from, Qt::Uninitialized);
            for (int i = from; i < to; ++i)
                text[i - from] = m_doc->characterAt(block.position() + i);
            QTextLayout around(text);
            const int found = from + (back ? around.previousCursorPosition(pos - from, mode)
                                           : around.nextCursorPosition(pos - from, mode));
            if ((found > from || from == 0) && (found < to || to == length)) {
                target = found;
                break;
            }
        }
    }
    const int position = qBound(0, block.position() + target, m_doc->characterCount() - 1);
    if (position != caret.position)
        caret.setPosition(position, mode);
}

void TextView::moveCaretsHome(bool select)
{
    for (Caret &caret : m_carets) {
        const QTextBlock block = m_doc->findBlock(caret.position);
        const int indent = leadingWhitespace(block.text());
        const int target = caret.position - block.position() == indent ? 0 : indent;
        caret.setPosition(block.position() + target, select ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
        caret.desiredX = -1;
    }
    caretsChanged();
}

bool TextView::moveVertically(Caret &caret, int direction, QTextCursor::MoveMode mode)
{
    QTextBlock block = m_doc->findBlock(caret.position);
    LineLayout *layout = layoutForLine(block.blockNumber());
    const int pos = caret.position - block.position();
    layout->settleAround(pos);
    LineLayout::Line line = layout->lineForTextPosition(pos);
    if (caret.desiredX < 0)
        caret.desiredX = line.isValid() ? line.cursorToX(pos) : 0;

    const int target = line.lineNumber() + direction;
    if (target >= 0 && target < layout->lineCount()) {
        caret.setPosition(block.position() + layout->lineAt(target).xToCursor(caret.desiredX), mode);
        return true;
    }
    block = direction < 0 ? block.previous() : block.next();
    if (!block.isValid()) {
        caret.setPosition(direction < 0 ? 0 : m_doc->characterCount() - 1, mode);
        return false;
    }
    layout = layoutForLine(block.blockNumber());
    layout->settleAround(direction < 0 ? layout->length() : 0);
    const LineLayout::Line next = layout->lineAt(direction < 0 ? layout->lineCount() - 1 : 0);
    caret.setPosition(block.position() + next.xToCursor(caret.desiredX), mode);
    return true;
}

void TextView::moveCaretsVertically(int lines, bool select)
{
    const auto mode = select ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;
    const int direction = lines < 0 ? -1 : 1;
    for (Caret &caret : m_carets) {
        if (!select && caret.hasSelection())
            caret.setPosition(direction < 0 ? caret.start() : caret.end());
        for (int i = 0; i < qAbs(lines); ++i) {
            if (!moveVertically(caret, direction, mode))
                break;
        }
    }
    // Keep each caret's remembered x through normalizeCarets().
    std::vector<qreal> xs;
    for (const Caret &caret : m_carets)
        xs.push_back(caret.desiredX);
    const auto before = m_carets.size();
    caretsChanged();
    if (m_carets.size() == before) {
        for (size_t i = 0; i < m_carets.size(); ++i)
            m_carets[i].desiredX = xs[i];
    }
}

void TextView::addCursorAbove()
{
    // Add a caret above the topmost one (or below the bottom-most).
    auto it = std::min_element(m_carets.begin(), m_carets.end(),
                               [](const Caret &a, const Caret &b) { return a.position < b.position; });
    Caret caret;
    caret.desiredX = it->desiredX;
    caret.setPosition(it->position);
    if (!moveVertically(caret, -1, QTextCursor::MoveAnchor))
        return;
    m_carets.push_back(caret);
    m_primary = int(m_carets.size()) - 1;
    caretsChanged();
}

void TextView::addCursorBelow()
{
    auto it = std::max_element(m_carets.begin(), m_carets.end(),
                               [](const Caret &a, const Caret &b) { return a.position < b.position; });
    Caret caret;
    caret.desiredX = it->desiredX;
    caret.setPosition(it->position);
    if (!moveVertically(caret, 1, QTextCursor::MoveAnchor))
        return;
    m_carets.push_back(caret);
    m_primary = int(m_carets.size()) - 1;
    caretsChanged();
}

TextView::CaretRange TextView::wordAt(int position) const
{
    const QTextBlock block = m_doc->findBlock(position);
    const int length = block.length() - 1;
    int pos = position - block.position();
    const auto isWord = [](QChar c) { return c.isLetterOrNumber() || c == u'_'; };
    const auto at = [&](int i) { return m_doc->characterAt(block.position() + i); };
    if (pos > 0 && (pos >= length || !isWord(at(pos))) && isWord(at(pos - 1)))
        --pos;
    if (pos >= length || !isWord(at(pos))) {
        // A single non-word character is selected.
        return {position, pos < length ? block.position() + pos + 1 : position};
    }
    int start = pos;
    int end = pos;
    while (start > 0 && isWord(at(start - 1)))
        --start;
    while (end < length && isWord(at(end)))
        ++end;
    return {block.position() + start, block.position() + end};
}

TextView::CaretRange TextView::findNext(const QString &text, int from) const
{
    const auto flags = QTextDocument::FindCaseSensitively;
    QTextCursor found = m_doc->find(text, from, flags);
    if (found.isNull())
        found = m_doc->find(text, 0, flags);
    if (found.isNull())
        return {-1, -1};
    return {found.anchor(), found.position()};
}

void TextView::addNextOccurrence()
{
    // With no selections, Ctrl+D first selects the word at each caret.
    bool anySelection = false;
    for (const Caret &caret : m_carets)
        anySelection |= caret.hasSelection();
    if (!anySelection) {
        for (Caret &caret : m_carets) {
            const CaretRange word = wordAt(caret.position);
            caret.anchor = word.anchor;
            caret.position = word.position;
        }
        caretsChanged();
        return;
    }
    const Caret primary = m_carets[m_primary];
    const QString needle = selectedTextOf(primary);
    if (needle.isEmpty())
        return;
    CaretRange found = findNext(needle, primary.end());
    for (int guard = 0; found.position >= 0 && guard < int(m_carets.size()) + 1; ++guard) {
        const bool taken = std::any_of(m_carets.begin(), m_carets.end(), [&](const Caret &c) {
            return c.start() == found.start() && c.end() == found.end();
        });
        if (!taken) {
            Caret caret;
            caret.anchor = found.anchor;
            caret.position = found.position;
            m_carets.push_back(caret);
            m_primary = int(m_carets.size()) - 1;
            caretsChanged();
            return;
        }
        found = findNext(needle, found.end());
    }
}

void TextView::selectAllOccurrences()
{
    CaretRange primary = m_carets[m_primary].range();
    if (!primary.hasSelection())
        primary = wordAt(primary.position);
    const QString needle = selectedTextOf(primary);
    if (needle.isEmpty())
        return;
    QList<CaretRange> found;
    int primaryIndex = 0;
    for (QTextCursor c = m_doc->find(needle, 0, QTextDocument::FindCaseSensitively); !c.isNull();
         c = m_doc->find(needle, c.selectionEnd(), QTextDocument::FindCaseSensitively)) {
        if (c.selectionStart() == primary.start())
            primaryIndex = int(found.size());
        found.append({c.anchor(), c.position()});
    }
    setCaretRanges(found, primaryIndex);
}

void TextView::selectAll()
{
    setCaretRanges({{0, m_doc->characterCount() - 1}}, 0);
}

void TextView::selectLine()
{
    for (Caret &caret : m_carets) {
        const QTextBlock first = m_doc->findBlock(caret.start());
        const QTextBlock last = m_doc->findBlock(caret.end());
        const QTextBlock next = last.next();
        caret.anchor = first.position();
        caret.position = next.isValid() ? next.position() : m_doc->characterCount() - 1;
    }
    caretsChanged();
}

void TextView::expandSelection()
{
    m_expandHistory.append(captureCarets());
    const int primary = m_primary;
    for (Caret &c : m_carets) {
        const QTextBlock block = m_doc->findBlock(c.position);
        const int lineStart = block.position();
        const int lineEnd = lineStart + block.length() - 1;
        if (!c.hasSelection()) {
            const CaretRange word = wordAt(c.position);
            c.anchor = word.anchor;
            c.position = word.position;
            if (c.hasSelection())
                continue;
        }
        if (c.start() > lineStart || c.end() < lineEnd) {
            if (m_doc->findBlock(c.start()) == m_doc->findBlock(c.end())) {
                c.anchor = lineStart;
                c.position = lineEnd;
                continue;
            }
        }
        c.anchor = 0;
        c.position = m_doc->characterCount() - 1;
    }
    const auto history = m_expandHistory;
    m_primary = primary;
    caretsChanged();
    m_expandHistory = history;
}

void TextView::shrinkSelection()
{
    if (m_expandHistory.isEmpty())
        return;
    const CaretState previous = m_expandHistory.takeLast();
    const auto history = m_expandHistory;
    QList<CaretRange> ranges;
    for (const auto &[anchor, position] : previous.ranges)
        ranges.append({anchor, position});
    setCaretRanges(ranges, qMin(m_primary, int(ranges.size()) - 1));
    m_expandHistory = history;
}

void TextView::expandLineSelection()
{
    for (Caret &caret : m_carets) {
        const QTextBlock first = m_doc->findBlock(caret.start());
        const QTextBlock last = m_doc->findBlock(caret.end());
        // To the start of the next line, or the end of the last one.
        const QTextBlock next = last.next();
        const int end = next.isValid() ? next.position() : last.position() + last.length() - 1;
        caret.anchor = first.position();
        caret.position = end;
        caret.desiredX = -1;
    }
    caretsChanged();
}

void TextView::addCursorsToLineEnds()
{
    QList<CaretRange> result;
    const auto add = [&](int position) { result.append({position, position}); };
    for (const Caret &c : m_carets) {
        if (!c.hasSelection())
            continue;
        const QTextBlock first = m_doc->findBlock(c.start());
        const QTextBlock last = m_doc->findBlock(c.end());
        for (QTextBlock b = first; b.isValid() && b.blockNumber() < last.blockNumber(); b = b.next())
            add(b.position() + b.length() - 1);
        // The last line only gets a caret where the selection ends, unless
        // that is the start of the line.
        if (c.end() > last.position())
            add(c.end());
    }
    if (!result.isEmpty())
        setCaretRanges(result, int(result.size()) - 1);
}

void TextView::moveLastSelectionToNextMatch()
{
    Caret &last = m_carets[m_primary];
    CaretRange match;
    if (!last.hasSelection()) {
        match = wordAt(last.position);
    } else {
        const QString needle = selectedTextOf(last);
        if (needle.isEmpty())
            return;
        match = findNext(needle, last.end());
    }
    if (match.position < 0 || !match.hasSelection())
        return;
    last.anchor = match.anchor;
    last.position = match.position;
    last.desiredX = -1;
    caretsChanged();
}

QString TextView::searchSeedText() const
{
    const Caret &caret = m_carets[m_primary];
    if (caret.hasSelection()) {
        const QString text = selectedTextOf(caret);
        return text.contains(QChar::ParagraphSeparator) ? QString() : text;
    }
    const QString word = selectedTextOf(wordAt(caret.position));
    if (word.isEmpty() || !(word.at(0).isLetterOrNumber() || word.at(0) == u'_'))
        return {};
    return word;
}

void TextView::scrollPage(bool forward)
{
    if (m_columnsOn)
        return columnsPage(forward, false, false);
    // A page is the window's height less two lines, as in VS Code.
    const int lines = qMax(1, viewport()->height() / m_lineHeight - 2);
    m_topOffset += (forward ? 1 : -1) * lines * m_lineHeight;
    normalizeScroll();
    ensureShownMeasured();
    updateScrollBars();
    viewport()->update();
    Q_EMIT scrolled();
}

void TextView::pasteTextAt(int position, const QString &text)
{
    if (!canEdit() || text.isEmpty())
        return;
    const int at = qBound(0, position, m_doc->characterCount() - 1);
    setCaretRanges({{at, at}}, 0);
    QString clean = text;
    clean.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    beginEdit();
    Caret &caret = m_carets[m_primary];
    QTextCursor c = cursorAt(caret);
    insertText(c, clean);
    caret.setPosition(c.position());
    endEdit();
    caretsChanged();
}

// ---------------------------------------------------------------------------
// Caret history (cursorUndo / cursorRedo)

void TextView::resetCursorHistory()
{
    m_cursorUndo.clear();
    m_cursorRedo.clear();
    m_cursorLast = captureCarets();
    m_cursorLastRevision = m_doc->revision();
}

// Called with every caret change. As in VS Code, the state being left is
// pushed (at most 50 deep) unless it is already on top; a new push empties
// the redo stack, and any document change forgets the history.
void TextView::recordCursorHistory()
{
    if (m_doc->revision() != m_cursorLastRevision) {
        resetCursorHistory();
        return;
    }
    const CaretState current = captureCarets();
    if (current == m_cursorLast)
        return;
    if (!m_cursorHistoryPaused && (m_cursorUndo.isEmpty() || !(m_cursorUndo.last().state == m_cursorLast))) {
        m_cursorUndo.append({m_cursorLast, m_topRow, m_topOffset, horizontalScrollBar()->value()});
        m_cursorRedo.clear();
        if (m_cursorUndo.size() > 50)
            m_cursorUndo.removeFirst();
    }
    m_cursorLast = current;
}

void TextView::cursorUndo()
{
    if (m_doc->revision() != m_cursorLastRevision)
        resetCursorHistory();
    if (m_cursorUndo.isEmpty())
        return;
    applyCursorHistory(int(m_cursorUndo.size()) - 1, true);
}

void TextView::cursorRedo()
{
    if (m_doc->revision() != m_cursorLastRevision)
        resetCursorHistory();
    if (m_cursorRedo.isEmpty())
        return;
    applyCursorHistory(int(m_cursorRedo.size()) - 1, false);
}

void TextView::applyCursorHistory(int index, bool undo)
{
    QList<CursorHistoryEntry> &from = undo ? m_cursorUndo : m_cursorRedo;
    QList<CursorHistoryEntry> &to = undo ? m_cursorRedo : m_cursorUndo;
    const CursorHistoryEntry target = from.takeAt(index);
    to.append({captureCarets(), m_topRow, m_topOffset, horizontalScrollBar()->value()});
    m_cursorHistoryPaused = true;
    restoreCarets(target.state);
    m_cursorHistoryPaused = false;
    m_topRow = qBound(0, target.topRow, qMax(0, rowCount() - 1));
    m_topOffset = target.topOffset;
    normalizeScroll();
    updateScrollBars();
    horizontalScrollBar()->setValue(target.scrollX);
    viewport()->update();
    Q_EMIT scrolled();
}

// ---------------------------------------------------------------------------
// Editing

bool TextView::canEdit() const
{
    return !isReadOnly();
}

void TextView::beginEdit()
{
    if (m_editDepth == 0)
        editStarting();
    if (m_editDepth++ == 0) {
        // Entries only line up with undo steps while every change went through us.
        if (m_doc->revision() != m_syncedRevision)
            m_undoEntries.clear();
        m_editStart = captureCarets();
        m_editStartRevision = m_doc->revision();
        m_editStartSteps = m_doc->availableUndoSteps();
    }
    QTextCursor(m_doc).beginEditBlock();
}

void TextView::endEdit()
{
    QTextCursor(m_doc).endEditBlock();
    if (--m_editDepth > 0)
        return;
    const int steps = m_doc->availableUndoSteps();
    if (steps <= 0 || m_doc->revision() == m_editStartRevision)
        return;
    // Steps beyond the one before this edit were discarded.
    const bool merged = steps == m_editStartSteps && m_undoEntries.count(steps) > 0;
    m_undoEntries.erase(m_undoEntries.upper_bound(m_editStartSteps), m_undoEntries.end());
    // Typing that Qt merged into the previous step keeps that step's carets.
    UndoEntry &entry = m_undoEntries[steps];
    if (!merged)
        entry.before = m_editStart;
    entry.after = {};
    m_syncedRevision = m_doc->revision();
}

TextView::CaretState TextView::captureCarets() const
{
    CaretState state;
    state.ranges.reserve(qsizetype(m_carets.size()));
    for (const Caret &caret : m_carets)
        state.ranges.append({caret.anchor, caret.position});
    state.primary = m_primary;
    return state;
}

void TextView::restoreCarets(const CaretState &state)
{
    QList<CaretRange> ranges;
    ranges.reserve(state.ranges.size());
    for (const auto &[anchor, position] : state.ranges)
        ranges.append({anchor, position});
    setCaretRanges(ranges, state.primary);
}

void TextView::editAtCarets(const std::function<void(Caret &, QTextCursor &)> &edit)
{
    std::vector<CaretRange> spans;
    std::vector<qreal> desiredX;
    spans.reserve(m_carets.size());
    desiredX.reserve(m_carets.size());
    for (const Caret &caret : m_carets) {
        spans.push_back(caret.range());
        desiredX.push_back(caret.desiredX);
    }
    const int primary = m_primary;
    CaretSpans carets(std::move(spans));
    m_spans = &carets;
    QTextCursor cursor(m_doc); // (One for all: placed within the edit block, at no cost.)
    for (int i = 0, n = carets.size(); i < n; ++i) {
        const CaretRange now = carets.next();
        Caret work;
        work.anchor = now.anchor;
        work.position = now.position;
        work.desiredX = desiredX[i];
        cursor.setPosition(now.anchor);
        cursor.setPosition(now.position, QTextCursor::KeepAnchor);
        edit(work, cursor);
        desiredX[i] = work.desiredX;
        carets.finish(work.range());
    }
    m_spans = nullptr;
    m_carets.clear();
    m_carets.reserve(carets.size());
    for (int i = 0; i < carets.size(); ++i) {
        Caret caret;
        caret.anchor = carets.spans()[i].anchor;
        caret.position = carets.spans()[i].position;
        caret.desiredX = desiredX[i];
        m_carets.push_back(caret);
    }
    m_primary = primary;
}

void TextView::insertTextAtCarets(const QString &text)
{
    if (!canEdit())
        return;
    beginEdit();
    editAtCarets([this, &text](Caret &caret, QTextCursor &c) {
        insertText(c, text);
        caret.setPosition(c.position());
        caret.desiredX = -1;
    });
    endEdit();
    m_expandHistory.clear();
    caretsChanged();
}

void TextView::deleteAtCarets(QTextCursor::MoveOperation op)
{
    if (!canEdit())
        return;
    beginEdit();
    editAtCarets([this, op](Caret &caret, QTextCursor &c) {
        if (!c.hasSelection()) {
            if (op == QTextCursor::PreviousCharacter && deleteAutoClosedPair(caret, c)) {
                caret.setPosition(c.position());
                return;
            }
            if (op == QTextCursor::PreviousCharacter && m_insertSpaces && c.positionInBlock() > 0) {
                // In leading spaces, Backspace removes back to the previous tab stop.
                const QString text = c.block().text();
                const int pos = c.positionInBlock();
                if (leadingWhitespace(text) >= pos && text.left(pos).count(u' ') == pos) {
                    const int remove = ((pos - 1) % m_tabWidth) + 1;
                    c.setPosition(c.position() - remove, QTextCursor::KeepAnchor); // (Spaces.)
                    removeSelectedText(c);
                    caret.setPosition(c.position());
                    return;
                }
            }
            moveCaret(caret, op, QTextCursor::KeepAnchor);
            c.setPosition(caret.anchor);
            c.setPosition(caret.position, QTextCursor::KeepAnchor);
        }
        removeSelectedText(c);
        caret.setPosition(c.position());
        caret.desiredX = -1;
    });
    endEdit();
    caretsChanged();
}

QString TextView::indentUnit() const
{
    return m_insertSpaces ? QString(m_tabWidth, u' ') : QStringLiteral("\t");
}

bool TextView::continueList(QTextCursor &c)
{
    if (c.hasSelection())
        return false;
    const QString text = c.block().text();
    const md::ListLine item = md::parseListLine(text);
    if (!item.valid || c.positionInBlock() < item.contentStart)
        return false;
    if (item.empty) {
        // Enter on an empty item ends the list (or quote).
        if (c.positionInBlock() != text.size())
            return false;
        c.setPosition(c.block().position());
        c.setPosition(c.block().position() + text.size(), QTextCursor::KeepAnchor);
        insertText(c, !item.isQuote && item.lead.contains(u'>') ? item.lead : QString());
    } else {
        insertText(c, QStringLiteral("\n") + item.nextPrefix());
    }
    return true;
}

void TextView::insertNewline()
{
    if (!canEdit())
        return;
    beginEdit();
    editAtCarets([this](Caret &caret, QTextCursor &c) {
        if (!(m_markdown && continueList(c))) {
            const QString text = c.block().text();
            const QString indent = text.left(qMin(leadingWhitespace(text), c.positionInBlock()));
            insertText(c, QStringLiteral("\n") + indent);
        }
        caret.setPosition(c.position());
        caret.desiredX = -1;
    });
    endEdit();
    caretsChanged();
}

void TextView::insertTab()
{
    if (!canEdit())
        return;
    for (const Caret &caret : m_carets) {
        if (caret.hasSelection()) {
            indent();
            return;
        }
    }
    beginEdit();
    QSet<int> indentedItems;
    editAtCarets([&](Caret &caret, QTextCursor &c) {
        const md::ListLine item = m_markdown ? md::parseListLine(c.block().text()) : md::ListLine();
        if (item.valid && !item.isQuote) {
            // Tab in a list item indents the whole item.
            const QTextBlock block = c.block();
            if (indentedItems.contains(block.blockNumber()))
                return;
            indentedItems.insert(block.blockNumber());
            const QString unit = indentUnit();
            const int anchor = c.anchor() + unit.size();
            const int position = c.position() + unit.size();
            QTextCursor start(block);
            insertText(start, unit);
            caret.anchor = anchor;
            caret.position = position;
            return;
        }
        if (m_insertSpaces) {
            removeSelectedText(c);
            const int column = c.positionInBlock();
            insertText(c, QString(m_tabWidth - column % m_tabWidth, u' '));
        } else {
            insertText(c, QStringLiteral("\t"));
        }
        caret.setPosition(c.position());
    });
    endEdit();
    caretsChanged();
}

namespace {

using CharPair = std::pair<QChar, QChar>;

// VS Code's Markdown surroundingPairs, and the default set for other files.
const QList<CharPair> &surroundingPairs(bool markdown)
{
    static const QList<CharPair> md = {{u'(', u')'}, {u'[', u']'}, {u'`', u'`'}, {u'_', u'_'}, {u'*', u'*'},
                                       {u'{', u'}'}, {u'\'', u'\''}, {u'"', u'"'}, {u'<', u'>'}, {u'~', u'~'},
                                       {u'$', u'$'}};
    static const QList<CharPair> other = {{u'(', u')'}, {u'[', u']'}, {u'{', u'}'}, {u'<', u'>'},
                                          {u'"', u'"'}, {u'\'', u'\''}, {u'`', u'`'}};
    return markdown ? md : other;
}

// Markdown follows markdown-basics, code gets brackets and quotes, plain
// text has no auto-closing, like VS Code.
const QList<CharPair> &autoClosingPairs(bool markdown, bool code)
{
    static const QList<CharPair> md = {{u'{', u'}'}, {u'[', u']'}, {u'(', u')'}, {u'<', u'>'}};
    static const QList<CharPair> other = {{u'{', u'}'}, {u'[', u']'}, {u'(', u')'},
                                          {u'"', u'"'}, {u'\'', u'\''}, {u'`', u'`'}};
    static const QList<CharPair> none;
    return markdown ? md : code ? other : none;
}

bool isQuoteChar(QChar ch)
{
    return ch == u'"' || ch == u'\'' || ch == u'`';
}

} // namespace

bool TextView::typeCharacter(const QString &text)
{
    if (!canEdit() || text.size() != 1 || text.at(0).unicode() >= 128 || !text.at(0).isPrint())
        return false;
    const QChar ch = text.at(0);
    auto &settings = Settings::instance();
    if (settings.flag(key::autoSurround) && surroundSelections(ch))
        return true;
    if (settings.flag(key::autoClose))
        return overtypeClosers(ch) || autoCloseBrackets(ch);
    return false;
}

bool TextView::surroundSelections(QChar open)
{
    QChar close;
    for (const CharPair &pair : surroundingPairs(m_markdown)) {
        if (pair.first == open)
            close = pair.second;
    }
    if (close.isNull())
        return false;
    for (const Caret &caret : m_carets) {
        if (!caret.hasSelection())
            return false;
        const QString selected = selectedTextOf(caret);
        bool onlyWhitespace = true;
        for (QChar sc : selected) {
            if (sc != u' ' && sc != u'\t' && sc != QChar::ParagraphSeparator) {
                onlyWhitespace = false;
                break;
            }
        }
        if (onlyWhitespace)
            return false;
        // A quote typed over a single quote replaces it.
        if (isQuoteChar(open) && selected.size() == 1 && isQuoteChar(selected.at(0)))
            return false;
    }
    beginEdit();
    editAtCarets([&](Caret &caret, QTextCursor &edit) {
        const bool forward = caret.position > caret.anchor;
        const int start = caret.start();
        const int end = caret.end();
        edit.setPosition(end);
        insertText(edit, close);
        edit.setPosition(start);
        insertText(edit, open);
        // Keep the wrapped text selected so the key can be repeated.
        caret.anchor = forward ? start + 1 : end + 1;
        caret.position = forward ? end + 1 : start + 1;
        caret.desiredX = -1;
    });
    endEdit();
    m_expandHistory.clear();
    caretsChanged();
    return true;
}

bool TextView::overtypeClosers(QChar ch)
{
    if (m_autoClosed.isEmpty())
        return false;
    QList<qsizetype> used;
    for (const Caret &caret : m_carets) {
        if (caret.hasSelection())
            return false;
        const int pos = caret.position;
        if (m_doc->characterAt(pos) != ch)
            return false;
        // Quotes after a backslash are escaped, not closers.
        if (isQuoteChar(ch) && pos > 0 && m_doc->characterAt(pos - 1) == u'\\')
            return false;
        qsizetype found = -1;
        for (qsizetype i = 0; i < m_autoClosed.size(); ++i) {
            if (m_autoClosed.at(i).closer.start() == pos) {
                found = i;
                break;
            }
        }
        if (found < 0)
            return false;
        used.append(found);
    }
    std::sort(used.begin(), used.end(), std::greater<>());
    for (qsizetype i : used)
        m_autoClosed.removeAt(i);
    for (Caret &caret : m_carets) {
        caret.setPosition(caret.position + 1);
        caret.desiredX = -1;
    }
    caretsChanged();
    return true;
}

bool TextView::autoCloseBrackets(QChar open)
{
    const auto &pairs = autoClosingPairs(m_markdown, m_codeEditing);
    QChar close;
    for (const CharPair &pair : pairs) {
        if (pair.first == open)
            close = pair.second;
    }
    if (close.isNull())
        return false;
    const bool quote = isQuoteChar(open);
    // VS Code's languageDefined auto-close-before characters.
    const QString before = quote ? QStringLiteral(";:.,=}])> \n\t") : QStringLiteral("'\"`;:.,=}])> \n\t");
    for (const Caret &caret : m_carets) {
        if (caret.hasSelection())
            return false;
        const QTextBlock block = m_doc->findBlock(caret.position);
        const QString line = block.text();
        const int col = caret.position - block.position();
        if (col < line.size()) {
            const QChar next = line.at(col);
            bool isCloser = false;
            for (const CharPair &pair : pairs)
                isCloser = isCloser || pair.second == next;
            if (!isCloser && !before.contains(next))
                return false;
        }
        // Not after a word character for ' and ", which are often apostrophes or closers.
        if ((open == u'\'' || open == u'"') && col > 0) {
            const QChar prev = line.at(col - 1);
            if (prev.isLetterOrNumber() || prev == u'_')
                return false;
        }
    }
    beginEdit();
    editAtCarets([&](Caret &caret, QTextCursor &c) {
        insertText(c, QString(open) + close);
        const int inner = c.position() - 1; // Before the closer.
        caret.setPosition(inner);
        m_autoClosed.append(AutoClosed{inner, {inner, inner + 1}});
        caret.desiredX = -1;
    });
    endEdit();
    m_expandHistory.clear();
    caretsChanged();
    return true;
}

bool TextView::deleteAutoClosedPair(Caret &caret, QTextCursor &c)
{
    if (m_autoClosed.isEmpty() || !Settings::instance().flag(key::autoClose))
        return false;
    const int pos = c.position();
    if (pos == 0)
        return false;
    for (qsizetype i = 0; i < m_autoClosed.size(); ++i) {
        if (m_autoClosed.at(i).closer.start() != pos)
            continue;
        const QChar open = m_doc->characterAt(pos - 1);
        const QChar close = m_doc->characterAt(pos);
        for (const CharPair &pair : autoClosingPairs(m_markdown, m_codeEditing)) {
            if (pair.first == open && pair.second == close) {
                m_autoClosed.removeAt(i);
                c.setPosition(pos - 1);
                c.setPosition(pos + 1, QTextCursor::KeepAnchor);
                removeSelectedText(c);
                caret.desiredX = -1;
                return true;
            }
        }
        return false;
    }
    return false;
}

void TextView::pruneAutoClosed()
{
    for (qsizetype i = m_autoClosed.size() - 1; i >= 0; --i) {
        const AutoClosed &entry = m_autoClosed.at(i);
        bool keep = entry.closer.end() - entry.closer.start() == 1;
        if (keep) {
            // Only while a caret stays between the opener and the closer.
            keep = false;
            for (const Caret &caret : m_carets) {
                const int pos = caret.position;
                if (!caret.hasSelection() && pos >= entry.inner && pos <= entry.closer.start()) {
                    keep = true;
                    break;
                }
            }
        }
        if (!keep)
            m_autoClosed.removeAt(i);
    }
}

QList<int> TextView::caretLines() const
{
    QList<int> lines;
    for (const auto &[first, last] : caretLineRanges()) {
        for (int line = first; line <= last; ++line)
            lines.append(line);
    }
    return lines;
}

QList<std::pair<int, int>> TextView::caretLineRanges() const
{
    QList<std::pair<int, int>> ranges;
    for (const Caret &c : m_carets) {
        const QTextBlock startBlock = m_doc->findBlock(c.start());
        QTextBlock endBlock = m_doc->findBlock(c.end());
        // A selection ending at column 0 does not include that line.
        if (c.hasSelection() && endBlock != startBlock && c.end() == endBlock.position())
            endBlock = endBlock.previous();
        ranges.append({startBlock.blockNumber(), endBlock.blockNumber()});
    }
    std::sort(ranges.begin(), ranges.end());
    QList<std::pair<int, int>> merged;
    for (const auto &range : ranges) {
        if (!merged.isEmpty() && range.first <= merged.last().second + 1)
            merged.last().second = qMax(merged.last().second, range.second);
        else
            merged.append(range);
    }
    return merged;
}

void TextView::indent()
{
    if (!canEdit())
        return;
    const QString unit = indentUnit();
    beginEdit();
    for (int line : caretLines()) {
        const QTextBlock block = m_doc->findBlockByNumber(line);
        if (block.text().isEmpty())
            continue;
        QTextCursor c(block);
        insertText(c, unit);
    }
    endEdit();
    caretsChanged();
}

void TextView::outdent()
{
    if (!canEdit())
        return;
    beginEdit();
    for (int line : caretLines()) {
        const QTextBlock block = m_doc->findBlockByNumber(line);
        const QString text = block.text();
        int remove = 0;
        if (!text.isEmpty() && text.at(0) == u'\t')
            remove = 1;
        else
            while (remove < m_tabWidth && remove < text.size() && text.at(remove) == u' ')
                ++remove;
        if (remove == 0)
            continue;
        QTextCursor c(block);
        c.setPosition(c.position() + remove, QTextCursor::KeepAnchor); // (Spaces or a tab.)
        removeSelectedText(c);
    }
    endEdit();
    caretsChanged();
}

void TextView::trimTrailingWhitespace()
{
    if (!canEdit())
        return;
    beginEdit();
    for (QTextBlock block = m_doc->begin(); block.isValid(); block = block.next()) {
        const QString text = block.text();
        int keep = text.size();
        while (keep > 0 && (text.at(keep - 1) == u' ' || text.at(keep - 1) == u'\t'))
            --keep;
        if (keep == text.size())
            continue;
        QTextCursor c(m_doc);
        c.setPosition(block.position() + keep);
        c.setPosition(block.position() + text.size(), QTextCursor::KeepAnchor);
        removeSelectedText(c);
    }
    endEdit();
    caretsChanged();
}

void TextView::toggleLineComment()
{
    if (!canEdit() || m_commentMarker.isEmpty())
        return;
    const QList<int> lines = caretLines();
    bool allCommented = true;
    int minIndent = INT_MAX;
    for (int line : lines) {
        const QString text = m_doc->findBlockByNumber(line).text();
        const int indent = leadingWhitespace(text);
        if (indent == text.size())
            continue;
        minIndent = qMin(minIndent, indent);
        if (!QStringView(text).mid(indent).startsWith(m_commentMarker))
            allCommented = false;
    }
    if (minIndent == INT_MAX)
        return;
    beginEdit();
    for (int line : lines) {
        const QTextBlock block = m_doc->findBlockByNumber(line);
        const QString text = block.text();
        const int indent = leadingWhitespace(text);
        if (indent == text.size())
            continue;
        QTextCursor c(block);
        if (allCommented) {
            int remove = m_commentMarker.size();
            if (indent + remove < text.size() && text.at(indent + remove) == u' ')
                ++remove;
            c.setPosition(block.position() + indent);
            c.setPosition(block.position() + indent + remove, QTextCursor::KeepAnchor);
            removeSelectedText(c);
        } else {
            c.setPosition(block.position() + minIndent);
            insertText(c, m_commentMarker + u' ');
        }
    }
    endEdit();
    caretsChanged();
}

void TextView::deleteLines()
{
    if (!canEdit())
        return;
    const auto ranges = caretLineRanges();
    beginEdit();
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        QTextCursor c(m_doc->findBlockByNumber(it->first));
        const QTextBlock last = m_doc->findBlockByNumber(it->second);
        if (last.next().isValid()) {
            c.setPosition(last.next().position(), QTextCursor::KeepAnchor);
        } else if (c.block().previous().isValid()) {
            // Last line: also remove the newline before it.
            c.setPosition(c.block().previous().position() + c.block().previous().length() - 1);
            c.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
        } else {
            c.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
        }
        removeSelectedText(c);
    }
    endEdit();
    for (Caret &caret : m_carets)
        caret.clearSelection();
    caretsChanged();
}

void TextView::insertLineBelow()
{
    if (!canEdit())
        return;
    beginEdit();
    // (Each caret as a cursor would be: moved by the edits made before it.)
    for (size_t i = 0; i < m_carets.size(); ++i) {
        QTextCursor c = cursorAt(m_carets[i]);
        const QString text = c.block().text();
        c.clearSelection();
        c.movePosition(QTextCursor::EndOfBlock);
        insertText(c, QStringLiteral("\n") + text.left(leadingWhitespace(text)));
        m_carets[i].setPosition(c.position());
    }
    endEdit();
    caretsChanged();
}

void TextView::insertLineAbove()
{
    if (!canEdit())
        return;
    beginEdit();
    for (size_t i = 0; i < m_carets.size(); ++i) {
        QTextCursor c = cursorAt(m_carets[i]);
        const QString indent = c.block().text().left(leadingWhitespace(c.block().text()));
        c.clearSelection();
        c.movePosition(QTextCursor::StartOfBlock);
        insertText(c, indent + QStringLiteral("\n"));
        c.movePosition(QTextCursor::PreviousBlock);
        c.movePosition(QTextCursor::EndOfBlock);
        m_carets[i].setPosition(c.position());
    }
    endEdit();
    caretsChanged();
}

void TextView::moveLinesUp()
{
    moveLines(-1);
}

void TextView::moveLinesDown()
{
    moveLines(1);
}

void TextView::copyLinesUp()
{
    copyLines(-1);
}

void TextView::copyLinesDown()
{
    copyLines(1);
}

void TextView::moveLines(int direction)
{
    if (!canEdit())
        return;
    const auto ranges = caretLineRanges();
    if (ranges.isEmpty())
        return;
    if (direction < 0 && ranges.first().first == 0)
        return;
    if (direction > 0 && ranges.last().second >= m_doc->blockCount() - 1)
        return;

    // Remember carets as offsets from their range's start so they can be
    // restored after the text moves.
    struct Saved
    {
        int range;
        int anchor;
        int position;
    };
    std::vector<Saved> saved;
    for (const Caret &caret : m_carets) {
        const int line = blockNumberOf(caret.position);
        for (int i = 0; i < ranges.size(); ++i) {
            const int start = m_doc->findBlockByNumber(ranges[i].first).position();
            const int anchorLine = blockNumberOf(caret.anchor);
            if ((line >= ranges[i].first && line <= ranges[i].second)
                || (anchorLine >= ranges[i].first && anchorLine <= ranges[i].second)) {
                saved.push_back({i, caret.anchor - start, caret.position - start});
                break;
            }
        }
    }

    beginEdit();
    QList<int> newStarts(ranges.size());
    const auto moveRange = [&](int i) {
        const auto [first, last] = ranges[i];
        // Swap the range with the neighbouring line.
        const int neighbour = direction < 0 ? first - 1 : last + 1;
        const QTextBlock nb = m_doc->findBlockByNumber(neighbour);
        const QString neighbourText = nb.text();
        QTextCursor c(nb);
        // Remove the neighbour line including one newline.
        if (direction < 0) {
            c.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor);
        } else {
            c.setPosition(nb.previous().position() + nb.previous().length() - 1);
            c.setPosition(nb.position() + nb.length() - 1, QTextCursor::KeepAnchor);
        }
        removeSelectedText(c);
        // Re-insert it on the other side of the range.
        if (direction < 0) {
            const QTextBlock lastBlock = m_doc->findBlockByNumber(last - 1);
            QTextCursor ins(m_doc);
            ins.setPosition(lastBlock.position() + lastBlock.length() - 1);
            insertText(ins, QStringLiteral("\n") + neighbourText);
            newStarts[i] = m_doc->findBlockByNumber(first - 1).position();
        } else {
            QTextCursor ins(m_doc->findBlockByNumber(first));
            insertText(ins, neighbourText + QStringLiteral("\n"));
            newStarts[i] = m_doc->findBlockByNumber(first + 1).position();
        }
    };
    if (direction < 0) {
        for (int i = 0; i < ranges.size(); ++i)
            moveRange(i);
    } else {
        for (int i = int(ranges.size()) - 1; i >= 0; --i)
            moveRange(i);
    }
    endEdit();

    std::vector<Caret> carets;
    const int end = m_doc->characterCount() - 1;
    for (const Saved &s : saved) {
        // (As QTextCursor::setPosition(): a place outside the text is not taken.)
        Caret caret;
        if (const int anchor = newStarts[s.range] + s.anchor; anchor >= 0 && anchor <= end)
            caret.setPosition(anchor);
        if (const int position = newStarts[s.range] + s.position; position >= 0 && position <= end)
            caret.setPosition(position, QTextCursor::KeepAnchor);
        carets.push_back(caret);
    }
    if (!carets.empty()) {
        const int primary = carets[qMin(m_primary, int(carets.size()) - 1)].position;
        m_carets = std::move(carets);
        m_primary = 0;
        for (int i = 0; i < int(m_carets.size()); ++i) {
            if (m_carets[i].position == primary)
                m_primary = i;
        }
    }
    caretsChanged();
}

void TextView::copyLines(int direction)
{
    if (!canEdit())
        return;
    const auto ranges = caretLineRanges();
    beginEdit();
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        const QTextBlock firstBlock = m_doc->findBlockByNumber(it->first);
        const QTextBlock lastBlock = m_doc->findBlockByNumber(it->second);
        QTextCursor range(m_doc);
        range.setPosition(firstBlock.position());
        range.setPosition(lastBlock.position() + lastBlock.length() - 1, QTextCursor::KeepAnchor);
        const QString text = range.selectedText().replace(QChar::ParagraphSeparator, u'\n');
        // Insert the copy after the range; for "up" the carets then move to
        // the original (upper) copy by shifting back.
        QTextCursor ins(m_doc);
        ins.setPosition(lastBlock.position() + lastBlock.length() - 1);
        insertText(ins, QStringLiteral("\n") + text);
    }
    endEdit();
    if (direction > 0) {
        // Carets follow to the lower copy.
        for (Caret &caret : m_carets) {
            const QTextBlock block = m_doc->findBlock(caret.position);
            const auto range = std::find_if(ranges.begin(), ranges.end(), [&](const auto &r) {
                const int line = block.blockNumber();
                return line >= r.first && line <= r.second;
            });
            if (range == ranges.end())
                continue;
            const int lines = range->second - range->first + 1;
            const QTextBlock anchorBlock = m_doc->findBlock(caret.anchor);
            const int anchorCol = caret.anchor - anchorBlock.position();
            const int posCol = caret.position - block.position();
            caret.anchor = m_doc->findBlockByNumber(anchorBlock.blockNumber() + lines).position() + anchorCol;
            caret.position = m_doc->findBlockByNumber(block.blockNumber() + lines).position() + posCol;
        }
    }
    caretsChanged();
}

// ---------------------------------------------------------------------------
// Clipboard and undo

void TextView::copy()
{
    bool anySelection = false;
    for (const Caret &caret : m_carets)
        anySelection |= caret.hasSelection();
    auto *mime = new QMimeData;
    if (anySelection) {
        mime->setText(selectedText());
    } else {
        // No selection: copy the caret lines, VS Code style.
        QStringList lines;
        for (int line : caretLines())
            lines.append(m_doc->findBlockByNumber(line).text());
        mime->setText(lines.join(u'\n') + u'\n');
        mime->setData(wholeLinesMime, QByteArray("1"));
    }
    if (m_markdown && !m_documentPath.isEmpty() && Settings::instance().flag(key::updateLinksOnPaste))
        mime->setData(markdownSourceMime, m_documentPath.toUtf8());
    QApplication::clipboard()->setMimeData(mime);
}

void TextView::cut()
{
    if (!canEdit()) {
        copy();
        return;
    }
    bool anySelection = false;
    for (const Caret &caret : m_carets)
        anySelection |= caret.hasSelection();
    copy();
    if (!anySelection) {
        deleteLines();
        return;
    }
    beginEdit();
    editAtCarets([this](Caret &caret, QTextCursor &c) {
        removeSelectedText(c);
        caret.setPosition(c.position());
    });
    endEdit();
    caretsChanged();
}

void TextView::paste()
{
    if (!canEdit())
        return;
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (!mime)
        return;
    if (m_markdown) {
        const QStringList files = localFiles(mime);
        if (!files.isEmpty()) {
            insertFileLinks(files);
            return;
        }
        if (mime->hasImage() && !mime->hasText()) {
            pasteImage(qvariant_cast<QImage>(mime->imageData()));
            return;
        }
    }
    if (!mime->hasText())
        return;
    QString text = mime->text();
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (m_markdown && !m_documentPath.isEmpty() && mime->hasFormat(markdownSourceMime)
        && Settings::instance().flag(key::updateLinksOnPaste)) {
        // Relative links keep pointing at the same files from the new folder.
        const QString from = QFileInfo(QString::fromUtf8(mime->data(markdownSourceMime))).absolutePath();
        const QString to = QFileInfo(m_documentPath).absolutePath();
        if (from != to)
            text = md::rebaseLinks(text, from, to);
    }
    const bool wholeLines = mime->hasFormat(wholeLinesMime);

    QStringList parts;
    if (m_carets.size() > 1) {
        QString trimmed = text;
        if (trimmed.endsWith(u'\n'))
            trimmed.chop(1);
        parts = trimmed.split(u'\n');
        if (parts.size() != int(m_carets.size()))
            parts.clear();
    }

    beginEdit();
    // (The carets are in document order, so the parts go in that order.)
    int k = 0;
    editAtCarets([&](Caret &caret, QTextCursor &c) {
        if (!parts.isEmpty()) {
            insertText(c, parts.at(k));
            caret.setPosition(c.position());
        } else if (wholeLines && !c.hasSelection()) {
            // Whole lines go above the caret's line (the caret stays on it).
            QTextCursor lineStart(c.block());
            const int at = lineStart.position();
            insertText(lineStart, text);
            caret.setPosition(caret.position + lineStart.position() - at);
        } else {
            insertText(c, text);
            caret.setPosition(c.position());
        }
        ++k;
    });
    endEdit();
    caretsChanged();
}

void TextView::insertFileLinks(const QStringList &files)
{
    const QString dir = m_documentPath.isEmpty() ? QString() : QFileInfo(m_documentPath).absolutePath();
    QStringList links;
    for (const QString &file : files)
        links.append(md::linkForFile(file, dir));
    insertTextAtCarets(links.join(u'\n'));
}

void TextView::pasteImage(const QImage &image)
{
    if (image.isNull())
        return;
    if (m_documentPath.isEmpty()) {
        QMessageBox::information(this, QApplication::applicationDisplayName(),
                                 tr("Save the document before pasting images."));
        return;
    }
    const QString documentDir = QFileInfo(m_documentPath).absolutePath();
    const QString folder = Settings::instance().text(key::pasteImageFolder).trimmed();
    const QString targetDir = folder.isEmpty()             ? documentDir
                              : QDir::isAbsolutePath(folder) ? folder
                                                             : QDir(documentDir).filePath(folder);
    if (!QDir().mkpath(targetDir)) {
        plainWarning(this, QApplication::applicationDisplayName(),
                     tr("Could not create the folder %1.").arg(QDir::toNativeSeparators(targetDir)));
        return;
    }
    const QString base = QStringLiteral("image-") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString path = QDir(targetDir).filePath(base + QStringLiteral(".png"));
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = QDir(targetDir).filePath(base + u'-' + QString::number(n) + QStringLiteral(".png"));
    if (!image.save(path, "PNG")) {
        plainWarning(this, QApplication::applicationDisplayName(),
                     tr("Could not save the image to %1.").arg(QDir::toNativeSeparators(path)));
        return;
    }
    insertTextAtCarets(md::linkForFile(path, documentDir));
}

Qt::KeyboardModifier TextView::linkModifier() const
{
    return m_multiCursorModifier == Qt::ControlModifier ? Qt::AltModifier : Qt::ControlModifier;
}

QString TextView::linkAtPosition(int position)
{
    if (m_linkRevision != m_doc->revision()) {
        QString text = m_doc->toRawText();
        text.replace(QChar::ParagraphSeparator, u'\n');
        m_linkSpans = md::linkTargets(text);
        m_linkRevision = m_doc->revision();
    }
    const QTextBlock block = m_doc->findBlock(position);
    return md::linkAt(m_linkSpans, block.text(), block.position(), position);
}

void TextView::dragEnterEvent(QDragEnterEvent *event)
{
    if (m_markdown && canEdit() && !localFiles(event->mimeData()).isEmpty())
        event->acceptProposedAction();
    else
        event->ignore();
}

void TextView::dragMoveEvent(QDragMoveEvent *event)
{
    if (m_markdown && canEdit() && !localFiles(event->mimeData()).isEmpty())
        event->acceptProposedAction();
    else
        event->ignore();
}

void TextView::dropEvent(QDropEvent *event)
{
    const QStringList files = localFiles(event->mimeData());
    if (!m_markdown || !canEdit() || files.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    // Like VS Code: a plain drop opens the files, Shift+drop links them.
    if (!(event->modifiers() & Qt::ShiftModifier)) {
        Q_EMIT filesDropped(files);
        return;
    }
    QTextCursor cursor(m_doc);
    cursor.setPosition(positionAt(event->position()));
    setTextCursor(cursor);
    insertFileLinks(files);
    setFocus(Qt::MouseFocusReason);
}

// QTextDocument::undo(QTextCursor *) reports a cursor position that is not
// the edit (it can be 0), so collect the document's own change notifications
// instead and put a caret at the end of every changed range.
void TextView::undoRedo(bool undo)
{
    if (!canEdit() || !(undo ? m_doc->isUndoAvailable() : m_doc->isRedoAvailable()))
        return;
    // Prefer the carets recorded around the step by beginEdit()/endEdit(),
    // as long as no change bypassed them.
    if (m_doc->revision() != m_syncedRevision)
        m_undoEntries.clear();
    editStarting();
    const int steps = m_doc->availableUndoSteps();
    // Entries are keyed by the undo position right after their step; redo's
    // key is only known once it has run.
    const auto found = undo ? m_undoEntries.find(steps) : m_undoEntries.end();
    UndoEntry *entry = found != m_undoEntries.end() ? &found->second : nullptr;
    const CaretState current = captureCarets();

    QList<int> positions;
    const QMetaObject::Connection connection =
        connect(m_doc, &QTextDocument::contentsChange, this, [&positions](int from, int removed, int added) {
            for (int &p : positions) {
                if (p >= from + removed)
                    p += added - removed;
                else if (p > from)
                    p = from;
            }
            positions.append(from + added);
        });
    // The other positions kept go along with the undo (see Journal); this
    // view's carets are put where they go below.
    m_journal->undoRedo(this, undo);
    disconnect(connection);
    m_syncedRevision = m_doc->revision();
    if (!undo) {
        const auto it = m_undoEntries.find(m_doc->availableUndoSteps());
        entry = it != m_undoEntries.end() ? &it->second : nullptr;
    }

    if (entry && !(undo ? entry->before : entry->after).ranges.isEmpty()) {
        // Redo needs the state we are leaving.
        const CaretState target = undo ? entry->before : entry->after;
        if (undo)
            entry->after = current;
        restoreCarets(target);
        return;
    }
    if (positions.isEmpty()) {
        // Nothing changed: the carets are as they were.
        for (Caret &caret : m_carets)
            caret.desiredX = -1;
        return;
    }
    QList<CaretRange> carets;
    for (const int p : std::as_const(positions))
        carets.append({p, p});
    setCaretRanges(carets, 0);
}

void TextView::undo()
{
    undoRedo(true);
}

void TextView::redo()
{
    undoRedo(false);
}

} // namespace vn
