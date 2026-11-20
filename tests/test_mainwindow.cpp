// Offscreen checks of the main window. Set VN_SCREENSHOT=<file.png> to also
// save a rendering of the window.
#include "Commands.h"
#include "EditorGroup.h"
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

#include "markdown/VisualView.h"

#include <QFontMetricsF>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QSlider>
#include <QStyleOptionSlider>
#include <QWheelEvent>

#include <QTextDocument>
#include <QScrollBar>
#include <QScreen>
#include <QSignalSpy>
#include <QTextBlock>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>

#include "TextFile.h"
#include "vn_core_bridge/lib.h"

#include <QElapsedTimer>
#include <QScopeGuard>

#include <functional>
#include <memory>

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

QToolButton *readControl(const QFrame *popup, const char *name)
{
    for (auto *button : popup->findChildren<QToolButton *>()) {
        if (button->property("readControl").toString() == QLatin1String(name))
            return button;
    }
    return nullptr;
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
    void unchangedHeadingsLeaveTheOutlineAlone();
    void outlineCloseButtonHides();
    void outlineToggleIsPerWindow();
    void largeOutlineIsParsedOffThread();
    void crlfIsPreserved();
    void fileDropOnEmptyWindowOpensIt();
    void fileDropOnTextTabOpensIt();
    void vscodeEditorShortcuts();
    void menuMnemonics();
    void zoomIsPerHalf();
    void fontSurvivesThemeChange();
    void movedTabKeepsItsFont();
    void accentFollowsTheHalfInUse();
    void zoomStepsAndDefault();
    void ctrlWheelZoomsTheHalfInUse();
    void editorLineSpacingLeavesTheVisualViewAlone();
    void editorLineSpacingLeavesReadModeAlone();
    void zoomCellAndPopup();
    void zoomPopupInSingleView();
    void newTabStartsAtDefaultZoom();
    void adoptedTabKeepsItsZoom();
    void collapseCommands();
    void readModeStatusAndPages();
    void readPopupIsPerTabAndStartsFromPreferences();
    void readPopupNarrowsAndRestoresWidth();
    void readPopupFollowsColumnsButtonWhenStatusChanges();
    void columnsResolvePreferenceOnce_data();
    void columnsResolvePreferenceOnce();
    void viewSwitchComesFirstAndHides();
    void columnsMenuItemMirrorsTheSwitch();
    void columnsArePerTabAndNotSaved();
    void lockNoLongerForcesColumns();
    void sourceColumnsLeaveForTheSplitAndComeBack();
    void sourceColumnsStatusBarAndEditing();

private:
    // A Markdown tab in `mode`, in a shown window; the half given has focus.
    vn::DocumentTab *zoomTab(vn::MainWindow &window, vn::DocumentTab::ViewMode mode);
    static void focusView(vn::TextView *view);
    static QWidget *accentOf(vn::TextView *view);
    // A Markdown file of many paragraphs in a tab of the window, in Visual
    // mode, locked, in Columns: read mode.
    vn::DocumentTab *readingTab(vn::MainWindow &window, const QString &name);
    static void setLocked(vn::DocumentTab *tab, bool locked);
    // The Scroll or Columns button of the status bar's view switch.
    static QToolButton *viewButton(vn::MainWindow &window, bool columns);
    // The status cells shown, left to right (the switch is "ViewSwitch").
    static QStringList shownCells(vn::MainWindow &window);
    // A Python file of `lines` lines in a tab of the window.
    vn::DocumentTab *codeTab(vn::MainWindow &window, const QString &name, int lines);
    static void wheel(vn::TextView *over, int delta);
    QString write(const QString &name, const QByteArray &content);

    QTemporaryDir m_dir;
    int m_zoomFiles = 0;
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

// An edit that leaves the headings as they are does not rebuild the tree (its
// items stay), unless the user's selection or a collapsed branch is there:
// then the rebuild clears it, as it always did.
void TestMainWindow::unchangedHeadingsLeaveTheOutlineAlone()
{
    vn::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *outline = window.findChild<QTreeWidget *>();
    QVERIFY(window.openFile(write(QStringLiteral("same.md"), "# A\n\n## B\n\ntext\n\n## C\n")));
    vn::DocumentTab *tab = window.currentTab();
    QVERIFY(outline->topLevelItem(0));
    // A mark on the item, not its address: a rebuilt tree's new item can be
    // allocated where the old one was (it was, on macOS).
    constexpr int markRole = Qt::UserRole + 1000;
    outline->topLevelItem(0)->setData(0, markRole, true);
    const auto marked = [&] { return outline->topLevelItem(0) && outline->topLevelItem(0)->data(0, markRole).toBool(); };
    const int parses = window.outlineParses();

    QTextCursor end(tab->document());
    end.movePosition(QTextCursor::End);
    end.insertText(QStringLiteral("more text\n"));
    QTRY_COMPARE(window.outlineParses(), parses + 1);
    QVERIFY(marked()); // not rebuilt
    QCOMPARE(dumpOutline(outline), referenceOutline(tab->text()));

    // A changed heading is rebuilt.
    end.insertText(QStringLiteral("## D\n"));
    QTRY_COMPARE(outline->topLevelItem(0)->child(0)->parent()->childCount(), 3);
    QVERIFY(!marked());
    QCOMPARE(dumpOutline(outline), referenceOutline(tab->text()));

    // A selected item, or a collapsed branch, is gone after the next one.
    outline->setCurrentItem(outline->topLevelItem(0)->child(1));
    QVERIFY(outline->currentItem());
    end.insertText(QStringLiteral("typing\n"));
    QTRY_COMPARE(window.outlineParses(), parses + 3);
    QVERIFY(!outline->currentItem());
    QVERIFY(outline->topLevelItem(0)->isExpanded());
    outline->topLevelItem(0)->setExpanded(false);
    end.insertText(QStringLiteral("typing\n"));
    QTRY_COMPARE(window.outlineParses(), parses + 4);
    QVERIFY(outline->topLevelItem(0)->isExpanded());
    QCOMPARE(dumpOutline(outline), referenceOutline(tab->text()));
}

// The close button in the outline's title hides it, like View > Outline.
void TestMainWindow::outlineCloseButtonHides()
{
    vn::Settings::instance().setValue(vn::key::outline, true);
    vn::MainWindow window;
    QVERIFY(window.openFile(write(QStringLiteral("close-button.md"), "# Title\n")));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *dock = window.findChild<QDockWidget *>();
    QVERIFY(dock->isVisible());
    auto *close = dock->titleBarWidget()->findChild<QToolButton *>();
    QVERIFY(close);
    QTest::mouseClick(close, Qt::LeftButton);
    QVERIFY(dock->isHidden());
    QVERIFY(!vn::Settings::instance().flag(vn::key::outline));
    vn::Settings::instance().setValue(vn::key::outline, true);
    QVERIFY(dock->isVisible());
}

// The menu command, its shortcut and the close button act on their own window
// only; the choice is kept for windows opened later. Preferences reach all.
void TestMainWindow::outlineToggleIsPerWindow()
{
    vn::Settings::instance().setValue(vn::key::outline, true);
    const QString path = write(QStringLiteral("per-window.md"), "# Title\n\n## Part\n");
    vn::MainWindow one;
    vn::MainWindow two;
    for (vn::MainWindow *w : {&one, &two}) {
        QVERIFY(w->openFile(path));
        w->show();
        QVERIFY(QTest::qWaitForWindowExposed(w));
    }
    auto *dockOne = one.findChild<QDockWidget *>();
    auto *dockTwo = two.findChild<QDockWidget *>();
    QVERIFY(dockOne->isVisible());
    QVERIFY(dockTwo->isVisible());

    // The command (menu item or Ctrl+Shift+B) in the first window.
    const auto outlineAction = [](vn::MainWindow &w) -> QAction * {
        for (QAction *a : w.actions()) {
            if (a->text() == vn::command(QStringLiteral("view.outline"))->menuText)
                return a;
        }
        return nullptr;
    };
    QAction *toggleOne = outlineAction(one);
    QAction *toggleTwo = outlineAction(two);
    QVERIFY(toggleOne && toggleTwo);
    toggleOne->trigger();
    QVERIFY(dockOne->isHidden());
    QVERIFY(!toggleOne->isChecked());
    QVERIFY(dockTwo->isVisible());
    QVERIFY(toggleTwo->isChecked());
    QVERIFY(!vn::Settings::instance().flag(vn::key::outline));
    toggleOne->trigger();
    QVERIFY(dockOne->isVisible());
    QVERIFY(dockTwo->isVisible());

    // The close button in the second window's outline title.
    auto *close = dockTwo->titleBarWidget()->findChild<QToolButton *>();
    QVERIFY(close);
    QTest::mouseClick(close, Qt::LeftButton);
    QVERIFY(dockTwo->isHidden());
    QVERIFY(!toggleTwo->isChecked());
    QVERIFY(dockOne->isVisible());
    QVERIFY(toggleOne->isChecked());

    // A new window follows the last choice.
    {
        vn::MainWindow three;
        QVERIFY(three.openFile(path));
        QVERIFY(!outlineAction(three)->isChecked());
    }

    // The preference applies to every window.
    vn::Settings::instance().setValue(vn::key::outline, true);
    QVERIFY(dockOne->isVisible());
    QVERIFY(dockTwo->isVisible());
    QVERIFY(toggleTwo->isChecked());
    vn::Settings::instance().setValue(vn::key::outline, false);
    QVERIFY(dockOne->isHidden());
    QVERIFY(dockTwo->isHidden());
    vn::Settings::instance().setValue(vn::key::outline, true);
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
    // A file this big loads on a worker thread; its tab, and with it the
    // parse, follows from the event loop.
    bool opened = false, busy = false;
    int itemsAtOpen = -1;
    QVERIFY(window.openFile(path, [&](vn::DocumentTab *) {
        opened = true;
        busy = window.outlineBusy();
        itemsAtOpen = outline->topLevelItemCount();
    }));
    QTRY_VERIFY(opened);
    QVERIFY(busy);
    QCOMPARE(itemsAtOpen, 0); // not the small file's
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

namespace {

// The letter after "&" in a menu title, upper case, or empty. (Not
// QKeySequence::mnemonic(), which is empty on macOS: Qt has no mnemonics there.)
QString mnemonicLetter(const QString &text)
{
    for (qsizetype i = 0; i + 1 < text.size(); ++i) {
        if (text.at(i) == u'&') {
            if (text.at(i + 1) == u'&') {
                ++i;
                continue;
            }
            return text.at(i + 1).toUpper();
        }
    }
    return {};
}

} // namespace

void TestMainWindow::menuMnemonics()
{
    vn::MainWindow window;
    QStringList barKeys;
    for (QAction *menuAction : window.menuBar()->actions()) {
        QMenu *menu = menuAction->menu();
        QVERIFY(menu);
        barKeys << mnemonicLetter(menuAction->text());
        // A repeated letter only selects in QMenu; it must be unique to trigger.
        QStringList keys;
        for (QAction *item : menu->actions()) {
            if (item->isSeparator() || item->text() == QLatin1String("Reset Zoom")
                || item->text() == QLatin1String("Columns")) // No free letter
                continue;
            const QString key = mnemonicLetter(item->text());
            QVERIFY2(!key.isEmpty(), qPrintable(item->text()));
            QVERIFY2(!keys.contains(key), qPrintable(item->text()));
            keys << key;
        }
    }
    barKeys.removeDuplicates();
    QCOMPARE(barKeys.size(), window.menuBar()->actions().size());
    for (const vn::Command &command : vn::commands())
        QVERIFY2(!command.text.contains(QLatin1Char('&')), qPrintable(command.text));
    QCOMPARE(vn::command(QStringLiteral("file.saveAs"))->text, QStringLiteral("Save As…"));
    QCOMPARE(vn::command(QStringLiteral("file.saveAs"))->menuText, QStringLiteral("Save &As…"));
#ifndef Q_OS_MACOS
    QCOMPARE(vn::menuForMnemonic(QKeySequence(QStringLiteral("Alt+O"))), QStringLiteral("Format"));
#endif
    QVERIFY(vn::menuForMnemonic(QKeySequence(QStringLiteral("Alt+C"))).isEmpty());
    QVERIFY(vn::menuForMnemonic(QKeySequence(QStringLiteral("Shift+Alt+F"))).isEmpty());
}

// The window's action for a command, as the menus and shortcuts see it.
static QAction *actionFor(const vn::MainWindow &window, const QString &id)
{
    const QString text = vn::command(id)->menuText;
    for (QAction *action : window.actions()) {
        if (action->text() == text)
            return action;
    }
    return nullptr;
}

vn::DocumentTab *TestMainWindow::zoomTab(vn::MainWindow &window, vn::DocumentTab::ViewMode mode)
{
    vn::Settings::instance().reset(vn::key::zoom);
    const QString path = write(QStringLiteral("zoom%1.md").arg(m_zoomFiles++), "# Title\n\nSome text.\n");
    window.resize(900, 500);
    if (!window.openFile(path))
        return nullptr;
    vn::DocumentTab *tab = window.currentTab();
    tab->setViewMode(mode);
    window.show();
    if (!QTest::qWaitForWindowExposed(&window))
        return nullptr;
    window.activateWindow();
    return tab;
}

void TestMainWindow::focusView(vn::TextView *view)
{
    view->setFocus();
    QTRY_VERIFY(view->hasFocus());
}

QWidget *TestMainWindow::accentOf(vn::TextView *view)
{
    return view->findChild<QWidget *>(QStringLiteral("ZoomAccent"));
}

// Ctrl+wheel over `over`, `delta` eighths of a degree (120 is one notch).
void TestMainWindow::wheel(vn::TextView *over, int delta)
{
    QWidget *target = over->viewport();
    const QPointF pos = target->rect().center();
    QWheelEvent event(pos, target->mapToGlobal(pos), {}, QPoint(0, delta), Qt::NoButton, Qt::ControlModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(target, &event);
}

// Source and Visual each have their own zoom, and the keys and the status
// cell act on the one last focused.
void TestMainWindow::zoomIsPerHalf()
{
    vn::MainWindow window;
    vn::DocumentTab *tab = zoomTab(window, vn::DocumentTab::ViewMode::SourceVisual);
    QVERIFY(tab);
    vn::TextView *source = tab->sourceView();
    vn::TextView *visual = tab->visualView();
    QVERIFY(visual);
    auto *cell = window.findChild<QLabel *>(QStringLiteral("ZoomStatus"));
    QVERIFY(cell);
    const qreal base = vn::theme::editorFont().pointSizeF();

    focusView(visual);
    QCOMPARE(tab->editor(), visual);
    actionFor(window, QStringLiteral("view.zoomIn"))->trigger();
    QCOMPARE(tab->zoom(visual), 105);
    QCOMPARE(tab->zoom(source), 100);
    QCOMPARE(cell->text(), QStringLiteral("105%"));
    QCOMPARE(cell->toolTip(), QStringLiteral("Visual zoom"));
    QCOMPARE(visual->font().pointSizeF(), base * 1.05);
    QCOMPARE(source->font().pointSizeF(), base);

    focusView(source);
    QCOMPARE(tab->editor(), source);
    QCOMPARE(cell->text(), QStringLiteral("100%"));
    QCOMPARE(cell->toolTip(), QStringLiteral("Source zoom"));
    actionFor(window, QStringLiteral("view.zoomOut"))->trigger();
    actionFor(window, QStringLiteral("view.zoomOut"))->trigger();
    QCOMPARE(tab->zoom(source), 90);
    QCOMPARE(tab->zoom(visual), 105);
    QCOMPARE(cell->text(), QStringLiteral("90%"));
    QCOMPARE(source->font().pointSizeF(), base * 0.9);
    QCOMPARE(visual->font().pointSizeF(), base * 1.05);

    // Back on the visual half, its own value is what the cell shows again.
    focusView(visual);
    QCOMPARE(cell->text(), QStringLiteral("105%"));
}

// The style sheet is applied again on a theme change: fonts set on the views
// must come out of it as they were.
void TestMainWindow::fontSurvivesThemeChange()
{
    vn::MainWindow window;
    vn::DocumentTab *tab = zoomTab(window, vn::DocumentTab::ViewMode::SourceVisual);
    QVERIFY(tab);
    vn::Settings &settings = vn::Settings::instance();
    const QScopeGuard restore([&settings] {
        settings.reset(vn::key::theme);
        settings.reset(vn::key::fontFamily);
        settings.reset(vn::key::fontSize);
        settings.resetColorOverride(QStringLiteral("standard"), QStringLiteral("editor"));
    });
    settings.setValue(vn::key::theme, QStringLiteral("modern"));
    {
        vn::Settings::ChangeBatch batch;
        settings.setValue(vn::key::fontFamily, QStringLiteral("DejaVu Sans Mono"));
        settings.setValue(vn::key::fontSize, 17);
    }
    focusView(tab->sourceView());
    tab->setZoom(150);
    const QFont source = tab->sourceView()->font();
    const QFont visual = tab->visualView()->font();
    QCOMPARE(source.pointSizeF(), 17 * 1.5);
    QCOMPARE(visual.pointSizeF(), 17.0);

    settings.setValue(vn::key::theme, QStringLiteral("standard"));
    QCOMPARE(tab->sourceView()->font().pointSizeF(), source.pointSizeF());
    QCOMPARE(tab->sourceView()->font().family(), source.family());
    QCOMPARE(tab->visualView()->font().pointSizeF(), visual.pointSizeF());
    QCOMPARE(tab->visualView()->font().family(), visual.family());

    // Colour overrides apply the style sheet again too.
    settings.setColorOverride(QStringLiteral("standard"), QStringLiteral("editor"), QColor(0x123456));
    QCOMPARE(tab->sourceView()->font().pointSizeF(), source.pointSizeF());
    QCOMPARE(tab->visualView()->font().pointSizeF(), visual.pointSizeF());

    // A tab that is hidden when the theme changes gets its fonts when shown,
    // and so does one whose font changed in the meantime.
    tab->hide();
    settings.setValue(vn::key::theme, QStringLiteral("classic"));
    tab->show();
    QCOMPARE(tab->sourceView()->font().pointSizeF(), source.pointSizeF());
    QCOMPARE(tab->visualView()->font().pointSizeF(), visual.pointSizeF());
    tab->hide();
    settings.setValue(vn::key::fontSize, 11);
    settings.setValue(vn::key::theme, QStringLiteral("modern"));
    tab->show();
    QCOMPARE(tab->sourceView()->font().pointSizeF(), 11 * 1.5);
    QCOMPARE(tab->visualView()->font().pointSizeF(), 11.0);
}

// Moving a tab to another group or window shows it again under a new parent,
// which polishes it again: it keeps the font and zoom it had.
void TestMainWindow::movedTabKeepsItsFont()
{
    vn::MainWindow window;
    vn::DocumentTab *tab = zoomTab(window, vn::DocumentTab::ViewMode::SourceVisual);
    QVERIFY(tab);
    QVERIFY(window.openFile(write(QStringLiteral("moved.txt"), "other\n")));
    vn::Settings &settings = vn::Settings::instance();
    const QScopeGuard restore([&settings] {
        settings.reset(vn::key::fontFamily);
        settings.reset(vn::key::fontSize);
    });
    settings.setValue(vn::key::fontSize, 13);
    window.activateGroup(window.groupOf(tab));
    window.groupOf(tab)->setCurrentIndex(window.groupOf(tab)->indexOf(tab));
    focusView(tab->sourceView());
    tab->setZoom(150);
    QCOMPARE(tab->sourceView()->font().pointSizeF(), 13 * 1.5);
    QCOMPARE(tab->visualView()->font().pointSizeF(), 13.0);

    vn::EditorGroup *beside = window.addGroup(window.groupOf(tab), vn::MainWindow::SplitDirection::Right);
    window.receiveTab(tab, beside, -1);
    QCOMPARE(window.groupOf(tab), beside);
    QTRY_VERIFY(tab->isVisible());
    QCOMPARE(tab->sourceView()->font().pointSizeF(), 13 * 1.5);
    QCOMPARE(tab->visualView()->font().pointSizeF(), 13.0);

    vn::MainWindow *other = window.moveTabsToNewWindow({tab}, std::nullopt, false);
    QVERIFY(other);
    const QScopeGuard close([other] { delete other; });
    QVERIFY(QTest::qWaitForWindowExposed(other));
    QTRY_VERIFY(tab->isVisible());
    QCOMPARE(tab->sourceView()->font().pointSizeF(), 13 * 1.5);
    QCOMPARE(tab->visualView()->font().pointSizeF(), 13.0);
}

// The 2 px accent line sits along the bottom of the half last focused, in a
// split only.
void TestMainWindow::accentFollowsTheHalfInUse()
{
    vn::MainWindow window;
    vn::DocumentTab *tab = zoomTab(window, vn::DocumentTab::ViewMode::SourceVisual);
    QVERIFY(tab);
    vn::TextView *source = tab->sourceView();
    vn::TextView *visual = tab->visualView();
    QWidget *sourceLine = accentOf(source);
    QWidget *visualLine = accentOf(visual);
    QVERIFY(sourceLine && visualLine);

    focusView(source);
    QVERIFY(sourceLine->isVisible());
    QVERIFY(!visualLine->isVisible());
    QCOMPARE(sourceLine->height(), 2);
    QCOMPARE(sourceLine->geometry(), QRect(0, source->height() - 2, source->width(), 2));
    // It is drawn in the theme's accent colour, over the bottom edge.
    const QImage image = source->grab().toImage();
    const qreal ratio = image.devicePixelRatio();
    QCOMPARE(image.pixelColor(int(source->width() / 2 * ratio), int((source->height() - 1) * ratio)),
             vn::theme::color(vn::theme::Role::accent));

    focusView(visual);
    QVERIFY(!sourceLine->isVisible());
    QVERIFY(visualLine->isVisible());
    QCOMPARE(visualLine->geometry(), QRect(0, visual->height() - 2, visual->width(), 2));

    // One view alone has nothing to tell apart.
    tab->setViewMode(vn::DocumentTab::ViewMode::Visual);
    QVERIFY(!sourceLine->isVisible());
    QVERIFY(!visualLine->isVisible());
    tab->setViewMode(vn::DocumentTab::ViewMode::Source);
    QVERIFY(!sourceLine->isVisible());
    QVERIFY(!visualLine->isVisible());
}

// Steps of 5% between 30% and 500%; Ctrl+0 goes to the "default zoom"
// preference rather than 100%.
void TestMainWindow::zoomStepsAndDefault()
{
    vn::MainWindow window;
    vn::DocumentTab *tab = zoomTab(window, vn::DocumentTab::ViewMode::SourceVisual);
    QVERIFY(tab);
    vn::TextView *source = tab->sourceView();
    vn::TextView *visual = tab->visualView();
    focusView(source);

    QAction *in = actionFor(window, QStringLiteral("view.zoomIn"));
    QAction *out = actionFor(window, QStringLiteral("view.zoomOut"));
    in->trigger();
    QCOMPARE(tab->zoom(), 105);
    in->trigger();
    QCOMPARE(tab->zoom(), 110);
    out->trigger();
    out->trigger();
    out->trigger();
    QCOMPARE(tab->zoom(), 95);
    for (int i = 0; i < 100; ++i)
        out->trigger();
    QCOMPARE(tab->zoom(), 30);
    for (int i = 0; i < 200; ++i)
        in->trigger();
    QCOMPARE(tab->zoom(), 500);

    focusView(visual);
    in->trigger();
    QCOMPARE(tab->zoom(visual), 105);
    vn::Settings::instance().setValue(vn::key::zoom, 125);
    // Ctrl+0 resets the half in use only.
    QTest::keyClick(visual, Qt::Key_0, Qt::ControlModifier);
    QCOMPARE(tab->zoom(visual), 125);
    QCOMPARE(tab->zoom(source), 500);
    focusView(source);
    QTest::keyClick(source, Qt::Key_0, Qt::ControlModifier);
    QCOMPARE(tab->zoom(source), 125);
    // Changing the preference leaves open tabs alone.
    vn::Settings::instance().setValue(vn::key::zoom, 90);
    QCOMPARE(tab->zoom(source), 125);
    QCOMPARE(tab->zoom(visual), 125);
    vn::Settings::instance().reset(vn::key::zoom);
}

// The editor's line spacing is the source's: the visual half (rows, gaps,
// pixels) answers to the Markdown line spacing only.
void TestMainWindow::editorLineSpacingLeavesTheVisualViewAlone()
{
    vn::Settings &settings = vn::Settings::instance();
    const QVariant editorSpacing = settings.value(vn::key::lineSpacing);
    const QVariant textSpacing = settings.value(vn::key::textLineHeight);
    settings.setValue(vn::key::lineSpacing, 1.0);
    settings.setValue(vn::key::textLineHeight, 1.4);
    QByteArray text;
    for (int i = 1; i <= 40; ++i)
        text += QByteArray("Paragraph ") + QByteArray::number(i) + "\n\n";
    vn::MainWindow window;
    window.resize(900, 500);
    QVERIFY(window.openFile(write(QStringLiteral("spacing.md"), text)));
    vn::DocumentTab *tab = window.currentTab();
    tab->setViewMode(vn::DocumentTab::ViewMode::SourceVisual);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    vn::TextView *source = tab->sourceView();
    vn::TextView *visual = tab->visualView();
    focusView(source);
    const auto shot = [](vn::TextView *view) { return view->grab().toImage(); };
    const QImage visualBefore = shot(visual);
    const QImage sourceBefore = shot(source);
    const int visualPage = visual->verticalScrollBar()->pageStep();

    settings.setValue(vn::key::lineSpacing, 2.0);
    QVERIFY(shot(source) != sourceBefore); // The source follows it...
    QVERIFY(shot(visual) == visualBefore); // ...the rendered view does not.
    QCOMPARE(visual->verticalScrollBar()->pageStep(), visualPage);

    settings.setValue(vn::key::textLineHeight, 2.0);
    QVERIFY(shot(visual) != visualBefore);
    QVERIFY(visual->verticalScrollBar()->pageStep() < visualPage);
    settings.setValue(vn::key::lineSpacing, editorSpacing);
    settings.setValue(vn::key::textLineHeight, textSpacing);
}

// Read mode's pages are laid out from the Markdown spacing alone as well.
void TestMainWindow::editorLineSpacingLeavesReadModeAlone()
{
    vn::Settings &settings = vn::Settings::instance();
    const QVariant editorSpacing = settings.value(vn::key::lineSpacing);
    const QVariant textSpacing = settings.value(vn::key::textLineHeight);
    settings.setValue(vn::key::lineSpacing, 1.0);
    settings.setValue(vn::key::textLineHeight, 1.4);
    vn::MainWindow window;
    window.resize(1000, 600);
    vn::DocumentTab *tab = readingTab(window, QStringLiteral("readspacing.md"));
    QVERIFY(tab);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    vn::VisualView *visual = tab->visualView();
    QTRY_VERIFY(visual->showsReadColumns());
    const auto shot = [&] {
        visual->grab();
        return visual->grab().toImage();
    };
    const QImage before = shot();
    settings.setValue(vn::key::lineSpacing, 2.0);
    QVERIFY(shot() == before);
    settings.setValue(vn::key::textLineHeight, 2.0);
    QVERIFY(shot() != before);
    settings.setValue(vn::key::lineSpacing, editorSpacing);
    settings.setValue(vn::key::textLineHeight, textSpacing);
}

// Ctrl+wheel moves the half in use by 5% a notch, wherever the pointer is.
void TestMainWindow::ctrlWheelZoomsTheHalfInUse()
{
    vn::MainWindow window;
    vn::DocumentTab *tab = zoomTab(window, vn::DocumentTab::ViewMode::SourceVisual);
    QVERIFY(tab);
    vn::TextView *source = tab->sourceView();
    vn::TextView *visual = tab->visualView();
    focusView(source);

    // The pointer is over the visual half, but the source one has the line.
    wheel(visual, 120);
    QCOMPARE(tab->zoom(source), 105);
    QCOMPARE(tab->zoom(visual), 100);
    wheel(visual, -120);
    wheel(visual, -120);
    QCOMPARE(tab->zoom(source), 95);
    // Finer wheels add up to whole notches.
    wheel(source, 60);
    QCOMPARE(tab->zoom(source), 95);
    wheel(source, 60);
    QCOMPARE(tab->zoom(source), 100);
    QCOMPARE(tab->zoom(visual), 100);

    focusView(visual);
    wheel(source, 120);
    QCOMPARE(tab->zoom(visual), 105);
    QCOMPARE(tab->zoom(source), 100);

    // Without Ctrl the wheel still scrolls and does not zoom.
    QWheelEvent plain(QPointF(10, 10), QPointF(10, 10), {}, QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(visual->viewport(), &plain);
    QCOMPARE(tab->zoom(visual), 105);
}

// The cell shows the number of the half in use; clicking it opens the popup.
void TestMainWindow::zoomCellAndPopup()
{
    vn::MainWindow window;
    vn::DocumentTab *tab = zoomTab(window, vn::DocumentTab::ViewMode::SourceVisual);
    QVERIFY(tab);
    vn::TextView *source = tab->sourceView();
    vn::TextView *visual = tab->visualView();
    auto *cell = window.findChild<QLabel *>(QStringLiteral("ZoomStatus"));
    QVERIFY(cell);
    focusView(visual);
    tab->setZoom(125);
    QCOMPARE(cell->text(), QStringLiteral("125%"));

    QTest::mouseClick(cell, Qt::LeftButton);
    auto *popup = window.findChild<QFrame *>(QStringLiteral("ZoomPopup"));
    QVERIFY(popup && popup->isVisible());
    // The half's name, then − / slider / + and the value; no reset button.
    auto *name = popup->findChild<QLabel *>(QStringLiteral("ZoomHalf"));
    QVERIFY(name);
    QCOMPARE(name->text(), QStringLiteral("Visual"));
    QCOMPARE(popup->findChild<QLabel *>(QStringLiteral("ZoomValue"))->text(), QStringLiteral("125%"));
    const QList<QToolButton *> buttons = popup->findChildren<QToolButton *>();
    QCOMPARE(buttons.size(), 2);
    QToolButton *minus = buttons.at(0);
    QToolButton *plus = buttons.at(1);
    QCOMPARE(minus->text(), QString(QChar(0x2212)));
    QCOMPARE(plus->text(), QStringLiteral("+"));
    plus->click();
    QCOMPARE(tab->zoom(visual), 130);
    minus->click();
    minus->click();
    QCOMPARE(tab->zoom(visual), 120);
    QCOMPARE(cell->text(), QStringLiteral("120%"));
    QCOMPARE(popup->findChild<QLabel *>(QStringLiteral("ZoomValue"))->text(), QStringLiteral("120%"));

    // Log scale from 30% to 500%: the middle is the geometric mean (122%),
    // and the zoom snaps to a whole step.
    auto *slider = popup->findChild<QSlider *>();
    QVERIFY(slider);
    slider->setValue(slider->minimum());
    QCOMPARE(tab->zoom(visual), 30);
    slider->setValue(slider->maximum());
    QCOMPARE(tab->zoom(visual), 500);
    slider->setValue((slider->minimum() + slider->maximum()) / 2);
    QCOMPARE(tab->zoom(visual), 120);
    QCOMPARE(tab->zoom(source), 100);
    // The handle sits on the step it snapped to.
    const int snapped = slider->value();
    slider->setValue(snapped + 1);
    QCOMPARE(tab->zoom(visual), 120);
    QVERIFY(slider->value() == snapped);
    // Moving the zoom elsewhere moves the slider.
    tab->setZoom(300);
    QVERIFY(slider->value() > snapped);
    QCOMPARE(popup->findChild<QLabel *>(QStringLiteral("ZoomValue"))->text(), QStringLiteral("300%"));

    popup->close();
    QTRY_VERIFY(!window.findChild<QFrame *>(QStringLiteral("ZoomPopup")));

    // Opened with the source half in use, it is about that one.
    window.activateWindow();
    focusView(source);
    QCOMPARE(cell->text(), QStringLiteral("100%"));
    QTest::mouseClick(cell, Qt::LeftButton);
    popup = window.findChild<QFrame *>(QStringLiteral("ZoomPopup"));
    QVERIFY(popup);
    QCOMPARE(popup->findChild<QLabel *>(QStringLiteral("ZoomHalf"))->text(), QStringLiteral("Source"));
    popup->close();
}

// With one view the cell says "Zoom" and the popup has no half's name.
void TestMainWindow::zoomPopupInSingleView()
{
    for (const auto mode : {vn::DocumentTab::ViewMode::Source, vn::DocumentTab::ViewMode::Visual}) {
        vn::MainWindow window;
        vn::DocumentTab *tab = zoomTab(window, mode);
        QVERIFY(tab);
        auto *cell = window.findChild<QLabel *>(QStringLiteral("ZoomStatus"));
        QVERIFY(cell);
        QCOMPARE(cell->toolTip(), QStringLiteral("Zoom"));
        actionFor(window, QStringLiteral("view.zoomIn"))->trigger();
        QCOMPARE(tab->zoom(), 105);
        QCOMPARE(cell->text(), QStringLiteral("105%"));
        QTest::mouseClick(cell, Qt::LeftButton);
        auto *popup = window.findChild<QFrame *>(QStringLiteral("ZoomPopup"));
        QVERIFY(popup);
        QVERIFY(!popup->findChild<QLabel *>(QStringLiteral("ZoomHalf")));
        QCOMPARE(popup->findChild<QLabel *>(QStringLiteral("ZoomValue"))->text(), QStringLiteral("105%"));
        popup->close();
    }
    // A file that is not Markdown has the one view too.
    vn::MainWindow window;
    QVERIFY(window.openFile(write(QStringLiteral("zoom.txt"), "plain\n")));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("ZoomStatus"))->toolTip(), QStringLiteral("Zoom"));
}

// A tab starts at the "default zoom level" preference, whichever window.
void TestMainWindow::newTabStartsAtDefaultZoom()
{
    vn::Settings::instance().setValue(vn::key::zoom, 125);
    vn::MainWindow window;
    const QString first = write(QStringLiteral("d1.md"), "# One\n");
    const QString second = write(QStringLiteral("d2.md"), "# Two\n");
    QVERIFY(window.openFile(first));
    vn::DocumentTab *a = window.currentTab();
    QCOMPARE(a->zoom(a->sourceView()), 125);
    QCOMPARE(a->zoom(a->visualView() ? static_cast<vn::TextView *>(a->visualView()) : a->sourceView()), 125);
    a->setZoom(150);
    // Opening another tab does not move this one.
    QVERIFY(window.openFile(second));
    vn::DocumentTab *b = window.currentTab();
    QCOMPARE(b->zoom(), 125);
    QCOMPARE(a->zoom(), 150);
    vn::Settings::instance().reset(vn::key::zoom);
}

// A tab moved or copied to another window keeps its zoom, and does not
// change the zoom of the tabs already there.
// Collapse all folds blocks only; Expand all opens blocks and sections. Both
// need Markdown shown rendered.
void TestMainWindow::collapseCommands()
{
    for (const char *id : {"view.collapseAll", "view.expandAll"})
        QVERIFY(vn::command(QLatin1String(id)));
    QCOMPARE(vn::command(QStringLiteral("view.collapseAll"))->defaults,
             QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+K, Ctrl+0"), QKeySequence::PortableText)});
    QCOMPARE(vn::command(QStringLiteral("view.expandAll"))->defaults,
             QList<QKeySequence>{QKeySequence(QStringLiteral("Ctrl+K, Ctrl+J"), QKeySequence::PortableText)});

    vn::MainWindow window;
    const QString path = write(QStringLiteral("collapse.md"),
                               "# One\n\nText.\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n```\ncode\n```\n\n## Two\n\nMore.\n");
    window.resize(900, 600);
    QVERIFY(window.openFile(path));
    vn::DocumentTab *tab = window.currentTab();
    tab->setViewMode(vn::DocumentTab::ViewMode::Source);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.activateWindow();
    QAction *collapse = actionFor(window, QStringLiteral("view.collapseAll"));
    QAction *expand = actionFor(window, QStringLiteral("view.expandAll"));
    QVERIFY(collapse && expand);
    // Only the source is shown: nothing to collapse.
    QVERIFY(!collapse->isEnabled());
    QVERIFY(!expand->isEnabled());
    tab->setViewMode(vn::DocumentTab::ViewMode::Visual);
    QTRY_VERIFY(collapse->isEnabled());
    QVERIFY(expand->isEnabled());
    vn::VisualView *visual = tab->visualView();
    QVERIFY(visual);
    // A section folded by hand stays folded by Collapse all; the two blocks fold.
    QTRY_VERIFY(visual->setFolded(12, true));
    collapse->trigger();
    QCOMPARE(visual->foldCount(), 3);
    QVERIFY(visual->isFolded(12));
    QVERIFY(visual->isFolded(4));
    QVERIFY(visual->isFolded(8));
    QVERIFY(!visual->isFolded(0));
    // Expand all opens the blocks and the section.
    expand->trigger();
    QCOMPARE(visual->foldCount(), 0);
    // A file that is not Markdown has none.
    const QString text = write(QStringLiteral("plain.txt"), "text\n");
    QVERIFY(window.openFile(text));
    QVERIFY(!collapse->isEnabled());
}

void TestMainWindow::adoptedTabKeepsItsZoom()
{
    vn::MainWindow one;
    vn::DocumentTab *tab = zoomTab(one, vn::DocumentTab::ViewMode::SourceVisual);
    QVERIFY(tab);
    focusView(tab->sourceView());
    tab->setZoom(150);
    focusView(tab->visualView());
    tab->setZoom(75);

    vn::MainWindow two;
    two.resize(900, 500);
    QVERIFY(two.openFile(write(QStringLiteral("zoomother.txt"), "other\n")));
    vn::DocumentTab *resident = two.currentTab();
    resident->setZoom(110);

    vn::DocumentTab *copy = tab->openAnotherView();
    QCOMPARE(copy->zoom(copy->sourceView()), 150);
    QCOMPARE(copy->zoom(copy->visualView()), 75);
    two.adoptTab(copy);
    QCOMPARE(copy->zoom(copy->sourceView()), 150);
    QCOMPARE(copy->zoom(copy->visualView()), 75);
    QCOMPARE(resident->zoom(), 110);
    two.show();
    QVERIFY(QTest::qWaitForWindowExposed(&two));
    // The fonts follow once it is shown.
    QCOMPARE(copy->sourceView()->font().pointSizeF(), vn::theme::editorFont().pointSizeF() * 1.5);
    QCOMPARE(copy->visualView()->font().pointSizeF(), vn::theme::editorFont().pointSizeF() * 0.75);
    QCOMPARE(tab->zoom(tab->sourceView()), 150);
}

vn::DocumentTab *TestMainWindow::readingTab(vn::MainWindow &window, const QString &name)
{
    QByteArray text = "# Reading\n\n";
    for (int i = 1; i <= 120; ++i)
        text += QByteArray("Paragraph ") + QByteArray::number(i)
                + " has enough words in it to wrap over a line or two in a column of seventy characters.\n\n";
    if (!window.openFile(write(name, text)))
        return nullptr;
    vn::DocumentTab *tab = window.currentTab();
    tab->setViewMode(vn::DocumentTab::ViewMode::Visual);
    setLocked(tab, true);
    tab->setColumns(true);
    return tab;
}

void TestMainWindow::setLocked(vn::DocumentTab *tab, bool locked)
{
    for (QToolButton *button : tab->findChildren<QToolButton *>()) {
        if (button->toolTip() == (locked ? QStringLiteral("Read-only") : QStringLiteral("Editing")))
            button->click();
    }
}

// The status bar in read mode: the view switch, the zoom, the lines on
// screen and the language; nothing about editing the file. Alt+PgDn turns a
// page.
void TestMainWindow::readModeStatusAndPages()
{
    for (const char *k : {vn::key::readColumns, vn::key::readChars, vn::key::readKeptLines, vn::key::zoom})
        vn::Settings::instance().reset(k);
    vn::MainWindow window;
    window.resize(1000, 600);
    vn::DocumentTab *tab = readingTab(window, QStringLiteral("status.md"));
    QVERIFY(tab);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.activateWindow();
    QVERIFY(tab->isReadMode());
    vn::VisualView *visual = tab->visualView();
    QTRY_VERIFY(visual->showsReadColumns());
    visual->grab();
    auto *position = window.findChild<QLabel *>(QStringLiteral("RangeStatus"));
    QVERIFY(position);
    QVERIFY(!window.findChild<QLabel *>(QStringLiteral("PositionStatus"))->isVisible());
    const auto lines = [tab] {
        const auto [first, last] = tab->readLineRange();
        return QStringLiteral("Lines %1–%2 of %3").arg(first + 1).arg(last + 1).arg(tab->document()->blockCount());
    };
    QCOMPARE(tab->readLineRange().first, 0);
    QTRY_COMPARE(position->text(), lines());
    for (const char *name : {"IndentationStatus", "LineEndingStatus", "EncodingStatus"})
        QVERIFY2(!window.findChild<QLabel *>(QLatin1String(name))->isVisible(), name);
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("ZoomStatus"))->isVisible());
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("ZoomStatus"))->toolTip(), QStringLiteral("Zoom"));
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("LanguageStatus"))->isVisible());
    // (The cell grows to the text once the status bar is laid out again.)
    QTRY_VERIFY(position->width() >= position->sizeHint().width());
    if (const QString file = qEnvironmentVariable("VN_SCREENSHOT"); !file.isEmpty())
        window.grab().save(QString(file).replace(QStringLiteral(".png"), QStringLiteral("-read.png")));

    // The command (Alt+PgDn) turns a page, and the status follows.
    focusView(visual);
    const int before = tab->readLineRange().first;
    actionFor(window, QStringLiteral("view.scrollPageDown"))->trigger();
    const int after = tab->readLineRange().first;
    QVERIFY(after > before);
    QTRY_COMPARE(position->text(), lines());
    actionFor(window, QStringLiteral("view.scrollPageUp"))->trigger();
    QCOMPARE(tab->readLineRange().first, before);
    QTest::keyClick(visual, Qt::Key_PageDown, Qt::AltModifier);
    QCOMPARE(tab->readLineRange().first, after);
    // The zoom cell works as in a single view, and zooming keeps the place.
    actionFor(window, QStringLiteral("view.zoomIn"))->trigger();
    QCOMPARE(tab->zoom(), 105);
    visual->grab();
    QCOMPARE(tab->readLineRange().first, after);
    QTRY_COMPARE(position->text(), lines());
    // The hidden source view keeps the place too (a saved session has it).
    QTRY_COMPARE(tab->sourceView()->topLine(), after);

    // Go to line and the outline turn to the line's screen.
    actionFor(window, QStringLiteral("edit.goToLine"))->trigger();
    auto *field = tab->findChild<QLineEdit *>(QStringLiteral("GoToLineEdit"));
    QVERIFY(field && field->isVisible());
    field->setText(QStringLiteral("201")); // Paragraph 100.
    QTest::keyClick(field, Qt::Key_Return);
    QCOMPARE(tab->readLineRange().first, 200);
    QCOMPARE(tab->visualView()->textCursor().blockNumber(), 200);
    auto *outline = window.findChild<QTreeWidget *>();
    QVERIFY(outline);
    QTRY_VERIFY(outline->topLevelItemCount() > 0);
    Q_EMIT outline->itemActivated(outline->topLevelItem(0), 0);
    QCOMPARE(tab->readLineRange().first, 0);
    tab->visualView()->readPageForward();
    QCOMPARE(tab->readLineRange().first, after);

    // Unlocked: editing again, and every cell back.
    setLocked(tab, false);
    QVERIFY(!tab->isReadMode());
    QVERIFY(!visual->showsReadColumns());
    QTRY_VERIFY(!position->isVisible());
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("PositionStatus"))->text().startsWith(QLatin1String("Ln ")));
    for (const char *name : {"IndentationStatus", "LineEndingStatus", "EncodingStatus"})
        QVERIFY2(window.findChild<QLabel *>(QLatin1String(name))->isVisible(), name);
    vn::Settings::instance().reset(vn::key::zoom);
}

// The bar over a tab in read mode changes that tab only, and nothing is
// saved; Preferences give where a new tab starts.
void TestMainWindow::readPopupIsPerTabAndStartsFromPreferences()
{
    vn::Settings &settings = vn::Settings::instance();
    settings.setValue(vn::key::readChars, 60);
    settings.setValue(vn::key::readColumns, 1);
    const auto restore = qScopeGuard([&settings] {
        settings.reset(vn::key::readChars);
        settings.reset(vn::key::readColumns);
    });
    vn::MainWindow window;
    window.resize(1600, 600);
    vn::DocumentTab *first = readingTab(window, QStringLiteral("bar1.md"));
    vn::DocumentTab *second = readingTab(window, QStringLiteral("bar2.md"));
    QVERIFY(first && second);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCOMPARE(window.currentTab(), second);
    for (vn::DocumentTab *tab : {first, second}) {
        QCOMPARE(tab->readChars(), 60);
        QCOMPARE(tab->readColumns(), 1);
    }
    second->visualView()->grab();
    viewButton(window, true)->click();
    auto *bar = window.findChild<QFrame *>(QStringLiteral("ReadPopup"));
    QVERIFY(bar && bar->isVisible());
    auto *slider = bar->findChild<QSlider *>(QStringLiteral("ReadCharsSlider"));
    auto *count = bar->findChild<QLabel *>(QStringLiteral("ReadColumnCount"));
    auto *fewer = readControl(bar, "ReadFewerColumns");
    auto *more = readControl(bar, "ReadMoreColumns");
    auto *narrower = readControl(bar, "ReadNarrower");
    auto *wider = readControl(bar, "ReadWider");
    QVERIFY(slider && count && fewer && more && narrower && wider);
    wider->click();
    QCOMPARE(slider->value(), 65);
    narrower->click();
    QCOMPARE(slider->value(), 60);
    slider->setValue(50);
    QVERIFY(!narrower->isEnabled());
    slider->setValue(100);
    QVERIFY(!wider->isEnabled());
    slider->setValue(60);
    QCOMPARE(slider->value(), 60);
    QCOMPARE(bar->findChild<QLabel *>(QStringLiteral("ReadCharsValue"))->text(), QStringLiteral("60"));
    QCOMPARE(count->text(), QStringLiteral("1"));
    QVERIFY(!fewer->isEnabled());
    // Above the button, right edges lined up, as far as the screen allows.
    QWidget *button = viewButton(window, true);
    QRect expected(button->mapToGlobal(QPoint(button->width(), 0)) - QPoint(bar->width(), bar->height()), bar->size());
    const QRect available = button->screen()->availableGeometry();
    expected.moveLeft(qBound(available.left(), expected.left(), available.right() + 1 - expected.width()));
    expected.moveTop(qBound(available.top(), expected.top(), available.bottom() + 1 - expected.height()));
    const QPoint offset = bar->pos() - expected.topLeft();
    QVERIFY(qAbs(offset.x()) <= 2 && qAbs(offset.y()) <= 2);

    slider->setValue(72);
    QCOMPARE(slider->value(), 70);
    QCOMPARE(second->readChars(), 70);
    QCOMPARE(second->visualView()->readChars(), 70);
    const int fit = second->columnsFit();
    QVERIFY(fit >= 2);
    for (int next = 2; next <= fit; ++next) {
        QVERIFY(more->isEnabled());
        more->click();
        QCOMPARE(second->readColumns(), next);
    }
    QVERIFY(!more->isEnabled());
    QCOMPARE(second->columnsShownCount(), fit);
    QCOMPARE(count->text(), QString::number(fit));
    QVERIFY(fewer->isEnabled());
    fewer->click();
    QCOMPARE(second->readColumns(), fit - 1);
    QCOMPARE(count->text(), QString::number(fit - 1));

    // Escape closes the popup, and clicking Columns again opens it.
    QTest::keyClick(bar, Qt::Key_Escape);
    QTRY_VERIFY(!window.findChild<QFrame *>(QStringLiteral("ReadPopup")));
    viewButton(window, true)->click();
    bar = window.findChild<QFrame *>(QStringLiteral("ReadPopup"));
    QVERIFY(bar && bar->isVisible());
    bar->close();

    // The other tab and the preferences are as they were.
    QCOMPARE(first->readChars(), 60);
    QCOMPARE(first->readColumns(), 1);
    QCOMPARE(settings.number(vn::key::readChars), 60);
    QCOMPARE(settings.number(vn::key::readColumns), 1);
    // A copy of the tab takes its values; a new tab the preferences'.
    std::unique_ptr<vn::DocumentTab> copy(second->openAnotherView());
    QCOMPARE(copy->readChars(), 70);
    QCOMPARE(copy->readColumns(), fit - 1);
    settings.setValue(vn::key::readChars, 85);
    QCOMPARE(second->readChars(), 70);
    vn::DocumentTab *third = readingTab(window, QStringLiteral("bar3.md"));
    QVERIFY(third);
    QCOMPARE(third->readChars(), 85);

}

void TestMainWindow::readPopupNarrowsAndRestoresWidth()
{
    vn::MainWindow window;
    window.resize(1200, 600);
    auto *tab = readingTab(window, QStringLiteral("width.md"));
    QVERIFY(tab);
    tab->setReadColumns(1);
    tab->setReadChars(85);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *view = tab->visualView();
    QFont body = vn::theme::textFont();
    body.setPointSizeF(body.pointSizeF() * view->font().pointSizeF() / vn::theme::editorFont().pointSizeF());
    const qreal charWidth = QFontMetricsF(body).averageCharWidth();
    // Leave room for two 72-character columns, which puts the slider's end at 70.
    const auto resizeFor = [&](int chars) {
        const int viewportWidth = 2 * 56 + 40 + 2 * qRound(chars * charWidth);
        window.resize(window.width() + viewportWidth - view->viewport()->width(), window.height());
    };
    resizeFor(72);
    QTRY_COMPARE(tab->columnsFit(), 2);
    viewButton(window, true)->click();
    auto *popup = window.findChild<QFrame *>(QStringLiteral("ReadPopup"));
    QVERIFY(popup && popup->isVisible());
    auto *more = readControl(popup, "ReadMoreColumns");
    auto *fewer = readControl(popup, "ReadFewerColumns");
    auto *wider = readControl(popup, "ReadWider");
    auto *slider = popup->findChild<QSlider *>(QStringLiteral("ReadCharsSlider"));
    auto *count = popup->findChild<QLabel *>(QStringLiteral("ReadColumnCount"));
    auto *value = popup->findChild<QLabel *>(QStringLiteral("ReadCharsValue"));
    QVERIFY(more && fewer && wider && slider && count && value);
    QCOMPARE(count->text(), QStringLiteral("1"));
    QCOMPARE(count->geometry().center().x(), slider->geometry().center().x());
    QCOMPARE(value->width(), value->fontMetrics().horizontalAdvance(QStringLiteral("100")));
    QCOMPARE(slider->maximum(), 100);
    QCOMPARE(slider->value(), 85);

    QVERIFY(more->isEnabled());
    more->click();
    QCOMPARE(count->text(), QStringLiteral("2"));
    QCOMPARE(tab->readColumns(), 2);
    QCOMPARE(tab->readChars(), 85);
    QCOMPARE(slider->minimum(), 50);
    QCOMPARE(slider->maximum(), 70);
    QCOMPARE(slider->sliderPosition(), 70);
    QCOMPARE(value->text(), QStringLiteral("70"));
    QVERIFY(!wider->isEnabled());
    QVERIFY(!more->isEnabled());

    fewer->click();
    QCOMPARE(count->text(), QStringLiteral("1"));
    QCOMPARE(slider->maximum(), 100);
    QCOMPARE(slider->sliderPosition(), 85);
    QCOMPARE(value->text(), QStringLiteral("85"));

    // The window loses room for two even at 50, then gets it back.
    resizeFor(49);
    QTRY_VERIFY(!more->isEnabled());
    QCOMPARE(count->text(), QStringLiteral("1"));
    QCOMPARE(tab->readChars(), 85);
    resizeFor(72);
    QTRY_VERIFY(more->isEnabled());
    more->click();

    QStyleOptionSlider option;
    option.initFrom(slider);
    option.orientation = Qt::Horizontal;
    option.minimum = slider->minimum();
    option.maximum = slider->maximum();
    option.sliderPosition = slider->sliderPosition();
    option.sliderValue = slider->value();
    const QPoint handle = slider->style()
                              ->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, slider)
                              .center();
    const QPoint left(0, handle.y());
    QTest::mousePress(slider, Qt::LeftButton, {}, handle);
    QTest::mouseMove(slider, left);
    QTest::mouseRelease(slider, Qt::LeftButton, {}, left);
    QCOMPARE(tab->readChars(), 50);
    fewer->click();
    QCOMPARE(slider->value(), 50);
}

void TestMainWindow::readPopupFollowsColumnsButtonWhenStatusChanges()
{
    vn::MainWindow window;
    const QRect screen = window.screen()->availableGeometry();
    window.resize(qMin(screen.width(), 1000), 600);
    window.move(screen.topLeft());
    auto *tab = readingTab(window, QStringLiteral("placement.md"));
    QVERIFY(tab);
    tab->setColumns(false);
    setLocked(tab, false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();
    auto *range = window.findChild<QLabel *>(QStringLiteral("RangeStatus"));
    QVERIFY(range && !range->isVisible());

    auto *button = viewButton(window, true);
    const auto rightEdge = [](QWidget *widget) { return widget->mapToGlobal(QPoint(widget->width(), 0)).x(); };
    const int before = rightEdge(button);
    button->click();
    auto *popup = window.findChild<QFrame *>(QStringLiteral("ReadPopup"));
    QVERIFY(popup && popup->isVisible());
    QCoreApplication::processEvents();
    QVERIFY(range->isVisible());
    QVERIFY(range->text().startsWith(QStringLiteral("Lines ")));
    QVERIFY(qAbs(rightEdge(button) - before) > 2);
    QCOMPARE_LE(qAbs(rightEdge(popup) - rightEdge(button)), 2);
}

void TestMainWindow::columnsResolvePreferenceOnce_data()
{
    QTest::addColumn<bool>("visual");
    QTest::newRow("visual") << true;
    QTest::newRow("source") << false;
}

void TestMainWindow::columnsResolvePreferenceOnce()
{
    QFETCH(bool, visual);
    auto &settings = vn::Settings::instance();
    settings.setValue(vn::key::readColumns, 0);
    settings.setValue(vn::key::readChars, 85);
    const auto restore = qScopeGuard([&] {
        settings.reset(vn::key::readColumns);
        settings.reset(vn::key::readChars);
    });
    vn::MainWindow window;
    window.resize(2000, 600);
    auto *tab = visual ? readingTab(window, QStringLiteral("initial.md"))
                       : codeTab(window, QStringLiteral("initial.py"), 212);
    QVERIFY(tab);
    tab->setColumns(true);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY(tab->readColumns() >= 2);
    const int initial = tab->readColumns();
    QCOMPARE(tab->columnsShownCount(), initial);
    QCOMPARE(tab->columnsChars().first, 85);
    const int width = visual ? tab->visualView()->readColumnRect(0).width()
                             : tab->sourceView()->columnRect(0).width();
    const int viewport = tab->editor()->viewport()->width();
    QCOMPARE(initial, (viewport - 2 * 56 + 40) / (width + 40));

    window.resize(3200, 600);
    QTRY_VERIFY(tab->columnsFit() > initial);
    QCOMPARE(tab->readColumns(), initial);
    QCOMPARE(tab->columnsShownCount(), initial);
    window.resize(600, 600);
    QTRY_VERIFY(tab->columnsShownCount() < initial);
    QCOMPARE(tab->readColumns(), initial);
    QCOMPARE(tab->readChars(), 85);
    window.resize(2000, 600);
    QTRY_COMPARE(tab->columnsShownCount(), initial);
    QCOMPARE(tab->columnsChars().first, 85);
}

QToolButton *TestMainWindow::viewButton(vn::MainWindow &window, bool columns)
{
    const QString name = columns ? QStringLiteral("ViewColumns") : QStringLiteral("ViewScroll");
    for (QToolButton *button : window.findChildren<QToolButton *>()) {
        if (button->property("viewSwitch").toString() == name)
            return button;
    }
    return nullptr;
}

QStringList TestMainWindow::shownCells(vn::MainWindow &window)
{
    QList<QWidget *> cells;
    for (QWidget *cell : window.statusBar()->findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) {
        if (cell->isVisible() && !cell->objectName().isEmpty() && cell->objectName() != QLatin1String("qt_rubberband"))
            cells << cell;
    }
    std::sort(cells.begin(), cells.end(), [](QWidget *a, QWidget *b) { return a->x() < b->x(); });
    QStringList names;
    for (QWidget *cell : cells)
        names << cell->objectName();
    return names;
}

vn::DocumentTab *TestMainWindow::codeTab(vn::MainWindow &window, const QString &name, int lines)
{
    QByteArray text;
    for (int i = 1; i <= lines; ++i)
        text += "def function_" + QByteArray::number(i) + "(value):\n";
    if (!window.openFile(write(name, text)))
        return nullptr;
    return window.currentTab();
}

// Scroll | Columns is the first cell of the status bar's right-aligned row,
// the zoom next to it, in every view; where columns cannot show (the Visual
// Editor unlocked, Source / Visual) it is hidden and the zoom comes first.
void TestMainWindow::viewSwitchComesFirstAndHides()
{
    vn::MainWindow window;
    window.resize(1200, 600);
    vn::DocumentTab *code = codeTab(window, QStringLiteral("switch.py"), 10);
    QVERIFY(code);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QToolButton *scroll = viewButton(window, false);
    QToolButton *columns = viewButton(window, true);
    QVERIFY(scroll && columns);
    QTRY_COMPARE(shownCells(window).mid(0, 3), (QStringList{QStringLiteral("ViewSwitch"), QStringLiteral("ZoomStatus"),
                                                           QStringLiteral("PositionStatus")}));
    QVERIFY(scroll->isChecked() && !columns->isChecked());
    QVERIFY(!code->showsColumns());
    QCOMPARE(scroll->toolTip(), QStringLiteral("Scroll"));
    QCOMPARE(columns->toolTip(), QStringLiteral("Columns"));
    if (const QString file = qEnvironmentVariable("VN_SCREENSHOT"); !file.isEmpty())
        window.grab().save(QString(file).replace(QStringLiteral(".png"), QStringLiteral("-switch.png")));

    // Markdown: the Visual Editor shows it, editing too: Columns there locks it.
    QVERIFY(window.openFile(write(QStringLiteral("switch.md"), "# Title\n\nText.\n")));
    vn::DocumentTab *md = window.currentTab();
    md->setViewMode(vn::DocumentTab::ViewMode::Visual);
    setLocked(md, false);
    QTRY_COMPARE(shownCells(window).value(0), QStringLiteral("ViewSwitch"));
    QVERIFY(md->canShowColumns());
    QVERIFY(scroll->isChecked() && !md->showsColumns());
    columns->click();
    QTRY_VERIFY(md->isReadMode());
    QVERIFY(md->isReadOnly());
    QVERIFY(columns->isChecked());
    // Unlocked again: Scroll, editing.
    setLocked(md, false);
    QTRY_VERIFY(!md->showsColumns());
    QVERIFY(scroll->isChecked());
    md->setColumns(false);
    // Source / Visual hides it.
    md->setViewMode(vn::DocumentTab::ViewMode::SourceVisual);
    QTRY_COMPARE(shownCells(window).value(0), QStringLiteral("ZoomStatus"));
    // Source, and the Visual Editor locked, show it.
    md->setViewMode(vn::DocumentTab::ViewMode::Source);
    QTRY_COMPARE(shownCells(window).value(0), QStringLiteral("ViewSwitch"));
    md->setViewMode(vn::DocumentTab::ViewMode::Visual);
    setLocked(md, true);
    QTRY_COMPARE(shownCells(window).value(0), QStringLiteral("ViewSwitch"));
    QVERIFY(scroll->isChecked());
    // Another tab shows its own.
    window.mainGroup()->setCurrentIndex(0);
    QTRY_VERIFY(scroll->isChecked());
}

// View > Columns (checkable, no shortcut, between Word Wrap and Status
// Bar) does what the switch does, and shows what it shows.
void TestMainWindow::columnsMenuItemMirrorsTheSwitch()
{
    vn::MainWindow window;
    window.resize(1200, 600);
    vn::DocumentTab *tab = codeTab(window, QStringLiteral("menu.py"), 200);
    QVERIFY(tab);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QAction *item = actionFor(window, QStringLiteral("view.columns"));
    QVERIFY(item);
    QVERIFY(item->isCheckable());
    QVERIFY(item->shortcuts().isEmpty());
    QCOMPARE(item->text(), QStringLiteral("Columns"));
    QMenu *view = nullptr;
    for (QAction *menu : window.menuBar()->actions()) {
        if (menu->menu() && menu->menu()->actions().contains(item))
            view = menu->menu();
    }
    QVERIFY(view);
    const QList<QAction *> items = view->actions();
    const qsizetype at = items.indexOf(item);
    QCOMPARE(items.value(at - 1), actionFor(window, QStringLiteral("view.wordWrap")));
    QCOMPARE(items.value(at + 1), actionFor(window, QStringLiteral("view.statusBar")));
    QVERIFY(item->isEnabled() && !item->isChecked());

    item->trigger();
    QVERIFY(!window.findChild<QFrame *>(QStringLiteral("ReadPopup")));
    QVERIFY(tab->showsColumns());
    QVERIFY(tab->sourceView()->columnsShown());
    QVERIFY(viewButton(window, true)->isChecked());
    QVERIFY(!viewButton(window, false)->isChecked());
    QVERIFY(item->isChecked());
    viewButton(window, false)->click();
    QVERIFY(!tab->showsColumns());
    QVERIFY(!item->isChecked());
    viewButton(window, true)->click();
    auto *popup = window.findChild<QFrame *>(QStringLiteral("ReadPopup"));
    QVERIFY(popup && popup->isVisible());
    popup->close();
    QVERIFY(item->isChecked());
    item->trigger();
    QVERIFY(!tab->showsColumns());
    // Where columns cannot show, it is greyed (and unchecked: the view scrolls).
    QVERIFY(window.openFile(write(QStringLiteral("menu.md"), "# Title\n\nText.\n")));
    vn::DocumentTab *md = window.currentTab();
    md->setViewMode(vn::DocumentTab::ViewMode::SourceVisual);
    QTRY_VERIFY(!item->isEnabled());
    QVERIFY(!item->isChecked());
}

// Every tab opens in Scroll; the choice is the tab's and is saved nowhere.
void TestMainWindow::columnsArePerTabAndNotSaved()
{
    vn::MainWindow window;
    window.resize(1200, 600);
    vn::DocumentTab *first = codeTab(window, QStringLiteral("one.py"), 100);
    vn::DocumentTab *second = codeTab(window, QStringLiteral("two.py"), 100);
    QVERIFY(first && second);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(!first->wantsColumns() && !second->wantsColumns());
    QSignalSpy settingsChanged(&vn::Settings::instance(), &vn::Settings::changed);
    second->setColumns(true);
    QVERIFY(second->showsColumns());
    QVERIFY(!first->showsColumns());
    window.mainGroup()->setCurrentIndex(0);
    QTRY_VERIFY(viewButton(window, false)->isChecked());
    window.mainGroup()->setCurrentIndex(1);
    QTRY_VERIFY(viewButton(window, true)->isChecked());
    // Nothing went to the settings; a file opened again opens in Scroll.
    QVERIFY(settingsChanged.isEmpty());
    const QString path = second->path();
    QVERIFY(window.closeTabs(1, vn::MainWindow::CloseScope::This));
    QTRY_COMPARE(window.tabCount(), 1);
    QVERIFY(window.openFile(path));
    QVERIFY(!window.currentTab()->wantsColumns());
    QVERIFY(!window.currentTab()->showsColumns());
}

// The lock is read-only only: a locked Visual Editor scrolls until Columns
// is chosen. Unlocking drops to Scroll with the same line at the top and
// the caret at its start; locking again shows columns from the caret's line
// (from the top line, if the caret is off screen).
void TestMainWindow::lockNoLongerForcesColumns()
{
    vn::MainWindow window;
    window.resize(1000, 600);
    QByteArray text = "# Reading\n\n";
    for (int i = 1; i <= 120; ++i)
        text += QByteArray("Paragraph ") + QByteArray::number(i) + " has a few words in it.\n\n";
    QVERIFY(window.openFile(write(QStringLiteral("lock.md"), text)));
    vn::DocumentTab *tab = window.currentTab();
    tab->setViewMode(vn::DocumentTab::ViewMode::Visual);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    vn::VisualView *visual = tab->visualView();
    setLocked(tab, true);
    QVERIFY(tab->isReadOnly());
    QVERIFY(!tab->isReadMode());
    QVERIFY(!visual->showsReadColumns());
    QVERIFY(visual->verticalScrollBar()->maximum() > 0); // It scrolls.
    QTRY_VERIFY(viewButton(window, false)->isChecked());

    tab->setColumns(true);
    QVERIFY(tab->isReadMode());
    QTRY_VERIFY(visual->showsReadColumns());
    visual->grab();
    visual->readPageForward();
    const int top = tab->readLineRange().first;
    QVERIFY(top > 0);
    if (const QString file = qEnvironmentVariable("VN_SCREENSHOT"); !file.isEmpty())
        window.grab().save(QString(file).replace(QStringLiteral(".png"), QStringLiteral("-locked-columns.png")));

    setLocked(tab, false);
    QVERIFY(!tab->isReadMode());
    QVERIFY(tab->wantsColumns());
    QCOMPARE(visual->topLine(), top);
    QCOMPARE(visual->textCursor().blockNumber(), top);
    QCOMPARE(visual->textCursor().positionInBlock(), 0);
    // The switch stays, showing Scroll (Columns would lock it again).
    QTRY_VERIFY(viewButton(window, false)->isChecked());

    // Locked again: from the caret's line, which is on screen.
    QTextCursor caret(tab->document()->findBlockByNumber(top + 4));
    visual->setTextCursor(caret);
    QVERIFY(visual->isLineShown(top + 4));
    setLocked(tab, true);
    QVERIFY(tab->isReadMode());
    visual->grab();
    QCOMPARE(tab->readLineRange().first, top + 4);

    // The caret off screen: from the top line.
    setLocked(tab, false);
    visual->setTextCursor(QTextCursor(tab->document()->findBlockByNumber(230)));
    visual->scrollToLine(100);
    visual->grab();
    QVERIFY(!visual->isLineShown(230));
    const int shownTop = visual->topLine();
    setLocked(tab, true);
    visual->grab();
    QCOMPARE(tab->readLineRange().first, shownTop);

    // Scroll chosen while locked: it stays read-only, and scrolls.
    viewButton(window, false)->click();
    QVERIFY(!tab->isReadMode());
    QVERIFY(tab->isReadOnly());
    QVERIFY(!visual->showsReadColumns());
}

// Source columns go when the split is chosen (the same line at the top, the
// caret at its start) and come back, from the caret's line, in Source; from
// Source to the locked Visual Editor they stay columns at the same place.
void TestMainWindow::sourceColumnsLeaveForTheSplitAndComeBack()
{
    vn::MainWindow window;
    window.resize(1200, 600);
    QByteArray text = "# Notes\n\n";
    for (int i = 1; i <= 200; ++i)
        text += QByteArray("Paragraph ") + QByteArray::number(i) + " of the notes.\n\n"; // On even lines.
    QVERIFY(window.openFile(write(QStringLiteral("split.md"), text)));
    vn::DocumentTab *tab = window.currentTab();
    tab->setViewMode(vn::DocumentTab::ViewMode::Source);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    vn::TextView *source = tab->sourceView();
    tab->setColumns(true);
    QVERIFY(source->columnsShown());
    source->grab();
    source->columnsPageForward();
    const int top = tab->readLineRange().first;
    QVERIFY(top > 0);
    QVERIFY(!tab->isReadOnly()); // (It edits in columns.)

    tab->setViewMode(vn::DocumentTab::ViewMode::SourceVisual);
    QVERIFY(!tab->showsColumns());
    QVERIFY(!source->columnsShown());
    QCOMPARE(source->topLine(), top);
    QCOMPARE(tab->editor()->textCursor().blockNumber(), top);
    QCOMPARE(tab->editor()->textCursor().positionInBlock(), 0);
    QVERIFY(!tab->isReadOnly());

    source->setFocus();
    const int caret = (top + 4) & ~1; // A paragraph's line, a few lines down.
    source->setTextCursor(QTextCursor(tab->document()->findBlockByNumber(caret)));
    QVERIFY(source->isLineShown(caret));
    tab->setViewMode(vn::DocumentTab::ViewMode::Source);
    QVERIFY(source->columnsShown());
    source->grab();
    QCOMPARE(tab->readLineRange().first, caret);

    // To the locked Visual Editor: still columns (read mode), at the same place.
    tab->setViewMode(vn::DocumentTab::ViewMode::Visual);
    setLocked(tab, true);
    tab->setViewMode(vn::DocumentTab::ViewMode::Source);
    source->grab();
    QCOMPARE(tab->readLineRange().first, caret);
    tab->setViewMode(vn::DocumentTab::ViewMode::Visual);
    QVERIFY(tab->isReadMode());
    tab->visualView()->grab();
    QCOMPARE(tab->readLineRange().first, caret);
}

// A code file in columns: the status bar has the switch, the zoom, the lines
// on screen, then the caret and the file's cells; the bar at the top right
// (no pill: code has none). Typing waits until editing in columns is built.
void TestMainWindow::sourceColumnsStatusBarAndEditing()
{
    vn::MainWindow window;
    window.resize(1200, 600);
    vn::DocumentTab *tab = codeTab(window, QStringLiteral("build.py"), 212);
    QVERIFY(tab);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    vn::TextView *source = tab->sourceView();
    focusView(source);
    viewButton(window, true)->click();
    source->grab();
    const auto [first, last] = tab->readLineRange();
    QCOMPARE(first, 0);
    QVERIFY(last > 20);
    QTRY_COMPARE(shownCells(window),
                 (QStringList{QStringLiteral("ViewSwitch"), QStringLiteral("ZoomStatus"), QStringLiteral("RangeStatus"),
                              QStringLiteral("PositionStatus"), QStringLiteral("IndentationStatus"),
                              QStringLiteral("LineEndingStatus"), QStringLiteral("EncodingStatus"),
                              QStringLiteral("LanguageStatus")}));
    auto *range = window.findChild<QLabel *>(QStringLiteral("RangeStatus"));
    QTRY_COMPARE(range->text(), QStringLiteral("Lines 1–%1 of 213").arg(last + 1));
    QTRY_VERIFY(range->width() >= range->sizeHint().width());
    QCOMPARE(window.findChild<QLabel *>(QStringLiteral("PositionStatus"))->text(), QStringLiteral("Ln 1, Col 1"));
    auto *bar = window.findChild<QFrame *>(QStringLiteral("ReadPopup"));
    QVERIFY(bar && bar->isVisible());
    bar->close();
    for (QToolButton *button : tab->findChildren<QToolButton *>()) {
        if (button->toolTip() == QStringLiteral("Read-only"))
            QVERIFY(!button->isVisible());
    }
    if (const QString file = qEnvironmentVariable("VN_SCREENSHOT"); !file.isEmpty())
        window.grab().save(QString(file).replace(QStringLiteral(".png"), QStringLiteral("-code-columns.png")));
    // A page on, and the status follows.
    QTest::keyClick(source, Qt::Key_PageDown);
    QTRY_COMPARE(range->text(), QStringLiteral("Lines %1–%2 of 213")
                                    .arg(tab->readLineRange().first + 1)
                                    .arg(tab->readLineRange().second + 1));
    QVERIFY(tab->readLineRange().first > 0);
    // It edits: typing, the edit commands and undo; the caret's place follows.
    const QString before = tab->text();
    QVERIFY(!tab->isReadOnly());
    QVERIFY(actionFor(window, QStringLiteral("edit.paste"))->isEnabled());
    const int line = source->textCursor().blockNumber();
    QVERIFY(line > 0);
    QTest::keyClicks(source, QStringLiteral("abc"));
    QTRY_COMPARE(window.findChild<QLabel *>(QStringLiteral("PositionStatus"))->text(),
                 QStringLiteral("Ln %1, Col 4").arg(line + 1));
    actionFor(window, QStringLiteral("edit.toggleComment"))->trigger();
    QVERIFY(tab->document()->findBlockByNumber(line).text().startsWith(QStringLiteral("# abc")));
    for (int i = 0; i < 10 && tab->text() != before; ++i)
        actionFor(window, QStringLiteral("edit.undo"))->trigger();
    QCOMPARE(tab->text(), before);
    QVERIFY(tab->showsColumns());
    // Scroll: editable as well.
    viewButton(window, false)->click();
    QVERIFY(!tab->isReadOnly());
    QVERIFY(!bar->isVisible());
    QTRY_VERIFY(!range->isVisible());
    QTest::keyClicks(source, QStringLiteral("abc"));
    QVERIFY(tab->text() != before);
}

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
