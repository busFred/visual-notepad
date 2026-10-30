// Find/replace engine and the find bar.
#include "Settings.h"
#include "editor/DocumentLayout.h"
#include "editor/FindBar.h"
#include "editor/Search.h"
#include "editor/TextView.h"

#include <QLineEdit>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>

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

private:
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

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestFind)
#include "test_find.moc"
