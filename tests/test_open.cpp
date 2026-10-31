// Opening big files without blocking the window: TextFile::loadAsync() and
// MainWindow::openFile() for files from TextFile::largeLoadBytes up.
#include "DocumentTab.h"
#include "Encodings.h"
#include "MainWindow.h"
#include "Settings.h"
#include "TextFile.h"
#include "Theme.h"
#include "editor/TextView.h"

#include <QElapsedTimer>
#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStringEncoder>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>

#include <memory>

using vn::DocumentTab;
using vn::TextFile;
namespace enc = vn::encoding;

namespace {

// Lines of varied text, a bit over `bytes` of UTF-8: ASCII, accents, CJK,
// tabs, indentation, a Markdown heading now and then, blank lines.
QByteArray sampleText(qsizetype bytes, const QByteArray &eol = "\n")
{
    QByteArray out;
    out.reserve(bytes + 200);
    for (int i = 0; out.size() < bytes; ++i) {
        switch (i % 7) {
        case 0:
            out += "## Heading " + QByteArray::number(i) + eol;
            break;
        case 1:
            out += "plain line " + QByteArray::number(i) + " with caf\xc3\xa9 and na\xc3\xaf" "ve text" + eol;
            break;
        case 2:
            out += "\tindented with a tab, \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e " + QByteArray::number(i) + eol;
            break;
        case 3:
            out += eol;
            break;
        case 4:
            out += "    four spaces `code` and **bold** and [link](https://example.com/" + QByteArray::number(i) + ")"
                   + eol;
            break;
        case 5:
            out += "emoji \xf0\x9f\x98\x80 pair " + QByteArray::number(i) + eol;
            break;
        default:
            out += "- item " + QByteArray::number(i) + eol;
            break;
        }
    }
    return out;
}

// A document's blocks with their text and highlighting formats.
struct Snapshot
{
    QString text;
    int blocks = 0;
    QList<QList<QTextLayout::FormatRange>> formats;
};

Snapshot formatsOf(QTextDocument *doc)
{
    Snapshot s;
    s.blocks = doc->blockCount();
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        s.formats.append(b.layout()->formats());
    return s;
}

} // namespace

class TestOpen : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void loadsLikeLoad_data();
    void loadsLikeLoad();
    void eventLoopRunsWhileLoading();
    void savesByteIdentically_data();
    void savesByteIdentically();
    void failureIsReported();
    void cancelStopsTheWorker();
    void destroyingTheFileMidLoadIsSafe();
    void highlightsLikeLoad();
    void mainWindowOpensLargeFilesInOrder();
    void mainWindowOpensTheSameFileOnce();
    void closingTheWindowMidLoadIsSafe();
    void largeFileOfAnotherWindowIsShared();

private:
    QString write(const QString &name, const QByteArray &content);
    static QByteArray read(const QString &path);

    QTemporaryDir m_dir;
};

void TestOpen::initTestCase()
{
    QVERIFY(m_dir.isValid());
    vn::theme::apply(*qApp);
}

QString TestOpen::write(const QString &name, const QByteArray &content)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
        return {};
    return path;
}

QByteArray TestOpen::read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void TestOpen::loadsLikeLoad_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<QString>("encoding");
    QTest::addColumn<int>("crlf");

    QTest::newRow("10MB-lf") << sampleText(10'000'000) << enc::utf8 << 0;
    QTest::newRow("crlf") << sampleText(1'500'000, "\r\n") << enc::utf8 << 1;
    QTest::newRow("bom") << (QByteArray("\xEF\xBB\xBF") + sampleText(1'200'000)) << enc::utf8Bom << 0;
    QStringEncoder utf16(QStringEncoder::Utf16LE);
    QTest::newRow("utf16le") << (QByteArray("\xFF\xFE") + QByteArray(utf16(QString::fromUtf8(sampleText(1'200'000)))))
                             << enc::utf16LE << 0;
    // Not UTF-8: decoded with the legacy encoding.
    QTest::newRow("legacy") << (sampleText(1'200'000) + QByteArray("caf\xe9 \x93quoted\x94\n")) << QString() << 0;
    QTest::newRow("no-final-newline") << (sampleText(1'200'000) + "last") << enc::utf8 << 0;
    QTest::newRow("one-long-line") << QByteArray("word ").repeated(300'000) << enc::utf8 << 0;
    // Lone carriage returns and Unicode separators stay what they are.
    QTest::newRow("odd-separators") << (sampleText(1'100'000) + QByteArray("a\rb\xe2\x80\xa8" "c\xe2\x80\xa9" "d\n"))
                                    << enc::utf8 << 0;
}

void TestOpen::loadsLikeLoad()
{
    QFETCH(QByteArray, bytes);
    QFETCH(QString, encoding);
    QFETCH(int, crlf);
    const QString path = write(QStringLiteral("big.md"), bytes);
    QVERIFY(!path.isEmpty());

    auto sync = TextFile::create();
    QVERIFY(sync->load(path));
    QVERIFY(sync->path() == path);
    const QString syncText = sync->text();
    const int blocks = sync->document()->blockCount();
    const auto syncEncoding = sync->encoding();
    const auto syncEol = sync->lineEnding();
    const auto syncBaseline = sync->diskBaseline();
    if (!encoding.isEmpty())
        QCOMPARE(sync->encoding(), encoding);
    QCOMPARE(sync->lineEnding() == TextFile::LineEnding::CRLF, crlf == 1);

    auto async = TextFile::create();
    QSignalSpy finished(async.get(), &TextFile::loadFinished);
    QSignalSpy reset(async.get(), &TextFile::contentReset);
    async->loadAsync(path);
    QVERIFY(async->isLoading());
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
    QVERIFY(finished.first().first().toBool());
    QVERIFY(!async->isLoading());
    QCOMPARE(reset.size(), 1);
    QVERIFY(async->errorString().isEmpty());

    QCOMPARE(async->text(), syncText);
    QCOMPARE(async->document()->blockCount(), blocks);
    QCOMPARE(async->encoding(), syncEncoding);
    QCOMPARE(async->lineEnding(), syncEol);
    QCOMPARE(async->diskBaseline().hash, syncBaseline.hash);
    QCOMPARE(async->diskBaseline().size, syncBaseline.size);
    QCOMPARE(async->diskBaseline().modified, syncBaseline.modified);
    QVERIFY(!async->isModified());
    QVERIFY(!async->document()->isUndoAvailable());
    QVERIFY(async->document()->isUndoRedoEnabled());
    QCOMPARE(async->path(), path);
    QCOMPARE(async->definitionName(), sync->definitionName());
    QCOMPARE(async->isHighlighted(), sync->isHighlighted());
    QCOMPARE(async->indentation().tabSize, sync->indentation().tabSize);
    QCOMPARE(async->indentation().insertSpaces, sync->indentation().insertSpaces);
    for (int n : {0, blocks / 3, blocks / 2, blocks - 1}) {
        QCOMPARE(async->document()->findBlockByNumber(n).text(), sync->document()->findBlockByNumber(n).text());
        QCOMPARE(async->document()->findBlockByNumber(n).position(), sync->document()->findBlockByNumber(n).position());
    }

    // It is a normal file afterwards: editable, undoable, modified by edits.
    QTextCursor cursor(async->document());
    cursor.insertText(QStringLiteral("x"));
    QVERIFY(async->isModified());
    QVERIFY(async->document()->isUndoAvailable());
    async->document()->undo();
    QCOMPARE(async->text(), syncText);
    QVERIFY(!async->isModified());
}

// The point of it: timers keep firing and input keeps being handled while the
// worker builds the document, and the half-built file reveals nothing.
void TestOpen::eventLoopRunsWhileLoading()
{
    const QString path = write(QStringLiteral("loop.txt"), sampleText(10'000'000));
    auto file = TextFile::create();
    QSignalSpy finished(file.get(), &TextFile::loadFinished);
    int ticks = 0, ticksBeforeFinish = -1;
    qint64 longestGap = 0;
    QElapsedTimer clock;
    clock.start();
    qint64 last = 0;
    QTimer timer;
    timer.setInterval(2);
    timer.setTimerType(Qt::PreciseTimer);
    connect(&timer, &QTimer::timeout, &timer, [&] {
        ++ticks;
        longestGap = qMax(longestGap, clock.elapsed() - last);
        last = clock.elapsed();
        // Whatever asks while it loads is told there is nothing yet.
        QVERIFY(file->isLoading() == finished.isEmpty());
        if (file->isLoading()) {
            QVERIFY(file->text().isEmpty());
            QVERIFY(!file->isModified());
            QVERIFY(file->path().isEmpty());
            QVERIFY(!TextFile::find(file->path()));
        }
    });
    timer.start();
    file->loadAsync(path);
    QVERIFY(TextFile::isLoadingPath(path));
    QVERIFY(!TextFile::find(path));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
    ticksBeforeFinish = ticks;
    QVERIFY(!TextFile::isLoadingPath(path));
    QVERIFY(TextFile::find(path));
    qInfo() << "timer ticks while loading:" << ticksBeforeFinish << "longest gap ms:" << longestGap;
    QVERIFY2(ticksBeforeFinish >= 10, qPrintable(QString::number(ticksBeforeFinish)));
    // Nothing blocked the loop for anywhere near the load itself (generous:
    // CI machines stall for no reason, and the tab-less finish does little).
    QVERIFY2(longestGap < 400, qPrintable(QString::number(longestGap)));
}

void TestOpen::savesByteIdentically_data()
{
    QTest::addColumn<QByteArray>("bytes");

    QTest::newRow("lf") << sampleText(1'300'000);
    QTest::newRow("crlf") << sampleText(1'300'000, "\r\n");
    QTest::newRow("bom") << (QByteArray("\xEF\xBB\xBF") + sampleText(1'200'000));
    QStringEncoder utf16(QStringEncoder::Utf16LE);
    QTest::newRow("utf16le") << (QByteArray("\xFF\xFE") + QByteArray(utf16(QString::fromUtf8(sampleText(1'100'000)))));
    QTest::newRow("legacy") << (sampleText(1'100'000) + QByteArray("caf\xe9 \x93quoted\x94\n"));
    QTest::newRow("10MB") << sampleText(10'000'000);
}

void TestOpen::savesByteIdentically()
{
    QFETCH(QByteArray, bytes);
    const QString path = write(QStringLiteral("save.txt"), bytes);

    // Saved over itself, and under another name, through a tab like the app does.
    auto file = TextFile::create();
    QSignalSpy finished(file.get(), &TextFile::loadFinished);
    file->loadAsync(path);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
    QVERIFY(file->save(path));
    QCOMPARE(read(path), bytes);

    DocumentTab tab(file);
    const QString other = m_dir.filePath(QStringLiteral("saved-as.txt"));
    QVERIFY(tab.save(other));
    QCOMPARE(read(other), bytes);
    QVERIFY(!tab.isModified());

    // And after an edit that is undone.
    QTextCursor cursor(tab.document());
    cursor.insertText(QStringLiteral("zz"));
    QVERIFY(tab.isModified());
    tab.document()->undo();
    QVERIFY(tab.save(other));
    QCOMPARE(read(other), bytes);
}

void TestOpen::failureIsReported()
{
    auto file = TextFile::create();
    QSignalSpy finished(file.get(), &TextFile::loadFinished);
    file->loadAsync(m_dir.filePath(QStringLiteral("does-not-exist.txt")));
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(!finished.first().first().toBool());
    QVERIFY(!file->errorString().isEmpty());
    QVERIFY(!file->isLoading());
    QVERIFY(file->document()->isEmpty());
    QVERIFY(file->path().isEmpty());
    QVERIFY(file->document()->isUndoRedoEnabled());
}

void TestOpen::cancelStopsTheWorker()
{
    const QString path = write(QStringLiteral("cancel.txt"), sampleText(10'000'000));
    auto file = TextFile::create();
    QSignalSpy finished(file.get(), &TextFile::loadFinished);
    file->loadAsync(path);
    QTest::qWait(30);
    QVERIFY(file->isLoading());
    QElapsedTimer t;
    t.start();
    file->cancelLoad();
    const qint64 ms = t.elapsed();
    qInfo() << "cancel took ms:" << ms;
    QVERIFY2(ms < 500, qPrintable(QString::number(ms)));
    QVERIFY(!file->isLoading());
    QVERIFY(!TextFile::isLoadingPath(path));
    QVERIFY(!TextFile::find(path));
    QVERIFY(file->document()->isEmpty());
    QVERIFY(file->path().isEmpty());
    QVERIFY(!file->isModified());
    QVERIFY(file->document()->isUndoRedoEnabled());
    QTest::qWait(50);
    QCOMPARE(finished.size(), 0);

    // The file is usable and loads again, synchronously or not.
    QVERIFY(file->load(path));
    QCOMPARE(file->text().size(), QString::fromUtf8(sampleText(10'000'000)).size());
}

void TestOpen::destroyingTheFileMidLoadIsSafe()
{
    const QString path = write(QStringLiteral("destroy.txt"), sampleText(10'000'000));
    for (int wait : {0, 5, 40}) {
        auto file = TextFile::create();
        file->loadAsync(path);
        QTest::qWait(wait);
        file.reset(); // joins the worker
        QVERIFY(!TextFile::isLoadingPath(path));
        QVERIFY(!TextFile::find(path));
        QTest::qWait(20); // the queued end of the load finds nothing
    }
    // And a load still works after that.
    auto file = TextFile::create();
    QSignalSpy finished(file.get(), &TextFile::loadFinished);
    file->loadAsync(path);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
}

// Highlighting a file loaded this way when its source is shown gives the
// formats the synchronous load gives.
void TestOpen::highlightsLikeLoad()
{
    const auto restore = qScopeGuard([] { vn::Settings::instance().reset(vn::key::defaultMode); });
    vn::Settings::instance().setValue(vn::key::defaultMode, QStringLiteral("source"));
    const QString path = write(QStringLiteral("colours.md"), sampleText(1'200'000));

    DocumentTab expected;
    QVERIFY(expected.load(path));
    QVERIFY(expected.file()->isHighlighting());
    const Snapshot want = formatsOf(expected.document());

    auto file = TextFile::create();
    QSignalSpy finished(file.get(), &TextFile::loadFinished);
    file->loadAsync(path);
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
    QVERIFY(!file->isHighlighting()); // no view yet
    QVERIFY(file->isHighlighted());
    DocumentTab tab(file);
    QVERIFY(file->isHighlighting());
    const Snapshot got = formatsOf(tab.document());
    QCOMPARE(got.blocks, want.blocks);
    QCOMPARE(got.formats.size(), want.formats.size());
    for (int i = 0; i < got.formats.size(); ++i)
        QVERIFY2(got.formats[i] == want.formats[i], qPrintable(QStringLiteral("block %1").arg(i)));
    QVERIFY(!tab.isModified());
}

void TestOpen::mainWindowOpensLargeFilesInOrder()
{
    const QByteArray big1 = sampleText(3'000'000);
    const QByteArray big2 = sampleText(2'000'000, "\r\n");
    const QString first = write(QStringLiteral("first.md"), big1);
    const QString second = write(QStringLiteral("second.md"), big2);
    const QString small = write(QStringLiteral("small.md"), "# small\n");

    vn::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QStringList order;
    QVERIFY(window.openFile(first, [&](DocumentTab *tab) { order << QStringLiteral("first:") + tab->path(); }));
    QVERIFY(window.isOpening());
    QCOMPARE(window.tabCount(), 0);
    QVERIFY(window.openFile(second, [&](DocumentTab *tab) { order << QStringLiteral("second:") + tab->path(); }));
    // A small file does not wait for them.
    QVERIFY(window.openFile(small, [&](DocumentTab *tab) { order << QStringLiteral("small:") + tab->path(); }));
    QCOMPARE(window.tabCount(), 1);
    QCOMPARE(order, QStringList{QStringLiteral("small:") + small});
    QTRY_VERIFY_WITH_TIMEOUT(!window.isOpening(), 20000);
    QCOMPARE(window.tabCount(), 3);
    QCOMPARE(order, (QStringList{QStringLiteral("small:") + small, QStringLiteral("first:") + first,
                                 QStringLiteral("second:") + second}));
    const QList<DocumentTab *> tabs = window.tabs();
    QCOMPARE(tabs.size(), 3);
    QCOMPARE(tabs[1]->path(), first);
    QCOMPARE(tabs[2]->path(), second);
    QCOMPARE(window.currentTab(), tabs[2]);
    QCOMPARE(tabs[1]->text(), QString::fromUtf8(big1));
    QCOMPARE(tabs[2]->text(), QString::fromUtf8(big2).replace(QStringLiteral("\r\n"), QStringLiteral("\n")));
    QVERIFY(!tabs[1]->isModified());
    QVERIFY(!tabs[2]->isModified());
    QCOMPARE(tabs[2]->file()->lineEnding(), TextFile::LineEnding::CRLF);

    // Saving gives the file back as it was.
    QVERIFY(tabs[2]->save(second));
    QCOMPARE(read(second), big2);

    // Open files are not loaded again.
    bool again = false;
    QVERIFY(window.openFile(first, [&](DocumentTab *tab) { again = tab == tabs[1]; }));
    QVERIFY(again);
    QCOMPARE(window.tabCount(), 3);
}

void TestOpen::mainWindowOpensTheSameFileOnce()
{
    const QString path = write(QStringLiteral("once.md"), sampleText(2'000'000));
    vn::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    int calls = 0;
    QVERIFY(window.openFile(path, [&](DocumentTab *) { ++calls; }));
    QVERIFY(window.openFile(path, [&](DocumentTab *) { ++calls; })); // on its way already
    QTRY_VERIFY_WITH_TIMEOUT(!window.isOpening(), 20000);
    QCOMPARE(window.tabCount(), 1);
    QCOMPARE(calls, 1);
    // A failed one is reported through the dialog hook of QMessageBox only for
    // real files, so just check that the window is still fine afterwards.
    QVERIFY(window.currentTab());
}

void TestOpen::closingTheWindowMidLoadIsSafe()
{
    const QString path = write(QStringLiteral("close.md"), sampleText(10'000'000));
    const QString other = write(QStringLiteral("close2.md"), sampleText(4'000'000));
    for (int wait : {0, 10, 80}) {
        auto *window = new vn::MainWindow;
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
        bool opened = false;
        QVERIFY(window->openFile(path, [&](DocumentTab *) { opened = true; }));
        QVERIFY(window->openFile(other, [&](DocumentTab *) { opened = true; }));
        QTest::qWait(wait);
        delete window; // cancels both and joins their workers
        QTest::qWait(30);
        QVERIFY(!opened);
        QVERIFY(!TextFile::isLoadingPath(path));
        QVERIFY(!TextFile::isLoadingPath(other));
        QVERIFY(!TextFile::find(path));
    }

    // Closed through the window's own close, with the load still running and
    // another window staying: that one goes on and opens the file.
    vn::MainWindow stays;
    stays.show();
    QVERIFY(QTest::qWaitForWindowExposed(&stays));
    auto *window = new vn::MainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QPointer<vn::MainWindow> guard(window);
    QVERIFY(window->openFile(path));
    window->close();
    QTRY_VERIFY(guard.isNull());
    QVERIFY(!TextFile::isLoadingPath(path));
    QVERIFY(stays.openFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(stays.tabCount() == 1, 20000);
}

// A file that is already open in another window is not read again.
void TestOpen::largeFileOfAnotherWindowIsShared()
{
    const QString path = write(QStringLiteral("shared.md"), sampleText(2'000'000));
    vn::MainWindow one;
    one.show();
    QVERIFY(QTest::qWaitForWindowExposed(&one));
    QVERIFY(one.openFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(one.tabCount() == 1, 20000);

    vn::MainWindow two;
    two.show();
    QVERIFY(QTest::qWaitForWindowExposed(&two));
    DocumentTab *seen = nullptr;
    QVERIFY(two.openFile(path, [&](DocumentTab *tab) { seen = tab; }));
    // At once: no worker, a second view of the same document.
    QVERIFY(seen);
    QCOMPARE(seen->file(), one.currentTab()->file());
    QCOMPARE(two.tabCount(), 1);
    QVERIFY(!two.isOpening());
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestOpen)
#include "test_open.moc"
