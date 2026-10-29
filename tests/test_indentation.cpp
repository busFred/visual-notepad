// Per-file indentation: guessing (cases ported from VS Code's
// src/vs/editor/test/common/model/textModel.test.ts, "guess indentation 1"
// and the issue #44991 / #55818 / #70832 / #62143 tests), the status bar item
// and its menu, per-file overrides against the global settings, conversions.
#include "DocumentTab.h"
#include "MainWindow.h"
#include "Settings.h"
#include "Theme.h"
#include "editor/Indentation.h"
#include "editor/TextView.h"
#include "markdown/VisualView.h"

#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>

using namespace vn;

namespace {

struct Case
{
    // nullopt: the guesser cannot tell; the defaults come back.
    std::optional<bool> insertSpaces;
    // 0: cannot guess. spacesOnly: only guessed when inserting spaces.
    int tabSize;
    bool spacesOnly;
    QStringList lines;
    const char *message;
};

// Mirrors testGuessIndentation / assertGuess in textModel.test.ts: every case
// runs with both default insertSpaces values.
bool check(const Case &c)
{
    for (const bool defaultSpaces : {true, false}) {
        const int defaultSize = defaultSpaces ? 13370 : 13371;
        QTextDocument doc;
        doc.setPlainText(c.lines.join(u'\n'));
        const bool wantSpaces = c.insertSpaces.value_or(defaultSpaces);
        int wantSize = c.tabSize ? c.tabSize : defaultSize;
        if (c.spacesOnly && !wantSpaces)
            wantSize = defaultSize;
        const indentation::Settings got = indentation::guess(&doc, defaultSize, defaultSpaces);
        if (got.insertSpaces != wantSpaces || got.tabSize != wantSize) {
            qWarning("%s (default spaces %d): got spaces=%d size=%d, expected spaces=%d size=%d", c.message,
                     defaultSpaces, got.insertSpaces, got.tabSize, wantSpaces, wantSize);
            return false;
        }
    }
    return true;
}

} // namespace

class TestIndentation : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void guessesLikeVsCode();
    void guessesLikeVsCodeIssues();
    void alignmentIsIgnored();
    void guessesFromOpenedFile();
    void inconclusiveContentUsesDefaults();
    void detectionOffUsesDefaults();
    void statusBarText();
    void statusBarMenu();
    void overrideBeatsSettings();
    void settingsUpdateTabsWithoutOverride();
    void detectActionClearsOverride();
    void convertToSpacesIsOneUndo();
    void convertToTabsIsOneUndo();
    void convertSkipsReadOnly();

private:
    QString write(const QString &name, const QByteArray &content);
    static void setText(DocumentTab *tab, const QString &text);
    // What Tab types into an empty editor of the tab.
    static QString pressTab(DocumentTab *tab);
    static QLabel *indentLabel(MainWindow &window);
    // Runs `pick` on the open status bar menu, then closes it.
    static void withMenu(const std::function<void(QMenu *)> &pick);
    static QAction *find(QMenu *menu, const QString &text);

    QTemporaryDir m_dir;
};

void TestIndentation::initTestCase()
{
    QVERIFY(m_dir.isValid());
    theme::apply(*qApp);
}

void TestIndentation::init()
{
    // Each test starts from the default settings.
    Settings &s = Settings::instance();
    s.reset(key::tabWidth);
    s.reset(key::insertSpaces);
    s.reset(key::detectIndentation);
}

QString TestIndentation::write(const QString &name, const QByteArray &content)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
        return {};
    return path;
}

void TestIndentation::setText(DocumentTab *tab, const QString &text)
{
    tab->document()->setPlainText(text);
}

QString TestIndentation::pressTab(DocumentTab *tab)
{
    tab->document()->setPlainText(QString());
    tab->setLanguage(QString()); // Markdown has its own Tab handling.
    tab->show();
    QTest::qWaitForWindowExposed(tab);
    TextView *view = tab->sourceView();
    view->activateWindow();
    view->setFocus();
    view->setTextCursor(QTextCursor(tab->document()));
    QTest::keyClick(view, Qt::Key_Tab);
    const QString typed = tab->text();
    tab->document()->setPlainText(QString());
    return typed;
}

QLabel *TestIndentation::indentLabel(MainWindow &window)
{
    return window.findChild<QLabel *>(QStringLiteral("IndentationStatus"));
}

QAction *TestIndentation::find(QMenu *menu, const QString &text)
{
    for (QAction *action : menu->actions()) {
        if (action->text() == text)
            return action;
    }
    return nullptr;
}

void TestIndentation::withMenu(const std::function<void(QMenu *)> &pick)
{
    QTimer::singleShot(0, qApp, [pick] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        pick(menu);
        menu->close();
    });
}

void TestIndentation::guessesLikeVsCode()
{
    const int spacesOnly = true;
    const std::optional<bool> unknown;
    const QList<Case> cases = {
        {unknown, 0, false, {"x", "x", "x", "x", "x", "x", "x"}, "no clues"},
        {false, 0, false, {"\tx", "x", "x", "x", "x", "x", "x"}, "no spaces, 1xTAB"},
        {true, 2, false, {"  x", "x", "x", "x", "x", "x", "x"}, "1x2"},
        {false, 0, false, {"\tx", "\tx", "\tx", "\tx", "\tx", "\tx", "\tx"}, "7xTAB"},
        {unknown, 2, spacesOnly, {"\tx", "  x", "\tx", "  x", "\tx", "  x", "\tx", "  x"}, "4x2, 4xTAB"},
        {false, 0, false, {"\tx", " x", "\tx", " x", "\tx", " x", "\tx", " x"}, "4x1, 4xTAB"},
        {false, 0, false, {"\tx", "\tx", "  x", "\tx", "  x", "\tx", "  x", "\tx", "  x"}, "4x2, 5xTAB"},
        {false, 0, false, {"\tx", "\tx", "x", "\tx", "x", "\tx", "x", "\tx", "  x"}, "1x2, 5xTAB"},
        {false, 0, false, {"\tx", "\tx", "x", "\tx", "x", "\tx", "x", "\tx", "    x"}, "1x4, 5xTAB"},
        {false, 0, false, {"\tx", "\tx", "x", "\tx", "x", "\tx", "  x", "\tx", "    x"}, "1x2, 1x4, 5xTAB"},
        {unknown, 0, false, {"x", " x", " x", " x", " x", " x", " x", " x"}, "7x1 - 1 space is never guessed"},
        {true, 0, false, {"x", "          x", " x", " x", " x", " x", " x", " x"}, "1x10, 6x1"},
        {unknown, 0, false, {"", "  ", "    ", "      ", "        ", "          "}, "whitespace lines don't count"},
        {true, 3, false, {"x", "   x", "   x", "    x", "x", "   x", "   x", "    x", "x", "   x", "   x", "    x"}, "6x3, 3x4"},
        {true, 5, false, {"x", "     x", "     x", "    x", "x", "     x", "     x", "    x", "x", "     x", "     x", "    x"}, "6x5, 3x4"},
        {true, 7, false, {"x", "       x", "       x", "     x", "x", "       x", "       x", "    x", "x", "       x", "       x", "    x"}, "6x7, 1x5, 2x4"},
        {true, 2, false, {"x", "  x", "  x", "  x", "  x", "x", "  x", "  x", "  x", "  x"}, "8x2"},
        {true, 2, false, {"x", "  x", "    x", "x", "  x", "    x", "x", "  x", "    x", "x", "  x", "    x"}, "4x2, 4x4"},
        {true, 4, false, {"x", "    x", "    x", "x", "    x", "    x", "x", "    x", "    x", "x", "    x", "    x"}, "8x4"},
        {true, 2, false, {"x", "  x", "    x", "    x", "      x", "      x", "        x"}, "1x2, 2x4, 2x6, 1x8"},
        {true, 4, false, {"x", "    x", "    x", "    x", "     x", "        x", "x", "    x", "    x", "    x", "     x", "        x"}, "6x4, 2x5, 2x8"},
        {true, 3, false, {"x", " x", " x", " x", " x", " x", "x", "   x", "    x", "    x"}, "5x1, 2x0, 1x3, 2x4"},
        {false, 0, false, {"\t x", " \t x", "\tx"}, "mixed whitespace 1"},
        {false, 0, false, {"\tx", "\t    x"}, "mixed whitespace 2"},
    };
    for (const Case &c : cases)
        QVERIFY2(check(c), c.message);
}

void TestIndentation::guessesLikeVsCodeIssues()
{
    const std::optional<bool> unknown;
    // Issue #44991: wrong indentation size auto-detection.
    QVERIFY(check({true, 4, false,
                   {"a = 10", "b = 5", "if a > 10:", "    a += 1", "    if b > 5:", "        b += 1", "        b += 1",
                    "        b += 1", "# comment line 1", "# comment line 2", "# comment line 3", "        b += 1",
                    "        b += 1", "        b += 1"},
                   "#44991"}));
    // Issue #70832: a stray 4-space line among tabs.
    QVERIFY(check({false, 0, false,
                   {"x", "x", "x", "x", "\tx", "\t\tx", "    x", "\t\tx", "\tx", "\t\tx", "\tx", "\tx", "\tx", "\tx",
                    "x"},
                   "#70832"}));
    // Issue #62143: with the default size already matching, "- item" lists count.
    QVERIFY(check({true, 2, false, {"x", "  - item2", "  - item3"}, "#62143 a"}));
    QVERIFY(check({true, 2, false, {"x", "x", "  x", "  x"}, "#62143 b"}));
    // The 2-over-4 tie-break: at least 2/3 as many 2-diffs as 4-diffs.
    QVERIFY(check({true, 2, false,
                   {"x", "  x", "    x", "x", "  x", "    x", "x", "  x", "    x", "x", "  x", "    x"}, "2 beats 4"}));
    QVERIFY(check({true, 2, false, {"x", "  x", "  x", "    x", "x", "  x", "  x", "    x", "x", "  x", "  x", "    x"},
                   "6x2, 3x4"}));
    Q_UNUSED(unknown);
}

void TestIndentation::alignmentIsIgnored()
{
    // Issue #55818: aligned continuation lines are not an indentation hint.
    QVERIFY(check({true, 2, false,
                   {"", "/* REQUIRE */", "", "const foo = require ( 'foo' ),", "      bar = require ( 'bar' );", "",
                    "/* MY FN */", "", "function myFn () {", "", "  const asd = 1,", "        dsa = 2;", "",
                    "  return bar ( foo ( asd ) );", "", "}", "", "/* EXPORT */", "", "module.exports = myFn;", ""},
                   "#55818"}));
}

void TestIndentation::guessesFromOpenedFile()
{
    MainWindow window;
    QVERIFY(window.openFile(write("two.txt", "a\n  b\n  c\nd\n  e\n")));
    QCOMPARE(window.currentTab()->indentation(), (indentation::Settings{2, true}));
    QVERIFY(!window.currentTab()->indentationOverridden());
    QVERIFY(window.openFile(write("tabs.txt", "a\n\tb\n\tc\nd\n\te\n")));
    QCOMPARE(window.currentTab()->indentation().insertSpaces, false);
    QCOMPARE(window.currentTab()->indentation().tabSize, 4); // the default size
    QVERIFY(window.openFile(write("four.txt", "a\n    b\n    c\nd\n    e\n")));
    QCOMPARE(window.currentTab()->indentation(), (indentation::Settings{4, true}));
}

void TestIndentation::inconclusiveContentUsesDefaults()
{
    Settings::instance().setValue(key::tabWidth, 6);
    Settings::instance().setValue(key::insertSpaces, false);
    DocumentTab tab;
    QVERIFY(tab.load(write("flat.txt", "a\nb\nc\n")));
    QCOMPARE(tab.indentation(), (indentation::Settings{6, false}));
}

void TestIndentation::detectionOffUsesDefaults()
{
    const QString path = write("off.txt", "a\n  b\n  c\nd\n  e\n");
    Settings::instance().setValue(key::detectIndentation, false);
    DocumentTab tab;
    QVERIFY(tab.load(path));
    QCOMPARE(tab.indentation(), (indentation::Settings{4, true}));
    // Switching it on detects the content of tabs without an override.
    Settings::instance().setValue(key::detectIndentation, true);
    QCOMPARE(tab.indentation(), (indentation::Settings{2, true}));
    // Hot-exit content counts as arriving content.
    DocumentTab restored;
    restored.loadBackup(QString(), QStringLiteral("a\n\tb\n\tc\n"), QStringLiteral("UTF-8"),
                        DocumentTab::LineEnding::LF);
    QCOMPARE(restored.indentation().insertSpaces, false);
}

void TestIndentation::statusBarText()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openFile(write("s.txt", "a\n  b\n  c\nd\n  e\n")));
    QLabel *label = indentLabel(window);
    QVERIFY(label);
    QCOMPARE(label->text(), QStringLiteral("Spaces: 2"));
    QVERIFY(window.openFile(write("t.txt", "a\n\tb\n\tc\n")));
    QCOMPARE(label->text(), QStringLiteral("Tab Size: 4"));
    // Follows the tab selection.
    window.setCurrentTabIndex(0);
    QCOMPARE(label->text(), QStringLiteral("Spaces: 2"));
    // And hidden, like the other cells, with no file open.
    QVERIFY(window.closeTabs(0, MainWindow::CloseScope::All));
    QVERIFY(!label->isVisible());
}

void TestIndentation::statusBarMenu()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.openFile(write("m.txt", "a\n  b\n  c\nd\n  e\n")));
    DocumentTab *tab = window.currentTab();
    QLabel *label = indentLabel(window);

    withMenu([](QMenu *menu) {
        QVERIFY(find(menu, QStringLiteral("Indent Using Spaces")));
        QVERIFY(find(menu, QStringLiteral("Indent Using Tabs")));
        QVERIFY(find(menu, QStringLiteral("Detect Indentation from Content")));
        QVERIFY(find(menu, QStringLiteral("Convert Indentation to Spaces")));
        QVERIFY(find(menu, QStringLiteral("Convert Indentation to Tabs")));
        QMenu *spaces = find(menu, QStringLiteral("Indent Using Spaces"))->menu();
        QCOMPARE(spaces->actions().size(), 8);
        // The current choice is checked.
        QVERIFY(find(spaces, QStringLiteral("2"))->isChecked());
        QVERIFY(!find(spaces, QStringLiteral("4"))->isChecked());
        QMenu *tabs = find(menu, QStringLiteral("Indent Using Tabs"))->menu();
        QCOMPARE(tabs->actions().size(), 8);
        QVERIFY(!find(tabs, QStringLiteral("2"))->isChecked());
        find(tabs, QStringLiteral("8"))->trigger();
    });
    QTest::mouseClick(label, Qt::LeftButton);
    QCOMPARE(tab->indentation(), (indentation::Settings{8, false}));
    QVERIFY(tab->indentationOverridden());
    QCOMPARE(label->text(), QStringLiteral("Tab Size: 8"));

    withMenu([](QMenu *menu) {
        QMenu *tabs = find(menu, QStringLiteral("Indent Using Tabs"))->menu();
        QVERIFY(find(tabs, QStringLiteral("8"))->isChecked());
        find(find(menu, QStringLiteral("Indent Using Spaces"))->menu(), QStringLiteral("3"))->trigger();
    });
    QTest::mouseClick(label, Qt::LeftButton);
    QCOMPARE(label->text(), QStringLiteral("Spaces: 3"));

    withMenu([](QMenu *menu) { find(menu, QStringLiteral("Detect Indentation from Content"))->trigger(); });
    QTest::mouseClick(label, Qt::LeftButton);
    QCOMPARE(label->text(), QStringLiteral("Spaces: 2"));
    QVERIFY(!tab->indentationOverridden());

    withMenu([](QMenu *menu) { find(menu, QStringLiteral("Convert Indentation to Tabs"))->trigger(); });
    QTest::mouseClick(label, Qt::LeftButton);
    QCOMPARE(tab->text(), QStringLiteral("a\n\tb\n\tc\nd\n\te\n"));
    QCOMPARE(label->text(), QStringLiteral("Tab Size: 2"));
}

void TestIndentation::overrideBeatsSettings()
{
    DocumentTab tab;
    tab.setIndentation({3, false});
    Settings::instance().setValue(key::tabWidth, 8);
    Settings::instance().setValue(key::insertSpaces, true);
    QCOMPARE(tab.indentation(), (indentation::Settings{3, false}));
    // Content arriving later does not undo the user's choice either...
    QVERIFY(tab.load(write("o.txt", "a\n  b\n")));
    QCOMPARE(tab.indentation(), (indentation::Settings{2, true})); // ...but reloading is a fresh start.
    QVERIFY(!tab.indentationOverridden());
    tab.setIndentation({5, true});
    Settings::instance().setValue(key::detectIndentation, false);
    QCOMPARE(tab.indentation(), (indentation::Settings{5, true}));
    // The editor itself follows.
    QCOMPARE(pressTab(&tab), QStringLiteral("     "));
}

void TestIndentation::settingsUpdateTabsWithoutOverride()
{
    Settings::instance().setValue(key::detectIndentation, false);
    DocumentTab plain;
    DocumentTab chosen;
    chosen.setIndentation({3, true});
    Settings::instance().setValue(key::tabWidth, 2);
    QCOMPARE(plain.indentation(), (indentation::Settings{2, true}));
    QCOMPARE(chosen.indentation(), (indentation::Settings{3, true}));
    Settings::instance().setValue(key::insertSpaces, false);
    QCOMPARE(plain.indentation(), (indentation::Settings{2, false}));
    QCOMPARE(pressTab(&plain), QStringLiteral("\t"));
    QCOMPARE(chosen.indentation(), (indentation::Settings{3, true}));
}

void TestIndentation::detectActionClearsOverride()
{
    Settings::instance().setValue(key::detectIndentation, false);
    DocumentTab tab;
    setText(&tab, QStringLiteral("a\n  b\n  c\nd\n  e\n"));
    QCOMPARE(tab.indentation(), (indentation::Settings{4, true}));
    tab.setIndentation({8, false});
    // Detecting works even with the setting off.
    tab.detectIndentation();
    QCOMPARE(tab.indentation(), (indentation::Settings{2, true}));
    QVERIFY(!tab.indentationOverridden());
}

void TestIndentation::convertToSpacesIsOneUndo()
{
    DocumentTab tab;
    tab.setIndentation({4, false});
    const QString original = QStringLiteral("a\n\tb\n\t\tc\n  \t d\n\n\t\nend\t\tx");
    setText(&tab, original);
    tab.document()->setUndoRedoEnabled(false);
    tab.document()->setUndoRedoEnabled(true);
    tab.setIndentation({4, false});
    tab.convertIndentation(true);
    // Only the indentation changes; whitespace-only lines convert wholly.
    QCOMPARE(tab.text(), QStringLiteral("a\n    b\n        c\n       d\n\n    \nend\t\tx"));
    QCOMPARE(tab.indentation(), (indentation::Settings{4, true}));
    QVERIFY(tab.indentationOverridden());
    QVERIFY(tab.document()->isUndoAvailable());
    tab.document()->undo();
    QCOMPARE(tab.text(), original);
    QVERIFY(!tab.document()->isUndoAvailable());
}

void TestIndentation::convertToTabsIsOneUndo()
{
    DocumentTab tab;
    tab.setIndentation({2, true});
    const QString original = QStringLiteral("a\n  b\n    c\n   d\n  \nx    y");
    setText(&tab, original);
    tab.document()->setUndoRedoEnabled(false);
    tab.document()->setUndoRedoEnabled(true);
    tab.convertIndentation(false);
    // Each run of tabSize spaces in the indentation becomes a tab; the odd
    // space stays, and so does text after the indentation.
    QCOMPARE(tab.text(), QStringLiteral("a\n\tb\n\t\tc\n\t d\n\t\nx    y"));
    QCOMPARE(tab.indentation(), (indentation::Settings{2, false}));
    tab.document()->undo();
    QCOMPARE(tab.text(), original);
    QVERIFY(!tab.document()->isUndoAvailable());
}

void TestIndentation::convertSkipsReadOnly()
{
    DocumentTab tab;
    QVERIFY(tab.load(write("ro.md", "a\n\tb\n")));
    tab.setViewMode(DocumentTab::ViewMode::Visual);
    for (QToolButton *button : tab.findChildren<QToolButton *>()) {
        if (button->toolTip() == QStringLiteral("Read-only"))
            button->click();
    }
    QVERIFY(tab.isReadOnly());
    tab.convertIndentation(true);
    QCOMPARE(tab.text(), QStringLiteral("a\n\tb\n"));
    QVERIFY(!tab.indentationOverridden());
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestIndentation)
#include "test_indentation.moc"
