// Session restore, crash recovery, recent files and auto save. Settings and the session data
// directory live in a temporary directory; modal prompts are watched by a
// timer that answers Discard and counts them.
#include "DocumentTab.h"
#include "EditorGroup.h"
#include "MainWindow.h"
#include "Session.h"
#include "Settings.h"
#include "TextFile.h"
#include "Theme.h"
#include "WelcomeView.h"
#include "editor/TextView.h"

#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/stat.h>
#endif

using namespace vn;
using Mode = DocumentTab::ViewMode;

class TestSession : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();
    void roundTrip();
    void crashRecovery();
    void discardedTabIsNotRecovered();
    void closeOthersDropsBackupsAtOnce();
    void unchangedBackupIsNotRewritten();
    void quitPrompts();
    void recoveryOff();
    void recentFiles();
    void welcomeFollowsRecentFiles();
    void encodingNameIsRestored();
    void indentationIsRestored();
    void missingFileIsSkipped();
    void bigFilesRestoreInTheBackground();
    void crashWhileRestoringKeepsTheSession();
    void fileChangedWhileRestoringIsReloaded();
    void closingAWindowWhileRestoring();
    void autoSaveAfterDelay();
    void autoSaveOnFocusChange();
    void recoveryDataIsPrivate();
#ifdef Q_OS_UNIX
    // Last: the session stays in its quitting state afterwards.
    void sigtermBacksUp();
#endif

private:
    MainWindow *makeWindow();
    QString write(const QString &name, const QByteArray &content);
    static QByteArray read(const QString &path);
    static void append(DocumentTab *tab, const QString &text);
    static int cursorLine(DocumentTab *tab);
    static QByteArray bigText(const QByteArray &prefix);
    static QStringList pathsOf(EditorGroup *group);
    // The paths session.json lists for its first window, in grid order.
    static QList<QStringList> storedGroups();
    int backupCount() const;
    void closeWindows();

    QTemporaryDir m_dir;
    QTimer m_watcher;
    int m_prompts = 0;
    QList<QPointer<MainWindow>> m_made;
};

void TestSession::initTestCase()
{
    theme::apply(*qApp);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    SessionManager::instance().start();
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

void TestSession::init()
{
    QVERIFY(m_dir.isValid());
    m_prompts = 0;
    const QString root = m_dir.filePath(QString::number(QTest::currentTestFailed()) + QTest::currentTestFunction());
    QVERIFY(QDir().mkpath(root));
    Settings::instance().useFile(root + QStringLiteral("/settings.ini"));
    SessionManager::setDataDir(root + QStringLiteral("/data"));
}

void TestSession::cleanup()
{
    closeWindows();
}

void TestSession::closeWindows()
{
    for (MainWindow *window : SessionManager::instance().windows())
        delete window;
    for (const QPointer<MainWindow> &window : std::as_const(m_made))
        delete window.data();
    m_made.clear();
}

MainWindow *TestSession::makeWindow()
{
    auto *window = new MainWindow;
    m_made.append(window);
    window->show();
    return window;
}

QString TestSession::write(const QString &name, const QByteArray &content)
{
    const QString path = m_dir.filePath(QTest::currentTestFunction() + QStringLiteral("-") + name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
        return {};
    return path;
}

QByteArray TestSession::read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void TestSession::append(DocumentTab *tab, const QString &text)
{
    QTextCursor cursor(tab->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
}

int TestSession::cursorLine(DocumentTab *tab)
{
    return tab->editor()->textCursor().blockNumber();
}

// Big enough to load off the GUI thread.
QByteArray TestSession::bigText(const QByteArray &prefix)
{
    QByteArray text;
    for (int i = 0; text.size() < TextFile::largeLoadBytes + 100'000; ++i)
        text += prefix + " line " + QByteArray::number(i) + " with some words to make it longer\n";
    return text;
}

QStringList TestSession::pathsOf(EditorGroup *group)
{
    QStringList paths;
    for (int i = 0; i < group->count(); ++i)
        paths.append(group->tab(i)->path());
    return paths;
}

QList<QStringList> TestSession::storedGroups()
{
    const QJsonObject window = QJsonDocument::fromJson(read(SessionManager::dataDir() + QStringLiteral("/session.json")))
                                   .object()
                                   .value(QStringLiteral("windows"))
                                   .toArray()
                                   .at(0)
                                   .toObject();
    const QJsonArray tabs = window.value(QStringLiteral("tabs")).toArray();
    QList<QStringList> groups;
    const auto collect = [&](const auto &self, const QJsonObject &node) -> void {
        if (node.contains(QStringLiteral("tabs"))) {
            QStringList paths;
            for (const QJsonValue &id : node.value(QStringLiteral("tabs")).toArray())
                paths.append(tabs.at(id.toInt()).toObject().value(QStringLiteral("path")).toString());
            groups.append(paths);
        }
        for (const QJsonValue &child : node.value(QStringLiteral("children")).toArray())
            self(self, child.toObject());
    };
    collect(collect, window.value(QStringLiteral("grid")).toObject().value(QStringLiteral("root")).toObject());
    return groups;
}

int TestSession::backupCount() const
{
    return QDir(SessionManager::dataDir() + QStringLiteral("/backups")).entryList(QDir::Files).size();
}

void TestSession::roundTrip()
{
    Settings::instance().setValue(key::restoreSession, true);
    const QString a = write("a.md", "one\ntwo\nthree\nfour\n");
    const QString b = write("b.txt", "x\ny\nz\n");
    const QString c = write("c.md", "# c\n\nlast\n");

    MainWindow *w1 = makeWindow();
    QVERIFY(w1->openFile(a));
    QVERIFY(w1->openFile(b));
    w1->tabs().at(0)->setViewMode(Mode::Source);
    w1->tabs().at(0)->goToLine(3);
    w1->tabs().at(1)->goToLine(2);
    w1->tabs().at(1)->setLanguage(QStringLiteral("Markdown"));
    w1->setCurrentTabIndex(0);
    MainWindow *w2 = makeWindow();
    QVERIFY(w2->openFile(c));
    w2->tabs().at(0)->setViewMode(Mode::SourceVisual);
    w2->tabs().at(0)->goToLine(3);

    QVERIFY(SessionManager::instance().quit());
    QCOMPARE(m_prompts, 0);
    closeWindows();
    QVERIFY(QFile::exists(SessionManager::dataDir() + QStringLiteral("/session.json")));

    QVERIFY(SessionManager::instance().restore());
    const QList<MainWindow *> windows = SessionManager::instance().windows();
    QCOMPARE(windows.size(), 2);
    const QList<DocumentTab *> first = windows.at(0)->tabs();
    QCOMPARE(first.size(), 2);
    QCOMPARE(first.at(0)->path(), a);
    QCOMPARE(first.at(1)->path(), b);
    QCOMPARE(first.at(0)->viewMode(), Mode::Source);
    QCOMPARE(cursorLine(first.at(0)), 2);
    QCOMPARE(cursorLine(first.at(1)), 1);
    QVERIFY(first.at(1)->languageOverridden());
    QCOMPARE(first.at(1)->definitionName(), QStringLiteral("Markdown"));
    QVERIFY(!first.at(0)->languageOverridden());
    QCOMPARE(windows.at(0)->currentTabIndex(), 0);
    const QList<DocumentTab *> second = windows.at(1)->tabs();
    QCOMPARE(second.size(), 1);
    QCOMPARE(second.at(0)->path(), c);
    QCOMPARE(second.at(0)->viewMode(), Mode::SourceVisual);
    QCOMPARE(cursorLine(second.at(0)), 2);
}

void TestSession::crashRecovery()
{
    const QString f = write("f.txt", "a\r\nb\r\n");
    const QString clean = write("clean.txt", "c\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(f));
    QVERIFY(window->openFile(clean));
    DocumentTab *file = window->tabs().at(0);
    QCOMPARE(file->lineEnding(), DocumentTab::LineEnding::CRLF);
    append(file, QStringLiteral("edit"));
    window->newFile();
    append(window->tabs().at(2), QStringLiteral("scratch"));
    window->newFile(); // Empty untitled: nothing to keep.

    // Backed up shortly after the edits, without quitting.
    QTRY_COMPARE_WITH_TIMEOUT(backupCount(), 2, 5000);
    QCOMPARE(read(f), QByteArray("a\r\nb\r\n"));
    closeWindows(); // A crash: no close events, no prompts.
    QCOMPARE(m_prompts, 0);

    // Only the unsaved work comes back while restoreSession is off.
    QVERIFY(SessionManager::instance().restore());
    const QList<MainWindow *> windows = SessionManager::instance().windows();
    QCOMPARE(windows.size(), 1);
    const QList<DocumentTab *> tabs = windows.at(0)->tabs();
    QCOMPARE(tabs.size(), 2);
    QCOMPARE(tabs.at(0)->path(), f);
    QVERIFY(tabs.at(0)->isModified());
    QCOMPARE(tabs.at(0)->text(), QStringLiteral("a\nb\nedit"));
    QCOMPARE(tabs.at(0)->lineEnding(), DocumentTab::LineEnding::CRLF);
    QVERIFY(tabs.at(1)->path().isEmpty());
    QVERIFY(tabs.at(1)->isModified());
    QCOMPARE(tabs.at(1)->text(), QStringLiteral("scratch"));

    QVERIFY(tabs.at(0)->save(f));
    QCOMPARE(read(f), QByteArray("a\r\nb\r\nedit"));
}

void TestSession::discardedTabIsNotRecovered()
{
    MainWindow *window = makeWindow();
    window->newFile();
    append(window->tabs().at(0), QStringLiteral("keep"));
    window->newFile();
    append(window->tabs().at(1), QStringLiteral("throw away"));
    QTRY_COMPARE_WITH_TIMEOUT(backupCount(), 2, 5000);

    QVERIFY(window->closeTabs(1, MainWindow::CloseScope::This)); // The watcher discards.
    QCOMPARE(m_prompts, 1);
    QCOMPARE(backupCount(), 1); // Gone at once, not at the next backup.
    closeWindows();

    QVERIFY(SessionManager::instance().restore());
    const QList<DocumentTab *> tabs = SessionManager::instance().windows().at(0)->tabs();
    QCOMPARE(tabs.size(), 1);
    QCOMPARE(tabs.at(0)->text(), QStringLiteral("keep"));
}

void TestSession::closeOthersDropsBackupsAtOnce()
{
    MainWindow *window = makeWindow();
    for (const QString &text : {QStringLiteral("keep"), QStringLiteral("drop 1"), QStringLiteral("drop 2")}) {
        window->newFile();
        append(window->tabs().last(), text);
    }
    QTRY_COMPARE_WITH_TIMEOUT(backupCount(), 3, 5000);

    // One session write for the batch, done before closeTabs returns.
    QVERIFY(window->closeTabs(0, MainWindow::CloseScope::Others)); // The watcher discards.
    QCOMPARE(m_prompts, 2);
    QCOMPARE(window->tabCount(), 1);
    QCOMPARE(backupCount(), 1);
    closeWindows();

    QVERIFY(SessionManager::instance().restore());
    const QList<DocumentTab *> tabs = SessionManager::instance().windows().at(0)->tabs();
    QCOMPARE(tabs.size(), 1);
    QCOMPARE(tabs.at(0)->text(), QStringLiteral("keep"));
}

void TestSession::unchangedBackupIsNotRewritten()
{
    auto &session = SessionManager::instance();
    MainWindow *window = makeWindow();
    window->newFile();
    DocumentTab *tab = window->tabs().at(0);
    append(tab, QStringLiteral("one"));
    QVERIFY(session.saveSession().isEmpty());
    const QString name = tab->file()->backupName();
    QVERIFY(!name.isEmpty());
    const QString path = SessionManager::dataDir() + QStringLiteral("/backups/") + name;
    QCOMPARE(read(path), QByteArray("one"));

    // Overwritten behind the session's back, so a rewrite would show.
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("marker");
    }
    QVERIFY(session.saveSession().isEmpty());
    QCOMPARE(read(path), QByteArray("marker"));

    // Edited: written again, under the same name.
    append(tab, QStringLiteral(" two"));
    QVERIFY(session.saveSession().isEmpty());
    QCOMPARE(tab->file()->backupName(), name);
    QCOMPARE(read(path), QByteArray("one two"));

    // Gone (removed as stale or by hand): written again although unchanged.
    QVERIFY(QFile::remove(path));
    QVERIFY(session.saveSession().isEmpty());
    QCOMPARE(read(path), QByteArray("one two"));
}

void TestSession::quitPrompts()
{
    const QString f = write("f.txt", "a\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(f));
    append(window->tabs().at(0), QStringLiteral("edit"));
    window->newFile();
    append(window->tabs().at(1), QStringLiteral("scratch"));
    window->newFile(); // Empty untitled never prompts.
    QTRY_COMPARE_WITH_TIMEOUT(backupCount(), 2, 5000);

    QVERIFY(SessionManager::instance().quit());
    QCOMPARE(m_prompts, 2);
    QCOMPARE(read(f), QByteArray("a\n"));
    QCOMPARE(backupCount(), 0);
    closeWindows();
    // Discarded on purpose: the next start opens a new file instead.
    QVERIFY(!SessionManager::instance().restore());
}

void TestSession::recoveryOff()
{
    Settings::instance().setValue(key::restoreUnsaved, false);
    MainWindow *window = makeWindow();
    window->newFile();
    append(window->tabs().at(0), QStringLiteral("scratch"));
    QTest::qWait(2500);
    QCOMPARE(backupCount(), 0);
    closeWindows();
    QVERIFY(!SessionManager::instance().restore());
}

void TestSession::recentFiles()
{
    Settings &settings = Settings::instance();
    settings.clearRecentFiles();
    const QString a = write("a.txt", "a\n");
    const QString b = write("b.txt", "b\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(a));
    QCOMPARE(settings.recentFiles(), QStringList{a});

    settings.setValue(key::rememberRecent, false);
    QVERIFY(settings.recentFiles().isEmpty());
    QVERIFY(window->openFile(b));
    QVERIFY(settings.recentFiles().isEmpty());

    settings.setValue(key::rememberRecent, true);
    const QString c = write("c.txt", "c\n");
    QVERIFY(window->openFile(c));
    QCOMPARE(settings.recentFiles(), QStringList{c});
}

void TestSession::welcomeFollowsRecentFiles()
{
    Settings &settings = Settings::instance();
    settings.clearRecentFiles();
    MainWindow *window = makeWindow(); // No tabs: the welcome page shows.
    // The page is built when the event loop first finds the group empty.
    WelcomeView *welcome = nullptr;
    QTRY_VERIFY((welcome = window->findChild<WelcomeView *>()) && welcome->isVisible());
    auto headings = [welcome] {
        int count = 0;
        for (QLabel *label : welcome->findChildren<QLabel *>(QStringLiteral("WelcomeHeading")))
            count += label->text() == QLatin1String("Recent");
        return count;
    };
    QCOMPARE(headings(), 0);
    settings.addRecentFile(write("a.txt", "a\n"));
    QTRY_COMPARE(headings(), 1);
    settings.clearRecentFiles(); // File > Open Recent > Clear Recently Opened.
    QTRY_COMPARE(headings(), 0);
}

void TestSession::encodingNameIsRestored()
{
    MainWindow *window = makeWindow();
    window->newFile();
    append(window->tabs().at(0), QStringLiteral("日本語"));
    window->tabs().at(0)->setEncoding(QStringLiteral("Shift_JIS"));
    window->newFile();
    append(window->tabs().at(1), QStringLiteral("abc"));
    window->tabs().at(1)->setEncoding(encoding::utf8Bom);

    SessionManager::instance().saveSession();
    closeWindows();
    QVERIFY(SessionManager::instance().restore());
    const QList<DocumentTab *> tabs = SessionManager::instance().windows().at(0)->tabs();
    QCOMPARE(tabs.size(), 2);
    QCOMPARE(tabs.at(0)->encoding(), QStringLiteral("Shift_JIS"));
    QCOMPARE(tabs.at(0)->text(), QStringLiteral("日本語"));
    QCOMPARE(tabs.at(1)->encoding(), encoding::utf8Bom);
    SessionManager::instance().saveSession();
}

void TestSession::indentationIsRestored()
{
    MainWindow *window = makeWindow();
    window->newFile();
    append(window->tabs().at(0), QStringLiteral("a\n  b\n  c\n"));
    window->tabs().at(0)->setIndentation({3, false}); // chosen by the user
    window->newFile();
    append(window->tabs().at(1), QStringLiteral("a\n\tb\n\tc\n")); // detected, not saved as an override

    SessionManager::instance().saveSession();
    closeWindows();
    QVERIFY(SessionManager::instance().restore());
    const QList<DocumentTab *> tabs = SessionManager::instance().windows().at(0)->tabs();
    QCOMPARE(tabs.size(), 2);
    QVERIFY(tabs.at(0)->indentationOverridden());
    QCOMPARE(tabs.at(0)->indentation(), (indentation::Settings{3, false}));
    // Content arrives with the restore, so a tab without an override detects.
    QVERIFY(!tabs.at(1)->indentationOverridden());
    QCOMPARE(tabs.at(1)->indentation().insertSpaces, false);
    SessionManager::instance().saveSession();
}

void TestSession::missingFileIsSkipped()
{
    Settings::instance().setValue(key::restoreSession, true);
    const QString gone = write("gone.txt", "x\n");
    const QString kept = write("kept.txt", "y\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(gone));
    QVERIFY(window->openFile(kept));
    QVERIFY(SessionManager::instance().quit());
    closeWindows();
    QVERIFY(QFile::remove(gone));

    QVERIFY(SessionManager::instance().restore());
    const QList<DocumentTab *> tabs = SessionManager::instance().windows().at(0)->tabs();
    QCOMPARE(tabs.size(), 1);
    QCOMPARE(tabs.at(0)->path(), kept);
}

// Big files of a session load off the GUI thread: restore() returns with
// the small ones, and each big one's tab comes once it has loaded, in its
// place (group, order, current tab) with its mode, language, caret and
// scroll position. The session meanwhile keeps them where they were.
void TestSession::bigFilesRestoreInTheBackground()
{
    Settings::instance().setValue(key::restoreSession, true);
    const QString a = write("a.md", "# a\n");
    const QString big1 = write("big1.md", bigText("first"));
    const QString c = write("c.txt", "c\n");
    const QString big2 = write("big2.txt", bigText("second"));
    const QString d = write("d.txt", "d\n");

    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(a));
    QVERIFY(window->openFile(big1));
    QTRY_COMPARE_WITH_TIMEOUT(window->tabCount(), 2, 10000);
    QVERIFY(window->openFile(c));
    EditorGroup *right = window->addGroup(window->mainGroup(), MainWindow::SplitDirection::Right);
    window->activateGroup(right);
    QVERIFY(window->openFile(big2));
    QTRY_COMPARE_WITH_TIMEOUT(window->tabCount(), 4, 10000);
    QVERIFY(window->openFile(d));
    DocumentTab *tab1 = window->mainGroup()->tab(1);
    QCOMPARE(tab1->path(), big1);
    window->mainGroup()->setCurrentIndex(1);
    tab1->setViewMode(Mode::Source);
    tab1->setLanguage(QStringLiteral("Python"));
    tab1->goToLine(3000);
    const int top1 = tab1->sourceView()->topLine();
    QVERIFY(top1 > 0);
    right->setCurrentIndex(0); // big2
    window->mainGroup()->setCurrentIndex(0);
    window->activateGroup(window->mainGroup());
    QVERIFY(SessionManager::instance().quit());
    closeWindows();

    QVERIFY(SessionManager::instance().restore());
    const QList<MainWindow *> windows = SessionManager::instance().windows();
    QCOMPARE(windows.size(), 1);
    MainWindow *restored = windows.at(0);
    // The small files are there at once, the big ones are loading.
    QCOMPARE(restored->groups().size(), 2);
    QCOMPARE(pathsOf(restored->groups().at(0)), (QStringList{a, c}));
    QCOMPARE(pathsOf(restored->groups().at(1)), (QStringList{d}));
    QVERIFY(TextFile::isLoadingPath(big1));
    QVERIFY(TextFile::isLoadingPath(big2));
    // restore() wrote the session again: with the loading tabs in place.
    QCOMPARE(storedGroups(), (QList<QStringList>{{a, big1, c}, {big2, d}}));

    QTRY_COMPARE_WITH_TIMEOUT(restored->tabCount(), 5, 10000);
    QCOMPARE(pathsOf(restored->groups().at(0)), (QStringList{a, big1, c}));
    QCOMPARE(pathsOf(restored->groups().at(1)), (QStringList{big2, d}));
    // Current tabs as they were.
    QCOMPARE(restored->groups().at(0)->currentTab()->path(), a);
    QCOMPARE(restored->groups().at(1)->currentTab()->path(), big2);
    DocumentTab *back1 = restored->groups().at(0)->tab(1);
    QCOMPARE(back1->viewMode(), Mode::Source);
    QVERIFY(back1->languageOverridden());
    QCOMPARE(back1->definitionName(), QStringLiteral("Python"));
    QCOMPARE(cursorLine(back1), 2999);
    QVERIFY(!back1->isModified());
    QCOMPARE(back1->text().toUtf8(), read(big1));
    QTRY_COMPARE(back1->sourceView()->topLine(), top1);
    QCOMPARE(restored->groups().at(1)->tab(0)->text().toUtf8(), read(big2));
}

// A session written while its big files are still loading has them, so a
// crash (or quit) then loses no tab; they come back on the next start.
void TestSession::crashWhileRestoringKeepsTheSession()
{
    Settings::instance().setValue(key::restoreSession, true);
    const QString small = write("small.txt", "small\n");
    const QString big = write("big.md", bigText("big"));
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(big));
    QTRY_COMPARE_WITH_TIMEOUT(window->tabCount(), 1, 10000);
    QVERIFY(window->openFile(small));
    QVERIFY(SessionManager::instance().quit());
    closeWindows();

    for (int round = 0; round < 2; ++round) {
        QVERIFY(SessionManager::instance().restore());
        QCOMPARE(SessionManager::instance().windows().at(0)->tabCount(), 1);
        QVERIFY(TextFile::isLoadingPath(big));
        // Saved while loading; then a crash: no close events, the load stops.
        SessionManager::instance().saveSession();
        closeWindows();
        QVERIFY(!TextFile::isLoadingPath(big));
        QCOMPARE(storedGroups(), (QList<QStringList>{{big, small}}));
        QCOMPARE(read(big), bigText("big")); // Never written to.
    }
    QVERIFY(SessionManager::instance().restore());
    MainWindow *restored = SessionManager::instance().windows().at(0);
    QTRY_COMPARE_WITH_TIMEOUT(restored->tabCount(), 2, 10000);
    QCOMPARE(pathsOf(restored->mainGroup()), (QStringList{big, small}));
    QCOMPARE(restored->tabs().at(0)->text().toUtf8(), bigText("big"));
}

// A file changed on disk while it loads for the restore ends up with what
// is on disk now.
void TestSession::fileChangedWhileRestoringIsReloaded()
{
    Settings::instance().setValue(key::restoreSession, true);
    const QString big = write("changing.md", bigText("before"));
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(big));
    QTRY_COMPARE_WITH_TIMEOUT(window->tabCount(), 1, 10000);
    QVERIFY(SessionManager::instance().quit());
    closeWindows();

    QVERIFY(SessionManager::instance().restore());
    QVERIFY(TextFile::isLoadingPath(big));
    const QByteArray after = bigText("after, longer");
    QCOMPARE(write("changing.md", after), big);
    MainWindow *restored = SessionManager::instance().windows().at(0);
    QTRY_COMPARE_WITH_TIMEOUT(restored->tabCount(), 1, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(restored->tabs().at(0)->text().toUtf8(), after, 10000);
    QVERIFY(!restored->tabs().at(0)->isModified());
}

// Closing a window whose big files still load stops their loads, but not one
// another window restores too; that window gets its tab.
void TestSession::closingAWindowWhileRestoring()
{
    Settings::instance().setValue(key::restoreSession, true);
    const QString shared = write("shared.md", bigText("shared"));
    const QString own = write("own.md", bigText("own"));
    MainWindow *first = makeWindow();
    QVERIFY(first->openFile(shared));
    QVERIFY(first->openFile(own));
    QTRY_COMPARE_WITH_TIMEOUT(first->tabCount(), 2, 10000);
    MainWindow *second = makeWindow();
    QVERIFY(second->openFile(shared));
    QCOMPARE(second->tabCount(), 1);
    QVERIFY(SessionManager::instance().quit());
    closeWindows();

    QVERIFY(SessionManager::instance().restore());
    const QList<MainWindow *> windows = SessionManager::instance().windows();
    QCOMPARE(windows.size(), 2);
    QVERIFY(windows.at(0)->isOpening());
    delete windows.at(0);
    QVERIFY(!TextFile::isLoadingPath(own));
    QVERIFY(TextFile::isLoadingPath(shared));
    MainWindow *left = SessionManager::instance().windows().at(0);
    QTRY_COMPARE_WITH_TIMEOUT(left->tabCount(), 1, 10000);
    QCOMPARE(left->tabs().at(0)->path(), shared);
    QCOMPARE(left->tabs().at(0)->text().toUtf8(), bigText("shared"));
    QTest::qWait(50); // The stopped load reports nothing.
    QCOMPARE(left->tabCount(), 1);
}

void TestSession::autoSaveAfterDelay()
{
    Settings::instance().setValue(key::autoSave, QStringLiteral("afterDelay"));
    const QString f = write("f.txt", "x\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(f));
    append(window->tabs().at(0), QStringLiteral("Y"));
    QCOMPARE(read(f), QByteArray("x\n"));
    QTRY_COMPARE_WITH_TIMEOUT(read(f), QByteArray("x\nY"), 4000);
    QVERIFY(!window->tabs().at(0)->isModified());

    window->newFile();
    append(window->tabs().at(1), QStringLiteral("untitled"));
    QTest::qWait(1500);
    QVERIFY(window->tabs().at(1)->isModified());

    // Changing the setting applies to edits already waiting.
    Settings::instance().setValue(key::autoSave, QStringLiteral("off"));
    append(window->tabs().at(0), QStringLiteral("Z"));
    QTest::qWait(1500);
    QCOMPARE(read(f), QByteArray("x\nY"));
    Settings::instance().setValue(key::autoSave, QStringLiteral("afterDelay"));
    QTRY_COMPARE_WITH_TIMEOUT(read(f), QByteArray("x\nYZ"), 4000);
}

void TestSession::autoSaveOnFocusChange()
{
    Settings::instance().setValue(key::autoSave, QStringLiteral("onFocusChange"));
    const QString a = write("a.txt", "a\n");
    const QString b = write("b.txt", "b\n");
    MainWindow *window = makeWindow();
    QVERIFY(window->openFile(a));
    QVERIFY(window->openFile(b));
    window->setCurrentTabIndex(0);
    append(window->tabs().at(0), QStringLiteral("1"));
    QCOMPARE(read(a), QByteArray("a\n"));
    window->setCurrentTabIndex(1);
    QCOMPARE(read(a), QByteArray("a\n1"));

    append(window->tabs().at(1), QStringLiteral("2"));
    QCOMPARE(read(b), QByteArray("b\n"));
    QEvent deactivate(QEvent::WindowDeactivate);
    QCoreApplication::sendEvent(window, &deactivate);
    QCOMPARE(read(b), QByteArray("b\n2"));
    QCOMPARE(m_prompts, 0);
}

#ifdef Q_OS_UNIX
void TestSession::recoveryDataIsPrivate()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix modes; elsewhere the per-user folder's ACL applies");
#else
    const auto mode = [](const QString &path) {
        struct stat info {};
        return ::stat(QFile::encodeName(path).constData(), &info) == 0 ? int(info.st_mode & 07777) : -1;
    };
    const QString data = SessionManager::dataDir();
    const QString backups = data + QStringLiteral("/backups");
    // A data folder left world-readable by an older version or a umask.
    QVERIFY(QDir().mkpath(backups));
    QVERIFY(::chmod(QFile::encodeName(data).constData(), 0755) == 0);
    QVERIFY(::chmod(QFile::encodeName(backups).constData(), 0777) == 0);
    const mode_t oldMask = ::umask(0); // Files would be 0666 by default.

    auto &session = SessionManager::instance();
    MainWindow *window = makeWindow();
    window->newFile();
    DocumentTab *tab = window->tabs().at(0);
    append(tab, QStringLiteral("secret"));
    const bool saved = session.saveSession().isEmpty();
    ::umask(oldMask);
    QVERIFY(saved);
    const QString backup = backups + u'/' + tab->file()->backupName();
    QCOMPARE(read(backup), QByteArray("secret"));
    QCOMPARE(mode(data), 0700);
    QCOMPARE(mode(backups), 0700);
    QCOMPARE(mode(backup), 0600);
    QCOMPARE(mode(data + QStringLiteral("/session.json")), 0600);

    // Created afresh: private from the start.
    closeWindows();
    QVERIFY(QDir(data).removeRecursively());
    window = makeWindow();
    window->newFile();
    tab = window->tabs().at(0);
    append(tab, QStringLiteral("again"));
    QVERIFY(session.saveSession().isEmpty());
    QCOMPARE(mode(data), 0700);
    QCOMPARE(mode(backups), 0700);
    QCOMPARE(mode(backups + u'/' + tab->file()->backupName()), 0600);
#endif
}

void TestSession::sigtermBacksUp()
{
    MainWindow *window = makeWindow();
    window->newFile();
    append(window->tabs().at(0), QStringLiteral("typed just now"));
    QCOMPARE(backupCount(), 0); // Faster than the backup timer.

    QVERIFY(::raise(SIGTERM) == 0);
    QTRY_VERIFY_WITH_TIMEOUT(SessionManager::instance().windows().isEmpty(), 2000);
    QCOMPARE(m_prompts, 0);
    QCOMPARE(backupCount(), 1);
}
#endif

QTEST_MAIN(TestSession)
#include "test_session.moc"
