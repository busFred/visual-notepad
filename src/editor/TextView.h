// Text editor view over a QTextDocument with multiple cursors.
//
// The view lays out each line itself (QTextLayout per block, using the
// highlighter's formats from the document) and scrolls by "rows". In this
// class a row is one source line. Subclasses can merge runs of lines into a
// single rendered row (the visual Markdown view does this for blocks without
// a caret) by overriding the row hooks.
#pragma once

#include <QAbstractScrollArea>
#include <QBasicTimer>
#include <QTextCursor>
#include <QTextLayout>

#include <memory>
#include <vector>

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

    void ensureCursorVisible();
    void centerCursor();

    // Background highlights for find results; current is an index or -1.
    void setSearchHighlights(const QList<QTextCursor> &ranges, int current);

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

Q_SIGNALS:
    void cursorPositionChanged();
    void selectionChanged();

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

private:
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
    QTextLayout *layoutForLine(int line);
    QTextOption textOption() const;
    qreal rowHeight(int row);
    int gutterWidth() const;
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
    // Distinct line numbers covered by the carets, ascending.
    QList<int> caretLines() const;
    // Merged [first, last] line ranges covered by the carets.
    QList<std::pair<int, int>> caretLineRanges() const;
    void moveLines(int direction);
    void copyLines(int direction);
    QString indentUnit() const;

    // Caret helpers.
    void moveCarets(QTextCursor::MoveOperation op, bool select);
    void moveCaretsVertically(int lines, bool select);
    bool moveVertically(Caret &caret, int direction, QTextCursor::MoveMode mode);
    void moveCaretsHome(bool select);
    void normalizeCarets();
    void caretsChanged();
    QTextCursor wordAt(const QTextCursor &cursor) const;
    QTextCursor findNext(const QString &text, int from) const;
    void restartBlink();
    void selectByUnit(const QPointF &point, bool extend);

    QTextDocument *m_doc;
    std::vector<LineCache> m_lines;
    std::vector<Caret> m_carets;
    int m_primary = 0;
    QList<QList<QTextCursor>> m_expandHistory;

    QList<QTextCursor> m_searchRanges;
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

    // Caret blink and input method.
    QBasicTimer m_blink;
    bool m_caretOn = true;
    QString m_preedit;
    QList<QInputMethodEvent::Attribute> m_preeditAttributes;
};

} // namespace vn
