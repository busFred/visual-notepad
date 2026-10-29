// Session restore, crash recovery, recent files and auto save. Settings and the session data
// directory live in a temporary directory; modal prompts are watched by a
// timer that answers Discard and counts them.
#include "DocumentTab.h"
#include "MainWindow.h"
#include "Session.h"
#include "Settings.h"
#include "Theme.h"
#include "editor/TextView.h"

#include <QApplication>
#include <QDir>
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
    void quitPrompts();
    void recoveryOff();
    void recentFiles();
    void encodingNameIsRestored();
    void missingFileIsSkipped();
    void autoSaveAfterDelay();
    void autoSaveOnFocusChange();
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
