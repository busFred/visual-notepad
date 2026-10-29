// Editing behaviour of the multi-cursor TextView, run offscreen.
#include "Theme.h"
#include "Settings.h"
#include "editor/DocumentLayout.h"
#include "editor/TextView.h"

#include <QApplication>
#include <QClipboard>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>

using vn::TextView;

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
    void toggleLineComment();
    void backspaceInIndentGoesToTabStop();
    void enterKeepsIndent();
    void smartHome();
    void escapeCollapsesCarets();
    void lineCacheFollowsEdits();

private:
    void setText(const QString &text);
    QString text() const;
    void type(const QString &text);
    void place(int line, int column);

    QTextDocument *m_doc = nullptr;
    TextView *m_view = nullptr;
};

void TestTextView::init()
{
    m_doc = new QTextDocument;
    m_doc->setDocumentLayout(new vn::DocumentLayout(m_doc));
    m_view = new TextView(m_doc);
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

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestTextView)
#include "test_textview.moc"
