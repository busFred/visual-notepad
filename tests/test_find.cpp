// Find/replace engine and the find bar.
#include "editor/DocumentLayout.h"
#include "editor/FindBar.h"
#include "editor/Search.h"
#include "editor/TextView.h"

#include <QLineEdit>
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
    QCOMPARE(search::replacementFor(o, matches.first(), QStringLiteral("$2: $1\\t$&"), false),
             QStringLiteral("value: key\tkey = value"));
}

void TestFind::preserveCase()
{
    m_doc->setPlainText(QStringLiteral("Hello HELLO hello"));
    const search::Options o{QStringLiteral("hello")};
    const auto m = search::findAll(m_doc, o);
    QCOMPARE(m.size(), 3);
    QCOMPARE(search::replacementFor(o, m[0], QStringLiteral("world"), true), QStringLiteral("World"));
    QCOMPARE(search::replacementFor(o, m[1], QStringLiteral("world"), true), QStringLiteral("WORLD"));
    QCOMPARE(search::replacementFor(o, m[2], QStringLiteral("World"), true), QStringLiteral("world"));
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
    QCOMPARE(m_bar->matchCount(), 3);
    // Typing selects the first match at or after the original selection.
    QCOMPARE(m_view->textCursor().selectionStart(), 11);
}

void TestFind::enterCyclesMatches()
{
    m_doc->setPlainText(QStringLiteral("x y x y x"));
    m_view->setTextCursor(QTextCursor(m_doc));
    m_bar->open(false);
    QTest::keyClicks(m_bar->findField(), QStringLiteral("x"));
    QCOMPARE(m_bar->currentMatch(), 0);
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

QTEST_MAIN(TestFind)
#include "test_find.moc"
