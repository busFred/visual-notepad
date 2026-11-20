// Editor groups and tab moves. QDrag cannot be driven offscreen, so the move
// operations are called directly and the drop path is exercised with
// synthetic drop events. Set VN_SCREENSHOT_DIR=<dir> to save a split-editor
// rendering.
#include "DocumentTab.h"
#include "DragGeometry.h"
#include "EditorGroup.h"
#include "EvenSplitter.h"
#include "MainWindow.h"
#include "Session.h"
#include "Settings.h"
#include "Theme.h"
#include "WelcomeView.h"
#include "editor/TextView.h"

#include <QApplication>
#include <QDropEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTimer>

using vn::DocumentTab;
using vn::EditorGroup;
using vn::MainWindow;

class TestGroups : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void newFileFollowsActiveGroup();
    void tabClickActivatesGroup();
    void touchHoldOpensTabMenu();
    void dropMovesTabBetweenGroups();
    void dropIsIgnoredForOtherMime();
    void moveToOtherWindow();
    void closingLastTab_data();
    void closingLastTab();
    void lastTabDraggedOutClosesWindow();
    void lastTabMovedByMenuKeepsWindow();
    void droppedNowhereOpensWindow();
    void tabsListBothGroups();
    void doubleClickingHandleEvensGroups();
    void doubleClickingHandleEvensSourceAndVisual();
    void dragHotSpotStaysOnTab();
    void screenshot();

private:
    QString write(const QString &name, const QByteArray &content);
    // Sends a tab drop to `group` at `pos` (group coordinates).
    static void drop(EditorGroup *group, DocumentTab *tab, const QPoint &pos);
    static QPoint stripPos(EditorGroup *group, int x) { return QPoint(group->tabBar()->mapTo(group, QPoint(x, 0)).x(), 10); }
    // The middle of the body: a merge, not a split.
    static QPoint bodyPos(EditorGroup *group) { return QPoint(group->width() / 2, group->height() / 2 + 16); }
    static EditorGroup *split(MainWindow &window)
    {
        return window.addGroup(window.mainGroup(), vn::dragGeometry::SplitDirection::Right);
    }
    static void show(MainWindow &window)
    {
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
    }

    QTemporaryDir m_dir;
};

void TestGroups::initTestCase()
{
    QVERIFY(m_dir.isValid());
    vn::SessionManager::setDataDir(m_dir.filePath(QStringLiteral("session")));
    vn::theme::apply(*qApp);
}

QString TestGroups::write(const QString &name, const QByteArray &content)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
        return {};
    return path;
}

void TestGroups::drop(EditorGroup *group, DocumentTab *tab, const QPoint &pos)
{
    std::unique_ptr<QMimeData> mime(EditorGroup::mimeForTab(tab));
    QDragEnterEvent enter(QPointF(pos), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(group, &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent event(QPointF(pos), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(group, &event);
    QVERIFY(event.isAccepted());
}

void TestGroups::newFileFollowsActiveGroup()
{
    const QString path = write(QStringLiteral("a.txt"), "alpha\n");
    MainWindow window;
    window.newFile();
    split(window);
    show(window);

    window.activateGroup(window.groups().value(1));
    QVERIFY(window.openFile(path));
    QCOMPARE(window.groups().value(1)->count(), 1);
    QCOMPARE(window.mainGroup()->count(), 1);
    QCOMPARE(window.currentTab(), window.groups().value(1)->tab(0));
    QCOMPARE(window.windowTitle(), QStringLiteral("a.txt — Visual Notepad"));

    // Opening the same file again finds it in the right group.
    window.activateGroup(window.mainGroup());
    QVERIFY(window.openFile(path));
    QCOMPARE(window.activeGroup(), window.groups().value(1));
    QCOMPARE(window.tabCount(), 2);

    window.activateGroup(window.mainGroup());
    QCOMPARE(window.windowTitle(), QStringLiteral("Untitled — Visual Notepad"));
    window.newFile();
    QCOMPARE(window.mainGroup()->count(), 2);
}

void TestGroups::touchHoldOpensTabMenu()
{
    EditorGroup group;
    group.resize(550, 350);
    // The bar uses the existing tab menu signal even without the main window.
    group.tabBar()->addTab(QStringLiteral("Tab"));
    group.show();
    QVERIFY(QTest::qWaitForWindowExposed(&group));
    QTabBar *bar = group.tabBar();
    QSignalSpy menu(&group, &EditorGroup::tabContextMenuRequested);
    auto touch = QTest::touchEvent(bar, QTest::createTouchDevice(), false);
    const QPoint at = bar->tabRect(0).center();
    touch.press(0, at).commit();
    QTest::qWait(550);
    touch.release(0, at).commit();
    QTRY_COMPARE(menu.size(), 1);
    QCOMPARE(menu.first().first().toInt(), 0);
}

void TestGroups::tabClickActivatesGroup()
{
    MainWindow window;
    window.newFile();
    split(window);
    window.activateGroup(window.groups().value(1));
    window.newFile();
    window.activateGroup(window.mainGroup());
    show(window);

    QTabBar *bar = window.groups().value(1)->tabBar();
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(0).center());
    QCOMPARE(window.activeGroup(), window.groups().value(1));
    QCOMPARE(window.currentTab(), window.groups().value(1)->tab(0));

    bar = window.mainGroup()->tabBar();
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(0).center());
    QCOMPARE(window.activeGroup(), window.mainGroup());
}

void TestGroups::dropMovesTabBetweenGroups()
{
    const QString pathA = write(QStringLiteral("one.txt"), "one two three\n");
    const QString pathB = write(QStringLiteral("two.txt"), "second\n");
    MainWindow window;
    QVERIFY(window.openFile(pathA));
    QVERIFY(window.openFile(pathB));
    split(window);
    show(window);
    EditorGroup *main = window.mainGroup();
    EditorGroup *side = window.groups().value(1);
    DocumentTab *first = main->tab(0);
    DocumentTab *second = main->tab(1);
    QTextCursor cursor = first->editor()->textCursor();
    cursor.setPosition(4);
    first->editor()->setTextCursor(cursor);
    QCOMPARE(window.currentTab(), second);

    // Body of the empty right group: appended and made current.
    drop(side, first, bodyPos(side));
    QCOMPARE(main->count(), 1);
    QCOMPARE(side->count(), 1);
    QCOMPARE(side->tab(0), first);
    QCOMPARE(window.activeGroup(), side);
    QCOMPARE(window.currentTab(), first);
    QCOMPARE(main->currentTab(), second);
    QCOMPARE(first->editor()->textCursor().position(), 4);
    QCOMPARE(first->text(), QStringLiteral("one two three\n"));

    // Back onto the main strip, before its only tab; the emptied group goes.
    drop(main, first, stripPos(main, main->tabBar()->tabRect(0).left() + 2));
    QCOMPARE(main->count(), 2);
    QCOMPARE(window.groups(), QList<EditorGroup *>{main});
    QCOMPARE(main->tab(0), first);
    QCOMPARE(main->tab(1), second);
    QCOMPARE(window.currentTab(), first);
    QCOMPARE(first->editor()->textCursor().position(), 4);

    // And after the last tab.
    drop(main, first, stripPos(main, main->tabBar()->tabRect(1).right() - 2));
    QCOMPARE(main->tab(0), second);
    QCOMPARE(main->tab(1), first);
    QCOMPARE(window.currentTab(), first);
    QCOMPARE(main->insertIndexAt(stripPos(main, main->tabBar()->tabRect(0).left() + 1)), 0);
    QCOMPARE(main->insertIndexAt(stripPos(main, main->tabBar()->tabRect(0).right() - 1)), 1);
}

void TestGroups::dropIsIgnoredForOtherMime()
{
    MainWindow window;
    window.newFile();
    split(window);
    show(window);
    QMimeData mime;
    mime.setText(QStringLiteral("x"));
    EditorGroup *side = window.groups().value(1);
    QDropEvent event(QPointF(bodyPos(side)), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(side, &event);
    QCOMPARE(side->count(), 0);
}

void TestGroups::moveToOtherWindow()
{
    MainWindow source;
    source.newFile();
    source.newFile();
    MainWindow target;
    target.newFile();
    show(source);
    show(target);
    DocumentTab *tab = source.mainGroup()->tab(0);
    const QString name = tab->displayName();
    tab->document()->setPlainText(QStringLiteral("moved text"));

    drop(target.mainGroup(), tab, stripPos(target.mainGroup(), target.mainGroup()->tabBar()->tabRect(0).left() + 1));
    QCOMPARE(source.tabCount(), 1);
    QVERIFY(source.isVisible());
    QCOMPARE(target.tabCount(), 2);
    QCOMPARE(target.mainGroup()->tab(0), tab);
    QCOMPARE(target.currentTab(), tab);
    QCOMPARE(tab->text(), QStringLiteral("moved text"));
    QCOMPARE(tab->displayName(), name);
    // The source no longer follows the tab.
    QCOMPARE(source.currentTab(), source.mainGroup()->tab(0));
    QCOMPARE(target.windowTitle(), QStringLiteral("%1 — Visual Notepad").arg(name));
}

void TestGroups::closingLastTab_data()
{
    QTest::addColumn<bool>("closeWindow");
    QTest::newRow("keep-window") << false;
    QTest::newRow("close-window") << true;
}

void TestGroups::closingLastTab()
{
    QFETCH(bool, closeWindow);
    auto &settings = vn::Settings::instance();
    const auto reset = qScopeGuard([&settings] { settings.reset(vn::key::lastTabClosed); });
    if (closeWindow)
        settings.setValue(vn::key::lastTabClosed, QStringLiteral("close"));

    MainWindow other;
    MainWindow window;
    show(other);
    show(window);
    window.newFile();
    window.newFile();
    QVERIFY(window.closeTabs(0, MainWindow::CloseScope::This));
    QVERIFY(window.isVisible());
    QCOMPARE(window.tabCount(), 1);
    window.currentTab()->document()->setPlainText(QStringLiteral("unsaved"));

    auto response = QMessageBox::Cancel;
    int prompts = 0;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            ++prompts;
            box->done(response);
        }
    });
    answer.start(10);
    QVERIFY(!window.closeTabs(0, MainWindow::CloseScope::All));
    QCOMPARE(prompts, 1);
    QVERIFY(window.isVisible());
    QCOMPARE(window.tabCount(), 1);

    response = QMessageBox::Discard;
    QVERIFY(window.closeTabs(0, MainWindow::CloseScope::All));
    QCOMPARE(prompts, 2);
    QCOMPARE(window.tabCount(), 0);
    QCOMPARE(window.isVisible(), !closeWindow);
    QCOMPARE(vn::SessionManager::instance().windows().contains(&window), !closeWindow);
    QVERIFY(other.isVisible());
    if (!closeWindow) {
        auto *welcome = window.findChild<vn::WelcomeView *>();
        QVERIFY(welcome && welcome->isVisible());
    }
}

// Moving the last tab with the menu is no close: the window stays, whatever
// the preference for closing the last tab says.
void TestGroups::lastTabMovedByMenuKeepsWindow()
{
    auto &settings = vn::Settings::instance();
    const auto reset = qScopeGuard([&settings] { settings.reset(vn::key::lastTabClosed); });
    settings.setValue(vn::key::lastTabClosed, QStringLiteral("close"));
    MainWindow window;
    window.newFile();
    show(window);
    DocumentTab *tab = window.mainGroup()->tab(0);
    window.moveTabToNewWindow(tab);
    auto *other = qobject_cast<MainWindow *>(tab->window());
    QVERIFY(other && other != &window);
    const auto closeOther = qScopeGuard([other] { other->close(); });
    QCOMPARE(window.tabCount(), 0);
    QVERIFY(window.isVisible());
}

void TestGroups::lastTabDraggedOutClosesWindow()
{
    MainWindow source;
    source.newFile();
    MainWindow target;
    target.newFile();
    show(source);
    show(target);
    DocumentTab *tab = source.mainGroup()->tab(0);
    tab->document()->setPlainText(QStringLiteral("unsaved"));
    QVERIFY(tab->isModified());

    drop(target.mainGroup(), tab, bodyPos(target.mainGroup()));
    // Closed without asking about the (moved) unsaved tab.
    QVERIFY(!source.isVisible());
    QVERIFY(!vn::SessionManager::instance().windows().contains(&source));
    QCOMPARE(target.tabCount(), 2);
    QCOMPARE(target.currentTab(), tab);
    QVERIFY(tab->isModified());
}

void TestGroups::droppedNowhereOpensWindow()
{
    MainWindow source;
    source.newFile();
    source.newFile();
    show(source);
    DocumentTab *tab = source.mainGroup()->tab(1);
    tab->document()->setPlainText(QStringLiteral("elsewhere"));
    tab->document()->setModified(false); // So closing the new window later does not prompt.
    const auto before = vn::SessionManager::instance().windows();

    Q_EMIT source.mainGroup()->tabsDroppedOutside({tab}, QPoint(120, 80), false);
    QCOMPARE(source.tabCount(), 1);
    QVERIFY(source.isVisible());
    auto after = vn::SessionManager::instance().windows();
    QCOMPARE(after.size(), before.size() + 1);
    MainWindow *created = after.last();
    QCOMPARE(created->tabCount(), 1);
    QCOMPARE(created->currentTab(), tab);
    QCOMPARE(tab->text(), QStringLiteral("elsewhere"));
    created->close();
}

void TestGroups::tabsListBothGroups()
{
    MainWindow window;
    window.newFile();
    split(window);
    window.activateGroup(window.groups().value(1));
    window.newFile();
    DocumentTab *mainTab = window.mainGroup()->tab(0);
    DocumentTab *sideTab = window.groups().value(1)->tab(0);
    QCOMPARE(window.tabs(), (QList<DocumentTab *>{mainTab, sideTab}));
    QCOMPARE(window.currentTabIndex(), 1);
    window.setCurrentTabIndex(0);
    QCOMPARE(window.activeGroup(), window.mainGroup());
    QCOMPARE(window.currentTab(), mainTab);
}

void TestGroups::doubleClickingHandleEvensGroups()
{
    MainWindow window;
    show(window);
    split(window);
    auto *groups = window.findChild<vn::EvenSplitter *>();
    QVERIFY(groups);
    QCOMPARE(groups->count(), 2);
    const int total = groups->width() - groups->handleWidth();
    groups->setSizes({total / 4, total - total / 4});
    QVERIFY(qAbs(groups->sizes()[0] - groups->sizes()[1]) > 50);
    QSplitterHandle *handle = groups->handle(1);
    // The widened hit area over the 1-px handle forwards to it.
    QWidget *strip = groups->childAt(handle->geometry().center());
    QVERIFY(strip);
    QVERIFY(strip != handle);
    QVERIFY(strip->width() > handle->width());
    QTest::mouseDClick(strip, Qt::LeftButton, {}, strip->rect().center());
    QVERIFY(qAbs(groups->sizes()[0] - groups->sizes()[1]) <= 1);
    groups->setSizes({total / 4, total - total / 4});
    QTest::mouseDClick(handle, Qt::LeftButton, {}, handle->rect().center());
    QVERIFY(qAbs(groups->sizes()[0] - groups->sizes()[1]) <= 1);
}

void TestGroups::doubleClickingHandleEvensSourceAndVisual()
{
    MainWindow window;
    QVERIFY(window.openFile(write(QStringLiteral("even.md"), "# Title\n\ntext\n")));
    show(window);
    DocumentTab *tab = window.currentTab();
    QVERIFY(tab);
    tab->setViewMode(DocumentTab::ViewMode::SourceVisual);
    auto *splitter = tab->findChild<vn::EvenSplitter *>();
    QVERIFY(splitter);
    QCOMPARE(splitter->count(), 2);
    const int total = splitter->sizes()[0] + splitter->sizes()[1];
    splitter->setSizes({total / 5, total - total / 5});
    QVERIFY(qAbs(splitter->sizes()[0] - splitter->sizes()[1]) > 50);
    QSplitterHandle *handle = splitter->handle(1);
    QTest::mouseDClick(handle, Qt::LeftButton, {}, handle->rect().center());
    QVERIFY(qAbs(splitter->sizes()[0] - splitter->sizes()[1]) <= 1);
}

void TestGroups::dragHotSpotStaysOnTab()
{
    const QRect tab(100, 0, 120, 30);
    // Pressed inside the tab: the hot spot is the press offset, however far
    // the cursor has moved since.
    QCOMPARE(vn::dragGeometry::hotSpot(QPoint(130, 12), tab), QPoint(30, 12));
    // A press point outside the pixmap is held to its edge.
    QCOMPARE(vn::dragGeometry::hotSpot(QPoint(400, 90), tab), QPoint(119, 29));
    QCOMPARE(vn::dragGeometry::hotSpot(QPoint(50, -5), tab), QPoint(0, 0));
}

void TestGroups::screenshot()
{
    const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR");
    if (dir.isEmpty())
        QSKIP("VN_SCREENSHOT_DIR not set");
    const QString left = write(QStringLiteral("left.md"), "# Left\n\nSome *text* on the left.\n");
    const QString right = write(QStringLiteral("right.md"), "# Right\n\nSome text on the right.\n");
    MainWindow window;
    QVERIFY(window.openFile(left));
    split(window);
    window.activateGroup(window.groups().value(1));
    QVERIFY(window.openFile(right));
    window.groups().value(1)->tab(0)->setViewMode(DocumentTab::ViewMode::SourceVisual);
    show(window);
    // The drop hints, as during a drag: an enter event first, then moves.
    const QString file = QDir(dir).filePath(QStringLiteral("groups.png"));
    QVERIFY(window.grab().save(file));

    std::unique_ptr<QMimeData> mime(EditorGroup::mimeForTab(window.mainGroup()->tab(0)));
    EditorGroup *side = window.groups().value(1);
    QDragMoveEvent move(QPointF(bodyPos(side)), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QDragEnterEvent enter(QPointF(bodyPos(side)), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(side, &enter);
    QCoreApplication::sendEvent(side, &move);
    QVERIFY(move.isAccepted());
    QVERIFY(window.grab().save(QDir(dir).filePath(QStringLiteral("groups-drop-body.png"))));
    QDragMoveEvent strip(QPointF(stripPos(side, side->tabBar()->tabRect(0).right() - 3)), Qt::MoveAction, mime.get(),
                         Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(side, &strip);
    QVERIFY(window.grab().save(QDir(dir).filePath(QStringLiteral("groups-drop-strip.png"))));
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestGroups)
#include "test_groups.moc"
