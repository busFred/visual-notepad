// Text editor view over a QTextDocument with multiple cursors.
//
// The view lays out each line itself (QTextLayout per block, using the
// highlighter's formats from the document) and scrolls by "rows". In this
// class a row is one source line. Subclasses can merge runs of lines into a
// single rendered row (the visual Markdown view does this for blocks without
// a caret) by overriding the row hooks.
#pragma once

#include "../markdown/MarkdownEditing.h"
#include "Search.h"

#include <QAbstractScrollArea>
#include <QBasicTimer>
#include <QElapsedTimer>
#include <QTextCursor>
#include <QTextLayout>

#include <QString>

#include <map>
#include <memory>
#include <vector>

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QImage;
class QTextDocument;

namespace vn {

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

    // The primary caret.
    QTextCursor textCursor() const;
    // Replaces all carets with one.
    void setTextCursor(const QTextCursor &cursor);
    QList<QTextCursor> cursors() const;
    // Replaces all carets; primary is an index into the list.
    void setCursors(const QList<QTextCursor> &cursors, int primary);
    int cursorCount() const { return int(m_carets.size()); }
    // Selected text of every caret, joined by newlines, in document order.
    QString selectedText() const;

    bool hasSelection() const;
    // Shows another view's carets and selections (same document) without
    // emitting signals, taking focus or changing this view's own mouse state;
    // used to mirror the selection between the views of a split. The primary
    // caret is scrolled into view when reveal is set.
    void mirrorCursorsFrom(const TextView *other, bool reveal);

    void ensureCursorVisible();
    void centerCursor();
    // Space left and right of the text (the gutter is extra).
    void setTextMargins(qreal left, qreal right, qreal top = 0);

    // First line at the top of the viewport, and scrolling to put a line there.
    int topLine() const;
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
    void scrollPage(bool forward);
    // Inserts text at a document position and puts the only caret after it;
    // what a middle click does with the X11 primary selection.
    void pasteTextAt(int position, const QString &text);

    // Background highlights for find results; current is an index or -1.
    void setSearchHighlights(const QList<search::Range> &ranges, int current);

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

Q_SIGNALS:
    void cursorPositionChanged();
    void selectionChanged();
    void scrolled();
    // Files dropped without Shift (Markdown views insert links with Shift).
    void filesDropped(const QStringList &files);
    // Ctrl+Click (Alt+Click when Ctrl adds carets) on a link in Markdown text.
    void linkRequested(const QString &url);

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

    // The row at viewport y and its top, or -1 past the end.
    int rowAt(qreal y, qreal *top);

    // Left edge of text in viewport coordinates.
    qreal textLeft() const;
    qreal textWidth() const;
    int lineHeight() const { return m_lineHeight; }
    // Forgets cached row heights and repaints.
    void relayout();

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
    struct DiagnosticRange
    {
        QTextCursor start;
        QTextCursor end;
        QString message;
    };
    struct Caret
    {
        QTextCursor cursor;
        qreal desiredX = -1; // Remembered x for up/down movement.
    };
    struct LineCache
    {
        std::unique_ptr<QTextLayout> layout;
        qreal wrapWidth = -1;
    };
    enum class Unit { Char, Word, Line };

    void onDocumentChanged(int from, int charsRemoved, int charsAdded);
    void updateMetrics();
    void invalidateLayouts();
    // Drops cached layouts far from [firstLine, lastLine] once too many are live.
    void trimLayouts(int firstLine, int lastLine);
    void shiftSearchRanges(int from, int charsRemoved, int charsAdded);
    QTextLayout *layoutForLine(int line);
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
    void updateScrollBars();

    // Geometry. Returns false if the position is not in a visible row.
    bool rowTop(int row, qreal *top);
    QRectF caretRect(const QTextCursor &cursor);
    int positionAt(const QPointF &point);
    qreal cursorX(const QTextCursor &cursor);

    // Editing helpers.
    bool canEdit() const;
    void beginEdit();
    void endEdit();
    void insertTextAtCarets(const QString &text);
    void deleteAtCarets(QTextCursor::MoveOperation op);
    void insertNewline();
    void insertTab();
    // Auto-closing, overtyping and surrounding for one typed ASCII character.
    // Returns false when it should be inserted normally.
    bool typeCharacter(const QString &text);
    bool surroundSelections(QChar open);
    bool overtypeClosers(QChar ch);
    bool autoCloseBrackets(QChar open);
    bool deleteAutoClosedPair(Caret &caret);
    void pruneAutoClosed();
    // Distinct line numbers covered by the carets, ascending.
    QList<int> caretLines() const;
    // Merged [first, last] line ranges covered by the carets.
    QList<std::pair<int, int>> caretLineRanges() const;
    // Markdown helpers.
    QString linkAtPosition(int position);
    bool continueList(Caret &caret);
    void insertFileLinks(const QStringList &files);
    void pasteImage(const QImage &image);
    QString diagnosticAt(int position) const;
    void moveLines(int direction);
    void copyLines(int direction);
    QString indentUnit() const;
    // Markdown formatting (TextViewFormat.cpp).
    void replaceRange(int from, int to, const QString &text);
    void toggleInline(const QString &open, const QString &close);
    void setHeading(int level);
    void shiftHeading(int direction);
    void toggleLinePrefix(MarkdownFormat kind);
    void insertLink();
    void wrapInCodeBlock();

    // Caret helpers.
    void moveCarets(QTextCursor::MoveOperation op, bool select);
    void moveCaretsVertically(int lines, bool select);
    bool moveVertically(Caret &caret, int direction, QTextCursor::MoveMode mode);
    void moveCaretsHome(bool select);
    void normalizeCarets();
    void undoRedo(bool undo);
    void caretsChanged();
    QTextCursor wordAt(const QTextCursor &cursor) const;
    QTextCursor findNext(const QString &text, int from) const;
    void restartBlink();
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
    QList<QList<QTextCursor>> m_expandHistory;

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
    };
    ColumnAnchor m_columnAnchor;
    CaretState m_columnResult; // Carets the last box selection produced.
    bool m_columnAnchorValid = false;
    bool m_columnDragging = false;

    QList<search::Range> m_searchRanges; // Sorted, disjoint.
    int m_searchCurrent = -1;

    // Options.
    bool m_wordWrap = true;
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
    bool m_markdown = false;
    bool m_codeEditing = false;
    // Closers inserted by auto-closing that may still be typed over or
    // deleted with their opener. closer selects the closer; inner marks where
    // the text between the pair starts.
    struct AutoClosed
    {
        QTextCursor inner;
        QTextCursor closer;
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
    qreal m_maxLineWidth = 0;
    bool m_updatingScrollBars = false;

    // Mouse.
    bool m_dragging = false;
    Unit m_dragUnit = Unit::Char;
    QTextCursor m_dragOrigin;
    QBasicTimer m_autoScroll;
    QPointF m_lastMousePos;
    // Double click time and place, for detecting the triple click.
    QElapsedTimer m_doubleClickTime;
    QPointF m_doubleClickPos;

    // Caret blink and input method.
    QBasicTimer m_blink;
    bool m_caretOn = true;
    QString m_preedit;
    QList<QInputMethodEvent::Attribute> m_preeditAttributes;
};

} // namespace vn
