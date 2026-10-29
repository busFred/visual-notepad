// Markdown formatting commands: inline toggles, headings, line prefixes, link
// and code block, their shortcuts and enabling.
#include "Commands.h"
#include "DocumentTab.h"
#include "MainWindow.h"
#include "Settings.h"
#include "Theme.h"
#include "editor/FindBar.h"
#include "editor/TextView.h"
#include "markdown/VisualView.h"

#include <QAction>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QToolButton>

using namespace vn;
using F = TextView::MarkdownFormat;

namespace {

// MainWindow::action() is private; the window owns every action by command text.
QAction *actionOf(MainWindow &window, const QString &id)
{
    const Command *cmd = command(id);
    for (QAction *a : window.actions()) {
        if (a->text() == cmd->text)
            return a;
    }
    return nullptr;
}

} // namespace

class TestFormatting : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanup();

    void boldWrapsSelection();
    void boldUnwrapsOutside();
    void boldUnwrapsInside();
    void wordUnderCaret();
    void emptyInsertAndRemove();
    void boldItalicDisambiguation();
    void otherInlineMarkers();
    void multipleCarets();
    void headingSetReplaceToggle();
    void headingUpDown();
    void headingsOverLines();
    void quoteToggle();
    void bulletToggle();
    void orderedToggle();
    void taskToggle();
    void linkWrapsSelection();
    void linkWithoutSelection();
    void codeBlockWrapsLines();
    void oneUndoStep();
    void readOnlyIsNoOp();
    void visualViewFormats();
    void disabledForPlainText();
    void shortcutsAreBound();
    void defaultShortcutsDoNotCollide();
    void findBarKeepsAltC();

private:
    DocumentTab *makeTab(const QString &text);
    static QString selected(TextView *view) { return view->textCursor().selectedText(); }
    static void select(TextView *view, int from, int to);

    QList<DocumentTab *> m_tabs;
    QTemporaryDir m_dir;
};

void TestFormatting::initTestCase()
{
    QVERIFY(m_dir.isValid());
    // Defaults, whatever the user's own settings say.
    Settings::instance().useFile(m_dir.filePath(QStringLiteral("settings.ini")));
    theme::apply(*qApp);
}

void TestFormatting::cleanup()
{
    qDeleteAll(m_tabs);
    m_tabs.clear();
}

DocumentTab *TestFormatting::makeTab(const QString &text)
{
    auto *tab = new DocumentTab;
    m_tabs.append(tab);
    tab->resize(700, 400);
    tab->setViewMode(DocumentTab::ViewMode::Source);
    tab->document()->setPlainText(text);
    tab->show();
    return tab;
}

void TestFormatting::select(TextView *view, int from, int to)
{
    QTextCursor c(view->document());
    c.setPosition(from);
    c.setPosition(to, QTextCursor::KeepAnchor);
    view->setTextCursor(c);
}

void TestFormatting::boldWrapsSelection()
{
    DocumentTab *tab = makeTab(QStringLiteral("hello world"));
    TextView *v = tab->editor();
    select(v, 0, 5);
    v->formatMarkdown(F::Bold);
    QCOMPARE(tab->text(), QStringLiteral("**hello** world"));
    QCOMPARE(selected(v), QStringLiteral("hello"));
}

void TestFormatting::boldUnwrapsOutside()
{
    DocumentTab *tab = makeTab(QStringLiteral("**hello** world"));
    TextView *v = tab->editor();
    select(v, 2, 7);
    v->formatMarkdown(F::Bold);
    QCOMPARE(tab->text(), QStringLiteral("hello world"));
    QCOMPARE(selected(v), QStringLiteral("hello"));
}

void TestFormatting::boldUnwrapsInside()
{
    DocumentTab *tab = makeTab(QStringLiteral("**hello** world"));
    TextView *v = tab->editor();
    select(v, 0, 9);
    v->formatMarkdown(F::Bold);
    QCOMPARE(tab->text(), QStringLiteral("hello world"));
    QCOMPARE(selected(v), QStringLiteral("hello"));
}

void TestFormatting::wordUnderCaret()
{
    DocumentTab *tab = makeTab(QStringLiteral("say hello now"));
    TextView *v = tab->editor();
    select(v, 6, 6);
    v->formatMarkdown(F::Bold);
    QCOMPARE(tab->text(), QStringLiteral("say **hello** now"));
    QCOMPARE(v->textCursor().position(), 8);
    QVERIFY(!v->textCursor().hasSelection());
    v->formatMarkdown(F::Bold);
    QCOMPARE(tab->text(), QStringLiteral("say hello now"));
    QCOMPARE(v->textCursor().position(), 6);
    // At the end of a word still counts.
    select(v, 9, 9);
    v->formatMarkdown(F::Italic);
    QCOMPARE(tab->text(), QStringLiteral("say *hello* now"));
}

void TestFormatting::emptyInsertAndRemove()
{
    DocumentTab *tab = makeTab(QStringLiteral("a  b"));
    TextView *v = tab->editor();
    select(v, 2, 2);
    v->formatMarkdown(F::Bold);
    QCOMPARE(tab->text(), QStringLiteral("a **** b"));
    QCOMPARE(v->textCursor().position(), 4);
    v->formatMarkdown(F::Bold);
    QCOMPARE(tab->text(), QStringLiteral("a  b"));
    QCOMPARE(v->textCursor().position(), 2);
}

void TestFormatting::boldItalicDisambiguation()
{
    DocumentTab *tab = makeTab(QStringLiteral("x"));
    TextView *v = tab->editor();
    select(v, 0, 1);
    v->formatMarkdown(F::Italic);
    QCOMPARE(tab->text(), QStringLiteral("*x*"));
    v->formatMarkdown(F::Bold);
    QCOMPARE(tab->text(), QStringLiteral("***x***"));
    QCOMPARE(selected(v), QStringLiteral("x"));
    v->formatMarkdown(F::Italic);
    QCOMPARE(tab->text(), QStringLiteral("**x**"));
    v->formatMarkdown(F::Italic);
    QCOMPARE(tab->text(), QStringLiteral("***x***"));
    v->formatMarkdown(F::Bold);
    QCOMPARE(tab->text(), QStringLiteral("*x*"));
    v->formatMarkdown(F::Italic);
    QCOMPARE(tab->text(), QStringLiteral("x"));

    // Selecting the markers too.
    tab->document()->setPlainText(QStringLiteral("**x**"));
    select(v, 0, 5);
    v->formatMarkdown(F::Italic);
    QCOMPARE(tab->text(), QStringLiteral("***x***"));
    select(v, 0, 7);
    v->formatMarkdown(F::Bold);
    QCOMPARE(tab->text(), QStringLiteral("*x*"));
}

void TestFormatting::otherInlineMarkers()
{
    const struct
    {
        F format;
        const char *wrapped;
    } cases[] = {{F::Underline, "<u>x</u>"},
                 {F::Strikethrough, "~~x~~"},
                 {F::InlineCode, "`x`"}};
    for (const auto &c : cases) {
        DocumentTab *tab = makeTab(QStringLiteral("x"));
        TextView *v = tab->editor();
        select(v, 0, 1);
        v->formatMarkdown(c.format);
        QCOMPARE(tab->text(), QString::fromLatin1(c.wrapped));
        QCOMPARE(selected(v), QStringLiteral("x"));
        v->formatMarkdown(c.format);
        QCOMPARE(tab->text(), QStringLiteral("x"));
        QCOMPARE(selected(v), QStringLiteral("x"));
    }
}

void TestFormatting::multipleCarets()
{
    DocumentTab *tab = makeTab(QStringLiteral("aa bb\ncc"));
    TextView *v = tab->editor();
    QTextCursor c1(v->document());
    c1.setPosition(1);
    QTextCursor c2(v->document());
    c2.setPosition(4);
    QTextCursor c3(v->document());
    c3.setPosition(6);
    c3.setPosition(8, QTextCursor::KeepAnchor);
    v->setCursors({c1, c2, c3}, 0);
    v->formatMarkdown(F::Bold);
    QCOMPARE(tab->text(), QStringLiteral("**aa** **bb**\n**cc**"));
    QCOMPARE(v->cursorCount(), 3);
    v->formatMarkdown(F::Bold);
    QCOMPARE(tab->text(), QStringLiteral("aa bb\ncc"));
    v->undo();
    QCOMPARE(tab->text(), QStringLiteral("**aa** **bb**\n**cc**"));
    QCOMPARE(v->cursorCount(), 3);
}

void TestFormatting::headingSetReplaceToggle()
{
    DocumentTab *tab = makeTab(QStringLiteral("Title"));
    TextView *v = tab->editor();
    v->formatMarkdown(F::Heading2);
    QCOMPARE(tab->text(), QStringLiteral("## Title"));
    v->formatMarkdown(F::Heading1);
    QCOMPARE(tab->text(), QStringLiteral("# Title"));
    v->formatMarkdown(F::Heading1);
    QCOMPARE(tab->text(), QStringLiteral("Title"));
    v->formatMarkdown(F::Heading6);
    QCOMPARE(tab->text(), QStringLiteral("###### Title"));
}

void TestFormatting::headingUpDown()
{
    DocumentTab *tab = makeTab(QStringLiteral("# T"));
    TextView *v = tab->editor();
    v->formatMarkdown(F::HeadingDown);
    QCOMPARE(tab->text(), QStringLiteral("## T"));
    v->formatMarkdown(F::HeadingUp);
    QCOMPARE(tab->text(), QStringLiteral("# T"));
    v->formatMarkdown(F::HeadingUp); // H1 stays.
    QCOMPARE(tab->text(), QStringLiteral("# T"));
    tab->document()->setPlainText(QStringLiteral("###### T"));
    v->formatMarkdown(F::HeadingDown); // Past H6 is plain text.
    QCOMPARE(tab->text(), QStringLiteral("T"));
    v->formatMarkdown(F::HeadingUp); // Plain text becomes H6.
    QCOMPARE(tab->text(), QStringLiteral("###### T"));
}

void TestFormatting::headingsOverLines()
{
    DocumentTab *tab = makeTab(QStringLiteral("a\n\n## b\nc"));
    TextView *v = tab->editor();
    v->selectAll();
    v->formatMarkdown(F::Heading2);
    // Not all lines were H2: all become H2, blank lines are skipped.
    QCOMPARE(tab->text(), QStringLiteral("## a\n\n## b\n## c"));
    v->formatMarkdown(F::Heading2);
    QCOMPARE(tab->text(), QStringLiteral("a\n\nb\nc"));
}

void TestFormatting::quoteToggle()
{
    DocumentTab *tab = makeTab(QStringLiteral("a\nb\nc"));
    TextView *v = tab->editor();
    select(v, 0, 3); // Lines a and b.
    v->formatMarkdown(F::Quote);
    QCOMPARE(tab->text(), QStringLiteral("> a\n> b\nc"));
    v->formatMarkdown(F::Quote);
    QCOMPARE(tab->text(), QStringLiteral("a\nb\nc"));
    // A selection ending at column 0 leaves that line alone.
    select(v, 0, 4);
    v->formatMarkdown(F::Quote);
    QCOMPARE(tab->text(), QStringLiteral("> a\n> b\nc"));
}

void TestFormatting::bulletToggle()
{
    DocumentTab *tab = makeTab(QStringLiteral("a\nb\n  c"));
    TextView *v = tab->editor();
    v->selectAll();
    v->formatMarkdown(F::BulletList);
    QCOMPARE(tab->text(), QStringLiteral("- a\n- b\n  - c"));
    v->formatMarkdown(F::BulletList);
    QCOMPARE(tab->text(), QStringLiteral("a\nb\n  c"));
}

void TestFormatting::orderedToggle()
{
    DocumentTab *tab = makeTab(QStringLiteral("a\nb\n\nc"));
    TextView *v = tab->editor();
    v->selectAll();
    v->formatMarkdown(F::OrderedList);
    QCOMPARE(tab->text(), QStringLiteral("1. a\n2. b\n\n3. c"));
    v->formatMarkdown(F::OrderedList);
    QCOMPARE(tab->text(), QStringLiteral("a\nb\n\nc"));
    // Bullets convert to numbers.
    tab->document()->setPlainText(QStringLiteral("- a\n- b"));
    v->selectAll();
    v->formatMarkdown(F::OrderedList);
    QCOMPARE(tab->text(), QStringLiteral("1. a\n2. b"));
    v->formatMarkdown(F::BulletList);
    QCOMPARE(tab->text(), QStringLiteral("- a\n- b"));
}

void TestFormatting::taskToggle()
{
    DocumentTab *tab = makeTab(QStringLiteral("a\n- b\n1. c"));
    TextView *v = tab->editor();
    v->selectAll();
    v->formatMarkdown(F::TaskList);
    QCOMPARE(tab->text(), QStringLiteral("- [ ] a\n- [ ] b\n1. [ ] c"));
    v->formatMarkdown(F::TaskList); // All unchecked: check all.
    QCOMPARE(tab->text(), QStringLiteral("- [ ] a\n- [ ] b\n1. [ ] c").replace(QStringLiteral("[ ]"), QStringLiteral("[x]")));
    v->formatMarkdown(F::TaskList); // All checked: uncheck all.
    QCOMPARE(tab->text(), QStringLiteral("- [ ] a\n- [ ] b\n1. [ ] c"));
}

void TestFormatting::linkWrapsSelection()
{
    DocumentTab *tab = makeTab(QStringLiteral("see docs now"));
    TextView *v = tab->editor();
    select(v, 4, 8);
    v->formatMarkdown(F::Link);
    QCOMPARE(tab->text(), QStringLiteral("see [docs](url) now"));
    QCOMPARE(selected(v), QStringLiteral("url"));
}

void TestFormatting::linkWithoutSelection()
{
    DocumentTab *tab = makeTab(QStringLiteral("ab"));
    TextView *v = tab->editor();
    select(v, 1, 1);
    v->formatMarkdown(F::Link);
    QCOMPARE(tab->text(), QStringLiteral("a[](url)b"));
    QCOMPARE(v->textCursor().position(), 2);
    QVERIFY(!v->textCursor().hasSelection());
}

void TestFormatting::codeBlockWrapsLines()
{
    DocumentTab *tab = makeTab(QStringLiteral("x\n  a\n  b\ny"));
    TextView *v = tab->editor();
    select(v, 2, 8); // Lines "  a" and "  b".
    v->formatMarkdown(F::CodeBlock);
    QCOMPARE(tab->text(), QStringLiteral("x\n  ```\n  a\n  b\n  ```\ny"));
    QCOMPARE(v->textCursor().position(), 7); // After the opening fence.
    QVERIFY(!v->textCursor().hasSelection());
    v->undo();
    QCOMPARE(tab->text(), QStringLiteral("x\n  a\n  b\ny"));
}

void TestFormatting::oneUndoStep()
{
    DocumentTab *tab = makeTab(QStringLiteral("a\nb\nc"));
    TextView *v = tab->editor();
    v->selectAll();
    const F formats[] = {F::Bold, F::Heading3, F::Quote, F::BulletList, F::OrderedList, F::TaskList, F::CodeBlock};
    for (F f : formats) {
        tab->document()->setPlainText(QStringLiteral("a\nb\nc"));
        v->selectAll();
        v->formatMarkdown(f);
        QVERIFY(tab->text() != QStringLiteral("a\nb\nc"));
        v->undo();
        QCOMPARE(tab->text(), QStringLiteral("a\nb\nc"));
    }
}

void TestFormatting::readOnlyIsNoOp()
{
    DocumentTab *tab = makeTab(QStringLiteral("a\nb"));
    TextView *v = tab->editor();
    v->setReadOnly(true);
    v->selectAll();
    for (F f : {F::Bold, F::Italic, F::Heading1, F::HeadingUp, F::Quote, F::BulletList, F::OrderedList,
                F::TaskList, F::Link, F::CodeBlock})
        v->formatMarkdown(f);
    QCOMPARE(tab->text(), QStringLiteral("a\nb"));
}

void TestFormatting::visualViewFormats()
{
    DocumentTab *tab = makeTab(QStringLiteral("hello"));
    tab->setViewMode(DocumentTab::ViewMode::Visual);
    TextView *v = tab->visualView();
    QVERIFY(v);
    QCOMPARE(tab->editor(), v);
    select(v, 0, 5);
    v->formatMarkdown(F::Bold);
    QCOMPARE(tab->text(), QStringLiteral("**hello**"));
    v->formatMarkdown(F::Heading1);
    QCOMPARE(tab->text(), QStringLiteral("# **hello**"));
}

void TestFormatting::disabledForPlainText()
{
    QFile md(m_dir.filePath(QStringLiteral("a.md")));
    QVERIFY(md.open(QIODevice::WriteOnly));
    md.write("hello\n");
    md.close();
    QFile txt(m_dir.filePath(QStringLiteral("a.txt")));
    QVERIFY(txt.open(QIODevice::WriteOnly));
    txt.write("hello\n");
    txt.close();

    MainWindow window;
    QVERIFY(window.openFile(txt.fileName()));
    window.show();
    for (const Command &c : commands()) {
        if (c.id.startsWith(QLatin1String("format.")))
            QVERIFY2(!actionOf(window, c.id)->isEnabled(), qPrintable(c.id));
    }
    actionOf(window, QStringLiteral("format.bold"))->trigger();
    QCOMPARE(window.currentTab()->text(), QStringLiteral("hello\n"));

    QVERIFY(window.openFile(md.fileName()));
    for (const Command &c : commands()) {
        if (c.id.startsWith(QLatin1String("format.")))
            QVERIFY2(actionOf(window, c.id)->isEnabled(), qPrintable(c.id));
    }
    TextView *v = window.currentTab()->editor();
    select(v, 0, 5);
    actionOf(window, QStringLiteral("format.bold"))->trigger();
    QCOMPARE(window.currentTab()->text(), QStringLiteral("**hello**\n"));
}

void TestFormatting::shortcutsAreBound()
{
    QCOMPARE(shortcutsFor(QStringLiteral("format.bold")), QList<QKeySequence>{QKeySequence(Qt::CTRL | Qt::Key_B)});
    QCOMPARE(shortcutsFor(QStringLiteral("view.outline")),
             QList<QKeySequence>{QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B)});

    QFile md(m_dir.filePath(QStringLiteral("k.md")));
    QVERIFY(md.open(QIODevice::WriteOnly));
    md.write("# Title\n\nhello\n");
    md.close();
    MainWindow window;
    QVERIFY(window.openFile(md.fileName()));
    window.show();
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    TextView *v = window.currentTab()->editor();
    v->setFocus();
    QTRY_VERIFY(v->hasFocus());
    QAction *outline = actionOf(window, QStringLiteral("view.outline"));
    const bool outlineBefore = outline->isChecked();
    select(v, 9, 14);
    QTest::keyClick(v, Qt::Key_B, Qt::ControlModifier);
    QCOMPARE(window.currentTab()->text(), QStringLiteral("# Title\n\n**hello**\n"));
    QCOMPARE(outline->isChecked(), outlineBefore);
    QTest::keyClick(v, Qt::Key_B, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(outline->isChecked(), !outlineBefore);
    QCOMPARE(window.currentTab()->text(), QStringLiteral("# Title\n\n**hello**\n"));
}

void TestFormatting::defaultShortcutsDoNotCollide()
{
    for (const Command &c : commands()) {
        for (const QKeySequence &k : c.defaults)
            QVERIFY2(conflictingCommands(k, c.id).isEmpty(), qPrintable(c.id + u' ' + k.toString()));
    }
}

void TestFormatting::findBarKeepsAltC()
{
    QFile md(m_dir.filePath(QStringLiteral("f.md")));
    QVERIFY(md.open(QIODevice::WriteOnly));
    md.write("hello\n");
    md.close();
    MainWindow window;
    QVERIFY(window.openFile(md.fileName()));
    window.show();
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    DocumentTab *tab = window.currentTab();
    tab->findBar()->open(false);
    QLineEdit *field = tab->findBar()->findField();
    QTRY_VERIFY(field->hasFocus());
    QToolButton *matchCase = nullptr;
    for (QToolButton *b : tab->findBar()->findChildren<QToolButton *>()) {
        if (b->text() == QStringLiteral("Aa"))
            matchCase = b;
    }
    QVERIFY(matchCase);
    QVERIFY(!matchCase->isChecked());
    QTest::keyClick(field, Qt::Key_C, Qt::AltModifier);
    QVERIFY(matchCase->isChecked());
    QCOMPARE(tab->text(), QStringLiteral("hello\n"));

    // In the editor Alt+C is Task List.
    TextView *v = tab->editor();
    v->setFocus();
    QTRY_VERIFY(v->hasFocus());
    select(v, 0, 0);
    QTest::keyClick(v, Qt::Key_C, Qt::AltModifier);
    QCOMPARE(tab->text(), QStringLiteral("- [ ] hello\n"));
}

QTEST_MAIN(TestFormatting)
#include "test_formatting.moc"
