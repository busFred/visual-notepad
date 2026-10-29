// The editor grid: drop zones, splitting and collapsing groups, the grid in
// the session, multi-selected tabs and copying tabs. QDrag cannot run
// offscreen, so drops are synthetic drop events or direct calls.
#include "Commands.h"
#include "DocumentTab.h"
#include "DragGeometry.h"
#include "EditorGroup.h"
#include "EvenSplitter.h"
#include "MainWindow.h"
#include "Session.h"
#include "Settings.h"
#include "TextFile.h"
#include "Theme.h"
#include "editor/TextView.h"

#include <QApplication>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTimer>

using namespace vn;
using Split = dragGeometry::SplitDirection;

class TestEditorGrid : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();
    void dropZones();
    void overlayRects();
    void modifiers();
    void splitEditorCopiesCurrentTab();
    void closingLastTabRemovesGroup();
    void collapseFlattensSameOrientation();
    void collapseHoistsIntoRoot();
    void focusNextGroupCycles();
    void dropOnEdgeSplits();
    void altDropMerges();
    void ownOnlyTabCannotSplitItsGroup();
    void ctrlDropCopies();
    void copyIntoGroupShowingFileActivates();
    void ctrlDropInOwnStripMoves();
    void moveIntoGroupShowingFileKeepsOne();
    void ctrlClickTogglesSelection();
    void shiftClickSelectsRange();
    void plainClickClearsSelection();
    void mouseClicksSelect();
    void draggingSelectionMovesAll();
    void selectionDroppedOutsideOpensOneWindow();
    void copyOutsideKeepsSources();
    void contextMenuSplitsSelection();
    void gridSessionRoundTrip();
    void oldSessionLoadsIntoOneGroup();
    void newCommandsHaveNoConflicts();

private:
    QString write(const QString &name, const QByteArray &content);
    MainWindow *makeWindow();
    static EvenSplitter *root(MainWindow *window) { return qobject_cast<EvenSplitter *>(window->centralWidget()); }
    static QPoint bodyPoint(EditorGroup *group, double x, double y);
    static QPoint stripPoint(EditorGroup *group, int tab);
    // A drag of `tabs` over and onto `group` at `pos`; whether the drop was
    // accepted.
    static bool drop(EditorGroup *group, const QList<DocumentTab *> &tabs, const QPoint &pos,
                     Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    static void type(DocumentTab *tab, const QString &text);
    static void triggerMenuItem(const QString &text);

    QTemporaryDir m_dir;
    QTimer m_watcher;
    int m_prompts = 0;
    QList<QPointer<MainWindow>> m_made;
};

void TestEditorGrid::initTestCase()
{
    QVERIFY(m_dir.isValid());
    theme::apply(*qApp);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    // Counts unsaved-changes prompts and answers Don't Save.
    connect(&m_watcher, &QTimer::timeout, this, [this] {
        QWidget *modal = QApplication::activeModalWidget();
        if (!modal)
            return;
        ++m_prompts;
        if (auto *box = qobject_cast<QMessageBox *>(modal); box && box->button(QMessageBox::Discard))
            box->button(QMessageBox::Discard)->click();
        else
            modal->close();
    });
    m_watcher.start(20);
}

void TestEditorGrid::init()
{
    m_prompts = 0;
    const QString dir = m_dir.filePath(QStringLiteral("data-") + QTest::currentTestFunction());
    QVERIFY(QDir().mkpath(dir));
    Settings::instance().useFile(dir + QStringLiteral("/settings.ini"));
    SessionManager::setDataDir(dir + QStringLiteral("/data"));
}

void TestEditorGrid::cleanup()
{
    for (const QPointer<MainWindow> &window : std::as_const(m_made))
        delete window.data();
    m_made.clear();
    for (MainWindow *window : SessionManager::instance().windows())
        delete window;
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(TextFile::all().isEmpty());
}

QString TestEditorGrid::write(const QString &name, const QByteArray &content)
{
    const QString path = m_dir.filePath(QTest::currentTestFunction() + QStringLiteral("-") + name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
        return {};
    return path;
}

MainWindow *TestEditorGrid::makeWindow()
{
    auto *window = new MainWindow;
    m_made.append(window);
    window->show();
    if (!QTest::qWaitForWindowExposed(window))
        qWarning("window not exposed");
    return window;
}

QPoint TestEditorGrid::bodyPoint(EditorGroup *group, double x, double y)
{
    // The body is everything under the 32-px strip.
    const int top = group->tabBar()->isVisible() ? 32 : 0;
    return QPoint(int(group->width() * x), top + int((group->height() - top) * y));
}

QPoint TestEditorGrid::stripPoint(EditorGroup *group, int tab)
{
    QTabBar *bar = group->tabBar();
    return QPoint(bar->mapTo(group, QPoint(bar->tabRect(tab).left() + 2, 0)).x(), 10);
}

bool TestEditorGrid::drop(EditorGroup *group, const QList<DocumentTab *> &tabs, const QPoint &pos,
                          Qt::KeyboardModifiers modifiers)
{
    std::unique_ptr<QMimeData> mime(EditorGroup::mimeForTabs(tabs));
    const Qt::DropActions actions = Qt::MoveAction | Qt::CopyAction;
    QDragEnterEvent enter(QPointF(pos), actions, mime.get(), Qt::LeftButton, modifiers);
    QCoreApplication::sendEvent(group, &enter);
    QDragMoveEvent move(QPointF(pos), actions, mime.get(), Qt::LeftButton, modifiers);
    QCoreApplication::sendEvent(group, &move);
    if (!move.isAccepted()) {
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(group, &leave);
        return false;
    }
    QDropEvent event(QPointF(pos), actions, mime.get(), Qt::LeftButton, modifiers);
    QCoreApplication::sendEvent(group, &event);
    return event.isAccepted();
}

void TestEditorGrid::type(DocumentTab *tab, const QString &text)
{
    TextView *view = tab->sourceView();
    QTextCursor end(tab->document());
    end.movePosition(QTextCursor::End);
    view->setTextCursor(end);
    QTest::keyClicks(view, text);
}

void TestEditorGrid::triggerMenuItem(const QString &text)
{
    QTimer::singleShot(0, qApp, [text] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        QAction *found = nullptr;
        for (QAction *action : menu->actions()) {
            if (action->text() == text)
                found = action;
        }
        menu->close();
        QVERIFY(found);
        found->trigger();
    });
}

// editorDropTarget.ts positionOverlay() for an editor drag, with the default
// openSideBySideDirection "right".
void TestEditorGrid::dropZones()
{
    using dragGeometry::dropZone;
    const QSize size(300, 200);
    QCOMPARE(dropZone(QPoint(150, 100), size), Split::None);
    // The 10% edge band: left and right thirds split sideways...
    QCOMPARE(dropZone(QPoint(10, 100), size), Split::Left);
    QCOMPARE(dropZone(QPoint(290, 100), size), Split::Right);
    QCOMPARE(dropZone(QPoint(50, 5), size), Split::Left);
    QCOMPARE(dropZone(QPoint(250, 195), size), Split::Right);
    // ...the middle third up or down by half.
    QCOMPARE(dropZone(QPoint(150, 5), size), Split::Up);
    QCOMPARE(dropZone(QPoint(150, 195), size), Split::Down);
    // Just inside the band's inner edge is still a merge.
    QCOMPARE(dropZone(QPoint(31, 100), size), Split::None);
    QCOMPARE(dropZone(QPoint(150, 21), size), Split::None);
    // Splitting off (Alt): always a merge.
    QCOMPARE(dropZone(QPoint(10, 100), size, false), Split::None);
    // openSideBySideDirection "down": top and bottom thirds split up/down.
    QCOMPARE(dropZone(QPoint(10, 30), size, true, false), Split::Up);
    QCOMPARE(dropZone(QPoint(10, 180), size, true, false), Split::Down);
    QCOMPARE(dropZone(QPoint(10, 100), size, true, false), Split::Left);
    QCOMPARE(dropZone(QPoint(290, 100), size, true, false), Split::Right);
}

void TestEditorGrid::overlayRects()
{
    using dragGeometry::overlayRect;
    const QRect body(0, 32, 300, 201);
    QCOMPARE(overlayRect(Split::None, body), body);
    QCOMPARE(overlayRect(Split::Left, body), QRect(0, 32, 150, 201));
    QCOMPARE(overlayRect(Split::Right, body), QRect(150, 32, 150, 201));
    QCOMPARE(overlayRect(Split::Up, body), QRect(0, 32, 300, 100));
    QCOMPARE(overlayRect(Split::Down, body), QRect(0, 132, 300, 101));
}

void TestEditorGrid::modifiers()
{
#ifndef Q_OS_MACOS
    QVERIFY(dragGeometry::isCopyOperation(Qt::ControlModifier));
    QVERIFY(!dragGeometry::isCopyOperation(Qt::AltModifier));
    QVERIFY(dragGeometry::isToggleSplitOperation(Qt::AltModifier));
    QVERIFY(!dragGeometry::isToggleSplitOperation(Qt::ControlModifier));
#endif
    QVERIFY(!dragGeometry::isCopyOperation(Qt::NoModifier));
    QVERIFY(!dragGeometry::isToggleSplitOperation(Qt::NoModifier));
}

void TestEditorGrid::splitEditorCopiesCurrentTab()
{
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(write(QStringLiteral("a.txt"), "alpha\n")));
    DocumentTab *first = window->currentTab();
    EditorGroup *main = window->activeGroup();

    window->splitEditor(Split::Right);
    QCOMPARE(window->groups().size(), 2);
    EditorGroup *right = window->groups().value(1);
    QCOMPARE(window->activeGroup(), right);
    QCOMPARE(right->count(), 1);
    DocumentTab *copy = right->tab(0);
    QVERIFY(copy != first);
    QCOMPARE(copy->file(), first->file());
    QCOMPARE(main->count(), 1);
    QCOMPARE(root(window)->orientation(), Qt::Horizontal);

    // Edits show in both; closing the copy does not ask.
    type(copy, QStringLiteral("beta"));
    QCOMPARE(first->text(), QStringLiteral("alpha\nbeta"));
    QVERIFY(window->closeTabs(0, MainWindow::CloseScope::This));
    QCOMPARE(m_prompts, 0);
    QCOMPARE(window->groups(), QList<EditorGroup *>{main});
    QVERIFY(first->isModified());

    // Up / Down nest a vertical splitter in the horizontal root.
    window->splitEditor(Split::Down);
    EditorGroup *below = window->activeGroup();
    QVERIFY(below != main);
    QCOMPARE(root(window)->orientation(), Qt::Vertical);
    window->splitEditor(Split::Left);
    EditorGroup *left = window->activeGroup();
    QCOMPARE(window->groups(), (QList<EditorGroup *>{main, left, below}));
    auto *branch = qobject_cast<EvenSplitter *>(left->parentWidget());
    QVERIFY(branch && branch != root(window));
    QCOMPARE(branch->orientation(), Qt::Horizontal);
    QCOMPARE(branch->parentWidget(), root(window));
}

void TestEditorGrid::closingLastTabRemovesGroup()
{
    MainWindow *window = makeWindow();
    window->newFile();
    EditorGroup *main = window->activeGroup();
    EditorGroup *right = window->addGroup(main, Split::Right);
    EditorGroup *below = window->addGroup(right, Split::Down);
    window->activateGroup(below);
    window->newFile();
    window->activateGroup(right);
    window->newFile();
    QCOMPARE(window->groups(), (QList<EditorGroup *>{main, right, below}));
    auto *branch = qobject_cast<EvenSplitter *>(right->parentWidget());
    QVERIFY(branch != root(window));

    // The most recently active other group takes over (doRemoveEmptyGroup()).
    window->activateGroup(below);
    window->activateGroup(right);
    QVERIFY(window->closeTabs(0, MainWindow::CloseScope::This));
    QCOMPARE(window->groups(), (QList<EditorGroup *>{main, below}));
    QCOMPARE(window->activeGroup(), below);
    // The vertical splitter with one pane left gave way to it.
    QCOMPARE(below->parentWidget(), root(window));
    QCOMPARE(root(window)->count(), 2);

    // The only group stays, empty.
    window->activateGroup(main);
    QVERIFY(window->closeTabs(0, MainWindow::CloseScope::All));
    window->activateGroup(below);
    QVERIFY(window->closeTabs(0, MainWindow::CloseScope::All));
    QCOMPARE(window->groups().size(), 1);
    QCOMPARE(window->activeGroup()->count(), 0);
    QVERIFY(!window->currentTab());
}

void TestEditorGrid::collapseFlattensSameOrientation()
{
    // Root horizontal [A, vertical [horizontal [B, D], C]]; closing C leaves
    // the vertical splitter with one horizontal pane, which merges into the
    // root: [A, B, D].
    MainWindow *window = makeWindow();
    window->newFile();
    EditorGroup *a = window->activeGroup();
    EditorGroup *b = window->addGroup(a, Split::Right);
    EditorGroup *c = window->addGroup(b, Split::Down);
    EditorGroup *d = window->addGroup(b, Split::Right);
    for (EditorGroup *group : {b, c, d}) {
        window->activateGroup(group);
        window->newFile();
    }
    QCOMPARE(window->groups(), (QList<EditorGroup *>{a, b, d, c}));
    window->activateGroup(c);
    QVERIFY(window->closeTabs(0, MainWindow::CloseScope::This));
    QCOMPARE(window->groups(), (QList<EditorGroup *>{a, b, d}));
    QCOMPARE(root(window)->count(), 3);
    for (EditorGroup *group : {a, b, d})
        QCOMPARE(group->parentWidget(), root(window));
    QCOMPARE(root(window)->orientation(), Qt::Horizontal);
    // Every pane kept some room.
    for (int size : root(window)->sizes())
        QVERIFY(size > 50);
}

void TestEditorGrid::collapseHoistsIntoRoot()
{
    // Root horizontal [A, vertical [B, C]]; closing A leaves the root with
    // the vertical splitter alone, whose panes become the root's.
    MainWindow *window = makeWindow();
    window->newFile();
    EditorGroup *a = window->activeGroup();
    EditorGroup *b = window->addGroup(a, Split::Right);
    EditorGroup *c = window->addGroup(b, Split::Down);
    for (EditorGroup *group : {b, c}) {
        window->activateGroup(group);
        window->newFile();
    }
    window->activateGroup(a);
    QVERIFY(window->closeTabs(0, MainWindow::CloseScope::This));
    QCOMPARE(window->groups(), (QList<EditorGroup *>{b, c}));
    QCOMPARE(root(window)->orientation(), Qt::Vertical);
    QCOMPARE(b->parentWidget(), root(window));
    QCOMPARE(c->parentWidget(), root(window));
    // Double-clicking the handle evens them out.
    root(window)->setSizes({100, 400});
    QSplitterHandle *handle = root(window)->handle(1);
    QTest::mouseDClick(handle, Qt::LeftButton, {}, handle->rect().center());
    QVERIFY(qAbs(root(window)->sizes()[0] - root(window)->sizes()[1]) <= 1);
}

void TestEditorGrid::focusNextGroupCycles()
{
    MainWindow *window = makeWindow();
    window->newFile();
    EditorGroup *a = window->activeGroup();
    EditorGroup *b = window->addGroup(a, Split::Right);
    EditorGroup *c = window->addGroup(b, Split::Down);
    window->activateGroup(a);
    window->focusNextGroup(1);
    QCOMPARE(window->activeGroup(), b);
    window->focusNextGroup(1);
    QCOMPARE(window->activeGroup(), c);
    window->focusNextGroup(1);
    QCOMPARE(window->activeGroup(), a);
    window->focusNextGroup(-1);
    QCOMPARE(window->activeGroup(), c);
}

void TestEditorGrid::dropOnEdgeSplits()
{
    MainWindow *window = makeWindow();
    window->newFile();
    window->newFile();
    EditorGroup *main = window->activeGroup();
    DocumentTab *moved = main->tab(0);
    DocumentTab *stays = main->tab(1);

    // The overlay shades the half the new group would take.
    std::unique_ptr<QMimeData> mime(EditorGroup::mimeForTab(moved));
    const QPoint left = bodyPoint(main, 0.02, 0.5);
    QDragEnterEvent enter(QPointF(left), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(main, &enter);
    QVERIFY(enter.isAccepted());
    QCOMPARE(main->dropZoneAt(left, Qt::NoModifier), Split::Left);
    QDragLeaveEvent leave;
    QCoreApplication::sendEvent(main, &leave);

    QVERIFY(drop(main, {moved}, left));
    QCOMPARE(window->groups().size(), 2);
    EditorGroup *created = window->groups().value(0);
    QVERIFY(created != main);
    QCOMPARE(created->count(), 1);
    QCOMPARE(created->tab(0), moved);
    QCOMPARE(main->count(), 1);
    QCOMPARE(main->tab(0), stays);
    QCOMPARE(window->activeGroup(), created);
    QCOMPARE(window->currentTab(), moved);

    // The only tab of a group onto another group's bottom edge: the source
    // group goes, a group below the target holds the tab.
    QVERIFY(drop(main, {moved}, bodyPoint(main, 0.5, 0.98)));
    QCOMPARE(window->groups().size(), 2);
    QCOMPARE(window->groups().value(0), main);
    EditorGroup *below = window->groups().value(1);
    QCOMPARE(below->tab(0), moved);
    QCOMPARE(root(window)->orientation(), Qt::Vertical);
}

void TestEditorGrid::altDropMerges()
{
    MainWindow *window = makeWindow();
    window->newFile();
    EditorGroup *main = window->activeGroup();
    EditorGroup *right = window->addGroup(main, Split::Right);
    window->activateGroup(right);
    window->newFile();
    DocumentTab *tab = right->tab(0);
    window->activateGroup(main);
    window->newFile();
    QCOMPARE(main->dropZoneAt(bodyPoint(main, 0.02, 0.5), Qt::AltModifier), Split::None);
    QVERIFY(drop(main, {tab}, bodyPoint(main, 0.02, 0.5), Qt::AltModifier));
    QCOMPARE(window->groups(), QList<EditorGroup *>{main});
    QCOMPARE(main->count(), 3);
    QCOMPARE(main->tab(2), tab);
}

void TestEditorGrid::ownOnlyTabCannotSplitItsGroup()
{
    MainWindow *window = makeWindow();
    window->newFile();
    EditorGroup *main = window->activeGroup();
    DocumentTab *tab = main->tab(0);
    QVERIFY(!drop(main, {tab}, bodyPoint(main, 0.02, 0.5)));
    QVERIFY(!drop(main, {tab}, bodyPoint(main, 0.5, 0.5)));
    QCOMPARE(window->groups(), QList<EditorGroup *>{main});
    QCOMPARE(main->tab(0), tab);
    // A copy may: the tab stays and its copy opens beside.
    QVERIFY(drop(main, {tab}, bodyPoint(main, 0.98, 0.5), Qt::ControlModifier));
    QCOMPARE(window->groups().size(), 2);
    QCOMPARE(main->tab(0), tab);
    QCOMPARE(window->groups().value(1)->tab(0)->file(), tab->file());
}

void TestEditorGrid::ctrlDropCopies()
{
    MainWindow *source = makeWindow();
    QVERIFY(source->openFile(write(QStringLiteral("a.txt"), "alpha\n")));
    DocumentTab *tab = source->currentTab();
    MainWindow *target = makeWindow();
    target->newFile();
    EditorGroup *group = target->activeGroup();

    QVERIFY(drop(group, {tab}, stripPoint(group, 0), Qt::ControlModifier));
    QCOMPARE(source->tabCount(), 1);
    QCOMPARE(source->currentTab(), tab);
    QCOMPARE(target->tabCount(), 2);
    DocumentTab *copy = group->tab(0);
    QVERIFY(copy != tab);
    QCOMPARE(copy->file(), tab->file());
    QCOMPARE(target->currentTab(), copy);

    type(copy, QStringLiteral("beta"));
    QCOMPARE(tab->text(), QStringLiteral("alpha\nbeta"));
    QVERIFY(tab->isModified());
    // Closing the copy does not ask: the source still shows the document.
    QVERIFY(target->closeTabs(0, MainWindow::CloseScope::This));
    QCOMPARE(m_prompts, 0);
    QVERIFY(tab->otherViews().isEmpty());
    QCOMPARE(tab->text(), QStringLiteral("alpha\nbeta"));
}

void TestEditorGrid::copyIntoGroupShowingFileActivates()
{
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(write(QStringLiteral("a.txt"), "alpha\n")));
    DocumentTab *tab = window->currentTab();
    window->splitEditor(Split::Right);
    EditorGroup *right = window->activeGroup();
    DocumentTab *copy = right->tab(0);
    window->newFile();
    QCOMPARE(right->count(), 2);
    QCOMPARE(right->currentTab() != copy, true);

    QVERIFY(drop(right, {tab}, bodyPoint(right, 0.5, 0.5), Qt::ControlModifier));
    QCOMPARE(right->count(), 2);
    QCOMPARE(right->currentTab(), copy);
    QCOMPARE(window->tabCount(), 3);
    QCOMPARE(window->copyTab(tab, right, -1), copy);
    QCOMPARE(right->count(), 2);
}

void TestEditorGrid::ctrlDropInOwnStripMoves()
{
    MainWindow *window = makeWindow();
    for (int i = 0; i < 4; ++i)
        window->newFile();
    EditorGroup *group = window->activeGroup();
    const QList<DocumentTab *> all{group->tab(0), group->tab(1), group->tab(2), group->tab(3)};
    // Within the group a copy is a move (isMoveOperation()); a selection
    // keeps its order.
    window->dropTabs({all[0], all[2]}, group, 4, Split::None, true);
    QCOMPARE(window->tabs(), (QList<DocumentTab *>{all[1], all[3], all[0], all[2]}));
    window->dropTabs({all[0], all[2]}, group, 0, Split::None, false);
    QCOMPARE(window->tabs(), (QList<DocumentTab *>{all[0], all[2], all[1], all[3]}));
    QCOMPARE(window->currentTab(), all[2]);
}

void TestEditorGrid::moveIntoGroupShowingFileKeepsOne()
{
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(write(QStringLiteral("a.txt"), "alpha\n")));
    window->newFile();
    EditorGroup *main = window->activeGroup();
    DocumentTab *tab = main->tab(0);
    window->activateGroup(main);
    main->setCurrentIndex(0);
    window->splitEditor(Split::Right);
    EditorGroup *right = window->activeGroup();
    DocumentTab *copy = right->tab(0);
    type(tab, QStringLiteral("x"));

    // Moving the original onto the group with the copy: one tab is left
    // there, and nothing asks.
    QVERIFY(drop(right, {tab}, bodyPoint(right, 0.5, 0.5)));
    QCOMPARE(m_prompts, 0);
    QCOMPARE(right->count(), 1);
    QCOMPARE(right->tab(0), copy);
    QCOMPARE(main->count(), 1);
    QCOMPARE(window->currentTab(), copy);
    QCOMPARE(copy->text(), QStringLiteral("alpha\nx"));
}

void TestEditorGrid::ctrlClickTogglesSelection()
{
    MainWindow *window = makeWindow();
    for (int i = 0; i < 5; ++i)
        window->newFile();
    EditorGroup *group = window->activeGroup();
    const auto tab = [group](int i) { return group->tab(i); };

    group->clickTab(1, Qt::NoModifier);
    QCOMPARE(group->selectedTabs(), QList<DocumentTab *>{tab(1)});
    group->clickTab(3, Qt::ControlModifier);
    QCOMPARE(group->currentTab(), tab(3));
    QCOMPARE(group->selectedTabs(), (QList<DocumentTab *>{tab(1), tab(3)}));
    QCOMPARE(window->currentTab(), tab(3));
    // Ctrl+click on a selected tab unselects it...
    group->clickTab(1, Qt::ControlModifier);
    QCOMPARE(group->selectedTabs(), QList<DocumentTab *>{tab(3)});
    QCOMPARE(group->currentTab(), tab(3));
    // ...but not the last one.
    group->clickTab(3, Qt::ControlModifier);
    QCOMPARE(group->selectedTabs(), QList<DocumentTab *>{tab(3)});
    // Unselecting the current tab hands over to the most recent other one.
    group->clickTab(0, Qt::ControlModifier);
    group->clickTab(4, Qt::ControlModifier);
    QCOMPARE(group->selectedTabs(), (QList<DocumentTab *>{tab(0), tab(3), tab(4)}));
    group->clickTab(4, Qt::ControlModifier);
    QCOMPARE(group->currentTab(), tab(0));
    QCOMPARE(group->selectedTabs(), (QList<DocumentTab *>{tab(0), tab(3)}));
    QVERIFY(group->isSelected(tab(3)));
    QVERIFY(!group->isSelected(tab(4)));
}

void TestEditorGrid::shiftClickSelectsRange()
{
    MainWindow *window = makeWindow();
    for (int i = 0; i < 6; ++i)
        window->newFile();
    EditorGroup *group = window->activeGroup();
    const auto tab = [group](int i) { return group->tab(i); };

    // From the current tab.
    group->clickTab(2, Qt::NoModifier);
    group->clickTab(4, Qt::ShiftModifier);
    QCOMPARE(group->currentTab(), tab(4));
    QCOMPARE(group->selectedTabs(), (QList<DocumentTab *>{tab(2), tab(3), tab(4)}));
    // The anchor stays: back past it drops the run on the other side.
    group->clickTab(0, Qt::ShiftModifier);
    QCOMPARE(group->selectedTabs(), (QList<DocumentTab *>{tab(0), tab(1), tab(2)}));
    // A Ctrl+click moves the anchor.
    group->clickTab(5, Qt::ControlModifier);
    group->clickTab(4, Qt::ShiftModifier);
    QCOMPARE(group->selectedTabs(), (QList<DocumentTab *>{tab(0), tab(1), tab(2), tab(4), tab(5)}));
    QCOMPARE(group->currentTab(), tab(4));
}

void TestEditorGrid::plainClickClearsSelection()
{
    MainWindow *window = makeWindow();
    for (int i = 0; i < 4; ++i)
        window->newFile();
    EditorGroup *group = window->activeGroup();
    const auto tab = [group](int i) { return group->tab(i); };
    group->clickTab(0, Qt::NoModifier);
    group->clickTab(2, Qt::ShiftModifier);
    // Pressing a selected tab keeps the selection for a drag; the release
    // clears it.
    group->clickTab(1, Qt::NoModifier);
    QCOMPARE(group->currentTab(), tab(1));
    QCOMPARE(group->selectedTabs().size(), 3);
    group->releaseTab();
    QCOMPARE(group->selectedTabs(), QList<DocumentTab *>{tab(1)});
    // An unselected one clears it at once.
    group->clickTab(1, Qt::ShiftModifier);
    group->clickTab(3, Qt::ShiftModifier);
    QCOMPARE(group->selectedTabs().size(), 3);
    group->clickTab(0, Qt::NoModifier);
    QCOMPARE(group->selectedTabs(), QList<DocumentTab *>{tab(0)});
    // So does making another tab current any other way.
    group->clickTab(2, Qt::ControlModifier);
    group->setCurrentIndex(3);
    QCOMPARE(group->selectedTabs(), QList<DocumentTab *>{tab(3)});
}

void TestEditorGrid::mouseClicksSelect()
{
    MainWindow *window = makeWindow();
    for (int i = 0; i < 3; ++i)
        window->newFile();
    EditorGroup *group = window->activeGroup();
    QTabBar *bar = group->tabBar();
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(0).center());
    QCOMPARE(group->currentIndex(), 0);
    QTest::mouseClick(bar, Qt::LeftButton, Qt::ControlModifier, bar->tabRect(2).center());
    QCOMPARE(group->currentIndex(), 2);
    QCOMPARE(group->selectedTabs(), (QList<DocumentTab *>{group->tab(0), group->tab(2)}));
    // From the Ctrl+clicked anchor; the other selected tab stays.
    QTest::mouseClick(bar, Qt::LeftButton, Qt::ShiftModifier, bar->tabRect(1).center());
    QCOMPARE(group->selectedTabs().size(), 3);
    QCOMPARE(group->currentIndex(), 1);
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->tabRect(2).center());
    QCOMPARE(group->selectedTabs(), QList<DocumentTab *>{group->tab(2)});
}

void TestEditorGrid::draggingSelectionMovesAll()
{
    MainWindow *window = makeWindow();
    for (int i = 0; i < 4; ++i)
        window->newFile();
    EditorGroup *main = window->activeGroup();
    const QList<DocumentTab *> all{main->tab(0), main->tab(1), main->tab(2), main->tab(3)};
    EditorGroup *right = window->addGroup(main, Split::Right);
    window->activateGroup(right);
    window->newFile();
    DocumentTab *there = right->tab(0);

    main->clickTab(0, Qt::NoModifier);
    main->clickTab(2, Qt::ControlModifier);
    const QList<DocumentTab *> selected = main->selectedTabs();
    QCOMPARE(selected, (QList<DocumentTab *>{all[0], all[2]}));
    // Onto the other group's strip, before its tab, in order.
    QVERIFY(drop(right, selected, stripPoint(right, 0)));
    QCOMPARE(right->count(), 3);
    QCOMPARE(right->tab(0), all[0]);
    QCOMPARE(right->tab(1), all[2]);
    QCOMPARE(right->tab(2), there);
    QCOMPARE(main->count(), 2);
    QCOMPARE(window->currentTab(), all[2]);

    // Into a split below the main group.
    window->dropTabs({all[1], all[3]}, right, -1, Split::Down, false);
    QCOMPARE(window->groups().size(), 2);
    EditorGroup *below = window->groups().value(1);
    QVERIFY(below != main && below != right);
    QCOMPARE(below->count(), 2);
    QCOMPARE(below->tab(0), all[1]);
    QCOMPARE(below->tab(1), all[3]);
}

void TestEditorGrid::selectionDroppedOutsideOpensOneWindow()
{
    MainWindow *window = makeWindow();
    for (int i = 0; i < 3; ++i)
        window->newFile();
    EditorGroup *group = window->activeGroup();
    DocumentTab *a = group->tab(0);
    DocumentTab *b = group->tab(1);
    const auto before = SessionManager::instance().windows();
    Q_EMIT group->tabsDroppedOutside({a, b}, QPoint(100, 100), false);
    const auto after = SessionManager::instance().windows();
    QCOMPARE(after.size(), before.size() + 1);
    MainWindow *created = after.last();
    m_made.append(created);
    QCOMPARE(created->tabs(), (QList<DocumentTab *>{a, b}));
    QCOMPARE(window->tabCount(), 1);
}

void TestEditorGrid::copyOutsideKeepsSources()
{
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(write(QStringLiteral("a.txt"), "alpha\n")));
    QVERIFY(window->openFile(write(QStringLiteral("b.txt"), "beta\n")));
    EditorGroup *group = window->activeGroup();
    DocumentTab *a = group->tab(0);
    DocumentTab *b = group->tab(1);
    Q_EMIT group->tabsDroppedOutside({a, b}, QPoint(100, 100), true);
    MainWindow *created = SessionManager::instance().windows().last();
    QVERIFY(created != window);
    m_made.append(created);
    QCOMPARE(window->tabs(), (QList<DocumentTab *>{a, b}));
    QCOMPARE(created->tabCount(), 2);
    QCOMPARE(created->tabs().at(0)->file(), a->file());
    QCOMPARE(created->tabs().at(1)->file(), b->file());
    QVERIFY(window->isVisible());
}

void TestEditorGrid::contextMenuSplitsSelection()
{
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(write(QStringLiteral("a.txt"), "alpha\n")));
    QVERIFY(window->openFile(write(QStringLiteral("b.txt"), "beta\n")));
    window->newFile();
    EditorGroup *group = window->activeGroup();
    group->clickTab(0, Qt::NoModifier);
    group->clickTab(1, Qt::ShiftModifier);

    // On a selected tab: the whole selection.
    triggerMenuItem(QStringLiteral("Split Down"));
    Q_EMIT group->tabContextMenuRequested(1, window->mapToGlobal(QPoint(50, 50)));
    QCOMPARE(window->groups().size(), 2);
    EditorGroup *below = window->groups().value(1);
    QCOMPARE(below->count(), 2);
    QCOMPARE(below->tab(0)->file(), group->tab(0)->file());
    QCOMPARE(below->tab(1)->file(), group->tab(1)->file());
    QCOMPARE(group->count(), 3);
    QCOMPARE(root(window)->orientation(), Qt::Vertical);

    // On a tab outside it: that tab only.
    triggerMenuItem(QStringLiteral("Copy to New Window"));
    Q_EMIT group->tabContextMenuRequested(2, window->mapToGlobal(QPoint(50, 50)));
    MainWindow *created = SessionManager::instance().windows().last();
    QVERIFY(created != window);
    m_made.append(created);
    QCOMPARE(created->tabCount(), 1);
    QCOMPARE(created->currentTab()->file(), group->tab(2)->file());
    QCOMPARE(group->count(), 3);

    triggerMenuItem(QStringLiteral("Move to New Window"));
    Q_EMIT below->tabContextMenuRequested(0, window->mapToGlobal(QPoint(50, 50)));
    created = SessionManager::instance().windows().last();
    m_made.append(created);
    QCOMPARE(created->tabCount(), 1);
    QCOMPARE(below->count(), 1);
}

void TestEditorGrid::gridSessionRoundTrip()
{
    Settings::instance().setValue(key::restoreSession, true);
    const QString a = write(QStringLiteral("a.txt"), "a\n");
    const QString b = write(QStringLiteral("b.txt"), "b\n");
    const QString c = write(QStringLiteral("c.txt"), "c\n");
    const QString d = write(QStringLiteral("d.txt"), "d\n");
    {
        // Root horizontal [A {a, b}, vertical [B {c}, C {d, a}]], C active.
        MainWindow *window = makeWindow();
        window->resize(1000, 700);
        QVERIFY(window->openFile(a));
        QVERIFY(window->openFile(b));
        EditorGroup *first = window->activeGroup();
        first->setCurrentIndex(0);
        EditorGroup *second = window->addGroup(first, Split::Right);
        EditorGroup *third = window->addGroup(second, Split::Down);
        window->activateGroup(second);
        QVERIFY(window->openFile(c));
        window->activateGroup(third);
        QVERIFY(window->openFile(d));
        window->copyTab(first->tab(0), third, -1);
        third->setCurrentIndex(0);
        root(window)->setSizes({300, 700});
        qobject_cast<EvenSplitter *>(second->parentWidget())->setSizes({200, 400});
        QVERIFY(SessionManager::instance().quit());
        QCOMPARE(m_prompts, 0);
        cleanup();
    }
    QVERIFY(SessionManager::instance().restore());
    MainWindow *window = SessionManager::instance().windows().value(0);
    QVERIFY(window);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    const QList<EditorGroup *> groups = window->groups();
    QCOMPARE(groups.size(), 3);
    const auto paths = [](EditorGroup *group) {
        QStringList result;
        for (int i = 0; i < group->count(); ++i)
            result.append(group->tab(i)->path());
        return result;
    };
    QCOMPARE(paths(groups[0]), (QStringList{a, b}));
    QCOMPARE(paths(groups[1]), QStringList{c});
    QCOMPARE(paths(groups[2]), (QStringList{d, a}));
    QCOMPARE(groups[0]->currentIndex(), 0);
    QCOMPARE(groups[2]->currentIndex(), 0);
    QCOMPARE(window->activeGroup(), groups[2]);
    // One document for both tabs on a.txt.
    QCOMPARE(groups[2]->tab(1)->file(), groups[0]->tab(0)->file());
    QCOMPARE(root(window)->orientation(), Qt::Horizontal);
    auto *branch = qobject_cast<EvenSplitter *>(groups[1]->parentWidget());
    QVERIFY(branch && branch != root(window));
    QCOMPARE(branch->orientation(), Qt::Vertical);
    QCOMPARE(groups[2]->parentWidget(), branch);
    // The proportions survive.
    const QList<int> outer = root(window)->sizes();
    QVERIFY(outer[0] * 2 < outer[1]);
    const QList<int> inner = branch->sizes();
    QVERIFY(inner[0] * 3 / 2 < inner[1]);
}

void TestEditorGrid::oldSessionLoadsIntoOneGroup()
{
    Settings::instance().setValue(key::restoreSession, true);
    const QString a = write(QStringLiteral("a.txt"), "a\n");
    const QString b = write(QStringLiteral("b.txt"), "b\n");
    // As written before the grid: tabs and the current one, no "grid".
    QJsonArray tabs;
    for (const QString &path : {a, b}) {
        QJsonObject entry;
        entry[QStringLiteral("path")] = path;
        tabs.append(entry);
    }
    QJsonObject window;
    window[QStringLiteral("tabs")] = tabs;
    window[QStringLiteral("current")] = 1;
    QJsonObject rootObject;
    rootObject[QStringLiteral("version")] = 1;
    rootObject[QStringLiteral("windows")] = QJsonArray{window};
    QVERIFY(QDir().mkpath(SessionManager::dataDir()));
    QFile file(SessionManager::dataDir() + QStringLiteral("/session.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(rootObject).toJson());
    file.close();

    QVERIFY(SessionManager::instance().restore());
    MainWindow *restored = SessionManager::instance().windows().value(0);
    QVERIFY(restored);
    QCOMPARE(restored->groups().size(), 1);
    QCOMPARE(restored->tabCount(), 2);
    QCOMPARE(restored->tabs().at(0)->path(), a);
    QCOMPARE(restored->currentTab()->path(), b);
}

void TestEditorGrid::newCommandsHaveNoConflicts()
{
    for (const char *id : {"view.splitEditorUp", "view.splitEditorDown", "view.splitEditorLeft",
                           "view.splitEditorRight", "view.focusNextGroup", "view.focusPreviousGroup",
                           "view.copyToNewWindow", "view.splitEditor"}) {
        QVERIFY2(command(QLatin1String(id)), id);
        QVERIFY2(shortcutConflicts(QLatin1String(id)).isEmpty(), id);
    }
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestEditorGrid)
#include "test_editor_grid.moc"
