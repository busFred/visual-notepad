// Links that would run a program ask first: md::isLaunchable() and
// MainWindow::openLink() through its confirmer and opener hooks. Nothing here
// reaches QDesktopServices.
#include "DocumentTab.h"
#include "MainWindow.h"
#include "Settings.h"
#include "Theme.h"
#include "markdown/MarkdownEditing.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>

using vn::DocumentTab;
using vn::MainWindow;

class TestLinks : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanupTestCase();

    void launchableNames_data();
    void launchableNames();
    void launchableByExecuteBit();
    void cleanedPaths_data();
    void cleanedPaths();
    void networkNames_data();
    void networkNames();

    void declinedLaunchOpensNothing_data();
    void declinedLaunchOpensNothing();
    void acceptedLaunchOpensOnce_data();
    void acceptedLaunchOpensOnce();
    void ordinaryLinksDoNotAsk();
    void webLinksKeepTheirPrompt();
    void networkTargetsAreRefused_data();
    void networkTargetsAreRefused();
    void invisibleCharactersAreSpelledOut_data();
    void invisibleCharactersAreSpelledOut();
    void promptShowsDisguisedNames();

private:
    QString write(const QString &name, const QByteArray &content);
    DocumentTab *openDocument(MainWindow &window);
    // Clicks `button` of the next message box (the web link prompt).
    static void answerNextBox(const QString &button);

    QTemporaryDir m_dir;
    QStringList m_asked;
    QList<QUrl> m_opened;
};

void TestLinks::initTestCase()
{
    QVERIFY(m_dir.isValid());
    vn::theme::apply(*qApp);
    // Before any link opens: never the real handler.
    MainWindow::setUrlOpener([this](const QUrl &url) { m_opened.append(url); });

    write(QStringLiteral("doc.md"), "# Heading\n\ntext\n");
    write(QStringLiteral("other.md"), "# Other\n");
    write(QStringLiteral("notes.txt"), "plain\n");
    write(QStringLiteral("evil.exe"), QByteArray("MZ\x90\x00", 4) + QByteArray(60, '\0'));
    write(QStringLiteral("evil.lnk"), QByteArray("L\x00\x00\x00", 4) + QByteArray(60, '\0'));
    write(QStringLiteral("pic.png"), QByteArray("\x89PNG\r\n\x1a\n", 8) + QByteArray(32, '\0'));
    write(QStringLiteral("doc.pdf"), "%PDF-1.4\n%%EOF\n");
#ifdef Q_OS_UNIX
    // A binary without a suffix: only its execute bit tells.
    const QString tool = write(QStringLiteral("tool"), QByteArray("\x7f" "ELF\x02\x01\x01", 7) + QByteArray(64, '\0'));
    QVERIFY(QFile::setPermissions(tool, QFile::permissions(tool) | QFile::ExeOwner));
#endif
}

void TestLinks::init()
{
    m_asked.clear();
    m_opened.clear();
    MainWindow::setLaunchConfirmer({});
}

void TestLinks::cleanupTestCase()
{
    MainWindow::setLaunchConfirmer({});
    MainWindow::setUrlOpener({});
}

QString TestLinks::write(const QString &name, const QByteArray &content)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
        return {};
    return path;
}

DocumentTab *TestLinks::openDocument(MainWindow &window)
{
    DocumentTab *tab = nullptr;
    window.openFile(m_dir.filePath(QStringLiteral("doc.md")), [&](DocumentTab *opened) { tab = opened; });
    return tab;
}

void TestLinks::answerNextBox(const QString &button)
{
    auto *timer = new QTimer;
    timer->setInterval(10);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, button] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        for (QAbstractButton *b : box->buttons()) {
            if (b->text().remove(u'&') == button) {
                b->click();
                timer->deleteLater();
                return;
            }
        }
    });
    timer->start();
}

void TestLinks::launchableNames_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("launchable");
    // None of these exist: the name alone decides.
    const QString dir = QStringLiteral("/no/such/dir/");
    const auto row = [&](const char *name, bool launchable) {
        QTest::newRow(name) << dir + QString::fromUtf8(name) << launchable;
    };
    row("evil.exe", true);
    row("EVIL.EXE", true);
    row("Setup.Msi", true);
    row("evil.lnk", true);
    row("x.msi", true);
    row("run.bat", true);
    row("s.ps1", true);
    row("a.jar", true);
    row("x.desktop", true);
    row("setup.sh", true);
    row("Tool.AppImage", true);
    row("x.settingcontent-ms", true);
    row("y.library-ms", true);
    row("Thing.app", true);
    row("a.exe.", true);
    row("a.exe ", true);
    row("a.exe. . .", true);
    row("readme.txt.exe", true);
    row("a.exe::$DATA", true);
    row("a.exe::$data", true);
    row("a.txt:evil.exe", true);
    row("a.txt:evil.exe:$DATA", true);
    row(".exe", true);
    row("notes.md", false);
    row("pic.png", false);
    row("notes.txt", false);
    row("doc.pdf", false);
    row("readme.exe.txt", false);
    row("archive.zip", false);
    row("exe", false);
    row("noext", false);
    // A last component of "." or ".." names the folder it is in.
    row("evil.exe/.", true);
    row("evil.exe/x/..", true);
    row("evil.exe/./x/../.", true);
    row("evil.exe/. ", true);
    row("evil.exe. /.", true);
    row("evil.exe/x/.. ", true);
    row("evil.exe/x/... /..", false); // ".." first, as Windows does: evil.exe/x.
    row("x/evil.exe/..", false);
    row("notes.md/.", false);
    // Case: Windows folds more than ASCII; anything not ASCII asks.
    row("evil.ExE", true);
    // (UTF-16 literals: narrow ones would take MSVC's code page.)
    QTest::newRow("long s") << dir + QStringLiteral(u"a.\u017Fcr") << true;
    QTest::newRow("zero width") << dir + QStringLiteral(u"a.md\u200B") << true;
    QTest::newRow("cyrillic e") << dir + QStringLiteral(u"a.\u0435xe") << true;
    QTest::newRow("accents") << dir + QStringLiteral(u"r\u00E9sum\u00E9.txt") << false;
    // Reach the OS without a prompt otherwise.
    for (const char *suffix : {"rdp", "ica", "appref-ms", "vsto", "xll", "xlam", "themepack", "deskthemepack", "msu",
                               "ppkg", "mdb", "accdb", "accde", "mde", "ade", "adp", "iso", "img", "vhd", "vhdx", "one",
                               "docm", "xlsm", "pptm", "dotm", "xltm", "potm", "ppam", "sldm", "jnlp", "slk"}) {
        const QByteArray name = QByteArray("file.") + suffix;
        QTest::newRow(name.constData()) << dir + QString::fromLatin1(name) << true;
        const QByteArray upper = name.toUpper();
        QTest::newRow(upper.constData()) << dir + QString::fromLatin1(upper) << true;
    }
    // Still documents.
    row("report.docx", false);
    row("sheet.xlsx", false);
    row("slides.pptx", false);
    row("data.csv", false);
    row("photo.jpeg", false);
    row("page.html", false);
    QTest::newRow("windows drive") << QStringLiteral("C:evil.exe") << true;
    QTest::newRow("backslashes") << QStringLiteral("C:\\dir\\evil.LNK") << true;
    QTest::newRow("backslash dir") << QStringLiteral("C:\\evil.exe\\notes.md") << false;
}

void TestLinks::launchableNames()
{
    QFETCH(QString, path);
    QFETCH(bool, launchable);
    QCOMPARE(vn::md::isLaunchable(path), launchable);
}

void TestLinks::launchableByExecuteBit()
{
#ifndef Q_OS_UNIX
    QSKIP("Execute bits are a Unix thing");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto make = [&](const QString &name, bool executable) {
        const QString path = dir.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return QString();
        file.write("data\n");
        file.close();
        if (executable)
            file.setPermissions(file.permissions() | QFile::ExeOwner);
        return path;
    };
    QVERIFY(vn::md::isLaunchable(make(QStringLiteral("tool"), true)));
    QVERIFY(!vn::md::isLaunchable(make(QStringLiteral("plain"), false)));
    QVERIFY(vn::md::isLaunchable(make(QStringLiteral("data.dat"), true)));
    // Documents stay documents on mounts that mark everything executable.
    QVERIFY(!vn::md::isLaunchable(make(QStringLiteral("notes.md"), true)));
    QVERIFY(!vn::md::isLaunchable(make(QStringLiteral("pic.PNG"), true)));
    QVERIFY(!vn::md::isLaunchable(make(QStringLiteral("doc.pdf"), true)));
    // Directories open a file manager; a bundle (by name) runs.
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("folder")));
    QVERIFY(!vn::md::isLaunchable(dir.filePath(QStringLiteral("folder"))));
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("Thing.app")));
    QVERIFY(vn::md::isLaunchable(dir.filePath(QStringLiteral("Thing.app"))));
    // Symlinks: judged by what they point at too.
    QVERIFY(QFile::link(dir.filePath(QStringLiteral("tool")), dir.filePath(QStringLiteral("readme.md"))));
    QVERIFY(vn::md::isLaunchable(dir.filePath(QStringLiteral("readme.md"))));
    make(QStringLiteral("evil.exe"), false);
    QVERIFY(QFile::link(dir.filePath(QStringLiteral("evil.exe")), dir.filePath(QStringLiteral("link.md"))));
    QVERIFY(vn::md::isLaunchable(dir.filePath(QStringLiteral("link.md"))));
    QVERIFY(QFile::link(dir.filePath(QStringLiteral("notes.md")), dir.filePath(QStringLiteral("other.md"))));
    QVERIFY(!vn::md::isLaunchable(dir.filePath(QStringLiteral("other.md"))));
#endif
}

void TestLinks::cleanedPaths_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("clean");
    QTest::addColumn<QString>("windows");
    const auto row = [](const char *path, const char *clean, const char *windows) {
        QTest::newRow(path) << QString::fromUtf8(path) << QString::fromUtf8(clean) << QString::fromUtf8(windows);
    };
    row("/d/evil.exe/.", "/d/evil.exe", "/d/evil.exe");
    row("/d/evil.exe/x/..", "/d/evil.exe", "/d/evil.exe");
    row("/d/evil.exe/. ", "/d/evil.exe/. ", "/d/evil.exe");
    row("/d/evil.exe. ", "/d/evil.exe. ", "/d/evil.exe");
    row("/d/x. /../evil.exe", "/d/evil.exe", "/d/evil.exe");
    row("/d/x/.. /evil.exe", "/d/x/.. /evil.exe", "/d/evil.exe");
#ifdef Q_OS_WIN
    row("C:\\d\\evil.exe\\.", "C:/d/evil.exe", "C:/d/evil.exe");
#else
    row("C:\\d\\evil.exe\\.", "C:\\d\\evil.exe\\.", "C:/d/evil.exe");
#endif
    row("d/../../evil.exe", "../evil.exe", "../evil.exe");
    row("/d/notes.md", "/d/notes.md", "/d/notes.md");
}

void TestLinks::cleanedPaths()
{
    QFETCH(QString, path);
    QFETCH(QString, clean);
    QFETCH(QString, windows);
    QCOMPARE(vn::md::cleanLocalPath(path, false), clean);
    QCOMPARE(vn::md::cleanLocalPath(path, true), windows);
}

void TestLinks::networkNames_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<bool>("network");
    const auto row = [](const char *path, bool network) { QTest::newRow(path) << QString::fromUtf8(path) << network; };
    row("//host/share/x", true);
    row("\\\\host\\share\\x", true);
    row("\\??\\UNC\\host\\share\\x", true);
    row("/?\?/UNC/host/share/x", true);
    row("\\?\?/UNC/host/share/x", true);
    row("/??\\C:\\evil.exe", true);
    row("\\\\?\\UNC\\host\\share", true);
    row("\\\\?\\C:\\evil.exe", true);
    row("//?/C:/evil.exe", true);
    row("\\\\.\\pipe\\x", true);
    row("//./C:/evil.exe", true);
    row(" \\??\\UNC\\host", true);
    row("/?/x", false);
    row("/??x", false);
    row("?\?/x", false);
    row("C:/?\?/x", false);
    row("evil.exe", false);
    row("/home/user/x.md", false);
}

void TestLinks::networkNames()
{
    QFETCH(QString, path);
    QFETCH(bool, network);
    QCOMPARE(vn::md::isNetworkPath(path), network);
}

void TestLinks::declinedLaunchOpensNothing_data()
{
    QTest::addColumn<QString>("link");
    QTest::addColumn<QString>("shown"); // empty: the file's native path
    QTest::addColumn<QString>("file");
    QTest::newRow("exe") << QStringLiteral("evil.exe") << QString() << QStringLiteral("evil.exe");
    QTest::newRow("dot slash") << QStringLiteral("./evil.exe") << QString() << QStringLiteral("evil.exe");
    QTest::newRow("encoded") << QStringLiteral("evil%2Eexe") << QString() << QStringLiteral("evil.exe");
    QTest::newRow("fragment") << QStringLiteral("evil.exe#x") << QString() << QStringLiteral("evil.exe");
    QTest::newRow("lnk") << QStringLiteral("evil.lnk") << QString() << QStringLiteral("evil.lnk");
    // The prompt names the file itself, not "evil.exe/.".
    QTest::newRow("dot") << QStringLiteral("evil.exe/.") << QString() << QStringLiteral("evil.exe");
    QTest::newRow("dot dot") << QStringLiteral("evil.exe/x/..") << QString() << QStringLiteral("evil.exe");
    QTest::newRow("encoded dot") << QStringLiteral("evil.exe%2F.") << QString() << QStringLiteral("evil.exe");
    QTest::newRow("detour") << QStringLiteral("x/../evil.lnk/.") << QString() << QStringLiteral("evil.lnk");
#ifdef Q_OS_UNIX
    QTest::newRow("execute bit") << QStringLiteral("tool") << QString() << QStringLiteral("tool");
#endif
    const QString msdt = QStringLiteral("ms-msdt:/id PCWDiagnostic /skip force");
    QTest::newRow("ms-msdt") << msdt << msdt << QString();
    QTest::newRow("shell") << QStringLiteral("shell:startup") << QStringLiteral("shell:startup") << QString();
    QTest::newRow("upper case") << QStringLiteral("SHELL:startup") << QStringLiteral("SHELL:startup") << QString();
    QTest::newRow("search-ms") << QStringLiteral("search-ms:query=x") << QStringLiteral("search-ms:query=x")
                               << QString();
    QTest::newRow("vscode") << QStringLiteral("vscode://x/y") << QStringLiteral("vscode://x/y") << QString();
    QTest::newRow("ftp") << QStringLiteral("ftp://example.com/x") << QStringLiteral("ftp://example.com/x")
                         << QString();
}

void TestLinks::declinedLaunchOpensNothing()
{
    QFETCH(QString, link);
    QFETCH(QString, shown);
    QFETCH(QString, file);
    if (!file.isEmpty()) {
        shown = QDir::toNativeSeparators(m_dir.filePath(file));
    }
    MainWindow::setLaunchConfirmer([this](const QString &target) {
        m_asked.append(target);
        return false;
    });
    MainWindow window;
    DocumentTab *tab = openDocument(window);
    QVERIFY(tab);
    Q_EMIT tab->linkActivated(link);
    QCOMPARE(m_asked, QStringList{shown});
    QVERIFY(m_opened.isEmpty());
    QCOMPARE(window.tabCount(), 1);
}

void TestLinks::acceptedLaunchOpensOnce_data()
{
    QTest::addColumn<QString>("link");
    QTest::addColumn<QUrl>("opened");
    QTest::newRow("exe") << QStringLiteral("evil.exe") << QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("evil.exe")));
    QTest::newRow("file url") << QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("evil.lnk"))).toString()
                              << QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("evil.lnk")));
    QTest::newRow("shell") << QStringLiteral("shell:startup") << QUrl(QStringLiteral("shell:startup"));
    // The file opened is the one asked about.
    QTest::newRow("dot") << QStringLiteral("evil.exe/.") << QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("evil.exe")));
    QTest::newRow("dot dot") << QStringLiteral("evil.exe/x/..")
                             << QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("evil.exe")));
}

void TestLinks::acceptedLaunchOpensOnce()
{
    QFETCH(QString, link);
    QFETCH(QUrl, opened);
    MainWindow::setLaunchConfirmer([this](const QString &target) {
        m_asked.append(target);
        return true;
    });
    MainWindow window;
    DocumentTab *tab = openDocument(window);
    QVERIFY(tab);
    Q_EMIT tab->linkActivated(link);
    QCOMPARE(m_asked.size(), 1);
    QCOMPARE(m_opened, QList<QUrl>{opened});
}

void TestLinks::ordinaryLinksDoNotAsk()
{
    MainWindow::setLaunchConfirmer([this](const QString &target) {
        m_asked.append(target);
        return false;
    });
    MainWindow window;
    DocumentTab *tab = openDocument(window);
    QVERIFY(tab);

    Q_EMIT tab->linkActivated(QStringLiteral("#heading"));
    Q_EMIT tab->linkActivated(QStringLiteral("pic.png"));
    Q_EMIT tab->linkActivated(QStringLiteral("doc.pdf"));
    QCOMPARE(m_opened, (QList<QUrl>{QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("pic.png"))),
                                    QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("doc.pdf")))}));
    QCOMPARE(window.tabCount(), 1);

    // Text opens in the app.
    Q_EMIT tab->linkActivated(QStringLiteral("other.md"));
    Q_EMIT tab->linkActivated(QStringLiteral("notes.txt"));
    QCOMPARE(window.tabCount(), 3);
    QCOMPARE(m_opened.size(), 2);
    QVERIFY(m_asked.isEmpty());
}

void TestLinks::webLinksKeepTheirPrompt()
{
    MainWindow::setLaunchConfirmer([this](const QString &target) {
        m_asked.append(target);
        return false;
    });
    MainWindow window;
    DocumentTab *tab = openDocument(window);
    QVERIFY(tab);

    answerNextBox(QStringLiteral("Cancel"));
    Q_EMIT tab->linkActivated(QStringLiteral("https://example.com/"));
    QVERIFY(m_opened.isEmpty());

    answerNextBox(QStringLiteral("Open"));
    Q_EMIT tab->linkActivated(QStringLiteral("https://example.com/"));
    answerNextBox(QStringLiteral("Open"));
    Q_EMIT tab->linkActivated(QStringLiteral("mailto:a@example.com"));
    QCOMPARE(m_opened, (QList<QUrl>{QUrl(QStringLiteral("https://example.com/")),
                                    QUrl(QStringLiteral("mailto:a@example.com"))}));
    QVERIFY(m_asked.isEmpty());
}

void TestLinks::networkTargetsAreRefused_data()
{
    QTest::addColumn<QString>("link");
    QTest::newRow("slashes") << QStringLiteral("//host/share/evil.exe");
    QTest::newRow("backslashes") << QStringLiteral("\\\\host\\share\\evil.exe");
    QTest::newRow("mixed") << QStringLiteral("\\/host/share/evil.lnk");
    QTest::newRow("file url") << QStringLiteral("file://host/share/evil.exe");
    QTest::newRow("object manager") << QStringLiteral("\\??\\UNC\\host\\share\\evil.exe");
    QTest::newRow("object manager slashes") << QStringLiteral("/?\?/UNC/host/share/evil.exe");
    QTest::newRow("object manager encoded") << QStringLiteral("%5C%3F%3F%5CUNC%5Chost%5Cshare%5Cevil.exe");
    QTest::newRow("device") << QStringLiteral("\\\\.\\UNC\\host\\share\\evil.exe");
    QTest::newRow("long path") << QStringLiteral("\\\\?\\UNC\\host\\share\\evil.exe");
}

void TestLinks::networkTargetsAreRefused()
{
    QFETCH(QString, link);
    MainWindow::setLaunchConfirmer([this](const QString &target) {
        m_asked.append(target);
        return true;
    });
    MainWindow window;
    DocumentTab *tab = openDocument(window);
    QVERIFY(tab);
    Q_EMIT tab->linkActivated(link);
    QVERIFY(m_asked.isEmpty());
    QVERIFY(m_opened.isEmpty());
    QCOMPARE(window.tabCount(), 1);
}

void TestLinks::invisibleCharactersAreSpelledOut_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("shown");
    // UTF-16 literals: narrow ones would take MSVC's code page.
    const auto row = [](const char *name, const QString &text, const QString &shown) {
        QTest::newRow(name) << text << shown;
    };
    row("override", QStringLiteral(u"doc\u202Etxt.exe"), QStringLiteral(u"doc\\u{202E}txt.exe"));
    row("embedding", QStringLiteral(u"\u202A\u202B\u202C\u202D"),
        QStringLiteral(u"\\u{202A}\\u{202B}\\u{202C}\\u{202D}"));
    row("isolates", QStringLiteral(u"\u2066\u2067\u2068\u2069"),
        QStringLiteral(u"\\u{2066}\\u{2067}\\u{2068}\\u{2069}"));
    row("marks", QStringLiteral(u"\u200E\u200F\u061C"), QStringLiteral(u"\\u{200E}\\u{200F}\\u{061C}"));
    row("zero width", QStringLiteral(u"a\u200Bb\uFEFF\u00AD"), QStringLiteral(u"a\\u{200B}b\\u{FEFF}\\u{00AD}"));
    row("tag", QStringLiteral(u"x\U000E0041"), QStringLiteral(u"x\\u{E0041}"));
    row("new line", QStringLiteral("a.exe\n\nsafe.txt"), QStringLiteral(u"a.exe\\u{000A}\\u{000A}safe.txt"));
    row("line separator", QStringLiteral(u"a\u2028b"), QStringLiteral(u"a\\u{2028}b"));
    row("lone surrogate", QString(QChar(0xD800)) + u'x', QStringLiteral(u"\\u{D800}x"));
    row("plain", QStringLiteral(u"C:\\Users\\Jos\u00E9\\\u0645\u0644\u0641.exe"),
        QStringLiteral(u"C:\\Users\\Jos\u00E9\\\u0645\u0644\u0641.exe"));
}

void TestLinks::invisibleCharactersAreSpelledOut()
{
    QFETCH(QString, text);
    QFETCH(QString, shown);
    QCOMPARE(vn::md::withVisibleControls(text), shown);
}

void TestLinks::promptShowsDisguisedNames()
{
    // "doc", U+202E, "txt.exe": shown right to left it reads "docexe.txt".
    const QString name = QStringLiteral(u"doc\u202Etxt.exe");
    QVERIFY(!write(name, QByteArray("MZ\x90\x00", 4) + QByteArray(60, '\0')).isEmpty());
    MainWindow window;
    DocumentTab *tab = openDocument(window);
    QVERIFY(tab);
    QString text;
    auto *timer = new QTimer(&window);
    timer->setInterval(10);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, &text] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        text = box->text();
        for (QAbstractButton *b : box->buttons()) {
            if (b->text().remove(u'&') == QLatin1String("Don't Open"))
                b->click();
        }
        timer->stop();
    });
    timer->start();
    Q_EMIT tab->linkActivated(name);
    QVERIFY(m_opened.isEmpty());
    QVERIFY2(!text.contains(QChar(0x202E)), qPrintable(text));
    const QString shown = QStringLiteral("doc\\u{202E}txt.exe");
    QVERIFY2(text.contains(QDir::toNativeSeparators(m_dir.filePath(shown))), qPrintable(text));
    QVERIFY2(text.split(u'\n').contains(QStringLiteral("File name: ") + shown), qPrintable(text));
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestLinks)
#include "test_links.moc"
