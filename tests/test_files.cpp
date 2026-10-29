// File handling: encodings, line endings, language override, tab management
// and drag and drop.
#include "DocumentTab.h"
#include "MainWindow.h"
#include "Settings.h"
#include "Theme.h"
#include "editor/TextView.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QTest>
#include <QTextCursor>
#include <QTextDocument>
#include <QUrl>

using vn::DocumentTab;
using Encoding = vn::DocumentTab::Encoding;

class TestFiles : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void opensEncodings_data();
    void opensEncodings();
    void setEncodingWritesUtf16();
    void unrepresentableCharacterRefusesSave();
    void reopenWithEncoding();
    void lineEndingChangeIsSaved();
    void languageOverride();
    void closeScopes();
    void dropOpensFiles();

private:
    QString write(const QString &name, const QByteArray &content);
    static QByteArray read(const QString &path);
    static void append(DocumentTab &tab, const QString &text);

    QTemporaryDir m_dir;
};

void TestFiles::initTestCase()
{
    QVERIFY(m_dir.isValid());
    vn::theme::apply(*qApp);
}

QString TestFiles::write(const QString &name, const QByteArray &content)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
        return {};
    return path;
}

QByteArray TestFiles::read(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void TestFiles::append(DocumentTab &tab, const QString &text)
{
    QTextCursor cursor(tab.document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
}

void TestFiles::opensEncodings_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<int>("encoding");
    QTest::addColumn<QString>("text");
    QTest::addColumn<QByteArray>("appended");

    const QString cafe = QStringLiteral("café\n");
    QTest::newRow("utf8") << QByteArray("caf\xc3\xa9\n") << int(Encoding::Utf8) << cafe << QByteArray("\xc3\xa9");
    QTest::newRow("utf8bom") << QByteArray("\xEF\xBB\xBF" "caf\xc3\xa9\n") << int(Encoding::Utf8Bom) << cafe
                             << QByteArray("\xc3\xa9");
    QTest::newRow("utf16le") << QByteArray("\xFF\xFE" "c\0a\0f\0\xe9\0\n\0", 12) << int(Encoding::Utf16LE) << cafe
                             << QByteArray("\xe9\0", 2);
    QTest::newRow("utf16be") << QByteArray("\xFE\xFF" "\0c\0a\0f\0\xe9\0\n", 12) << int(Encoding::Utf16BE) << cafe
                             << QByteArray("\0\xe9", 2);
    QTest::newRow("latin1") << QByteArray("caf\xe9\n") << int(Encoding::Legacy) << cafe << QByteArray("\xe9");
}

void TestFiles::opensEncodings()
{
    QFETCH(QByteArray, bytes);
    QFETCH(int, encoding);
    QFETCH(QString, text);
    QFETCH(QByteArray, appended);

    const QString path = write(QStringLiteral("enc.txt"), bytes);
    DocumentTab tab;
    QVERIFY2(tab.load(path), qPrintable(tab.errorString()));
    QCOMPARE(int(tab.encoding()), encoding);
    QCOMPARE(tab.text(), text);
    QVERIFY(!tab.isModified());

    // An edit re-encodes in the same encoding, BOM included.
    append(tab, QStringLiteral("é"));
    QVERIFY2(tab.save(path), qPrintable(tab.errorString()));
    QCOMPARE(read(path), bytes + appended);
}

void TestFiles::setEncodingWritesUtf16()
{
    const QString path = write(QStringLiteral("to16.txt"), "ab");
    DocumentTab tab;
    QVERIFY(tab.load(path));
    QCOMPARE(int(tab.encoding()), int(Encoding::Utf8));
    tab.setEncoding(Encoding::Utf16LE);
    QVERIFY(tab.isModified());
    QVERIFY(tab.save(path));
    QCOMPARE(read(path), QByteArray("\xFF\xFE" "a\0b\0", 6));
}

void TestFiles::unrepresentableCharacterRefusesSave()
{
    const QString path = write(QStringLiteral("latin.txt"), "caf\xe9");
    DocumentTab tab;
    QVERIFY(tab.load(path));
    QCOMPARE(int(tab.encoding()), int(Encoding::Legacy));
    append(tab, QStringLiteral("€"));
    QVERIFY(!tab.save(path));
    QVERIFY(!tab.errorString().isEmpty());
    QCOMPARE(read(path), QByteArray("caf\xe9"));
    QVERIFY(tab.isModified());
}

void TestFiles::reopenWithEncoding()
{
    const QString path = write(QStringLiteral("reopen.txt"), "caf\xc3\xa9");
    DocumentTab tab;
    QVERIFY(tab.load(path));
    QCOMPARE(tab.text(), QStringLiteral("café"));
    QVERIFY(tab.reopenWithEncoding(Encoding::Legacy));
    QCOMPARE(tab.text(), QStringLiteral("cafÃ©"));
    QCOMPARE(int(tab.encoding()), int(Encoding::Legacy));

    // Unsaved changes block it.
    append(tab, QStringLiteral("x"));
    QVERIFY(!tab.reopenWithEncoding(Encoding::Utf8));
    QVERIFY(!tab.errorString().isEmpty());

    DocumentTab untitled;
    QVERIFY(!untitled.reopenWithEncoding(Encoding::Utf8));
}

void TestFiles::lineEndingChangeIsSaved()
{
    const QString path = write(QStringLiteral("eol.txt"), "a\nb\n");
    DocumentTab tab;
    QVERIFY(tab.load(path));
    QCOMPARE(tab.lineEnding(), DocumentTab::LineEnding::LF);
    QSignalSpy spy(&tab, &DocumentTab::formatChanged);
    tab.setLineEnding(DocumentTab::LineEnding::CRLF);
    QCOMPARE(spy.count(), 1);
    QVERIFY(tab.isModified());
    QVERIFY(tab.save(path));
    QCOMPARE(read(path), QByteArray("a\r\nb\r\n"));
}

void TestFiles::languageOverride()
{
    const QString path = write(QStringLiteral("readme.md"), "# Title\n");
    DocumentTab tab;
    QVERIFY(tab.load(path));
    QVERIFY(tab.isMarkdown());

    tab.setLanguage(QStringLiteral("Python"));
    QCOMPARE(tab.languageName(), QStringLiteral("Python"));
    QCOMPARE(tab.definitionName(), QStringLiteral("Python"));
    QVERIFY(!tab.isMarkdown());

    tab.setLanguage(QString());
    QVERIFY(tab.definitionName().isEmpty());
    QVERIFY(!tab.isMarkdown());

    // Saving under a new name goes back to choosing by file name.
    QVERIFY(tab.save(m_dir.filePath(QStringLiteral("script.py"))));
    QCOMPARE(tab.definitionName(), QStringLiteral("Python"));
}

void TestFiles::closeScopes()
{
    vn::MainWindow window;
    for (int i = 0; i < 4; ++i)
        window.newFile();
    QCOMPARE(window.tabCount(), 4);

    // Untitled, unmodified tabs close without prompting.
    QVERIFY(window.closeTabs(3, vn::MainWindow::CloseScope::Right));
    QCOMPARE(window.tabCount(), 4);
    QVERIFY(window.closeTabs(2, vn::MainWindow::CloseScope::Right));
    QCOMPARE(window.tabCount(), 3);
    QVERIFY(window.closeTabs(1, vn::MainWindow::CloseScope::Others));
    QCOMPARE(window.tabCount(), 1);
    QVERIFY(window.closeTabs(0, vn::MainWindow::CloseScope::Saved));
    QCOMPARE(window.tabCount(), 0);

    window.newFile();
    window.newFile();
    QVERIFY(window.closeTabs(0, vn::MainWindow::CloseScope::All));
    QCOMPARE(window.tabCount(), 0);
}

void TestFiles::dropOpensFiles()
{
    const QString a = write(QStringLiteral("drop-a.txt"), "a");
    const QString b = write(QStringLiteral("drop-b.txt"), "b");
    vn::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(a), QUrl::fromLocalFile(b)});
    const QPointF pos(100, 100);
    QDragEnterEvent enter(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &enter);
    QVERIFY(enter.isAccepted());

    QDropEvent drop(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &drop);
    QCOMPARE(window.tabCount(), 2);

    QMimeData text;
    text.setText(QStringLiteral("hello"));
    QDragEnterEvent reject(pos, Qt::CopyAction, &text, Qt::LeftButton, Qt::NoModifier);
    reject.setAccepted(false);
    QCoreApplication::sendEvent(&window, &reject);
    QVERIFY(!reject.isAccepted());
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestFiles)
#include "test_files.moc"
