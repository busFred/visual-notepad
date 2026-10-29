#include "TextView.h"

#include "../Settings.h"
#include "../Theme.h"
#include "DocumentLayout.h"

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
#include <QtMath>

#include <algorithm>
#include <climits>

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

bool lessByPosition(const QTextCursor &a, const QTextCursor &b)
{
    return a.selectionStart() < b.selectionStart();
}

} // namespace

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
    viewport()->setAutoFillBackground(false);
    setFont(theme::editorFont());

    m_lines.resize(m_doc->blockCount());
    m_carets.push_back({QTextCursor(m_doc), -1});
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

TextView::~TextView() = default;

// ---------------------------------------------------------------------------
// Options

void TextView::setWordWrap(bool on)
{
    if (m_wordWrap == on)
        return;
    m_wordWrap = on;
    horizontalScrollBar()->setValue(0);
    relayout();
}

void TextView::setLineNumbersVisible(bool on)
{
    m_lineNumbers = on;
    relayout();
}

void TextView::setShowWhitespace(bool on)
{
    m_showWhitespace = on;
    relayout();
}

void TextView::setHighlightCurrentLine(bool on)
{
    m_highlightCurrentLine = on;
    viewport()->update();
}

void TextView::setTabWidth(int columns)
{
    m_tabWidth = qMax(1, columns);
    relayout();
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
    m_lineSpacing = qBound(1.0, factor, 3.0);
    updateMetrics();
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

QTextCursor TextView::textCursor() const
{
    return m_carets[m_primary].cursor;
}

void TextView::setTextCursor(const QTextCursor &cursor)
{
    setCursors({cursor}, 0);
}

bool TextView::hasSelection() const
{
    for (const Caret &caret : m_carets) {
        if (caret.cursor.hasSelection())
            return true;
    }
    return false;
}

void TextView::mirrorCursorsFrom(const TextView *other, bool reveal)
{
    if (!other || other == this || other->m_doc != m_doc)
        return;
    m_carets.clear();
    for (const Caret &caret : other->m_carets)
        m_carets.push_back({caret.cursor, -1});
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
        result.append(caret.cursor);
    return result;
}

void TextView::setCursors(const QList<QTextCursor> &cursors, int primary)
{
    if (cursors.isEmpty())
        return;
    m_carets.clear();
    for (const QTextCursor &cursor : cursors)
        m_carets.push_back({cursor, -1});
    m_primary = qBound(0, primary, int(m_carets.size()) - 1);
    m_expandHistory.clear();
    caretsChanged();
}

QString TextView::selectedText() const
{
    QList<QTextCursor> sorted = cursors();
    std::sort(sorted.begin(), sorted.end(), lessByPosition);
    QStringList parts;
    for (const QTextCursor &cursor : sorted) {
        if (cursor.hasSelection())
            parts.append(cursor.selectedText().replace(QChar::ParagraphSeparator, u'\n'));
    }
    return parts.join(u'\n');
}

void TextView::normalizeCarets()
{
    // Sort by position and merge carets whose selections touch or overlap.
    const QTextCursor primary = m_carets[m_primary].cursor;
    std::stable_sort(m_carets.begin(), m_carets.end(),
                     [](const Caret &a, const Caret &b) { return lessByPosition(a.cursor, b.cursor); });
    std::vector<Caret> merged;
    for (Caret &caret : m_carets) {
        if (!merged.empty()) {
            Caret &last = merged.back();
            const int lastEnd = last.cursor.selectionEnd();
            const int start = caret.cursor.selectionStart();
            const bool bothEmpty = !last.cursor.hasSelection() && !caret.cursor.hasSelection();
            if (start < lastEnd || (bothEmpty && start == lastEnd)
                || (start == lastEnd && (!caret.cursor.hasSelection() || !last.cursor.hasSelection()))) {
                const int newStart = last.cursor.selectionStart();
                const int newEnd = qMax(lastEnd, caret.cursor.selectionEnd());
                const bool forward = caret.cursor.position() >= caret.cursor.anchor();
                last.cursor.setPosition(forward ? newStart : newEnd);
                last.cursor.setPosition(forward ? newEnd : newStart, QTextCursor::KeepAnchor);
                continue;
            }
        }
        merged.push_back(caret);
    }
    m_carets = std::move(merged);
    m_primary = 0;
    for (int i = 0; i < int(m_carets.size()); ++i) {
        const QTextCursor &c = m_carets[i].cursor;
        if (c.selectionStart() <= primary.position() && primary.position() <= c.selectionEnd())
            m_primary = i;
    }
}

void TextView::caretsChanged()
{
    normalizeCarets();
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
    QTextCursor primary = textCursor();
    if (m_carets.size() == 1 && primary.hasSelection())
        primary.clearSelection();
    setTextCursor(primary);
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
    } else {
        m_lines.erase(m_lines.begin() + first, m_lines.begin() + first + oldAffected);
        std::vector<LineCache> fresh(newAffected);
        m_lines.insert(m_lines.begin() + first, std::make_move_iterator(fresh.begin()),
                       std::make_move_iterator(fresh.end()));
    }
    m_topRow = qBound(0, m_topRow, qMax(0, rowCount() - 1));
    rowsMayHaveChanged();
    updateScrollBars();
    viewport()->update();
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
    for (LineCache &line : m_lines)
        line.layout.reset();
    m_maxLineWidth = 0;
}

void TextView::relayout()
{
    invalidateLayouts();
    rowsMayHaveChanged();
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

qreal TextView::textLeft() const
{
    return gutterWidth() + m_leftMargin;
}

qreal TextView::textWidth() const
{
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
    return row(m_topRow).firstLine;
}

void TextView::scrollToLine(int line)
{
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
    const int rows = rowCount();
    const int index = qBound(0, m_topRow, rows - 1);
    const qreal height = rowHeight(index);
    const std::vector<LineAnchor> anchors = lineAnchors(index, height);
    const qreal y = qBound<qreal>(0, m_topOffset, height);
    // The first anchor below y and the one before it.
    for (size_t i = 1; i < anchors.size(); ++i) {
        const LineAnchor &a = anchors[i - 1];
        const LineAnchor &b = anchors[i];
        if (y < b.y)
            return a.line + (b.line - a.line) * (y - a.y) / (b.y - a.y);
    }
    return anchors.back().line;
}

void TextView::scrollToLinePosition(qreal position)
{
    const int lines = m_doc->blockCount();
    position = qBound<qreal>(0, position, lines);
    const int index = qBound(0, rowOfLine(qMin(int(position), lines - 1)), qMax(0, rowCount() - 1));
    const qreal height = rowHeight(index);
    const std::vector<LineAnchor> anchors = lineAnchors(index, height);
    qreal y = height;
    for (size_t i = 1; i < anchors.size(); ++i) {
        const LineAnchor &a = anchors[i - 1];
        const LineAnchor &b = anchors[i];
        if (position < b.line) {
            y = a.y + (b.y - a.y) * qMax<qreal>(0, position - a.line) / (b.line - a.line);
            break;
        }
    }
    m_topRow = index;
    m_topOffset = y;
    normalizeScroll();
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
    m_topRow = rowCount() - 1;
    m_topOffset = maxLastRowOffset();
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

int TextView::rowAt(qreal y, qreal *top)
{
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

QTextLayout *TextView::layoutForLine(int line)
{
    if (line < 0 || line >= int(m_lines.size()))
        return nullptr;
    LineCache &cache = m_lines[line];
    const qreal wrapWidth = m_wordWrap ? textWidth() : hugeWidth;
    const QTextBlock block = m_doc->findBlockByNumber(line);
    const bool hasPreedit = !m_preedit.isEmpty() && textCursor().block() == block;
    if (cache.layout && cache.wrapWidth == wrapWidth && !hasPreedit)
        return cache.layout.get();

    if (!cache.layout)
        cache.layout = std::make_unique<QTextLayout>();
    QTextLayout *layout = cache.layout.get();
    layout->clearLayout();
    layout->setText(block.text());
    layout->setFont(font());
    layout->setTextOption(textOption());
    layout->setCacheEnabled(true);
    layout->setFormats(block.layout()->formats());
    if (hasPreedit) {
        layout->setPreeditArea(textCursor().positionInBlock(), m_preedit);
        QList<QTextLayout::FormatRange> formats = layout->formats();
        for (const QInputMethodEvent::Attribute &a : std::as_const(m_preeditAttributes)) {
            if (a.type != QInputMethodEvent::TextFormat)
                continue;
            const QTextCharFormat f = qvariant_cast<QTextFormat>(a.value).toCharFormat();
            if (f.isValid())
                formats.append({textCursor().positionInBlock() + a.start, a.length, f});
        }
        layout->setFormats(formats);
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
    return layout;
}

qreal TextView::rowHeight(int index)
{
    const Row r = row(index);
    if (r.rendered)
        return renderedRowHeight(r, textWidth());
    QTextLayout *layout = layoutForLine(r.firstLine);
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
    v->setRange(0, qMax(0, rows - 1));
    v->setPageStep(pageRows);
    v->setSingleStep(1);
    v->setValue(m_topRow);

    QScrollBar *h = horizontalScrollBar();
    if (m_wordWrap) {
        h->setRange(0, 0);
    } else {
        h->setRange(0, qMax(0, int(m_maxLineWidth - textWidth() + m_charWidth * 2)));
        h->setPageStep(int(textWidth()));
        h->setSingleStep(int(m_charWidth * 2));
    }
    m_updatingScrollBars = false;
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

QRectF TextView::caretRect(const QTextCursor &cursor)
{
    const QTextBlock block = cursor.block();
    const int rowIndex = rowOfLine(block.blockNumber());
    qreal top = 0;
    if (!rowTop(rowIndex, &top))
        return {};
    const Row r = row(rowIndex);
    if (r.rendered)
        return QRectF(textLeft(), top, 2, rowHeight(rowIndex));
    QTextLayout *layout = layoutForLine(block.blockNumber());
    const int pos = cursor.positionInBlock();
    const QTextLine line = layout->lineForTextPosition(pos);
    if (!line.isValid())
        return QRectF(textLeft(), top, 2, m_lineHeight);
    const qreal x = line.cursorToX(pos) + textLeft() - horizontalScrollBar()->value();
    return QRectF(x, top + line.y() - m_linePad, 2, m_lineHeight);
}

qreal TextView::cursorX(const QTextCursor &cursor)
{
    QTextLayout *layout = layoutForLine(cursor.blockNumber());
    const QTextLine line = layout ? layout->lineForTextPosition(cursor.positionInBlock()) : QTextLine();
    return line.isValid() ? line.cursorToX(cursor.positionInBlock()) : 0;
}

int TextView::positionAt(const QPointF &point)
{
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
    QTextLayout *layout = layoutForLine(info.firstLine);
    const qreal localY = qMax<qreal>(0, point.y() - y);
    const int lineIndex = qBound(0, int(localY / m_lineHeight), layout->lineCount() - 1);
    const QTextLine line = layout->lineAt(lineIndex);
    const qreal x = point.x() - textLeft() + horizontalScrollBar()->value();
    return block.position() + line.xToCursor(x);
}

void TextView::ensureCursorVisible()
{
    if (viewport()->height() <= 0)
        return;
    const QTextCursor cursor = textCursor();
    const int line = cursor.blockNumber();
    const int rowIndex = rowOfLine(line);
    const Row r = row(rowIndex);

    qreal caretTop = 0;
    if (!r.rendered) {
        QTextLayout *layout = layoutForLine(line);
        const QTextLine textLine = layout->lineForTextPosition(cursor.positionInBlock());
        if (textLine.isValid())
            caretTop = textLine.y() - m_linePad;
    }
    const qreal caretBottom = caretTop + m_lineHeight;
    const qreal height = viewport()->height();

    qreal top = 0;
    const bool visible = rowTop(rowIndex, &top) && top + caretTop >= 0 && top + caretBottom <= height;
    if (!visible) {
        if (rowIndex < m_topRow || (rowIndex == m_topRow && caretTop < m_topOffset)) {
            m_topRow = rowIndex;
            m_topOffset = caretTop;
        } else {
            // Scroll so the caret's bottom sits at the viewport's bottom.
            int topRow = rowIndex;
            qreal offset = caretBottom - height;
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
        const qreal x = cursorX(cursor);
        const qreal width = textWidth();
        if (x < h->value())
            h->setValue(int(qMax<qreal>(0, x - m_charWidth * 4)));
        else if (x > h->value() + width - m_charWidth)
            h->setValue(int(x - width + m_charWidth * 4));
    }
    viewport()->update();
}

void TextView::centerCursor()
{
    const int rowIndex = rowOfLine(textCursor().blockNumber());
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
    updateScrollBars();
    viewport()->update();
}

void TextView::setSearchHighlights(const QList<QTextCursor> &ranges, int current)
{
    m_searchRanges = ranges;
    m_searchCurrent = current;
    viewport()->update();
}

// ---------------------------------------------------------------------------
// Painting

void TextView::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    const QRect area = event->rect();
    painter.fillRect(area, theme::color(theme::Role::editor));

    const qreal left = textLeft();
    const qreal hscroll = horizontalScrollBar()->value();
    const int gutter = gutterWidth();
    const int height = viewport()->height();
    const int rows = rowCount();

    // Lines that hold a caret with no selection get the current-line shade.
    QSet<int> currentLines;
    for (const Caret &caret : m_carets) {
        if (!caret.cursor.hasSelection())
            currentLines.insert(caret.cursor.blockNumber());
    }

    struct VisibleLine
    {
        int line;
        qreal top;
        qreal height;
    };
    QList<VisibleLine> visibleLines;

    painter.save();
    painter.setClipRect(QRectF(gutter, 0, viewport()->width() - gutter, height));
    qreal y = firstRowY();
    for (int r = m_topRow; r < rows && y < height; ++r) {
        const Row info = row(r);
        const qreal h = rowHeight(r);
        if (y + h < area.top()) {
            y += h;
            continue;
        }
        if (info.rendered) {
            paintRenderedRow(painter, info, QRectF(left, y, textWidth(), h));
            y += h;
            continue;
        }

        const int lineNo = info.firstLine;
        const QTextBlock block = m_doc->findBlockByNumber(lineNo);
        QTextLayout *layout = layoutForLine(lineNo);
        const QRectF rowRect(gutter, y, viewport()->width() - gutter, h);
        paintLineBackground(painter, lineNo, rowRect);
        if (m_highlightCurrentLine && currentLines.contains(lineNo))
            painter.fillRect(rowRect, theme::color(theme::Role::currentLine));

        const int blockStart = block.position();
        const int blockEnd = blockStart + block.length(); // Includes the separator.
        const QPointF origin(left - hscroll, y);

        // Backgrounds are filled by hand so they span the full line height
        // (QTextLayout's own selection only covers the font height).
        const auto fillRange = [&](int start, int end, const QColor &color, bool newline) {
            for (int i = 0; i < layout->lineCount(); ++i) {
                const QTextLine textLine = layout->lineAt(i);
                const int ls = textLine.textStart();
                const int le = ls + textLine.textLength();
                const bool last = i == layout->lineCount() - 1;
                const int s = qMax(start, ls);
                const int e = qMin(end, le);
                if (s > e || (s == e && !(newline && last && e == le)))
                    continue;
                qreal x1 = textLine.cursorToX(s);
                qreal x2 = textLine.cursorToX(e);
                if (x1 > x2)
                    std::swap(x1, x2);
                if (newline && last && end > le)
                    x2 += m_charWidth; // Show the selected line break.
                painter.fillRect(QRectF(origin.x() + x1, y + textLine.y() - m_linePad, x2 - x1, m_lineHeight), color);
            }
        };
        for (int i = 0; i < m_searchRanges.size(); ++i) {
            const QTextCursor &match = m_searchRanges.at(i);
            if (match.selectionEnd() <= blockStart || match.selectionStart() >= blockEnd)
                continue;
            fillRange(match.selectionStart() - blockStart, match.selectionEnd() - blockStart,
                      i == m_searchCurrent ? searchCurrentColor() : searchColor(), false);
        }
        // A selection text colour other than the normal text (Legacy's white on
        // navy) replaces the syntax colours inside the selection.
        const QColor selectionText = theme::color(theme::Role::selectionText);
        const bool recolorSelection = selectionText != theme::color(theme::Role::text);
        QList<QTextLayout::FormatRange> selectedText;
        for (const Caret &caret : m_carets) {
            const QTextCursor &c = caret.cursor;
            if (!c.hasSelection() || c.selectionEnd() < blockStart || c.selectionStart() >= blockEnd)
                continue;
            fillRange(c.selectionStart() - blockStart, c.selectionEnd() - blockStart, theme::color(theme::Role::selection),
                      c.selectionEnd() >= blockEnd);
            if (recolorSelection) {
                QTextLayout::FormatRange range;
                range.start = qMax(c.selectionStart(), blockStart) - blockStart;
                range.length = qMin(c.selectionEnd(), blockEnd) - blockStart - range.start;
                range.format.setForeground(selectionText);
                selectedText.append(range);
            }
        }

        painter.setPen(theme::color(theme::Role::text));
        layout->draw(&painter, origin, selectedText);

        for (const DiagnosticRange &d : std::as_const(m_diagnostics)) {
            const int rangeStart = d.start.position();
            const int rangeEnd = d.end.position();
            if (rangeEnd <= blockStart || rangeStart >= blockEnd)
                continue;
            painter.save();
            painter.setPen(QPen(diagnosticColor(), 1));
            painter.setRenderHint(QPainter::Antialiasing, true);
            const int ds = qMax(rangeStart, blockStart) - blockStart;
            const int de = qMin(rangeEnd, blockEnd - 1) - blockStart;
            for (int i = 0; i < layout->lineCount(); ++i) {
                const QTextLine textLine = layout->lineAt(i);
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
            painter.setPen(m_readOnly ? theme::color(theme::Role::mutedText) : theme::color(theme::Role::text));
            for (const Caret &caret : m_carets) {
                if (caret.cursor.block() != block)
                    continue;
                int pos = caret.cursor.positionInBlock();
                if (!m_preedit.isEmpty() && &caret == &m_carets[m_primary])
                    pos += m_preedit.size();
                layout->drawCursor(&painter, origin, pos, 2);
            }
        }
        visibleLines.append({lineNo, y, h});
        y += h;
    }
    painter.restore();

    if (gutter > 0) {
        painter.fillRect(QRect(0, 0, gutter, height), theme::color(theme::Role::editor));
        painter.setFont(font());
        const QFontMetricsF fm(font());
        for (const VisibleLine &line : std::as_const(visibleLines)) {
            painter.setPen(currentLines.contains(line.line) ? theme::color(theme::Role::text) : theme::color(theme::Role::gutterText));
            painter.drawText(QRectF(0, line.top + m_linePad, gutter - gutterRightPad, fm.height()),
                             Qt::AlignRight | Qt::AlignVCenter, QString::number(line.line + 1));
        }
    }
}

// ---------------------------------------------------------------------------
// Events

bool TextView::event(QEvent *event)
{
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
                QToolTip::showText(help->globalPos(), message, viewport());
                return true;
            }
        }
        QToolTip::hideText();
        event->ignore();
        return true;
    }
    return QAbstractScrollArea::event(event);
}

void TextView::keyPressEvent(QKeyEvent *event)
{
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
                if (caret.cursor.hasSelection()) {
                    const int pos = left ? caret.cursor.selectionStart() : caret.cursor.selectionEnd();
                    caret.cursor.setPosition(pos);
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
        } else {
            insertNewline();
        }
        return;
    case Qt::Key_Tab:
        if (!ctrl && !alt) {
            insertTab();
            return;
        }
        break;
    case Qt::Key_Backtab:
        if (!ctrl && !alt) {
            outdent();
            return;
        }
        break;
    case Qt::Key_Escape:
        if (m_carets.size() > 1 || textCursor().hasSelection()) {
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
    const int line = textCursor().blockNumber();
    const bool typed = event->replacementLength() == 0 && typeCharacter(event->commitString());
    if (!typed && (!event->commitString().isEmpty() || event->replacementLength() > 0)) {
        beginEdit();
        for (Caret &caret : m_carets) {
            if (event->replacementLength() > 0) {
                caret.cursor.setPosition(caret.cursor.position() + event->replacementStart());
                caret.cursor.setPosition(caret.cursor.position() + event->replacementLength(),
                                         QTextCursor::KeepAnchor);
            }
            caret.cursor.insertText(event->commitString());
        }
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
    const QTextCursor cursor = textCursor();
    switch (query) {
    case Qt::ImEnabled:
        return !m_readOnly;
    case Qt::ImCursorRectangle:
        return self->caretRect(cursor).toAlignedRect().translated(viewport()->pos());
    case Qt::ImFont:
        return font();
    case Qt::ImCursorPosition:
        return cursor.positionInBlock();
    case Qt::ImAnchorPosition:
        return cursor.anchor() - cursor.block().position();
    case Qt::ImSurroundingText:
        return cursor.block().text();
    case Qt::ImCurrentSelection:
        return cursor.selectedText();
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
    qreal top = 0;
    const int pressedRow = rowAt(event->position().y(), &top);
    if (pressedRow >= 0) {
        const Row info = row(pressedRow);
        if (info.rendered
            && renderedRowPressed(info, QPointF(event->position().x() - textLeft(), event->position().y() - top), event))
            return;
    }
    // Third click in a row: select the line (the paragraph in rendered text).
    if (m_doubleClickTime.isValid() && m_doubleClickTime.elapsed() <= QApplication::doubleClickInterval()
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
        && !row(pressedRow).rendered) {
        // Shift+Alt+drag (Shift+Ctrl+drag): box selection. It starts from the
        // primary caret, or from where the previous box selection started.
        const QTextCursor primary = textCursor();
        if (!(m_columnAnchorValid && captureCarets() == m_columnResult)) {
            QTextCursor anchor(m_doc);
            anchor.setPosition(primary.anchor());
            QTextLayout *layout = layoutForLine(anchor.blockNumber());
            const QTextLine line = layout->lineForTextPosition(anchor.positionInBlock());
            m_columnAnchor = {anchor.blockNumber(), line.isValid() ? line.lineNumber() : 0, cursorX(anchor)};
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
            if (m_carets[i].cursor.position() == pos && m_carets.size() > 1) {
                m_carets.erase(m_carets.begin() + i);
                m_primary = qMin(m_primary, int(m_carets.size()) - 1);
                caretsChanged();
                return;
            }
        }
        QTextCursor cursor(m_doc);
        cursor.setPosition(pos);
        m_carets.push_back({cursor, -1});
        m_primary = int(m_carets.size()) - 1;
        m_dragOrigin = cursor;
        m_dragUnit = Unit::Char;
        m_dragging = true;
        caretsChanged();
        return;
    }

    QTextCursor cursor = textCursor();
    if (mods & Qt::ShiftModifier) {
        cursor.setPosition(pos, QTextCursor::KeepAnchor);
        m_dragOrigin = QTextCursor(m_doc);
        m_dragOrigin.setPosition(cursor.anchor());
    } else {
        cursor.setPosition(pos);
        m_dragOrigin = cursor;
    }
    m_dragUnit = Unit::Char;
    m_dragging = true;
    setTextCursor(cursor);
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
    QTextCursor target(m_doc);
    target.setPosition(pos);
    if (m_dragUnit == Unit::Word)
        target = wordAt(target);
    else if (m_dragUnit == Unit::Line)
        target = lineUnitAt(pos);
    if (!extend) {
        m_dragOrigin = target;
        Caret &caret = m_carets[m_primary];
        caret.cursor = target;
        caretsChanged();
        return;
    }
    // Extend from the origin unit to the unit under the mouse.
    const int start = qMin(m_dragOrigin.selectionStart(), target.selectionStart());
    const int end = qMax(m_dragOrigin.selectionEnd(), target.selectionEnd());
    QTextCursor result(m_doc);
    if (target.selectionStart() < m_dragOrigin.selectionStart()) {
        result.setPosition(end);
        result.setPosition(start, QTextCursor::KeepAnchor);
    } else {
        result.setPosition(start);
        result.setPosition(end, QTextCursor::KeepAnchor);
    }
    m_carets[m_primary].cursor = result;
    caretsChanged();
}

bool TextView::sourceLineAt(const QPointF &point, int *block, int *subLine)
{
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
    QTextLayout *layout = layoutForLine(info.firstLine);
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
        QTextLayout *layout = layoutForLine(b);
        // The ends of the box sit on the wrapped lines they were made on.
        const int fromLine = b == fromBlock ? qMin(m_columnAnchor.subLine, layout->lineCount() - 1) : 0;
        const int toLine = b == toBlock ? qMin(toSubLine, layout->lineCount() - 1) : 0;
        const QTextLine startLine = layout->lineAt(fromLine);
        const QTextLine endLine = layout->lineAt(toLine);
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
        QTextCursor c(m_doc);
        c.setPosition(span.start);
        c.setPosition(span.end, QTextCursor::KeepAnchor);
        m_carets.push_back({c, -1});
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
    if (!bounds.contains(m_lastMousePos)) {
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
    caret.cursor.setPosition(m_dragOrigin.position());
    caret.cursor.setPosition(pos, QTextCursor::KeepAnchor);
    caret.desiredX = -1;
    caretsChanged();
}

void TextView::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    m_dragging = false;
    m_columnDragging = false;
    m_autoScroll.stop();
    if (textCursor().hasSelection() && QApplication::clipboard()->supportsSelection())
        QApplication::clipboard()->setText(selectedText(), QClipboard::Selection);
}

void TextView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        event->ignore(); // Zoom is handled by the window.
        return;
    }
    if (event->angleDelta().x() != 0 && !m_wordWrap) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - event->angleDelta().x() / 2);
    }
    qreal dy = event->pixelDelta().y();
    if (dy == 0)
        dy = event->angleDelta().y() / 120.0 * 3 * m_lineHeight;
    // Scroll by pixels across rows of different heights.
    m_topOffset -= dy;
    normalizeScroll();
    updateScrollBars();
    viewport()->update();
    event->accept();
    Q_EMIT scrolled();
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
        relayout();
    }
}

void TextView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    if (m_wordWrap)
        rowsMayHaveChanged();
    updateScrollBars();
}

void TextView::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_blink.timerId()) {
        m_caretOn = !m_caretOn;
        viewport()->update();
    } else if (event->timerId() == m_autoScroll.timerId()) {
        const qreal h = viewport()->height();
        QScrollBar *v = verticalScrollBar();
        if (m_lastMousePos.y() < 0)
            v->setValue(v->value() - 1);
        else if (m_lastMousePos.y() > h)
            v->setValue(v->value() + 1);
        if (m_columnDragging) {
            columnSelectTo(QPointF(m_lastMousePos.x(), qBound<qreal>(0, m_lastMousePos.y(), h - 1)));
            return;
        }
        const int pos = positionAt(QPointF(m_lastMousePos.x(), qBound<qreal>(0, m_lastMousePos.y(), h - 1)));
        Caret &caret = m_carets[m_primary];
        caret.cursor.setPosition(m_dragOrigin.position());
        caret.cursor.setPosition(pos, QTextCursor::KeepAnchor);
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
    menu.exec(event->globalPos());
}

// ---------------------------------------------------------------------------
// Movement

void TextView::moveCarets(QTextCursor::MoveOperation op, bool select)
{
    for (Caret &caret : m_carets) {
        caret.cursor.movePosition(op, select ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
        caret.desiredX = -1;
    }
    caretsChanged();
}

void TextView::moveCaretsHome(bool select)
{
    for (Caret &caret : m_carets) {
        const QTextBlock block = caret.cursor.block();
        const int indent = leadingWhitespace(block.text());
        const int target = caret.cursor.positionInBlock() == indent ? 0 : indent;
        caret.cursor.setPosition(block.position() + target, select ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
        caret.desiredX = -1;
    }
    caretsChanged();
}

bool TextView::moveVertically(Caret &caret, int direction, QTextCursor::MoveMode mode)
{
    QTextCursor &cursor = caret.cursor;
    QTextBlock block = cursor.block();
    QTextLayout *layout = layoutForLine(block.blockNumber());
    const int pos = cursor.positionInBlock();
    QTextLine line = layout->lineForTextPosition(pos);
    if (caret.desiredX < 0)
        caret.desiredX = line.isValid() ? line.cursorToX(pos) : 0;

    const int target = line.lineNumber() + direction;
    if (target >= 0 && target < layout->lineCount()) {
        cursor.setPosition(block.position() + layout->lineAt(target).xToCursor(caret.desiredX), mode);
        return true;
    }
    block = direction < 0 ? block.previous() : block.next();
    if (!block.isValid()) {
        cursor.movePosition(direction < 0 ? QTextCursor::Start : QTextCursor::End, mode);
        return false;
    }
    layout = layoutForLine(block.blockNumber());
    const QTextLine next = layout->lineAt(direction < 0 ? layout->lineCount() - 1 : 0);
    cursor.setPosition(block.position() + next.xToCursor(caret.desiredX), mode);
    return true;
}

void TextView::moveCaretsVertically(int lines, bool select)
{
    const auto mode = select ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor;
    const int direction = lines < 0 ? -1 : 1;
    for (Caret &caret : m_carets) {
        if (!select && caret.cursor.hasSelection()) {
            caret.cursor.setPosition(direction < 0 ? caret.cursor.selectionStart() : caret.cursor.selectionEnd());
        }
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
    auto it = std::min_element(m_carets.begin(), m_carets.end(), [](const Caret &a, const Caret &b) {
        return a.cursor.position() < b.cursor.position();
    });
    Caret caret{QTextCursor(m_doc), it->desiredX};
    caret.cursor.setPosition(it->cursor.position());
    if (!moveVertically(caret, -1, QTextCursor::MoveAnchor))
        return;
    m_carets.push_back(caret);
    m_primary = int(m_carets.size()) - 1;
    caretsChanged();
}

void TextView::addCursorBelow()
{
    auto it = std::max_element(m_carets.begin(), m_carets.end(), [](const Caret &a, const Caret &b) {
        return a.cursor.position() < b.cursor.position();
    });
    Caret caret{QTextCursor(m_doc), it->desiredX};
    caret.cursor.setPosition(it->cursor.position());
    if (!moveVertically(caret, 1, QTextCursor::MoveAnchor))
        return;
    m_carets.push_back(caret);
    m_primary = int(m_carets.size()) - 1;
    caretsChanged();
}

QTextCursor TextView::wordAt(const QTextCursor &cursor) const
{
    const QTextBlock block = cursor.block();
    const QString text = block.text();
    int pos = cursor.positionInBlock();
    const auto isWord = [](QChar c) { return c.isLetterOrNumber() || c == u'_'; };
    if (pos > 0 && (pos >= text.size() || !isWord(text.at(pos))) && isWord(text.at(pos - 1)))
        --pos;
    QTextCursor result(m_doc);
    if (pos >= text.size() || !isWord(text.at(pos))) {
        result.setPosition(cursor.position());
        if (pos < text.size()) // Select a single non-word character.
            result.setPosition(block.position() + pos + 1, QTextCursor::KeepAnchor);
        return result;
    }
    int start = pos;
    int end = pos;
    while (start > 0 && isWord(text.at(start - 1)))
        --start;
    while (end < text.size() && isWord(text.at(end)))
        ++end;
    result.setPosition(block.position() + start);
    result.setPosition(block.position() + end, QTextCursor::KeepAnchor);
    return result;
}

QTextCursor TextView::findNext(const QString &text, int from) const
{
    const auto flags = QTextDocument::FindCaseSensitively;
    QTextCursor found = m_doc->find(text, from, flags);
    if (found.isNull())
        found = m_doc->find(text, 0, flags);
    return found;
}

void TextView::addNextOccurrence()
{
    // With no selections, Ctrl+D first selects the word at each caret.
    bool anySelection = false;
    for (const Caret &caret : m_carets)
        anySelection |= caret.cursor.hasSelection();
    if (!anySelection) {
        for (Caret &caret : m_carets)
            caret.cursor = wordAt(caret.cursor);
        caretsChanged();
        return;
    }
    const QTextCursor primary = textCursor();
    const QString needle = primary.selectedText();
    if (needle.isEmpty())
        return;
    QTextCursor found = findNext(needle, primary.selectionEnd());
    for (int guard = 0; !found.isNull() && guard < int(m_carets.size()) + 1; ++guard) {
        const bool taken = std::any_of(m_carets.begin(), m_carets.end(), [&](const Caret &c) {
            return c.cursor.selectionStart() == found.selectionStart() && c.cursor.selectionEnd() == found.selectionEnd();
        });
        if (!taken) {
            m_carets.push_back({found, -1});
            m_primary = int(m_carets.size()) - 1;
            caretsChanged();
            return;
        }
        found = findNext(needle, found.selectionEnd());
    }
}

void TextView::selectAllOccurrences()
{
    QTextCursor primary = textCursor();
    if (!primary.hasSelection())
        primary = wordAt(primary);
    const QString needle = primary.selectedText();
    if (needle.isEmpty())
        return;
    QList<QTextCursor> found;
    int primaryIndex = 0;
    for (QTextCursor c = m_doc->find(needle, 0, QTextDocument::FindCaseSensitively); !c.isNull();
         c = m_doc->find(needle, c.selectionEnd(), QTextDocument::FindCaseSensitively)) {
        if (c.selectionStart() == primary.selectionStart())
            primaryIndex = int(found.size());
        found.append(c);
    }
    setCursors(found, primaryIndex);
}

void TextView::selectAll()
{
    QTextCursor cursor(m_doc);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    setTextCursor(cursor);
}

void TextView::selectLine()
{
    for (Caret &caret : m_carets) {
        const int start = caret.cursor.selectionStart();
        const int end = caret.cursor.selectionEnd();
        QTextCursor c(m_doc);
        c.setPosition(start);
        c.movePosition(QTextCursor::StartOfBlock);
        const int anchor = c.position();
        c.setPosition(end);
        if (!c.movePosition(QTextCursor::NextBlock))
            c.movePosition(QTextCursor::End);
        caret.cursor.setPosition(anchor);
        caret.cursor.setPosition(c.position(), QTextCursor::KeepAnchor);
    }
    caretsChanged();
}

void TextView::expandSelection()
{
    m_expandHistory.append(cursors());
    const int primary = m_primary;
    for (Caret &caret : m_carets) {
        QTextCursor &c = caret.cursor;
        const QTextBlock block = c.block();
        const int lineStart = block.position();
        const int lineEnd = lineStart + block.length() - 1;
        if (!c.hasSelection()) {
            c = wordAt(c);
            if (c.hasSelection())
                continue;
        }
        if (c.selectionStart() > lineStart || c.selectionEnd() < lineEnd) {
            if (m_doc->findBlock(c.selectionStart()) == m_doc->findBlock(c.selectionEnd())) {
                c.setPosition(lineStart);
                c.setPosition(lineEnd, QTextCursor::KeepAnchor);
                continue;
            }
        }
        c.setPosition(0);
        c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
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
    const QList<QTextCursor> previous = m_expandHistory.takeLast();
    const auto history = m_expandHistory;
    setCursors(previous, qMin(m_primary, int(previous.size()) - 1));
    m_expandHistory = history;
}

void TextView::expandLineSelection()
{
    for (Caret &caret : m_carets) {
        const QTextBlock first = m_doc->findBlock(caret.cursor.selectionStart());
        const QTextBlock last = m_doc->findBlock(caret.cursor.selectionEnd());
        // To the start of the next line, or the end of the last one.
        const QTextBlock next = last.next();
        const int end = next.isValid() ? next.position() : last.position() + last.length() - 1;
        caret.cursor.setPosition(first.position());
        caret.cursor.setPosition(end, QTextCursor::KeepAnchor);
        caret.desiredX = -1;
    }
    caretsChanged();
}

void TextView::addCursorsToLineEnds()
{
    QList<QTextCursor> result;
    const auto add = [&](int position) {
        QTextCursor c(m_doc);
        c.setPosition(position);
        result.append(c);
    };
    for (const Caret &caret : m_carets) {
        const QTextCursor &c = caret.cursor;
        if (!c.hasSelection())
            continue;
        const QTextBlock first = m_doc->findBlock(c.selectionStart());
        const QTextBlock last = m_doc->findBlock(c.selectionEnd());
        for (QTextBlock b = first; b.isValid() && b.blockNumber() < last.blockNumber(); b = b.next())
            add(b.position() + b.length() - 1);
        // The last line only gets a caret where the selection ends, unless
        // that is the start of the line.
        if (c.selectionEnd() > last.position())
            add(c.selectionEnd());
    }
    if (!result.isEmpty())
        setCursors(result, int(result.size()) - 1);
}

void TextView::moveLastSelectionToNextMatch()
{
    Caret &last = m_carets[m_primary];
    QTextCursor match;
    if (!last.cursor.hasSelection()) {
        match = wordAt(last.cursor);
    } else {
        const QString needle = last.cursor.selectedText();
        if (needle.isEmpty())
            return;
        match = findNext(needle, last.cursor.selectionEnd());
    }
    if (match.isNull() || !match.hasSelection())
        return;
    last.cursor = match;
    last.desiredX = -1;
    caretsChanged();
}

QString TextView::searchSeedText() const
{
    const QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        const QString text = cursor.selectedText();
        return text.contains(QChar::ParagraphSeparator) ? QString() : text;
    }
    const QString word = wordAt(cursor).selectedText();
    if (word.isEmpty() || !(word.at(0).isLetterOrNumber() || word.at(0) == u'_'))
        return {};
    return word;
}

void TextView::scrollPage(bool forward)
{
    // A page is the window's height less two lines, as in VS Code.
    const int lines = qMax(1, viewport()->height() / m_lineHeight - 2);
    m_topOffset += (forward ? 1 : -1) * lines * m_lineHeight;
    normalizeScroll();
    updateScrollBars();
    viewport()->update();
    Q_EMIT scrolled();
}

void TextView::pasteTextAt(int position, const QString &text)
{
    if (!canEdit() || text.isEmpty())
        return;
    QTextCursor cursor(m_doc);
    cursor.setPosition(qBound(0, position, m_doc->characterCount() - 1));
    setTextCursor(cursor);
    QString clean = text;
    clean.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    beginEdit();
    m_carets[m_primary].cursor.insertText(clean);
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
    return !m_readOnly;
}

void TextView::beginEdit()
{
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
    for (const Caret &caret : m_carets)
        state.ranges.append({caret.cursor.anchor(), caret.cursor.position()});
    state.primary = m_primary;
    return state;
}

void TextView::restoreCarets(const CaretState &state)
{
    QList<QTextCursor> cursors;
    const int end = m_doc->characterCount() - 1;
    for (const auto &range : state.ranges) {
        QTextCursor cursor(m_doc);
        cursor.setPosition(qBound(0, range.first, end));
        cursor.setPosition(qBound(0, range.second, end), QTextCursor::KeepAnchor);
        cursors.append(cursor);
    }
    setCursors(cursors, state.primary);
}

void TextView::insertTextAtCarets(const QString &text)
{
    if (!canEdit())
        return;
    beginEdit();
    for (Caret &caret : m_carets) {
        caret.cursor.insertText(text);
        caret.desiredX = -1;
    }
    endEdit();
    m_expandHistory.clear();
    caretsChanged();
}

void TextView::deleteAtCarets(QTextCursor::MoveOperation op)
{
    if (!canEdit())
        return;
    beginEdit();
    for (Caret &caret : m_carets) {
        QTextCursor &c = caret.cursor;
        if (!c.hasSelection()) {
            if (op == QTextCursor::PreviousCharacter && deleteAutoClosedPair(caret))
                continue;
            if (op == QTextCursor::PreviousCharacter && m_insertSpaces && c.positionInBlock() > 0) {
                // In leading spaces, Backspace removes back to the previous tab stop.
                const QString text = c.block().text();
                const int pos = c.positionInBlock();
                if (leadingWhitespace(text) >= pos && text.left(pos).count(u' ') == pos) {
                    const int remove = ((pos - 1) % m_tabWidth) + 1;
                    c.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor, remove);
                    c.removeSelectedText();
                    continue;
                }
            }
            c.movePosition(op, QTextCursor::KeepAnchor);
        }
        c.removeSelectedText();
        caret.desiredX = -1;
    }
    endEdit();
    caretsChanged();
}

QString TextView::indentUnit() const
{
    return m_insertSpaces ? QString(m_tabWidth, u' ') : QStringLiteral("\t");
}

bool TextView::continueList(Caret &caret)
{
    QTextCursor &c = caret.cursor;
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
        c.insertText(!item.isQuote && item.lead.contains(u'>') ? item.lead : QString());
    } else {
        c.insertText(QStringLiteral("\n") + item.nextPrefix());
    }
    caret.desiredX = -1;
    return true;
}

void TextView::insertNewline()
{
    if (!canEdit())
        return;
    beginEdit();
    for (Caret &caret : m_carets) {
        QTextCursor &c = caret.cursor;
        if (m_markdown && continueList(caret))
            continue;
        const QString text = c.block().text();
        const QString indent = text.left(qMin(leadingWhitespace(text), c.positionInBlock()));
        c.insertText(QStringLiteral("\n") + indent);
        caret.desiredX = -1;
    }
    endEdit();
    caretsChanged();
}

void TextView::insertTab()
{
    if (!canEdit())
        return;
    for (const Caret &caret : m_carets) {
        const QTextCursor &c = caret.cursor;
        if (c.hasSelection()) {
            indent();
            return;
        }
    }
    beginEdit();
    QSet<int> indentedItems;
    for (Caret &caret : m_carets) {
        QTextCursor &c = caret.cursor;
        const md::ListLine item = m_markdown ? md::parseListLine(c.block().text()) : md::ListLine();
        if (item.valid && !item.isQuote) {
            // Tab in a list item indents the whole item.
            const QTextBlock block = c.block();
            if (indentedItems.contains(block.blockNumber()))
                continue;
            indentedItems.insert(block.blockNumber());
            const QString unit = indentUnit();
            const int anchor = c.anchor() + unit.size();
            const int position = c.position() + unit.size();
            QTextCursor(block).insertText(unit);
            c.setPosition(anchor);
            c.setPosition(position, QTextCursor::KeepAnchor);
            continue;
        }
        if (m_insertSpaces) {
            c.removeSelectedText();
            const int column = c.positionInBlock();
            c.insertText(QString(m_tabWidth - column % m_tabWidth, u' '));
        } else {
            c.insertText(QStringLiteral("\t"));
        }
    }
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
        const QTextCursor &c = caret.cursor;
        if (!c.hasSelection())
            return false;
        const QString selected = c.selectedText();
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
    for (Caret &caret : m_carets) {
        QTextCursor &c = caret.cursor;
        const bool forward = c.position() > c.anchor();
        const int start = c.selectionStart();
        const int end = c.selectionEnd();
        QTextCursor edit(m_doc);
        edit.setPosition(end);
        edit.insertText(close);
        edit.setPosition(start);
        edit.insertText(open);
        // Keep the wrapped text selected so the key can be repeated.
        c.setPosition(forward ? start + 1 : end + 1);
        c.setPosition(forward ? end + 1 : start + 1, QTextCursor::KeepAnchor);
        caret.desiredX = -1;
    }
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
        const QTextCursor &c = caret.cursor;
        if (c.hasSelection())
            return false;
        const int pos = c.position();
        if (m_doc->characterAt(pos) != ch)
            return false;
        // Quotes after a backslash are escaped, not closers.
        if (isQuoteChar(ch) && pos > 0 && m_doc->characterAt(pos - 1) == u'\\')
            return false;
        qsizetype found = -1;
        for (qsizetype i = 0; i < m_autoClosed.size(); ++i) {
            if (m_autoClosed.at(i).closer.selectionStart() == pos) {
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
        caret.cursor.setPosition(caret.cursor.position() + 1);
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
        const QTextCursor &c = caret.cursor;
        if (c.hasSelection())
            return false;
        const QString line = c.block().text();
        const int col = c.positionInBlock();
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
    QList<AutoClosed> added;
    for (Caret &caret : m_carets) {
        QTextCursor &c = caret.cursor;
        c.insertText(QString(open) + close);
        c.movePosition(QTextCursor::PreviousCharacter);
        AutoClosed entry;
        entry.inner = QTextCursor(m_doc);
        entry.inner.setPosition(c.position());
        entry.inner.setKeepPositionOnInsert(true);
        entry.closer = QTextCursor(m_doc);
        entry.closer.setPosition(c.position());
        entry.closer.setPosition(c.position() + 1, QTextCursor::KeepAnchor);
        m_autoClosed.append(entry);
        caret.desiredX = -1;
    }
    endEdit();
    m_expandHistory.clear();
    caretsChanged();
    return true;
}

bool TextView::deleteAutoClosedPair(Caret &caret)
{
    if (m_autoClosed.isEmpty() || !Settings::instance().flag(key::autoClose))
        return false;
    QTextCursor &c = caret.cursor;
    const int pos = c.position();
    if (pos == 0)
        return false;
    for (qsizetype i = 0; i < m_autoClosed.size(); ++i) {
        if (m_autoClosed.at(i).closer.selectionStart() != pos)
            continue;
        const QChar open = m_doc->characterAt(pos - 1);
        const QChar close = m_doc->characterAt(pos);
        for (const CharPair &pair : autoClosingPairs(m_markdown, m_codeEditing)) {
            if (pair.first == open && pair.second == close) {
                m_autoClosed.removeAt(i);
                c.setPosition(pos - 1);
                c.setPosition(pos + 1, QTextCursor::KeepAnchor);
                c.removeSelectedText();
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
        bool keep = entry.closer.selectionEnd() - entry.closer.selectionStart() == 1;
        if (keep) {
            // Only while a caret stays between the opener and the closer.
            keep = false;
            for (const Caret &caret : m_carets) {
                const int pos = caret.cursor.position();
                if (!caret.cursor.hasSelection() && pos >= entry.inner.position()
                    && pos <= entry.closer.selectionStart()) {
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
    for (const Caret &caret : m_carets) {
        const QTextCursor &c = caret.cursor;
        const QTextBlock startBlock = m_doc->findBlock(c.selectionStart());
        QTextBlock endBlock = m_doc->findBlock(c.selectionEnd());
        // A selection ending at column 0 does not include that line.
        if (c.hasSelection() && endBlock != startBlock && c.selectionEnd() == endBlock.position())
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
        c.insertText(unit);
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
        c.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, remove);
        c.removeSelectedText();
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
        c.removeSelectedText();
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
            c.removeSelectedText();
        } else {
            c.setPosition(block.position() + minIndent);
            c.insertText(m_commentMarker + u' ');
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
        c.removeSelectedText();
    }
    endEdit();
    for (Caret &caret : m_carets)
        caret.cursor.clearSelection();
    caretsChanged();
}

void TextView::insertLineBelow()
{
    if (!canEdit())
        return;
    beginEdit();
    for (Caret &caret : m_carets) {
        QTextCursor &c = caret.cursor;
        const QString text = c.block().text();
        c.clearSelection();
        c.movePosition(QTextCursor::EndOfBlock);
        c.insertText(QStringLiteral("\n") + text.left(leadingWhitespace(text)));
    }
    endEdit();
    caretsChanged();
}

void TextView::insertLineAbove()
{
    if (!canEdit())
        return;
    beginEdit();
    for (Caret &caret : m_carets) {
        QTextCursor &c = caret.cursor;
        const QString indent = c.block().text().left(leadingWhitespace(c.block().text()));
        c.clearSelection();
        c.movePosition(QTextCursor::StartOfBlock);
        c.insertText(indent + QStringLiteral("\n"));
        c.movePosition(QTextCursor::PreviousBlock);
        c.movePosition(QTextCursor::EndOfBlock);
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
        const int line = caret.cursor.blockNumber();
        for (int i = 0; i < ranges.size(); ++i) {
            const int start = m_doc->findBlockByNumber(ranges[i].first).position();
            const int anchorLine = m_doc->findBlock(caret.cursor.anchor()).blockNumber();
            if ((line >= ranges[i].first && line <= ranges[i].second)
                || (anchorLine >= ranges[i].first && anchorLine <= ranges[i].second)) {
                saved.push_back({i, caret.cursor.anchor() - start, caret.cursor.position() - start});
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
        c.removeSelectedText();
        // Re-insert it on the other side of the range.
        if (direction < 0) {
            const QTextBlock lastBlock = m_doc->findBlockByNumber(last - 1);
            QTextCursor ins(m_doc);
            ins.setPosition(lastBlock.position() + lastBlock.length() - 1);
            ins.insertText(QStringLiteral("\n") + neighbourText);
            newStarts[i] = m_doc->findBlockByNumber(first - 1).position();
        } else {
            QTextCursor ins(m_doc->findBlockByNumber(first));
            ins.insertText(neighbourText + QStringLiteral("\n"));
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
    for (const Saved &s : saved) {
        QTextCursor c(m_doc);
        c.setPosition(newStarts[s.range] + s.anchor);
        c.setPosition(newStarts[s.range] + s.position, QTextCursor::KeepAnchor);
        carets.push_back({c, -1});
    }
    if (!carets.empty()) {
        const QTextCursor primary = carets[qMin(m_primary, int(carets.size()) - 1)].cursor;
        m_carets = std::move(carets);
        m_primary = 0;
        for (int i = 0; i < int(m_carets.size()); ++i) {
            if (m_carets[i].cursor.position() == primary.position())
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
        ins.insertText(QStringLiteral("\n") + text);
    }
    endEdit();
    if (direction > 0) {
        // Carets follow to the lower copy.
        for (Caret &caret : m_carets) {
            const auto range = std::find_if(ranges.begin(), ranges.end(), [&](const auto &r) {
                const int line = caret.cursor.blockNumber();
                return line >= r.first && line <= r.second;
            });
            if (range == ranges.end())
                continue;
            const int lines = range->second - range->first + 1;
            const int anchorLine = m_doc->findBlock(caret.cursor.anchor()).blockNumber();
            const int anchorCol = caret.cursor.anchor() - m_doc->findBlock(caret.cursor.anchor()).position();
            const int posCol = caret.cursor.positionInBlock();
            QTextCursor c(m_doc);
            c.setPosition(m_doc->findBlockByNumber(anchorLine + lines).position() + anchorCol);
            c.setPosition(m_doc->findBlockByNumber(caret.cursor.blockNumber() + lines).position() + posCol,
                          QTextCursor::KeepAnchor);
            caret.cursor = c;
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
        anySelection |= caret.cursor.hasSelection();
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
        anySelection |= caret.cursor.hasSelection();
    copy();
    if (!anySelection) {
        deleteLines();
        return;
    }
    beginEdit();
    for (Caret &caret : m_carets)
        caret.cursor.removeSelectedText();
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
    std::vector<int> order(m_carets.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = int(i);
    std::sort(order.begin(), order.end(), [this](int a, int b) {
        return m_carets[a].cursor.position() < m_carets[b].cursor.position();
    });
    for (size_t k = 0; k < order.size(); ++k) {
        QTextCursor &c = m_carets[order[k]].cursor;
        if (!parts.isEmpty()) {
            c.insertText(parts.at(int(k)));
        } else if (wholeLines && !c.hasSelection()) {
            // Whole lines go above the caret's line.
            QTextCursor lineStart(c.block());
            lineStart.insertText(text);
        } else {
            c.insertText(text);
        }
    }
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
        QMessageBox::warning(this, QApplication::applicationDisplayName(),
                             tr("Could not create the folder %1.").arg(QDir::toNativeSeparators(targetDir)));
        return;
    }
    const QString base = QStringLiteral("image-") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString path = QDir(targetDir).filePath(base + QStringLiteral(".png"));
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = QDir(targetDir).filePath(base + u'-' + QString::number(n) + QStringLiteral(".png"));
    if (!image.save(path, "PNG")) {
        QMessageBox::warning(this, QApplication::applicationDisplayName(),
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
    if (undo)
        m_doc->undo();
    else
        m_doc->redo();
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
    if (positions.isEmpty())
        return;
    QList<QTextCursor> cursors;
    const int end = m_doc->characterCount() - 1;
    for (const int p : std::as_const(positions)) {
        QTextCursor cursor(m_doc);
        cursor.setPosition(qBound(0, p, end));
        cursors.append(cursor);
    }
    setCursors(cursors, 0);
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
