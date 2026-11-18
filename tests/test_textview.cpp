// Editing behaviour of the multi-cursor TextView, run offscreen.
#include "Theme.h"
#include "Settings.h"
#include "ThreadCpuTime.h"
#include "editor/DocumentLayout.h"
#include "editor/LineLayout.h"
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
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QRandomGenerator>

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <memory>
#include <tuple>

using vn::TextView;

// Exposes the geometry the mouse tests need.
class ProbeView : public TextView
{
public:
    using TextView::TextView;
    qreal left() const { return textLeft(); }
    qreal widthForText() const { return textWidth(); }
    int rowHeight() const { return lineHeight(); }
    int positionAtPoint(const QPointF &point) { return positionAt(point); }
    bool bounds(int line, qreal *top, qreal *bottom) { return lineBounds(line, top, bottom); }
    // The caret's rectangle in the viewport, as input methods get it.
    QRect caretRectangle() const
    {
        return inputMethodQuery(Qt::ImCursorRectangle).toRect().translated(-viewport()->pos());
    }
};

// Lets the fence test ask a view whether a line is inside fenced code.
class FenceProbe
{
public:
    static bool inFence(TextView *view, int line) { return view->inFencedCode(line); }
};

// Counts what the view's viewport is asked to paint.
class PaintSpy : public QObject
{
public:
    QList<QRect> rects;

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Paint)
            rects.append(static_cast<QPaintEvent *>(event)->rect());
        return false;
    }
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
    void columnsMouseSelectsAcrossColumns();
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
    void manyCaretsEditAsLiveCursorsDo();
    void moreManyCaretEditsAsLiveCursorsDo();
    void autoClosedPairsAtManyCarets();
    void otherViewsCaretsMoveAsLiveCursors();
    void manyCaretsUndoAndRedoInOneStep();
    void undoRestoresCaretAtEdit();
    void toggleLineComment();
    void backspaceInIndentGoesToTabStop();
    void enterKeepsIndent();
    void smartHome();
    void escapeCollapsesCarets();
    void lineCacheFollowsEdits();
    void caretBlinkRepaintsOnlyTheCaret();
    void longLinePaintsTheSameInStrips();
    void fenceStateAfterEdits();
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

    void longLineJoinsAreSafe_data();
    void longLineJoinsAreSafe();
    void longLineHitTestingMatchesTheWholeLine_data();
    void longLineHitTestingMatchesTheWholeLine();
    void longLineCaretMovesAsQtDoes();
    void longLineEditsLookAsLaidOutAfresh();
    void longLineWrapsInPieces();
    void longLineSelectionAndMatchesSpanAJoin();
    void longLineComposes();
    void emptyInputMethodEventKeepsTheScroll();
    void longLineInColumns();
    void longLineTypingIsFast();
    void longLineResetupKeepsPiecesAndSettlesExact();
    void longLineRebuildKeepsEstimates();
    void longLineEstimatesShowExactScreen();
    void longLineWheelUpIntoEstimatesKeepsScreen();
    void longLineCaretMovesAfterResize();
    void longLineFindAfterZoom();
    void longLineEditsAfterResize();
    void longLineWrapOffMeasuresOnlyShown();
    void longLineResizeIsFast();
    void longLineRowsPastEstimateAfterResetup();
    void longLineBoxSelectAfterResize();
    void longLineHiddenZoomThenEditKeepsTop();

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
    void dragTo(const QPointF &point);

    // The view's long first line laid out whole by Qt, as the view lays
    // out a short one (Word Wrap off).
    std::unique_ptr<QTextLayout> wholeLine() const;
    // Where the pieces of a block start (they are cut from the text alone).
    QList<int> joinsOf(const QTextBlock &block) const;
    // The view against a new one on the same text, carets and scroll,
    // neither focused: the same pixels.
    void compareWithFreshView(int line);
    void compareWithSettledLongLine();
    // The view's long first line laid out exactly, every piece measured.
    std::unique_ptr<vn::LineLayout> exactFirstLine(ProbeView &view);

    QTextDocument *m_doc = nullptr;
    TextView *m_view = nullptr;
};

namespace {

// A line long enough to be laid out in pieces (vn::LineLayout).
QString longLine(const QString &unit, int length = 40000)
{
    QString line;
    while (line.size() < length)
        line += unit;
    return line;
}

// Texts whose joins land in the awkward places: inside words, next to
// surrogate pairs and combining marks, around right-to-left runs, between
// tabs.
const QList<std::pair<const char *, QString>> &longLines()
{
    static const QList<std::pair<const char *, QString>> lines = {
        {"words", longLine(QStringLiteral("lorem ipsum dolor sit amet, consectetur "))},
        {"no spaces", longLine(QStringLiteral("abcdefghijklmnopqrstuvwxyz0123456789"))},
        {"json", longLine(QStringLiteral("{\"id\":12345,\"name\":\"value\",\"list\":[1,2,3]},"))},
        {"tabs", longLine(QStringLiteral("ab\tcde\t\tf ghi jk\t") + QString(3000, u'a') + QStringLiteral(" b\tc ")
                          + QString(1500, u'd') + QStringLiteral("x\t"))},
        {"right-to-left", longLine(QStringLiteral("abc \u05E9\u05DC\u05D5\u05DD \u05E2\u05D5\u05DC\u05DD def 123 ghi ")
                                   + QStringLiteral("start ") + longLine(QStringLiteral("\u05E9\u05DC\u05D5\u05DD "), 6000)
                                   + QStringLiteral("end 42 "))},
        {"combining", longLine(QStringLiteral("xe\u0301ye\u0301\u0302z"))},
        {"surrogates", longLine(QStringLiteral("ab\U0001F600c\U0001F44D\U0001F3FDd"))},
    };
    return lines;
}

QChar::Direction directionAt(const QString &text, int i)
{
    char32_t c = text.at(i).unicode();
    if (QChar::isLowSurrogate(c) && i > 0)
        c = QChar::surrogateToUcs4(text.at(i - 1), text.at(i));
    else if (QChar::isHighSurrogate(c) && i + 1 < text.size())
        c = QChar::surrogateToUcs4(text.at(i), text.at(i + 1));
    return QChar::direction(c);
}

bool rightToLeft(QChar::Direction d)
{
    return d == QChar::DirR || d == QChar::DirAL || d == QChar::DirAN;
}

} // namespace

std::unique_ptr<QTextLayout> TestTextView::wholeLine() const
{
    auto layout = std::make_unique<QTextLayout>(m_doc->firstBlock().text(), m_view->font());
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setTabStopDistance(QFontMetricsF(m_view->font()).horizontalAdvance(QLatin1Char(' ')) * 4);
    layout->setTextOption(option);
    layout->beginLayout();
    QTextLine line = layout->createLine();
    line.setLineWidth(1e7);
    layout->endLayout();
    return layout;
}

QList<int> TestTextView::joinsOf(const QTextBlock &block) const
{
    vn::LineLayout layout;
    vn::LineLayout::Setup setup;
    setup.font = m_view->font();
    setup.wrapWidth = 1e7;
    setup.lineHeight = 20;
    if (!layout.build(block, setup))
        return {};
    QList<int> joins = layout.pieceStarts();
    joins.removeFirst(); // (0)
    return joins;
}

void TestTextView::compareWithFreshView(int line)
{
    QTextDocument doc;
    doc.setDocumentLayout(new vn::DocumentLayout(&doc));
    doc.setPlainText(m_doc->toPlainText());
    ProbeView fresh(&doc);
    fresh.resize(m_view->size());
    fresh.setFont(m_view->font());
    fresh.setWordWrap(m_view->wordWrap());
    fresh.show();
    QVERIFY(QTest::qWaitForWindowExposed(&fresh));
    QList<QTextCursor> carets;
    for (const QTextCursor &c : m_view->cursors()) {
        QTextCursor copy(&doc);
        copy.setPosition(c.anchor());
        copy.setPosition(c.position(), QTextCursor::KeepAnchor);
        carets.append(copy);
    }
    fresh.setCursors(carets, 0);
    m_view->clearFocus();
    fresh.clearFocus();
    m_view->scrollToLine(line);
    fresh.scrollToLine(line);
    fresh.grab();
    fresh.horizontalScrollBar()->setValue(m_view->horizontalScrollBar()->value());
    QCOMPARE(fresh.horizontalScrollBar()->value(), m_view->horizontalScrollBar()->value());
    QCOMPARE(fresh.viewport()->grab().toImage(), m_view->viewport()->grab().toImage());
    m_view->setFocus();
}

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

// The text and carets a key must leave, found with a live QTextCursor for
// each caret (a second document, the way every edit once went).
static QString editedByLiveCursors(const QList<QTextCursor> &carets, const std::function<void(QTextCursor &)> &edit,
                                   QStringList *carets_)
{
    QTextDocument reference;
    reference.setPlainText(carets.first().document()->toPlainText());
    QList<QTextCursor> live;
    for (const QTextCursor &c : carets) {
        QTextCursor copy(&reference);
        copy.setPosition(c.anchor());
        copy.setPosition(c.position(), QTextCursor::KeepAnchor);
        live.append(copy);
    }
    for (QTextCursor &c : live)
        edit(c);
    // The view merges carets that come to touch.
    std::vector<std::pair<int, int>> ranges;
    for (const QTextCursor &c : std::as_const(live))
        ranges.emplace_back(c.selectionStart(), c.selectionEnd());
    std::sort(ranges.begin(), ranges.end());
    carets_->clear();
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (i > 0 && ranges[i] == ranges[i - 1])
            continue;
        carets_->append(QStringLiteral("%1-%2").arg(ranges[i].first).arg(ranges[i].second));
    }
    return reference.toPlainText();
}

void TestTextView::manyCaretsEditAsLiveCursorsDo()
{
    // Edits go through all the carets as numbers, not as live cursors that every
    // change would move (see CaretSpans): the same text and carets must come
    // out, also where carets touch or an edit reaches into the next.
    QRandomGenerator random(20260114);
    struct Key
    {
        Qt::Key key;
        Qt::KeyboardModifiers mods;
        QString text; // Typed (empty: the key's own).
        std::function<void(QTextCursor &)> edit;
    };
    const std::vector<Key> keys = {
        {Qt::Key_Z, Qt::NoModifier, QStringLiteral("z"), [](QTextCursor &c) { c.insertText(QStringLiteral("z")); }},
        {Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "), [](QTextCursor &c) { c.insertText(QStringLiteral(" ")); }},
        {Qt::Key_Backspace, Qt::NoModifier, {}, [](QTextCursor &c) {
             if (!c.hasSelection())
                 c.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
             c.removeSelectedText();
         }},
        {Qt::Key_Delete, Qt::NoModifier, {}, [](QTextCursor &c) {
             if (!c.hasSelection())
                 c.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
             c.removeSelectedText();
         }},
        {Qt::Key_Backspace, Qt::ControlModifier, {}, [](QTextCursor &c) {
             if (!c.hasSelection())
                 c.movePosition(QTextCursor::PreviousWord, QTextCursor::KeepAnchor);
             c.removeSelectedText();
         }},
        {Qt::Key_Delete, Qt::ControlModifier, {}, [](QTextCursor &c) {
             if (!c.hasSelection())
                 c.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor);
             c.removeSelectedText();
         }},
    };
    for (int round = 0; round < 60; ++round) {
        QStringList lines;
        for (int i = 0; i < 40; ++i) {
            QString line;
            for (int w = random.bounded(0, 7); w > 0; --w)
                line += QStringLiteral("word%1 ").arg(random.bounded(100));
            lines << line;
        }
        setText(lines.join(u'\n'));
        const int length = m_doc->characterCount() - 1;
        // Carets close together (words are 5 to 7 characters long), some
        // selecting, some touching.
        QList<QTextCursor> chosen;
        for (int pos = random.bounded(0, 6); pos < length;) {
            QTextCursor c(m_doc);
            const int width = random.bounded(0, 4) == 0 ? random.bounded(1, 12) : 0;
            c.setPosition(pos);
            c.setPosition(qMin(length, pos + width), QTextCursor::KeepAnchor);
            chosen.append(c);
            pos += width + random.bounded(0, 3) * random.bounded(1, 9);
        }
        if (chosen.isEmpty())
            continue;
        m_view->setCursors(chosen, chosen.size() / 2);
        const Key &key = keys[round % keys.size()];
        const QList<QTextCursor> before = m_view->cursors();
        QStringList expectedCarets;
        const QString expected = editedByLiveCursors(before, key.edit, &expectedCarets);
        if (key.text.isEmpty())
            QTest::keyClick(m_view, key.key, key.mods);
        else
            QTest::keyClicks(m_view, key.text);
        QStringList carets;
        for (const QTextCursor &c : m_view->cursors())
            carets << QStringLiteral("%1-%2").arg(c.selectionStart()).arg(c.selectionEnd());
        const QString what = QStringLiteral("round %1, key %2").arg(round).arg(round % keys.size());
        QVERIFY2(text() == expected, qPrintable(what + QStringLiteral(": text")));
        QVERIFY2(carets == expectedCarets, qPrintable(what + QStringLiteral(": carets %1 against %2")
                                                          .arg(carets.join(u' '), expectedCarets.join(u' '))));
    }
}

void TestTextView::moreManyCaretEditsAsLiveCursorsDo()
{
    // The other edits at every caret, against a model with a live cursor for
    // each caret, made as the view made them when its carets were cursors.
    QRandomGenerator random(20261115);
    vn::Settings::instance().setValue(vn::key::autoClose, true);
    vn::Settings::instance().setValue(vn::key::autoSurround, true);
    const auto leading = [](const QString &text) {
        int i = 0;
        while (i < text.size() && (text.at(i) == u' ' || text.at(i) == u'\t'))
            ++i;
        return i;
    };
    struct Edit
    {
        QString name;
        bool selections; // Some carets select, else none does.
        bool markdown;
        std::function<void()> act;
        std::function<void(QList<QTextCursor> &)> model;
    };
    QStringList parts;
    const std::vector<Edit> edits = {
        {QStringLiteral("enter"), true, false, [this] { QTest::keyClick(m_view, Qt::Key_Return); },
         [&](QList<QTextCursor> &live) {
             for (QTextCursor &c : live) {
                 const QString text = c.block().text();
                 c.insertText(u'\n' + text.left(qMin(leading(text), c.positionInBlock())));
             }
         }},
        {QStringLiteral("enter in lists"), true, true, [this] { QTest::keyClick(m_view, Qt::Key_Return); },
         [&](QList<QTextCursor> &live) {
             for (QTextCursor &c : live) {
                 const QString text = c.block().text();
                 const vn::md::ListLine item = vn::md::parseListLine(text);
                 if (!c.hasSelection() && item.valid && c.positionInBlock() >= item.contentStart
                     && (!item.empty || c.positionInBlock() == text.size())) {
                     if (item.empty) {
                         c.setPosition(c.block().position());
                         c.setPosition(c.block().position() + text.size(), QTextCursor::KeepAnchor);
                         c.insertText(!item.isQuote && item.lead.contains(u'>') ? item.lead : QString());
                     } else {
                         c.insertText(u'\n' + item.nextPrefix());
                     }
                     continue;
                 }
                 c.insertText(u'\n' + text.left(qMin(leading(text), c.positionInBlock())));
             }
         }},
        {QStringLiteral("tab"), false, true, [this] { QTest::keyClick(m_view, Qt::Key_Tab); },
         [&](QList<QTextCursor> &live) {
             QSet<int> items;
             for (QTextCursor &c : live) {
                 const vn::md::ListLine item = vn::md::parseListLine(c.block().text());
                 if (item.valid && !item.isQuote) {
                     if (items.contains(c.blockNumber()))
                         continue;
                     items.insert(c.blockNumber());
                     const int anchor = c.anchor() + 4;
                     const int position = c.position() + 4;
                     QTextCursor(c.block()).insertText(QStringLiteral("    "));
                     c.setPosition(anchor);
                     c.setPosition(position, QTextCursor::KeepAnchor);
                     continue;
                 }
                 c.insertText(QString(4 - c.positionInBlock() % 4, u' '));
             }
         }},
        {QStringLiteral("surround"), true, false, [this] { QTest::keyClicks(m_view, QStringLiteral("(")); },
         [&](QList<QTextCursor> &live) {
             const bool all = std::all_of(live.cbegin(), live.cend(), [](const QTextCursor &c) {
                 return c.hasSelection() && !c.selectedText().trimmed().isEmpty();
             });
             for (QTextCursor &c : live) {
                 if (!all) {
                     c.insertText(QStringLiteral("("));
                     continue;
                 }
                 const bool forward = c.position() > c.anchor();
                 const int start = c.selectionStart();
                 const int end = c.selectionEnd();
                 QTextCursor edit(c.document());
                 edit.setPosition(end);
                 edit.insertText(QStringLiteral(")"));
                 edit.setPosition(start);
                 edit.insertText(QStringLiteral("("));
                 c.setPosition(forward ? start + 1 : end + 1);
                 c.setPosition(forward ? end + 1 : start + 1, QTextCursor::KeepAnchor);
             }
         }},
        {QStringLiteral("paste a line each"), true, false, [this] { m_view->paste(); },
         [&](QList<QTextCursor> &live) {
             for (int k = 0; k < live.size(); ++k)
                 live[k].insertText(parts.at(k));
         }},
        {QStringLiteral("cut"), true, false, [this] { m_view->cut(); },
         [&](QList<QTextCursor> &live) {
             if (std::none_of(live.cbegin(), live.cend(), [](const QTextCursor &c) { return c.hasSelection(); }))
                 return; // (Whole lines then: not this test's.)
             for (QTextCursor &c : live)
                 c.removeSelectedText();
         }},
        {QStringLiteral("input method"), true, false,
         [this] {
             QInputMethodEvent event;
             event.setCommitString(QStringLiteral("é"));
             QApplication::sendEvent(m_view, &event);
         },
         [&](QList<QTextCursor> &live) {
             for (QTextCursor &c : live)
                 c.insertText(QStringLiteral("é"));
         }},
    };
    for (int round = 0; round < 140; ++round) {
        const Edit &edit = edits[round % edits.size()];
        m_view->setMarkdownEditing(edit.markdown);
        QStringList lines;
        for (int i = 0; i < 30; ++i) {
            QString line = QString(random.bounded(0, 3) * 2, u' ');
            if (edit.markdown && random.bounded(0, 2) == 0)
                line += random.bounded(0, 2) ? QStringLiteral("- ") : QStringLiteral("1. ");
            for (int w = random.bounded(0, 5); w > 0; --w)
                line += QStringLiteral("word%1 ").arg(random.bounded(100));
            lines << line;
        }
        setText(lines.join(u'\n'));
        const int length = m_doc->characterCount() - 1;
        QList<QTextCursor> chosen;
        for (int pos = random.bounded(0, 6); pos < length;) {
            QTextCursor c(m_doc);
            const int width = edit.selections && random.bounded(0, 3) == 0 ? random.bounded(1, 12) : 0;
            c.setPosition(pos);
            c.setPosition(qMin(length, pos + width), QTextCursor::KeepAnchor);
            if (random.bounded(0, 2) == 0) { // Backwards.
                const int anchor = c.anchor();
                c.setPosition(c.position());
                c.setPosition(anchor, QTextCursor::KeepAnchor);
            }
            chosen.append(c);
            pos += width + random.bounded(0, 3) * random.bounded(1, 9);
        }
        if (chosen.isEmpty())
            continue;
        m_view->setCursors(chosen, chosen.size() / 2);
        // The model starts from the carets as the view keeps them (merged).
        QTextDocument reference;
        reference.setPlainText(text());
        QList<QTextCursor> live;
        for (const TextView::CaretRange &range : m_view->caretRanges()) {
            QTextCursor c(&reference);
            c.setPosition(range.anchor);
            c.setPosition(range.position, QTextCursor::KeepAnchor);
            live.append(c);
        }
        parts.clear();
        for (int k = 0; k < live.size(); ++k)
            parts << QStringLiteral("p%1").arg(k);
        QApplication::clipboard()->setText(parts.join(u'\n'));
        edit.model(live);
        edit.act();
        std::vector<std::pair<int, int>> expected;
        for (const QTextCursor &c : std::as_const(live))
            expected.emplace_back(c.anchor(), c.position());
        std::sort(expected.begin(), expected.end(), [](const auto &a, const auto &b) {
            return std::min(a.first, a.second) < std::min(b.first, b.second);
        });
        expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
        QStringList carets;
        QStringList expectedCarets;
        for (const TextView::CaretRange &range : m_view->caretRanges())
            carets << QStringLiteral("%1>%2").arg(range.anchor).arg(range.position);
        for (const auto &[anchor, position] : expected)
            expectedCarets << QStringLiteral("%1>%2").arg(anchor).arg(position);
        const QString what = QStringLiteral("round %1, %2").arg(round).arg(edit.name);
        QVERIFY2(text() == reference.toPlainText(), qPrintable(what + QStringLiteral(": text")));
        QVERIFY2(carets == expectedCarets, qPrintable(what + QStringLiteral(": carets %1 against %2")
                                                          .arg(carets.join(u' '), expectedCarets.join(u' '))));
    }
}

void TestTextView::autoClosedPairsAtManyCarets()
{
    // Each caret's pair is deleted with Backspace, and typed over, as one
    // pair would be.
    vn::Settings::instance().setValue(vn::key::autoClose, true);
    m_view->setCodeEditing(true);
    QStringList lines;
    for (int i = 0; i < 300; ++i)
        lines << QStringLiteral("call %1 x").arg(i);
    const QString original = lines.join(u'\n');
    setText(original);
    QList<QTextCursor> carets;
    for (int i = 0; i < 300; ++i) {
        QTextCursor c(m_doc->findBlockByNumber(i));
        c.movePosition(QTextCursor::EndOfBlock);
        c.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, 2);
        carets.append(c);
    }
    m_view->setCursors(carets, 0);
    QTest::keyClicks(m_view, QStringLiteral("("));
    QVERIFY(text().startsWith(QStringLiteral("call 0() x\ncall 1() x")));
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QCOMPARE(text(), original);
    QCOMPARE(m_view->cursorCount(), 300);
    QTest::keyClicks(m_view, QStringLiteral("(a)"));
    QVERIFY(text().startsWith(QStringLiteral("call 0(a) x\ncall 1(a) x")));
    QCOMPARE(m_view->primaryCaret().position, 9); // After the closer typed over.
    m_view->setCodeEditing(false);
}

void TestTextView::otherViewsCaretsMoveAsLiveCursors()
{
    // A second view on the document keeps its carets where cursors in the
    // document would be, whatever this view does to the text: its own edits
    // (told as they are made), undo and redo, and edits made otherwise.
    QRandomGenerator random(20261116);
    m_view->setCommentMarker(QStringLiteral("//"));
    auto *other = new TextView(m_doc);
    other->resize(300, 200);
    const auto makeText = [&](int count) {
        QStringList lines;
        for (int i = 0; i < count; ++i) {
            QString line = QString(random.bounded(0, 3) * 2, u' ');
            for (int w = random.bounded(0, 5); w > 0; --w)
                line += QStringLiteral("word%1 ").arg(random.bounded(100));
            lines << line;
        }
        setText(lines.join(u'\n'));
    };
    makeText(40);
    const std::vector<std::function<void()>> edits = {
        [this] { QTest::keyClicks(m_view, QStringLiteral("ab")); },
        [this] { QTest::keyClick(m_view, Qt::Key_Backspace); },
        [this] { QTest::keyClick(m_view, Qt::Key_Delete, Qt::ControlModifier); },
        [this] { QTest::keyClick(m_view, Qt::Key_Return); },
        [this] { QTest::keyClick(m_view, Qt::Key_Tab); },
        [this] { m_view->outdent(); },
        [this] { m_view->toggleLineComment(); },
        [this] { m_view->moveLinesDown(); },
        [this] { m_view->moveLinesUp(); },
        [this] { m_view->copyLinesDown(); },
        [this] { m_view->deleteLines(); },
        [this] { m_view->insertLineBelow(); },
        [this] { m_view->insertLineAbove(); },
        [this] { m_view->trimTrailingWhitespace(); },
        [this] {
            QApplication::clipboard()->setText(QStringLiteral("one\ntwo"));
            m_view->paste();
        },
        [this] { m_view->undo(); },
        [this] { m_view->undo(); },
        [this] { m_view->redo(); },
        [this] {
            // As the find bar replaces: one block, back to front.
            QTextCursor edit(m_doc);
            edit.beginEditBlock();
            for (int at = m_doc->characterCount() - 9; at > 0; at -= 23) {
                edit.setPosition(at);
                edit.setPosition(at + 3, QTextCursor::KeepAnchor);
                TextView::insertText(edit, QStringLiteral("R"));
            }
            edit.endEditBlock();
        },
        [this] {
            // An edit block the views are not told of, as a whole.
            QTextCursor edit(m_doc);
            edit.beginEditBlock();
            edit.setPosition(m_doc->characterCount() / 2);
            edit.insertText(QStringLiteral("xyz"));
            edit.setPosition(2);
            edit.insertText(QStringLiteral("q"));
            edit.endEditBlock();
        },
    };
    // A few carets there (undo puts cursors in for them), then hundreds
    // (undo tells the views the changes it undoes, as they were made).
    for (int round = 0; round < 320; ++round) {
        const bool many = round >= 160;
        // (The edits remove more than they add.)
        if (m_doc->characterCount() < (many ? 4000 : 300))
            makeText(many ? 400 : 40);
        const int length = m_doc->characterCount() - 1;
        // This view's carets.
        QList<QTextCursor> mine;
        for (int k = random.bounded(1, 6); k > 0; --k) {
            QTextCursor c(m_doc);
            c.setPosition(random.bounded(0, length + 1));
            if (random.bounded(0, 3) == 0)
                c.setPosition(random.bounded(0, length + 1), QTextCursor::KeepAnchor);
            mine.append(c);
        }
        m_view->setCursors(mine, 0);
        // The other view's, and live cursors where they are.
        QList<QTextCursor> theirs;
        for (int k = many ? random.bounded(300, 400) : random.bounded(1, 30); k > 0; --k) {
            QTextCursor c(m_doc);
            c.setPosition(random.bounded(0, length + 1));
            if (!many && random.bounded(0, 2) == 0)
                c.setPosition(random.bounded(0, length + 1), QTextCursor::KeepAnchor);
            else if (many && random.bounded(0, 4) == 0)
                c.setPosition(qBound(0, c.position() + random.bounded(-6, 7), length), QTextCursor::KeepAnchor);
            theirs.append(c);
        }
        other->setCursors(theirs, 0);
        QList<QTextCursor> live;
        for (const TextView::CaretRange &range : other->caretRanges()) {
            QTextCursor c(m_doc);
            c.setPosition(range.anchor);
            c.setPosition(range.position, QTextCursor::KeepAnchor);
            live.append(c);
        }
        // (Not the edit made otherwise among many: the undo steps before it
        // are not known as changes after it.)
        const int which = random.bounded(int(edits.size()) - (many ? 1 : 0));
        edits[which]();
        QStringList carets;
        QStringList expected;
        for (const TextView::CaretRange &range : other->caretRanges())
            carets << QStringLiteral("%1>%2").arg(range.anchor).arg(range.position);
        for (const QTextCursor &c : std::as_const(live))
            expected << QStringLiteral("%1>%2").arg(c.anchor()).arg(c.position());
        // (Changes not told of are only known as a whole: those carets are
        // checked against cursors only where the whole change misses them.)
        if (which == int(edits.size()) - 1)
            continue;
        QVERIFY2(carets == expected, qPrintable(QStringLiteral("round %1, edit %2: carets %3 against %4")
                                                    .arg(round).arg(which).arg(carets.join(u' '), expected.join(u' '))));
    }

    // Where Backspace and Delete took text away, undo puts it back after a
    // cursor (Backspace) or before it (Delete), many carets there or few.
    for (const int count : {3, 400}) {
        for (const auto key : {Qt::Key_Backspace, Qt::Key_Delete}) {
            makeText(500);
            QList<QTextCursor> mine;
            for (int k = 1; k <= 3; ++k) {
                QTextCursor c(m_doc);
                c.setPosition(k * 1000);
                mine.append(c);
            }
            m_view->setCursors(mine, 0);
            QTest::keyClick(m_view, key);
            QList<QTextCursor> theirs = m_view->cursors();
            for (int k = int(theirs.size()); k < count; ++k) {
                QTextCursor c(m_doc);
                c.setPosition(k * 7 % (m_doc->characterCount() - 1));
                theirs.append(c);
            }
            other->setCursors(theirs, 0);
            QList<QTextCursor> live;
            for (const TextView::CaretRange &range : other->caretRanges()) {
                QTextCursor c(m_doc);
                c.setPosition(range.position);
                live.append(c);
            }
            m_view->undo();
            QStringList carets;
            QStringList expected;
            for (const TextView::CaretRange &range : other->caretRanges())
                carets << QString::number(range.position);
            for (const QTextCursor &c : std::as_const(live))
                expected << QString::number(c.position());
            QCOMPARE(carets, expected);
        }
    }
    delete other;
}

void TestTextView::manyCaretsUndoAndRedoInOneStep()
{
    QStringList lines;
    for (int i = 0; i < 3000; ++i)
        lines << QStringLiteral("lorem %1 lorem").arg(i);
    const QString original = lines.join(u'\n');
    setText(original);
    selectRange(0, 5);
    m_view->selectAllOccurrences();
    QCOMPARE(m_view->cursorCount(), 6000);
    QTest::keyClick(m_view, Qt::Key_Z);
    const QString typed = text();
    QVERIFY(typed.startsWith(QStringLiteral("z 0 z\nz 1 z")));
    QCOMPARE(m_view->cursorCount(), 6000);
    m_view->undo();
    QCOMPARE(text(), original); // One step.
    QCOMPARE(m_view->cursorCount(), 6000);
    QVERIFY(m_view->textCursor().hasSelection());
    m_view->redo();
    QCOMPARE(text(), typed);
    QCOMPARE(m_view->cursorCount(), 6000);
    m_view->undo();
    QCOMPARE(text(), original);
    // Typing again over the carets undone to goes on as before.
    QTest::keyClick(m_view, Qt::Key_Y);
    QVERIFY(text().startsWith(QStringLiteral("y 0 y\ny 1 y")));
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

void TestTextView::caretBlinkRepaintsOnlyTheCaret()
{
    // A blink repaints a strip where the caret is, and the view painted there is
    // what a whole repaint gives.
    QStringList lines;
    for (int i = 0; i < 60; ++i)
        lines << QStringLiteral("line %1 with some words in it").arg(i);
    setText(lines.join(u'\n'));
    place(5, 7);
    QWidget *viewport = m_view->viewport();
    PaintSpy spy;
    viewport->installEventFilter(&spy);
    const auto checkBlink = [&](const QString &what) {
        QTest::qWait(100);
        const QImage before = viewport->grab().toImage();
        spy.rects.clear();
        QTRY_VERIFY_WITH_TIMEOUT(!spy.rects.isEmpty(), 3000);
        const QRect strip = spy.rects.first();
        QVERIFY2(strip.width() < 60 && strip.height() < 3 * static_cast<ProbeView *>(m_view)->rowHeight(),
                 qPrintable(QStringLiteral("%1: repainted %2x%3").arg(what).arg(strip.width()).arg(strip.height())));
        QImage composed = before;
        viewport->render(&composed, strip.topLeft(), QRegion(strip));
        QVERIFY2(composed != before, qPrintable(what)); // The caret went or came.
        QVERIFY2(composed == viewport->grab().toImage(), qPrintable(what));
    };
    checkBlink(QStringLiteral("scrolling"));
    if (QTest::currentTestFailed())
        return;
    m_view->resize(1400, 500);
    m_view->setColumnChars(50);
    m_view->setColumnsShown(true);
    place(5, 7);
    checkBlink(QStringLiteral("in columns"));
}

void TestTextView::longLinePaintsTheSameInStrips()
{
    // A long line is drawn only where the view is painted; painting the view
    // a strip at a time (as a caret blink or an uncovered edge does) must give
    // what painting it whole does, selection and highlights included.
    QString line;
    for (int i = 0; i < 4000; ++i)
        line += QStringLiteral("word%1 lorem ").arg(i);
    setText(line + QStringLiteral("\nshort line\n") + line);
    m_view->setSearchHighlights(vn::search::findAll(m_doc, vn::search::Options{"lorem", false, false, false}), 3);
    selectRange(900, 41000);
    const auto compareInStrips = [&](const QString &what) {
        const QImage whole = m_view->viewport()->grab().toImage();
        const int w = whole.width();
        const int h = whole.height();
        const int strip = 37; // Not a multiple of the row height.
        for (int y = 0; y < h; y += strip) {
            const QRect rect(0, y, w, qMin(strip, h - y));
            QImage part(whole.size(), whole.format());
            part.fill(Qt::magenta);
            m_view->viewport()->render(&part, rect.topLeft(), QRegion(rect));
            QVERIFY2(part.copy(rect) == whole.copy(rect), qPrintable(QStringLiteral("%1, strip at %2").arg(what).arg(y)));
        }
    };
    for (const int top : {0, 90, 400}) {
        m_view->verticalScrollBar()->setValue(top);
        compareInStrips(QStringLiteral("scrolled to %1").arg(top));
    }
    // The same line in columns, which cut the line at their edges.
    m_view->resize(1400, 500);
    m_view->setColumnChars(50);
    m_view->setColumnsShown(true);
    compareInStrips(QStringLiteral("in columns"));
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
    const QPoint pastEnd = pointAt(2, 20);
    boxSelect(pastEnd, pastEnd, Qt::ShiftModifier | Qt::AltModifier);
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

// The fence state of every line, by scanning the whole document afresh (the
// rules of core/src/format.rs: a fence opens at a mark and closes at the next
// mark of the same character, at least as long, with nothing after it).
static QList<bool> scanFences(const QTextDocument &doc)
{
    QList<bool> in;
    QChar openCh;
    int openRun = 0;
    bool open = false;
    for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
        in.append(open);
        const QString t = b.text();
        qsizetype i = 0;
        while (i < t.size() && (t.at(i) == u' ' || t.at(i) == u'>'))
            ++i;
        if (i >= t.size() || (t.at(i) != u'`' && t.at(i) != u'~'))
            continue;
        const QChar ch = t.at(i);
        qsizetype run = i;
        while (run < t.size() && t.at(run) == ch)
            ++run;
        if (run - i < 3)
            continue;
        bool closes = true;
        for (qsizetype k = run; k < t.size(); ++k)
            closes = closes && t.at(k).isSpace();
        if (!open) {
            open = true;
            openCh = ch;
            openRun = int(run - i);
        } else if (ch == openCh && run - i >= openRun && closes) {
            open = false;
        }
    }
    return in;
}

void TestTextView::fenceStateAfterEdits()
{
    m_view->setMarkdownEditing(true);
    const QStringList pieces = {
        QStringLiteral("```"), QStringLiteral("````"), QStringLiteral("~~~"), QStringLiteral("~~~~~"),
        QStringLiteral("```cpp"), QStringLiteral("  ```"), QStringLiteral("> ```"), QStringLiteral("- ```"),
        QStringLiteral(">   ~~~  "), QStringLiteral("``"), QStringLiteral("text"), QStringLiteral("| a | b |"),
        QStringLiteral(""), QStringLiteral("``` x"), QStringLiteral("\t```"),
    };
    QRandomGenerator rng(12345);
    const auto line = [&] { return pieces.at(int(rng.bounded(quint32(pieces.size())))); };
    QStringList lines;
    for (int i = 0; i < 400; ++i)
        lines << line();
    setText(lines.join(u'\n'));
    const auto check = [&](const char *what) {
        const QList<bool> want = scanFences(*m_doc);
        QCOMPARE(int(want.size()), m_doc->blockCount());
        // Asked in a mixed order, so that the scan is made in steps.
        for (int n = 0; n < 40; ++n) {
            const int l = int(rng.bounded(quint32(want.size())));
            QVERIFY2(FenceProbe::inFence(m_view, l) == want.at(l), what);
        }
        for (int l = 0; l < want.size(); ++l)
            QVERIFY2(FenceProbe::inFence(m_view, l) == want.at(l), what);
    };
    check("initial");
    for (int step = 0; step < 300; ++step) {
        const int blocks = m_doc->blockCount();
        QTextCursor c(m_doc);
        const int kind = int(rng.bounded(6u));
        c.setPosition(m_doc->findBlockByNumber(int(rng.bounded(quint32(blocks)))).position());
        if (kind == 0) {
            c.insertText(line() + u'\n');
        } else if (kind == 1) {
            c.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, 1 + int(rng.bounded(40u)));
            c.removeSelectedText();
        } else if (kind == 2) {
            c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            c.insertText(line());
        } else if (kind == 3) {
            c.insertText(QStringList{line(), line(), line()}.join(u'\n') + u'\n');
        } else if (kind == 4) {
            m_doc->undo();
        } else {
            m_doc->redo();
        }
        if (rng.bounded(3u) == 0)
            QCoreApplication::processEvents(); // (Lets the idle scan go on.)
        if (rng.bounded(4u) == 0 || step % 25 == 0)
            check("after an edit");
    }
    check("end");
    // A scan finished in idle time gives the same answers.
    QTRY_VERIFY(m_view->isVisible());
    for (int i = 0; i < 200; ++i)
        QCoreApplication::processEvents();
    check("after idle");
}

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

void TestTextView::dragTo(const QPointF &point)
{
    QWidget *viewport = m_view->viewport();
    QMouseEvent move(QEvent::MouseMove, point, viewport->mapToGlobal(point), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(viewport, &move);
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

// The mouse selects in text order across columns; below the last column the
// view moves on a column at a time while the selection grows, above the first
// back again.
void TestTextView::columnsMouseSelectsAcrossColumns()
{
    auto *probe = static_cast<ProbeView *>(m_view);
    showColumns(300);
    const int per = perColumn();
    const int count = m_view->shownColumnCount();
    QWidget *viewport = m_view->viewport();
    const QPointF from = columnPoint(0, 1);
    const int anchor = probe->positionAtPoint(from);
    QTest::mousePress(viewport, Qt::LeftButton, {}, from.toPoint());
    dragTo(columnPoint(1, 3));
    QCOMPARE(m_view->textCursor().anchor(), anchor);
    QCOMPARE(caretLine(), per + 3);
    QVERIFY(m_view->selectedText().contains(QStringLiteral("line %1").arg(per))); // Column 1's end.
    // Back into column 1: it shrinks.
    dragTo(columnPoint(0, 4));
    QCOMPARE(caretLine(), 4);
    QCOMPARE(m_view->textCursor().anchor(), anchor);
    // Below the last column, inside the window.
    const QRect lastColumn = m_view->columnRect(count - 1);
    dragTo(QPointF(lastColumn.center().x(), lastColumn.bottom() + 8));
    QTRY_COMPARE(m_view->columnLineRange().first, per);
    QCOMPARE(m_view->textCursor().anchor(), anchor); // (Off screen now.)
    QTRY_COMPARE(m_view->columnLineRange().first, 2 * per);
    QTRY_COMPARE(caretLine(), m_view->columnLineRange().second);
    // Above the first column: back.
    const QRect firstColumn = m_view->columnRect(0);
    const QPointF above(firstColumn.center().x(), firstColumn.top() - 8);
    dragTo(above);
    QTRY_COMPARE(m_view->columnLineRange().first, per);
    QTRY_COMPARE(caretLine(), per);
    QTest::mouseRelease(viewport, Qt::LeftButton, {}, above.toPoint());
    QTest::qWait(600);
    QCOMPARE(m_view->columnLineRange().first, per); // (Released: it stays.)
    QCOMPARE(m_view->textCursor().anchor(), anchor);
    QCOMPARE(caretLine(), per);
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
    // (The time the thread worked: a busy machine takes seconds more for the
    // same work.)
    ThreadCpuTimer timer;
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

// ---------------------------------------------------------------------------
// Very long lines, laid out in pieces (vn::LineLayout)

void TestTextView::longLineJoinsAreSafe_data()
{
    QTest::addColumn<QString>("line");
    for (const auto &[name, line] : longLines())
        QTest::newRow(name) << line;
}

void TestTextView::longLineJoinsAreSafe()
{
    QFETCH(QString, line);
    setText(line + QStringLiteral("\nshort"));
    const QList<int> joins = joinsOf(m_doc->firstBlock());
    QVERIFY(joins.size() >= 3);
    const QTextBlock block = m_doc->firstBlock();
    for (int join : joins) {
        // Never inside a grapheme: a surrogate pair, a combining sequence,
        // an emoji with its modifier.
        QTextBoundaryFinder graphemes(QTextBoundaryFinder::Grapheme, line);
        graphemes.setPosition(join);
        QVERIFY2(graphemes.isAtBoundary(), qPrintable(QString::number(join)));
        // Never where a right-to-left run could reach across: the nearest
        // strong characters each side are left-to-right.
        for (int step : {-1, 1}) {
            for (int i = step < 0 ? join - 1 : join; i >= 0 && i < line.size(); i += step) {
                const QChar::Direction d = directionAt(line, i);
                QVERIFY2(!rightToLeft(d), qPrintable(QStringLiteral("join %1, %2 at %3").arg(join).arg(int(d)).arg(i)));
                if (d == QChar::DirL)
                    break;
            }
        }
        // A piece that does not start right after a tab holds a tab only last.
        if (line.at(join - 1) != u'\t') {
            const int next = joins.indexOf(join) + 1 < joins.size() ? joins.at(joins.indexOf(join) + 1) : line.size();
            const qsizetype tab = line.indexOf(u'\t', join);
            QVERIFY(tab < 0 || tab >= next - 1);
        }
    }
    // In text with no space or punctuation for thousands of characters,
    // joins land inside words.
    if (!line.contains(u' ') && !line.contains(u',')) {
        for (int join : joins)
            QVERIFY(line.at(join - 1).isLetterOrNumber() || line.at(join).isLetterOrNumber());
    }
    Q_UNUSED(block);
}

void TestTextView::longLineHitTestingMatchesTheWholeLine_data()
{
    longLineJoinsAreSafe_data();
}

void TestTextView::longLineHitTestingMatchesTheWholeLine()
{
    // Word Wrap off, the pieces sit side by side where the whole line has
    // its characters: carets and clicks land as they would on it.
    QFETCH(QString, line);
    auto *probe = static_cast<ProbeView *>(m_view);
    m_view->setWordWrap(false);
    setText(line + QStringLiteral("\nshort"));
    m_view->grab();
    const std::unique_ptr<QTextLayout> whole = wholeLine();
    const QTextLine wholeRow = whole->lineAt(0);
    QList<int> positions;
    for (int join : joinsOf(m_doc->firstBlock())) {
        for (int d = -6; d <= 6; ++d)
            positions.append(join + d);
    }
    for (int i = 0; i < 100; ++i)
        positions.append(QRandomGenerator::global()->bounded(int(line.size())));
    positions.append(int(line.size()));
    const qreal y = probe->rowHeight() / 2.0;
    for (int p : std::as_const(positions)) {
        const qreal x = wholeRow.cursorToX(p);
        // A click just right of where the whole line has the position.
        const qreal at = probe->left() + x + 0.4 - m_view->horizontalScrollBar()->value();
        QCOMPARE(probe->positionAtPoint(QPointF(at, y)), wholeRow.xToCursor(x + 0.4));
        // The caret.
        QTextCursor c(m_doc);
        c.setPosition(p);
        m_view->setTextCursor(c);
        const QRect caret = probe->caretRectangle();
        const qreal expected = probe->left() + x - m_view->horizontalScrollBar()->value();
        QVERIFY2(qAbs(caret.x() - expected) <= 1, qPrintable(QStringLiteral("%1: %2 vs %3").arg(p).arg(caret.x()).arg(expected)));
    }
    // Scrolled to the far end, the row is as wide as the whole line.
    QVERIFY(m_view->horizontalScrollBar()->maximum() >= int(wholeRow.naturalTextWidth() - m_view->viewport()->width()));
}

void TestTextView::longLineCaretMovesAsQtDoes()
{
    // Left, Right, Ctrl+Left and Ctrl+Right look only at the text around
    // the caret in a long line; they go where Qt's own moves go, word
    // longer than what they look at first included.
    QString line = longLine(QStringLiteral("lorem ipsum, e\u0301te\u0301 ab\U0001F600c\U0001F44D\U0001F3FDd dolor "), 20000)
                   + QString(5000, u'w') + QStringLiteral(" tail ") + longLine(QStringLiteral("xyz"), 3000);
    setText(line + QStringLiteral("\nshort"));
    QTextDocument reference;
    reference.setPlainText(m_doc->toPlainText());
    QList<int> positions{0, 1, int(line.size()) - 1, int(line.size()), 20000, 20001, 22500, 25005};
    for (int join : joinsOf(m_doc->firstBlock())) {
        for (int d = -3; d <= 3; ++d)
            positions.append(join + d);
    }
    for (int i = 0; i < 60; ++i)
        positions.append(QRandomGenerator::global()->bounded(int(line.size())));
    const QList<std::pair<Qt::KeyboardModifiers, std::pair<Qt::Key, QTextCursor::MoveOperation>>> moves = {
        {Qt::NoModifier, {Qt::Key_Left, QTextCursor::PreviousCharacter}},
        {Qt::NoModifier, {Qt::Key_Right, QTextCursor::NextCharacter}},
        {Qt::ControlModifier, {Qt::Key_Left, QTextCursor::PreviousWord}},
        {Qt::ControlModifier, {Qt::Key_Right, QTextCursor::NextWord}},
    };
    for (int p : std::as_const(positions)) {
        QTextCursor ref(&reference);
        ref.setPosition(p);
        if (ref.position() != p) // (Inside a surrogate pair.)
            continue;
        for (const auto &[mods, move] : moves) {
            QTextCursor c(m_doc);
            c.setPosition(p);
            m_view->setTextCursor(c);
            QTest::keyClick(m_view, move.first, mods);
            QTextCursor expected(&reference);
            expected.setPosition(p);
            expected.movePosition(move.second);
            QVERIFY2(m_view->textCursor().position() == expected.position(),
                     qPrintable(QStringLiteral("%1 from %2: %3, Qt %4")
                                    .arg(int(move.second)).arg(p).arg(m_view->textCursor().position()).arg(expected.position())));
        }
    }
    // Backspace and Delete take what Qt's moves would.
    for (int p : {1, 20001, 25005, int(line.size()) - 2}) {
        for (const auto &[key, op] : {std::pair{Qt::Key_Backspace, QTextCursor::PreviousCharacter},
                                      std::pair{Qt::Key_Delete, QTextCursor::NextCharacter}}) {
            setText(line + QStringLiteral("\nshort"));
            QTextCursor c(m_doc);
            c.setPosition(p);
            m_view->setTextCursor(c);
            QTest::keyClick(m_view, key);
            reference.setPlainText(line + QStringLiteral("\nshort"));
            QTextCursor r(&reference);
            r.setPosition(p);
            r.movePosition(op, QTextCursor::KeepAnchor);
            r.removeSelectedText();
            QCOMPARE(text(), reference.toPlainText());
        }
    }
}

void TestTextView::longLineEditsLookAsLaidOutAfresh()
{
    // Edits lay out again only the pieces they reach; the line looks as a
    // new view lays it out, at the joins too: typing, deleting, Enter and
    // Backspace across lines, pastes, undo and redo.
    m_view->setWordWrap(false);
    const QString line = longLines().at(0).second + longLines().at(3).second;
    setText(QStringLiteral("first\n") + line + QStringLiteral("\nlast"));
    m_view->grab();
    QRandomGenerator random(7);
    for (int step = 0; step < 40; ++step) {
        const QTextBlock block = m_doc->findBlockByNumber(1);
        const QList<int> joins = joinsOf(block);
        const int length = block.length() - 1;
        int at = joins.isEmpty() ? length / 2 : joins.at(random.bounded(int(joins.size()))) + random.bounded(-4, 5);
        at = qBound(1, at, length - 1);
        QTextCursor c(m_doc);
        c.setPosition(block.position() + at);
        m_view->setTextCursor(c);
        switch (step % 8) {
        case 0:
            type(QStringLiteral("abc"));
            break;
        case 1:
            QTest::keyClick(m_view, Qt::Key_Backspace);
            QTest::keyClick(m_view, Qt::Key_Backspace);
            break;
        case 2:
            QTest::keyClick(m_view, Qt::Key_Delete);
            break;
        case 3:
            type(QStringLiteral("\t\t"));
            break;
        case 4:
            m_view->pasteTextAt(c.position(), longLine(QStringLiteral("pasted words "), 9000));
            break;
        case 5:
            QTest::keyClick(m_view, Qt::Key_Z, Qt::ControlModifier);
            break;
        case 6:
            QTest::keyClick(m_view, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
            break;
        case 7:
            // Split the line and join it again.
            QTest::keyClick(m_view, Qt::Key_Return);
            m_view->grab();
            QTest::keyClick(m_view, Qt::Key_Backspace);
            break;
        }
        m_view->grab();
        if (step % 4 == 3) {
            compareWithFreshView(1);
            if (QTest::currentTestFailed())
                return;
        }
    }
    // Enter in the middle leaves two long lines, each as laid out afresh.
    const QTextBlock block = m_doc->findBlockByNumber(1);
    QTextCursor c(m_doc);
    c.setPosition(block.position() + (block.length() - 1) / 2);
    m_view->setTextCursor(c);
    QTest::keyClick(m_view, Qt::Key_Return);
    m_view->grab();
    compareWithFreshView(1);
    QTextCursor end(m_doc->findBlockByNumber(1));
    end.movePosition(QTextCursor::EndOfBlock);
    m_view->setTextCursor(end);
    compareWithFreshView(1);
    compareWithFreshView(2);
}

void TestTextView::longLineWrapsInPieces()
{
    // With Word Wrap on, each piece wraps on its own: about as many rows as
    // the whole line, every row reached by Down and back by Up.
    auto *probe = static_cast<ProbeView *>(m_view);
    m_view->setWordWrap(true);
    const QString line = longLines().at(0).second;
    setText(line + QStringLiteral("\nshort"));
    m_view->resize(600, 30000);
    m_view->grab();
    qreal top = 0;
    qreal bottom = 0;
    QVERIFY(probe->bounds(0, &top, &bottom));
    const int rows = qRound((bottom - top) / probe->rowHeight());
    QTextLayout whole(line, m_view->font());
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    whole.setTextOption(option);
    whole.beginLayout();
    int wholeRows = 0;
    for (QTextLine l = whole.createLine(); l.isValid(); l = whole.createLine())
        l.setLineWidth(m_view->viewport()->width() - probe->left()), ++wholeRows;
    whole.endLayout();
    const int pieces = int(joinsOf(m_doc->firstBlock()).size()) + 1;
    QVERIFY2(rows >= wholeRows && rows <= wholeRows + pieces, qPrintable(QStringLiteral("%1 %2").arg(rows).arg(wholeRows)));
    // Each row starts where a click at its left edge lands, after the last.
    int last = -1;
    for (int r = 0; r < rows; ++r) {
        const int p = probe->positionAtPoint(QPointF(probe->left(), top + r * probe->rowHeight() + 2));
        QVERIFY(p > last);
        last = p;
    }
    m_view->resize(600, 400);
    m_view->setTextCursor(QTextCursor(m_doc));
    int previous = 0;
    QList<int> visited{0};
    for (int r = 1; r < rows; ++r) {
        QTest::keyClick(m_view, Qt::Key_Down);
        QVERIFY(m_view->textCursor().position() > previous);
        previous = m_view->textCursor().position();
        visited.append(previous);
    }
    for (int r = rows - 2; r >= 0; --r) {
        QTest::keyClick(m_view, Qt::Key_Up);
        QCOMPARE(m_view->textCursor().position(), visited.at(r));
    }
    // Typing at a join keeps the rows in order.
    const int join = joinsOf(m_doc->firstBlock()).at(2);
    QTextCursor c(m_doc);
    c.setPosition(join);
    m_view->setTextCursor(c);
    type(QStringLiteral("inserted words "));
    m_view->grab();
    QCOMPARE(m_doc->firstBlock().text(), QString(line).insert(join, QStringLiteral("inserted words ")));
}

void TestTextView::longLineSelectionAndMatchesSpanAJoin()
{
    // A selection and a find match across a join are filled without a gap.
    auto *probe = static_cast<ProbeView *>(m_view);
    m_view->setWordWrap(false);
    setText(longLines().at(1).second + QStringLiteral("\nshort"));
    const int join = joinsOf(m_doc->firstBlock()).at(3);
    const std::unique_ptr<QTextLayout> whole = wholeLine();
    const auto filled = [&](const QColor &color, int from, int to) {
        const QImage image = m_view->viewport()->grab().toImage();
        const int hscroll = m_view->horizontalScrollBar()->value();
        const int x1 = int(std::ceil(probe->left() + whole->lineAt(0).cursorToX(from) - hscroll)) + 1;
        const int x2 = int(probe->left() + whole->lineAt(0).cursorToX(to) - hscroll) - 1;
        if (x1 >= x2)
            return false;
        for (int x = x1; x <= x2; ++x) {
            if (image.pixelColor(x, 1) != color)
                return false;
        }
        return true;
    };
    selectRange(join - 20, join + 20);
    m_view->clearFocus();
    QVERIFY(filled(vn::theme::color(vn::theme::Role::selection), join - 20, join + 20));
    QTextCursor c(m_doc);
    c.setPosition(join + 30);
    m_view->setTextCursor(c);
    m_view->setSearchHighlights({{join - 5, join + 5}, {join + 6, join + 9}}, 0);
    QVERIFY(filled(QColor(0xE8, 0xC2, 0x5C), join - 5, join + 5));
    QVERIFY(filled(QColor(0xF2, 0xDF, 0xA6), join + 6, join + 9));
    // The whole line selected: the selection colour wherever the view is.
    m_view->selectAll();
    for (int value : {0, m_view->horizontalScrollBar()->maximum() / 2, m_view->horizontalScrollBar()->maximum()}) {
        m_view->horizontalScrollBar()->setValue(value);
        const QImage image = m_view->viewport()->grab().toImage();
        QCOMPARE(image.pixelColor(int(probe->left()) + 40, 1), vn::theme::color(vn::theme::Role::selection));
    }
}

void TestTextView::longLineComposes()
{
    // Text being composed in a long line shows in its piece, moving the
    // rest along; committed, it is typed.
    m_view->setWordWrap(false);
    const QString line = longLines().at(0).second;
    setText(line + QStringLiteral("\nshort"));
    const int join = joinsOf(m_doc->firstBlock()).at(1);
    QTextCursor c(m_doc);
    c.setPosition(join);
    m_view->setTextCursor(c);
    m_view->grab();
    const int before = m_view->horizontalScrollBar()->maximum();
    QInputMethodEvent compose(QStringLiteral("\u304B\u306A"), {});
    QApplication::sendEvent(m_view, &compose);
    m_view->grab();
    QVERIFY(m_view->horizontalScrollBar()->maximum() > before);
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("\u4EEE"));
    QApplication::sendEvent(m_view, &commit);
    m_view->grab();
    QCOMPARE(m_doc->firstBlock().text(), QString(line).insert(join, QStringLiteral("\u4EEE")));
    compareWithFreshView(0);
}

void TestTextView::emptyInputMethodEventKeepsTheScroll()
{
    // An input method that reports nothing composed (as it does when focus
    // comes and goes) leaves the view where it was scrolled, away from the
    // caret; composing and committing still bring the caret into view.
    QStringList lines;
    for (int i = 0; i < 400; ++i)
        lines << QStringLiteral("line %1").arg(i + 1);
    setText(lines.join(u'\n'));
    m_view->grab();
    m_view->scrollToLine(300);
    QCOMPARE(m_view->topLine(), 300);
    QInputMethodEvent nothing;
    QApplication::sendEvent(m_view, &nothing);
    QCOMPARE(m_view->topLine(), 300);

    QInputMethodEvent compose(QStringLiteral("かな"), {});
    QApplication::sendEvent(m_view, &compose);
    QVERIFY(m_view->topLine() < 300);
    // Taking the composition back is a change, so it is handled.
    m_view->scrollToLine(300);
    QInputMethodEvent cleared;
    QApplication::sendEvent(m_view, &cleared);
    QVERIFY(m_view->topLine() < 300);
}

void TestTextView::longLineInColumns()
{
    // Columns show a long line's rows like any other's; typing in it there
    // keeps the caret in sight.
    QStringList lines;
    for (int i = 0; i < 30; ++i)
        lines << QStringLiteral("line %1").arg(i + 1);
    lines.insert(10, longLines().at(0).second);
    setText(lines.join(u'\n'));
    m_view->resize(1400, 500);
    m_view->setColumnChars(50);
    for (bool wrap : {true, false}) {
        m_view->setWordWrap(wrap);
        m_view->setColumnsShown(true, 0);
        m_view->grab();
        QVERIFY(m_view->shownColumnCount() >= 2);
        const int join = joinsOf(m_doc->findBlockByNumber(10)).at(1);
        QTextCursor c(m_doc->findBlockByNumber(10));
        c.setPosition(c.position() + join);
        m_view->setTextCursor(c);
        m_view->grab();
        type(QStringLiteral("xy"));
        m_view->grab();
        QVERIFY(m_view->isLineShown(10));
        QCOMPARE(m_doc->findBlockByNumber(10).text().mid(join, 2), QStringLiteral("xy"));
        for (int i = 0; i < 5; ++i) {
            m_view->columnsPageForward();
            m_view->grab();
        }
        if (wrap) {
            QFont zoom = m_view->font(); zoom.setPointSizeF(14); m_view->setFont(zoom);
            m_view->columnsToStart();
            m_view->clearFocus();
            QTextDocument doc;
            doc.setDocumentLayout(new vn::DocumentLayout(&doc));
            doc.setPlainText(m_doc->toPlainText());
            ProbeView fresh(&doc);
            fresh.setFont(zoom); fresh.resize(m_view->size()); fresh.setColumnChars(50);
            fresh.setColumnsShown(true, 0); fresh.show();
            for (int join : joinsOf(doc.findBlockByNumber(10))) {
                QTextCursor caret(doc.findBlockByNumber(10));
                caret.setPosition(caret.position() + join); fresh.setTextCursor(caret);
            }
            QTextCursor caret(&doc); caret.setPosition(m_view->textCursor().position());
            fresh.setTextCursor(caret); fresh.clearFocus(); fresh.columnsToStart();
            QCoreApplication::processEvents(); // Place the controls queued by the font change.
            for (QWidget *view : {static_cast<QWidget *>(m_view), static_cast<QWidget *>(&fresh)})
                for (QWidget *child : view->findChildren<QWidget *>()) child->setAttribute(Qt::WA_UnderMouse, false);
            for (int direction : {1, -1}) {
                for (int page = 0; page < 5; ++page) {
                    const auto expected = fresh.viewport()->grab().toImage();
                    const auto actual = m_view->viewport()->grab().toImage();
                    QCOMPARE(actual, expected);
                    if (direction > 0) { fresh.columnsPageForward(); m_view->columnsPageForward(); }
                    else { fresh.columnsPageBack(); m_view->columnsPageBack(); }
                }
            }
            fresh.columnsToEnd(); m_view->columnsToEnd();
            QCOMPARE(fresh.viewport()->grab().toImage(), m_view->viewport()->grab().toImage());
            QCOMPARE(m_view->columnLineRange().second, m_doc->blockCount() - 1);
        }
        m_view->columnsToStart();
        m_view->setColumnsShown(false);
        QTest::keyClick(m_view, Qt::Key_Z, Qt::ControlModifier);
    }
}

void TestTextView::longLineTypingIsFast()
{
    // A key in a line of a megabyte lays out a piece or two, not the line
    // (a fifth of a second a key before; a few milliseconds now).
    m_view->setWordWrap(false);
    const QString line = longLine(QStringLiteral("{\"id\":12345,\"name\":\"value\",\"list\":[1,2,3]},"), 1'000'000);
    setText(line);
    QTextCursor c(m_doc);
    c.setPosition(500'000);
    m_view->setTextCursor(c);
    m_view->grab();
    for (bool wrap : {false, true}) {
        m_view->setWordWrap(wrap);
        m_view->grab();
        // The fastest of three tries: a loaded machine stalls now and then.
        qint64 fastest = LLONG_MAX;
        for (int attempt = 0; attempt < 3 && fastest >= 20 * 60; ++attempt) {
            ThreadCpuTimer timer;
            for (int i = 0; i < 10; ++i) {
                QTest::keyClick(m_view, Qt::Key_A);
                m_view->grab();
                QTest::keyClick(m_view, Qt::Key_Backspace);
                m_view->grab();
            }
            fastest = qMin(fastest, timer.elapsed());
        }
        QVERIFY2(fastest < 20 * 60, qPrintable(QString::number(fastest)));
    }
    QCOMPARE(m_doc->firstBlock().text(), line);
}


std::unique_ptr<vn::LineLayout> TestTextView::exactFirstLine(ProbeView &view)
{
    auto exact = std::make_unique<vn::LineLayout>();
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setTabStopDistance(QFontMetricsF(view.font()).horizontalAdvance(u' ') * 4);
    const vn::LineLayout::Setup setup{view.font(), option, true, view.widthForText(), qreal(view.rowHeight()), 0};
    if (!exact->build(view.document()->firstBlock(), setup))
        return nullptr;
    while (exact->settleOne(0)) {}
    return exact;
}

void TestTextView::compareWithSettledLongLine()
{
    auto *probe = static_cast<ProbeView *>(m_view);
    const int top = probe->positionAtPoint(QPointF(probe->left(), 0));
    const qreal remainder = std::fmod(m_view->scrollPixels(), probe->rowHeight());
    const QImage image = m_view->viewport()->grab().toImage();
    QTextDocument doc;
    doc.setDocumentLayout(new vn::DocumentLayout(&doc));
    doc.setPlainText(m_doc->toPlainText());
    ProbeView fresh(&doc);
    fresh.setFont(m_view->font());
    fresh.resize(m_view->size());
    fresh.setWordWrap(true);
    fresh.show();
    // Force every piece to be asked for in the reference, without an idle
    // timer or the production view's estimates deciding its row numbers.
    vn::LineLayout exact;
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setTabStopDistance(QFontMetricsF(fresh.font()).horizontalAdvance(u' ') * 4);
    vn::LineLayout::Setup setup{fresh.font(), option, true,
        fresh.widthForText(), qreal(fresh.rowHeight()), 0};
    QVERIFY(exact.build(doc.firstBlock(), setup));
    while (exact.settleOne(0)) {}
    const int row = exact.lineForTextPosition(top).lineNumber();
    // Ask the reference view for every piece too, then restore its carets.
    for (int position : exact.pieceStarts()) {
        QTextCursor c(&doc); c.setPosition(position); fresh.setTextCursor(c);
    }
    QList<QTextCursor> carets;
    for (const QTextCursor &c : m_view->cursors()) {
        QTextCursor copy(&doc); copy.setPosition(c.anchor()); copy.setPosition(c.position(), QTextCursor::KeepAnchor);
        carets.append(copy);
    }
    fresh.setCursors(carets, 0);
    fresh.clearFocus();
    fresh.scrollToLinePosition((row * fresh.rowHeight() + remainder) / (exact.lineCount() * qreal(fresh.rowHeight())));
    QCOMPARE(fresh.positionAtPoint(QPointF(fresh.left(), 0)), top);
    QCOMPARE(fresh.viewport()->grab().toImage(), image);
}

void TestTextView::longLineResetupKeepsPiecesAndSettlesExact()
{
    QTextDocument doc;
    doc.setPlainText(longLine(QStringLiteral("words, ii WWW 1234 \t"), 100000));
    vn::LineLayout lazy;
    vn::LineLayout::Setup setup{m_view->font(), {}, true, 430, 20, 0};
    setup.option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setup.option.setTabStopDistance(40);
    QVERIFY(lazy.build(doc.firstBlock(), setup));
    QVERIFY(lazy.unmeasuredCount() > 0);
    const auto starts = lazy.pieceStarts();
    for (int step = 0; step < 4; ++step) {
        setup.wrapWidth = 300 + step * 53;
        setup.font.setPointSizeF(10 + step);
        QVERIFY(lazy.resetup(doc.firstBlock(), setup));
        QCOMPARE(lazy.pieceStarts(), starts);
        while (lazy.settleOne(0)) {}
        QCOMPARE(lazy.unmeasuredCount(), 0);
        vn::LineLayout fresh;
        QVERIFY(fresh.build(doc.firstBlock(), setup));
        while (fresh.settleOne(0)) {}
        QCOMPARE(lazy.lineCount(), fresh.lineCount());
        for (int row = 0; row < fresh.lineCount(); ++row)
            QCOMPARE(lazy.lineAt(row).textStart(), fresh.lineAt(row).textStart());
        setup.lineHeight += 2;
        QVERIFY(lazy.resetup(doc.firstBlock(), setup));
        QCOMPARE(lazy.unmeasuredCount(), 0);
        QCOMPARE(lazy.lineCount(), fresh.lineCount());
    }
}

void TestTextView::longLineRebuildKeepsEstimates()
{
    QTextDocument doc;
    doc.setPlainText(longLine(QStringLiteral("alpha beta WWW iii "), 100000));
    vn::LineLayout::Setup setup{m_view->font(), {}, true, 430, 20, 0};
    setup.option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    vn::LineLayout before;
    QVERIFY(before.build(doc.firstBlock(), setup));
    before.settleAround(60000);
    const auto starts = before.pieceStarts();
    const int count = before.lineCount(), pending = before.unmeasuredCount();
    QVERIFY(pending > 0);
    vn::LineLayout after;
    QVERIFY(after.rebuild(doc.firstBlock(), setup, &before, before.length(), nullptr, 0));
    QCOMPARE(after.unmeasuredCount(), pending);
    QCOMPARE(after.lineCount(), count);
    QCOMPARE(after.pieceStarts(), starts);
    setup.wrapWidth = 300; // An edit may receive pieces with a stale setup too.
    vn::LineLayout changed;
    QVERIFY(changed.rebuild(doc.firstBlock(), setup, &after, after.length(), nullptr, 0));
    QCOMPARE(changed.pieceStarts(), starts);
    QCOMPARE(changed.unmeasuredCount(), int(starts.size()));
}

void TestTextView::longLineEstimatesShowExactScreen()
{
    // Capture without an event-loop wait: the 300 ms idle timer cannot help.
    setText(longLine(QStringLiteral("alpha, WWW iii 12345 words "), 1000000));
    m_view->setWordWrap(true);
    auto *probe = static_cast<ProbeView *>(m_view);
    QTextCursor cursor(m_doc);
    cursor.setPosition(650000);
    m_view->setTextCursor(cursor);
    m_view->clearFocus();
    for (int step = 0; step < 8; ++step) {
        const int before = probe->positionAtPoint(QPointF(probe->left(), 0));
        if (step % 2 == 0) m_view->resize(550 + (step / 2) * 43, 400);
        else { QFont f = m_view->font(); f.setPointSizeF(QList<int>{11, 14, 9, 16}.at(step / 2)); m_view->setFont(f); }
        const QImage immediate = m_view->viewport()->grab().toImage();
        compareWithSettledLongLine();
        const int top = probe->positionAtPoint(QPointF(probe->left(), 0));
        // The prior top character remains on the first row (a wider row may
        // start slightly before it).
        const int next = probe->positionAtPoint(QPointF(probe->left(), probe->rowHeight()));
        QVERIFY2(top <= before && before < next, qPrintable(QStringLiteral("step %1: %2 <= %3 < %4").arg(step).arg(top).arg(before).arg(next)));
        QTest::qWait(900);
        QCOMPARE(probe->positionAtPoint(QPointF(probe->left(), 0)), top);
        QCOMPARE(m_view->viewport()->grab().toImage(), immediate);
    }
}

void TestTextView::longLineWheelUpIntoEstimatesKeepsScreen()
{
    setText(longLine(QStringLiteral("alpha beta WWW iii words, "), 1000000));
    m_view->setWordWrap(true);
    auto *probe = static_cast<ProbeView *>(m_view);
    QTextCursor c(m_doc); c.setPosition(700000); m_view->setTextCursor(c);
    m_view->resize(470, 400);
    m_view->clearFocus();
    for (int i = 0; i < 30; ++i) {
        QWheelEvent wheel(QPointF(200, 100), QPointF(200, 100), {}, QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(m_view->viewport(), &wheel);
    }
    const int top = probe->positionAtPoint(QPointF(probe->left(), 2));
    const auto image = m_view->viewport()->grab().toImage();
    compareWithSettledLongLine();
    QTest::qWait(900);
    QCOMPARE(probe->positionAtPoint(QPointF(probe->left(), 2)), top);
    QCOMPARE(m_view->viewport()->grab().toImage(), image);
}

void TestTextView::longLineCaretMovesAfterResize()
{
    setText(longLine(QStringLiteral("alpha beta words 12345 "), 1000000));
    m_view->setWordWrap(true);
    QList<QTextCursor> carets;
    for (int position : {450000, 650000}) {
        QTextCursor c(m_doc); c.setPosition(position); carets.append(c);
    }
    m_view->setCursors(carets, 0);
    m_view->resize(470, 400);
    // The same moves in a view whose line is laid out whole first.
    QTextDocument doc;
    doc.setDocumentLayout(new vn::DocumentLayout(&doc));
    doc.setPlainText(m_doc->toPlainText());
    ProbeView fresh(&doc);
    fresh.resize(m_view->size());
    fresh.setFont(m_view->font());
    fresh.setWordWrap(true);
    fresh.show();
    QVERIFY(QTest::qWaitForWindowExposed(&fresh));
    for (int position : exactFirstLine(fresh)->pieceStarts()) {
        QTextCursor c(&doc); c.setPosition(position); fresh.setTextCursor(c);
    }
    QList<QTextCursor> freshCarets;
    for (int position : {450000, 650000}) {
        QTextCursor c(&doc); c.setPosition(position); freshCarets.append(c);
    }
    fresh.setCursors(freshCarets, 0);
    const auto positions = [](const QList<QTextCursor> &cursors) {
        QList<int> list;
        for (const QTextCursor &c : cursors) list.append(c.position());
        return list;
    };
    const auto original = positions(m_view->cursors());
    for (Qt::Key key : {Qt::Key_Down, Qt::Key_Down, Qt::Key_Up, Qt::Key_Up}) {
        QTest::keyClick(m_view, key);
        QTest::keyClick(&fresh, key);
        QCOMPARE(positions(m_view->cursors()), positions(fresh.cursors()));
    }
    QCOMPARE(positions(m_view->cursors()), original);
}

void TestTextView::longLineFindAfterZoom()
{
    setText(longLine(QStringLiteral("alpha beta words "), 800000) + QStringLiteral("unique-match"));
    QFont f = m_view->font(); f.setPointSizeF(16); m_view->setFont(f);
    selectRange(m_doc->characterCount() - 13, m_doc->characterCount() - 1);
    m_view->ensureCursorVisible();
    m_view->grab();
    QVERIFY(static_cast<ProbeView *>(m_view)->caretRectangle().intersects(m_view->viewport()->rect()));
}

void TestTextView::longLineEditsAfterResize()
{
    setText(longLine(QStringLiteral("alpha beta words "), 1000000));
    QTextCursor c(m_doc); c.setPosition(650000); m_view->setTextCursor(c);
    m_view->resize(470, 400);
    type(QStringLiteral("inserted "));
    compareWithFreshView(0);
}

void TestTextView::longLineWrapOffMeasuresOnlyShown()
{
    QTextDocument doc;
    doc.setPlainText(longLine(QStringLiteral("alpha beta words "), 1000000));
    vn::LineLayout layout;
    vn::LineLayout::Setup setup{m_view->font(), {}, false, 1e7, 20, 0};
    QVERIFY(layout.build(doc.firstBlock(), setup));
    setup.font.setPointSizeF(16);
    QVERIFY(layout.resetup(doc.firstBlock(), setup));
    const int before = layout.unmeasuredCount();
    layout.settleTo(600);
    QVERIFY(layout.unmeasuredCount() < before);
    QVERIFY(layout.unmeasuredCount() >= before - 2);
    vn::LineLayout fresh;
    QVERIFY(fresh.build(doc.firstBlock(), setup));
    while (fresh.settleOne(0)) {}
    for (int x = 0; x < 600; x += 7)
        QCOMPARE(layout.lineAt(0).xToCursor(x), fresh.lineAt(0).xToCursor(x));
    setText(doc.toPlainText());
    m_view->setWordWrap(false);
    m_view->setFont(setup.font);
    m_view->grab();
    compareWithFreshView(0);
    QTextCursor selection(m_doc);
    selection.setPosition(m_doc->characterCount() - 1);
    selection.setPosition(0, QTextCursor::KeepAnchor); // Keep the caret at the shown end.
    m_view->setTextCursor(selection);
    m_view->grab(); // Painting the clipped selection must not settle the next piece.
}

void TestTextView::longLineResizeIsFast()
{
    setText(longLine(QStringLiteral("alpha beta words "), 1000000));
    m_view->setWordWrap(true);
    m_view->grab();
    ThreadCpuTimer timer;
    for (int step = 0; step < 20; ++step) {
        m_view->resize(500 + step * 3, 400);
        m_view->grab();
    }
    QVERIFY2(timer.elapsed() < 20 * 15, qPrintable(QString::number(timer.elapsed())));
}


void TestTextView::longLineRowsPastEstimateAfterResetup()
{
    // Rows counted from estimates may be past what a piece really has once
    // it is measured: asking for them gives the piece's last row.
    QTextDocument doc;
    doc.setPlainText(longLine(QStringLiteral("alpha, WWW iii 12345 words "), 400000));
    vn::LineLayout::Setup setup{m_view->font(), {}, true, 430, 20, 0};
    setup.option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    vn::LineLayout layout;
    QVERIFY(layout.build(doc.firstBlock(), setup));
    while (layout.settleOne(0)) {}
    for (int width = 429; width > 200; width -= 3) {
        setup.wrapWidth = width;
        QVERIFY(layout.resetup(doc.firstBlock(), setup));
        const vn::LineLayout::Line last = layout.lineAt(layout.lineCount() - 1);
        QVERIFY(last.isValid());
        const int start = last.textStart();
        QVERIFY(start >= 0 && start < layout.length());
        QVERIFY(start + last.textLength() <= layout.length());
        QVERIFY(last.xToCursor(50) >= start);
        QVERIFY(last.cursorToX(start) >= 0);
        QVERIFY(last.naturalTextWidth() >= 0);
        QVERIFY(last.ascent() > 0);
    }

    // A piece the highlighter coloured again keeps its old rows until laid
    // out: a bold piece's rows, more than it has plain.
    QTextLayout::FormatRange bold;
    bold.start = 0;
    bold.length = 10000;
    bold.format.setFontWeight(QFont::Black);
    bold.format.setFontLetterSpacing(150);
    doc.firstBlock().layout()->setFormats({bold});
    setup.wrapWidth = 430;
    QVERIFY(layout.resetup(doc.firstBlock(), setup));
    while (layout.settleOne(0)) {}
    const int boldEnd = layout.lineForTextPosition(9999).lineNumber();
    doc.firstBlock().layout()->setFormats({});
    layout.formatsChanged(doc.firstBlock());
    QVERIFY(layout.unmeasuredCount() > 0);
    const vn::LineLayout::Line row = layout.lineAt(boldEnd);
    QVERIFY(row.isValid());
    QVERIFY(row.textStart() >= 0 && row.textStart() < layout.length());
}

void TestTextView::longLineBoxSelectAfterResize()
{
    setText(longLine(QStringLiteral("alpha, WWW iii 12345 words "), 1000000));
    m_view->setWordWrap(true);
    m_view->resize(600, 400);
    auto *probe = static_cast<ProbeView *>(m_view);
    constexpr int anchor = 650000;
    QTextCursor c(m_doc); c.setPosition(anchor); m_view->setTextCursor(c);
    m_view->grab();
    const Qt::KeyboardModifiers mods = Qt::ShiftModifier | Qt::AltModifier;
    boxSelect(QPoint(150, 200), QPoint(150, 200), mods);
    QCOMPARE(m_view->cursorCount(), 1);
    for (int step = 1; step <= 40; ++step) {
        m_view->resize(600 - step * 7, 400);
        m_view->grab();
        boxSelect(QPoint(150, 220), QPoint(150, 220), mods);
        // The box still starts on the wrapped row holding the anchor.
        const auto exact = exactFirstLine(*probe);
        QVERIFY(exact);
        const QList<QTextCursor> carets = m_view->cursors();
        QCOMPARE(carets.size(), 1);
        const vn::LineLayout::Line row = exact->lineForTextPosition(anchor);
        const int start = carets.first().anchor();
        QVERIFY2(start >= row.textStart() && start <= row.textStart() + row.textLength(),
                 qPrintable(QStringLiteral("step %1: %2 not in [%3, +%4]").arg(step).arg(start).arg(row.textStart()).arg(row.textLength())));
    }
}

void TestTextView::longLineHiddenZoomThenEditKeepsTop()
{
    // A second view on the document, zoomed while hidden, then the text is
    // edited in the first: shown again, it keeps its top text.
    // (Below a screen of short lines: nothing lays the long one out again
    // while the view is hidden.)
    QString shortLines;
    for (int i = 0; i < 100; ++i) shortLines += QStringLiteral("short line\n");
    setText(shortLines + longLine(QStringLiteral("alpha, WWW iii 12345 words "), 1000000));
    const int base = int(shortLines.size());
    m_view->setWordWrap(true);
    ProbeView other(m_doc);
    other.resize(500, 400);
    other.setWordWrap(true);
    other.show();
    QVERIFY(QTest::qWaitForWindowExposed(&other));
    QTextCursor c(m_doc); c.setPosition(base + 650000); other.setTextCursor(c);
    other.clearFocus();
    other.grab();
    const int before = other.positionAtPoint(QPointF(other.left(), 0));
    other.hide();
    QFont zoom = other.font(); zoom.setPointSizeF(15); other.setFont(zoom);
    // (After the top: an edit before it may cut its pieces again, and rows
    // start at the joins.)
    QTextCursor edit(m_doc); edit.setPosition(base + 900000); m_view->setTextCursor(edit);
    type(QStringLiteral("x"));
    other.show();
    QVERIFY(QTest::qWaitForWindowExposed(&other));
    other.grab();
    const int top = other.positionAtPoint(QPointF(other.left(), 0));
    const int next = other.positionAtPoint(QPointF(other.left(), other.rowHeight()));
    QVERIFY2(top <= before && before < next, qPrintable(QStringLiteral("%1 <= %2 < %3").arg(top).arg(before).arg(next)));
}

QTEST_MAIN(TestTextView)
#include "test_textview.moc"
