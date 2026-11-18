// Text editor view over a QTextDocument with multiple cursors.
//
// The view lays out each line itself (QTextLayout per block, using the
// highlighter's formats from the document; a very long line in pieces, see
// LineLayout) and scrolls by "rows". In this class a row is one source line.
// Subclasses can merge runs of lines into a single rendered row (the visual
// Markdown view does this for blocks without a caret) by overriding the row
// hooks.
#pragma once

#include "../markdown/MarkdownEditing.h"
#include "LineLayout.h"
#include "Search.h"

#include <QAbstractScrollArea>
#include <QBasicTimer>
#include <QElapsedTimer>
#include <QPointer>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextLayout>

#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QImage;
class QMenu;
class QTextDocument;

class FenceProbe; // (tests)

namespace vn {

namespace md {
struct Screen;
}

class TextView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    // The document must use vn::DocumentLayout.
    explicit TextView(QTextDocument *document, QWidget *parent = nullptr);
    ~TextView() override;

    QTextDocument *document() const { return m_doc; }

    bool wordWrap() const { return m_wordWrap; }
    void setWordWrap(bool on);
    void setLineNumbersVisible(bool on);
    void setShowWhitespace(bool on);
    void setHighlightCurrentLine(bool on);
    void setTabWidth(int columns);
    void setInsertSpaces(bool on);
    void setLineSpacing(qreal factor);
    void beginSettingsUpdate();
    void endSettingsUpdate();
    void setCommentMarker(const QString &marker);
    bool isReadOnly() const { return m_readOnly; }
    void setReadOnly(bool on);
    // Modifier that adds a caret on click: Alt (default) or Ctrl.
    void setMultiCursorModifier(Qt::KeyboardModifier modifier) { m_multiCursorModifier = modifier; }

    // Markdown documents get list continuation, link and image pasting and
    // Ctrl+Click on links. Off for other languages.
    void setMarkdownEditing(bool on);
    bool markdownEditing() const { return m_markdown; }
    // Documents with a syntax definition other than Markdown auto-close
    // quotes and backticks too; plain text auto-closes nothing.
    void setCodeEditing(bool on) { m_codeEditing = on; }
    bool codeEditing() const { return m_codeEditing; }
    // The file being edited; relative paths in pasted links and images are
    // relative to its folder.
    void setDocumentPath(const QString &path);

    // A range drawn with a red wavy underline and a tooltip. Positions follow
    // edits; a range whose text is deleted disappears.
    struct Diagnostic
    {
        int start = 0;
        int end = 0;
        QString message;
    };
    void setDiagnostics(const QList<Diagnostic> &diagnostics);
    QList<Diagnostic> diagnostics() const;

    // A caret as numbers: where its selection starts (the anchor) and where
    // the caret is; the same where nothing is selected.
    struct CaretRange
    {
        int anchor = 0;
        int position = 0;
        int start() const { return qMin(anchor, position); }
        int end() const { return qMax(anchor, position); }
        bool hasSelection() const { return anchor != position; }
        bool operator==(const CaretRange &other) const = default;
    };

    // The primary caret. (A QTextCursor made for the asking: placing one in
    // a very long line costs Qt a pass over the line after every edit, and a
    // document that has held thousands of them stays slower to edit; the
    // view keeps its carets as numbers, see primaryCaret() and caretRanges().)
    QTextCursor textCursor() const;
    // Replaces all carets with one.
    void setTextCursor(const QTextCursor &cursor);
    QList<QTextCursor> cursors() const;
    // Replaces all carets; primary is an index into the list.
    void setCursors(const QList<QTextCursor> &cursors, int primary);
    int cursorCount() const { return int(m_carets.size()); }
    // The same as numbers: the primary caret, every caret in document order,
    // and replacing them all (in any order; primary is an index into the list).
    CaretRange primaryCaret() const;
    QList<CaretRange> caretRanges() const;
    void setCaretRanges(const QList<CaretRange> &ranges, int primary);
    // An edit made outside the views, within an edit block, as
    // QTextCursor::insertText(): the carets of the views on the document
    // move as cursors in it would (see Journal).
    static void insertText(QTextCursor &cursor, const QString &text);
    // Selected text of every caret, joined by newlines, in document order.
    QString selectedText() const;

    bool hasSelection() const;
    // Shows another view's carets and selections (same document) without
    // emitting signals, taking focus or changing this view's own mouse state;
    // used to mirror the selection between the views of a split. The primary
    // caret is scrolled into view when reveal is set.
    void mirrorCursorsFrom(const TextView *other, bool reveal);

    // Virtual: a view that does not scroll (read mode) shows the caret its own way.
    virtual void ensureCursorVisible();
    virtual void centerCursor();
    // The caret was put where the user asked to go (find, go to line, the
    // outline): a view that hides text (collapsed blocks) shows it.
    virtual void revealCaret() {}
    // Space left and right of the text (the gutter is extra).
    void setTextMargins(qreal left, qreal right, qreal top = 0);

    // First line at the top of the viewport, and scrolling to put a line there.
    int topLine() const;
    // The last line the viewport can show, at most (fewer with wrapping).
    int bottomLine() const;
    // Whether some of a line is in the viewport (on the screen, in columns).
    bool isLineShown(int line);
    void scrollToLine(int line);
    // The same with fractions: 12.3 is 30% of the way from where line 12
    // starts to where line 13 starts. Rendered rows place lines through
    // rowLineAnchors() and interpolate between anchors.
    qreal topLinePosition();
    void scrollToLinePosition(qreal position);
    // Scrolled as far down as it goes, and scrolling there.
    bool isAtScrollEnd();
    void scrollToEnd();
    // Puts the leader's top line position at this view's top (the end at the
    // end), for split views of the same document.
    void followScroll(TextView &leader);
    // Pixels between the top of the content and the top of the viewport.
    // Walks every row above, so it is meant for tests.
    qreal scrollPixels();

    // The text a "find selection" command searches for: the primary caret's
    // one-line selection, or else the word at the caret. Empty if neither.
    QString searchSeedText() const;
    // Scrolls by a viewport height without moving the carets (down when
    // forward).
    virtual void scrollPage(bool forward);
    // Inserts text at a document position and puts the only caret after it;
    // what a middle click does with the X11 primary selection.
    void pasteTextAt(int position, const QString &text);

    // Background highlights for find results; current is an index or -1.
    void setSearchHighlights(const QList<search::Range> &ranges, int current);

    // Columns (TextViewColumns.cpp): the lines in columns of a set width, a
    // screen at a time, with a page arrow at each side and a button for a
    // line up and down, as read mode shows a rendered document (md::ReadFlow
    // places the lines in both). Only for a view whose rows are source lines,
    // not the visual view (it has read mode). Turned on, the first column
    // starts at startLine (-1: the line at the top); turned off, the view
    // scrolls so the first column's first line is at the top. The text edits
    // in columns as anywhere; the caret going past the last column (or before
    // the first) moves the view on (back) by one column, not a page.
    void setColumnsShown(bool on, int startLine = -1);
    bool columnsShown() const { return m_columnsOn; }
    // 0: as many as fit.
    void setColumnCount(int columns);
    // Characters of text per column, in the editor font's character width.
    void setColumnChars(int chars);
    int shownColumnCount();
    int maxColumnCount();
    // The lines on screen in columns (0-based, both included), or (-1, -1).
    std::pair<int, int> columnLineRange();
    void columnsPageForward();
    void columnsPageBack();
    // Negative: up. Lines are the rows the screen shows (wrapped ones count).
    void columnsNudge(int lines);
    void columnsToStart();
    void columnsToEnd();
    // Where a column is in the viewport, its gutter included; empty if none.
    QRect columnRect(int column);
    // How far a column's lines are scrolled sideways (Word Wrap off: each
    // column has its own), and its scroll bar (empty if it needs none).
    qreal columnOffset(int column);
    void setColumnOffset(int column, qreal offset);
    QRect columnBarRect(int column);

public Q_SLOTS:
    void undo();
    void redo();
    void cut();
    void copy();
    void paste();
    void selectAll();

    void addCursorAbove();
    void addCursorBelow();
    void addNextOccurrence();
    void selectAllOccurrences();
    void collapseToPrimary();
    void expandSelection();
    void shrinkSelection();
    void selectLine();
    // Selects whole lines for each caret; again, one more line.
    void expandLineSelection();
    // One caret at the end of each line of each selection.
    void addCursorsToLineEnds();
    // Like addNextOccurrence(), but the last added selection moves.
    void moveLastSelectionToNextMatch();
    // Steps back and forth through earlier caret and selection states.
    void cursorUndo();
    void cursorRedo();
    void trimTrailingWhitespace();

    void moveLinesUp();
    void moveLinesDown();
    void copyLinesUp();
    void copyLinesDown();
    void deleteLines();
    void insertLineAbove();
    void insertLineBelow();
    void indent();
    void outdent();
    void toggleLineComment();

public:
    // Markdown formatting commands (no-ops while read-only).
    enum class MarkdownFormat {
        Bold, Italic, Underline, Strikethrough, InlineCode,
        Heading1, Heading2, Heading3, Heading4, Heading5, Heading6,
        HeadingUp, HeadingDown,
        Quote, OrderedList, BulletList, TaskList,
        Link, CodeBlock
    };
    void formatMarkdown(MarkdownFormat format);

    // Pipe tables (TextViewTable.cpp), in Markdown views. They act on the
    // table around the primary caret and do nothing outside one.
    // The table being edited: the one holding the caret, or the cell a
    // subclass names with setTableTarget (a cell editor over a rendered table).
    md::TableInfo tableAtCaret();
    // Applies the operation to the whole table as one undo step, realigned,
    // and puts the caret where the operation says. False if it was refused.
    bool editTable(md::TableOp op, const QString &arg = {});
    // The same for the table holding `line`, naming the cell (row, col);
    // `rows` and `cols` above 0 make it about that many rows and columns
    // from there (see md::tableEdit).
    bool editTableAt(int line, md::TableOp op, int row, int col, const QString &arg = {}, bool moveCaret = true,
                     int rows = 0, int cols = 0);
    // A table edited with the caret in it is realigned once the caret has
    // left it (typing in it leaves it as it is meanwhile). Off by default.
    void setRealignTablesOnLeave(bool on);
    // Selects the text of a cell of the table holding `line`.
    bool moveToTableCell(int line, int row, int col);
    // Inserts a table of `rows` (with the header) by `cols` below the caret's
    // line, with its first header selected.
    virtual void insertTable(int rows, int cols);
    // The keys in a table: to the next or previous cell (a new row after the
    // last), and a new row below. False, with nothing done, outside one.
    bool tableTab(bool forward);
    bool tableEnter();
    // Adds the table commands (for the caret's table, enabled as they apply)
    // to a menu.
    void populateTableMenu(QMenu *menu);

    // The colours of lines [firstLine, lastLine] changed (the highlighter
    // ran) without a text change.
    void formatsChanged(int firstLine, int lastLine);

Q_SIGNALS:
    void cursorPositionChanged();
    void selectionChanged();
    void scrolled();
    // Files dropped without Shift (Markdown views insert links with Shift).
    void filesDropped(const QStringList &files);
    // Ctrl+Click (Alt+Click when Ctrl adds carets) on a link in Markdown text.
    void linkRequested(const QString &url);
    // Ctrl+wheel: this many notches up (positive) or down.
    void zoomRequested(int notches);
    // Columns came or went, or the screen or what fits changed. Sent once
    // the event that changed it is done.
    void columnsChanged();

protected:
    struct Row
    {
        int firstLine = 0;
        int lastLine = 0;
        bool rendered = false;
    };

    // Row model. The default is one row per line.
    virtual int rowCount() const;
    virtual Row row(int index) const;
    virtual int rowOfLine(int line) const;
    // Rendered rows only.
    virtual qreal renderedRowHeight(const Row &row, qreal width);
    virtual void paintRenderedRow(QPainter &painter, const Row &row, const QRectF &rect);
    // Returns a document position for a point relative to the row's top-left.
    virtual int hitTestRenderedRow(const Row &row, const QPointF &point);
    // Called before a source line's text is drawn, e.g. to shade active blocks.
    virtual void paintLineBackground(QPainter &painter, int line, const QRectF &rect);
    // Called after the carets or the document change, before repainting.
    virtual void rowsMayHaveChanged() {}
    // A press on a rendered row; return true to consume it (e.g. a link).
    virtual bool renderedRowPressed(const Row &row, const QPointF &point, QMouseEvent *event);
    // The modifier that follows links (the other one adds carets).
    Qt::KeyboardModifier linkModifier() const;
    // Where source lines start inside a row of the given height: (line, y)
    // pairs ascending in both. {firstLine, 0} and {lastLine + 1, height} are
    // implied; lines between anchors are spread evenly. The default adds
    // nothing, which suits source rows (one line, maybe wrapped).
    struct LineAnchor
    {
        qreal line = 0;
        qreal y = 0;
    };
    virtual std::vector<LineAnchor> rowLineAnchors(const Row &row, qreal height);

    // The "line" a triple click selects around a position (a block, or for
    // views that merge lines into rows, the whole row). Includes the line break.
    virtual QTextCursor lineUnitAt(int position) const;
    // True while the left button is down after a click or drag selection.
    bool isMouseSelecting() const { return m_dragging; }
    // Makes the table commands act on cell (row, col) of the table holding
    // `line`, not on the caret's table, until cleared (line < 0).
    void setTableTarget(int line, int row = 0, int col = 0);
    // Called before a table command acts on that target: save what is being
    // edited there (and clear the target).
    virtual void tableTargetEditing() {}
    // Puts in a pipe table of `rows` (with the header) x `cols` empty cells
    // at the caret, apart from the text around it, as one undo step.
    // `roomAfter`: a line after it to type on, apart from it, when nothing
    // follows. Returns the table's first line (the caret is left as Qt put it).
    int insertTableLines(int rows, int cols, bool roomAfter);
    // The table's cell (row, col) in the document, as md::tableCell says.
    md::TableCell tableCellOf(int line, int row, int col);
    md::TableInfo tableInfoAt(int line, int column);
    // Realigns the table the primary caret was in if it left it after
    // editing (see setRealignTablesOnLeave); the caret moves are checked on
    // their own, a mouse selection once it ends.
    void realignLeftTable();

    // The row at viewport y and its top, or -1 past the end.
    int rowAt(qreal y, qreal *top);
    // The row under a viewport point, with where the row's top-left corner is
    // in the viewport; -1 if none. The default: rowAt() and textLeft(). A view
    // that lays its rows out otherwise (in columns) says where they are.
    virtual int rowAtPoint(const QPointF &point, QPointF *origin);
    // The document position at a viewport point (what a click or a drag
    // selects); rendered rows through hitTestRenderedRow().
    virtual int positionAt(const QPointF &point);
    // The source line position (as topLinePosition() has it) at y from the
    // top of a row, and the row and y where a line position is drawn.
    qreal linePositionAt(int index, qreal y);
    std::pair<int, qreal> placeOfLinePosition(qreal position);
    // Hides the scroll bars whatever the document's length.
    void setScrollBarsHidden(bool hidden);
    // Whether the blinking carets are drawn at the moment.
    bool caretBlinkOn() const { return m_caretOn; }
    // Whether the rows are laid out as in the scrolling view (or in columns),
    // so that a blink can repaint just where the carets are; a view that
    // draws its carets by other rules says no, and the blink repaints it all.
    virtual bool caretRectsKnown() const { return true; }
    // Called when an edit made through this view starts (once per undo step,
    // also for undo and redo), before the document changes.
    virtual void editStarting() {}
    // Types text at every caret (replacing selections), undone as typing is.
    void insertTextAtCarets(const QString &text);

    // Left edge of text in viewport coordinates.
    qreal textLeft() const;
    // The width text wraps at; rendered rows are laid out at it. Virtual: a
    // view showing columns makes it a column's.
    virtual qreal textWidth() const;
    int lineHeight() const { return m_lineHeight; }
    // The viewport y range of the row holding a source line; false if that
    // row is not (even partly) in the viewport.
    bool lineBounds(int line, qreal *top, qreal *bottom);
    // Forgets cached row heights and repaints.
    void relayout();
    void requestRelayout();

    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void changeEvent(QEvent *event) override;
    void timerEvent(QTimerEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    bool focusNextPrevChild(bool next) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    // Columns of source lines; see setColumnsShown (TextViewColumns.cpp).
    struct Columns;
    class ColumnRows;
    struct ColumnLayout
    {
        int count = 1;    // Columns shown.
        int fit = 1;      // The most that fit.
        int left = 0;     // Of the first column, its gutter included.
        int top = 0;
        int width = 0;    // Of a column's text.
        int textLeft = 0; // From a column's left edge to its text.
        int gap = 0;
        int height = 0;   // What the lines get; the scroll bars go below.
        int columnWidth() const { return textLeft + width; }
        int columnLeft(int column) const { return left + column * (columnWidth() + gap); }
    };
    // Made on first use.
    Columns &columnState();
    const ColumnLayout &columnLayout();
    qreal columnsCharWidth() const;
    const md::Screen &columnsScreen();
    void columnsMoved(bool keepOffsets);
    // The lines changed: renumbered (a line added or removed, all laid out
    // anew), or only their colours or heights.
    void columnsRowsChanged(bool renumbered);
    void noteColumns();
    void placeColumnControls();
    void updateColumnControls();
    void paintColumns(QPaintEvent *event);
    bool columnsKey(QKeyEvent *event);
    void columnsWheel(QWheelEvent *event);
    void columnsEnsureCursorVisible();
    int columnsPositionAt(const QPointF &point);
    int columnsRowAt(const QPointF &point, QPointF *origin);
    // Where a caret is drawn in columns, and in which column; null if it is
    // not on the screen.
    QRectF columnsCaretRect(int position, int *column = nullptr);
    // The column (and its x) a point is in: the nearest, the gaps shared.
    int columnAtX(qreal x);
    // Widest line a column shows (Word Wrap off), to scroll it sideways.
    qreal columnContentWidth(int column);
    bool columnBarPressed(QMouseEvent *event);
    bool columnBarMoved(QMouseEvent *event);
    void columnsPage(bool forward, bool moveCarets, bool select);
    // Moves the view on (1) or back (-1) by one column; false if it cannot.
    bool columnsShift(int direction);
    // A drag past the last column (1) or before the first (-1) moves the view
    // under it; 0 elsewhere.
    int columnsDragDirection(const QPointF &point);
    void columnsDragTick();
    // Around a key press: the note under a column the caret left.
    void columnsKeyStarting(QKeyEvent *event);
    void columnsKeyDone();
    void showColumnsNote(int column, bool forward);
    void hideColumnsNote();

    struct DiagnosticRange
    {
        QTextCursor start;
        QTextCursor end;
        QString message;
    };
    // A caret: numbers, not a QTextCursor (see textCursor()), moved by every
    // change of the document as Qt moves a cursor (see adjustPositions()).
    struct Caret : CaretRange
    {
        qreal desiredX = -1; // Remembered x for up/down movement.
        // As QTextCursor::setPosition() and clearSelection().
        void setPosition(int pos, QTextCursor::MoveMode mode = QTextCursor::MoveAnchor)
        {
            position = pos;
            if (mode == QTextCursor::MoveAnchor)
                anchor = pos;
        }
        void clearSelection() { anchor = position; }
        CaretRange range() const { return {anchor, position}; }
    };
    struct LineCache
    {
        std::unique_ptr<LineLayout> layout;
        qreal wrapWidth = -1;
    };
    enum class Unit { Char, Word, Line };

    void onDocumentChanged(int from, int charsRemoved, int charsAdded);
    void updateMetrics();
    void invalidateLayouts();
    void listenToLayout(int line);
    void rowsMoved(int line, LineLayout *layout, int fromRow, int delta);
    void ensureShownMeasured(bool bottom = false);
    void settleColumnsScreen();
    void restartSettling();
    // Notes whether lines [first, last] include one long enough for pieces.
    void noteLongLines(int first, int last);
    // A line has been long enough for pieces (only then is there anything
    // to settle; normal files skip the long-line scans).
    bool m_hasLongLines = false;
    QBasicTimer m_settleTimer;
    QList<QTextBlock> m_lazyBlocks;
    bool m_settling = false;
    bool m_painting = false;
    // Drops cached layouts far from [firstLine, lastLine] once too many are live.
    void trimLayouts(int firstLine, int lastLine);
    void shiftSearchRanges(int from, int charsRemoved, int charsAdded);
    // Lines laid out in pieces before an edit (old[i] was line first + i)
    // pass the pieces it did not reach on to the lines now there.
    void keepPieces(int first, int newAffected, std::vector<std::unique_ptr<LineLayout>> old);
    LineLayout *layoutForLine(int line);
    LineLayout::Setup layoutSetup(qreal wrapWidth) const;
    QTextOption textOption() const;
    qreal rowHeight(int row);
    int gutterWidth() const;
    // Viewport y of the top row (the top margin shows only at the very top).
    qreal firstRowY() const { return (m_topRow == 0 ? m_topMargin : 0) - m_topOffset; }
    // Moves m_topOffset into range, stepping m_topRow across rows; the last
    // row can scroll up until one line of it is left.
    void normalizeScroll();
    qreal maxLastRowOffset();
    // rowLineAnchors() with both ends added and anything out of order dropped.
    std::vector<LineAnchor> lineAnchors(int row, qreal height);
    // Where, from the row's top, a source line position is drawn: anchors
    // from lineAnchors().
    static qreal yOfLinePosition(const std::vector<LineAnchor> &anchors, qreal position);
    void updateScrollBars();

    // Geometry. Returns false if the position is not in a visible row.
    bool rowTop(int row, qreal *top);
    QRectF caretRect(int position);
    qreal cursorX(int position);

    // Editing helpers.
    bool canEdit() const;
    void beginEdit();
    void endEdit();
    void deleteAtCarets(QTextCursor::MoveOperation op);
    // Edits within beginEdit()/endEdit(), as QTextCursor::insertText() (see
    // above) and removeSelectedText() do them: every position the views on
    // the document keep (carets, auto-closed pairs, ...) moves as a cursor
    // would.
    static void removeSelectedText(QTextCursor &cursor);
    // The text from `from` to `to` replaced by `text`, as insertText() does.
    void replaceRange(int from, int to, const QString &text);
    // A cursor at the range. (Placed within an edit block, it costs Qt
    // nothing; outside one, see textCursor().)
    QTextCursor cursorAt(const CaretRange &range) const;
    // Runs `edit` at each caret in turn, in document order, with the caret
    // (to be left where it goes) and a cursor placed at it. The edit changes
    // the text around its caret through insertText() and removeSelectedText(),
    // leaving the carets in document order. Within beginEdit()/endEdit(). The
    // other carets wait as numbers moved in bulk (see CaretSpans): moving
    // each by every edit would take N times N moves.
    void editAtCarets(const std::function<void(Caret &, QTextCursor &)> &edit);
    void insertNewline();
    void insertTab();
    // Auto-closing, overtyping and surrounding for one typed ASCII character.
    // Returns false when it should be inserted normally.
    bool typeCharacter(const QString &text);
    bool surroundSelections(QChar open);
    bool overtypeClosers(QChar ch);
    bool autoCloseBrackets(QChar open);
    bool deleteAutoClosedPair(Caret &caret, QTextCursor &cursor);
    void pruneAutoClosed();
    // Distinct line numbers covered by the carets, ascending.
    QList<int> caretLines() const;
    // Merged [first, last] line ranges covered by the carets.
    QList<std::pair<int, int>> caretLineRanges() const;
    // Markdown helpers.
    QString linkAtPosition(int position);
    bool continueList(QTextCursor &cursor);
    void insertFileLinks(const QStringList &files);
    void pasteImage(const QImage &image);
    QString diagnosticAt(int position) const;
    void moveLines(int direction);
    void copyLines(int direction);
    QString indentUnit() const;
    // Markdown formatting (TextViewFormat.cpp).
    void toggleInline(const QString &open, const QString &close);
    void setHeading(int level);
    void shiftHeading(int direction);
    void toggleLinePrefix(MarkdownFormat kind);
    void insertLink();
    void wrapInCodeBlock();
    // The document's text for the table functions; made once per revision.
    // The lines that can hold the table around `line`: the run of pipe-ended
    // lines it is in, joined by newlines, with the first one's number. The
    // Rust table functions get this instead of the whole document (which
    // costs a copy of it per call): a table never crosses a line that is not
    // such a line, and the run holds no code fence. Empty (firstLine -1) if
    // `line` is not such a line or the run is inside fenced code.
    struct TableWindow
    {
        QString text;
        int firstLine = -1;
    };
    TableWindow tableWindow(int line);
    // The run tableWindow() last made, kept across edits inside it (see
    // spliceTableRun): a keystroke in a long table does not read it again.
    struct TableRun
    {
        QStringList lines;
        QString text; // The lines joined.
        int firstLine = -1;
    };
    TableRun m_tableRun;
    void spliceTableRun(int first, int oldAffected, int newAffected);
    // For a line that is not a pipe row: the run of non-blank lines around
    // it (an HTML block ends at a blank line), if one of them starts with
    // <table. Kept until the document changes; `found` false is kept too, so
    // caret moves through a paragraph do not read it again.
    struct HtmlRun
    {
        QString text;
        int firstLine = -1;
        int lineCount = 0;
        int revision = -1;
        bool found = false;
    };
    HtmlRun m_htmlRun;
    TableWindow htmlTableWindow(int line);
    // Whether fenced code is open where `line` starts (see m_fenceMarks).
    friend class ::FenceProbe;
    bool inFencedCode(int line);
    void invalidateFenceMarks();
    void scanFenceMarks(int upTo, int budgetMs);
    void scheduleFenceScan();
    void spliceFenceMarks(int first, int oldAffected, int newAffected);
    void addTableMenu(QMenu *menu);

    // Caret helpers.
    void moveCarets(QTextCursor::MoveOperation op, bool select);
    // QTextCursor::movePosition(); a character or word move in a very long
    // line looks only at the text around the cursor (Qt works out the
    // whole line's character attributes first: a tenth of a second on a
    // 3 MB line, again after every edit).
    void moveCaret(Caret &caret, QTextCursor::MoveOperation op, QTextCursor::MoveMode mode) const;
    void moveCaretsVertically(int lines, bool select);
    bool moveVertically(Caret &caret, int direction, QTextCursor::MoveMode mode);
    void moveCaretsHome(bool select);
    void normalizeCarets();
    void undoRedo(bool undo);
    void caretsChanged();
    // Paints a source line's layout at origin: what is behind it (shade is
    // the strip the current line gets), find results, selections, the text,
    // diagnostics and carets. The state is the same for every line of a paint.
    struct LinePaintState
    {
        QSet<int> currentLines;
        struct DiagnosticSpan
        {
            int start;
            int end;
            int index;
        };
        std::vector<DiagnosticSpan> diagnosticSpans;
        std::vector<int> diagnosticMaxEnd;
        QColor selectionText;
        bool recolorSelection = false;
    };
    LinePaintState linePaintState() const;
    // `visible` is the part of the view being painted: rows of a long line
    // outside it are skipped.
    void paintSourceLine(QPainter &painter, int lineNo, const QPointF &origin, const QRectF &shade,
                         const QRectF &visible, const LinePaintState &state);
    // The word at a position (selected), or the character after it.
    CaretRange wordAt(int position) const;
    // The next match of text (case-sensitive) from `from`, around the end;
    // nothing selected if there is none.
    CaretRange findNext(const QString &text, int from) const;
    // The text between two positions, line breaks as U+2029, as
    // QTextCursor::selectedText() has it.
    QString textBetween(int from, int to) const;
    QString selectedTextOf(const CaretRange &range) const { return textBetween(range.start(), range.end()); }
    int blockNumberOf(int position) const;
    void restartBlink();
    // Repaints what a blink of the carets changes: where they are drawn.
    void updateBlinkingCarets();
    void selectByUnit(const QPointF &point, bool extend);
    // Box selection from the anchor to the point.
    bool columnSelectTo(const QPointF &point);
    // Block and wrapped line under a viewport point; false for rendered rows.
    bool sourceLineAt(const QPointF &point, int *block, int *subLine);
    void recordCursorHistory();
    void resetCursorHistory();
    void applyCursorHistory(int index, bool undo);

    QTextDocument *m_doc;
    std::vector<LineCache> m_lines;
    int m_liveLayouts = 0; // Layouts created since the last trim; an upper bound on the live ones.
    std::vector<Caret> m_carets;
    int m_primary = 0;

    // Caret state around each undo step made through beginEdit()/endEdit(),
    // indexed by undo step, so undo/redo can restore every caret.
    struct CaretState
    {
        QList<std::pair<int, int>> ranges; // anchor, position
        int primary = 0;
        bool operator==(const CaretState &other) const { return ranges == other.ranges && primary == other.primary; }
    };
    struct UndoEntry
    {
        CaretState before;
        CaretState after; // Filled in when the step is undone, for redo.
    };
    CaretState captureCarets() const;
    void restoreCarets(const CaretState &state);
    std::map<int, UndoEntry> m_undoEntries; // Keyed by undo position after the step.
    CaretState m_editStart;
    int m_editStartRevision = -1;
    int m_editStartSteps = 0;
    int m_syncedRevision = -1; // Document revision our entries are valid for.
    int m_editDepth = 0;
    QList<CaretState> m_expandHistory;

    // Earlier caret states for cursorUndo(), cleared by document changes.
    struct CursorHistoryEntry
    {
        CaretState state;
        int topRow = 0;
        qreal topOffset = 0;
        int scrollX = 0;
    };
    QList<CursorHistoryEntry> m_cursorUndo;
    QList<CursorHistoryEntry> m_cursorRedo;
    CaretState m_cursorLast;
    int m_cursorLastRevision = -1;
    bool m_cursorHistoryPaused = false;

    // Box selection: where it started (block, wrapped line, x in the line).
    struct ColumnAnchor
    {
        int block = 0;
        int subLine = 0;
        qreal x = 0;
        int position = 0; // In the block: finds the wrapped line again after a long line's rows change.
    };
    ColumnAnchor m_columnAnchor;
    CaretState m_columnResult; // Carets the last box selection produced.
    bool m_columnAnchorValid = false;
    bool m_columnDragging = false;

    QList<search::Range> m_searchRanges; // Sorted, disjoint.
    int m_searchCurrent = -1;

    // Options.
    bool m_wordWrap = true;
    bool m_settingsUpdate = false;
    bool m_relayoutPending = false;
    bool m_lineNumbers = true;
    bool m_showWhitespace = false;
    bool m_highlightCurrentLine = true;
    bool m_insertSpaces = true;
    bool m_readOnly = false;
    int m_tabWidth = 4;
    qreal m_lineSpacing = 1.5;
    QString m_commentMarker;
    qreal m_leftMargin = 4;
    qreal m_rightMargin = 8;
    qreal m_topMargin = 0;
    Qt::KeyboardModifier m_multiCursorModifier = Qt::AltModifier;
    // The lines that open or close fenced code (three or more backticks or
    // tildes after any quote markers; see core/src/format.rs), in order,
    // for the lines scanned so far: in idle time after a change, or up to a
    // line when it is asked about, then kept up to date
    // by onDocumentChanged. Pairing them says whether a line is in a fence.
    struct FenceMark
    {
        int line = 0;
        QChar ch;
        int run = 0;
        bool closes = false; // Nothing else on the line: it can close one.
    };
    // The mark of a line, if it is one; as the core's scan reads it.
    static std::optional<FenceMark> fenceMarkOf(int line, const QString &text);
    std::vector<FenceMark> m_fenceMarks;
    int m_fenceScanned = 0; // Lines from the top that m_fenceMarks covers.
    bool m_fenceScanPending = false;
    md::TableInfo m_tableInfo;
    struct TableTarget
    {
        int line = -1;
        int row = 0;
        int col = 0;
    } m_tableTarget;
    md::TableInfo caretTableInfo();
    int m_tableInfoRevision = -1;
    int m_tableInfoPosition = -1;
    bool m_realignTables = false;
    int m_tablePosition = 0; // In the table the caret was last seen in.
    bool m_tableTracked = false;
    int m_tableEnterRevision = -1;
    bool m_markdown = false;
    bool m_codeEditing = false;
    // Closers inserted by auto-closing that may still be typed over or
    // deleted with their opener. closer selects the closer; inner marks where
    // the text between the pair starts (kept there by text typed at it, as a
    // cursor with keepPositionOnInsert).
    struct AutoClosed
    {
        int inner = 0;
        CaretRange closer;
    };
    QList<AutoClosed> m_autoClosed;
    QString m_documentPath;
    QList<DiagnosticRange> m_diagnostics;
    QList<md::LinkTarget> m_linkSpans;
    int m_linkRevision = -1;

    // Metrics.
    int m_lineHeight = 16;
    qreal m_linePad = 0;
    qreal m_charWidth = 8;

    // Scrolling: the first visible row and how far it is scrolled up.
    int m_topRow = 0;
    qreal m_topOffset = 0;
    // Ctrl+wheel movement not yet worth a whole notch.
    int m_zoomDelta = 0;
    qreal m_maxLineWidth = 0;
    bool m_updatingScrollBars = false;
    bool m_scrollBarsHidden = false;

    // Mouse.
    bool m_dragging = false;
    Unit m_dragUnit = Unit::Char;
    CaretRange m_dragOrigin;
    QBasicTimer m_autoScroll;
    QPointF m_lastMousePos;
    // Double click time and place, for detecting the triple click.
    QElapsedTimer m_doubleClickTime;
    QPointF m_doubleClickPos;

    // Columns: made when first shown.
    std::unique_ptr<Columns> m_columns;
    bool m_columnsOn = false;
    // The first and last line on screen in columns, for the const queries.
    int m_columnsFirst = 0;
    int m_columnsLast = 0;

    // Caret blink and input method.
    QBasicTimer m_blink;
    bool m_caretOn = true;
    QString m_preedit;
    QList<QInputMethodEvent::Attribute> m_preeditAttributes;

    // The views on a document (one journal for each document), told of every
    // change made through insertText() and removeSelectedText() as it is
    // made, so that each moves its positions as Qt moves cursors. The
    // document tells of a whole edit block as one change, after it: a
    // position inside it cannot be placed from that. The views' undo is told
    // the same way (see Journal::undoRedo()). A block with changes made
    // otherwise (by other code) shows as reports that do not add up to the
    // change: the views go back to their positions from before it and move
    // them by the change as a whole.
    class Journal;
    QPointer<Journal> m_journal;
    bool m_reported = false; // Changes were reported since the document last told of one.
    // Moves every kept position as Qt moves a cursor when `removed`
    // characters at `at` go and then `added` come there (keepAtInsert: as an
    // undo puts back what a cursor removed with its position before its
    // anchor, a cursor at `at` stays before them).
    void adjustPositions(int at, int removed, int added, bool keepAtInsert = false);
    // The positions as they were before the edit block being reported.
    struct KeptPositions
    {
        std::vector<Caret> carets;
        QList<AutoClosed> autoClosed;
        QList<CaretState> expandHistory;
        CaretRange dragOrigin;
        int tablePosition = 0;
    };
    std::optional<KeptPositions> m_keptBefore;
    KeptPositions keptPositions() const;
    // During editAtCarets(): the carets wait there, not in m_carets.
    class CaretSpans;
    CaretSpans *m_spans = nullptr;
};

} // namespace vn
