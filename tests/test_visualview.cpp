// The hybrid visual Markdown view, run offscreen. Set VN_SCREENSHOT_DIR to
// save renderings.
#include "DocumentTab.h"
#include "Settings.h"
#include "Theme.h"
#include "editor/DocumentLayout.h"
#include "markdown/MarkdownModel.h"
#include "markdown/VisualView.h"
#include "editor/TextView.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>

using namespace vn;

namespace {

const char *sample = R"(---
title: Project notes
tags: demo
---

# Project notes

## Goals

A light Markdown editor that replaces **Notepad**.
Diagrams use [mermaid-rs-renderer](https://github.com/1jehuang/mermaid-rs-renderer).

- [x] Visual editor
- [ ] Math like $e^{i\pi} + 1 = 0$

## Install

Run the install script once per platform:

```python
from __future__ import annotations
import subprocess
```

| Platform | Status |
|:---------|-------:|
| Linux    | done   |
| Windows  | next   |

> Quoted text with `inline code`.

$$
f\colon \RR^n \to \RR
$$

```mermaid
flowchart LR; A[Source] --> B[Visual]
```
)";

} // namespace

class TestVisualView : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void blocksWithoutCaretAreRendered();
    void caretExpandsItsBlock();
    void typingInExpandedBlockKeepsRows();
    void taskCheckboxToggles();
    void linkClickEmits();
    void unfocusedRendersEverything();
    void lockedRendersEverything();
    void inverseMapping();
    void dragSelectsAcrossRenderedBlocks();
    void selectionIsPainted();
    void editableDragDefersExpansion();
    void doubleAndTripleClick();
    void selectionMirrorsInSplit();
    void screenshots();

private:
    QTextDocument *m_doc = nullptr;
    KSyntaxHighlighting::SyntaxHighlighter *m_highlighter = nullptr;
    MarkdownModel *m_model = nullptr;
    VisualView *m_view = nullptr;

    // Viewport y where a click lands on the given source line, or -1.
    int yOfLine(int line, int x = 60);
    int countColor(const QImage &image, const QColor &color) const;
};

int TestVisualView::yOfLine(int line, int x)
{
    for (int y = 0; y < m_view->viewport()->height(); y += 2) {
        QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(x, y));
        if (m_view->textCursor().blockNumber() == line)
            return y;
    }
    return -1;
}

int TestVisualView::countColor(const QImage &image, const QColor &color) const
{
    int n = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            n += QColor(image.pixel(x, y)) == color;
    return n;
}

void TestVisualView::initTestCase()
{
    theme::apply(*qApp);
}

void TestVisualView::init()
{
    m_doc = new QTextDocument;
    m_doc->setDocumentLayout(new DocumentLayout(m_doc));
    m_highlighter = new KSyntaxHighlighting::SyntaxHighlighter(m_doc);
    m_highlighter->setTheme(theme::syntaxTheme());
    m_highlighter->setDefinition(theme::syntaxRepository().definitionForName(QStringLiteral("Markdown")));
    m_doc->setPlainText(QString::fromUtf8(sample));
    m_model = new MarkdownModel(m_doc);
    m_view = new VisualView(m_model);
    m_view->resize(760, 900);
    m_view->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_view));
    m_view->activateWindow();
    m_view->setFocus();
    QVERIFY(QTest::qWaitForWindowActive(m_view));
    // Start with the caret on the blank line after the front matter.
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(4)));
}

void TestVisualView::cleanup()
{
    delete m_view;
    delete m_doc;
}

void TestVisualView::blocksWithoutCaretAreRendered()
{
    // Rendered: front matter, 2 headings, paragraph, list, heading, para,
    // code, table, quote, math, mermaid; plus gap rows for blank lines.
    const auto &blocks = m_model->blocks();
    QCOMPARE(int(blocks.size()), 12);
    QCOMPARE(blocks[7].kind, core::BlockKind::Code);
    QCOMPARE(blocks[10].kind, core::BlockKind::Math);
    QVERIFY(!m_view->grab().isNull());
}

void TestVisualView::caretExpandsItsBlock()
{
    // Put the caret in the "Goals" paragraph (line 9): its two lines become
    // source rows, everything else stays rendered.
    const QImage before = m_view->grab().toImage();
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(9)));
    const QImage after = m_view->grab().toImage();
    QVERIFY(before != after);
    QCOMPARE(m_model->blockAt(9), m_model->blockAt(10));
}

void TestVisualView::typingInExpandedBlockKeepsRows()
{
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(9)));
    QTest::keyClicks(m_view, QStringLiteral("New "));
    QVERIFY(m_doc->findBlockByNumber(9).text().startsWith(QLatin1String("New A light")));
    // A new paragraph splits the block; the view must follow.
    QTest::keyClick(m_view, Qt::Key_End);
    QTest::keyClick(m_view, Qt::Key_Return);
    QTest::keyClick(m_view, Qt::Key_Return);
    QTest::keyClicks(m_view, QStringLiteral("Third paragraph"));
    QVERIFY(m_model->blockAt(11) != m_model->blockAt(9));
    QVERIFY(!m_view->grab().isNull());
}

void TestVisualView::taskCheckboxToggles()
{
    // Hover over the list until the pointer turns into a hand, then click:
    // the second task (line 13) is the lower of the two boxes.
    QVERIFY(m_doc->findBlockByNumber(13).text().startsWith(QLatin1String("- [ ]")));
    // The boxes sit on adjacent lines: click the top and bottom of the hits.
    QList<QPoint> hits;
    for (int y = 0; y < m_view->viewport()->height(); y += 2) {
        for (int x = 20; x < 140; x += 2) {
            QTest::mouseMove(m_view->viewport(), QPoint(x, y));
            if (m_view->viewport()->cursor().shape() == Qt::PointingHandCursor) {
                hits.append(QPoint(x + 2, y));
                break;
            }
        }
    }
    QVERIFY2(hits.size() >= 4, "task boxes found");
    const QPoint first = hits.first() + QPoint(0, 2);
    const QPoint second = hits.last() - QPoint(0, 2);
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, second);
    QVERIFY(m_doc->findBlockByNumber(13).text().startsWith(QLatin1String("- [x]")));
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, first);
    QVERIFY(m_doc->findBlockByNumber(12).text().startsWith(QLatin1String("- [ ]")));
}

void TestVisualView::linkClickEmits()
{
    QSignalSpy spy(m_view, &VisualView::linkActivated);
    for (int y = 0; y < m_view->viewport()->height() && spy.isEmpty(); y += 3) {
        for (int x = 20; x < 700 && spy.isEmpty(); x += 6) {
            QTest::mouseMove(m_view->viewport(), QPoint(x, y));
            if (m_view->viewport()->cursor().shape() == Qt::PointingHandCursor)
                QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(x, y));
        }
    }
    QVERIFY(!spy.isEmpty());
    const QString url = spy.first().first().toString();
    QVERIFY2(url == QLatin1String("https://github.com/1jehuang/mermaid-rs-renderer") || url.startsWith(u'#'),
             qPrintable(url));
}

void TestVisualView::unfocusedRendersEverything()
{
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(9)));
    const QImage focused = m_view->grab().toImage();
    m_view->clearFocus();
    QVERIFY(m_view->grab().toImage() != focused);
}

void TestVisualView::lockedRendersEverything()
{
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(9)));
    const QImage editing = m_view->grab().toImage();
    m_view->setReadOnly(true);
    const QImage locked = m_view->grab().toImage();
    QVERIFY(editing != locked);
    // Typing does nothing while locked.
    QTest::keyClicks(m_view, QStringLiteral("zzz"));
    QVERIFY(!m_doc->toPlainText().contains(QLatin1String("zzz")));
}

void TestVisualView::inverseMapping()
{
    const QString source = QStringLiteral("A **bold** and [a link](http://x.org/y) and `code` end.");
    const QString rendered = QStringLiteral("A bold and a link and code end.");
    for (int k = 0; k <= rendered.size(); ++k) {
        const int s = sourceOffsetFor(rendered.left(k), source);
        QCOMPARE(renderedOffsetFor(rendered, source, s), k);
    }
    // Offsets inside syntax map to the nearest rendered boundary.
    QCOMPARE(renderedOffsetFor(rendered, source, 0), 0);
    QCOMPARE(renderedOffsetFor(rendered, source, 4), 2);  // Inside the opening **.
    QCOMPARE(renderedOffsetFor(rendered, source, 6), 4);  // After "bo".
    QCOMPARE(renderedOffsetFor(rendered, source, source.indexOf(u']')), rendered.indexOf(u'k') + 1);
    QCOMPARE(renderedOffsetFor(rendered, source, source.indexOf(u')') + 1), rendered.indexOf(u'k') + 1);
    QCOMPARE(renderedOffsetFor(rendered, source, source.size()), int(rendered.size()));
    // Soft line breaks match a space.
    QCOMPARE(renderedOffsetFor(QStringLiteral("one two"), QStringLiteral("one\ntwo"), 4), 4);
}

void TestVisualView::dragSelectsAcrossRenderedBlocks()
{
    m_view->setReadOnly(true);
    const int y1 = yOfLine(9);
    const int y2 = yOfLine(17); // "Run the install script ..."
    QVERIFY(y1 >= 0 && y2 > y1);
    QTest::mousePress(m_view->viewport(), Qt::LeftButton, {}, QPoint(100, y1));
    QTest::mouseMove(m_view->viewport(), QPoint(300, y2));
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, QPoint(300, y2));
    const QTextCursor c = m_view->textCursor();
    QVERIFY(c.hasSelection());
    QCOMPARE(m_doc->findBlock(c.selectionStart()).blockNumber(), 9);
    QCOMPARE(m_doc->findBlock(c.selectionEnd()).blockNumber(), 17);
    QVERIFY(m_view->selectedText().contains(QLatin1String("Diagrams use")));
    QVERIFY(m_view->selectedText().contains(QLatin1String("Run the")));
    // Every block stays rendered, selection or not.
    QCOMPARE(m_view->expandedBlockCount(), 0);
    // Shift+click extends from the same anchor.
    const int anchor = c.anchor();
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, Qt::ShiftModifier, QPoint(100, y2 + 2));
    QCOMPARE(m_view->textCursor().anchor(), anchor);
    QVERIFY(m_view->textCursor().hasSelection());
    QCOMPARE(m_view->expandedBlockCount(), 0);
}

void TestVisualView::selectionIsPainted()
{
    m_view->setReadOnly(true);
    const QColor selection = theme::color(theme::Role::selection);
    const QImage before = m_view->grab().toImage();
    const int base = countColor(before, selection);
    QTextCursor c(m_doc->findBlockByNumber(9));
    c.setPosition(m_doc->findBlockByNumber(10).position() + 8, QTextCursor::KeepAnchor);
    m_view->setTextCursor(c);
    const QImage after = m_view->grab().toImage();
    QVERIFY(countColor(after, selection) > base + 500);
    // Selecting inside code and the table paints too.
    const int rich = countColor(after, selection);
    QTextCursor code(m_doc->findBlockByNumber(18));
    code.setPosition(m_doc->findBlockByNumber(21).position(), QTextCursor::KeepAnchor);
    m_view->setTextCursor(code);
    QVERIFY(countColor(m_view->grab().toImage(), selection) > base + 200);
    Q_UNUSED(rich);
}

void TestVisualView::editableDragDefersExpansion()
{
    m_view->setReadOnly(false);
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(0)));
    QVERIFY(m_view->hasFocus());
    const int y1 = yOfLine(9);
    const int y2 = yOfLine(17);
    QVERIFY(y1 >= 0 && y2 > y1);
    QTest::mousePress(m_view->viewport(), Qt::LeftButton, {}, QPoint(100, y1));
    QTest::mouseMove(m_view->viewport(), QPoint(300, y2));
    // Mid-drag nothing opens for editing, so nothing moves under the pointer.
    QVERIFY(m_view->textCursor().hasSelection());
    const int during = m_view->expandedBlockCount();
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, QPoint(300, y2));
    QVERIFY(m_view->textCursor().hasSelection());
    QVERIFY(m_view->expandedBlockCount() > during);
}

void TestVisualView::doubleAndTripleClick()
{
    m_view->setReadOnly(true);
    const int y = yOfLine(9);
    QVERIFY(y >= 0);
    int foundX = -1;
    for (int x = 30; x < 700 && foundX < 0; x += 4) {
        QTest::mouseDClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(x, y));
        if (m_view->textCursor().selectedText() == QLatin1String("Markdown"))
            foundX = x;
    }
    QVERIFY(foundX >= 0);
    QCOMPARE(m_view->expandedBlockCount(), 0);
    // A third click selects the whole paragraph (both source lines).
    QTest::mousePress(m_view->viewport(), Qt::LeftButton, {}, QPoint(foundX, y));
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, QPoint(foundX, y));
    const QTextCursor c = m_view->textCursor();
    QCOMPARE(c.selectionStart(), m_doc->findBlockByNumber(9).position());
    QCOMPARE(c.selectionEnd(), m_doc->findBlockByNumber(11).position());
}

void TestVisualView::selectionMirrorsInSplit()
{
    QTemporaryDir dir;
    QFile file(dir.filePath(QStringLiteral("mirror.md")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("# Title\n\nFirst paragraph here.\n\nSecond paragraph there.\n");
    file.close();
    DocumentTab tab;
    QVERIFY(tab.load(file.fileName()));
    tab.setViewMode(DocumentTab::ViewMode::SourceVisual);
    tab.resize(1200, 500);
    tab.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tab));
    TextView *source = tab.sourceView();
    VisualView *visual = tab.visualView();
    QVERIFY(visual && source->isVisible() && visual->isVisible());
    visual->setReadOnly(true);
    source->setReadOnly(true);
    visual->setFocus();
    QTRY_VERIFY(visual->hasFocus());
    QTextDocument *doc = source->document();

    QSignalSpy sourceSpy(source, &TextView::selectionChanged);
    QSignalSpy visualSpy(visual, &TextView::selectionChanged);
    QSignalSpy tabSpy(&tab, &DocumentTab::cursorPositionChanged);
    QTextCursor c(doc);
    c.setPosition(doc->findBlockByNumber(2).position() + 6);
    c.setPosition(doc->findBlockByNumber(4).position() + 6, QTextCursor::KeepAnchor);
    visual->setTextCursor(c);
    QCOMPARE(source->textCursor().selectionStart(), c.selectionStart());
    QCOMPARE(source->textCursor().selectionEnd(), c.selectionEnd());
    QVERIFY(visual->hasFocus());
    QVERIFY(!source->hasFocus());
    QCOMPARE(visualSpy.count(), 1); // No echo back.
    QCOMPARE(sourceSpy.count(), 0); // Mirroring is silent.
    QVERIFY(!tabSpy.isEmpty());

    // And the other way: the source selection shows in the visual view.
    source->setFocus();
    QTRY_VERIFY(source->hasFocus());
    QTextCursor d(doc);
    d.setPosition(doc->findBlockByNumber(0).position());
    d.setPosition(doc->findBlockByNumber(2).position() + 5, QTextCursor::KeepAnchor);
    source->setTextCursor(d);
    QCOMPARE(visual->textCursor().selectionStart(), d.selectionStart());
    QCOMPARE(visual->textCursor().selectionEnd(), d.selectionEnd());
    QVERIFY(source->hasFocus());
    QCOMPARE(source->textCursor().anchor(), d.anchor());
    // Clearing one clears the other.
    source->setTextCursor(QTextCursor(doc));
    QVERIFY(!visual->textCursor().hasSelection());
}

void TestVisualView::screenshots()
{
    const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR");
    if (dir.isEmpty())
        QSKIP("VN_SCREENSHOT_DIR not set");
    QVERIFY(m_view->grab().save(dir + QStringLiteral("/visual-rendered.png")));
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(15)));
    QVERIFY(m_view->grab().save(dir + QStringLiteral("/visual-editing.png")));
}

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestVisualView)
#include "test_visualview.moc"
