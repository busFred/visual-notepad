// Syntax highlighting in slices (Highlighter): big texts are coloured a few
// milliseconds at a time, the lines on screen first, and end up exactly as
// colouring everything at once leaves them, edits made meanwhile included.
#include "DocumentTab.h"
#include "Settings.h"
#include "TextFile.h"
#include "Theme.h"
#include "editor/Highlighter.h"
#include "editor/TextView.h"

#include <QApplication>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>

#include <memory>

using vn::DocumentTab;
using vn::TextFile;

namespace {

using Formats = QList<QList<QTextLayout::FormatRange>>;

Formats formatsOf(QTextDocument *doc)
{
    Formats all;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next())
        all.append(block.layout()->formats());
    return all;
}

bool coloured(QTextDocument *doc, int line)
{
    return !doc->findBlockByNumber(line).layout()->formats().isEmpty();
}

// The last line with text (empty lines have no colours either way).
int lastTextLine(QTextDocument *doc)
{
    QTextBlock block = doc->lastBlock();
    while (block.isValid() && block.text().isEmpty())
        block = block.previous();
    return block.blockNumber();
}

// Markdown with code blocks, whose fences change how every following line is
// coloured; about `bytes` long.
QByteArray markdown(qsizetype bytes)
{
    QByteArray out;
    for (int i = 0; out.size() < bytes; ++i) {
        out += "## Section " + QByteArray::number(i) + "\n\nSome *emphasis*, **strong** and `code`, a [link](https://e.com/"
               + QByteArray::number(i) + ").\n\n```cpp\nint f" + QByteArray::number(i)
               + "() { return 1; } // comment\n```\n\n- item\n> quote\n\n";
    }
    return out;
}

// C++ with block comments; lines long enough that 4 MB stays under the
// line limit for highlighting.
QByteArray cpp(qsizetype bytes)
{
    QByteArray out;
    for (int i = 0; out.size() < bytes; ++i)
        out += "int f" + QByteArray::number(i) + "(int x, int y) { return x * 2 + y; } // twice the first plus the second\n"
               "const char *s" + QByteArray::number(i) + " = \"a string literal\"; /* a block comment */\n\n";
    return out;
}

void typeAt(QTextDocument *doc, int line, const QString &text)
{
    QTextCursor cursor(doc->findBlockByNumber(line));
    cursor.insertText(text);
}

} // namespace

class TestHighlighting : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();
    void smallTextsColourAtOnce();
    void bigTextsColourInSlices();
    void visibleLinesComeFirst();
    void editsWhileColouringEndAsAtOnce();
    void bigEditsColourInSlices();
    void colouringIsNotAnEdit();
    void languageChangeColoursInSlices();
    void longLinesColourTheirStart();
    void closingWhileColouringIsSafe();

private:
    QString write(const QString &name, const QByteArray &content);
    // The formats a file's text gets when it is all coloured at once.
    Formats atOnce(const QString &path, const QString &language = {});
    Formats atOnce(QTextDocument *doc, const QString &name, const QString &language = {});
    static void finish(TextFile *file);

    QTemporaryDir m_dir;
};

void TestHighlighting::initTestCase()
{
    QVERIFY(m_dir.isValid());
    vn::theme::apply(*qApp);
}

void TestHighlighting::init()
{
    vn::Settings::instance().setValue(vn::key::defaultMode, QStringLiteral("source"));
    TextFile::setHighlightInSlices(true);
}

void TestHighlighting::cleanup()
{
    TextFile::setHighlightInSlices(false);
    vn::Settings::instance().reset(vn::key::defaultMode);
}

QString TestHighlighting::write(const QString &name, const QByteArray &content)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
        return {};
    return path;
}

Formats TestHighlighting::atOnce(const QString &path, const QString &language)
{
    TextFile::setHighlightInSlices(false);
    const auto restore = qScopeGuard([] { TextFile::setHighlightInSlices(true); });
    DocumentTab tab;
    if (!tab.load(path))
        return {};
    if (!language.isEmpty())
        tab.setLanguage(language);
    return formatsOf(tab.document());
}

Formats TestHighlighting::atOnce(QTextDocument *doc, const QString &name, const QString &language)
{
    QString text = doc->toRawText();
    text.replace(QChar::ParagraphSeparator, u'\n');
    return atOnce(write(name, text.toUtf8()), language);
}

void TestHighlighting::finish(TextFile *file)
{
    QTRY_VERIFY_WITH_TIMEOUT(!file->isHighlightPending(), 30000);
}

// Small texts are coloured completely right away, as without slicing.
void TestHighlighting::smallTextsColourAtOnce()
{
    const QString path = write(QStringLiteral("small.md"), markdown(vn::Highlighter::slicedFromChars / 2));
    const Formats expected = atOnce(path);
    DocumentTab tab;
    QVERIFY(tab.load(path));
    QVERIFY(tab.file()->isHighlighting());
    QVERIFY(!tab.file()->isHighlightPending());
    QCOMPARE(formatsOf(tab.document()), expected);
}

// A big text starts with its top coloured and the rest uncoloured, and is
// coloured over several turns of the event loop, ending with the formats
// colouring it at once gives.
void TestHighlighting::bigTextsColourInSlices()
{
    const QString path = write(QStringLiteral("big.md"), markdown(1'200'000));
    const Formats expected = atOnce(path);

    DocumentTab tab;
    QVERIFY(tab.load(path));
    QTextDocument *doc = tab.document();
    QVERIFY(tab.file()->isHighlighted());
    QVERIFY(tab.file()->isHighlighting());
    QVERIFY(tab.file()->isHighlightPending());
    QVERIFY(coloured(doc, 0));
    QVERIFY(!coloured(doc, lastTextLine(doc)));
    // One turn of the event loop does a slice, not the rest.
    QApplication::processEvents();
    QVERIFY(tab.file()->isHighlightPending());
    QVERIFY(!coloured(doc, lastTextLine(doc)));

    finish(tab.file());
    const Formats got = formatsOf(doc);
    QCOMPARE(got.size(), expected.size());
    for (int i = 0; i < got.size(); ++i)
        QVERIFY2(got[i] == expected[i], qPrintable(QStringLiteral("block %1").arg(i)));

    // The same when the file is loaded off the GUI thread and a tab comes later.
    auto file = TextFile::create();
    QSignalSpy loaded(file.get(), &TextFile::loadFinished);
    file->loadAsync(path);
    QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 20000);
    DocumentTab late(file);
    QVERIFY(file->isHighlightPending());
    finish(file.get());
    QCOMPARE(formatsOf(late.document()), expected);
}

// Lines on screen are coloured before the pass from the top gets to them.
void TestHighlighting::visibleLinesComeFirst()
{
    const QString path = write(QStringLiteral("scrolled.cpp"), cpp(4'000'000));
    DocumentTab tab;
    tab.resize(800, 600);
    tab.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tab));
    QVERIFY(tab.load(path));
    QTextDocument *doc = tab.document();
    QVERIFY(tab.file()->isHighlighted());
    QVERIFY(doc->blockCount() > 60'000);
    QVERIFY(tab.file()->isHighlightPending());
    tab.sourceView()->scrollToLine(50'000);
    QTRY_VERIFY_WITH_TIMEOUT(coloured(doc, 50'005), 5000);
    QVERIFY(tab.file()->isHighlightPending());
    QVERIFY(!coloured(doc, 40'000));
    // The view shows the colours.
    QVERIFY(!tab.grab().isNull());
    finish(tab.file());
    QCOMPARE(formatsOf(doc), atOnce(path));
}

// Typing behind the pass, ahead of it and on lines coloured ahead of it, and
// opening a construct that changes every line after it: all of it ends as
// if the final text had been coloured at once.
void TestHighlighting::editsWhileColouringEndAsAtOnce()
{
    for (const QString name : {QStringLiteral("edits.md"), QStringLiteral("edits.cpp")}) {
        const bool md = name.endsWith(QLatin1String(".md"));
        const QString path = write(name, md ? markdown(1'200'000) : cpp(1'200'000));
        DocumentTab tab;
        tab.resize(800, 600);
        tab.show();
        QVERIFY(QTest::qWaitForWindowExposed(&tab));
        QVERIFY(tab.load(path));
        QTextDocument *doc = tab.document();
        QVERIFY(tab.file()->isHighlightPending());
        // Coloured ahead of the pass first, then edited there.
        tab.sourceView()->scrollToLine(20'000);
        QTRY_VERIFY_WITH_TIMEOUT(coloured(doc, 20'002), 5000);
        typeAt(doc, 20'001, md ? QStringLiteral("```\n") : QStringLiteral("/* "));
        // Behind the pass, opening a construct that runs to the end.
        typeAt(doc, 2, md ? QStringLiteral("```\n") : QStringLiteral("/* "));
        QApplication::processEvents();
        // Far ahead of it, and a line removed in the middle.
        typeAt(doc, doc->blockCount() - 10, QStringLiteral("tail edit "));
        QTextCursor line(doc->findBlockByNumber(10'000));
        line.select(QTextCursor::BlockUnderCursor);
        line.removeSelectedText();
        finish(tab.file());
        const Formats expected = atOnce(doc, QStringLiteral("expected-") + name);
        const Formats got = formatsOf(doc);
        QCOMPARE(got.size(), expected.size());
        for (int i = 0; i < got.size(); ++i)
            QVERIFY2(got[i] == expected[i], qPrintable(QStringLiteral("%1 block %2").arg(name).arg(i)));
    }
}

// An edit that brings a big text (a paste, a reload, undoing either) is
// coloured in slices too, from where it starts.
void TestHighlighting::bigEditsColourInSlices()
{
    const QString path = write(QStringLiteral("paste.md"), "# Title\n\nsome text\n");
    DocumentTab tab;
    QVERIFY(tab.load(path));
    QTextDocument *doc = tab.document();
    QVERIFY(!tab.file()->isHighlightPending());

    QTextCursor end(doc);
    end.movePosition(QTextCursor::End);
    end.insertText(QString::fromUtf8(markdown(1'000'000)));
    QVERIFY(tab.file()->isHighlightPending());
    // The first slice ran with the edit: where it starts is coloured.
    QVERIFY(coloured(doc, 0));
    QVERIFY(coloured(doc, 3)); // "## Section 0"
    QVERIFY(coloured(doc, 5));
    QVERIFY(!coloured(doc, lastTextLine(doc)));
    finish(tab.file());
    QCOMPARE(formatsOf(doc), atOnce(doc, QStringLiteral("pasted.md")));

    doc->undo();
    QCOMPARE(doc->blockCount(), 4);
    QVERIFY(!tab.file()->isHighlightPending());
    QCOMPARE(formatsOf(doc), atOnce(path));
    doc->redo();
    QVERIFY(tab.file()->isHighlightPending());
    finish(tab.file());
    QCOMPARE(formatsOf(doc), atOnce(doc, QStringLiteral("redone.md")));
}

// Colouring changes no text: no change signal while it runs, and the first
// keystroke afterwards reports only itself.
void TestHighlighting::colouringIsNotAnEdit()
{
    const QString path = write(QStringLiteral("quiet.md"), markdown(1'000'000));
    DocumentTab tab;
    QVERIFY(tab.load(path));
    const quint64 serial = tab.file()->changeSerial();
    QSignalSpy textChanged(&tab, &DocumentTab::textChanged);
    QSignalSpy change(tab.document(), &QTextDocument::contentsChange);
    finish(tab.file());
    QCOMPARE(textChanged.size(), 0);
    QCOMPARE(change.size(), 0);
    QCOMPARE(tab.file()->changeSerial(), serial);
    QVERIFY(!tab.isModified());

    QTextCursor(tab.document()).insertText(QStringLiteral("x"));
    QCOMPARE(change.size(), 1);
    QCOMPARE(change.first(), (QVariantList{0, 0, 1}));
    QCOMPARE(tab.file()->changeSerial(), serial + 1);
}

// Another language for a big file colours it again in slices.
void TestHighlighting::languageChangeColoursInSlices()
{
    const QString path = write(QStringLiteral("language.txt"), cpp(1'000'000));
    DocumentTab tab;
    QVERIFY(tab.load(path));
    finish(tab.file());
    tab.setLanguage(QStringLiteral("C++"));
    QVERIFY(tab.file()->isHighlightPending());
    finish(tab.file());
    QCOMPARE(formatsOf(tab.document()), atOnce(path, QStringLiteral("C++")));
}

// Only the start of a very long line is coloured (one call into the
// highlighter that cannot be split); the lines after it are coloured as usual.
void TestHighlighting::longLinesColourTheirStart()
{
    QByteArray json = "[";
    while (json.size() < 3 * vn::Highlighter::maxLineChars)
        json += "{\"key\": \"value\", \"n\": 12},";
    json += "{}]\n{\"next\": true}\n";
    TextFile::setHighlightInSlices(false);
    DocumentTab tab;
    QVERIFY(tab.load(write(QStringLiteral("long.json"), json)));
    const QTextBlock longLine = tab.document()->firstBlock();
    const auto formats = longLine.layout()->formats();
    QVERIFY(!formats.isEmpty());
    int end = 0;
    for (const auto &range : formats)
        end = std::max(end, range.start + range.length);
    QCOMPARE(end, vn::Highlighter::maxLineChars);
    QVERIFY(!longLine.next().layout()->formats().isEmpty());
}

// A tab or file that goes away, or a file that loads again, while slices are
// still to come stops them.
void TestHighlighting::closingWhileColouringIsSafe()
{
    const QString path = write(QStringLiteral("closing.md"), markdown(1'200'000));
    {
        auto tab = std::make_unique<DocumentTab>();
        QVERIFY(tab->load(path));
        QVERIFY(tab->file()->isHighlightPending());
        tab.reset();
        QTest::qWait(30);
    }
    {
        DocumentTab tab;
        QVERIFY(tab.load(path));
        QVERIFY(tab.file()->isHighlightPending());
        tab.setViewMode(DocumentTab::ViewMode::Visual);
        QTest::qWait(10);
        const QString small = write(QStringLiteral("closing-small.md"), "# small\n");
        QVERIFY(tab.load(small));
        QVERIFY(!tab.file()->isHighlightPending());
        QTest::qWait(30);
        QCOMPARE(tab.text(), QStringLiteral("# small\n"));
    }
}

QTEST_MAIN(TestHighlighting)
#include "test_highlighting.moc"
