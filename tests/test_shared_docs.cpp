// One shared document per file across tabs and windows, and watching files
// for changes on disk. Prompts are answered by a timer that clicks Discard
// and counts them; the save conflict prompt goes through
// MainWindow::setSaveConflictHandler.
#include "DocumentTab.h"
#include "EditorGroup.h"
#include "MainWindow.h"
#include "Session.h"
#include "Settings.h"
#include "TextFile.h"
#include "Theme.h"
#include "editor/TextView.h"
#include "markdown/VisualView.h"

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>

using namespace vn;
using Choice = MainWindow::SaveConflictChoice;

class TestSharedDocs : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();
    void twoTabsShareEdits();
    void saveFromOneClearsBoth();
    void closePromptsOnlyForLastView();
    void windowClosePromptsOnlyForLastViews();
    void openingAgainSharesAcrossWindows();
    void untitledIsNotShared();
    void undoWithTwoViews();
    void markdownViewsShareParser();
    void externalChangeReloadsWhenClean();
    void atomicExternalWriteReloads();
    void windowActivationChecksDisk();
    void dirtyIsNotReloaded();
    void conflictOnSave();
    void conflictWithoutNotification();
    void ownSaveDoesNotReload();
    void deletedFileIsOrphaned();
    void saveAsMovesAllViews();
    void saveAsOverOpenFileFails();
    void movingTabAcrossWindowsKeepsDocument();
    void hotExitRestoresSharedDocument();

private:
    QString write(const QString &name, const QByteArray &content);
    static bool overwrite(const QString &path, const QByteArray &content);
    static QByteArray read(const QString &path);
    static void type(DocumentTab *tab, const QString &text);
    static QString tabText(MainWindow &window, DocumentTab *tab);
    MainWindow *makeWindow();
    static void save(DocumentTab *tab);

    QTemporaryDir m_dir;
    QTimer m_watcher;
    int m_prompts = 0;
    QList<QPointer<MainWindow>> m_made;
};

void TestSharedDocs::initTestCase()
{
    QVERIFY(m_dir.isValid());
    theme::apply(*qApp);
    QGuiApplication::setQuitOnLastWindowClosed(false);
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

void TestSharedDocs::init()
{
    m_prompts = 0;
    const QString root = m_dir.filePath(QStringLiteral("data-") + QTest::currentTestFunction());
    QVERIFY(QDir().mkpath(root));
    Settings::instance().useFile(root + QStringLiteral("/settings.ini"));
    SessionManager::setDataDir(root + QStringLiteral("/data"));
    MainWindow::setSaveConflictHandler({});
}

void TestSharedDocs::cleanup()
{
    MainWindow::setSaveConflictHandler({});
    for (const QPointer<MainWindow> &window : std::as_const(m_made))
        delete window.data();
    m_made.clear();
    for (MainWindow *window : SessionManager::instance().windows())
        delete window;
    // Tabs closed with deleteLater.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(TextFile::all().isEmpty());
}

MainWindow *TestSharedDocs::makeWindow()
{
    auto *window = new MainWindow;
    m_made.append(window);
    window->show();
    return window;
}

// File > Save with the tab current, through its shortcut.
void TestSharedDocs::save(DocumentTab *tab)
{
    auto *window = qobject_cast<MainWindow *>(tab->window());
    QVERIFY(window);
    window->activateGroup(window->groupOf(tab));
    window->groupOf(tab)->setCurrentIndex(window->groupOf(tab)->indexOf(tab));
    window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(window));
    QTest::keySequence(tab->sourceView(), QKeySequence::Save);
}

QString TestSharedDocs::write(const QString &name, const QByteArray &content)
{
    const QString path = m_dir.filePath(QTest::currentTestFunction() + QStringLiteral("-") + name);
    return overwrite(path, content) ? path : QString();
}

// In place, like `echo > file`.
bool TestSharedDocs::overwrite(const QString &path, const QByteArray &content)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
}

QByteArray TestSharedDocs::read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void TestSharedDocs::type(DocumentTab *tab, const QString &text)
{
    TextView *view = tab->sourceView();
    QTextCursor end(tab->document());
    end.movePosition(QTextCursor::End);
    view->setTextCursor(end);
    QTest::keyClicks(view, text);
}

QString TestSharedDocs::tabText(MainWindow &window, DocumentTab *tab)
{
    EditorGroup *group = window.groupOf(tab);
    return group ? group->tabBar()->tabText(group->indexOf(tab)) : QString();
}

// Two tabs on one file: one document, so edits and the modified state show
// in both, and in both titles.
void TestSharedDocs::twoTabsShareEdits()
{
    const QString path = write(QStringLiteral("a.txt"), "alpha\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(path));
    DocumentTab *first = window->currentTab();
    window->activateGroup(window->addGroup(window->mainGroup(), vn::dragGeometry::SplitDirection::Right));
    DocumentTab *second = first->openAnotherView();
    window->adoptTab(second);
    QCOMPARE(window->groups().value(1)->tab(0), second);
    QCOMPARE(second->file(), first->file());
    QCOMPARE(second->document(), first->document());
    QCOMPARE(first->otherViews(), QList<DocumentTab *>{second});

    type(first, QStringLiteral("beta"));
    QCOMPARE(second->text(), QStringLiteral("alpha\nbeta"));
    QVERIFY(first->isModified());
    QVERIFY(second->isModified());
    QVERIFY(tabText(*window, first).startsWith(QStringLiteral("● ")));
    QVERIFY(tabText(*window, second).startsWith(QStringLiteral("● ")));

    // And the other way round, through the other tab's own view.
    type(second, QStringLiteral("!"));
    QCOMPARE(first->text(), QStringLiteral("alpha\nbeta!"));
    // Opening the file again still just activates its tab.
    window->activateGroup(window->mainGroup());
    QVERIFY(window->openFile(path));
    QCOMPARE(window->tabCount(), 2);
}

void TestSharedDocs::saveFromOneClearsBoth()
{
    const QString path = write(QStringLiteral("a.txt"), "alpha\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(path));
    DocumentTab *first = window->currentTab();
    DocumentTab *second = first->openAnotherView();
    window->adoptTab(second);
    type(first, QStringLiteral("x"));
    QSignalSpy firstModified(first, &DocumentTab::modificationChanged);
    QCOMPARE(window->currentTab(), second);
    save(second);
    QCOMPARE(read(path), QByteArray("alpha\nx"));
    QVERIFY(!first->isModified());
    QVERIFY(!second->isModified());
    QCOMPARE(firstModified.count(), 1);
    QCOMPARE(tabText(*window, first), QFileInfo(path).fileName());
    QCOMPARE(tabText(*window, second), QFileInfo(path).fileName());
    QCOMPARE(m_prompts, 0);
}

void TestSharedDocs::closePromptsOnlyForLastView()
{
    const QString path = write(QStringLiteral("a.txt"), "alpha\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(path));
    DocumentTab *first = window->currentTab();
    QPointer<DocumentTab> second = first->openAnotherView();
    window->adoptTab(second);
    type(first, QStringLiteral("x"));

    // Not the last tab of the document: no question.
    QVERIFY(window->closeTabs(window->mainGroup()->indexOf(first), MainWindow::CloseScope::This));
    QCOMPARE(m_prompts, 0);
    QCOMPARE(window->tabCount(), 1);
    QVERIFY(second->isModified());
    QCOMPARE(second->text(), QStringLiteral("alpha\nx"));
    QVERIFY(second->otherViews().isEmpty());

    // The last one asks (and the timer discards).
    QVERIFY(window->closeTabs(0, MainWindow::CloseScope::This));
    QCOMPARE(m_prompts, 1);
    QCOMPARE(window->tabCount(), 0);
    QCOMPARE(read(path), QByteArray("alpha\n"));
}

void TestSharedDocs::windowClosePromptsOnlyForLastViews()
{
    const QString path = write(QStringLiteral("a.txt"), "alpha\n");
    MainWindow *one = makeWindow();
    MainWindow *two = makeWindow();
    QVERIFY(one->openFile(path));
    DocumentTab *first = one->currentTab();
    // Two tabs on it in the second window, one in the first.
    QVERIFY(two->openFile(path));
    two->adoptTab(two->currentTab()->openAnotherView());
    type(first, QStringLiteral("x"));

    // Still shown in the first window: the second closes without asking.
    QVERIFY(two->close());
    QCOMPARE(m_prompts, 0);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(first->otherViews().isEmpty());
    QVERIFY(first->isModified());

    // Two tabs, one document, last window: asked once.
    one->adoptTab(first->openAnotherView());
    QVERIFY(SessionManager::instance().prepareQuit());
    QCOMPARE(m_prompts, 1);
}

// Opening a file that another window shows: a new tab on the same document.
void TestSharedDocs::openingAgainSharesAcrossWindows()
{
    const QString path = write(QStringLiteral("a.txt"), "alpha\n");
    MainWindow *one = makeWindow();
    MainWindow *two = makeWindow();
    QVERIFY(one->openFile(path));
    // Another spelling of the same path.
    QVERIFY(two->openFile(QFileInfo(path).absolutePath() + QStringLiteral("/./") + QFileInfo(path).fileName()));
    QCOMPARE(two->currentTab()->file(), one->currentTab()->file());
    QCOMPARE(TextFile::find(path).get(), one->currentTab()->file());
}

void TestSharedDocs::untitledIsNotShared()
{
    MainWindow *window = makeWindow();
    window->newFile();
    window->newFile();
    QVERIFY(window->mainGroup()->tab(0)->file() != window->mainGroup()->tab(1)->file());
    QVERIFY(window->mainGroup()->tab(0)->otherViews().isEmpty());
}

// Undo is the document's, as in VS Code: either view undoes the last edit,
// and a view that did not record the edit still restores a caret.
void TestSharedDocs::undoWithTwoViews()
{
    const QString path = write(QStringLiteral("a.txt"), "alpha\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(path));
    DocumentTab *first = window->currentTab();
    DocumentTab *second = first->openAnotherView();
    window->adoptTab(second);
    type(first, QStringLiteral("one"));
    type(second, QStringLiteral(" two"));
    QCOMPARE(first->text(), QStringLiteral("alpha\none two"));

    // One step of the shared stack, from the view that did not make it;
    // its caret goes to the undone change.
    first->sourceView()->undo();
    QVERIFY(QStringLiteral("alpha\none two").startsWith(second->text()));
    QVERIFY(second->text().size() < QStringLiteral("alpha\none two").size());
    QCOMPARE(first->sourceView()->textCursor().position(), first->text().size());
    // The rest from the other view, down to the saved state.
    while (second->document()->isUndoAvailable())
        second->sourceView()->undo();
    QCOMPARE(first->text(), QStringLiteral("alpha\n"));
    QVERIFY(!first->isModified());
    first->sourceView()->redo();
    QVERIFY(first->text().startsWith(QStringLiteral("alpha\no")));
    QVERIFY(second->isModified());
}

// Visual views of one Markdown file use one parser; a language change drops
// every tab's visual half before the parser goes.
void TestSharedDocs::markdownViewsShareParser()
{
    const QString path = write(QStringLiteral("a.md"), "# Title\n\ntext [x](#nowhere)\n");
    Settings::instance().setValue(key::validateLinks, true);
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(path));
    DocumentTab *first = window->currentTab();
    first->setViewMode(DocumentTab::ViewMode::Visual);
    DocumentTab *second = first->openAnotherView();
    window->adoptTab(second);
    QCOMPARE(second->viewMode(), DocumentTab::ViewMode::Visual);
    QVERIFY(first->visualView() && second->visualView());
    QCOMPARE(first->file()->markdownModel(), second->file()->markdownModel());
    // Link diagnostics are the file's, shown by both tabs.
    QTRY_COMPARE(first->file()->diagnostics().size(), 1);

    // Per-tab view mode.
    second->setViewMode(DocumentTab::ViewMode::Source);
    QCOMPARE(first->viewMode(), DocumentTab::ViewMode::Visual);
    second->setViewMode(DocumentTab::ViewMode::SourceVisual);
    QTextCursor(second->document()).insertText(QStringLiteral("intro\n"));
    QVERIFY(first->text().startsWith(QStringLiteral("intro\n# Title")));

    first->setLanguage(QString());
    QVERIFY(!first->isMarkdown());
    QVERIFY(!second->isMarkdown());
    QVERIFY(!first->visualView());
    QVERIFY(!second->visualView());
    QVERIFY(second->languageOverridden());
    QTRY_VERIFY(first->file()->diagnostics().isEmpty());
    first->setLanguage(QStringLiteral("Markdown"));
    QVERIFY(first->visualView());
    QVERIFY(second->visualView());
}

// A clean document follows the disk, as one undoable edit of the changed
// part only, so carets elsewhere stay where they were.
void TestSharedDocs::externalChangeReloadsWhenClean()
{
    const QString path = write(QStringLiteral("a.txt"), "one\ntwo\nthree\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(path));
    DocumentTab *first = window->currentTab();
    DocumentTab *second = first->openAnotherView();
    window->adoptTab(second);
    QTextCursor caret(first->document()->findBlockByNumber(2));
    caret.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, 2);
    first->sourceView()->setTextCursor(caret);

    QVERIFY(overwrite(path, "ONE!\ntwo\nthree\n"));
    QTRY_COMPARE(first->text(), QStringLiteral("ONE!\ntwo\nthree\n"));
    QCOMPARE(second->text(), first->text());
    QVERIFY(!first->isModified());
    QVERIFY(!first->file()->isChangedOnDisk());
    const QTextCursor after = first->sourceView()->textCursor();
    QCOMPARE(after.blockNumber(), 2);
    QCOMPARE(after.positionInBlock(), 2);
    // VS Code's reload is an edit too: undo brings the old text back.
    QVERIFY(first->document()->isUndoAvailable());

    // Line endings follow the disk.
    QVERIFY(overwrite(path, "ONE!\r\ntwo\r\n"));
    QTRY_COMPARE(first->text(), QStringLiteral("ONE!\ntwo\n"));
    QCOMPARE(first->lineEnding(), DocumentTab::LineEnding::CRLF);
    QVERIFY(!first->isModified());
}

// Editors that save by writing a new file and renaming it over the old one
// leave the watch on the old file; the next change must still be seen.
void TestSharedDocs::atomicExternalWriteReloads()
{
    const QString path = write(QStringLiteral("a.txt"), "first\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(path));
    DocumentTab *tab = window->currentTab();
    for (const QByteArray &content : {QByteArray("second version\n"), QByteArray("third\n")}) {
        QSaveFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(content);
        QVERIFY(file.commit());
        QTRY_COMPARE(tab->text().toUtf8(), content);
    }
    QVERIFY(!tab->file()->isOrphaned());
}

void TestSharedDocs::windowActivationChecksDisk()
{
    const QString path = write(QStringLiteral("a.txt"), "first\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(path));
    DocumentTab *tab = window->currentTab();
    // Showing activates the window, which checks too: let that pass first.
    window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(window));
    QTest::qWait(200);
    // As if a change notification had been missed: the file's idea of the
    // disk is stale and nothing on disk moves. Activation looks again.
    const TextFile::DiskState actual = tab->file()->diskBaseline();
    TextFile::DiskState stale = actual;
    stale.size += 1;
    stale.hash = QByteArray(20, 'x');
    tab->file()->setDiskBaseline(stale);
    QTest::qWait(200);
    QCOMPARE(tab->file()->diskBaseline().hash, stale.hash);
    QEvent activate(QEvent::WindowActivate);
    QCoreApplication::sendEvent(window, &activate);
    QTRY_COMPARE(tab->file()->diskBaseline().hash, actual.hash);
    QCOMPARE(tab->text(), QStringLiteral("first\n"));
    QVERIFY(!tab->isModified());
}

void TestSharedDocs::dirtyIsNotReloaded()
{
    const QString path = write(QStringLiteral("a.txt"), "alpha\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(path));
    DocumentTab *tab = window->currentTab();
    type(tab, QStringLiteral("mine"));
    QSignalSpy diskState(tab->file(), &TextFile::diskStateChanged);
    QVERIFY(overwrite(path, "theirs, longer\n"));
    QTRY_VERIFY(tab->file()->isChangedOnDisk());
    QCOMPARE(tab->text(), QStringLiteral("alpha\nmine"));
    QVERIFY(tab->isModified());
    QCOMPARE(diskState.count(), 1);
}

// Saving over a change on disk asks: Cancel keeps both, Overwrite writes
// ours, Revert takes theirs.
void TestSharedDocs::conflictOnSave()
{
    const QString path = write(QStringLiteral("a.txt"), "alpha\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(path));
    DocumentTab *tab = window->currentTab();
    DocumentTab *other = tab->openAnotherView();
    window->adoptTab(other);
    int asked = 0;
    Choice answer = Choice::Cancel;
    MainWindow::setSaveConflictHandler([&](DocumentTab *) {
        ++asked;
        return answer;
    });

    type(tab, QStringLiteral("mine"));
    QVERIFY(overwrite(path, "theirs, longer\n"));
    QTRY_VERIFY(tab->file()->isChangedOnDisk());

    save(other);
    QCOMPARE(asked, 1);
    QCOMPARE(read(path), QByteArray("theirs, longer\n"));
    QVERIFY(tab->isModified());

    answer = Choice::Overwrite;
    save(other);
    QCOMPARE(asked, 2);
    QCOMPARE(read(path), QByteArray("alpha\nmine"));
    QVERIFY(!tab->isModified());
    QVERIFY(!tab->file()->isChangedOnDisk());

    // Again, and this time take the disk's version.
    type(tab, QStringLiteral(" again"));
    QVERIFY(overwrite(path, "theirs again\n"));
    QTRY_VERIFY(tab->file()->isChangedOnDisk());
    answer = Choice::Revert;
    save(other);
    QCOMPARE(asked, 3);
    QCOMPARE(read(path), QByteArray("theirs again\n"));
    QCOMPARE(tab->text(), QStringLiteral("theirs again\n"));
    QCOMPARE(other->text(), QStringLiteral("theirs again\n"));
    QVERIFY(!tab->isModified());
    QVERIFY(!tab->file()->isChangedOnDisk());
    QCOMPARE(m_prompts, 0);

    // Without a change on disk there is nothing to ask.
    type(tab, QStringLiteral("x"));
    save(other);
    QCOMPARE(asked, 3);
    QCOMPARE(read(path), QByteArray("theirs again\nx"));
}

// The save compares the disk itself, notification or not; a touch that
// leaves the content alone is no conflict.
void TestSharedDocs::conflictWithoutNotification()
{
    const QString path = write(QStringLiteral("a.txt"), "alpha\n");
    const std::shared_ptr<TextFile> file = TextFile::create();
    QVERIFY(file->load(path));
    QTextCursor(file->document()).insertText(QStringLiteral("mine "));

    QVERIFY(overwrite(path, "alpha\n"));
    QVERIFY(file->save(path));
    QCOMPARE(read(path), QByteArray("mine alpha\n"));

    QTextCursor(file->document()).insertText(QStringLiteral("more "));
    QVERIFY(overwrite(path, "changed\n"));
    QVERIFY(!file->save(path));
    QVERIFY(file->hasSaveConflict());
    QVERIFY(!file->errorString().isEmpty());
    QCOMPARE(read(path), QByteArray("changed\n"));
    QVERIFY(file->save(path, true));
    QVERIFY(!file->hasSaveConflict());
    QCOMPARE(read(path), QByteArray("more mine alpha\n"));
}

void TestSharedDocs::ownSaveDoesNotReload()
{
    const QString path = write(QStringLiteral("a.txt"), "alpha\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(path));
    DocumentTab *tab = window->currentTab();
    type(tab, QStringLiteral("one"));
    const int steps = tab->document()->availableUndoSteps();
    save(tab);
    QVERIFY(!tab->isModified());
    QSignalSpy changed(tab->file(), &TextFile::contentsChanged);
    QSignalSpy diskState(tab->file(), &TextFile::diskStateChanged);
    QTest::qWait(400);
    QCOMPARE(changed.count(), 0);
    QCOMPARE(diskState.count(), 0);
    QCOMPARE(tab->document()->availableUndoSteps(), steps);

    // Saved twice and still watched: an outside change after it is seen.
    type(tab, QStringLiteral(" two"));
    save(tab);
    changed.clear();
    QTest::qWait(300);
    QCOMPARE(changed.count(), 0);
    QVERIFY(!tab->isModified());
    QVERIFY(overwrite(path, "outside\n"));
    QTRY_COMPARE(tab->text(), QStringLiteral("outside\n"));
}

void TestSharedDocs::deletedFileIsOrphaned()
{
    const QString path = write(QStringLiteral("gone.txt"), "alpha\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(path));
    DocumentTab *tab = window->currentTab();
    QVERIFY(QFile::remove(path));
    QTRY_VERIFY(tab->file()->isOrphaned());
    QCOMPARE(tabText(*window, tab), QStringLiteral("%1 (deleted)").arg(QFileInfo(path).fileName()));
    // The text stays; nothing was edited.
    QCOMPARE(tab->text(), QStringLiteral("alpha\n"));
    QVERIFY(!tab->isModified());

    // Back with the same content: just not deleted any more.
    QVERIFY(overwrite(path, "alpha\n"));
    QTRY_VERIFY(!tab->file()->isOrphaned());
    QCOMPARE(tabText(*window, tab), QFileInfo(path).fileName());
    // Watched again: a later change reloads.
    QVERIFY(overwrite(path, "beta, longer\n"));
    QTRY_COMPARE(tab->text(), QStringLiteral("beta, longer\n"));

    // Saving a deleted file writes it again.
    QVERIFY(QFile::remove(path));
    QTRY_VERIFY(tab->file()->isOrphaned());
    QVERIFY(tab->save(path));
    QVERIFY(!tab->file()->isOrphaned());
    QCOMPARE(read(path), QByteArray("beta, longer\n"));
}

// Save As moves the document, and so every tab on it, to the new path.
void TestSharedDocs::saveAsMovesAllViews()
{
    const QString path = write(QStringLiteral("a.txt"), "alpha\n");
    const QString target = m_dir.filePath(QStringLiteral("saveAsMovesAllViews-b.md"));
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(path));
    DocumentTab *first = window->currentTab();
    DocumentTab *second = first->openAnotherView();
    window->adoptTab(second);
    type(first, QStringLiteral("# x"));
    QVERIFY(!first->isMarkdown());
    QVERIFY(second->save(target));
    QCOMPARE(first->path(), target);
    QCOMPARE(second->path(), target);
    QVERIFY(first->isMarkdown());
    QVERIFY(!first->isModified());
    QCOMPARE(tabText(*window, first), QStringLiteral("saveAsMovesAllViews-b.md"));
    QCOMPARE(TextFile::find(target).get(), first->file());
    QVERIFY(!TextFile::find(path));
    QCOMPARE(read(path), QByteArray("alpha\n"));
    // The old file is not watched for this document any more.
    QVERIFY(overwrite(path, "changed\n"));
    QTest::qWait(300);
    QCOMPARE(first->text(), QStringLiteral("alpha\n# x"));
}

void TestSharedDocs::saveAsOverOpenFileFails()
{
    const QString pathA = write(QStringLiteral("a.txt"), "alpha\n");
    const QString pathB = write(QStringLiteral("b.txt"), "beta\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(pathA));
    QVERIFY(window->openFile(pathB));
    DocumentTab *b = window->currentTab();
    DocumentTab *a = window->mainGroup()->tab(0);
    QVERIFY(!a->save(pathB));
    QVERIFY(!a->file()->hasSaveConflict());
    QCOMPARE(read(pathB), QByteArray("beta\n"));
    QCOMPARE(a->path(), pathA);
    QCOMPARE(b->text(), QStringLiteral("beta\n"));
}

void TestSharedDocs::movingTabAcrossWindowsKeepsDocument()
{
    const QString path = write(QStringLiteral("a.txt"), "alpha\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(path));
    DocumentTab *first = window->currentTab();
    DocumentTab *second = first->openAnotherView();
    window->adoptTab(second);
    type(first, QStringLiteral("x"));
    const int before = int(SessionManager::instance().windows().size());

    window->moveTabToNewWindow(second);
    QCOMPARE(int(SessionManager::instance().windows().size()), before + 1);
    auto *other = qobject_cast<MainWindow *>(second->window());
    QVERIFY(other && other != window);
    m_made.append(other);
    QCOMPARE(second->file(), first->file());
    QCOMPARE(first->otherViews(), QList<DocumentTab *>{second});
    QVERIFY(tabText(*other, second).startsWith(QStringLiteral("● ")));

    type(second, QStringLiteral("y"));
    QCOMPARE(first->text(), QStringLiteral("alpha\nxy"));
    save(second);
    QVERIFY(!first->isModified());
    QCOMPARE(tabText(*window, first), QFileInfo(path).fileName());

    // And back into the first window's side group.
    vn::EditorGroup *side = window->addGroup(window->mainGroup(), vn::dragGeometry::SplitDirection::Right);
    window->receiveTab(second, side, -1);
    QCOMPARE(window->groupOf(second), side);
    QCOMPARE(second->file(), first->file());
}

// Hot exit with two tabs on one modified file: one backup, and one
// document again after the restart.
void TestSharedDocs::hotExitRestoresSharedDocument()
{
    SessionManager &session = SessionManager::instance();
    const QString path = write(QStringLiteral("a.txt"), "alpha\n");
    {
        MainWindow *window = makeWindow();
        QVERIFY(window->openFile(path));
        DocumentTab *first = window->currentTab();
        window->adoptTab(first->openAnotherView());
        type(first, QStringLiteral("unsaved"));
        QVERIFY(session.saveSession().isEmpty());
        const QDir backups(SessionManager::dataDir() + QStringLiteral("/backups"));
        QCOMPARE(backups.entryList(QDir::Files).size(), 1);
        delete window;
        QVERIFY(TextFile::all().isEmpty());
    }
    // Changed on disk while the app was not running.
    QVERIFY(overwrite(path, "changed while away\n"));

    QVERIFY(session.restore());
    QCOMPARE(session.windows().size(), 1);
    MainWindow *window = session.windows().constFirst();
    QCOMPARE(window->tabCount(), 2);
    const QList<DocumentTab *> tabs = window->tabs();
    QCOMPARE(tabs[0]->file(), tabs[1]->file());
    QCOMPARE(tabs[0]->text(), QStringLiteral("alpha\nunsaved"));
    QVERIFY(tabs[0]->isModified());
    QTRY_VERIFY(tabs[0]->file()->isChangedOnDisk());
}

QTEST_MAIN(TestSharedDocs)
#include "test_shared_docs.moc"
