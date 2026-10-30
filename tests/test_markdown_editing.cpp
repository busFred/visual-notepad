// Markdown editing in the source and visual views: list continuation,
// formatting, pasting images and files, link rewriting, link checking and
// Ctrl+Click.
#include "DocumentTab.h"
#include "Settings.h"
#include "Theme.h"
#include "editor/TextView.h"
#include "markdown/VisualView.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHelpEvent>
#include <QImage>
#include <QMessageBox>
#include <QMimeData>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>

using namespace vn;

class TestMarkdownEditing : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void bulletContinues();
    void orderedIncrements();
    void taskContinuesUnchecked();
    void quoteContinues();
    void emptyItemEndsList();
    void midLineSplitsItem();
    void multipleCarets();
    void notContinuedOutsideMarkdown();
    void visualViewContinuesLists();
    void splitIsTheDefaultMode();
    void splitLocksOnlyThePreview();
    void lineSpacingSetting();
    void tabIndentsItem();
    void formatIsOneUndoStep();
    void formatKeepsCaretLine();
    void pasteImageCreatesFile();
    void pasteImageUsesFolder();
    void untitledPasteImageWarns();
    void pasteFileUrlsInsertsLinks();
    void dropFileUrlsInsertsLinks();
    void pasteRewritesRelativeLinks();
    void pasteKeepsLinksWhenSettingOff();
    void diagnosticsFlagBrokenLinks();
    void diagnosticsFollowEdits();
    void diagnosticTooltip();
    void ctrlClickOpensLink();
    void altClickWhenCtrlAddsCarets();

private:
    // A Markdown document saved at dir/relative.
    DocumentTab *makeTab(const QString &relative, const QString &text);
    void moveToEnd(TextView *view);
    void press(TextView *view, Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier);

    QTemporaryDir m_dir;
    QList<DocumentTab *> m_tabs;
};

void TestMarkdownEditing::initTestCase()
{
    QVERIFY(m_dir.isValid());
    // These tests edit Markdown in untitled tabs; new files are plain text
    // by default.
    Settings::instance().setValue(key::newFileLanguage, QStringLiteral("markdown"));
    theme::apply(*qApp);
}

void TestMarkdownEditing::init()
{
    Settings::instance().reset(key::pasteImageFolder);
    Settings::instance().reset(key::updateLinksOnPaste);
    Settings::instance().reset(key::validateLinks);
    Settings::instance().reset(key::multiCursorModifier);
    Settings::instance().reset(key::tabWidth);
    Settings::instance().reset(key::insertSpaces);
}

void TestMarkdownEditing::cleanup()
{
    qDeleteAll(m_tabs);
    m_tabs.clear();
    QApplication::clipboard()->clear();
}

DocumentTab *TestMarkdownEditing::makeTab(const QString &relative, const QString &text)
{
    auto *tab = new DocumentTab;
    m_tabs.append(tab);
    tab->resize(700, 400);
    tab->setViewMode(DocumentTab::ViewMode::Source);
    tab->document()->setPlainText(text);
    if (!relative.isEmpty()) {
        const QString path = m_dir.filePath(relative);
        QDir().mkpath(QFileInfo(path).absolutePath());
        // Not Q_ASSERT: the save must also happen in Release builds.
        if (!tab->save(path))
            qFatal("could not save %s", qPrintable(path));
    }
    tab->show();
    return tab;
}

void TestMarkdownEditing::moveToEnd(TextView *view)
{
    QTextCursor c(view->document());
    c.movePosition(QTextCursor::End);
    view->setTextCursor(c);
}

void TestMarkdownEditing::press(TextView *view, Qt::Key key, Qt::KeyboardModifiers mods)
{
    QTest::keyClick(view, key, mods);
}

static QString plain(DocumentTab *tab)
{
    return tab->text();
}

void TestMarkdownEditing::bulletContinues()
{
    DocumentTab *tab = makeTab({}, QStringLiteral("- item"));
    TextView *view = tab->sourceView();
    moveToEnd(view);
    press(view, Qt::Key_Return);
    QCOMPARE(plain(tab), QStringLiteral("- item\n- "));
    QCOMPARE(view->textCursor().positionInBlock(), 2);
}

void TestMarkdownEditing::orderedIncrements()
{
    DocumentTab *tab = makeTab({}, QStringLiteral("  1. one"));
    TextView *view = tab->sourceView();
    moveToEnd(view);
    press(view, Qt::Key_Return);
    QCOMPARE(plain(tab), QStringLiteral("  1. one\n  2. "));
    press(view, Qt::Key_X);
    press(view, Qt::Key_Return);
    QCOMPARE(plain(tab), QStringLiteral("  1. one\n  2. x\n  3. "));

    DocumentTab *paren = makeTab({}, QStringLiteral("9) nine"));
    moveToEnd(paren->sourceView());
    press(paren->sourceView(), Qt::Key_Return);
    QCOMPARE(plain(paren), QStringLiteral("9) nine\n10) "));
}

void TestMarkdownEditing::taskContinuesUnchecked()
{
    DocumentTab *tab = makeTab({}, QStringLiteral("* [x] done"));
    TextView *view = tab->sourceView();
    moveToEnd(view);
    press(view, Qt::Key_Return);
    QCOMPARE(plain(tab), QStringLiteral("* [x] done\n* [ ] "));
    // An unticked, empty task ends the list.
    press(view, Qt::Key_Return);
    QCOMPARE(plain(tab), QStringLiteral("* [x] done\n"));
}

void TestMarkdownEditing::quoteContinues()
{
    DocumentTab *tab = makeTab({}, QStringLiteral("> > deep"));
    TextView *view = tab->sourceView();
    moveToEnd(view);
    press(view, Qt::Key_Return);
    QCOMPARE(plain(tab), QStringLiteral("> > deep\n> > "));
    press(view, Qt::Key_Return);
    QCOMPARE(plain(tab), QStringLiteral("> > deep\n"));
}

void TestMarkdownEditing::emptyItemEndsList()
{
    DocumentTab *tab = makeTab({}, QStringLiteral("- a\n- "));
    TextView *view = tab->sourceView();
    moveToEnd(view);
    press(view, Qt::Key_Return);
    QCOMPARE(plain(tab), QStringLiteral("- a\n"));
    QCOMPARE(view->textCursor().blockNumber(), 1);
    // Another Enter is a plain newline.
    press(view, Qt::Key_Return);
    QCOMPARE(plain(tab), QStringLiteral("- a\n\n"));

    DocumentTab *nested = makeTab({}, QStringLiteral("1. a\n    2. "));
    moveToEnd(nested->sourceView());
    press(nested->sourceView(), Qt::Key_Return);
    QCOMPARE(plain(nested), QStringLiteral("1. a\n"));
}

void TestMarkdownEditing::midLineSplitsItem()
{
    DocumentTab *tab = makeTab({}, QStringLiteral("- ab"));
    TextView *view = tab->sourceView();
    QTextCursor c(tab->document());
    c.setPosition(3);
    view->setTextCursor(c);
    press(view, Qt::Key_Return);
    QCOMPARE(plain(tab), QStringLiteral("- a\n- b"));
}

void TestMarkdownEditing::multipleCarets()
{
    DocumentTab *tab = makeTab({}, QStringLiteral("- a\n1. b\nplain\n  text"));
    TextView *view = tab->sourceView();
    QList<QTextCursor> carets;
    for (int line = 0; line < 4; ++line) {
        QTextCursor c(tab->document()->findBlockByNumber(line));
        c.movePosition(QTextCursor::EndOfBlock);
        carets.append(c);
    }
    view->setCursors(carets, 0);
    press(view, Qt::Key_Return);
    QCOMPARE(plain(tab), QStringLiteral("- a\n- \n1. b\n2. \nplain\n\n  text\n  "));
    QCOMPARE(view->cursorCount(), 4);
}

void TestMarkdownEditing::notContinuedOutsideMarkdown()
{
    DocumentTab *tab = makeTab({}, QStringLiteral("- item"));
    tab->setLanguage(QString());
    QVERIFY(!tab->isMarkdown());
    TextView *view = tab->sourceView();
    moveToEnd(view);
    press(view, Qt::Key_Return);
    QCOMPARE(plain(tab), QStringLiteral("- item\n"));
}

void TestMarkdownEditing::visualViewContinuesLists()
{
    DocumentTab *tab = makeTab({}, QStringLiteral("- item"));
    tab->setViewMode(DocumentTab::ViewMode::Visual);
    TextView *view = tab->visualView();
    QVERIFY(view);
    moveToEnd(view);
    press(view, Qt::Key_Return);
    QCOMPARE(plain(tab), QStringLiteral("- item\n- "));
    press(view, Qt::Key_Return);
    QCOMPARE(plain(tab), QStringLiteral("- item\n"));
}

void TestMarkdownEditing::splitIsTheDefaultMode()
{
    Settings::instance().reset(key::defaultMode);
    DocumentTab tab;
    QCOMPARE(tab.viewMode(), DocumentTab::ViewMode::SourceVisual);
}

void TestMarkdownEditing::splitLocksOnlyThePreview()
{
    DocumentTab *tab = makeTab({}, QStringLiteral("# Title\n\ntext"));
    const auto pillButton = [tab](const QString &tip) {
        for (QToolButton *button : tab->findChildren<QToolButton *>()) {
            if (button->toolTip() == tip)
                return button;
        }
        return static_cast<QToolButton *>(nullptr);
    };
    QToolButton *lock = pillButton(QStringLiteral("Read-only"));
    QToolButton *pen = pillButton(QStringLiteral("Editing"));
    QVERIFY(lock && pen);

    // Beside the source, the visual view is a locked preview.
    tab->setViewMode(DocumentTab::ViewMode::SourceVisual);
    QVERIFY(tab->visualView()->isReadOnly());
    QVERIFY(lock->isChecked());
    QVERIFY(!tab->sourceView()->isReadOnly());
    QVERIFY(!tab->isReadOnly());
    // On its own it is an editor.
    tab->setViewMode(DocumentTab::ViewMode::Visual);
    QVERIFY(!tab->visualView()->isReadOnly());
    QVERIFY(pen->isChecked());
    QVERIFY(!tab->isReadOnly());

    // Each mode remembers its own choice.
    lock->click();
    QVERIFY(tab->visualView()->isReadOnly());
    QVERIFY(tab->isReadOnly());
    tab->setViewMode(DocumentTab::ViewMode::SourceVisual);
    pen->click();
    QVERIFY(!tab->visualView()->isReadOnly());
    tab->setViewMode(DocumentTab::ViewMode::Visual);
    QVERIFY(tab->visualView()->isReadOnly());
    tab->setViewMode(DocumentTab::ViewMode::SourceVisual);
    QVERIFY(!tab->visualView()->isReadOnly());
    // The source view never locks, not even after Visual mode was locked.
    tab->setViewMode(DocumentTab::ViewMode::Source);
    QVERIFY(!tab->sourceView()->isReadOnly());
    QVERIFY(!tab->isReadOnly());
}

void TestMarkdownEditing::lineSpacingSetting()
{
    DocumentTab *tab = makeTab({}, QStringLiteral("one\ntwo"));
    TextView *view = tab->sourceView();
    // The distance between two lines' carets is the line height.
    const auto lineStep = [view] {
        QTextCursor c(view->document());
        view->setTextCursor(c);
        const int first = static_cast<QWidget *>(view)->inputMethodQuery(Qt::ImCursorRectangle).toRect().top();
        c.movePosition(QTextCursor::NextBlock);
        view->setTextCursor(c);
        return static_cast<QWidget *>(view)->inputMethodQuery(Qt::ImCursorRectangle).toRect().top() - first;
    };
    // As TextView::updateMetrics: the fractional font height, rounded up.
    const qreal fontHeight = QFontMetricsF(view->font()).height();
    Settings::instance().reset(key::lineSpacing);
    QCOMPARE(Settings::instance().value(key::lineSpacing).toDouble(), 1.4);
    const int normal = lineStep();
    QVERIFY2(normal == qCeil(fontHeight * 1.4), qPrintable(QStringLiteral("%1 vs %2").arg(normal).arg(fontHeight)));
    Settings::instance().setValue(key::lineSpacing, 2.0);
    QVERIFY2(lineStep() == qCeil(fontHeight * 2.0), qPrintable(QStringLiteral("%1 vs %2").arg(lineStep()).arg(fontHeight)));
    Settings::instance().reset(key::lineSpacing);
}

void TestMarkdownEditing::tabIndentsItem()
{
    Settings::instance().setValue(key::tabWidth, 4);
    Settings::instance().setValue(key::insertSpaces, true);
    DocumentTab *tab = makeTab({}, QStringLiteral("- a\n- b"));
    TextView *view = tab->sourceView();
    moveToEnd(view);
    const int column = view->textCursor().positionInBlock();
    press(view, Qt::Key_Tab);
    QCOMPARE(plain(tab), QStringLiteral("- a\n    - b"));
    QCOMPARE(view->textCursor().positionInBlock(), column + 4);
    press(view, Qt::Key_Backtab, Qt::ShiftModifier);
    QCOMPARE(plain(tab), QStringLiteral("- a\n- b"));

    // Plain text still gets a tab stop at the caret.
    DocumentTab *plainTab = makeTab({}, QStringLiteral("text"));
    moveToEnd(plainTab->sourceView());
    press(plainTab->sourceView(), Qt::Key_Tab);
    QCOMPARE(plain(plainTab), QStringLiteral("text    "));
}

void TestMarkdownEditing::formatIsOneUndoStep()
{
    const QString original = QStringLiteral("Title\n=====\n\n* a\n* b\n\n|a|b|\n|-|-|\n|long cell|2|\n");
    DocumentTab *tab = makeTab({}, original);
    tab->document()->clearUndoRedoStacks();
    tab->formatDocument();
    const QString formatted = plain(tab);
    QVERIFY(formatted != original);
    QVERIFY(formatted.startsWith(QStringLiteral("# Title\n")));
    QVERIFY(formatted.contains(QStringLiteral("- a\n- b\n")));
    QVERIFY(formatted.contains(QStringLiteral("| a         | b   |")));

    // Formatting again changes nothing, not even the undo stack.
    const int revision = tab->document()->revision();
    tab->formatDocument();
    QCOMPARE(tab->document()->revision(), revision);

    tab->sourceView()->undo();
    QCOMPARE(plain(tab), original);
    QVERIFY(!tab->document()->isUndoAvailable());
}

void TestMarkdownEditing::formatKeepsCaretLine()
{
    DocumentTab *tab = makeTab({}, QStringLiteral("# Title\n\n\n\ntext here\n"));
    TextView *view = tab->sourceView();
    QTextCursor c(tab->document()->findBlockByNumber(0));
    c.setPosition(c.position() + 3);
    view->setTextCursor(c);
    tab->formatDocument();
    QCOMPARE(plain(tab), QStringLiteral("# Title\n\ntext here\n"));
    QCOMPARE(view->textCursor().blockNumber(), 0);
    QCOMPARE(view->textCursor().positionInBlock(), 3);

    // Line 4 no longer exists afterwards; the caret is clamped to the end.
    DocumentTab *other = makeTab({}, QStringLiteral("# Title\n\n\n\ntext here\n"));
    QTextCursor late(other->document()->findBlockByNumber(4));
    late.setPosition(late.position() + 4);
    other->sourceView()->setTextCursor(late);
    other->formatDocument();
    QCOMPARE(other->sourceView()->textCursor().blockNumber(), other->document()->blockCount() - 1);
}

void TestMarkdownEditing::pasteImageCreatesFile()
{
    DocumentTab *tab = makeTab(QStringLiteral("img1/doc.md"), QString());
    QImage image(8, 8, QImage::Format_ARGB32);
    image.fill(Qt::red);
    QApplication::clipboard()->setImage(image);
    tab->sourceView()->paste();

    const QDir dir(m_dir.filePath(QStringLiteral("img1")));
    const QStringList pngs = dir.entryList({QStringLiteral("image-*.png")});
    QCOMPARE(pngs.size(), 1);
    QVERIFY(QImage(dir.filePath(pngs.first())).size() == QSize(8, 8));
    QCOMPARE(plain(tab), QStringLiteral("![](%1)").arg(pngs.first()));

    // A second paste in the same second gets a numbered name.
    tab->sourceView()->paste();
    QCOMPARE(dir.entryList({QStringLiteral("image-*.png")}).size(), 2);
}

void TestMarkdownEditing::pasteImageUsesFolder()
{
    Settings::instance().setValue(key::pasteImageFolder, QStringLiteral("assets/pics"));
    DocumentTab *tab = makeTab(QStringLiteral("img2/doc.md"), QString());
    QImage image(4, 4, QImage::Format_ARGB32);
    image.fill(Qt::blue);
    QApplication::clipboard()->setImage(image);
    tab->sourceView()->paste();

    const QDir dir(m_dir.filePath(QStringLiteral("img2/assets/pics")));
    const QStringList pngs = dir.entryList({QStringLiteral("image-*.png")});
    QCOMPARE(pngs.size(), 1);
    QCOMPARE(plain(tab), QStringLiteral("![](assets/pics/%1)").arg(pngs.first()));
}

void TestMarkdownEditing::untitledPasteImageWarns()
{
    DocumentTab *tab = makeTab({}, QString());
    QImage image(4, 4, QImage::Format_ARGB32);
    image.fill(Qt::green);
    QApplication::clipboard()->setImage(image);

    QString shown;
    QTimer::singleShot(0, [&shown] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            shown = box->text();
            box->accept();
        }
    });
    // Run a few times in case the box isn't up on the first tick.
    QTimer::singleShot(200, [&shown] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            shown = box->text();
            box->accept();
        }
    });
    tab->sourceView()->paste();
    QCOMPARE(shown, QStringLiteral("Save the document before pasting images."));
    QVERIFY(plain(tab).isEmpty());
}

void TestMarkdownEditing::pasteFileUrlsInsertsLinks()
{
    DocumentTab *tab = makeTab(QStringLiteral("files/doc.md"), QString());
    const QString base = m_dir.filePath(QStringLiteral("files"));
    auto *mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(base + QStringLiteral("/sub/my pic.PNG")),
                   QUrl::fromLocalFile(base + QStringLiteral("/notes.txt")),
                   QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("elsewhere/a.pdf")))});
    QApplication::clipboard()->setMimeData(mime);
    tab->sourceView()->paste();
    QCOMPARE(plain(tab), QStringLiteral("![](sub/my%20pic.PNG)\n[notes.txt](notes.txt)\n[a.pdf](../elsewhere/a.pdf)"));
}

void TestMarkdownEditing::dropFileUrlsInsertsLinks()
{
    DocumentTab *tab = makeTab(QStringLiteral("drop/doc.md"), QStringLiteral("x"));
    const QString file = m_dir.filePath(QStringLiteral("drop/pic.gif"));
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(file)});
    TextView *view = tab->sourceView();
    const QPointF pos(300, 5);
    QDragEnterEvent enter(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view->viewport(), &enter);
    QVERIFY(enter.isAccepted());
    // Shift+drop inserts a link; a plain drop asks to open the file (which
    // switches tabs, so it goes last).
    QDropEvent drop(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::ShiftModifier);
    QCoreApplication::sendEvent(view->viewport(), &drop);
    QCOMPARE(plain(tab), QStringLiteral("x![](pic.gif)"));
    QSignalSpy opened(tab, &DocumentTab::filesDropped);
    QDragEnterEvent again(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view->viewport(), &again); // Qt delivers drops only after an enter.
    QDropEvent plainDrop(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view->viewport(), &plainDrop);
    QCOMPARE(opened.size(), 1);
    QCOMPARE(plain(tab), QStringLiteral("x![](pic.gif)"));

    // Other languages leave drops to the window.
    DocumentTab *text = makeTab({}, QString());
    text->setLanguage(QString());
    QDragEnterEvent other(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    other.setAccepted(false);
    QCoreApplication::sendEvent(text->sourceView()->viewport(), &other);
    QVERIFY(!other.isAccepted());
}

void TestMarkdownEditing::pasteRewritesRelativeLinks()
{
    DocumentTab *from = makeTab(QStringLiteral("rw/a/one.md"),
                                QStringLiteral("[t](img/p.png#x) ![i](../shared/s.png) [u](https://e.com/x) [h](#top)"));
    DocumentTab *to = makeTab(QStringLiteral("rw/b/two.md"), QString());
    from->sourceView()->selectAll();
    from->sourceView()->copy();
    QVERIFY(QApplication::clipboard()->mimeData()->hasFormat(
        QStringLiteral("application/x-visnotepad-markdown-source")));
    to->sourceView()->paste();
    QCOMPARE(plain(to),
             QStringLiteral("[t](../a/img/p.png#x) ![i](../shared/s.png) [u](https://e.com/x) [h](#top)"));

    // Into a document in the same folder, nothing changes.
    DocumentTab *same = makeTab(QStringLiteral("rw/a/three.md"), QString());
    same->sourceView()->paste();
    QCOMPARE(plain(same), plain(from));
}

void TestMarkdownEditing::pasteKeepsLinksWhenSettingOff()
{
    Settings::instance().setValue(key::updateLinksOnPaste, false);
    DocumentTab *from = makeTab(QStringLiteral("rw2/a/one.md"), QStringLiteral("[t](x.md)"));
    DocumentTab *to = makeTab(QStringLiteral("rw2/b/two.md"), QString());
    from->sourceView()->selectAll();
    from->sourceView()->copy();
    QVERIFY(!QApplication::clipboard()->mimeData()->hasFormat(
        QStringLiteral("application/x-visnotepad-markdown-source")));
    to->sourceView()->paste();
    QCOMPARE(plain(to), QStringLiteral("[t](x.md)"));
}

void TestMarkdownEditing::diagnosticsFlagBrokenLinks()
{
    QFile other(m_dir.filePath(QStringLiteral("diag/exists.md")));
    QDir().mkpath(m_dir.filePath(QStringLiteral("diag")));
    QVERIFY(other.open(QIODevice::WriteOnly));
    other.write("# Sec\n\n## Other part\n");
    other.close();

    const QString text = QStringLiteral(
        "[a](missing.md)\n[b](exists.md)\n[c](#nope)\n[d](#real-heading)\n[e](exists.md#sec)\n"
        "[f](exists.md#nosuch)\n[g](https://example.com/x)\n[h](exists.md#other-part)\n\n# Real heading\n");
    DocumentTab *tab = makeTab(QStringLiteral("diag/doc.md"), text);
    QVERIFY(tab->sourceView()->diagnostics().isEmpty());
    Settings::instance().setValue(key::validateLinks, true);
    QVERIFY(tab->sourceView()->diagnostics().size() == 3);

    const auto diagnostics = tab->sourceView()->diagnostics();
    QStringList messages;
    for (const auto &d : diagnostics) {
        messages.append(d.message);
        // The range covers exactly the destination.
        QCOMPARE(tab->text().mid(d.start, d.end - d.start), d.message.section(QStringLiteral(": "), 1));
    }
    QCOMPARE(messages,
             (QStringList{QStringLiteral("Link target not found: missing.md"),
                          QStringLiteral("Link target not found: #nope"),
                          QStringLiteral("Link target not found: exists.md#nosuch")}));

    Settings::instance().setValue(key::validateLinks, false);
    QVERIFY(tab->sourceView()->diagnostics().isEmpty());
}

void TestMarkdownEditing::diagnosticsFollowEdits()
{
    Settings::instance().setValue(key::validateLinks, true);
    DocumentTab *tab = makeTab(QStringLiteral("diag2/doc.md"), QStringLiteral("[a](missing.md)\n"));
    QTRY_COMPARE(tab->sourceView()->diagnostics().size(), 1);

    // Text typed before the link shifts the range at once; the re-check after
    // the debounce confirms it.
    QTextCursor c(tab->document());
    c.insertText(QStringLiteral("é😀 "));
    auto d = tab->sourceView()->diagnostics();
    QCOMPARE(d.size(), 1);
    QCOMPARE(tab->text().mid(d[0].start, d[0].end - d[0].start), QStringLiteral("missing.md"));
    QTRY_COMPARE(tab->sourceView()->diagnostics().size(), 1);
    d = tab->sourceView()->diagnostics();
    QCOMPARE(tab->text().mid(d[0].start, d[0].end - d[0].start), QStringLiteral("missing.md"));

    // Fixing the link clears it.
    QTextCursor fix(tab->document());
    fix.movePosition(QTextCursor::End);
    fix.insertText(QStringLiteral("\n[b](doc.md)\n"));
    QTRY_COMPARE(tab->sourceView()->diagnostics().size(), 1);
}

void TestMarkdownEditing::diagnosticTooltip()
{
    Settings::instance().setValue(key::validateLinks, true);
    DocumentTab *tab = makeTab(QStringLiteral("tip/doc.md"), QStringLiteral("[a](missing.md) text"));
    QTRY_COMPARE(tab->sourceView()->diagnostics().size(), 1);
    QWidget *viewport = tab->sourceView()->viewport();
    for (int x = 10; x < 300 && !QToolTip::isVisible(); x += 4) {
        QHelpEvent help(QEvent::ToolTip, QPoint(x, 8), viewport->mapToGlobal(QPoint(x, 8)));
        QCoreApplication::sendEvent(viewport, &help);
    }
    QVERIFY(QToolTip::isVisible());
    QCOMPARE(QToolTip::text(), QStringLiteral("Link target not found: missing.md"));
    QToolTip::hideText();
}

void TestMarkdownEditing::ctrlClickOpensLink()
{
    DocumentTab *tab = makeTab(QStringLiteral("click/doc.md"), QStringLiteral("[a](target.md#sec) and more text"));
    QSignalSpy spy(tab, &DocumentTab::linkActivated);
    TextView *view = tab->sourceView();
    const int y = 8;

    // Plain clicks never open links.
    for (int x = 10; x < 300; x += 4)
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x, y));
    QCOMPARE(spy.count(), 0);

    for (int x = 10; x < 300 && spy.isEmpty(); x += 4)
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::ControlModifier, QPoint(x, y));
    QVERIFY(!spy.isEmpty());
    QCOMPARE(spy.first().first().toString(), QStringLiteral("target.md#sec"));

    // The visual view's source rows work the same way.
    tab->setViewMode(DocumentTab::ViewMode::Visual);
    TextView *visual = tab->visualView();
    tab->activateWindow();
    visual->setFocus();
    QVERIFY(QTest::qWaitForWindowActive(tab));
    QTextCursor c(tab->document());
    visual->setTextCursor(c); // Opens the block as source.
    spy.clear();
    for (int x = 10; x < 300 && spy.isEmpty(); x += 4)
        QTest::mouseClick(visual->viewport(), Qt::LeftButton, Qt::ControlModifier, QPoint(x, y));
    QVERIFY(!spy.isEmpty());
}

void TestMarkdownEditing::altClickWhenCtrlAddsCarets()
{
    Settings::instance().setValue(key::multiCursorModifier, QStringLiteral("ctrl"));
    DocumentTab *tab = makeTab(QStringLiteral("click2/doc.md"), QStringLiteral("[a](target.md) text"));
    QSignalSpy spy(tab, &DocumentTab::linkActivated);
    TextView *view = tab->sourceView();
    const int y = 8;
    for (int x = 10; x < 300; x += 4)
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::ControlModifier, QPoint(x, y));
    QCOMPARE(spy.count(), 0);
    for (int x = 10; x < 300 && spy.isEmpty(); x += 4)
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::AltModifier, QPoint(x, y));
    QVERIFY(!spy.isEmpty());
    QCOMPARE(spy.first().first().toString(), QStringLiteral("target.md"));
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestMarkdownEditing)
#include "test_markdown_editing.moc"
