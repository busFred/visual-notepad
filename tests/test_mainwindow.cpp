// Offscreen checks of the main window. Set VN_SCREENSHOT=<file.png> to also
// save a rendering of the window.
#include "Commands.h"
#include "MainWindow.h"
#include "Theme.h"

#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>
#include "DocumentTab.h"
#include "Settings.h"
#include "WelcomeView.h"
#include "editor/FindBar.h"
#include "editor/TextView.h"

#include <QLineEdit>
#include <QMimeData>

#include <QTextDocument>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

#include "TextFile.h"
#include "vn_core_bridge/lib.h"

#include <QElapsedTimer>

#include <functional>

namespace {

// The outline as text, one heading per line, indented by depth:
// "<depth> <text> @<line>".
QString dumpOutline(const QTreeWidget *outline)
{
    QString out;
    std::function<void(const QTreeWidgetItem *, int)> walk = [&](const QTreeWidgetItem *item, int depth) {
        out += QStringLiteral("%1 %2 @%3\n").arg(depth).arg(item->text(0)).arg(item->data(0, Qt::UserRole).toInt());
        for (int i = 0; i < item->childCount(); ++i)
            walk(item->child(i), depth + 1);
    };
    for (int i = 0; i < outline->topLevelItemCount(); ++i)
        walk(outline->topLevelItem(i), 0);
    return out;
}

// What the outline showed before it was cached and parsed off the GUI
// thread: core::outline of the text, each heading nested under the nearest
// previous one of lower level.
QString referenceOutline(const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    QString out;
    QList<int> levels;
    for (const auto &heading : vn::core::outline(rust::Str(utf8.constData(), utf8.size()))) {
        while (!levels.isEmpty() && levels.last() >= heading.level)
            levels.removeLast();
        out += QStringLiteral("%1 %2 @%3\n")
                   .arg(levels.size())
                   .arg(QString::fromUtf8(heading.text.data(), heading.text.size()))
                   .arg(static_cast<int>(heading.line));
        levels.append(heading.level);
    }
    return out;
}

QByteArray readSource(const char *relative)
{
    QFile file(QStringLiteral(VN_SOURCE_DIR "/") + QLatin1String(relative));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString firstHeading(const QTreeWidget *outline)
{
    return outline->topLevelItemCount() ? outline->topLevelItem(0)->text(0) : QString();
}

// Spins the event loop until the outline's worker thread has been started
// (not until it is done: that is only noticed by the next event loop pass).
bool waitUntilBusy(const vn::MainWindow &window)
{
    QElapsedTimer timer;
    timer.start();
    while (!window.outlineBusy() && timer.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return window.outlineBusy();
}

// Every awkward kind of heading (and not-heading) at once.
const char trickyDocument[] =
    "---\ntitle: x\n---\n# One\n\nSetext Two\n==========\n\nSetext Three\n------------\n\n## Dup\n\n## Dup\n\n### Dup\n\n"
    "> # Quoted *em* `code` [link](x) $x^2$ ![img](a.png)\n\n- # In list\n- text\n\n```\n# not a heading\n```\n\n"
    "~~~\n# nor this\n~~~\n\n<div>\n# html\n</div>\n\n#Not a heading\n\n####### seven\n\n   ## Indented ##\n\n"
    "# Trailing hashes ###\n\n## \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E heading\n\n| a | b |\n|---|---|\n| # c | d |\n\n"
    "#### Deep\n\n# Last\n";

} // namespace

class TestMainWindow : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void markdownShowsOutline();
    void textFileHidesOutline();
    void outlineMatchesCore();
    void outlineIsKeptPerTab();
    void largeOutlineIsParsedOffThread();
    void crlfIsPreserved();
    void fileDropOnEmptyWindowOpensIt();
    void fileDropOnTextTabOpensIt();
    void vscodeEditorShortcuts();
    void menuMnemonics();

private:
    QString write(const QString &name, const QByteArray &content);

    QTemporaryDir m_dir;
};

void TestMainWindow::initTestCase()
{
    QVERIFY(m_dir.isValid());
    vn::theme::apply(*qApp);
}

QString TestMainWindow::write(const QString &name, const QByteArray &content)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
        return {};
    return path;
}

void TestMainWindow::markdownShowsOutline()
{
    const QString path = write(QStringLiteral("notes.md"), "# Title\n\nSome *text*.\n\n## Section\n\n```python\n"
                                                           "from __future__ import annotations\n```\n\n## Other\n");
    vn::MainWindow window;
    QVERIFY(window.openFile(path));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *outline = window.findChild<QTreeWidget *>();
    QVERIFY(outline);
    QVERIFY(window.findChild<QDockWidget *>()->isVisible());
    QCOMPARE(outline->topLevelItemCount(), 1);
    QCOMPARE(outline->topLevelItem(0)->text(0), QStringLiteral("Title"));
    QCOMPARE(outline->topLevelItem(0)->childCount(), 2);
    QCOMPARE(window.windowTitle(), QStringLiteral("notes.md — Visual Notepad"));

    const QString shot = qEnvironmentVariable("VN_SCREENSHOT");
    if (!shot.isEmpty()) {
        QVERIFY(window.grab().save(shot));
        // Also save each menu-bar menu, opened under its title.
        auto *bar = window.menuBar();
        const auto actions = bar->actions();
        for (QAction *action : actions) {
            QMenu *menu = action->menu();
            menu->popup(window.mapToGlobal(bar->actionGeometry(action).bottomLeft()));
            QVERIFY(QTest::qWaitForWindowExposed(menu));
            QString name = action->text();
            name.remove(u'&');
            QVERIFY(menu->grab().save(QString(shot).replace(QStringLiteral(".png"), u'-' + name + u".png")));
            menu->hide();
        }
        // And the find/replace bar.
        auto *tab = window.findChild<vn::DocumentTab *>();
        tab->showFind(true);
        tab->findBar()->setQuery(QStringLiteral("Section"));
        tab->findBar()->replaceField()->setText(QStringLiteral("Part"));
        QVERIFY(window.grab().save(QString(shot).replace(QStringLiteral(".png"), QStringLiteral("-find.png"))));
    }
}

void TestMainWindow::textFileHidesOutline()
{
    const QString path = write(QStringLiteral("script.py"), "print('hi')\n");
    vn::MainWindow window;
    QVERIFY(window.openFile(path));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(!window.findChild<QDockWidget *>()->isVisible());
}

// The cached, threaded outline shows exactly what core::outline always gave:
// the repo's own documents, a document with every awkward heading, and
// documents past the size where parsing moves to a worker thread.
void TestMainWindow::outlineMatchesCore()
{
    QList<QByteArray> corpus{readSource("README.md"), readSource("docs/design.md"), QByteArray(trickyDocument)};
    QByteArray generated;
    for (int i = 0; i < 60; ++i)
        generated += QByteArray("## Section ") + QByteArray::number(i % 7) + "\n\ntext *with* markup\n\n### Part\n\n"
                     + "Title " + QByteArray::number(i) + "\n-----\n\n";
    corpus << generated << QByteArray(trickyDocument).repeated(2000) << generated.repeated(40);
    QVERIFY(!corpus[0].isEmpty() && !corpus[1].isEmpty());
    QVERIFY(corpus[4].size() > 600'000); // parsed on a worker thread
    QVERIFY(corpus[2].size() < 20'000);  // parsed directly

    vn::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *outline = window.findChild<QTreeWidget *>();
    QVERIFY(outline);
    for (int i = 0; i < corpus.size(); ++i) {
        const QString path = write(QStringLiteral("corpus%1.md").arg(i), corpus[i]);
        QVERIFY(window.openFile(path));
        QTRY_VERIFY(!window.outlineBusy());
        const QString expected = referenceOutline(window.currentTab()->text());
        QVERIFY2(!expected.isEmpty(), qPrintable(path));
        QCOMPARE(dumpOutline(outline), expected);
    }
    // Back on each of them: from the cache, the same.
    const int parses = window.outlineParses();
    for (int i = 0; i < corpus.size(); ++i) {
        window.setCurrentTabIndex(i);
        QVERIFY(!window.outlineBusy());
        QCOMPARE(dumpOutline(outline), referenceOutline(window.currentTab()->text()));
    }
    QCOMPARE(window.outlineParses(), parses);
    // The awkward document keeps its details.
    window.setCurrentTabIndex(2);
    const QString tricky = dumpOutline(outline);
    QVERIFY(tricky.contains(QStringLiteral("Setext Three @9")));
    QVERIFY(tricky.contains(QStringLiteral("Dup @")));
    QVERIFY(!tricky.contains(QStringLiteral("not a heading")));
}

// Switching tabs, toggling the outline and formatting changes parse nothing;
// only an edit does, once, however often the tabs are switched afterwards.
void TestMainWindow::outlineIsKeptPerTab()
{
    const QString a = write(QStringLiteral("keep-a.md"), "# Alpha\n\n## One\n");
    const QString b = write(QStringLiteral("keep-b.md"), "# Beta\n\n## Two\n\n## Three\n");
    vn::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *outline = window.findChild<QTreeWidget *>();
    QVERIFY(window.openFile(a));
    QVERIFY(window.openFile(b));
    QCOMPARE(window.outlineParses(), 2);
    QCOMPARE(outline->topLevelItem(0)->text(0), QStringLiteral("Beta"));

    for (int i = 0; i < 6; ++i) {
        window.setCurrentTabIndex(i % 2);
        QCOMPARE(outline->topLevelItem(0)->text(0), i % 2 ? QStringLiteral("Beta") : QStringLiteral("Alpha"));
        QCOMPARE(outline->topLevelItem(0)->childCount(), i % 2 ? 2 : 1);
    }
    QCOMPARE(window.outlineParses(), 2);

    // Hidden and shown again with the text unchanged.
    vn::Settings::instance().setValue(vn::key::outline, false);
    QVERIFY(window.findChild<QDockWidget *>()->isHidden());
    vn::Settings::instance().setValue(vn::key::outline, true);
    QCOMPARE(outline->topLevelItemCount(), 1);
    QCOMPARE(window.outlineParses(), 2);

    // An edit of the shown tab: rebuilt after the pause, once.
    vn::DocumentTab *tab = window.currentTab();
    QTextCursor end(tab->document());
    end.movePosition(QTextCursor::End);
    end.insertText(QStringLiteral("\n## Four\n"));
    QTRY_COMPARE(outline->topLevelItem(0)->childCount(), 3);
    QCOMPARE(window.outlineParses(), 3);
    QCOMPARE(dumpOutline(outline), referenceOutline(tab->text()));

    // An edit made while the outline is hidden: parsed when it is shown.
    vn::Settings::instance().setValue(vn::key::outline, false);
    end.insertText(QStringLiteral("\n# Five\n"));
    QCOMPARE(window.outlineParses(), 3);
    vn::Settings::instance().setValue(vn::key::outline, true);
    QCOMPARE(window.outlineParses(), 4);
    QCOMPARE(dumpOutline(outline), referenceOutline(tab->text()));

    // The other tab is still cached; going back and forth costs nothing.
    for (int i = 0; i < 4; ++i)
        window.setCurrentTabIndex((i + 1) % 2);
    QCOMPARE(window.outlineParses(), 4);
    QCOMPARE(dumpOutline(outline), referenceOutline(window.currentTab()->text()));

    // Closing a tab and opening a file in its place cannot revive its outline.
    QVERIFY(window.closeTabs(window.currentTabIndex(), vn::MainWindow::CloseScope::This));
    QVERIFY(window.openFile(write(QStringLiteral("keep-c.md"), "# Gamma\n")));
    QCOMPARE(outline->topLevelItem(0)->text(0), QStringLiteral("Gamma"));
}

// A big document is parsed off the GUI thread. The old outline stays while
// the same file is parsed again, another file's is cleared, and a result that
// edits have outdated is followed by the current one.
void TestMainWindow::largeOutlineIsParsedOffThread()
{
    QByteArray big;
    for (int i = 0; i < 20000; ++i)
        big += "## Heading " + QByteArray::number(i) + "\n\nsome text in a paragraph of the document\n\n";
    QVERIFY(big.size() > 600'000);
    const QString path = write(QStringLiteral("big.md"), big);
    const QString small = write(QStringLiteral("small.md"), "# Small\n");
    vn::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *outline = window.findChild<QTreeWidget *>();
    QVERIFY(window.openFile(small));
    QVERIFY(window.openFile(path));
    QVERIFY(window.outlineBusy());
    QCOMPARE(outline->topLevelItemCount(), 0); // not the small file's
    QTRY_VERIFY(!window.outlineBusy());
    QCOMPARE(outline->topLevelItemCount(), 20000);

    // Edits while a new parse runs.
    vn::DocumentTab *tab = window.currentTab();
    QTextCursor(tab->document()).insertText(QStringLiteral("# First\n"));
    QVERIFY(waitUntilBusy(window));
    QTextCursor(tab->document()).insertText(QStringLiteral("# Second\n"));
    QTRY_COMPARE_WITH_TIMEOUT(firstHeading(outline), QStringLiteral("Second"), 10000);
    QTRY_VERIFY(!window.outlineBusy());
    QCOMPARE(dumpOutline(outline), referenceOutline(tab->text()));

    // Switching away mid-parse and back ends with the right outline.
    QTextCursor(tab->document()).insertText(QStringLiteral("# Third\n"));
    QVERIFY(waitUntilBusy(window));
    window.setCurrentTabIndex(0);
    QCOMPARE(firstHeading(outline), QStringLiteral("Small"));
    window.setCurrentTabIndex(1);
    QTRY_COMPARE_WITH_TIMEOUT(firstHeading(outline), QStringLiteral("Third"), 10000);
    QTRY_VERIFY(!window.outlineBusy());
    QCOMPARE(dumpOutline(outline), referenceOutline(tab->text()));
}

void TestMainWindow::crlfIsPreserved()
{
    const QString path = write(QStringLiteral("dos.txt"), "a\r\nb\r\n");
    vn::MainWindow window;
    QVERIFY(window.openFile(path));
    window.show();
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto *editor = window.findChild<vn::TextView *>();
    QVERIFY(editor);
    QCOMPARE(editor->document()->toPlainText(), QStringLiteral("a\nb\n"));
    QTextCursor end(editor->document());
    end.movePosition(QTextCursor::End);
    end.insertText(QStringLiteral("\nc"));

    QTest::keySequence(editor, QKeySequence::Save);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("a\r\nb\r\n\r\nc"));
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

// With no tabs, a file dragged from the file manager lands on the welcome
// page. Qt sends the drag's moves and drop to whichever widget accepted its
// entry; this used to bounce between the editor group and the window until
// the stack overflowed.
void TestMainWindow::fileDropOnEmptyWindowOpensIt()
{
    const QString path = write(QStringLiteral("dropped.md"), "# Dropped\n");
    vn::MainWindow window;
    window.resize(800, 500);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCOMPARE(window.tabCount(), 0);
    auto *welcome = window.findChild<vn::WelcomeView *>();
    QVERIFY(welcome && welcome->isVisible());

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    const QPoint pos = welcome->rect().center();
    const Qt::DropActions actions = Qt::CopyAction | Qt::MoveAction | Qt::LinkAction;
    QDragEnterEvent enter(pos, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(welcome, &enter);
    QVERIFY(enter.isAccepted());
    QDragMoveEvent move(pos, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(welcome, &move);
    QVERIFY(move.isAccepted());
    QDropEvent drop(QPointF(pos), actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(welcome, &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(window.tabCount(), 1);
    QCOMPARE(window.windowTitle(), QStringLiteral("dropped.md — Visual Notepad"));
}

// A plain-text editor does not take file drops itself; they open as tabs.
void TestMainWindow::fileDropOnTextTabOpensIt()
{
    const QString text = write(QStringLiteral("plain.txt"), "plain\n");
    const QString path = write(QStringLiteral("dropped2.md"), "# Dropped\n");
    vn::MainWindow window;
    window.resize(800, 500);
    QVERIFY(window.openFile(text));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QWidget *target = window.currentTab()->editor()->viewport();

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    const QPoint pos = target->rect().center();
    const Qt::DropActions actions = Qt::CopyAction | Qt::MoveAction | Qt::LinkAction;
    QDragEnterEvent enter(pos, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    QVERIFY(enter.isAccepted());
    QDragMoveEvent move(pos, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &move);
    QDropEvent drop(QPointF(pos), actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(window.tabCount(), 2);
    QCOMPARE(window.windowTitle(), QStringLiteral("dropped2.md — Visual Notepad"));
}

// The VS Code editor commands are reachable through their default keys.
void TestMainWindow::vscodeEditorShortcuts()
{
    const QString path = write(QStringLiteral("keys.txt"), "alpha  \nbeta\ngamma\n");
    vn::MainWindow window;
    window.resize(800, 500);
    QVERIFY(window.openFile(path));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.activateWindow();
    vn::TextView *editor = window.currentTab()->editor();
    editor->setFocus();
    QVERIFY(QTest::qWaitFor([&] { return editor->hasFocus(); }));
    const auto actionFor = [&window](const QString &id) -> QAction * {
        const QString text = vn::command(id)->menuText;
        for (QAction *action : window.actions()) {
            if (action->text() == text)
                return action;
        }
        return nullptr;
    };

    for (const char *id : {"selection.addCursorsToLineEnds", "selection.expandLine", "selection.cursorUndo",
                           "selection.cursorRedo", "selection.moveLastToNextMatch", "edit.indentLines",
                           "edit.outdentLines", "edit.trimTrailingWhitespace", "edit.findNextSelection",
                           "edit.findPreviousSelection", "view.scrollPageUp", "view.scrollPageDown"})
        QVERIFY2(actionFor(QLatin1String(id)), id);
    QVERIFY(actionFor(QStringLiteral("selection.cursorUndo"))->shortcuts().isEmpty());

    // Ctrl+L selects the line, again adds one.
    QTest::keyClick(editor, Qt::Key_L, Qt::ControlModifier);
    QCOMPARE(editor->textCursor().selectedText().replace(QChar::ParagraphSeparator, u'|'), QStringLiteral("alpha  |"));
    QTest::keyClick(editor, Qt::Key_L, Qt::ControlModifier);
    QCOMPARE(editor->textCursor().selectedText().replace(QChar::ParagraphSeparator, u'|'),
             QStringLiteral("alpha  |beta|"));

    // Shift+Alt+I: a caret at the end of each selected line.
    QTest::keyClick(editor, Qt::Key_I, Qt::ShiftModifier | Qt::AltModifier);
    QCOMPARE(editor->cursorCount(), 2);

    // Ctrl+] / Ctrl+[ indent and outdent.
    editor->setTextCursor(QTextCursor(editor->document()));
    QTest::keyClick(editor, Qt::Key_BracketRight, Qt::ControlModifier);
    QVERIFY(editor->document()->toPlainText().startsWith(u' '));
    QTest::keyClick(editor, Qt::Key_BracketLeft, Qt::ControlModifier);
    QVERIFY(editor->document()->toPlainText().startsWith(QStringLiteral("alpha")));

    // Ctrl+K Ctrl+X trims trailing whitespace.
    QTest::keyClick(editor, Qt::Key_K, Qt::ControlModifier);
    QTest::keyClick(editor, Qt::Key_X, Qt::ControlModifier);
    QVERIFY(editor->document()->toPlainText().startsWith(QStringLiteral("alpha\nbeta")));
}

void TestMainWindow::menuMnemonics()
{
    vn::MainWindow window;
    QStringList barKeys;
    for (QAction *menuAction : window.menuBar()->actions()) {
        QMenu *menu = menuAction->menu();
        QVERIFY(menu);
        barKeys << QKeySequence::mnemonic(menuAction->text()).toString();
        // A repeated letter only selects in QMenu; it must be unique to trigger.
        QStringList keys;
        for (QAction *item : menu->actions()) {
            if (item->isSeparator() || item->text() == QLatin1String("Reset Zoom")) // No free letter
                continue;
            const QKeySequence key = QKeySequence::mnemonic(item->text());
            QVERIFY2(!key.isEmpty(), qPrintable(item->text()));
            QVERIFY2(!keys.contains(key.toString()), qPrintable(item->text()));
            keys << key.toString();
        }
    }
    barKeys.removeDuplicates();
    QCOMPARE(barKeys.size(), window.menuBar()->actions().size());
    for (const vn::Command &command : vn::commands())
        QVERIFY2(!command.text.contains(QLatin1Char('&')), qPrintable(command.text));
    QCOMPARE(vn::command(QStringLiteral("file.saveAs"))->text, QStringLiteral("Save As…"));
    QCOMPARE(vn::command(QStringLiteral("file.saveAs"))->menuText, QStringLiteral("Save &As…"));
    QCOMPARE(vn::menuForMnemonic(QKeySequence(QStringLiteral("Alt+O"))), QStringLiteral("Format"));
    QVERIFY(vn::menuForMnemonic(QKeySequence(QStringLiteral("Alt+C"))).isEmpty());
    QVERIFY(vn::menuForMnemonic(QKeySequence(QStringLiteral("Shift+Alt+F"))).isEmpty());
}

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
