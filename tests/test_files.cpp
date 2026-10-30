// File handling: encodings, line endings, language override, tab management
// and drag and drop.
#include "DocumentTab.h"
#include "Encodings.h"
#include "MainWindow.h"
#include "Settings.h"
#include "Theme.h"
#include "editor/TextView.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QSet>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QStringEncoder>
#include <QTest>
#include <QTextCursor>
#include <QTextDocument>
#include <QUrl>

using vn::DocumentTab;
namespace enc = vn::encoding;

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
    void guessesBig5_data();
    void guessesBig5();
    void legacySettingIsUsed();
    void multiByteRoundTrips_data();
    void multiByteRoundTrips();
    void curatedEncodingsRoundTrip();
    void menuListsEveryCodecOnce();
    void lineEndingChangeIsSaved();
    void languageOverride();
    void newFileLanguageSetting();
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
    QTest::addColumn<QString>("encoding");
    QTest::addColumn<QString>("text");
    QTest::addColumn<QByteArray>("appended");

    const QString cafe = QStringLiteral("café\n");
    QTest::newRow("utf8") << QByteArray("caf\xc3\xa9\n") << enc::utf8 << cafe << QByteArray("\xc3\xa9");
    QTest::newRow("utf8bom") << QByteArray("\xEF\xBB\xBF" "caf\xc3\xa9\n") << enc::utf8Bom << cafe
                             << QByteArray("\xc3\xa9");
    QTest::newRow("utf16le") << QByteArray("\xFF\xFE" "c\0a\0f\0\xe9\0\n\0", 12) << enc::utf16LE << cafe
                             << QByteArray("\xe9\0", 2);
    QTest::newRow("utf16be") << QByteArray("\xFE\xFF" "\0c\0a\0f\0\xe9\0\n", 12) << enc::utf16BE << cafe
                             << QByteArray("\0\xe9", 2);
    QTest::newRow("latin1") << QByteArray("caf\xe9\n") << enc::legacy() << cafe << QByteArray("\xe9");
}

void TestFiles::opensEncodings()
{
    QFETCH(QByteArray, bytes);
    QFETCH(QString, encoding);
    QFETCH(QString, text);
    QFETCH(QByteArray, appended);

    const QString path = write(QStringLiteral("enc.txt"), bytes);
    DocumentTab tab;
    QVERIFY2(tab.load(path), qPrintable(tab.errorString()));
    QCOMPARE(tab.encoding(), encoding);
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
    QCOMPARE(tab.encoding(), enc::utf8);
    tab.setEncoding(enc::utf16LE);
    QVERIFY(tab.isModified());
    QVERIFY(tab.save(path));
    QCOMPARE(read(path), QByteArray("\xFF\xFE" "a\0b\0", 6));
}

void TestFiles::unrepresentableCharacterRefusesSave()
{
    const QString path = write(QStringLiteral("latin.txt"), "caf\xe9");
    DocumentTab tab;
    QVERIFY(tab.load(path));
    QCOMPARE(tab.encoding(), enc::legacy());
    append(tab, QStringLiteral("中")); // Not in any Western code page.
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
    QVERIFY(tab.reopenWithEncoding(enc::legacy()));
    QCOMPARE(tab.text(), QStringLiteral("cafÃ©"));
    QCOMPARE(tab.encoding(), enc::legacy());

    // Unsaved changes block it.
    append(tab, QStringLiteral("x"));
    QVERIFY(!tab.reopenWithEncoding(enc::utf8));
    QVERIFY(!tab.errorString().isEmpty());

    DocumentTab untitled;
    QVERIFY(!untitled.reopenWithEncoding(enc::utf8));
}

static QByteArray encoded(const char *codec, const QString &text)
{
    QStringEncoder encoder(codec);
    return encoder(text);
}

void TestFiles::guessesBig5_data()
{
    QTest::addColumn<bool>("guess");
    QTest::newRow("guess on") << true;
    QTest::newRow("guess off") << false;
}

void TestFiles::guessesBig5()
{
    QFETCH(bool, guess);
    vn::Settings &settings = vn::Settings::instance();
    settings.setValue(vn::key::guessEncoding, guess);
    const QString sentence = QStringLiteral("今天天氣很好，我們一起去公園散步，然後在咖啡店喝茶聊天。\n");
    QString text;
    for (int i = 0; i < 6; ++i)
        text += sentence;
    const QByteArray bytes = encoded("Big5", text);
    QCOMPARE(encoded("Big5", QStringLiteral("中文")), QByteArray("\xA4\xA4\xA4\xE5"));
    const QString path = write(QStringLiteral("big5.txt"), bytes);

    DocumentTab tab;
    QVERIFY(tab.load(path));
    if (guess) {
#ifdef VN_HAVE_ICU
        QCOMPARE(tab.encoding(), QStringLiteral("Big5"));
        QCOMPARE(tab.text(), text);
        QVERIFY(tab.save(path));
        QCOMPARE(read(path), bytes);
#else
        QSKIP("Built without ICU: no guessing");
#endif
    } else {
        QCOMPARE(tab.encoding(), enc::legacy());
        QVERIFY(tab.text() != text);
    }
    settings.reset(vn::key::guessEncoding);
}

void TestFiles::legacySettingIsUsed()
{
    vn::Settings &settings = vn::Settings::instance();
    settings.setValue(vn::key::guessEncoding, false);
    settings.setValue(vn::key::legacyEncoding, QStringLiteral("windows-1251"));
    const QString text = QStringLiteral("Привет, мир\n");
    const QString path = write(QStringLiteral("cyr.txt"), encoded("windows-1251", text));
    const QString nul = write(QStringLiteral("with-nul.txt"), QByteArray("a\0\xff", 3));

    DocumentTab tab;
    QVERIFY(tab.load(path));
    QCOMPARE(tab.encoding(), QStringLiteral("windows-1251"));
    QCOMPARE(tab.text(), text);
    // Files with NUL bytes that are not UTF-16 use it as well.
    DocumentTab binary;
    QVERIFY(binary.load(nul));
    QCOMPARE(binary.encoding(), QStringLiteral("windows-1251"));

    settings.reset(vn::key::legacyEncoding);
    QVERIFY(enc::isValid(enc::legacy()));
    settings.reset(vn::key::guessEncoding);
}

void TestFiles::multiByteRoundTrips_data()
{
    QTest::addColumn<QString>("codec");
    QTest::addColumn<QString>("text");
    QTest::newRow("GB18030") << QStringLiteral("GB18030") << QStringLiteral("简体中文测试\n");
    QTest::newRow("Shift_JIS") << QStringLiteral("Shift_JIS") << QStringLiteral("日本語のテスト\n");
    QTest::newRow("EUC-KR") << QStringLiteral("EUC-KR") << QStringLiteral("한국어 시험\n");
    QTest::newRow("ISO-2022-JP") << QStringLiteral("ISO-2022-JP") << QStringLiteral("日本語\n");
}

void TestFiles::multiByteRoundTrips()
{
    QFETCH(QString, codec);
    QFETCH(QString, text);
    const QByteArray bytes = encoded(codec.toLatin1().constData(), text);
    QVERIFY(!bytes.isEmpty());
    const QString path = write(QStringLiteral("mb.txt"), bytes);

    // Decoded under the chosen encoding, whatever opening guessed.
    DocumentTab tab;
    QVERIFY(tab.load(path));
    QVERIFY2(tab.reopenWithEncoding(codec), qPrintable(tab.errorString()));
    QCOMPARE(tab.encoding(), codec);
    QCOMPARE(tab.text(), text);
    // Saving writes the identical bytes.
    tab.document()->setModified(true);
    QVERIFY(tab.save(path));
    QCOMPARE(read(path), bytes);
}

void TestFiles::curatedEncodingsRoundTrip()
{
    int count = 0;
    for (const auto &group : enc::curatedGroups()) {
        for (const enc::Entry &entry : group) {
            const auto bytes = enc::encode(QStringLiteral("abc"), entry.name);
            QVERIFY2(bytes.has_value(), qPrintable(entry.name));
            QCOMPARE(enc::decode(*bytes, entry.name), QStringLiteral("abc"));

            const QString path = write(QStringLiteral("abc.txt"), *bytes);
            DocumentTab tab;
            QVERIFY2(tab.load(path), qPrintable(entry.name));
            QVERIFY2(tab.reopenWithEncoding(entry.name), qPrintable(entry.name));
            QCOMPARE(tab.text(), QStringLiteral("abc"));
            QVERIFY(tab.save(path));
            QCOMPARE(read(path), *bytes);
            ++count;
        }
    }
    QVERIFY(count >= 20);
}

void TestFiles::menuListsEveryCodecOnce()
{
    QSet<QString> seen;
    const auto add = [&](const enc::Entry &e) {
        QVERIFY2(!seen.contains(e.name.toLower()), qPrintable(e.name));
        QVERIFY2(enc::isValid(e.name), qPrintable(e.name));
        seen.insert(e.name.toLower());
    };
    for (const auto &group : enc::curatedGroups())
        std::for_each(group.begin(), group.end(), add);
    const auto others = enc::otherEntries();
    std::for_each(others.begin(), others.end(), add);
    QVERIFY(seen.contains(QStringLiteral("utf-8 bom")));
    QVERIFY(seen.contains(QStringLiteral("big5")));
    QVERIFY(!others.isEmpty());
    for (qsizetype i = 1; i < others.size(); ++i)
        QVERIFY(others[i - 1].name.compare(others[i].name, Qt::CaseInsensitive) <= 0);
    QCOMPARE(enc::canonical(QStringLiteral("utf8bom")), enc::utf8Bom);
    QCOMPARE(enc::canonical(QStringLiteral("BIG5")), QStringLiteral("Big5"));
    QVERIFY(enc::canonical(QStringLiteral("no-such-codec")).isEmpty());
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

// "Open new files as": plain text by default, Markdown when chosen; files
// with a name still go by their name.
void TestFiles::newFileLanguageSetting()
{
    vn::Settings::instance().reset(vn::key::newFileLanguage);
    QCOMPARE(vn::Settings::instance().text(vn::key::newFileLanguage), QStringLiteral("plaintext"));
    {
        vn::MainWindow window;
        window.newFile();
        DocumentTab *tab = window.currentTab();
        QVERIFY(tab);
        QVERIFY(!tab->isMarkdown());
        QVERIFY(tab->definitionName().isEmpty());
        QCOMPARE(tab->languageName(), QStringLiteral("Plain Text"));
        QVERIFY(!tab->visualView());
    }

    vn::Settings::instance().setValue(vn::key::newFileLanguage, QStringLiteral("markdown"));
    {
        vn::MainWindow window;
        window.newFile();
        DocumentTab *tab = window.currentTab();
        QVERIFY(tab);
        QVERIFY(tab->isMarkdown());
        QCOMPARE(tab->definitionName(), QStringLiteral("Markdown"));
    }
    // Only untitled files follow the setting.
    const QString path = write(QStringLiteral("notes.txt"), "plain\n");
    DocumentTab loaded;
    QVERIFY(loaded.load(path));
    QVERIFY(!loaded.isMarkdown());

    vn::Settings::instance().reset(vn::key::newFileLanguage);
    DocumentTab untitled;
    QVERIFY(!untitled.isMarkdown());
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
