// Editing behaviour of the multi-cursor TextView, run offscreen.
#include "Theme.h"
#include "Settings.h"
#include "editor/DocumentLayout.h"
#include "editor/TableSizePicker.h"
#include "editor/TextView.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QClipboard>
#include <QTemporaryDir>
#include <QScrollBar>
#include <QMenu>
#include <QMouseEvent>
#include <QAbstractButton>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>
#include <QTextBlock>
#include <QTextDocument>

#include <tuple>

using vn::TextView;

// Exposes the geometry the mouse tests need.
class ProbeView : public TextView
{
public:
    using TextView::TextView;
    qreal left() const { return textLeft(); }
    int rowHeight() const { return lineHeight(); }
    int positionAtPoint(const QPointF &point) { return positionAt(point); }
};

class TestTextView : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    void columnsShowLinesInColumns();
    void columnsNumberEachColumnsLines();
    void columnsCutWrappedLinesBetweenRows();
    void columnsScrollSidewaysOneByOne();
    void columnsTurnPagesAndNudge();
    void columnsCaretBringsItsLine();
    void columnsDownGoesToTheNextColumnWithANote();
    void columnsMoveOnOneColumnAtTheLastColumn();
    void columnsTypingReflowsOverTheBreaks();
    void columnsOffsetsTravelWithTheText();
    void columnsEditAtSeveralCarets();
    void columnsStayFastOnABigFile();

    void typingAtSeveralCarets();
    void addNextOccurrenceThenType();
    void selectAllOccurrences();
    void pasteDistributesLines();
    void copyWithoutSelectionCopiesLine();
    void moveLinesKeepsSelection();
    void copyLinesDown();
    void deleteLines();
    void tabAndOutdent();
    void undoRevertsMultiCaretEditAtOnce();
    void undoRestoresCaretAtEdit();
    void toggleLineComment();
    void backspaceInIndentGoesToTabStop();
    void enterKeepsIndent();
    void smartHome();
    void escapeCollapsesCarets();
    void lineCacheFollowsEdits();
    void surroundSelection();
    void surroundRepeatsAndMultiCaret();
    void surroundPairsDependOnLanguage();
    void autoCloseConditions();
    void autoCloseOvertypeAndDelete();
    void plainTextDoesNotAutoClose();
    void autoCloseSettingsOff();
    void autoCloseIsOneUndoStep();
    void tabIndentsSingleLineSelection();
    void scrollBarOnlyWhenDocumentOverflows();
    void tabStaysInEditorBesideOtherWidgets();
    void addCursorsToLineEnds();
    void columnSelectWithMouse();
    void columnSelectUsesVisualColumns();
    void columnSelectWithCtrlModifier();
    void middleClickPastesPrimarySelection();
    void pasteTextAtPlacesCaretAfter();
    void expandLineSelection();
    void tableCommandsEditTheTableAsOneUndoStep();
    void tableCommandsNeedMarkdownAndATable();
    void tabMovesBetweenTableCells();
    void enterAddsATableRow();
    void insertingATable();
    void tablesInFencedCodeAreLeftAlone();
    void tableLinesAreFollowedAcrossEdits();
    void tableEditsReplaceOnlyTheChangedLines();
    void htmlTablesAreEditedToo();
    void tablesConvertBetweenPipesAndHtml();
    void tableSizePickerPicksASize();
    void cursorUndoAndRedo();
    void cursorHistoryDropsOnEditAndIsBounded();
    void moveLastSelectionToNextMatch();
    void indentAndOutdentActions();
    void trimTrailingWhitespace();
    void searchSeedText();
    void scrollPageKeepsCarets();

private:
    void setText(const QString &text);
    QString text() const;
    void type(const QString &text);
    void place(int line, int column);
    QPoint pointAt(int line, qreal column) const;
    QString describeCarets() const;
    void boxSelect(const QPoint &from, const QPoint &to, Qt::KeyboardModifiers mods);

    void selectRange(int from, int to);
    // Lines "line 1", "line 2"... in a view wide enough for two or more
    // columns of 50 characters, shown in columns.
    void showColumns(int lines);
    // The line at a point of the viewport (a position in it, in columns).
    int lineAtPoint(const QPointF &point);
    // Whether anything is drawn in a rectangle of the viewport.
    bool inked(const QImage &image, const QRect &rect);
    // Lines a column holds (none wrapped), and a point on a column's row
    // (-1: its last), a few characters into the text.
    int perColumn();
    QPointF columnPoint(int column, int row);
    int caretLine() const { return m_view->textCursor().blockNumber(); }
    // A key held down: a press the keyboard repeats.
    void repeatKey(Qt::Key key);

    QTextDocument *m_doc = nullptr;
    TextView *m_view = nullptr;
};

void TestTextView::selectRange(int from, int to)
{
    QTextCursor c(m_doc);
    c.setPosition(from);
    c.setPosition(to, QTextCursor::KeepAnchor);
    m_view->setTextCursor(c);
}

void TestTextView::init()
{
    vn::Settings::instance().reset(vn::key::autoClose);
    vn::Settings::instance().reset(vn::key::autoSurround);
    m_doc = new QTextDocument;
    m_doc->setDocumentLayout(new vn::DocumentLayout(m_doc));
    m_view = new ProbeView(m_doc);
    m_view->resize(600, 400);
    m_view->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_view));
    m_view->activateWindow();
    m_view->setFocus();
}

void TestTextView::cleanup()
{
    delete m_view;
    delete m_doc;
}

void TestTextView::setText(const QString &text)
{
    m_doc->setPlainText(text);
    m_view->setTextCursor(QTextCursor(m_doc));
}

QString TestTextView::text() const
{
    return m_doc->toPlainText();
}

void TestTextView::type(const QString &text)
{
    QTest::keyClicks(m_view, text);
}

void TestTextView::place(int line, int column)
{
    QTextCursor c(m_doc->findBlockByNumber(line));
    c.setPosition(c.position() + column);
    m_view->setTextCursor(c);
}

QString TestTextView::describeCarets() const
{
    QStringList parts;
    for (const QTextCursor &c : m_view->cursors())
        parts << QStringLiteral("%1:%2-%3:%4")
                     .arg(m_doc->findBlock(c.anchor()).blockNumber())
                     .arg(c.anchor() - m_doc->findBlock(c.anchor()).position())
                     .arg(c.blockNumber())
                     .arg(c.positionInBlock());
    return QStringLiteral("%1 carets (anchor-position as line:column): %2").arg(m_view->cursorCount()).arg(parts.join(u' '));
}

QPoint TestTextView::pointAt(int line, qreal column) const
{
    auto *probe = static_cast<ProbeView *>(m_view);
    const qreal cw = QFontMetricsF(m_view->font()).horizontalAdvance(QLatin1Char(' '));
    return QPoint(int(probe->left() + column * cw + 0.5), line * probe->rowHeight() + probe->rowHeight() / 2);
}

void TestTextView::boxSelect(const QPoint &from, const QPoint &to, Qt::KeyboardModifiers mods)
{
    QWidget *viewport = m_view->viewport();
    QTest::mousePress(viewport, Qt::LeftButton, mods, from);
    if (to != from) {
        QMouseEvent move(QEvent::MouseMove, QPointF(to), viewport->mapToGlobal(QPointF(to)), Qt::NoButton,
                         Qt::LeftButton, mods);
        QApplication::sendEvent(viewport, &move);
    }
    QTest::mouseRelease(viewport, Qt::LeftButton, mods, to);
}

void TestTextView::typingAtSeveralCarets()
{
    setText(QStringLiteral("one\ntwo\nthree"));
    m_view->addCursorBelow();
    m_view->addCursorBelow();
    QCOMPARE(m_view->cursorCount(), 3);
    type(QStringLiteral("> "));
    QCOMPARE(text(), QStringLiteral("> one\n> two\n> three"));
}

void TestTextView::addNextOccurrenceThenType()
{
    setText(QStringLiteral("foo bar foo baz foo"));
    m_view->addNextOccurrence(); // Selects the word at the caret.
    QCOMPARE(m_view->textCursor().selectedText(), QStringLiteral("foo"));
    m_view->addNextOccurrence();
    QCOMPARE(m_view->cursorCount(), 2);
    type(QStringLiteral("x"));
    QCOMPARE(text(), QStringLiteral("x bar x baz foo"));
}

void TestTextView::selectAllOccurrences()
{
    setText(QStringLiteral("a b a c a"));
    m_view->selectAllOccurrences();
    QCOMPARE(m_view->cursorCount(), 3);
    QCOMPARE(m_view->selectedText(), QStringLiteral("a\na\na"));
}

void TestTextView::pasteDistributesLines()
{
    setText(QStringLiteral("1\n2\n3"));
    m_view->addCursorBelow();
    m_view->addCursorBelow();
    QList<QTextCursor> carets = m_view->cursors();
    for (QTextCursor &c : carets)
        c.movePosition(QTextCursor::EndOfBlock);
    m_view->setCursors(carets, 0);
    QApplication::clipboard()->setText(QStringLiteral("a\nb\nc"));
    m_view->paste();
    QCOMPARE(text(), QStringLiteral("1a\n2b\n3c"));
}

void TestTextView::copyWithoutSelectionCopiesLine()
{
    setText(QStringLiteral("first\nsecond"));
    place(1, 2);
    m_view->copy();
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("second\n"));
    place(0, 3);
    m_view->paste();
    QCOMPARE(text(), QStringLiteral("second\nfirst\nsecond"));
}

void TestTextView::moveLinesKeepsSelection()
{
    setText(QStringLiteral("a\nb\nc\nd"));
    QTextCursor c(m_doc->findBlockByNumber(1));
    c.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor);
    c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor); // Selects "b\nc".
    m_view->setTextCursor(c);
    m_view->moveLinesDown();
    QCOMPARE(text(), QStringLiteral("a\nd\nb\nc"));
    QCOMPARE(m_view->textCursor().selectedText(), QStringLiteral("b c"));
    m_view->moveLinesUp();
    m_view->moveLinesUp();
    QCOMPARE(text(), QStringLiteral("b\nc\na\nd"));
    m_view->moveLinesUp(); // Already at the top: no change.
    QCOMPARE(text(), QStringLiteral("b\nc\na\nd"));
}

void TestTextView::copyLinesDown()
{
    setText(QStringLiteral("x\ny"));
    place(0, 1);
    m_view->copyLinesDown();
    QCOMPARE(text(), QStringLiteral("x\nx\ny"));
    QCOMPARE(m_view->textCursor().blockNumber(), 1);
}

void TestTextView::deleteLines()
{
    setText(QStringLiteral("a\nb\nc"));
    place(1, 0);
    m_view->deleteLines();
    QCOMPARE(text(), QStringLiteral("a\nc"));
    place(1, 0);
    m_view->deleteLines();
    QCOMPARE(text(), QStringLiteral("a"));
}

void TestTextView::tabAndOutdent()
{
    setText(QStringLiteral("ab\ncd"));
    place(0, 1);
    QTest::keyClick(m_view, Qt::Key_Tab);
    QCOMPARE(text(), QStringLiteral("a   b\ncd")); // To the next tab stop (4).
    m_view->selectAll();
    QTest::keyClick(m_view, Qt::Key_Tab);
    QCOMPARE(text(), QStringLiteral("    a   b\n    cd"));
    QTest::keyClick(m_view, Qt::Key_Backtab);
    QCOMPARE(text(), QStringLiteral("a   b\ncd"));
}

void TestTextView::tabStaysInEditorBesideOtherWidgets()
{
    // With another focusable widget in the window, Qt would move focus on Tab.
    QTextDocument doc;
    doc.setDocumentLayout(new vn::DocumentLayout(&doc));
    doc.setPlainText(QStringLiteral("ab"));
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *before = new QLineEdit(&window);
    auto *view = new vn::TextView(&doc, &window);
    auto *after = new QLineEdit(&window);
    layout->addWidget(before);
    layout->addWidget(view);
    layout->addWidget(after);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    view->setFocus();
    QTRY_VERIFY(view->hasFocus());
    QTest::keyClick(view, Qt::Key_Tab);
    QCOMPARE(doc.toPlainText(), QStringLiteral("    ab"));
    QVERIFY(view->hasFocus());
    QTest::keyClick(view, Qt::Key_Backtab);
    QCOMPARE(doc.toPlainText(), QStringLiteral("ab"));
    QVERIFY(view->hasFocus());
    // Read-only: nothing changes and focus stays.
    view->setReadOnly(true);
    QTest::keyClick(view, Qt::Key_Tab);
    QCOMPARE(doc.toPlainText(), QStringLiteral("ab"));
    QVERIFY(view->hasFocus());
}

void TestTextView::undoRevertsMultiCaretEditAtOnce()
{
    setText(QStringLiteral("a\nb\nc"));
    m_view->addCursorBelow();
    m_view->addCursorBelow();
    QTest::keyClick(m_view, Qt::Key_Delete);
    QCOMPARE(text(), QStringLiteral("\n\n"));
    m_view->undo();
    QCOMPARE(text(), QStringLiteral("a\nb\nc"));
    m_view->redo();
    QCOMPARE(text(), QStringLiteral("\n\n"));
}

void TestTextView::undoRestoresCaretAtEdit()
{
    QStringList lines;
    for (int i = 0; i < 200; ++i)
        lines << QStringLiteral("line %1").arg(i);
    setText(lines.join(u'\n'));
    place(120, 4);
    m_view->ensureCursorVisible();
    const int topBefore = m_view->topLine();
    const int startPos = m_view->textCursor().position();
    type(QStringLiteral("x"));
    m_view->undo();
    QCOMPARE(m_view->textCursor().position(), startPos);
    QCOMPARE(m_view->textCursor().blockNumber(), 120);
    QCOMPARE(m_view->topLine(), topBefore);
    m_view->redo();
    QCOMPARE(m_view->textCursor().position(), startPos + 1);
    QCOMPARE(m_view->textCursor().blockNumber(), 120);

    // Multi-caret edit: all carets are restored.
    place(100, 0);
    m_view->addCursorBelow();
    m_view->addCursorBelow();
    QTest::keyClick(m_view, Qt::Key_Delete);
    m_view->undo();
    QCOMPARE(m_view->cursorCount(), 3);
    QCOMPARE(m_view->textCursor().blockNumber(), 102);
    m_view->redo();
    QCOMPARE(m_view->cursorCount(), 3);

    // Two steps in a row, undone and redone.
    place(10, 2);
    type(QStringLiteral("a"));
    place(150, 1);
    QTest::keyClick(m_view, Qt::Key_Return);
    m_view->undo();
    QCOMPARE(m_view->textCursor().blockNumber(), 150);
    m_view->undo();
    QCOMPARE(m_view->textCursor().blockNumber(), 10);
    QCOMPARE(m_view->textCursor().positionInBlock(), 2);
    m_view->redo();
    m_view->redo();
    QCOMPARE(m_view->textCursor().blockNumber(), 151);
}

void TestTextView::toggleLineComment()
{
    m_view->setCommentMarker(QStringLiteral("#"));
    setText(QStringLiteral("  x = 1\n  y = 2"));
    m_view->selectAll();
    m_view->toggleLineComment();
    QCOMPARE(text(), QStringLiteral("  # x = 1\n  # y = 2"));
    m_view->toggleLineComment();
    QCOMPARE(text(), QStringLiteral("  x = 1\n  y = 2"));
}

void TestTextView::backspaceInIndentGoesToTabStop()
{
    setText(QStringLiteral("      x"));
    place(0, 6);
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QCOMPARE(text(), QStringLiteral("    x"));
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QCOMPARE(text(), QStringLiteral("x"));
}

void TestTextView::enterKeepsIndent()
{
    setText(QStringLiteral("    abc"));
    place(0, 7);
    QTest::keyClick(m_view, Qt::Key_Return);
    type(QStringLiteral("d"));
    QCOMPARE(text(), QStringLiteral("    abc\n    d"));
}

void TestTextView::smartHome()
{
    setText(QStringLiteral("    abc"));
    place(0, 6);
    QTest::keyClick(m_view, Qt::Key_Home);
    QCOMPARE(m_view->textCursor().positionInBlock(), 4);
    QTest::keyClick(m_view, Qt::Key_Home);
    QCOMPARE(m_view->textCursor().positionInBlock(), 0);
}

void TestTextView::escapeCollapsesCarets()
{
    setText(QStringLiteral("a\nb"));
    m_view->addCursorBelow();
    QCOMPARE(m_view->cursorCount(), 2);
    QTest::keyClick(m_view, Qt::Key_Escape);
    QCOMPARE(m_view->cursorCount(), 1);
}

void TestTextView::lineCacheFollowsEdits()
{
    // Structural edits must keep the per-line layout cache in step with the
    // document; a mismatch shows up as wrong text being painted.
    setText(QStringLiteral("l0\nl1\nl2\nl3"));
    QTextCursor c(m_doc->findBlockByNumber(1));
    c.insertText(QStringLiteral("new\nlines\n"));
    c.movePosition(QTextCursor::End);
    c.movePosition(QTextCursor::PreviousBlock, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    QCOMPARE(m_doc->blockCount(), 5);
    QVERIFY(!m_view->grab().isNull());
    m_doc->setPlainText(QStringLiteral("only"));
    QVERIFY(!m_view->grab().isNull());
    QCOMPARE(m_doc->blockCount(), 1);
}

void TestTextView::addCursorsToLineEnds()
{
    setText(QStringLiteral("abc\ndef\nghi\njkl"));
    // From "bc..." on line 0 to just after "gh" on line 2.
    QTextCursor c(m_doc);
    c.setPosition(1);
    c.setPosition(m_doc->findBlockByNumber(2).position() + 2, QTextCursor::KeepAnchor);
    m_view->setTextCursor(c);
    m_view->addCursorsToLineEnds();
    QCOMPARE(m_view->cursorCount(), 3);
    const QList<QTextCursor> carets = m_view->cursors();
    QCOMPARE(carets[0].position(), 3);
    QCOMPARE(carets[1].position(), 7);
    QCOMPARE(carets[2].position(), m_doc->findBlockByNumber(2).position() + 2);
    for (const QTextCursor &caret : carets)
        QVERIFY(!caret.hasSelection());

    // A selection that ends at the start of a line gets no caret there.
    c.setPosition(1);
    c.setPosition(m_doc->findBlockByNumber(2).position(), QTextCursor::KeepAnchor);
    m_view->setTextCursor(c);
    m_view->addCursorsToLineEnds();
    QCOMPARE(m_view->cursorCount(), 2);

    // Without a selection nothing changes.
    place(1, 1);
    m_view->addCursorsToLineEnds();
    QCOMPARE(m_view->cursorCount(), 1);
    QCOMPARE(m_view->textCursor().position(), m_doc->findBlockByNumber(1).position() + 1);
}

void TestTextView::columnSelectWithMouse()
{
    setText(QStringLiteral("xxxxxxxx\nxx\nxxxxxxxx\nxxxxxxxx"));
    place(0, 1);
    // Shift+Alt+click extends a box from the caret.
    boxSelect(pointAt(2, 4), pointAt(2, 4), Qt::ShiftModifier | Qt::AltModifier);
    // Line 1 is too short to reach column 4 but still reaches column 1.
    QCOMPARE(m_view->cursorCount(), 3);
    const QList<QTextCursor> carets = m_view->cursors();
    QCOMPARE(carets[0].selectedText(), QStringLiteral("xxx"));
    QCOMPARE(carets[1].selectedText(), QStringLiteral("x"));
    QCOMPARE(carets[2].selectedText(), QStringLiteral("xxx"));
    QCOMPARE(carets[2].position(), m_doc->findBlockByNumber(2).position() + 4);

    // Dragging a second box continues from the same start.
    boxSelect(pointAt(3, 3), pointAt(3, 3), Qt::ShiftModifier | Qt::AltModifier);
    QVERIFY2(m_view->cursorCount() == 4, qPrintable(describeCarets()));
    QCOMPARE(m_view->cursors()[3].selectedText(), QStringLiteral("xx"));

    // Dragging upwards keeps one selection per line, anchored at the start.
    place(3, 6);
    boxSelect(pointAt(0, 2), pointAt(0, 2), Qt::ShiftModifier | Qt::AltModifier);
    QCOMPARE(m_view->cursorCount(), 4);
    QCOMPARE(m_view->cursors()[0].selectedText(), QStringLiteral("xxxx"));
    QCOMPARE(m_view->cursors()[1].selectedText(), QString());

    // Typing replaces every box line.
    type(QStringLiteral("-"));
    QVERIFY(text().startsWith(QStringLiteral("xx-")));
}

void TestTextView::columnSelectUsesVisualColumns()
{
    // A tab runs to the next tab stop, so it spans several columns.
    m_view->setTabWidth(4);
    setText(QStringLiteral("a\tb\nabcdefgh\nzz"));
    place(0, 0);
    const QPoint tabEnd = pointAt(1, 4);
    boxSelect(tabEnd, tabEnd, Qt::ShiftModifier | Qt::AltModifier);
    const QList<QTextCursor> carets = m_view->cursors();
    QCOMPARE(carets.size(), 2);
    QCOMPARE(carets[0].selectedText(), QStringLiteral("a\t"));
    QCOMPARE(carets[1].selectedText(), QStringLiteral("abcd"));

    // Every line shorter than the box: a caret at each line end.
    setText(QStringLiteral("a\nbc\nd"));
    place(0, 0);
    const QPoint far = pointAt(2, 20);
    boxSelect(far, far, Qt::ShiftModifier | Qt::AltModifier);
    QCOMPARE(m_view->cursorCount(), 3);
    for (const QTextCursor &caret : m_view->cursors())
        QCOMPARE(caret.position(), caret.block().position() + caret.block().length() - 1);
}

void TestTextView::columnSelectWithCtrlModifier()
{
    m_view->setMultiCursorModifier(Qt::ControlModifier);
    setText(QStringLiteral("xxxx\nxxxx"));
    place(0, 1);
    boxSelect(pointAt(1, 3), pointAt(1, 3), Qt::ShiftModifier | Qt::ControlModifier);
    QCOMPARE(m_view->cursorCount(), 2);
    QCOMPARE(m_view->selectedText(), QStringLiteral("xx\nxx"));
    // Shift+Alt is then an ordinary shift-click.
    place(0, 1);
    boxSelect(pointAt(1, 3), pointAt(1, 3), Qt::ShiftModifier | Qt::AltModifier);
    QCOMPARE(m_view->cursorCount(), 1);
}

void TestTextView::middleClickPastesPrimarySelection()
{
    QClipboard *clipboard = QApplication::clipboard();
    if (!clipboard->supportsSelection())
        QSKIP("This platform has no primary selection");
    setText(QStringLiteral("abcdef"));
    clipboard->setText(QStringLiteral("XY"), QClipboard::Selection);
    QTest::mouseClick(m_view->viewport(), Qt::MiddleButton, {}, pointAt(0, 3));
    QCOMPARE(text(), QStringLiteral("abcXYdef"));
    QCOMPARE(m_view->textCursor().position(), 5);
}

void TestTextView::pasteTextAtPlacesCaretAfter()
{
    setText(QStringLiteral("abcdef"));
    m_view->addCursorBelow(); // Single line: stays one caret.
    m_view->pasteTextAt(3, QStringLiteral("X\r\nY"));
    QCOMPARE(text(), QStringLiteral("abcX\nYdef"));
    QCOMPARE(m_view->cursorCount(), 1);
    QCOMPARE(m_view->textCursor().position(), 6);
    m_view->undo();
    QCOMPARE(text(), QStringLiteral("abcdef"));
}

void TestTextView::expandLineSelection()
{
    setText(QStringLiteral("l0\nl1\nl2"));
    place(1, 1);
    m_view->expandLineSelection();
    QCOMPARE(m_view->textCursor().selectedText().replace(QChar::ParagraphSeparator, u'|'), QStringLiteral("l1|"));
    m_view->expandLineSelection();
    QCOMPARE(m_view->textCursor().selectedText().replace(QChar::ParagraphSeparator, u'|'), QStringLiteral("l1|l2"));
    // The last line has no break to include.
    m_view->expandLineSelection();
    QCOMPARE(m_view->textCursor().selectedText().replace(QChar::ParagraphSeparator, u'|'), QStringLiteral("l1|l2"));

    // Every caret expands on its own; touching ones merge.
    place(0, 0);
    QTextCursor second(m_doc->findBlockByNumber(2));
    m_view->setCursors({m_view->textCursor(), second}, 1);
    m_view->expandLineSelection();
    QCOMPARE(m_view->cursorCount(), 2);
    m_view->expandLineSelection(); // Touching selections stay apart...
    QCOMPARE(m_view->cursorCount(), 2);
    m_view->expandLineSelection(); // ...until they overlap.
    QCOMPARE(m_view->cursorCount(), 1);
    QCOMPARE(m_view->textCursor().selectionStart(), 0);
    QCOMPARE(m_view->textCursor().selectionEnd(), m_doc->characterCount() - 1);
}

void TestTextView::cursorUndoAndRedo()
{
    setText(QStringLiteral("one two three\nfour"));
    place(0, 0);
    place(0, 4);
    place(0, 8);
    m_view->cursorUndo();
    QCOMPARE(m_view->textCursor().position(), 4);
    m_view->cursorUndo();
    QCOMPARE(m_view->textCursor().position(), 0);
    m_view->cursorUndo(); // Nothing earlier.
    QCOMPARE(m_view->textCursor().position(), 0);
    m_view->cursorRedo();
    QCOMPARE(m_view->textCursor().position(), 4);
    m_view->cursorRedo();
    QCOMPARE(m_view->textCursor().position(), 8);
    m_view->cursorRedo();
    QCOMPARE(m_view->textCursor().position(), 8);

    // Selections and several carets are restored too.
    m_view->addNextOccurrence(); // Selects the word.
    m_view->addCursorBelow();
    m_view->cursorUndo();
    m_view->cursorUndo();
    QCOMPARE(m_view->cursorCount(), 1);
    QVERIFY(!m_view->textCursor().hasSelection());
    QCOMPARE(m_view->textCursor().position(), 8);

    // A new move forgets the redo states.
    place(0, 2);
    m_view->cursorRedo();
    QCOMPARE(m_view->textCursor().position(), 2);
}

void TestTextView::cursorHistoryDropsOnEditAndIsBounded()
{
    setText(QStringLiteral("abc"));
    place(0, 1);
    place(0, 2);
    type(QStringLiteral("x")); // A change in the text forgets the history.
    m_view->cursorUndo();
    QCOMPARE(m_view->textCursor().position(), 3);

    QString many;
    for (int i = 0; i < 80; ++i)
        many += QLatin1Char('a');
    setText(many);
    for (int i = 1; i <= 70; ++i)
        place(0, i);
    for (int i = 0; i < 100; ++i)
        m_view->cursorUndo();
    // Only the last 50 states are kept: 70 - 50 = 20.
    QCOMPARE(m_view->textCursor().position(), 20);
}

void TestTextView::moveLastSelectionToNextMatch()
{
    setText(QStringLiteral("foo bar foo baz foo"));
    m_view->addNextOccurrence(); // The word at the caret.
    m_view->addNextOccurrence(); // The second foo.
    QCOMPARE(m_view->cursorCount(), 2);
    m_view->moveLastSelectionToNextMatch();
    QCOMPARE(m_view->cursorCount(), 2);
    const QList<QTextCursor> carets = m_view->cursors();
    QCOMPARE(carets[0].selectionStart(), 0);
    QCOMPARE(carets[1].selectionStart(), 16); // The third foo, not the second.
    m_view->moveLastSelectionToNextMatch();   // Wraps around onto the first.
    QCOMPARE(m_view->cursorCount(), 1);

    // With nothing selected it selects the word, like Ctrl+D.
    setText(QStringLiteral("alpha beta"));
    place(0, 7);
    m_view->moveLastSelectionToNextMatch();
    QCOMPARE(m_view->textCursor().selectedText(), QStringLiteral("beta"));
}

void TestTextView::indentAndOutdentActions()
{
    setText(QStringLiteral("a\nb\nc"));
    m_view->setInsertSpaces(true);
    m_view->setTabWidth(2);
    QTextCursor c(m_doc);
    c.setPosition(0);
    c.setPosition(3, QTextCursor::KeepAnchor);
    m_view->setTextCursor(c);
    m_view->indent();
    QCOMPARE(text(), QStringLiteral("  a\n  b\nc"));
    m_view->outdent();
    QCOMPARE(text(), QStringLiteral("a\nb\nc"));
}

void TestTextView::trimTrailingWhitespace()
{
    setText(QStringLiteral("a  \n\t \nb\t\nc"));
    place(0, 3); // A caret inside the trailing blanks is trimmed too.
    m_view->trimTrailingWhitespace();
    QCOMPARE(text(), QStringLiteral("a\n\nb\nc"));
    QCOMPARE(m_view->textCursor().position(), 1);
    m_view->undo(); // One undo step for all lines.
    QCOMPARE(text(), QStringLiteral("a  \n\t \nb\t\nc"));
    m_view->redo();
    QCOMPARE(text(), QStringLiteral("a\n\nb\nc"));
}

void TestTextView::searchSeedText()
{
    setText(QStringLiteral("hello world\nnext"));
    place(0, 7);
    QCOMPARE(m_view->searchSeedText(), QStringLiteral("world"));
    QTextCursor c(m_doc);
    c.setPosition(2);
    c.setPosition(5, QTextCursor::KeepAnchor);
    m_view->setTextCursor(c);
    QCOMPARE(m_view->searchSeedText(), QStringLiteral("llo"));
    m_view->selectAll();
    QCOMPARE(m_view->searchSeedText(), QString()); // Multi-line.
    setText(QStringLiteral("a  b"));
    place(0, 2);
    QCOMPARE(m_view->searchSeedText(), QString());
}

void TestTextView::scrollPageKeepsCarets()
{
    QStringList lines;
    for (int i = 0; i < 300; ++i)
        lines << QStringLiteral("line %1").arg(i);
    setText(lines.join(u'\n'));
    place(5, 2);
    const int caret = m_view->textCursor().position();
    QCOMPARE(m_view->topLine(), 0);
    m_view->scrollPage(true);
    const int first = m_view->topLine();
    QVERIFY(first > 5);
    const int visible = m_view->viewport()->height() / static_cast<ProbeView *>(m_view)->rowHeight();
    QCOMPARE(first, visible - 2);
    QCOMPARE(m_view->textCursor().position(), caret);
    m_view->scrollPage(true);
    QCOMPARE(m_view->topLine(), 2 * (visible - 2));
    m_view->scrollPage(false);
    QCOMPARE(m_view->topLine(), first);
    m_view->scrollPage(false);
    m_view->scrollPage(false);
    QCOMPARE(m_view->topLine(), 0);
    QCOMPARE(m_view->textCursor().position(), caret);
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

void TestTextView::surroundSelection()
{
    m_view->setMarkdownEditing(true);
    setText(QStringLiteral("a word b"));
    selectRange(2, 6);
    type(QStringLiteral("("));
    QCOMPARE(text(), QStringLiteral("a (word) b"));
    QCOMPARE(m_view->textCursor().selectedText(), QStringLiteral("word"));
}

void TestTextView::surroundRepeatsAndMultiCaret()
{
    m_view->setMarkdownEditing(true);
    setText(QStringLiteral("word"));
    m_view->selectAll();
    type(QStringLiteral("**"));
    QCOMPARE(text(), QStringLiteral("**word**"));
    QCOMPARE(m_view->textCursor().selectedText(), QStringLiteral("word"));

    setText(QStringLiteral("foo bar foo"));
    m_view->addNextOccurrence();
    m_view->addNextOccurrence();
    QCOMPARE(m_view->cursorCount(), 2);
    type(QStringLiteral("["));
    QCOMPARE(text(), QStringLiteral("[foo] bar [foo]"));
    type(QStringLiteral("x"));
    QCOMPARE(text(), QStringLiteral("[x] bar [x]"));
}

void TestTextView::surroundPairsDependOnLanguage()
{
    setText(QStringLiteral("word"));
    m_view->selectAll();
    type(QStringLiteral("*"));
    QCOMPARE(text(), QStringLiteral("*")); // Not a plain-text surrounding pair.
    setText(QStringLiteral("word"));
    m_view->selectAll();
    type(QStringLiteral("\""));
    QCOMPARE(text(), QStringLiteral("\"word\""));
    m_view->setMarkdownEditing(true);
    setText(QStringLiteral("word"));
    m_view->selectAll();
    type(QStringLiteral("~"));
    QCOMPARE(text(), QStringLiteral("~word~"));
}

void TestTextView::autoCloseConditions()
{
    m_view->setMarkdownEditing(true);
    setText(QString());
    type(QStringLiteral("("));
    QCOMPARE(text(), QStringLiteral("()"));
    // Markdown has no quote pairs.
    setText(QString());
    type(QStringLiteral("\""));
    QCOMPARE(text(), QStringLiteral("\""));
    // Not before a word character.
    setText(QStringLiteral("word"));
    type(QStringLiteral("("));
    QCOMPARE(text(), QStringLiteral("(word"));
    m_view->setMarkdownEditing(false);
    m_view->setCodeEditing(true);
    setText(QString());
    type(QStringLiteral("'"));
    QCOMPARE(text(), QStringLiteral("''"));
    // Not after a word character for quotes.
    setText(QStringLiteral("don"));
    place(0, 3);
    type(QStringLiteral("'"));
    QCOMPARE(text(), QStringLiteral("don'"));
    // Brackets still close after a word.
    type(QStringLiteral("("));
    QCOMPARE(text(), QStringLiteral("don'()"));
    // Multi-caret.
    setText(QStringLiteral("\n"));
    m_view->addCursorBelow();
    type(QStringLiteral("["));
    QCOMPARE(text(), QStringLiteral("[]\n[]"));
}

void TestTextView::autoCloseOvertypeAndDelete()
{
    m_view->setCodeEditing(true);
    setText(QString());
    type(QStringLiteral("(a)"));
    QCOMPARE(text(), QStringLiteral("(a)"));
    QCOMPARE(m_view->textCursor().position(), 3);
    // A closer that was not auto-inserted is not typed over.
    setText(QStringLiteral("a)"));
    place(0, 1);
    type(QStringLiteral(")"));
    QCOMPARE(text(), QStringLiteral("a))"));
    // Backspace removes an empty auto-inserted pair.
    setText(QString());
    type(QStringLiteral("{"));
    QCOMPARE(text(), QStringLiteral("{}"));
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QCOMPARE(text(), QString());
    // But not a pair that was typed by hand.
    setText(QStringLiteral("()"));
    place(0, 1);
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QCOMPARE(text(), QStringLiteral(")"));
    // Nested pairs close from the inside out.
    setText(QString());
    type(QStringLiteral("([x])"));
    QCOMPARE(text(), QStringLiteral("([x])"));
    QCOMPARE(m_view->textCursor().position(), 5);
}

void TestTextView::plainTextDoesNotAutoClose()
{
    setText(QString());
    type(QStringLiteral("([\"'`"));
    QCOMPARE(text(), QStringLiteral("([\"'`"));
}

void TestTextView::autoCloseSettingsOff()
{
    m_view->setCodeEditing(true);
    vn::Settings::instance().setValue(vn::key::autoClose, false);
    setText(QString());
    type(QStringLiteral("("));
    QCOMPARE(text(), QStringLiteral("("));
    vn::Settings::instance().setValue(vn::key::autoSurround, false);
    setText(QStringLiteral("word"));
    m_view->selectAll();
    type(QStringLiteral("("));
    QCOMPARE(text(), QStringLiteral("("));
    vn::Settings::instance().reset(vn::key::autoClose);
    vn::Settings::instance().reset(vn::key::autoSurround);
}

void TestTextView::autoCloseIsOneUndoStep()
{
    m_view->setCodeEditing(true);
    setText(QString());
    type(QStringLiteral("("));
    QCOMPARE(text(), QStringLiteral("()"));
    m_view->undo();
    QCOMPARE(text(), QString());
    setText(QStringLiteral("word"));
    m_view->selectAll();
    type(QStringLiteral("("));
    QCOMPARE(text(), QStringLiteral("(word)"));
    m_view->undo();
    QCOMPARE(text(), QStringLiteral("word"));
}

void TestTextView::tabIndentsSingleLineSelection()
{
    setText(QStringLiteral("abc def"));
    selectRange(1, 3);
    QTest::keyClick(m_view, Qt::Key_Tab);
    QCOMPARE(text(), QStringLiteral("    abc def"));
    QTest::keyClick(m_view, Qt::Key_Backtab);
    QCOMPARE(text(), QStringLiteral("abc def"));
}

void TestTextView::scrollBarOnlyWhenDocumentOverflows()
{
    setText(QStringLiteral("one\ntwo\nthree"));
    QCOMPARE(m_view->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
    // The last line can still scroll to the top, like VS Code.
    QVERIFY(m_view->verticalScrollBar()->maximum() > 0);
    QStringList lines;
    for (int i = 0; i < 200; ++i)
        lines << QString::number(i);
    setText(lines.join(QLatin1Char('\n')));
    QCOMPARE(m_view->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    setText(QStringLiteral("short"));
    QCOMPARE(m_view->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
}

namespace {
const QString smallTable = QStringLiteral("| a   | b   |\n| --- | --- |\n| 1   | 2   |");
}

void TestTextView::tableCommandsEditTheTableAsOneUndoStep()
{
    m_view->setMarkdownEditing(true);
    setText(QStringLiteral("intro\n\n") + smallTable + QStringLiteral("\n\nend"));
    place(4, 8); // In the second cell of the last row.
    const vn::md::TableInfo info = m_view->tableAtCaret();
    QVERIFY(info.found);
    QCOMPARE(info.row, 1);
    QCOMPARE(info.col, 1);
    QVERIFY(m_view->editTable(vn::md::TableOp::ColumnRight));
    QCOMPARE(text(), QStringLiteral("intro\n\n| a   | b   |     |\n| --- | --- | --- |\n| 1   | 2   |     |\n\nend"));
    // The caret is in the new column of the same row.
    QCOMPARE(m_view->textCursor().blockNumber(), 4);
    QCOMPARE(m_view->textCursor().positionInBlock(), 14);
    m_view->undo();
    QCOMPARE(text(), QStringLiteral("intro\n\n") + smallTable + QStringLiteral("\n\nend"));
    // Deleting the table takes its lines.
    QVERIFY(m_view->editTable(vn::md::TableOp::DeleteTable));
    QCOMPARE(text(), QStringLiteral("intro\n\n\nend"));
}

void TestTextView::tableCommandsNeedMarkdownAndATable()
{
    setText(smallTable);
    place(0, 2);
    QVERIFY(!m_view->tableAtCaret().found); // Not a Markdown view.
    QVERIFY(!m_view->editTable(vn::md::TableOp::RowBelow));
    m_view->setMarkdownEditing(true);
    setText(QStringLiteral("plain | text |\n\n") + smallTable);
    place(0, 3);
    QVERIFY(!m_view->editTable(vn::md::TableOp::RowBelow));
    place(4, 2);
    m_view->setReadOnly(true);
    QVERIFY(!m_view->editTable(vn::md::TableOp::RowBelow));
}

void TestTextView::tabMovesBetweenTableCells()
{
    m_view->setMarkdownEditing(true);
    // Not aligned: Tab selects the next cell's text without rewriting.
    const QString raw = QStringLiteral("|a|bb|\n|-|-|\n|1|2|");
    setText(raw);
    place(0, 2);
    QTest::keyClick(m_view, Qt::Key_Tab);
    QCOMPARE(m_view->textCursor().selectedText(), QStringLiteral("bb"));
    QTest::keyClick(m_view, Qt::Key_Tab); // To the next row.
    QCOMPARE(m_view->textCursor().selectedText(), QStringLiteral("1"));
    QTest::keyClick(m_view, Qt::Key_Backtab);
    QCOMPARE(m_view->textCursor().selectedText(), QStringLiteral("bb"));
    QCOMPARE(text(), raw);
    // After the last cell a row is added.
    place(2, 1);
    QTest::keyClick(m_view, Qt::Key_Tab);
    QCOMPARE(m_view->textCursor().blockNumber(), 2);
    QTest::keyClick(m_view, Qt::Key_Tab);
    QCOMPARE(m_view->document()->blockCount(), 4);
    QCOMPARE(m_view->textCursor().blockNumber(), 3);
    // Outside a table Tab still indents.
    setText(QStringLiteral("text"));
    place(0, 0);
    QTest::keyClick(m_view, Qt::Key_Tab);
    QVERIFY(text() != QStringLiteral("text"));
}

void TestTextView::enterAddsATableRow()
{
    m_view->setMarkdownEditing(true);
    setText(smallTable);
    place(0, 3);
    QTest::keyClick(m_view, Qt::Key_Return);
    QCOMPARE(text(), QStringLiteral("| a   | b   |\n| --- | --- |\n|     |     |\n| 1   | 2   |"));
    QCOMPARE(m_view->textCursor().blockNumber(), 2);
    // Shift+Enter is an ordinary line break.
    setText(smallTable);
    place(2, 3);
    QTest::keyClick(m_view, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(m_view->document()->blockCount(), 4);
}

void TestTextView::insertingATable()
{
    m_view->setMarkdownEditing(true);
    setText(QStringLiteral("para\nnext"));
    place(0, 2);
    m_view->insertTable(3, 2);
    QCOMPARE(text(), QStringLiteral("para\n\n| Column 1 | Column 2 |\n| -------- | -------- |\n"
                                    "|          |          |\n|          |          |\n\nnext"));
    QCOMPARE(m_view->textCursor().selectedText(), QStringLiteral("Column 1"));
    QVERIFY(m_view->tableAtCaret().found);
    // On an empty line the table takes its place.
    setText(QString());
    m_view->insertTable(2, 1);
    QCOMPARE(text(), QStringLiteral("| Column 1 |\n| -------- |\n|          |"));
}

void TestTextView::tablesInFencedCodeAreLeftAlone()
{
    m_view->setMarkdownEditing(true);
    setText(QStringLiteral("```\n") + smallTable + QStringLiteral("\n```\n\n") + smallTable);
    place(3, 3);
    QVERIFY(!m_view->tableAtCaret().found);
    QVERIFY(!m_view->editTable(vn::md::TableOp::RowBelow));
    place(7, 3); // The table after the fence is one.
    QVERIFY(m_view->tableAtCaret().found);
    QCOMPARE(m_view->tableAtCaret().firstLine, 6);
    // Taking the opening fence away frees the first table and encloses the
    // second one (the fences are followed across edits).
    QTextCursor c(m_doc->findBlockByNumber(0));
    c.select(QTextCursor::BlockUnderCursor);
    c.removeSelectedText();
    place(2, 3);
    QVERIFY(m_view->tableAtCaret().found);
    QCOMPARE(m_view->tableAtCaret().firstLine, 1);
    place(6, 3);
    QVERIFY(!m_view->tableAtCaret().found);
    // A tilde fence does not close a backtick one.
    setText(QStringLiteral("```\n~~~\n") + smallTable);
    place(3, 3);
    QVERIFY(!m_view->tableAtCaret().found);
}

void TestTextView::tableLinesAreFollowedAcrossEdits()
{
    // The lines of the caret's table are kept from one ask to the next and
    // patched by edits in them; edits around them take them back to the text.
    m_view->setMarkdownEditing(true);
    setText(QStringLiteral("text\n") + smallTable + QStringLiteral("\n| 3   | 4   |\nend"));
    place(3, 3);
    QCOMPARE(m_view->tableAtCaret().firstLine, 1);
    QCOMPARE(m_view->tableAtCaret().rows, 3);
    type(QStringLiteral("xx"));
    QCOMPARE(m_view->tableAtCaret().rows, 3);
    QVERIFY(!m_view->tableAtCaret().aligned);
    QTest::keyClick(m_view, Qt::Key_Return); // A row inside: 4 rows.
    QCOMPARE(m_view->tableAtCaret().rows, 4);
    QCOMPARE(m_view->tableAtCaret().lastLine, 5);
    // The line after the last row stops being one.
    place(5, 13);
    type(QStringLiteral("z"));
    QVERIFY(!m_view->tableAtCaret().found);
    place(4, 3);
    QCOMPARE(m_view->tableAtCaret().rows, 3);
    QCOMPARE(m_view->tableAtCaret().lastLine, 4);
    // A line taken out above moves the table up; one put in moves it down.
    QTextCursor c(m_doc->findBlockByNumber(0));
    c.select(QTextCursor::BlockUnderCursor);
    c.removeSelectedText();
    c.deleteChar();
    place(3, 3);
    QCOMPARE(m_view->tableAtCaret().firstLine, 0);
    c.insertText(QStringLiteral("| p | q |\n"));
    place(4, 3);
    QCOMPARE(m_view->tableAtCaret().firstLine, 1);
    QCOMPARE(m_view->tableAtCaret().rows, 3);
}

void TestTextView::tableEditsReplaceOnlyTheChangedLines()
{
    // A realigned table is mostly as it was: only the lines that differ are
    // replaced, so a long table's edit stays a small change of the document.
    m_view->setMarkdownEditing(true);
    setText(QStringLiteral("text\n\n") + smallTable + QStringLiteral("\n| 3 | 4 |\n\nend"));
    QList<std::tuple<int, int, int>> changes;
    connect(m_doc, &QTextDocument::contentsChange, this,
            [&changes](int from, int removed, int added) { changes.append({from, removed, added}); });
    place(5, 3);
    QVERIFY(m_view->editTable(vn::md::TableOp::Realign));
    QCOMPARE(m_doc->findBlockByNumber(5).text(), QStringLiteral("| 3   | 4   |"));
    QCOMPARE(text(), QStringLiteral("text\n\n") + smallTable + QStringLiteral("\n| 3   | 4   |\n\nend"));
    QCOMPARE(changes.size(), 1);
    QCOMPARE(std::get<0>(changes.first()), m_doc->findBlockByNumber(5).position());
    QCOMPARE(std::get<1>(changes.first()), 9);
    QCOMPARE(std::get<2>(changes.first()), 13);
    // A row added in the middle: the lines before it stay, as do the ones after.
    changes.clear();
    place(4, 3);
    QVERIFY(m_view->editTable(vn::md::TableOp::RowAbove));
    QCOMPARE(m_doc->blockCount(), 9);
    QCOMPARE(changes.size(), 1);
    QVERIFY(std::get<0>(changes.first()) >= m_doc->findBlockByNumber(4).position());
    QVERIFY(std::get<2>(changes.first()) < 2 * 14);
    // Undo puts the table back as it was, in one step.
    m_view->undo();
    QCOMPARE(text(), QStringLiteral("text\n\n") + smallTable + QStringLiteral("\n| 3   | 4   |\n\nend"));
}

namespace {

namespace md = vn::md;

const QString htmlTable = QStringLiteral("<table>\n"
                                         "  <tr>\n"
                                         "    <th>Name</th>\n"
                                         "    <th>Qty</th>\n"
                                         "  </tr>\n"
                                         "  <tr>\n"
                                         "    <td>pear</td>\n"
                                         "    <td>3</td>\n"
                                         "  </tr>\n"
                                         "</table>");

} // namespace

void TestTextView::htmlTablesAreEditedToo()
{
    m_view->setMarkdownEditing(true);
    setText(QStringLiteral("text\n\n") + htmlTable + QStringLiteral("\n\nend"));
    // The caret in "pear" (line 8): row 1, column 0 of an HTML table.
    place(8, 9);
    md::TableInfo info = m_view->tableAtCaret();
    QVERIFY(info.found && info.html);
    QCOMPARE(info.rows, 2);
    QCOMPARE(info.cols, 2);
    QCOMPARE(info.row, 1);
    QCOMPARE(info.col, 0);
    QCOMPARE(info.firstLine, 2);
    QCOMPARE(info.lastLine, 11);
    QVERIFY(!md::tableOpAvailable(info, md::TableOp::Realign));
    QVERIFY(!md::tableOpAvailable(info, md::TableOp::ToHtml));
    // Not on a line of the table, or in plain text: no table.
    place(0, 1);
    QVERIFY(!m_view->tableAtCaret().found);
    // A row below: the same layout, empty cells, one undo step.
    place(8, 9);
    QVERIFY(m_view->editTable(md::TableOp::RowBelow));
    QCOMPARE(text(), QStringLiteral("text\n\n<table>\n  <tr>\n    <th>Name</th>\n    <th>Qty</th>\n  </tr>\n"
                                    "  <tr>\n    <td>pear</td>\n    <td>3</td>\n  </tr>\n"
                                    "  <tr>\n    <td></td>\n    <td></td>\n  </tr>\n</table>\n\nend"));
    m_view->undo();
    QCOMPARE(text(), QStringLiteral("text\n\n") + htmlTable + QStringLiteral("\n\nend"));
    // Enter and Tab are the source's own in HTML: no row, no cell jump.
    place(8, 9);
    QTest::keyClick(m_view, Qt::Key_Return);
    QCOMPARE(m_doc->blockCount(), 15);
    m_view->undo();
    // A cell is written as given; a line break is a <br>.
    place(8, 9);
    info = m_view->tableAtCaret();
    QVERIFY(m_view->editTable(md::TableOp::SetCell, QStringLiteral("big\npear")));
    QCOMPARE(m_doc->findBlockByNumber(8).text(), QStringLiteral("    <td>big<br>pear</td>"));
    // Moving to a cell selects its text.
    QVERIFY(m_view->moveToTableCell(2, 1, 1));
    QCOMPARE(m_view->textCursor().selectedText(), QStringLiteral("3"));
}

void TestTextView::tablesConvertBetweenPipesAndHtml()
{
    m_view->setMarkdownEditing(true);
    // A pipe table with a paragraph right under it: the HTML gets a blank
    // line after it, or the paragraph would be part of the HTML block.
    setText(QStringLiteral("| a | **b** |\n| --- | :-: |\n| 1 | 2 |\nafter"));
    place(2, 3);
    md::TableInfo info = m_view->tableAtCaret();
    QVERIFY(info.found && !info.html);
    QVERIFY(md::tableOpAvailable(info, md::TableOp::ToHtml));
    QVERIFY(!md::tableOpAvailable(info, md::TableOp::ToPipe));
    QVERIFY(m_view->editTable(md::TableOp::ToHtml));
    const QString html = text();
    QVERIFY2(html.startsWith(QLatin1String("<table>\n")), qPrintable(html));
    QVERIFY2(html.contains(QLatin1String("<strong>b</strong>")), qPrintable(html));
    QVERIFY2(html.contains(QLatin1String("align=\"center\"")), qPrintable(html));
    QVERIFY2(html.endsWith(QLatin1String("</table>\n\nafter")), qPrintable(html));
    // And back.
    place(1, 3);
    info = m_view->tableAtCaret();
    QVERIFY(info.found && info.html);
    QVERIFY(md::tableOpAvailable(info, md::TableOp::ToPipe));
    QVERIFY(m_view->editTable(md::TableOp::ToPipe));
    QVERIFY2(text().startsWith(QLatin1String("| a ")), qPrintable(text()));
    QVERIFY2(text().contains(QLatin1String("<strong>b</strong>")), qPrintable(text()));
    QVERIFY2(text().endsWith(QLatin1String("|\n\nafter")), qPrintable(text()));
}

void TestTextView::tableSizePickerPicksASize()
{
    QMenu menu;
    QList<QPair<int, int>> inserted;
    QMenu *sub = vn::addInsertTableMenu(&menu, [&inserted](int rows, int columns) { inserted.append({rows, columns}); });
    QCOMPARE(sub->actions().size(), 3); // The grid, a separator, Custom Size...
    QVERIFY(sub->actions().last()->text().contains(QLatin1String("Custom")));
    auto *picker = sub->findChild<vn::TableSizePicker *>();
    QVERIFY(picker);
    picker->resize(picker->sizeHint());
    // Pointing at the third square of the fourth row: 4 rows (with the
    // header) by 3 columns. The grid never offers fewer than 2 rows.
    const QPoint square(6 + 2 * 16 + 7, picker->sizeHint().height() - 6 - (vn::TableSizePicker::gridRows - 4) * 16 - 9);
    // (The menu is not shown: the move is sent to the picker directly.)
    QMouseEvent move(QEvent::MouseMove, square, picker->mapToGlobal(square), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(picker, &move);
    QCOMPARE(picker->rows(), 4);
    QCOMPARE(picker->columns(), 3);
    if (const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR"); !dir.isEmpty())
        QVERIFY(picker->grab().save(dir + QStringLiteral("/table-size-picker.png")));
    QTest::mouseClick(picker, Qt::LeftButton, {}, square);
    QTRY_COMPARE(inserted.size(), 1); // Once the menu is closed.
    QCOMPARE(inserted.first(), qMakePair(4, 3));
    // The keys: from nothing, Right gives 2 x 1; Down and Right grow it.
    picker->setSelection(0, 0);
    QTest::keyClick(picker, Qt::Key_Right);
    QTest::keyClick(picker, Qt::Key_Right);
    QTest::keyClick(picker, Qt::Key_Down);
    QCOMPARE(picker->rows(), 3);
    QCOMPARE(picker->columns(), 2);
    QTest::keyClick(picker, Qt::Key_Return);
    QTRY_COMPARE(inserted.size(), 2);
    QCOMPARE(inserted.last(), qMakePair(3, 2));
}

// ---------------------------------------------------------------------------
// Columns

void TestTextView::showColumns(int lines)
{
    QStringList text;
    for (int i = 1; i <= lines; ++i)
        text << QStringLiteral("line %1").arg(i);
    setText(text.join(u'\n'));
    m_view->resize(1400, 500);
    m_view->setColumnChars(50);
    m_view->setColumnsShown(true);
    m_view->grab();
}

int TestTextView::lineAtPoint(const QPointF &point)
{
    auto *probe = static_cast<ProbeView *>(m_view);
    QTextCursor cursor(m_doc);
    cursor.setPosition(probe->positionAtPoint(point));
    return cursor.blockNumber();
}

int TestTextView::perColumn()
{
    return m_view->columnRect(0).height() / static_cast<ProbeView *>(m_view)->rowHeight();
}

QPointF TestTextView::columnPoint(int column, int row)
{
    auto *probe = static_cast<ProbeView *>(m_view);
    const QRect rect = m_view->columnRect(column);
    const int h = probe->rowHeight();
    if (row < 0)
        row = perColumn() - 1;
    return QPointF(rect.left() + probe->left() + 12, rect.top() + (row + 0.5) * h);
}

void TestTextView::repeatKey(Qt::Key key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, QString(), true);
    QApplication::sendEvent(m_view, &press);
}

bool TestTextView::inked(const QImage &image, const QRect &rect)
{
    const QRgb background = image.pixel(rect.topLeft());
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        for (int x = rect.left(); x <= rect.right(); ++x) {
            if (image.pixel(x, y) != background)
                return true;
        }
    }
    return false;
}

// The lines run down the first column and on at the top of the next; the
// caret moves, selects, copies and edits there; turned off, the first
// column's first line is at the top.
void TestTextView::columnsShowLinesInColumns()
{
    auto *probe = static_cast<ProbeView *>(m_view);
    QSignalSpy changed(m_view, &TextView::columnsChanged);
    showColumns(300);
    QVERIFY(m_view->columnsShown());
    QVERIFY(m_view->shownColumnCount() >= 2);
    QVERIFY(m_view->shownColumnCount() <= m_view->maxColumnCount());
    QVERIFY(!m_view->verticalScrollBar()->isVisible());
    QVERIFY(!m_view->horizontalScrollBar()->isVisible());
    const QRect first = m_view->columnRect(0);
    const QRect second = m_view->columnRect(1);
    QVERIFY(first.right() < second.left());
    QCOMPARE(first.size(), second.size());
    const int perColumn = first.height() / probe->rowHeight();
    const auto [top, bottom] = m_view->columnLineRange();
    QCOMPARE(top, 0);
    QCOMPARE(bottom, perColumn * m_view->shownColumnCount() - 1);
    QCOMPARE(m_view->topLine(), top);
    QCOMPARE(m_view->bottomLine(), bottom);
    QTRY_VERIFY(!changed.isEmpty());
    // Column 2 goes on from the line after column 1's last.
    const qreal lineHeight = probe->rowHeight();
    QCOMPARE(lineAtPoint(QPointF(first.right() - 4, first.top() + (perColumn - 0.5) * lineHeight)), perColumn - 1);
    QCOMPARE(lineAtPoint(QPointF(second.right() - 4, second.top() + 0.5 * lineHeight)), perColumn);

    // A click in column 2 puts the caret there; the keys move it.
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {},
                      QPoint(second.right() - 4, second.top() + int(3.5 * lineHeight)));
    QCOMPARE(m_view->textCursor().blockNumber(), perColumn + 3);
    QTest::keyClick(m_view, Qt::Key_Down);
    QCOMPARE(m_view->textCursor().blockNumber(), perColumn + 4);
    QCOMPARE(m_view->columnLineRange().first, 0); // (On screen: nothing moved.)
    // It edits: typing, deleting, pasting and undo.
    QVERIFY(!m_view->isReadOnly());
    const QString before = text();
    QTest::keyClick(m_view, Qt::Key_End);
    type(QStringLiteral("xy"));
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QCOMPARE(m_doc->findBlockByNumber(perColumn + 4).text(), QStringLiteral("line %1x").arg(perColumn + 5));
    QApplication::clipboard()->setText(QStringLiteral("pasted"));
    m_view->paste();
    QCOMPARE(m_doc->findBlockByNumber(perColumn + 4).text(), QStringLiteral("line %1xpasted").arg(perColumn + 5));
    while (m_doc->isUndoAvailable())
        m_view->undo();
    QCOMPARE(text(), before);
    // A selection copies.
    place(perColumn + 4, 0);
    QTest::keyClick(m_view, Qt::Key_End, Qt::ShiftModifier);
    m_view->copy();
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("line %1").arg(perColumn + 5));

    // Off: the first column's first line stays at the top, and it edits again.
    m_view->columnsNudge(5);
    QCOMPARE(m_view->columnLineRange().first, 5);
    m_view->setColumnsShown(false);
    QVERIFY(!m_view->columnsShown());
    QCOMPARE(m_view->columnLineRange(), std::make_pair(-1, -1));
    QCOMPARE(m_view->topLine(), 5);
    QVERIFY(!m_view->isReadOnly());
    // On again from a line asked for.
    m_view->setColumnsShown(true, 40);
    m_view->grab();
    QCOMPARE(m_view->columnLineRange().first, 40);
}

// Each column has its own gutter; a wrapped row has no number, nor has a
// line going on from the column before.
void TestTextView::columnsNumberEachColumnsLines()
{
    auto *probe = static_cast<ProbeView *>(m_view);
    showColumns(300);
    const QRect first = m_view->columnRect(0);
    const int perColumn = first.height() / probe->rowHeight();
    // Line 2 wraps over three rows (50 characters a row).
    QTextCursor edit(m_doc->findBlockByNumber(1));
    edit.movePosition(QTextCursor::EndOfBlock);
    m_view->setColumnsShown(false);
    edit.insertText(QStringLiteral(" abcd").repeated(24)); // Three rows of 50 characters.
    m_view->setColumnsShown(true, 0);
    const QImage image = m_view->viewport()->grab().toImage();
    const int h = probe->rowHeight();
    const int gutter = qRound(probe->left()) - 4; // The gutter, without the text's margin.
    QVERIFY(gutter > 10);
    const auto gutterRow = [&](int column, int row) {
        const QRect c = m_view->columnRect(column);
        return QRect(c.left(), c.top() + row * h + 2, gutter - 2, h - 4);
    };
    QVERIFY(inked(image, gutterRow(0, 0)));  // 1
    QVERIFY(inked(image, gutterRow(0, 1)));  // 2...
    QVERIFY(!inked(image, gutterRow(0, 2))); // ...wrapped,
    QVERIFY(!inked(image, gutterRow(0, 3))); // and again.
    QVERIFY(inked(image, gutterRow(0, 4)));  // 3
    QVERIFY(inked(image, gutterRow(1, 0)));  // The second column's first line.
    QCOMPARE(lineAtPoint(QPointF(m_view->columnRect(1).right() - 4, m_view->columnRect(1).top() + h / 2.0)),
             perColumn - 2); // (Two rows less: line 2 took three.)
    // Without line numbers there is no gutter: the text starts at the column's edge.
    m_view->setLineNumbersVisible(false);
    const QImage bare = m_view->viewport()->grab().toImage();
    QVERIFY(!inked(bare, QRect(m_view->columnRect(1).left(), m_view->columnRect(1).top(), 3,
                               m_view->columnRect(1).height())));
    QVERIFY(inked(bare, QRect(m_view->columnRect(1).left(), m_view->columnRect(1).top(), 20, h)));
    QVERIFY(m_view->columnRect(0).width() < first.width());
}

// Code has no "two lines each side" rule: a column is full, and a wrapped
// line can be cut after any of its rows, going on at the top of the next.
void TestTextView::columnsCutWrappedLinesBetweenRows()
{
    auto *probe = static_cast<ProbeView *>(m_view);
    showColumns(300);
    const int h = probe->rowHeight();
    const int perColumn = m_view->columnRect(0).height() / h;
    // The column's last line wraps over three rows: one fits, two go on.
    m_view->setColumnsShown(false);
    QTextCursor edit(m_doc->findBlockByNumber(perColumn - 1));
    edit.movePosition(QTextCursor::EndOfBlock);
    edit.insertText(QStringLiteral(" abcd").repeated(24)); // Three rows of 50 characters.
    m_view->setColumnsShown(true, 0);
    m_view->grab();
    const QRect first = m_view->columnRect(0);
    const QRect second = m_view->columnRect(1);
    QCOMPARE(lineAtPoint(QPointF(first.left() + first.width() / 2.0, first.top() + (perColumn - 0.5) * h)),
             perColumn - 1);
    const QPointF topOfSecond(second.left() + probe->left() + 4, second.top() + h / 2.0);
    QCOMPARE(lineAtPoint(topOfSecond), perColumn - 1);
    QCOMPARE(lineAtPoint(QPointF(topOfSecond.x(), topOfSecond.y() + 2 * h)), perColumn);
    // (Where it goes on: past the first row's 50 characters.)
    const QTextBlock block = m_doc->findBlockByNumber(perColumn - 1);
    QVERIFY(probe->positionAtPoint(topOfSecond) - block.position() >= 40);
}

// Word Wrap off: each column scrolls sideways on its own, with a scroll bar
// only under a column that has a line too long for it; a column keeps its
// place when the caret leaves it, and through a nudge of a line.
void TestTextView::columnsScrollSidewaysOneByOne()
{
    auto *probe = static_cast<ProbeView *>(m_view);
    m_view->setWordWrap(false);
    showColumns(300);
    const int h = probe->rowHeight();
    const int perColumn = m_view->columnRect(0).height() / h;
    QVERIFY(m_view->columnBarRect(0).isEmpty());
    QVERIFY(m_view->columnBarRect(1).isEmpty());
    // A long line in column 2.
    const int longLine = perColumn + 3;
    m_view->setColumnsShown(false);
    QTextCursor edit(m_doc->findBlockByNumber(longLine));
    edit.movePosition(QTextCursor::EndOfBlock);
    edit.insertText(QStringLiteral(" ") + QString(200, u'w') + QStringLiteral(" end"));
    m_view->setColumnsShown(true, 0);
    m_view->grab();
    QVERIFY(m_view->columnBarRect(0).isEmpty());
    const QRect bar = m_view->columnBarRect(1);
    QVERIFY(!bar.isEmpty());
    QVERIFY(bar.top() >= m_view->columnRect(1).bottom()); // Under the column's lines.
    // End shows the end of the line: column 2 scrolls, column 1 does not.
    place(longLine, 0);
    QTest::keyClick(m_view, Qt::Key_End);
    QVERIFY(m_view->columnOffset(1) > 0);
    QCOMPARE(m_view->columnOffset(0), 0.0);
    const qreal offset = m_view->columnOffset(1);
    // The caret leaves for column 1: column 2 keeps its place.
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {},
                      QPoint(m_view->columnRect(0).center().x(), m_view->columnRect(0).top() + h / 2));
    QCOMPARE(m_view->textCursor().blockNumber(), 0);
    QCOMPARE(m_view->columnOffset(1), offset);
    // A line down: the long line is still in column 2, still scrolled.
    m_view->columnsNudge(1);
    QCOMPARE(m_view->columnOffset(1), offset);
    // The line numbers stay: a click in the gutter is at a line's start.
    const QRect second = m_view->columnRect(1);
    QTextCursor at(m_doc);
    at.setPosition(probe->positionAtPoint(QPointF(second.left() + probe->left() + 1, second.top() + h / 2.0)));
    QVERIFY(at.positionInBlock() > 0); // (Scrolled: the text's start is out of sight.)
    // The wheel sideways over column 1 does nothing (nothing is too long there);
    // the bar drags column 2 back to its start.
    QWheelEvent sideways(QPointF(m_view->columnRect(0).center()), m_view->mapToGlobal(m_view->columnRect(0).center()),
                         QPoint(), QPoint(-240, 0), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(m_view->viewport(), &sideways);
    QCOMPARE(m_view->columnOffset(0), 0.0);
    QTest::mousePress(m_view->viewport(), Qt::LeftButton, {}, QPoint(bar.left() + 1, bar.center().y()));
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, QPoint(bar.left() + 1, bar.center().y()));
    QCOMPARE(m_view->columnOffset(1), 0.0);
    // A page turn starts every column at its lines' start.
    m_view->setColumnOffset(1, 50);
    QVERIFY(m_view->columnOffset(1) > 0);
    m_view->columnsPageForward();
    QCOMPARE(m_view->columnOffset(1), 0.0);
}

// Page Down and Page Up turn a full page (keeping the lines set in
// Preferences) and the caret goes along; Ctrl+Down and the small buttons
// move a line, without the caret; the arrows at the edges turn pages.
void TestTextView::columnsTurnPagesAndNudge()
{
    vn::Settings::instance().reset(vn::key::readKeptLines);
    showColumns(300);
    const auto [top, bottom] = m_view->columnLineRange();
    place(3, 2);
    QTest::keyClick(m_view, Qt::Key_PageDown);
    const int next = bottom + 1 - 2; // Two lines kept.
    QCOMPARE(m_view->columnLineRange().first, next);
    QCOMPARE(m_view->textCursor().blockNumber(), 3 + next);
    QCOMPARE(m_view->textCursor().positionInBlock(), 2);
    QTest::keyClick(m_view, Qt::Key_PageUp);
    QCOMPARE(m_view->columnLineRange().first, top);
    QCOMPARE(m_view->textCursor().blockNumber(), 3);
    // Shift+Page Down selects along.
    QTest::keyClick(m_view, Qt::Key_PageDown, Qt::ShiftModifier);
    QVERIFY(m_view->textCursor().hasSelection());
    QTest::keyClick(m_view, Qt::Key_PageUp);
    QCOMPARE(m_view->columnLineRange().first, top);
    // A line at a time, the caret staying.
    const int caret = m_view->textCursor().position();
    QTest::keyClick(m_view, Qt::Key_Down, Qt::ControlModifier);
    QCOMPARE(m_view->columnLineRange().first, 1);
    QTest::keyClick(m_view, Qt::Key_Up, Qt::ControlModifier);
    QCOMPARE(m_view->columnLineRange().first, 0);
    QCOMPARE(m_view->textCursor().position(), caret);
    auto *down = m_view->findChild<QAbstractButton *>(QStringLiteral("ColumnsLineDown"));
    auto *up = m_view->findChild<QAbstractButton *>(QStringLiteral("ColumnsLineUp"));
    auto *forward = m_view->findChild<QAbstractButton *>(QStringLiteral("ColumnsPageForward"));
    auto *back = m_view->findChild<QAbstractButton *>(QStringLiteral("ColumnsPageBack"));
    QVERIFY(down && up && forward && back);
    QVERIFY(down->isVisible() && forward->isVisible());
    QVERIFY(!up->isEnabled() && !back->isEnabled());
    down->click();
    QCOMPARE(m_view->columnLineRange().first, 1);
    QVERIFY(up->isEnabled());
    up->click();
    forward->click();
    QCOMPARE(m_view->columnLineRange().first, next);
    back->click();
    QCOMPARE(m_view->columnLineRange().first, 0);
    QCOMPARE(m_view->textCursor().position(), caret);
    // The wheel: a line a notch.
    QWheelEvent notch(QPointF(m_view->columnRect(0).center()), m_view->mapToGlobal(m_view->columnRect(0).center()),
                      QPoint(), QPoint(0, -240), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(m_view->viewport(), &notch);
    QCOMPARE(m_view->columnLineRange().first, 2);
    // To the end: the last screen is full, and there is no page after it.
    QTest::keyClick(m_view, Qt::Key_End, Qt::ControlModifier);
    QCOMPARE(m_view->columnLineRange().second, 299);
    QCOMPARE(m_view->columnLineRange().first, 300 - (bottom + 1));
    QVERIFY(!forward->isEnabled() && !down->isEnabled());
    QTest::keyClick(m_view, Qt::Key_PageDown); // Nowhere to turn to.
    QCOMPARE(m_view->columnLineRange().second, 299);
    // Scrolling (Alt+Page Up, the command) turns without the caret.
    const int end = m_view->textCursor().position();
    m_view->scrollPage(false);
    QVERIFY(m_view->columnLineRange().second < 299);
    QCOMPARE(m_view->textCursor().position(), end);
    // Off: the controls go.
    m_view->setColumnsShown(false);
    QVERIFY(!down->isVisible() && !forward->isVisible());
}

// The caret's line is kept on screen: up from the first line the view goes
// back a column, down from the last line on a column; a caret far away (go
// to line, find) brings the screen that starts with its line.
void TestTextView::columnsCaretBringsItsLine()
{
    showColumns(300);
    const int per = perColumn();
    m_view->columnsNudge(per + 10);
    const auto [top, bottom] = m_view->columnLineRange();
    QCOMPARE(top, per + 10);
    place(top, 0);
    QTest::keyClick(m_view, Qt::Key_Up);
    QCOMPARE(m_view->columnLineRange().first, 10);
    QCOMPARE(lineAtPoint(columnPoint(0, -1)), top - 1); // The caret's line, at the first column's bottom.
    place(bottom - per, 0);
    QTest::keyClick(m_view, Qt::Key_Down); // To the line after the screen.
    QCOMPARE(m_view->columnLineRange().first, top);
    QTest::keyClick(m_view, Qt::Key_Down);
    QCOMPARE(m_view->columnLineRange().first, top);
    QCOMPARE(caretLine(), bottom - per + 2);
    place(200, 0);
    m_view->centerCursor();
    QCOMPARE(m_view->columnLineRange().first, 200);
    // Find shows its results in the columns as anywhere.
    m_view->setSearchHighlights({{m_doc->findBlockByNumber(201).position(), m_doc->findBlockByNumber(201).position() + 4}},
                                0);
    m_view->grab();
    // A line asked for by the session or the outline.
    m_view->scrollToLine(150);
    m_view->grab();
    QCOMPARE(m_view->columnLineRange().first, 150);
}

// Down at the bottom of a column: the caret goes to the top of the next and
// nothing moves; a note under the column it left says so, once while the key
// is held down, never after a click, and goes by itself.
void TestTextView::columnsDownGoesToTheNextColumnWithANote()
{
    showColumns(300);
    m_view->setColumnChars(30);
    m_view->grab();
    QVERIFY(m_view->shownColumnCount() >= 3);
    const int per = perColumn();
    const QString next = QStringLiteral("Caret moved to the next column →");
    const auto noteIn = [this](QLabel *note, int column) {
        const QRect rect = m_view->columnRect(column);
        return note->isVisible() && note->x() >= rect.left() && note->x() < rect.right()
               && note->y() >= rect.bottom();
    };
    place(per - 2, 0);
    // Held down: the first press stays in column 1, a repeat crosses over.
    QTest::keyPress(m_view, Qt::Key_Down);
    QCOMPARE(caretLine(), per - 1);
    auto *note = m_view->findChild<QLabel *>(QStringLiteral("ColumnsCaretNote"));
    QVERIFY(!note || !note->isVisible());
    repeatKey(Qt::Key_Down);
    QCOMPARE(caretLine(), per);
    QCOMPARE(lineAtPoint(columnPoint(1, 0)), per); // At the top of column 2...
    QCOMPARE(m_view->columnLineRange().first, 0);  // ...and nothing moved.
    note = m_view->findChild<QLabel *>(QStringLiteral("ColumnsCaretNote"));
    QVERIFY(note);
    QVERIFY(noteIn(note, 0));
    QCOMPARE(note->text(), next);
    // Still held: on into column 3, and no new note (it stays under column 1).
    for (int i = 0; i < per; ++i)
        repeatKey(Qt::Key_Down);
    QTest::keyRelease(m_view, Qt::Key_Down);
    QCOMPARE(caretLine(), 2 * per);
    QVERIFY(noteIn(note, 0));
    // A new press back up to column 2: a note under column 3, pointing back.
    QTest::keyClick(m_view, Qt::Key_Up);
    QCOMPARE(caretLine(), 2 * per - 1);
    QCOMPARE(m_view->columnLineRange().first, 0);
    QVERIFY(noteIn(note, 2));
    QCOMPARE(note->text(), QStringLiteral("← Caret moved to the previous column"));
    // It goes by itself.
    QTRY_VERIFY_WITH_TIMEOUT(!note->isVisible(), 5000);
    // A click into another column shows none.
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, columnPoint(0, 3).toPoint());
    QCOMPARE(caretLine(), 3);
    QVERIFY(!note->isVisible());
    // Nor a key that moves within a column.
    QTest::keyClick(m_view, Qt::Key_Down);
    QVERIFY(!note->isVisible());
}

// Down or Enter at the bottom of the last column moves the view on by one
// column (column 2's lines into column 1), the caret at the top of the last;
// Up at the top of the first column (or Backspace there) goes back by one;
// Up at the top of another column goes to the bottom of the one before.
void TestTextView::columnsMoveOnOneColumnAtTheLastColumn()
{
    showColumns(300);
    const int per = perColumn();
    const int count = m_view->shownColumnCount();
    const int bottom = m_view->columnLineRange().second;
    QCOMPARE(bottom, per * count - 1);
    place(bottom, 2);
    QTest::keyClick(m_view, Qt::Key_Down);
    QCOMPARE(m_view->columnLineRange().first, per);
    QCOMPARE(m_view->columnLineRange().second, bottom + per);
    QCOMPARE(caretLine(), bottom + 1);
    QCOMPARE(lineAtPoint(columnPoint(count - 1, 0)), bottom + 1);
    QCOMPARE(lineAtPoint(columnPoint(0, 0)), per); // What column 2 showed.
    // Enter at the end of the last line on screen: the same, the caret on the
    // new line at the top of the last column.
    const int last = m_view->columnLineRange().second;
    place(last, 0);
    QTest::keyClick(m_view, Qt::Key_End);
    QTest::keyClick(m_view, Qt::Key_Return);
    QCOMPARE(m_doc->blockCount(), 301);
    QCOMPARE(m_view->columnLineRange().first, 2 * per);
    QCOMPARE(caretLine(), last + 1);
    QVERIFY(m_doc->findBlockByNumber(last + 1).text().isEmpty());
    QCOMPARE(lineAtPoint(columnPoint(count - 1, 0)), last + 1);
    // Undo: the line goes, the caret is back at the end of the line before.
    m_view->undo();
    QCOMPARE(m_doc->blockCount(), 300);
    QCOMPARE(caretLine(), last);
    QVERIFY(m_view->isLineShown(last));
    // Up at the top of column 2: the bottom of column 1, nothing moves.
    const int first = m_view->columnLineRange().first;
    place(first + per, 0);
    QTest::keyClick(m_view, Qt::Key_Up);
    QCOMPARE(caretLine(), first + per - 1);
    QCOMPARE(m_view->columnLineRange().first, first);
    // Up at the top of column 1: back by one column, the caret at its bottom.
    place(first, 0);
    QTest::keyClick(m_view, Qt::Key_Up);
    QCOMPARE(m_view->columnLineRange().first, first - per);
    QCOMPARE(caretLine(), first - 1);
    QCOMPARE(lineAtPoint(columnPoint(0, -1)), first - 1);
    // Backspace at the start of the first line on screen joins it to the line
    // before: back by one column too.
    const int start = m_view->columnLineRange().first;
    QCOMPARE(start, per);
    place(start, 0);
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QCOMPARE(m_doc->findBlockByNumber(start - 1).text(), QStringLiteral("line %1line %2").arg(start).arg(start + 1));
    QCOMPARE(m_view->columnLineRange().first, 0);
    QCOMPARE(lineAtPoint(columnPoint(0, -1)), start - 1);
    // Page Down still turns a full page.
    m_view->undo();
    vn::Settings::instance().reset(vn::key::readKeptLines);
    const int to = m_view->columnLineRange().second;
    QTest::keyClick(m_view, Qt::Key_PageDown);
    QCOMPARE(m_view->columnLineRange().first, to + 1 - 2);
}

// A new or deleted line moves text over the column breaks: the last line of
// column 1 to the top of column 2 and back, the last on screen off it.
void TestTextView::columnsTypingReflowsOverTheBreaks()
{
    showColumns(300);
    const int per = perColumn();
    const auto [top, bottom] = m_view->columnLineRange();
    place(3, 6); // The end of "line 4".
    QTest::keyClick(m_view, Qt::Key_Return);
    type(QStringLiteral("new"));
    QCOMPARE(m_view->columnLineRange(), std::make_pair(top, bottom));
    QCOMPARE(lineAtPoint(columnPoint(1, 0)), per);
    QCOMPARE(m_doc->findBlockByNumber(per).text(), QStringLiteral("line %1").arg(per)); // Was column 1's last.
    QCOMPARE(m_doc->findBlockByNumber(bottom).text(), QStringLiteral("line %1").arg(bottom)); // Was one up.
    QCOMPARE(caretLine(), 4);
    // Deleted again: back over the break.
    for (int i = 0; i < 4; ++i)
        QTest::keyClick(m_view, Qt::Key_Backspace);
    QCOMPARE(m_doc->findBlockByNumber(per).text(), QStringLiteral("line %1").arg(per + 1));
    QCOMPARE(lineAtPoint(columnPoint(1, 0)), per);
    QCOMPARE(m_view->columnLineRange(), std::make_pair(top, bottom));

    // Column 2 starts part-way into a wrapped line, and the view moves on to
    // it: typing keeps the first column starting there.
    auto *probe = static_cast<ProbeView *>(m_view);
    m_view->setColumnsShown(false);
    QTextCursor edit(m_doc->findBlockByNumber(per - 1));
    edit.movePosition(QTextCursor::EndOfBlock);
    edit.insertText(QStringLiteral(" abcd").repeated(24)); // Three rows of 50 characters.
    m_view->setColumnsShown(true, 0);
    m_view->grab();
    place(m_view->columnLineRange().second, 0);
    QTest::keyClick(m_view, Qt::Key_Down);
    const QTextBlock wrapped = m_doc->findBlockByNumber(per - 1);
    QCOMPARE(lineAtPoint(columnPoint(0, 0)), per - 1);
    const int into = probe->positionAtPoint(columnPoint(0, 0)) - wrapped.position();
    QVERIFY(into >= 40);
    const int caret = caretLine();
    type(QStringLiteral("typed"));
    QCOMPARE(probe->positionAtPoint(columnPoint(0, 0)) - wrapped.position(), into);
    QCOMPARE(lineAtPoint(columnPoint(m_view->shownColumnCount() - 1, 0)), caret);
}

// Word Wrap off: a column's sideways offset goes with its lines when the view
// moves on (or back) by a column; the column that comes in starts at the
// lines' start.
void TestTextView::columnsOffsetsTravelWithTheText()
{
    m_view->setWordWrap(false);
    showColumns(300);
    m_view->setColumnChars(30);
    m_view->grab();
    const int count = m_view->shownColumnCount();
    QVERIFY(count >= 3);
    const int per = perColumn();
    // A long line at the top of column 2.
    m_view->setColumnsShown(false);
    QTextCursor edit(m_doc->findBlockByNumber(per));
    edit.movePosition(QTextCursor::EndOfBlock);
    edit.insertText(QStringLiteral(" ") + QString(200, u'w') + QStringLiteral(" end"));
    m_view->setColumnsShown(true, 0);
    m_view->grab();
    place(per, 0);
    QTest::keyClick(m_view, Qt::Key_End);
    const qreal offset = m_view->columnOffset(1);
    QVERIFY(offset > 0);
    // On by a column from the last line on screen: column 2 is column 1 now.
    place(m_view->columnLineRange().second, 0);
    QCOMPARE(m_view->columnOffset(1), offset); // (The caret left it.)
    QTest::keyClick(m_view, Qt::Key_Down);
    QCOMPARE(m_view->columnLineRange().first, per);
    QCOMPARE(m_view->columnOffset(0), offset);
    QCOMPARE(m_view->columnOffset(count - 1), 0.0);
    // Back by a column from the long line's end (now at the top): it goes back
    // to column 2, column 1 starts at the lines' start.
    place(per, m_doc->findBlockByNumber(per).length() - 1);
    QCOMPARE(m_view->columnOffset(0), offset);
    QTest::keyClick(m_view, Qt::Key_Up);
    QCOMPARE(m_view->columnLineRange().first, 0);
    QCOMPARE(caretLine(), per - 1);
    QCOMPARE(m_view->columnOffset(1), offset);
    QCOMPARE(m_view->columnOffset(0), 0.0);
}

// Several carets in different columns type, take Enter and undo together.
void TestTextView::columnsEditAtSeveralCarets()
{
    showColumns(300);
    const int per = perColumn();
    const QString before = text();
    QTextCursor a(m_doc->findBlockByNumber(2));
    a.movePosition(QTextCursor::EndOfBlock);
    QTextCursor b(m_doc->findBlockByNumber(per + 2));
    b.movePosition(QTextCursor::EndOfBlock);
    m_view->setCursors({a, b}, 1);
    type(QStringLiteral("!"));
    QCOMPARE(m_doc->findBlockByNumber(2).text(), QStringLiteral("line 3!"));
    QCOMPARE(m_doc->findBlockByNumber(per + 2).text(), QStringLiteral("line %1!").arg(per + 3));
    QTest::keyClick(m_view, Qt::Key_Return);
    QCOMPARE(m_doc->blockCount(), 302);
    QCOMPARE(m_view->cursorCount(), 2);
    QCOMPARE(m_view->columnLineRange().first, 0);
    // (The second caret's line moved down by the first's new line.)
    QCOMPARE(m_doc->findBlockByNumber(per + 3).text(), QStringLiteral("line %1!").arg(per + 3));
    m_view->undo();
    m_view->undo();
    QCOMPARE(text(), before);
    QCOMPARE(m_view->cursorCount(), 2);
}

// Columns read about a screen of lines however long the file: turning them
// on, a page turn, a nudge and the jump to the end take about as long at
// line 100 000 as at the start.
void TestTextView::columnsStayFastOnABigFile()
{
    QStringList lines;
    lines.reserve(200000);
    for (int i = 0; i < 200000; ++i)
        lines << QStringLiteral("    value_%1 = compute(value_%2, %3)  # a line of code").arg(i).arg(i - 1).arg(i % 97);
    setText(lines.join(u'\n'));
    m_view->resize(1400, 500);
    m_view->setColumnChars(50);
    QElapsedTimer timer;
    timer.start();
    m_view->setColumnsShown(true, 100000);
    m_view->grab();
    for (int i = 0; i < 20; ++i) {
        m_view->columnsPageForward();
        m_view->grab();
    }
    // Typing and Enter at the bottom of the last column (a column on each).
    place(m_view->columnLineRange().second, 0);
    const int first = m_view->columnLineRange().first;
    for (int i = 0; i < 20; ++i) {
        QTest::keyClick(m_view, Qt::Key_End);
        type(QStringLiteral("ab"));
        QTest::keyClick(m_view, Qt::Key_Return);
        m_view->grab();
    }
    QVERIFY(m_view->columnLineRange().first > first);
    QVERIFY(m_view->isLineShown(m_view->textCursor().blockNumber()));
    m_view->columnsNudge(1);
    m_view->columnsToEnd();
    m_view->grab();
    QCOMPARE(m_view->columnLineRange().second, 200019);
    m_view->columnsToStart();
    m_view->grab();
    QCOMPARE(m_view->columnLineRange().first, 0);
    // (A walk over every line would take seconds here; this is about 25 screens.)
    QVERIFY2(timer.elapsed() < 5000, qPrintable(QString::number(timer.elapsed())));
}

QTEST_MAIN(TestTextView)
#include "test_textview.moc"
