// Find/replace engine and the find bar.
#include "Settings.h"
#include "editor/DocumentLayout.h"
#include "editor/FindBar.h"
#include "editor/Search.h"
#include "editor/TextView.h"

#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QTextTable>
#include <QTimer>
#include <QToolButton>

#include <atomic>

using namespace vn;

class TestFind : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    void plainSearchIgnoresCaseByDefault();
    void caseAndWholeWord();
    void regexAndScope();
    void regexReplacementGroups();
    void preserveCase();
    void barSeedsFromSelectionAndCounts();
    void enterCyclesMatches();
    void replaceOneThenAll();
    void replaceAllIsOneUndoStep();
    void invalidRegexFlagsField();
    void findSelectionWithoutOpeningBar();
    void altEnterSelectsAllMatches();
    void findAllReportsRanges();
    void enterBeforeDebounceUsesTypedQuery();
    void replaceAllManyMatchesIsOneUndoStep();
    void textSnapshotSearchMatchesDocumentSearch();
    void bigDocumentsSearchOffTheGuiThread_data();
    void bigDocumentsSearchOffTheGuiThread();
    void staleResultsAreDropped();
    void closingCancelsTheSearch();
    void navigationWaitsForTheSearch();
    void replaceAllWaitsForTheSearch();
    void catastrophicRegexKeepsTheGuiResponsive();

private:
    // Sets the Aa, ab and .* toggles.
    void setOptions(bool matchCase, bool word, bool regex);
    void waitForSearch();
    QTextDocument *m_doc = nullptr;
    TextView *m_view = nullptr;
    FindBar *m_bar = nullptr;
    QWidget *m_host = nullptr;
};

void TestFind::init()
{
    m_doc = new QTextDocument;
    m_doc->setDocumentLayout(new DocumentLayout(m_doc));
    m_host = new QWidget;
    m_host->resize(700, 400);
    m_view = new TextView(m_doc, m_host);
    m_view->resize(700, 400);
    m_bar = new FindBar(m_view, m_host);
    m_host->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_host));
    m_host->activateWindow();
}

void TestFind::cleanup()
{
    delete m_host;
    delete m_doc;
}

void TestFind::plainSearchIgnoresCaseByDefault()
{
    m_doc->setPlainText(QStringLiteral("Foo foo FOO"));
    QCOMPARE(search::findAll(m_doc, {QStringLiteral("foo")}).size(), 3);
}

void TestFind::caseAndWholeWord()
{
    m_doc->setPlainText(QStringLiteral("cat Cat category cat"));
    search::Options o{QStringLiteral("cat"), true, false, false};
    QCOMPARE(search::findAll(m_doc, o).size(), 3);
    o.wholeWord = true;
    QCOMPARE(search::findAll(m_doc, o).size(), 2);
}

void TestFind::regexAndScope()
{
    m_doc->setPlainText(QStringLiteral("a1 b22 c333\nd4"));
    search::Options o{QStringLiteral("\\d+"), false, false, true};
    QCOMPARE(search::findAll(m_doc, o).size(), 4);
    // Only the first line.
    QCOMPARE(search::findAll(m_doc, o, 0, 11).size(), 3);
    // Empty matches are skipped, not looped on.
    o.pattern = QStringLiteral("^");
    QCOMPARE(search::findAll(m_doc, o).size(), 0);
}

void TestFind::regexReplacementGroups()
{
    m_doc->setPlainText(QStringLiteral("key = value"));
    search::Options o{QStringLiteral("(\\w+) = (\\w+)"), false, false, true};
    const auto matches = search::findAll(m_doc, o);
    QCOMPARE(matches.size(), 1);
    QCOMPARE(search::replacementFor(m_doc, o, matches.first(), QStringLiteral("$2: $1\\t$&"), false),
             QStringLiteral("value: key\tkey = value"));
}

void TestFind::preserveCase()
{
    m_doc->setPlainText(QStringLiteral("Hello HELLO hello"));
    const search::Options o{QStringLiteral("hello")};
    const auto m = search::findAll(m_doc, o);
    QCOMPARE(m.size(), 3);
    QCOMPARE(search::replacementFor(m_doc, o, m[0], QStringLiteral("world"), true), QStringLiteral("World"));
    QCOMPARE(search::replacementFor(m_doc, o, m[1], QStringLiteral("world"), true), QStringLiteral("WORLD"));
    QCOMPARE(search::replacementFor(m_doc, o, m[2], QStringLiteral("World"), true), QStringLiteral("world"));
}

void TestFind::barSeedsFromSelectionAndCounts()
{
    m_doc->setPlainText(QStringLiteral("alpha beta alpha gamma alpha"));
    QTextCursor c(m_doc);
    c.movePosition(QTextCursor::NextWord);
    c.movePosition(QTextCursor::EndOfWord, QTextCursor::KeepAnchor);
    m_view->setTextCursor(c); // "beta"
    m_bar->open(false);
    QCOMPARE(m_bar->query(), QStringLiteral("beta"));
    QCOMPARE(m_bar->matchCount(), 1);
    m_bar->findField()->clear();
    QTest::keyClicks(m_bar->findField(), QStringLiteral("alpha"));
    // Typing searches once it pauses, then selects the first match at or
    // after the original selection.
    QTRY_COMPARE(m_bar->matchCount(), 3);
    QTRY_COMPARE(m_view->textCursor().selectionStart(), 11);
}

void TestFind::enterCyclesMatches()
{
    m_doc->setPlainText(QStringLiteral("x y x y x"));
    m_view->setTextCursor(QTextCursor(m_doc));
    m_bar->open(false);
    QTest::keyClicks(m_bar->findField(), QStringLiteral("x"));
    QTRY_COMPARE(m_bar->currentMatch(), 0);
    QTest::keyClick(m_bar->findField(), Qt::Key_Return);
    QCOMPARE(m_bar->currentMatch(), 1);
    QTest::keyClick(m_bar->findField(), Qt::Key_Return);
    QTest::keyClick(m_bar->findField(), Qt::Key_Return);
    QCOMPARE(m_bar->currentMatch(), 0); // Wrapped.
    QTest::keyClick(m_bar->findField(), Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(m_bar->currentMatch(), 2);
}

void TestFind::replaceOneThenAll()
{
    m_doc->setPlainText(QStringLiteral("a-a-a"));
    m_view->setTextCursor(QTextCursor(m_doc));
    m_bar->open(true);
    m_bar->setQuery(QStringLiteral("a"));
    m_bar->replaceField()->setText(QStringLiteral("b"));
    m_bar->findNext(); // Select the first match.
    m_bar->replaceOne();
    QCOMPARE(m_doc->toPlainText(), QStringLiteral("b-a-a"));
    m_bar->replaceAll();
    QCOMPARE(m_doc->toPlainText(), QStringLiteral("b-b-b"));
    QCOMPARE(m_bar->matchCount(), 0);
}

void TestFind::replaceAllIsOneUndoStep()
{
    m_doc->setPlainText(QStringLiteral("1 2 3"));
    m_bar->open(true);
    m_bar->setQuery(QStringLiteral("\\d"));
    QTest::keyClick(m_bar->findField(), Qt::Key_R, Qt::AltModifier); // Regex on.
    m_bar->replaceField()->setText(QStringLiteral("<$0>"));
    m_bar->replaceAll();
    QCOMPARE(m_doc->toPlainText(), QStringLiteral("<1> <2> <3>"));
    m_view->undo();
    QCOMPARE(m_doc->toPlainText(), QStringLiteral("1 2 3"));
}

void TestFind::invalidRegexFlagsField()
{
    m_doc->setPlainText(QStringLiteral("text"));
    m_bar->open(false);
    QTest::keyClick(m_bar->findField(), Qt::Key_R, Qt::AltModifier);
    m_bar->setQuery(QStringLiteral("(unclosed"));
    QCOMPARE(m_bar->matchCount(), 0);
    QCOMPARE(m_bar->findField()->parentWidget()->property("error").toBool(), true);
}

void TestFind::findSelectionWithoutOpeningBar()
{
    m_doc->setPlainText(QStringLiteral("foo bar foo baz foo"));
    // The word under the caret.
    QTextCursor c(m_doc);
    c.setPosition(1);
    m_view->setTextCursor(c);
    m_bar->findSelection(true);
    QVERIFY(!m_bar->isVisible());
    QCOMPARE(m_bar->query(), QStringLiteral("foo"));
    QCOMPARE(m_view->textCursor().selectionStart(), 8);
    QCOMPARE(m_view->textCursor().selectedText(), QStringLiteral("foo"));
    // The selection is now the search text.
    m_bar->findSelection(true);
    QCOMPARE(m_view->textCursor().selectionStart(), 16);
    m_bar->findSelection(true); // Wraps.
    QCOMPARE(m_view->textCursor().selectionStart(), 0);
    m_bar->findSelection(false);
    QCOMPARE(m_view->textCursor().selectionStart(), 16);
    QVERIFY(!m_bar->isVisible());

    // A selected phrase replaces the old query.
    c.setPosition(4);
    c.setPosition(7, QTextCursor::KeepAnchor);
    m_view->setTextCursor(c); // "bar"
    m_bar->findSelection(true);
    QCOMPARE(m_bar->query(), QStringLiteral("bar"));
    QCOMPARE(m_view->textCursor().selectionStart(), 4);

    // With the bar open it keeps showing the results.
    m_bar->open(false);
    c.setPosition(0);
    c.setPosition(3, QTextCursor::KeepAnchor);
    m_view->setTextCursor(c);
    m_bar->findSelection(true);
    QVERIFY(m_bar->isVisible());
    QCOMPARE(m_bar->matchCount(), 3);
    QCOMPARE(m_view->textCursor().selectionStart(), 8);
}

void TestFind::altEnterSelectsAllMatches()
{
    m_doc->setPlainText(QStringLiteral("x y x y x"));
    m_view->setTextCursor(QTextCursor(m_doc));
    m_bar->open(false);
    QTest::keyClicks(m_bar->findField(), QStringLiteral("x"));
    QTest::keyClick(m_bar->findField(), Qt::Key_Return, Qt::AltModifier);
    QCOMPARE(m_view->cursorCount(), 3);
    QCOMPARE(m_view->selectedText(), QStringLiteral("x\nx\nx"));
    QVERIFY(m_view->hasFocus());
    QVERIFY(m_bar->isVisible());
    // Typing now replaces every match.
    QTest::keyClicks(m_view, QStringLiteral("z"));
    QCOMPARE(m_doc->toPlainText(), QStringLiteral("z y z y z"));
}

void TestFind::findAllReportsRanges()
{
    using R = search::Range;
    m_doc->setPlainText(QStringLiteral("ab ab\n\nab_ab ab\nxab"));
    QCOMPARE(search::findAll(m_doc, {QStringLiteral("ab")}), (QList<R>{{0, 2}, {3, 5}, {7, 9}, {10, 12}, {13, 15}, {17, 19}}));
    search::Options o{QStringLiteral("ab"), false, true, false};
    // An underscore is not part of a word for QTextDocument.
    QCOMPARE(search::findAll(m_doc, o), (QList<R>{{0, 2}, {3, 5}, {7, 9}, {10, 12}, {13, 15}}));
    // Scope: a match must end inside it.
    QCOMPARE(search::findAll(m_doc, search::Options{QStringLiteral("ab")}, 1, 12), (QList<R>{{3, 5}, {7, 9}, {10, 12}}));
    // Regex: empty matches are skipped and later lines still match.
    o = {QStringLiteral("b*"), false, false, true};
    QCOMPARE(search::findAll(m_doc, o), (QList<R>{{1, 2}, {4, 5}, {8, 9}, {11, 12}, {14, 15}, {18, 19}}));
    // Same matches as QTextDocument::find, which this replaced.
    m_doc->setPlainText(QStringLiteral("Foo foo\u00a0FOO fooo\n\nfoo-foo (foo) f\no"));
    for (const bool word : {false, true}) {
        for (const bool matchCase : {false, true}) {
            for (const QString &pattern : {QStringLiteral("foo"), QStringLiteral("foo ")}) {
                QList<R> expected;
                QTextDocument::FindFlags flags;
                if (word)
                    flags |= QTextDocument::FindWholeWords;
                if (matchCase)
                    flags |= QTextDocument::FindCaseSensitively;
                for (QTextCursor c = m_doc->find(pattern, 0, flags); !c.isNull();
                     c = m_doc->find(pattern, c.selectionEnd(), flags))
                    expected.append({c.selectionStart(), c.selectionEnd()});
                QCOMPARE(search::findAll(m_doc, search::Options{pattern, matchCase, word, false}), expected);
            }
        }
    }
    for (const bool word : {false, true}) {
        for (const QString &pattern : {QStringLiteral("o+"), QStringLiteral("fo*"), QStringLiteral("\\bfoo\\b|^"),
                                       QStringLiteral("f?$")}) {
            QList<R> expected;
            QTextDocument::FindFlags flags = word ? QTextDocument::FindWholeWords : QTextDocument::FindFlags();
            const QRegularExpression re(pattern, QRegularExpression::CaseInsensitiveOption
                                                     | QRegularExpression::UseUnicodePropertiesOption);
            int from = 0;
            while (true) {
                const QTextCursor c = m_doc->find(re, from, flags);
                if (c.isNull())
                    break;
                if (c.hasSelection()) {
                    expected.append({c.selectionStart(), c.selectionEnd()});
                    from = c.selectionEnd();
                } else {
                    from = c.position() + 1;
                }
            }
            QCOMPARE(search::findAll(m_doc, search::Options{pattern, false, word, true}), expected);
        }
    }
    // Matches are capped.
    m_doc->setPlainText(QString(search::maxMatches + 50, QLatin1Char('a')));
    QCOMPARE(search::findAll(m_doc, {QStringLiteral("a")}).size(), search::maxMatches);
}

void TestFind::enterBeforeDebounceUsesTypedQuery()
{
    m_doc->setPlainText(QStringLiteral("x y x y x"));
    m_view->setTextCursor(QTextCursor(m_doc));
    m_bar->open(false);
    QTest::keyClicks(m_bar->findField(), QStringLiteral("y"));
    // No wait: Enter must search first, then step past the first match.
    QTest::keyClick(m_bar->findField(), Qt::Key_Return);
    QCOMPARE(m_bar->matchCount(), 2);
    QCOMPARE(m_view->textCursor().selectionStart(), 6);
    QCOMPARE(m_bar->currentMatch(), 1);
}

void TestFind::replaceAllManyMatchesIsOneUndoStep()
{
    QStringList lines;
    for (int i = 0; i < 3000; ++i)
        lines << QStringLiteral("k%1=v%1 k").arg(i);
    const QString original = lines.join(u'\n');
    m_doc->setPlainText(original);
    m_bar->open(true);
    QTest::keyClick(m_bar->findField(), Qt::Key_R, Qt::AltModifier); // Regex on.
    m_bar->setQuery(QStringLiteral("k(\\d*)"));
    m_bar->replaceField()->setText(QStringLiteral("[$1]"));
    m_bar->replaceAll();
    QVERIFY(m_doc->toPlainText().startsWith(QStringLiteral("[0]=v0 []\n[1]=v1 []")));
    QVERIFY(m_doc->toPlainText().endsWith(QStringLiteral("[2999]=v2999 []")));
    QCOMPARE(m_bar->matchCount(), 0);
    m_view->undo();
    QCOMPARE(m_doc->toPlainText(), original);
}

namespace {

using R = search::Range;

// About `chars` characters of lines in the shapes the search cares about:
// repeated words, empty lines, tabs, no-break spaces, one long line.
QString makeText(int chars)
{
    QString text;
    text.reserve(chars + 200);
    int n = 0;
    while (text.size() < chars) {
        switch (n % 9) {
        case 0:
            text += QStringLiteral("## Section %1: notes on topic %1\n").arg(n);
            break;
        case 1:
            text += QStringLiteral("The cat and the Hat, the end of the line %1\n").arg(n);
            break;
        case 2:
            text += QLatin1Char('\n');
            break;
        case 3:
            text += QStringLiteral("\tindented section %1 with a category and cat nbsp\n").arg(n);
            break;
        case 4:
            text += QStringLiteral("foo-bar foo_bar FOO bar(baz) k%1=v%1 end\n").arg(n);
            break;
        case 5:
            text += QStringLiteral("été Café café 中文 %1 end\n").arg(n);
            break;
        case 6:
            if (n % 450 == 6)
                text += QString(3000, QLatin1Char('x')) + QStringLiteral(" the end\n");
            else
                text += QStringLiteral("plain words only %1\n").arg(n);
            break;
        case 7:
            text += QStringLiteral("a line with a separator the %1\n").arg(n);
            break;
        default:
            text += QStringLiteral("last line of the group %1\n").arg(n);
            break;
        }
        ++n;
    }
    return text;
}

struct Combo
{
    const char *pattern;
    bool matchCase;
    bool word;
    bool regex;
};

const Combo combos[] = {
    {"section", false, false, false},
    {"Section", true, false, false},
    {"the", false, true, false},
    {"cat", true, true, false},
    {"zzzz-not-there", false, false, false},
    {"café", false, false, false},
    {"section \\d+", false, false, true},
    {"\\bthe\\b", false, false, true},
    {"^#+ ", false, false, true},
    {"end$", false, false, true},
    {"(foo|bar)+", false, true, true},
    {"line\\n", false, false, true},
    {"x{5,}", false, false, true},
    {"t\\w*", true, true, true},
    {"b*", false, false, true},
};

} // namespace

void TestFind::setOptions(bool matchCase, bool word, bool regex)
{
    for (QToolButton *button : m_bar->findChildren<QToolButton *>(QStringLiteral("FindOption"))) {
        if (button->text() == QLatin1String("Aa"))
            button->setChecked(matchCase);
        else if (button->text() == QLatin1String("ab"))
            button->setChecked(word);
        else if (button->text() == QLatin1String(".*"))
            button->setChecked(regex);
    }
}

void TestFind::waitForSearch()
{
    QTRY_VERIFY_WITH_TIMEOUT(!m_bar->isSearching(), 30000);
}

void TestFind::textSnapshotSearchMatchesDocumentSearch()
{
    // A document with a table: its cells and frame boundaries are blocks too.
    m_doc->setPlainText(makeText(20000));
    QTextCursor cursor(m_doc);
    cursor.setPosition(500);
    QTextTable *table = cursor.insertTable(3, 3);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column)
            table->cellAt(row, column).firstCursorPosition().insertText(QStringLiteral("the cell %1 %2").arg(row).arg(column));
    }
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("\nlast the"));
    const QString raw = m_doc->toRawText();
    QCOMPARE(raw.size(), m_doc->characterCount() - 1);
    for (const Combo &c : combos) {
        const search::Options options{QString::fromUtf8(c.pattern), c.matchCase, c.word, c.regex};
        const search::Compiled compiled(options);
        for (const QList<int> &scope :
             {QList<int>{-1, -1}, QList<int>{0, 700}, QList<int>{650, 9000}, QList<int>{19000, -1}}) {
            const QList<R> expected = search::findAll(m_doc, compiled, scope[0], scope[1]);
            QVERIFY2(search::findAllInText(raw, compiled, scope[0], scope[1]) == expected, c.pattern);
        }
    }
    // A set flag stops the scan.
    std::atomic<bool> cancel{true};
    QVERIFY(search::findAllInText(raw, search::Compiled(search::Options{QStringLiteral("the")}), -1, -1, &cancel)
                .isEmpty());
}

void TestFind::bigDocumentsSearchOffTheGuiThread_data()
{
    QTest::addColumn<int>("chars");
    QTest::newRow("1 MB") << 1000000;
    QTest::newRow("10 MB") << 10000000;
}

void TestFind::bigDocumentsSearchOffTheGuiThread()
{
    QFETCH(int, chars);
    m_doc->setPlainText(makeText(chars));
    for (const Combo &c : combos) {
        m_bar->close();
        m_view->setTextCursor(QTextCursor(m_doc));
        m_bar->open(false);
        waitForSearch();
        setOptions(c.matchCase, c.word, c.regex);
        waitForSearch();
        const search::Options options{QString::fromUtf8(c.pattern), c.matchCase, c.word, c.regex};
        const QList<R> expected = search::findAll(m_doc, options);
        m_bar->setQuery(options.pattern);
        // The search runs on a worker; the GUI thread is free meanwhile.
        QVERIFY2(m_bar->isSearching(), c.pattern);
        waitForSearch();
        QVERIFY2(m_bar->matchCount() == expected.size(), c.pattern);
        // The current match is the first at or after the caret.
        QCOMPARE(m_bar->currentMatch(), expected.isEmpty() ? -1 : 0);
        if (expected.size() > 1) {
            m_bar->findNext();
            QVERIFY(!m_bar->isSearching());
            QCOMPARE(m_view->textCursor().selectionStart(), expected[0].start);
            QCOMPARE(m_view->textCursor().selectionEnd(), expected[0].end);
            m_bar->findNext();
            QCOMPARE(m_view->textCursor().selectionStart(), expected[1].start);
            m_bar->findPrevious();
            m_bar->findPrevious(); // Wraps to the last.
            QCOMPARE(m_view->textCursor().selectionStart(), expected.last().start);
        }
    }
}

void TestFind::staleResultsAreDropped()
{
    const QString text = makeText(1000000);
    m_doc->setPlainText(text);
    m_view->setTextCursor(QTextCursor(m_doc));
    m_bar->open(false);
    const search::Options section{QStringLiteral("section")};
    const QList<R> sections = search::findAll(m_doc, section);
    QVERIFY(!sections.isEmpty());

    // A newer query replaces a search in flight.
    m_bar->setQuery(QStringLiteral("cat"));
    QVERIFY(m_bar->isSearching());
    m_bar->setQuery(QStringLiteral("section"));
    waitForSearch();
    QCOMPARE(m_bar->matchCount(), int(sections.size()));

    // So does typing: only the query that was last typed is searched for.
    m_bar->findField()->clear();
    QTest::keyClicks(m_bar->findField(), QStringLiteral("cat"));
    QTest::qWait(150); // The debounce fires and the search starts.
    QTest::keyClicks(m_bar->findField(), QStringLiteral("egory"));
    QTest::qWait(150);
    m_bar->findField()->clear();
    QTest::keyClicks(m_bar->findField(), QStringLiteral("section"));
    QTRY_VERIFY_WITH_TIMEOUT(!m_bar->isSearching() && m_bar->matchCount() == sections.size(), 30000);
    QTest::qWait(300); // Nothing late replaces it.
    QCOMPARE(m_bar->matchCount(), int(sections.size()));

    // An edit makes the search of the old text useless: its matches are never
    // shown, even though its worker may finish later.
    m_bar->setQuery(QStringLiteral("section"));
    QVERIFY(m_bar->isSearching());
    m_doc->setPlainText(QString(text).replace(QLatin1String("section"), QLatin1String("sektion"), Qt::CaseInsensitive));
    QVERIFY(!m_bar->isSearching());
    QCOMPARE(m_bar->matchCount(), int(sections.size())); // The old ones stay until the new search ends.
    QTRY_COMPARE_WITH_TIMEOUT(m_bar->matchCount(), 0, 30000);
    QTest::qWait(300);
    QCOMPARE(m_bar->matchCount(), 0);

    // An edit that moves the matches.
    m_doc->setPlainText(text);
    QTRY_COMPARE_WITH_TIMEOUT(m_bar->matchCount(), int(sections.size()), 30000);
    QTextCursor edit(m_doc);
    edit.insertText(QStringLiteral("section section\n"));
    QTRY_COMPARE_WITH_TIMEOUT(m_bar->matchCount(), int(sections.size()) + 2, 30000);
    waitForSearch();
    QCOMPARE(m_bar->matchCount(), int(search::findAll(m_doc, section).size()));
}

void TestFind::closingCancelsTheSearch()
{
    m_doc->setPlainText(makeText(1000000));
    QTextCursor caret(m_doc);
    caret.setPosition(10);
    m_view->setTextCursor(caret);
    m_bar->open(false);
    m_bar->setQuery(QStringLiteral("section"));
    QVERIFY(m_bar->isSearching());
    // Navigation waiting for the search goes with it.
    m_bar->findNext();
    m_bar->close();
    QVERIFY(!m_bar->isSearching());
    QTest::qWait(500);
    QCOMPARE(m_bar->matchCount(), 0);
    QCOMPARE(m_view->textCursor().position(), 10);
    QVERIFY(!m_view->textCursor().hasSelection());

    // The bar searches again when it is reopened.
    m_bar->open(false);
    waitForSearch();
    QVERIFY(m_bar->matchCount() > 0);

    // Deleting the bar with a search in flight is harmless.
    m_bar->setQuery(QStringLiteral("the"));
    QVERIFY(m_bar->isSearching());
    delete m_bar;
    m_bar = nullptr;
    QTest::qWait(500);
}

void TestFind::navigationWaitsForTheSearch()
{
    m_doc->setPlainText(makeText(1000000));
    m_view->setTextCursor(QTextCursor(m_doc));
    m_bar->open(false);
    // Enter right after typing, as in enterBeforeDebounceUsesTypedQuery: the
    // search selects the first match and Enter then steps to the second.
    QTest::keyClicks(m_bar->findField(), QStringLiteral("cat"));
    QTest::keyClick(m_bar->findField(), Qt::Key_Return);
    QVERIFY(m_bar->isSearching());
    QVERIFY(!m_view->textCursor().hasSelection()); // Waiting, not blocking.
    waitForSearch();
    const QList<R> cats = search::findAll(m_doc, search::Options{QStringLiteral("cat")});
    QCOMPARE(m_bar->matchCount(), int(cats.size()));
    QCOMPARE(m_view->textCursor().selectionStart(), cats[1].start);
    QCOMPARE(m_bar->currentMatch(), 1);

    // Several steps queued behind one search all happen, in order.
    setOptions(false, true, false);
    waitForSearch();
    const QList<R> words = search::findAll(m_doc, search::Options{QStringLiteral("the"), false, true, false});
    m_view->setTextCursor(QTextCursor(m_doc));
    m_bar->close();
    m_bar->open(false);
    m_bar->setQuery(QStringLiteral("the"));
    QVERIFY(m_bar->isSearching());
    m_bar->findNext();
    m_bar->findNext();
    m_bar->findNext();
    m_bar->findPrevious();
    QVERIFY(!m_view->textCursor().hasSelection());
    waitForSearch();
    QCOMPARE(m_view->textCursor().selectionStart(), words[1].start);

    // Replace waits too, then moves on to the next match.
    setOptions(false, false, false);
    waitForSearch();
    m_bar->close();
    m_view->setTextCursor(QTextCursor(m_doc));
    m_bar->open(true);
    m_bar->setQuery(QStringLiteral("cat"));
    m_bar->replaceField()->setText(QStringLiteral("dog"));
    waitForSearch();
    m_bar->findNext(); // Selects the first.
    QCOMPARE(m_view->textCursor().selectionStart(), cats[0].start);
    m_bar->setQuery(QStringLiteral("cat")); // A search in flight again.
    QVERIFY(m_bar->isSearching());
    m_bar->replaceOne();
    waitForSearch();
    const QString after = m_doc->toPlainText();
    QCOMPARE(after.count(QLatin1String("dog")), 1);
    QCOMPARE(after.count(QLatin1String("cat"), Qt::CaseInsensitive), int(cats.size()) - 1);
    QCOMPARE(m_view->textCursor().selectionStart(), cats[1].start);
}

void TestFind::replaceAllWaitsForTheSearch()
{
    const QString original = makeText(1000000).replace(QChar(0x00a0), QLatin1Char(' ')).replace(QChar(0x2028), QLatin1Char(' '));
    m_doc->setPlainText(original);
    m_bar->open(true);
    m_bar->replaceField()->setText(QStringLiteral("<S>"));
    m_bar->setQuery(QStringLiteral("section"));
    QVERIFY(m_bar->isSearching());
    m_bar->replaceAll();
    QVERIFY(m_doc->toPlainText() == original); // Not yet.
    waitForSearch();
    const QString expected = QString(original).replace(QLatin1String("section"), QLatin1String("<S>"), Qt::CaseInsensitive);
    QVERIFY(m_doc->toPlainText() == expected);
    m_view->undo();
    QVERIFY(m_doc->toPlainText() == original);

    // Regex groups, with the replacement computed from the original text.
    setOptions(false, false, true);
    waitForSearch();
    m_bar->setQuery(QStringLiteral("(k)(\\d+)=v(\\d+)"));
    m_bar->replaceField()->setText(QStringLiteral("$2:$1:$3"));
    m_bar->replaceAll();
    waitForSearch();
    const QString regexExpected = QString(original).replace(QRegularExpression(QStringLiteral("(k)(\\d+)=v(\\d+)")), QStringLiteral("\\2:\\1:\\3"));
    QVERIFY(m_doc->toPlainText() == regexExpected);
}

void TestFind::catastrophicRegexKeepsTheGuiResponsive()
{
    // (a+)+$ backtracks exponentially on a line of a's ending in something
    // else. PCRE gives up on each such line after ten million steps (about 30
    // ms), so many of them make a search of seconds.
    QString text = makeText(300000);
    for (int i = 0; i < 200; ++i)
        text += QString(30, QLatin1Char('a')) + QStringLiteral("!\n");
    m_doc->setPlainText(text);
    m_view->setTextCursor(QTextCursor(m_doc));
    m_bar->open(false);
    setOptions(false, false, true);
    waitForSearch();

    qint64 longestGap = 0;
    int ticks = 0;
    QElapsedTimer sinceTick;
    sinceTick.start();
    QTimer ticker;
    ticker.setInterval(5);
    connect(&ticker, &QTimer::timeout, this, [&] {
        longestGap = qMax(longestGap, sinceTick.restart());
        ++ticks;
    });
    ticker.start();

    QElapsedTimer total;
    total.start();
    m_bar->setQuery(QStringLiteral("(a+)+$"));
    QVERIFY(m_bar->isSearching());
    QVERIFY(total.elapsed() < 100); // Starting it does not wait for it.
    const QLabel *count = m_bar->findChild<QLabel *>(QStringLiteral("FindCount"));
    bool sawBusy = false;
    while (m_bar->isSearching() && total.elapsed() < 60000) {
        QTest::qWait(10);
        sawBusy = sawBusy || count->text() == QStringLiteral("…");
    }
    ticker.stop();
    QVERIFY(!m_bar->isSearching());
    // The search took long, the GUI thread went on, and the counter said so.
    QVERIFY2(total.elapsed() > 500, qPrintable(QString::number(total.elapsed())));
    QVERIFY(ticks > total.elapsed() / 50);
    QVERIFY2(longestGap < 150, qPrintable(QString::number(longestGap)));
    QVERIFY(sawBusy);

    // A new query does not wait for the slow one: it is cancelled between lines.
    total.restart();
    m_bar->setQuery(QStringLiteral("(a+)+$"));
    QVERIFY(m_bar->isSearching());
    m_bar->setQuery(QStringLiteral("section"));
    waitForSearch();
    QVERIFY(total.elapsed() < 1500);
    QVERIFY(m_bar->matchCount() > 0);
    QVERIFY(count->text() != QStringLiteral("…"));
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestFind)
#include "test_find.moc"
