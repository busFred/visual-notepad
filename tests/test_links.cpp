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
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <functional>

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

    void declinedLaunchOpensNothing_data();
    void declinedLaunchOpensNothing();
    void acceptedLaunchOpensOnce_data();
    void acceptedLaunchOpensOnce();
    void ordinaryLinksDoNotAsk();
    void webLinksKeepTheirPrompt();
    void networkTargetsAreRefused_data();
    void networkTargetsAreRefused();
    void promptsShowAddressesAsText_data();
    void promptsShowAddressesAsText();
    void longAddressesAreElided();

private:
    QString write(const QString &name, const QByteArray &content);
    DocumentTab *openDocument(MainWindow &window);
    // Clicks `button` of the next message box (the web link prompt).
    static void answerNextBox(const QString &button);
    // Calls `inspect` with the next message box, then closes it unanswered.
    static void inspectNextBox(std::function<void(QMessageBox *)> inspect);

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

void TestLinks::inspectNextBox(std::function<void(QMessageBox *)> inspect)
{
    auto *timer = new QTimer;
    timer->setInterval(10);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, inspect] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!box)
            return;
        timer->deleteLater();
        timer->stop();
        inspect(box);
        box->reject();
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

void TestLinks::promptsShowAddressesAsText_data()
{
    // Markup in a link's address: shown as rich text, an <img> would load
    // (on Windows from another machine, over SMB) and an <a> would be a link
    // the OS opens without asking. On Linux "//{dir}" is the local folder: a
    // picture that loaded would paint red.
    QTest::addColumn<QString>("link");
    QTest::addColumn<QString>("shown"); // A label's text; {missing}: the "does not exist" message.
    // Not a raw string: moc reads the "//" in one as a comment.
    const QString markup =
        QStringLiteral("<img src=\"//{dir}/red.png\" width=\"40\" height=\"40\"><a href=\"https://e.example/\">x</a>");
    QTest::newRow("mailto") << u"mailto:a" + markup << u"mailto:a" + markup;
    QTest::newRow("https") << u"https://example.com/" + markup << u"https://example.com/" + markup;
    QTest::newRow("missing file") << u"x" + markup + u".md" << QStringLiteral("{missing}");
}

void TestLinks::promptsShowAddressesAsText()
{
    QFETCH(QString, link);
    QFETCH(QString, shown);
    QImage red(40, 40, QImage::Format_RGB32);
    red.fill(Qt::red);
    QVERIFY(red.save(m_dir.filePath(QStringLiteral("red.png"))));
    const QString folder = m_dir.path().mid(1);
    link.replace(QStringLiteral("{dir}"), folder);
    shown.replace(QStringLiteral("{dir}"), folder);
    shown.replace(QStringLiteral("{missing}"),
                  QStringLiteral("The file %1 does not exist.").arg(QDir::toNativeSeparators(m_dir.filePath(link))));
    MainWindow window;
    DocumentTab *tab = openDocument(window);
    QVERIFY(tab);
    bool inspected = false;
    QStringList texts;
    int redPixels = 0;
    bool plain = true;
    inspectNextBox([&](QMessageBox *box) {
        inspected = true;
        plain = box->textFormat() == Qt::PlainText;
        for (QLabel *label : box->findChildren<QLabel *>()) {
            plain = plain && label->textFormat() == Qt::PlainText && !label->openExternalLinks()
                    && !(label->textInteractionFlags() & (Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard));
            texts.append(label->text());
        }
        const QImage image = box->grab().toImage();
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                redPixels += QColor(image.pixel(x, y)) == QColor(Qt::red);
    });
    Q_EMIT tab->linkActivated(link);
    QVERIFY(inspected);
    QVERIFY(plain);
    QVERIFY2(texts.contains(shown), qPrintable(texts.join(u'|')));
    QCOMPARE(redPixels, 0);
    QVERIFY(m_opened.isEmpty());
}

void TestLinks::longAddressesAreElided()
{
    MainWindow window;
    DocumentTab *tab = openDocument(window);
    QVERIFY(tab);
    const QString url = QStringLiteral("https://example.com/") + QString(2000, u'a') + QStringLiteral("/end");
    QString informative;
    inspectNextBox([&](QMessageBox *box) { informative = box->informativeText(); });
    Q_EMIT tab->linkActivated(url);
    QCOMPARE(informative.size(), 400);
    QVERIFY(informative.startsWith(QLatin1String("https://example.com/aaa")));
    QVERIFY(informative.endsWith(QLatin1String("aaa/end")));
    QVERIFY(informative.contains(QChar(0x2026)));
    QVERIFY(m_opened.isEmpty());
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
