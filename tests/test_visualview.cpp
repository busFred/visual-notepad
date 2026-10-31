// The hybrid visual Markdown view, run offscreen. Set VN_SCREENSHOT_DIR to
// save renderings.
#include "DocumentTab.h"
#include "Settings.h"
#include "Theme.h"
#include "editor/DocumentLayout.h"
#include "markdown/BlockRenderer.h"
#include "markdown/ImageViewer.h"
#include "markdown/MarkdownModel.h"
#include "markdown/VisualView.h"
#include "editor/TextView.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QApplication>
#include <QFile>
#include <QPainter>
#include <QRegularExpression>
#include <QSlider>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QWheelEvent>

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

// Intro, a 60-row table (row N on line 3 + N), a code block and a list.
QString longDocument()
{
    QString text = QStringLiteral("Intro paragraph.\n\n| Row | Value |\n|-----|-------|\n");
    for (int i = 1; i <= 60; ++i)
        text += QStringLiteral("| r%1 | value %1 |\n").arg(i);
    text += QStringLiteral("\n```python\n");
    for (int i = 1; i <= 30; ++i)
        text += QStringLiteral("x%1 = %1\n").arg(i);
    text += QStringLiteral("```\n\n");
    for (int i = 1; i <= 30; ++i)
        text += QStringLiteral("- item %1\n").arg(i);
    text += QStringLiteral("\nA closing paragraph that is long enough to wrap over more than one line in "
                           "the preview, so the end of the document is not a single short line.\n");
    return text;
}

// Exposes the hit test, to read which source line is drawn at a point
// without going through the scroll anchors.
class ProbeView : public VisualView
{
public:
    using VisualView::VisualView;

    int lineAt(qreal y)
    {
        qreal top = 0;
        const int index = rowAt(y, &top);
        const Row r = row(index);
        if (!r.rendered)
            return r.firstLine;
        return document()->findBlock(hitTestRenderedRow(r, QPointF(12, y - top))).blockNumber();
    }

    // Rows cover every line once, in order, and lines map back to them.
    bool rowsConsistent() const
    {
        int next = 0;
        for (int i = 0; i < rowCount(); ++i) {
            const Row r = row(i);
            if (r.firstLine != next || r.lastLine < r.firstLine)
                return false;
            for (int line = r.firstLine; line <= r.lastLine; ++line) {
                if (rowOfLine(line) != i)
                    return false;
            }
            next = r.lastLine + 1;
        }
        return next == document()->blockCount();
    }

    bool lineIsRendered(int line) const { return row(rowOfLine(line)).rendered; }
};

void wheel(TextView *view, qreal dy)
{
    const QPointF pos(50, 50);
    QWheelEvent event(pos, view->viewport()->mapToGlobal(pos), QPoint(0, int(dy)), QPoint(0, 0), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(view->viewport(), &event);
}

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
    void lineMarkersAreInvisible();
    void scrollSyncLinesUpTableRows();
    void selectedImageKeepsPictureVisible();
    void scrollSyncIsContinuous_data();
    void scrollSyncIsContinuous();
    void scrollSyncEnds();
    void editsReparseLater();
    void caretMovesDontReparse();
    void diagramsRenderInBackground();
    void onlyParseSettingsReparse();
    void wideImagesShrinkToColumn();
    void clickEnlargesPicture();
    void networkPaths();
    void networkPathsAreNotOpened_data();
    void networkPathsAreNotOpened();

private:
    // Source and preview of longDocument(), side by side; the source has
    // focus so every block is rendered.
    void openLongDocument();

    QTextDocument *m_doc = nullptr;
    KSyntaxHighlighting::SyntaxHighlighter *m_highlighter = nullptr;
    MarkdownModel *m_model = nullptr;
    VisualView *m_view = nullptr;
    QTextDocument *m_longDoc = nullptr;
    MarkdownModel *m_longModel = nullptr;
    TextView *m_source = nullptr;
    ProbeView *m_preview = nullptr;

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
    delete m_source;
    delete m_preview;
    delete m_longDoc;
    m_source = nullptr;
    m_preview = nullptr;
    m_longDoc = nullptr;
}

void TestVisualView::openLongDocument()
{
    m_view->hide();
    m_longDoc = new QTextDocument;
    m_longDoc->setDocumentLayout(new DocumentLayout(m_longDoc));
    m_longDoc->setPlainText(longDocument());
    m_longModel = new MarkdownModel(m_longDoc);
    m_source = new TextView(m_longDoc);
    m_preview = new ProbeView(m_longModel);
    m_source->setGeometry(0, 0, 600, 500);
    m_preview->setGeometry(610, 0, 600, 500);
    m_source->show();
    m_preview->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_preview));
    m_source->activateWindow();
    m_source->setFocus();
    QVERIFY(QTest::qWaitForWindowActive(m_source));
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

void TestVisualView::lineMarkersAreInvisible()
{
    // The table's rows carry "vn-line:N" anchors; without them it must
    // render the same, with the same text, and still have no links.
    const auto &blocks = m_model->blocks();
    int table = -1;
    for (int i = 0; i < int(blocks.size()); ++i) {
        const QString html = QString::fromUtf8(blocks[i].html.data(), blocks[i].html.size());
        if (html.startsWith(QLatin1String("<table cellspacing")))
            table = i;
    }
    QVERIFY(table >= 0);
    core::Block marked = blocks[table];
    const QString html = QString::fromUtf8(marked.html.data(), marked.html.size());
    QVERIFY(html.contains(QLatin1String("<a name=\"vn-line:2\"></a>")));
    core::Block plain = marked;
    static const QRegularExpression marker(QStringLiteral("<a name=\"vn-line:\\d+\"></a>"));
    const QByteArray stripped = QString(html).remove(marker).toUtf8();
    plain.html = rust::String(stripped.constData(), stripped.size());

    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    const QString source = m_model->sourceText(int(marked.first_line), int(marked.last_line));
    const auto a = RenderedBlock::create(marked, source, ctx);
    const auto b = RenderedBlock::create(plain, source, ctx);
    QCOMPARE(a->height(), b->height());
    QCOMPARE(a->renderedText(), b->renderedText());
    QImage imageA(int(ctx.width), int(a->height()), QImage::Format_ARGB32);
    QImage imageB = imageA;
    imageA.fill(Qt::white);
    imageB.fill(Qt::white);
    {
        QPainter pa(&imageA);
        a->paint(pa, QPointF(0, 0), ctx.width);
        QPainter pb(&imageB);
        b->paint(pb, QPointF(0, 0), ctx.width);
    }
    QCOMPARE(imageA, imageB);
    for (int y = 0; y < a->height(); y += 3) {
        for (int x = 0; x < ctx.width; x += 7)
            QVERIFY(a->anchorAt(QPointF(x, y)).isEmpty());
    }
    // Rows 1 and 2 of the body (lines 2 and 3 of the block) go down the page.
    const auto &anchors = a->lineAnchors();
    QCOMPARE(int(anchors.size()), 2);
    QCOMPARE(anchors[0].first, 2);
    QCOMPARE(anchors[1].first, 3);
    QVERIFY(anchors[0].second > 5 && anchors[1].second > anchors[0].second + 10);
}

void TestVisualView::selectedImageKeepsPictureVisible()
{
    // The math block is an image; selecting it must only tint it lightly and
    // outline it, even where the selection colour is dark.
    const auto &blocks = m_model->blocks();
    QCOMPARE(blocks[10].kind, core::BlockKind::Math);
    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    const auto block = RenderedBlock::create(blocks[10], m_model->sourceText(int(blocks[10].first_line), int(blocks[10].last_line)), ctx);
    QCOMPARE(block->kind(), RenderedBlock::Kind::Image);
    const QColor selection(0x10, 0x20, 0x50); // Dark navy.
    const QString themeId = theme::currentThemeId();
    const QString role = theme::roleName(theme::Role::selection);
    Settings::instance().setColorOverride(themeId, role, selection);
    QCOMPARE(theme::color(theme::Role::selection), selection);
    auto render = [&](bool whole) {
        QImage image(int(ctx.width), int(block->height()), QImage::Format_ARGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        RenderedSelection sel;
        sel.whole = whole;
        block->paint(painter, QPointF(0, 0), ctx.width, sel);
        return image;
    };
    const QImage plain = render(false);
    const QImage selected = render(true);
    Settings::instance().resetColorOverride(themeId, role);
    // Bounding box of what the selection changed.
    QRect changed;
    for (int y = 0; y < plain.height(); ++y)
        for (int x = 0; x < plain.width(); ++x)
            if (plain.pixel(x, y) != selected.pixel(x, y))
                changed |= QRect(x, y, 1, 1);
    QVERIFY(!changed.isEmpty());
    // One pixel outline in the selection colour all around.
    for (int x = changed.left(); x <= changed.right(); ++x) {
        QCOMPARE(QColor(selected.pixel(x, changed.top())), selection);
        QCOMPARE(QColor(selected.pixel(x, changed.bottom())), selection);
    }
    for (int y = changed.top(); y <= changed.bottom(); ++y) {
        QCOMPARE(QColor(selected.pixel(changed.left(), y)), selection);
        QCOMPARE(QColor(selected.pixel(changed.right(), y)), selection);
    }
    // Inside, every channel keeps at least 70% of the plain picture.
    const QRect inner = changed.adjusted(1, 1, -1, -1);
    for (int y = inner.top(); y <= inner.bottom(); ++y) {
        for (int x = inner.left(); x <= inner.right(); ++x) {
            const QColor a(plain.pixel(x, y));
            const QColor b(selected.pixel(x, y));
            QVERIFY(b.red() >= a.red() * 0.7 - 1);
            QVERIFY(b.green() >= a.green() * 0.7 - 1);
            QVERIFY(b.blue() >= a.blue() * 0.7 - 1);
        }
    }
}

void TestVisualView::scrollSyncLinesUpTableRows()
{
    openLongDocument();
    // Source leads: row 30 (line 33) at the top of the source puts row 30 at
    // the top of the preview.
    m_source->scrollToLine(33);
    m_preview->followScroll(*m_source);
    QVERIFY2(qAbs(m_preview->topLinePosition() - 33) < 0.01, qPrintable(QString::number(m_preview->topLinePosition())));
    QCOMPARE(m_preview->lineAt(2), 33);
    QCOMPARE(m_preview->lineAt(8), 33);
    // Preview leads, from row 45 and from halfway through row 20.
    m_preview->scrollToLinePosition(48);
    QCOMPARE(m_preview->lineAt(2), 48);
    m_source->followScroll(*m_preview);
    QCOMPARE(m_source->topLine(), 48);
    QVERIFY(qAbs(m_source->topLinePosition() - 48) < 0.01);
    m_preview->scrollToLinePosition(23.5);
    QCOMPARE(m_preview->lineAt(2), 23);
    m_source->followScroll(*m_preview);
    QVERIFY2(qAbs(m_source->topLinePosition() - 23.5) < 0.05, qPrintable(QString::number(m_source->topLinePosition())));
    // Code lines map one to one too.
    const int code = 68; // "x3 = 3"
    QVERIFY(m_longDoc->findBlockByNumber(code).text().startsWith(QLatin1String("x3 ")));
    m_source->scrollToLine(code);
    m_preview->followScroll(*m_source);
    QVERIFY(qAbs(m_preview->topLinePosition() - code) < 0.01);
}

void TestVisualView::scrollSyncIsContinuous_data()
{
    QTest::addColumn<bool>("sourceLeads");
    QTest::newRow("source leads") << true;
    QTest::newRow("preview leads") << false;
}

void TestVisualView::scrollSyncIsContinuous()
{
    QFETCH(bool, sourceLeads);
    openLongDocument();
    TextView *leader = sourceLeads ? static_cast<TextView *>(m_source) : m_preview;
    TextView *follower = sourceLeads ? static_cast<TextView *>(m_preview) : m_source;
    // Scroll the leader 3 px at a time to the end; the follower must move
    // down steadily: never back, never by more than a fraction of a row.
    const qreal step = 3;
    qreal previous = follower->scrollPixels();
    qreal maxJump = 0;
    int steps = 0;
    while (!leader->isAtScrollEnd() && steps < 5000) {
        wheel(leader, -step);
        follower->followScroll(*leader);
        const qreal y = follower->scrollPixels();
        QVERIFY2(y >= previous - 0.01, qPrintable(QStringLiteral("step %1: %2 -> %3").arg(steps).arg(previous).arg(y)));
        if (!leader->isAtScrollEnd())
            maxJump = qMax(maxJump, y - previous);
        previous = y;
        ++steps;
    }
    QVERIFY(steps > 100);
    QVERIFY2(maxJump <= 4 * step, qPrintable(QStringLiteral("largest follower step %1 px").arg(maxJump)));
    // And back up to the top.
    while (leader->scrollPixels() > 0) {
        wheel(leader, step);
        follower->followScroll(*leader);
    }
    QCOMPARE(follower->scrollPixels(), 0.0);
}

void TestVisualView::scrollSyncEnds()
{
    openLongDocument();
    for (TextView *leader : {static_cast<TextView *>(m_source), static_cast<TextView *>(m_preview)}) {
        TextView *follower = leader == m_source ? static_cast<TextView *>(m_preview) : m_source;
        follower->scrollToLine(0);
        wheel(leader, -100000);
        QVERIFY(leader->isAtScrollEnd());
        follower->followScroll(*leader);
        QVERIFY(follower->isAtScrollEnd());
        // Really the end: the follower cannot scroll further.
        const qreal y = follower->scrollPixels();
        wheel(follower, -50);
        QCOMPARE(follower->scrollPixels(), y);
        leader->scrollToLine(0);
    }
}

void TestVisualView::editsReparseLater()
{
    openLongDocument();
    m_preview->grab();
    const int generation = m_longModel->parseGeneration();
    // Edits shift the parsed blocks without a reparse; rows always match
    // the document's lines.
    QTextCursor c(m_longDoc);
    c.insertText(QStringLiteral("Changed "));
    QVERIFY(m_longModel->hasPendingEdits());
    QVERIFY(m_preview->rowsConsistent());
    // The edited paragraph keeps its last rendering meanwhile.
    QVERIFY(m_preview->lineIsRendered(0));
    c.movePosition(QTextCursor::End);
    c.insertText(QStringLiteral("\n\n# New heading\n\nNew paragraph\n"));
    QVERIFY(m_preview->rowsConsistent());
    c.setPosition(m_longDoc->findBlockByNumber(5).position());
    c.setPosition(m_longDoc->findBlockByNumber(70).position(), QTextCursor::KeepAnchor);
    c.removeSelectedText();
    QVERIFY(m_preview->rowsConsistent());
    c.setPosition(m_longDoc->findBlockByNumber(2).position());
    c.insertText(QStringLiteral("a\nb\nc\n\n"));
    QVERIFY(m_preview->rowsConsistent());
    QVERIFY(!m_preview->grab().isNull());
    QCOMPARE(m_longModel->parseGeneration(), generation);
    // Then one reparse once typing pauses.
    QTRY_VERIFY(!m_longModel->hasPendingEdits());
    QCOMPARE(m_longModel->parseGeneration(), generation + 1);
    QVERIFY(m_preview->rowsConsistent());
    QVERIFY(m_preview->lineIsRendered(0));
    const int heading = m_longDoc->blockCount() - 4;
    QCOMPARE(m_longDoc->findBlockByNumber(heading).text(), QStringLiteral("# New heading"));
    QVERIFY(m_preview->lineIsRendered(heading));
    QVERIFY(m_longModel->blockAt(heading) >= 0);
}

void TestVisualView::caretMovesDontReparse()
{
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(9)));
    QTest::keyClicks(m_view, QStringLiteral("New "));
    // The paragraph being typed in stays open as source.
    QVERIFY(m_view->expandedBlockCount() == 1);
    QTRY_VERIFY(!m_model->hasPendingEdits());
    QCOMPARE(m_view->expandedBlockCount(), 1);
    const int generation = m_model->parseGeneration();
    for (int line : {0, 9, 17, 25, 30, 4})
        m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(line)));
    QTest::keyClick(m_view, Qt::Key_Down);
    QTest::keyClick(m_view, Qt::Key_Down);
    QVERIFY(!m_view->grab().isNull());
    QCOMPARE(m_model->parseGeneration(), generation);
}

void TestVisualView::diagramsRenderInBackground()
{
    const auto &blocks = m_model->blocks();
    const core::Block &diagram = blocks[11];
    QCOMPARE(QString::fromUtf8(diagram.lang.data(), diagram.lang.size()), QStringLiteral("mermaid"));
    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    ctx.devicePixelRatio = 1.5; // Not used by other tests, so not rendered yet.
    const QString source = m_model->sourceText(int(diagram.first_line), int(diagram.last_line));
    const auto first = RenderedBlock::create(diagram, source, ctx);
    // Until the worker is done the source stands in.
    if (first->isPending())
        QCOMPARE(first->kind(), RenderedBlock::Kind::Code);
    QTRY_VERIFY(!RenderedBlock::create(diagram, source, ctx)->isPending());
    const auto done = RenderedBlock::create(diagram, source, ctx);
    QCOMPARE(done->kind(), RenderedBlock::Kind::Image);
    QVERIFY(done->height() > 20);
    // The view swaps its stand-in for the picture by itself (a new theme
    // makes the diagram render again).
    m_view->clearFocus();
    QSignalSpy spy(&DiagramNotifier::instance(), &DiagramNotifier::ready);
    const QString theme = Settings::instance().text(key::mermaidTheme);
    Settings::instance().setValue(key::mermaidTheme, QStringLiteral("neutral"));
    const QImage pending = m_view->grab().toImage();
    QTRY_VERIFY(spy.count() > 0);
    QTRY_VERIFY(m_view->grab().toImage() != pending);
    Settings::instance().setValue(key::mermaidTheme, theme);
}

void TestVisualView::onlyParseSettingsReparse()
{
    m_model->blocks();
    const int generation = m_model->parseGeneration();
    QSignalSpy spy(m_model, &MarkdownModel::optionsChanged);
    Settings &settings = Settings::instance();
    const QVariant lineHeight = settings.value(key::textLineHeight);
    settings.setValue(key::textLineHeight, lineHeight.toDouble() + 0.1);
    QCOMPARE(spy.count(), 1); // Renders are redone...
    m_view->grab();
    QCOMPARE(m_model->parseGeneration(), generation); // ...without a parse.
    settings.setValue(key::textLineHeight, lineHeight);
    const bool breaks = settings.flag(key::breaks);
    settings.setValue(key::breaks, !breaks);
    m_view->grab();
    QCOMPARE(m_model->parseGeneration(), generation + 1);
    settings.setValue(key::breaks, breaks);
}

void TestVisualView::wideImagesShrinkToColumn()
{
    QTemporaryDir dir;
    QImage picture(2000, 500, QImage::Format_RGB32);
    picture.fill(Qt::red);
    QVERIFY(picture.save(dir.filePath(QStringLiteral("wide.png"))));
    const QString source = QStringLiteral("<p align=\"center\"><img src=\"wide.png\" width=\"1060\"></p>");
    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    ctx.width = 400;
    ctx.baseDirectory = dir.path();
    core::Block block;
    block.kind = core::BlockKind::Html;
    const QByteArray html = source.toUtf8();
    block.html = rust::String(html.constData(), html.size());
    const auto rendered = RenderedBlock::create(block, source, ctx);
    // 400 wide keeps the 4:1 shape: 100 high (160 in the paragraph's line
    // height); at the stated 1060 wide it would be over 400.
    QVERIFY2(rendered->height() < 200, qPrintable(QString::number(rendered->height())));
    QVERIFY(rendered->height() > 100);
    const auto picturePoint = rendered->pictureAt(QPointF(200, 50));
    QVERIFY(picturePoint);
    QCOMPARE(picturePoint->title, QStringLiteral("wide.png"));
    QCOMPARE(rendered->pictureImage(*picturePoint).size(), QSize(2000, 500)); // Full size.
    QVERIFY(!rendered->pictureAt(QPointF(200, 190)));
}

void TestVisualView::clickEnlargesPicture()
{
    // Math is the picture in the sample; find its row.
    const auto &blocks = m_model->blocks();
    QCOMPARE(blocks[10].kind, core::BlockKind::Math);
    m_view->setReadOnly(true);
    m_view->grab();
    const int line = int(blocks[10].first_line);
    int y = -1;
    for (int probe = 0; probe < m_view->viewport()->height() && y < 0; probe += 2) {
        // Where the cursor turns to a pointing hand, a click would zoom.
        QTest::mouseMove(m_view->viewport(), QPoint(m_view->viewport()->width() / 2, probe));
        if (m_view->viewport()->cursor().shape() == Qt::PointingHandCursor)
            y = probe;
    }
    Q_UNUSED(line);
    QVERIFY(y >= 0);
    QVERIFY(m_view->window()->findChildren<ImageViewer *>().isEmpty());
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(m_view->viewport()->width() / 2, y));
    const auto viewers = m_view->window()->findChildren<ImageViewer *>();
    QCOMPARE(viewers.size(), 1);
    ImageViewer *viewer = viewers.first();
    const qreal fitted = viewer->scale();
    if (const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR"); !dir.isEmpty())
        viewer->grab().save(dir + QStringLiteral("/image-viewer.png"));
    viewer->zoomAt(QPointF(viewer->width() / 2.0, viewer->height() / 2.0), 2);
    QVERIFY(viewer->scale() >= fitted); // Never below the fitted size.
    viewer->fit();
    QCOMPARE(viewer->scale(), fitted);
    // The slider zooms between the fitted size and the maximum, and follows other zooms.
    auto *slider = viewer->findChild<QSlider *>();
    QVERIFY(slider);
    QCOMPARE(slider->value(), 0);
    slider->setValue(slider->maximum());
    QVERIFY(viewer->scale() > 10);
    slider->setValue(slider->minimum());
    QCOMPARE(viewer->scale(), fitted);
    viewer->zoomAt(QPointF(viewer->width() / 2.0, viewer->height() / 2.0), 2);
    QVERIFY(slider->value() > 0);
    viewer->fit();
    QCOMPARE(slider->value(), 0);
    viewer->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    // Editable: a plain click edits instead.
    m_view->setReadOnly(false);
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(m_view->viewport()->width() / 2, y));
    QVERIFY(m_view->window()->findChildren<ImageViewer *>().isEmpty());
}

void TestVisualView::networkPaths()
{
    for (const char *path : {"//host/share/x.png", "\\\\host\\share\\x.png", "/\\host\\share", "\\/host/share",
                             "\\\\?\\UNC\\host\\share\\x.png", "  //host/x.png"})
        QVERIFY2(md::isNetworkPath(QString::fromUtf8(path)), path);
    for (const char *path : {"x.png", "img/x.png", "../x.png", "/home/user/x.png", "C:/Users/x.png", "C:\\Users\\x.png",
                             "/", ""})
        QVERIFY2(!md::isNetworkPath(QString::fromUtf8(path)), path);
}

void TestVisualView::networkPathsAreNotOpened_data()
{
    // %1 is the picture folder without its leading slash: on Linux "//%1"
    // names the same local folder as "/%1", so a path that slipped through
    // would paint red. On Windows it would be an SMB connection to a host.
    QTest::addColumn<QString>("source");
    QTest::addColumn<bool>("markdown");
    QTest::addColumn<bool>("loads");
    // {img} is an <img> of the given source, 40 by 40.
    const auto row = [](const char *name, const char *source, bool markdown, bool loads) {
        QString s = QString::fromUtf8(source);
        if (s.startsWith(u'='))
            s = QStringLiteral(R"(<img src="%1" width="40" height="40">)").arg(s.mid(1));
        QTest::newRow(name) << s << markdown << loads;
    };
    row("relative", "=red.png", false, true);
    row("absolute", "=/{dir}/red.png", false, true);
    row("file url", "=file:///{dir}/red.png", false, true);
    row("markdown relative", "![](red.png)", true, true);
    row("svg with local picture", "=local.svg", false, true);
    row("host", "=//{dir}/red.png", false, false);
    row("file host", "=file://{dir}/red.png", false, false);
    row("file empty host", "=file:////{dir}/red.png", false, false);
    row("percent encoded", "=%2F%2F{encoded}%2Fred.png", false, false);
    row("markdown host", "![](//{dir}/red.png)", true, false);
    row("markdown file host", "![](file://{dir}/red.png)", true, false);
    row("markdown file empty host", "![](file:////{dir}/red.png)", true, false);
    row("table background", R"(<table background="//{dir}/red.png" width="40"><tr><td>x</td></tr></table>)", false,
        false);
    row("cell background", R"(<table><tr><td background="//{dir}/red.png" width="40">x</td></tr></table>)", false,
        false);
    row("link style sheet", R"(<link rel="stylesheet" type="text/css" href="//{dir}/red.css"><p>xxxxxxxx</p>)", false,
        false);
    row("link local style sheet", R"(<link rel="stylesheet" type="text/css" href="/{dir}/red.css"><p>xxxxxxxx</p>)",
        false, false);
    row("style import", R"(<style>@import url("//{dir}/red.css");</style><p>xxxxxxxx</p>)", false, false);
    row("svg with network picture", "=network.svg", false, false);
}

void TestVisualView::networkPathsAreNotOpened()
{
    QFETCH(QString, source);
    QFETCH(bool, markdown);
    QFETCH(bool, loads);
    QTemporaryDir dir;
    QImage red(40, 40, QImage::Format_RGB32);
    red.fill(Qt::red);
    QVERIFY(red.save(dir.filePath(QStringLiteral("red.png"))));
    QFile css(dir.filePath(QStringLiteral("red.css")));
    QVERIFY(css.open(QIODevice::WriteOnly));
    css.write("p { background-color: #ff0000; }");
    css.close();
    const QString folder = dir.path().mid(1);
    const auto writeSvg = [&](const QString &name, const QString &href) {
        QFile svg(dir.filePath(name));
        QVERIFY(svg.open(QIODevice::WriteOnly));
        svg.write(QStringLiteral(R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" )"
                                 R"(width="40" height="40"><image x="0" y="0" width="40" height="40" xlink:href="%1"/></svg>)")
                      .arg(href)
                      .toUtf8());
    };
    writeSvg(QStringLiteral("local.svg"), QStringLiteral("red.png"));
    writeSvg(QStringLiteral("network.svg"), u"//" + folder + QStringLiteral("/red.png"));
    source.replace(QStringLiteral("{dir}"), folder);
    source.replace(QStringLiteral("{encoded}"), QString(folder).replace(u'/', QStringLiteral("%2F")));

    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    ctx.width = 400;
    ctx.baseDirectory = dir.path();
    QTextDocument text;
    MarkdownModel model(&text);
    core::Block block;
    if (markdown) {
        text.setPlainText(source);
        QCOMPARE(model.blocks().size(), size_t(1));
        block = model.blocks()[0];
    } else {
        block.kind = core::BlockKind::Html;
        const QByteArray html = source.toUtf8();
        block.html = rust::String(html.constData(), html.size());
    }
    const auto rendered = RenderedBlock::create(block, source, ctx);
    QImage image(int(ctx.width), qMax(1, int(rendered->height())), QImage::Format_RGB32);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        rendered->paint(painter, QPointF(0, 0), ctx.width);
    }
    QCOMPARE(countColor(image, Qt::red) > 0, loads);
    if (source.contains(QLatin1String(".svg"))) {
        // The SVG itself is local; its zoomed view must not reach out either.
        const auto picture = rendered->pictureAt(QPointF(20, 20));
        QVERIFY(picture);
        QCOMPARE(rendered->pictureImage(*picture).isNull(), !loads);
    } else if (source.contains(QLatin1String("img")) || markdown) {
        // A click on a picture that did not load must not open it either.
        QCOMPARE(bool(rendered->pictureAt(QPointF(20, 20))), loads);
    }
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
