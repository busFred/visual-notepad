// Editing behaviour of the multi-cursor TextView, run offscreen.
#include "Theme.h"
#include "Settings.h"
#include "editor/DocumentLayout.h"
#include "editor/TextView.h"

#include <QApplication>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QClipboard>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>

using vn::TextView;

// Exposes the geometry the mouse tests need.
class ProbeView : public TextView
{
public:
    using TextView::TextView;
    qreal left() const { return textLeft(); }
    int rowHeight() const { return lineHeight(); }
};

class TestTextView : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

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
    void tabStaysInEditorBesideOtherWidgets();
    void addCursorsToLineEnds();
    void columnSelectWithMouse();
    void columnSelectUsesVisualColumns();
    void columnSelectWithCtrlModifier();
    void middleClickPastesPrimarySelection();
    void pasteTextAtPlacesCaretAfter();
    void expandLineSelection();
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
    void boxSelect(const QPoint &from, const QPoint &to, Qt::KeyboardModifiers mods);

    void selectRange(int from, int to);

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
    QCOMPARE(m_view->cursorCount(), 4);
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

QTEST_MAIN(TestTextView)
#include "test_textview.moc"
