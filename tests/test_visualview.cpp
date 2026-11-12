// The hybrid visual Markdown view, run offscreen. Set VN_SCREENSHOT_DIR to
// save renderings.
#include "DocumentTab.h"
#include "MainWindow.h"
#include "PlainText.h"
#include "Settings.h"
#include "markdown/MathRenderer.h"
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

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QPainterPath>
#include <QScrollBar>
#include <QSet>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSlider>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QFontInfo>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QThreadPool>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QWheelEvent>

using namespace vn;

namespace {

quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xffffffffu;
    for (const char byte : data) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (~(crc & 1) + 1));
    }
    return ~crc;
}

void appendLittleEndian(QByteArray &out, quint32 value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.append(char((value >> shift) & 0xff));
}

// One gzip member holding `data`. QtCore only has the zlib format
// (qCompress: 4 bytes of size, the zlib header, the deflate stream, an
// Adler-32), so the deflate stream is cut out of that and wrapped by hand.
QByteArray gzipped(const QByteArray &data)
{
    const QByteArray zlib = qCompress(data, 9);
    QByteArray out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    out += zlib.mid(4 + 2, zlib.size() - 4 - 2 - 4);
    appendLittleEndian(out, crc32(data));
    appendLittleEndian(out, quint32(data.size()));
    return out;
}

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

// A document over MarkdownModel::backgroundParseChars (or `chars`): sections
// of a heading, a paragraph, a task list and a code block.
QString largeDocument(qsizetype chars = MarkdownModel::backgroundParseChars + 4096)
{
    QString text;
    for (int i = 0; text.size() <= chars; ++i)
        text += QStringLiteral("## Section %1\n\nA paragraph with *emphasis* and `code` number %1.\n\n"
                               "- [ ] task %1\n- item\n\n```\ncode %1\n```\n\n")
                    .arg(i);
    return text;
}

bool sameBlock(const core::Block &a, const core::Block &b)
{
    if (a.kind != b.kind || a.first_line != b.first_line || a.last_line != b.last_line || a.html != b.html
        || a.lang != b.lang || a.text != b.text || a.inline_math.size() != b.inline_math.size())
        return false;
    for (size_t i = 0; i < a.inline_math.size(); ++i) {
        if (a.inline_math[i] != b.inline_math[i])
            return false;
    }
    return true;
}

// Whether the model's blocks are exactly a fresh parse of its document (the
// model must have no pending edits, or blocks() would parse).
bool matchesFreshParse(MarkdownModel &model)
{
    if (model.hasPendingEdits())
        return false;
    QString text = model.document()->toRawText();
    text.replace(QChar::ParagraphSeparator, u'\n');
    const QByteArray utf8 = text.toUtf8();
    const auto fresh = core::blocks(rust::Str(utf8.constData(), utf8.size()), model.options());
    const auto &blocks = model.blocks();
    if (blocks.size() != fresh.size())
        return false;
    for (size_t i = 0; i < fresh.size(); ++i) {
        if (!sameBlock(blocks[i], fresh[i]))
            return false;
    }
    return true;
}

// Exposes the hit test, to read which source line is drawn at a point
// without going through the scroll anchors.
class ProbeView : public VisualView
{
public:
    using VisualView::VisualView;
    using VisualView::rowCount;

    // The viewport y range of the row holding a line, or false.
    bool boundsOfLine(int line, qreal *top, qreal *bottom) { return lineBounds(line, top, bottom); }
    qreal textStart() const { return textLeft(); }
    using VisualView::hasTableSelection;
    using VisualView::populateSelectionMenu;

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

    // The rendered rows that show some line of [first, last].
    int renderedRowsOver(int first, int last) const
    {
        int rows = 0;
        for (int i = rowOfLine(first); i <= rowOfLine(last); ++i)
            rows += row(i).rendered;
        return rows;
    }

    QTextCursor unitAt(int position) const { return lineUnitAt(position); }

    int rowIndexOf(int line) const { return rowOfLine(line); }
    int firstLineOfRow(int index) const { return row(index).firstLine; }
    qreal heightOfRow(int index) { return row(index).rendered ? renderedRowHeight(row(index), 0) : -1; }
    // Where the row's lines start: (line, y) from the row's top.
    QList<QPointF> anchorsOfRow(int index)
    {
        QList<QPointF> anchors;
        for (const LineAnchor &a : rowLineAnchors(row(index), heightOfRow(index)))
            anchors.append(QPointF(a.line, a.y));
        return anchors;
    }
};

// A rendered list as a view shows it: its text, its height and where its
// lines start, from its top.
struct ListShape
{
    QString text;
    qreal height = 0;
    QList<QPointF> lines;
    int rows = 0;
};

// A list of `count` items made from `item` (%1 is the number), and its lines.
QString listOf(const QString &item, int count)
{
    QString text;
    for (int i = 1; i <= count; ++i)
        text += item.arg(i) + u'\n';
    return text;
}

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
    void singleClickKeepsTheBlockRendered();
    void doubleClickOpensTheSource();
    void escAndBlankClicksCloseTheBlock();
    void typingGoesIntoRenderedText();
    void typingAtTheEndOfBoldContinuesIt();
    void typedMarkdownCharactersAreEscaped();
    void enterInAListContinuesIt();
    void enterInAHeadingStartsAParagraph();
    void backspaceRemovesEmptiedMarkup();
    void selectionIsReplacedAndFormatted();
    void arrowsMoveThroughRenderedText();
    void editsOverSeveralBlocksOpenThem();
    void taskCheckboxToggles();
    void longListsShowAnItemARow_data();
    void longListsShowAnItemARow();
    void typingInALongListRendersOneItem();
    void enterInALongOrderedListRenumbers();
    void backspaceAndDeleteJoinItems();
    void editsAroundALongListsItems();
    void taskBoxesAndLinksInALongList();
    void irregularListsRenderWhole();
    void linkClickEmits();
    void unfocusedRendersEverything();
    void lockedRendersEverything();
    void inverseMapping();
    void dragSelectsAcrossRenderedBlocks();
    void selectionIsPainted();
    void editableDragDefersExpansion();
    void tableIsRealignedWhenTheCaretLeavesIt();
    void tableToolbarFollowsTheCaret();
    void toolbarWorksWithACellOpen();
    void clickingAColumnEdgeSelectsTheColumn();
    void draggingAlongTheLeftEdgeSelectsRows();
    void deletingRowsKeepsTheHeader();
    void draggingFromCellToCellSelectsARectangle();
    void quickInsertAddsRowsAndColumns();
    void tableSelectionNeedsAnEditableView();
    void selectionMenuAndToolbarActOnTheSelection();
    void htmlTableCellsAreEditedInPlace();
    void mergedHtmlCellsOpenTheCellClicked();
    void insertedTableOpensItsFirstCell();
    void clickingATableCellEditsIt();
    void cellEditorKeysMoveBetweenCells();
    void cellEditorOnlyInEditableViews();
    void doubleAndTripleClick();
    void selectionMirrorsInSplit();
    void screenshots();
    void lineMarkersAreInvisible();
    void scrollSyncLinesUpTableRows();
    void selectedImageKeepsPictureVisible();
    void scrollSyncIsContinuous_data();
    void scrollSyncIsContinuous();
    void scrollSyncEnds();
    void selectingInALongTableKeepsTheScroll_data();
    void selectingInALongTableKeepsTheScroll();
    void editsReparseLater();
    void caretMovesDontReparse();
    void diagramsRenderInBackground();
    void oversizedDiagramsRenderOnce();
    void onlyParseSettingsReparse();
    void largeDocumentsParseInBackground();
    void editsDuringBackgroundParseAreKept();
    void flushOvertakesBackgroundParse();
    void largeDocumentsReparseIncrementally();
    void modelDeletedDuringBackgroundParse();
    void modeSwitchDuringBackgroundParse();
    void wideImagesShrinkToColumn();
    void decodedPicturesServeEveryWidth();
    void pictureCacheSettingResizesTheCache();
    void codeBlocksArePaintedAsTheStockHighlighterDoes_data();
    void codeBlocksArePaintedAsTheStockHighlighterDoes();
    void renderCostCountsPicturesAndText();
    void clickEnlargesPicture();
    void networkPaths();
    void plainToolTipText();
    void linkToolTipsArePlainText();
    void rawPictureNamesAreNotOpened_data();
    void rawPictureNamesAreNotOpened();
    void networkPathsAreNotOpened_data();
    void networkPathsAreNotOpened();
    void svgBombsAreRefused();
    void hugeBlocksFallBackToTheirSource_data();
    void hugeBlocksFallBackToTheirSource();
    void inlineMathSitsOnTheBaseline();
    void mathFollowsTheTextSize();
    void wideMathShrinksToTheColumn();
    void mathIsSharpWhenZoomed();
    void lineSpacingIsTheTextsBesideFormulas();
    void smallFormulasKeepTheLineInEveryFont();
    void aCellOfAFormulaAloneSitsOnTheTextsBaseline();
    void ordinaryBlocksIgnoreTheLayoutLimits();
    void longRunsWrapTheSameWithTheLimits();
    void scrollingKeepsOnlyRecentRenders();
    void decodedPictureCacheSurvivesRoundsOfMorePictures();
    void blocksOverTheBudgetAreRenderedOnce();
    void clippedPaintsMatchWholePaints();
    void longCodeBoxPartsLookLikeTheBox();
    void longCodeBoxesShowInParts();
    void largeDocumentsShowTheirSourceUntilParsed();
    void editsWhileShowingTheSourceAreParsed();
    void flushWhileShowingTheSource();
    void switchingToVisualShowsTheSourceFirst();
    void switchingToVisualRendersAtTheFinalWidthOnly();

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
    // Puts text in the view's document, parsed, with the caret at position.
    void setDocument(const QString &text, int position);
    ListShape shapeOf(int first, int last);
    // Lines [first, last] of `text` are a list: shown an item a row, it
    // looks as it does whole.
    void compareWithWholeList(const QString &text, int first, int last);
    // A table of five rows and three columns on line 2, rendered; read-write.
    void useSelectionTable();
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

void TestVisualView::setDocument(const QString &text, int position)
{
    m_doc->setPlainText(text);
    m_model->flush();
    QTextCursor c(m_doc);
    c.setPosition(position);
    m_view->setTextCursor(c);
}

ListShape TestVisualView::shapeOf(int first, int last)
{
    auto *view = static_cast<ProbeView *>(m_view);
    ListShape shape;
    for (int i = view->rowIndexOf(first); i <= view->rowIndexOf(last); ++i) {
        const int line = view->firstLineOfRow(i);
        if (shape.rows > 0) {
            shape.text += u'\n';
            shape.lines.append(QPointF(line, shape.height));
        }
        shape.text += m_view->renderedTextAt(line);
        for (const QPointF &a : view->anchorsOfRow(i)) {
            // (An item and its first paragraph both mark their line.)
            const QPointF at(a.x(), shape.height + a.y());
            if (shape.lines.isEmpty() || shape.lines.last() != at)
                shape.lines.append(at);
        }
        shape.height += view->heightOfRow(i);
        ++shape.rows;
    }
    return shape;
}

void TestVisualView::compareWithWholeList(const QString &text, int first, int last)
{
    m_view->clearFocus(); // No caret in the pictures.
    setDocument(text, 0);
    m_view->scrollToLine(first);
    m_view->grab();
    // (The scroll bar differs: it counts rows.)
    const QImage itemsImage = m_view->viewport()->grab().toImage();
    const ListShape items = shapeOf(first, last);
    VisualView::setItemRowsEnabled(false);
    const auto restore = qScopeGuard([] { VisualView::setItemRowsEnabled(true); });
    setDocument(text, 0);
    m_view->scrollToLine(first);
    m_view->grab();
    const QImage wholeImage = m_view->viewport()->grab().toImage();
    const ListShape whole = shapeOf(first, last);
    QCOMPARE(whole.rows, 1);
    QVERIFY2(items.rows == last - first + 1 || items.rows > 1, "an item a row");
    QCOMPARE(items.text, whole.text);
    QCOMPARE(items.height, whole.height);
    QCOMPARE(items.lines.size(), whole.lines.size());
    for (int i = 0; i < whole.lines.size(); ++i) {
        QCOMPARE(items.lines[i].x(), whole.lines[i].x());
        QCOMPARE(items.lines[i].y(), whole.lines[i].y());
    }
    // The same picture, but for antialiasing: the markers (and Qt's own in
    // nested lists) are placed from the item's top, not the list's, which
    // rounds to whole pixels from another fraction.
    QCOMPARE(itemsImage.size(), wholeImage.size());
    int differ = 0;
    for (int y = 0; y < itemsImage.height(); ++y) {
        for (int x = 0; x < itemsImage.width(); ++x)
            differ += itemsImage.pixel(x, y) != wholeImage.pixel(x, y);
    }
    if (!qEnvironmentVariableIsEmpty("VN_SCREENSHOT_DIR")) {
        itemsImage.save(QDir(qEnvironmentVariable("VN_SCREENSHOT_DIR")).filePath(QStringLiteral("list-items.png")));
        wholeImage.save(QDir(qEnvironmentVariable("VN_SCREENSHOT_DIR")).filePath(QStringLiteral("list-whole.png")));
    }
    QVERIFY2(differ < itemsImage.width() * itemsImage.height() / 200, qPrintable(QString::number(differ)));
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
    m_view = new ProbeView(m_model);
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
    // A caret in the "Goals" paragraph (line 9) leaves it rendered; opened,
    // its two lines become source rows, everything else stays rendered.
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(9)));
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QVERIFY(static_cast<ProbeView *>(m_view)->lineIsRendered(9));
    const QImage before = m_view->grab().toImage();
    m_view->openBlockAtCaret();
    QCOMPARE(m_view->expandedBlockCount(), 1);
    QVERIFY(!static_cast<ProbeView *>(m_view)->lineIsRendered(9));
    QVERIFY(!static_cast<ProbeView *>(m_view)->lineIsRendered(10));
    const QImage after = m_view->grab().toImage();
    QVERIFY(before != after);
    QCOMPARE(m_model->blockAt(9), m_model->blockAt(10));
}

void TestVisualView::typingInExpandedBlockKeepsRows()
{
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(9)));
    m_view->openBlockAtCaret();
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

void TestVisualView::singleClickKeepsTheBlockRendered()
{
    auto *view = static_cast<ProbeView *>(m_view);
    const int y = yOfLine(9, 100);
    QVERIFY(y >= 0);
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(100, y));
    QCOMPARE(m_view->textCursor().blockNumber(), 9);
    QVERIFY(!m_view->textCursor().hasSelection());
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QVERIFY(!m_view->isBlockOpen());
    QVERIFY(view->lineIsRendered(9));
    // The caret is drawn in the rendered text.
    QVERIFY(!m_view->grab().isNull());
}

void TestVisualView::doubleClickOpensTheSource()
{
    auto *view = static_cast<ProbeView *>(m_view);
    const int y = yOfLine(9, 100);
    QVERIFY(y >= 0);
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(100, y));
    const int clicked = m_view->textCursor().position();
    QTest::mouseDClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(100, y));
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, QPoint(100, y));
    QVERIFY(m_view->isBlockOpen());
    QCOMPARE(m_view->expandedBlockCount(), 1);
    QVERIFY(!view->lineIsRendered(9));
    // The caret where the click mapped to, not a word selected.
    QCOMPARE(m_view->textCursor().position(), clicked);
    QVERIFY(!m_view->textCursor().hasSelection());
    // In the source, a double click selects a word, and the block stays open.
    qreal top = 0;
    qreal bottom = 0;
    QVERIFY(view->boundsOfLine(9, &top, &bottom));
    const QPoint word(int(view->textStart()) + 30, int((top + bottom) / 2));
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, word);
    QTest::mouseDClick(m_view->viewport(), Qt::LeftButton, {}, word);
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, word);
    QVERIFY(m_view->textCursor().hasSelection());
    QCOMPARE(m_view->expandedBlockCount(), 1);
    // A read-only view opens nothing.
    m_view->closeBlocks();
    m_view->setReadOnly(true);
    const int y2 = yOfLine(17, 100);
    QTest::mouseDClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(100, y2));
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, QPoint(100, y2));
    QCOMPARE(m_view->expandedBlockCount(), 0);
}

void TestVisualView::escAndBlankClicksCloseTheBlock()
{
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(9)));
    m_view->openBlockAtCaret();
    QCOMPARE(m_view->expandedBlockCount(), 1);
    QTest::keyClick(m_view, Qt::Key_Escape);
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QVERIFY(!m_view->isBlockOpen());
    QCOMPARE(m_view->textCursor().blockNumber(), 9);
    // A click in the side margin closes it too, and opens nothing.
    m_view->openBlockAtCaret();
    QCOMPARE(m_view->expandedBlockCount(), 1);
    qreal top = 0;
    qreal bottom = 0;
    QVERIFY(static_cast<ProbeView *>(m_view)->boundsOfLine(17, &top, &bottom));
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(5, int((top + bottom) / 2)));
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QVERIFY(!m_view->isBlockOpen());
    // The caret leaving an open block closes it.
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(9)));
    m_view->openBlockAtCaret();
    QCOMPARE(m_view->expandedBlockCount(), 1);
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(17)));
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QVERIFY(!m_view->isBlockOpen());
}

void TestVisualView::typingGoesIntoRenderedText()
{
    auto *view = static_cast<ProbeView *>(m_view);
    setDocument(QStringLiteral("# Title\n\nHello world\n"), 14);
    m_view->grab();
    const int renders = m_view->renderCount();
    QTest::keyClicks(m_view, QStringLiteral(" big"));
    QCOMPARE(m_doc->findBlockByNumber(2).text(), QStringLiteral("Hello big world"));
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QVERIFY(view->lineIsRendered(2));
    // Shown at once (before the reparse), the edited block alone rendered again.
    QVERIFY(m_model->hasPendingEdits());
    QCOMPARE(m_view->renderedTextAt(2), QStringLiteral("Hello big world"));
    QCOMPARE(m_view->renderCount() - renders, 4);
    QVERIFY(!m_view->grab().isNull());
    QCOMPARE(m_view->renderCount() - renders, 4);
    // Undone as typing in source is.
    QTest::keyClick(m_view, Qt::Key_Z, Qt::ControlModifier);
    const QString undone = m_doc->findBlockByNumber(2).text();
    QCOMPARE(m_view->renderedTextAt(2), undone);
    QTest::keyClick(m_view, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(m_doc->findBlockByNumber(2).text(), QStringLiteral("Hello big world"));
    // Backspace and Delete take rendered characters.
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QTest::keyClick(m_view, Qt::Key_Delete);
    QCOMPARE(m_doc->findBlockByNumber(2).text(), QStringLiteral("Hello biworld"));
    // Typed on a blank line: a paragraph of its own.
    QTextCursor blank(m_doc->findBlockByNumber(1));
    m_view->setTextCursor(blank);
    QTest::keyClicks(m_view, QStringLiteral("New"));
    QCOMPARE(m_doc->toPlainText(), QStringLiteral("# Title\n\nNew\n\nHello biworld\n"));
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QCOMPARE(m_view->renderedTextAt(2), QStringLiteral("New"));
    QTRY_VERIFY(!m_model->hasPendingEdits());
    QVERIFY(view->rowsConsistent());
    QCOMPARE(m_view->renderedTextAt(2), QStringLiteral("New"));
    // The same typing in source, undone.
    setDocument(QStringLiteral("# Title\n\nHello world\n"), 14);
    m_view->openBlockAtCaret();
    QTest::keyClicks(m_view, QStringLiteral(" big"));
    QTest::keyClick(m_view, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(m_doc->findBlockByNumber(2).text(), undone);
}

void TestVisualView::typingAtTheEndOfBoldContinuesIt()
{
    setDocument(QStringLiteral("Say **bold** now\n"), 0);
    // "Say bold": the caret after the d, inside the run.
    for (int i = 0; i < 8; ++i)
        QTest::keyClick(m_view, Qt::Key_Right);
    QTest::keyClicks(m_view, QStringLiteral("er"));
    QCOMPARE(m_doc->firstBlock().text(), QStringLiteral("Say **bolder** now"));
    // Before the run (after the space), new text is not bold.
    QTextCursor c(m_doc);
    c.setPosition(4);
    m_view->setTextCursor(c);
    QTest::keyClicks(m_view, QStringLiteral("so "));
    QCOMPARE(m_doc->firstBlock().text(), QStringLiteral("Say so **bolder** now"));
    QCOMPARE(m_view->renderedTextAt(0), QStringLiteral("Say so bolder now"));
    QCOMPARE(m_view->expandedBlockCount(), 0);
}

void TestVisualView::typedMarkdownCharactersAreEscaped()
{
    setDocument(QStringLiteral("Plain text\n"), 10);
    QTest::keyClicks(m_view, QStringLiteral(" *not* [x] `y` a_b # | ~ \\"));
    QCOMPARE(m_doc->firstBlock().text(),
             QStringLiteral("Plain text \\*not\\* \\[x\\] \\`y\\` a\\_b \\# \\| \\~ \\\\"));
    QCOMPARE(m_view->renderedTextAt(0), QStringLiteral("Plain text *not* [x] `y` a_b # | ~ \\"));
    // At the start of a line, list and quote markers are escaped too.
    setDocument(QStringLiteral("\n"), 0);
    QTest::keyClicks(m_view, QStringLiteral("- 1. > x"));
    QCOMPARE(m_doc->firstBlock().text(), QStringLiteral("\\- 1. > x"));
    setDocument(QStringLiteral("\n"), 0);
    QTest::keyClicks(m_view, QStringLiteral("12. x"));
    QCOMPARE(m_doc->firstBlock().text(), QStringLiteral("12\\. x"));
    QCOMPARE(m_view->renderedTextAt(0), QStringLiteral("12. x"));
    QCOMPARE(m_view->expandedBlockCount(), 0);
}

void TestVisualView::enterInAListContinuesIt()
{
    setDocument(QStringLiteral("- one\n- two\n\n1. first\n"), 5);
    QTest::keyClick(m_view, Qt::Key_Return);
    QTest::keyClicks(m_view, QStringLiteral("new"));
    QCOMPARE(m_doc->toPlainText(), QStringLiteral("- one\n- new\n- two\n\n1. first\n"));
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QTextCursor c(m_doc->findBlockByNumber(4));
    c.movePosition(QTextCursor::EndOfBlock);
    m_view->setTextCursor(c);
    QTest::keyClick(m_view, Qt::Key_Return);
    QTest::keyClicks(m_view, QStringLiteral("second"));
    QCOMPARE(m_doc->findBlockByNumber(5).text(), QStringLiteral("2. second"));
    // In a paragraph, Enter starts a new one.
    setDocument(QStringLiteral("one two\n"), 3);
    QTest::keyClick(m_view, Qt::Key_Return);
    QCOMPARE(m_doc->toPlainText(), QStringLiteral("one\n\n two\n"));
    QCOMPARE(m_view->textCursor().blockNumber(), 2);
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QVERIFY(static_cast<ProbeView *>(m_view)->rowsConsistent());
}

void TestVisualView::enterInAHeadingStartsAParagraph()
{
    setDocument(QStringLiteral("# Title\n\nText\n"), 4);
    QTest::keyClick(m_view, Qt::Key_Return);
    QTest::keyClicks(m_view, QStringLiteral("New"));
    QCOMPARE(m_doc->toPlainText(), QStringLiteral("# Title\n\nNew\n\nText\n"));
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QCOMPARE(m_view->renderedTextAt(0), QStringLiteral("Title"));
}

void TestVisualView::backspaceRemovesEmptiedMarkup()
{
    // The caret after the b: deleting it leaves no empty "****".
    setDocument(QStringLiteral("a **b** c\n"), 5);
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QCOMPARE(m_doc->firstBlock().text(), QStringLiteral("a  c"));
    QCOMPARE(m_view->expandedBlockCount(), 0);
    // An escape goes with its character.
    setDocument(QStringLiteral("x \\* \\| y\n"), 7);
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QCOMPARE(m_doc->firstBlock().text(), QStringLiteral("x \\*  y"));
    QTest::keyClick(m_view, Qt::Key_Left);
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QCOMPARE(m_doc->firstBlock().text(), QStringLiteral("x   y"));
    // At the start of a block: joined with the line before, as source.
    setDocument(QStringLiteral("one\n\ntwo\n"), 5);
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QCOMPARE(m_doc->toPlainText(), QStringLiteral("one\ntwo\n"));
    QVERIFY(m_view->isBlockOpen());
    QCOMPARE(m_view->expandedBlockCount(), 1);
}

void TestVisualView::selectionIsReplacedAndFormatted()
{
    setDocument(QStringLiteral("Hello big world\n"), 6);
    QTest::keyClick(m_view, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(m_view, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(m_view, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(m_view->textCursor().selectedText(), QStringLiteral("big"));
    QTest::keyClicks(m_view, QStringLiteral("small"));
    QCOMPARE(m_doc->firstBlock().text(), QStringLiteral("Hello small world"));
    // Bold on a selection: wrapped, still rendered.
    QTextCursor c(m_doc);
    c.setPosition(12);
    c.setPosition(17, QTextCursor::KeepAnchor);
    m_view->setTextCursor(c);
    m_view->formatMarkdown(TextView::MarkdownFormat::Bold);
    QCOMPARE(m_doc->firstBlock().text(), QStringLiteral("Hello small **world**"));
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QCOMPARE(m_view->renderedTextAt(0), QStringLiteral("Hello small world"));
    // Typing over a bold selection keeps it bold.
    c.setPosition(14);
    c.setPosition(19, QTextCursor::KeepAnchor);
    m_view->setTextCursor(c);
    QTest::keyClicks(m_view, QStringLiteral("there"));
    QCOMPARE(m_doc->firstBlock().text(), QStringLiteral("Hello small **there**"));
    // Deleting all of it removes the markup.
    c.setPosition(14);
    c.setPosition(19, QTextCursor::KeepAnchor);
    m_view->setTextCursor(c);
    QTest::keyClick(m_view, Qt::Key_Delete);
    QCOMPARE(m_doc->firstBlock().text(), QStringLiteral("Hello small "));
}

void TestVisualView::arrowsMoveThroughRenderedText()
{
    setDocument(QStringLiteral("# Head\n\nSome **x** text\n"), 2);
    // Right from the end of the heading: the start of the paragraph's text.
    for (int i = 0; i < 4; ++i)
        QTest::keyClick(m_view, Qt::Key_Right);
    QCOMPARE(m_view->textCursor().position(), 6);
    QTest::keyClick(m_view, Qt::Key_Right);
    QCOMPARE(m_view->textCursor().position(), 8);
    QTest::keyClick(m_view, Qt::Key_Left);
    QCOMPARE(m_view->textCursor().position(), 6);
    QTest::keyClick(m_view, Qt::Key_Down);
    QCOMPARE(m_view->textCursor().blockNumber(), 2);
    QTest::keyClick(m_view, Qt::Key_End);
    QCOMPARE(m_view->textCursor().position(), m_doc->findBlockByNumber(2).position() + 15);
    QTest::keyClick(m_view, Qt::Key_Home);
    QCOMPARE(m_view->textCursor().position(), m_doc->findBlockByNumber(2).position());
    QTest::keyClick(m_view, Qt::Key_Up);
    QCOMPARE(m_view->textCursor().blockNumber(), 0);
    QCOMPARE(m_view->expandedBlockCount(), 0);
}

void TestVisualView::editsOverSeveralBlocksOpenThem()
{
    // A selection from the "Goals" paragraph into the list: typing replaces
    // it in the source, the blocks open.
    QTextCursor c(m_doc->findBlockByNumber(10));
    c.setPosition(m_doc->findBlockByNumber(12).position() + 4, QTextCursor::KeepAnchor);
    m_view->setTextCursor(c);
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QTest::keyClicks(m_view, QStringLiteral("Z"));
    QCOMPARE(m_doc->findBlockByNumber(10).text(), QStringLiteral("Z] Visual editor"));
    QVERIFY(m_view->isBlockOpen());
    QVERIFY(m_view->expandedBlockCount() >= 1);
    // Several carets: typed in the source too.
    m_view->closeBlocks();
    QTextCursor first(m_doc->findBlockByNumber(17));
    QTextCursor second(m_doc->findBlockByNumber(9));
    m_view->setCursors({first, second}, 0);
    QTest::keyClicks(m_view, QStringLiteral("Q"));
    QVERIFY(m_doc->findBlockByNumber(17).text().startsWith(u'Q'));
    QVERIFY(m_view->isBlockOpen());
    // Typing in a picture does nothing.
    m_view->closeBlocks();
    const QString before = m_doc->toPlainText();
    const QTextBlock tex = m_doc->find(QStringLiteral("f\\colon")).block();
    QVERIFY(tex.previous().text().startsWith(QLatin1String("$$")));
    m_view->setTextCursor(QTextCursor(tex));
    QTest::keyClicks(m_view, QStringLiteral("w"));
    QCOMPARE(m_doc->toPlainText(), before);
    QCOMPARE(m_view->expandedBlockCount(), 0);
    // A block too long to render again at every keystroke (a paragraph over
    // 2000 characters): typed into as source (escapes are for rendered text
    // only).
    QString paragraph;
    for (int i = 0; i < 300; ++i)
        paragraph += QStringLiteral("word%1 ").arg(i);
    setDocument(paragraph.trimmed() + u'\n', 4);
    QTest::keyClicks(m_view, QStringLiteral("*"));
    QVERIFY(m_doc->firstBlock().text().startsWith(QLatin1String("word*0 word1")));
    QVERIFY(m_view->isBlockOpen());
    QCOMPARE(m_view->expandedBlockCount(), 1);
    // A long list is typed into an item at a time, whatever its length.
    QString list;
    for (int i = 0; i < 60; ++i)
        list += QStringLiteral("- item %1\n").arg(i);
    setDocument(list, 6);
    QTest::keyClicks(m_view, QStringLiteral("*"));
    QCOMPARE(m_doc->firstBlock().text(), QStringLiteral("- item\\* 0"));
    QVERIFY(!m_view->isBlockOpen());
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

void TestVisualView::longListsShowAnItemARow_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<int>("first");
    QTest::addColumn<int>("last");
    const QString after = QStringLiteral("\nAfter.\n");
    QTest::newRow("bullets") << QStringLiteral("# Items\n\n")
                                    + listOf(QStringLiteral("- item %1 with **bold**, `code` and a [link](https://example.com/%1)"), 300)
                                    + after
                             << 2 << 301;
    QTest::newRow("ordered from 7") << listOf(QStringLiteral("%1. item"), 40).replace(QStringLiteral("1. item\n2."), QStringLiteral("7. item\n2."))
                                    << 0 << 39;
    QTest::newRow("tasks") << listOf(QStringLiteral("- [ ] task %1 to do"), 30) << 0 << 29;
    QTest::newRow("some tasks") << listOf(QStringLiteral("- [x] task %1\n- plain %1"), 15) << 0 << 29;
    QTest::newRow("loose") << listOf(QStringLiteral("- item %1\n"), 30) << 0 << 58;
    QTest::newRow("nested") << listOf(QStringLiteral("1. outer %1\n   - inner a\n   - inner b\n     1. deep"), 20) << 0 << 79;
    QTest::newRow("paragraphs and code")
        << listOf(QStringLiteral("- first %1\n\n  second paragraph\n\n  ```\n  code\n  ```\n\n  > quoted"), 20) << 0 << 179;
    QTest::newRow("wrapping") << listOf(QStringLiteral("- %1 ") + QStringLiteral("long words ").repeated(40), 20) << 0 << 19;
}

void TestVisualView::longListsShowAnItemARow()
{
    QFETCH(QString, text);
    QFETCH(int, first);
    QFETCH(int, last);
    compareWithWholeList(text, first, last);
}

void TestVisualView::typingInALongListRendersOneItem()
{
    auto *view = static_cast<ProbeView *>(m_view);
    setDocument(listOf(QStringLiteral("- item %1 with **bold** text"), 300), 0);
    QTextCursor c(m_doc->findBlockByNumber(150));
    c.movePosition(QTextCursor::EndOfBlock);
    m_view->setTextCursor(c);
    m_view->grab();
    QCOMPARE(m_view->itemMarkerAt(150), QStringLiteral("\u2022"));
    const int renders = m_view->renderCount();
    QTest::keyClicks(m_view, QStringLiteral("xy"));
    QCOMPARE(m_doc->findBlockByNumber(150).text(), QStringLiteral("- item 151 with **bold** textxy"));
    QVERIFY(!m_view->isBlockOpen());
    QCOMPARE(m_view->renderedTextAt(150), QStringLiteral("item 151 with bold textxy"));
    m_view->grab();
    // The item alone, once a keystroke.
    QCOMPARE(m_view->renderCount() - renders, 2);
    QVERIFY(view->rowsConsistent());
    // The document's parse after the pause renders nothing again.
    QTRY_VERIFY(!m_model->hasPendingEdits());
    m_view->grab();
    QCOMPARE(m_view->renderCount() - renders, 2);
    QCOMPARE(m_view->renderedTextAt(150), QStringLiteral("item 151 with bold textxy"));
    QVERIFY(view->rowsConsistent());
    // Typed in a nested item: its item again.
    setDocument(listOf(QStringLiteral("- item %1\n  - inner %1"), 30), 0);
    c = QTextCursor(m_doc->findBlockByNumber(21));
    c.movePosition(QTextCursor::EndOfBlock);
    m_view->setTextCursor(c);
    m_view->grab();
    const int before = m_view->renderCount();
    QTest::keyClicks(m_view, QStringLiteral("z"));
    QCOMPARE(m_doc->findBlockByNumber(21).text(), QStringLiteral("  - inner 11z"));
    QCOMPARE(m_view->renderedTextAt(20), QStringLiteral("item 11\ninner 11z"));
    m_view->grab();
    QCOMPARE(m_view->renderCount() - before, 1);
    QVERIFY(!m_view->isBlockOpen());
}

void TestVisualView::enterInALongOrderedListRenumbers()
{
    auto *view = static_cast<ProbeView *>(m_view);
    setDocument(listOf(QStringLiteral("%1. item %1"), 300), 0);
    QTextCursor c(m_doc->findBlockByNumber(149));
    c.movePosition(QTextCursor::EndOfBlock);
    m_view->setTextCursor(c);
    m_view->grab();
    const int renders = m_view->renderCount();
    QTest::keyClick(m_view, Qt::Key_Return);
    m_view->grab();
    // The new item alone (the one it was split from is unchanged).
    QVERIFY2(m_view->renderCount() - renders <= 2, qPrintable(QString::number(m_view->renderCount() - renders)));
    QCOMPARE(m_doc->findBlockByNumber(150).text(), QStringLiteral("151. "));
    QCOMPARE(m_view->itemMarkerAt(150), QStringLiteral("151."));
    QTest::keyClicks(m_view, QStringLiteral("new"));
    QCOMPARE(m_doc->findBlockByNumber(150).text(), QStringLiteral("151. new"));
    QCOMPARE(m_doc->findBlockByNumber(151).text(), QStringLiteral("151. item 151"));
    QVERIFY(!m_view->isBlockOpen());
    QCOMPARE(m_view->itemMarkerAt(0), QStringLiteral("1."));
    QCOMPARE(m_view->itemMarkerAt(149), QStringLiteral("150."));
    QCOMPARE(m_view->itemMarkerAt(151), QStringLiteral("152."));
    QCOMPARE(m_view->itemMarkerAt(300), QStringLiteral("301."));
    QCOMPARE(m_view->renderedTextAt(150), QStringLiteral("new"));
    QVERIFY(view->rowsConsistent());
    QTRY_VERIFY(!m_model->hasPendingEdits());
    QCOMPARE(m_view->itemMarkerAt(151), QStringLiteral("152."));
    QCOMPARE(m_view->itemMarkerAt(300), QStringLiteral("301."));
    // A list starting at 5 counts from there.
    setDocument(listOf(QStringLiteral("%1) item"), 40).replace(QStringLiteral("1) item\n2)"), QStringLiteral("5) item\n2)")), 0);
    QCOMPARE(m_view->itemMarkerAt(0), QStringLiteral("5."));
    QCOMPARE(m_view->itemMarkerAt(39), QStringLiteral("44."));
}

void TestVisualView::backspaceAndDeleteJoinItems()
{
    auto *view = static_cast<ProbeView *>(m_view);
    setDocument(listOf(QStringLiteral("- item %1 **b**"), 300), 0);
    // At the start of item 151's text (after its marker).
    QTextCursor c(m_doc->findBlockByNumber(150));
    c.setPosition(c.position() + 2);
    m_view->setTextCursor(c);
    m_view->grab();
    const int renders = m_view->renderCount();
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QCOMPARE(m_doc->findBlockByNumber(149).text(), QStringLiteral("- item 150 **b**item 151 **b**"));
    QCOMPARE(m_doc->findBlockByNumber(150).text(), QStringLiteral("- item 152 **b**"));
    QCOMPARE(m_doc->blockCount(), 300);
    QVERIFY(!m_view->isBlockOpen());
    QCOMPARE(m_view->renderedTextAt(149), QStringLiteral("item 150 bitem 151 b"));
    QCOMPARE(m_view->textCursor().position(), m_doc->findBlockByNumber(149).position() + 16);
    m_view->grab();
    // The joined item, and the one that comes into view at the bottom.
    QVERIFY(m_view->renderCount() - renders <= 2);
    // Delete at the end of an item's text joins the next one to it.
    c = QTextCursor(m_doc->findBlockByNumber(10));
    c.movePosition(QTextCursor::EndOfBlock);
    m_view->setTextCursor(c);
    QTest::keyClick(m_view, Qt::Key_Delete);
    QCOMPARE(m_doc->findBlockByNumber(10).text(), QStringLiteral("- item 11 **b**item 12 **b**"));
    QVERIFY(!m_view->isBlockOpen());
    QVERIFY(view->rowsConsistent());
    // A selection over items is deleted as in the whole list (as source:
    // the markers between them would stay in rendered text).
    c = QTextCursor(m_doc->findBlockByNumber(20));
    c.setPosition(c.position() + 4);
    c.setPosition(m_doc->findBlockByNumber(22).position() + 4, QTextCursor::KeepAnchor);
    m_view->setTextCursor(c);
    QTest::keyClick(m_view, Qt::Key_Delete);
    QCOMPARE(m_doc->findBlockByNumber(20).text(), QStringLiteral("- item 24 **b**"));
    m_view->closeBlocks();
    // In a short list too.
    setDocument(QStringLiteral("- one\n- two\n"), 8);
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QCOMPARE(m_doc->toPlainText(), QStringLiteral("- onetwo\n"));
    QVERIFY(!m_view->isBlockOpen());
    // The first item of a long list joins the block before, as source.
    setDocument(QStringLiteral("Before\n\n") + listOf(QStringLiteral("- item %1"), 30), 10);
    QTest::keyClick(m_view, Qt::Key_Backspace);
    QVERIFY(m_view->isBlockOpen());
}

void TestVisualView::editsAroundALongListsItems()
{
    auto *view = static_cast<ProbeView *>(m_view);
    // Edits in two items before the parse: both are parsed again, the rest
    // keeps its items.
    setDocument(listOf(QStringLiteral("- item %1"), 100), 0);
    QTextCursor c(m_doc->findBlockByNumber(10));
    c.movePosition(QTextCursor::EndOfBlock);
    m_view->setTextCursor(c);
    QTest::keyClick(m_view, Qt::Key_Return);
    QTest::keyClicks(m_view, QStringLiteral("a"));
    c = QTextCursor(m_doc->findBlockByNumber(60));
    c.movePosition(QTextCursor::EndOfBlock);
    m_view->setTextCursor(c);
    QTest::keyClicks(m_view, QStringLiteral("b"));
    QVERIFY(m_model->hasPendingEdits());
    QCOMPARE(m_view->renderedTextAt(11), QStringLiteral("a"));
    QCOMPARE(m_view->renderedTextAt(60), QStringLiteral("item 60b"));
    QCOMPARE(m_view->renderedTextAt(99), QStringLiteral("item 99"));
    QCOMPARE(m_view->itemMarkerAt(100), QStringLiteral("\u2022"));
    QVERIFY(view->rowsConsistent());
    // A different bullet starts another list: the edited list shows whole
    // until the parse says so.
    QTextCursor star(m_doc->findBlockByNumber(30));
    star.setPosition(star.position() + 1, QTextCursor::KeepAnchor);
    star.insertText(QStringLiteral("*"));
    m_view->grab();
    QCOMPARE(m_view->itemMarkerAt(30), QString());
    QTRY_VERIFY(!m_model->hasPendingEdits());
    QCOMPARE(m_view->itemMarkerAt(31), QStringLiteral("\u2022"));
    QVERIFY(view->rowsConsistent());
    // Typing into a list from another view (Split): its items show it.
    setDocument(listOf(QStringLiteral("- item %1"), 100), 0);
    QTextCursor other(m_doc->findBlockByNumber(70));
    other.movePosition(QTextCursor::EndOfBlock);
    other.insertText(QStringLiteral(" more"));
    m_view->grab();
    QCOMPARE(m_view->renderedTextAt(70), QStringLiteral("item 71 more"));
    QCOMPARE(m_view->itemMarkerAt(70), QStringLiteral("\u2022"));
    QVERIFY(view->rowsConsistent());
}

void TestVisualView::taskBoxesAndLinksInALongList()
{
    // (Another bullet: another list.)
    QString text = listOf(QStringLiteral("- [ ] task %1"), 150) + u'\n' + listOf(QStringLiteral("* see [page %1](https://example.com/%1)"), 150);
    setDocument(text, 0);
    // A box far down the list (its line in the document, not the item's).
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(120)));
    m_view->grab();
    auto *view = static_cast<ProbeView *>(m_view);
    qreal top = 0;
    qreal bottom = 0;
    QVERIFY(view->boundsOfLine(120, &top, &bottom));
    bool clicked = false;
    for (int x = int(view->textStart()); x < int(view->textStart()) + 120 && !clicked; x += 2) {
        const QPoint at(x, int(top) + 8);
        QTest::mouseMove(m_view->viewport(), at);
        if (m_view->viewport()->cursor().shape() == Qt::PointingHandCursor) {
            QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, at + QPoint(2, 0));
            clicked = true;
        }
    }
    QVERIFY(clicked);
    QCOMPARE(m_doc->findBlockByNumber(120).text(), QStringLiteral("- [x] task 121"));
    QVERIFY(!m_view->isBlockOpen());
    QCOMPARE(m_doc->findBlockByNumber(119).text(), QStringLiteral("- [ ] task 120"));
    // A list of tasks has no bullets; one with some plain items has.
    QCOMPARE(m_view->itemMarkerAt(120), QString());
    QCOMPARE(m_view->itemMarkerAt(200), QStringLiteral("\u2022"));
    // Links.
    QSignalSpy spy(m_view, &VisualView::linkActivated);
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(250)));
    m_view->grab();
    QVERIFY(view->boundsOfLine(250, &top, &bottom));
    for (int x = int(view->textStart()); x < int(view->textStart()) + 300 && spy.isEmpty(); x += 3) {
        const QPoint at(x, int(top) + 8);
        QTest::mouseMove(m_view->viewport(), at);
        if (m_view->viewport()->cursor().shape() == Qt::PointingHandCursor)
            QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, at);
    }
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.first().first().toString(), QStringLiteral("https://example.com/100"));
}

void TestVisualView::irregularListsRenderWhole()
{
    auto *view = static_cast<ProbeView *>(m_view);
    // An item opening with a list (Qt starts it without a marker): whole.
    setDocument(listOf(QStringLiteral("- item %1"), 30) + QStringLiteral("- - nested first\n"), 0);
    m_view->grab();
    QCOMPARE(m_view->itemMarkerAt(0), QString());
    QCOMPARE(view->renderedRowsOver(0, 30), 1);
    // Raw HTML spanning items: whole.
    setDocument(QStringLiteral("- <b>open\n") + listOf(QStringLiteral("- item %1"), 30) + QStringLiteral("- close</b>\n"), 0);
    m_view->grab();
    QCOMPARE(view->renderedRowsOver(0, 31), 1);
    // A short list: whole.
    setDocument(listOf(QStringLiteral("- item %1"), 10), 0);
    QCOMPARE(view->renderedRowsOver(0, 9), 1);
    QCOMPARE(m_view->itemMarkerAt(0), QString());
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
    m_view->openBlockAtCaret();
    const QImage focused = m_view->grab().toImage();
    m_view->clearFocus();
    QVERIFY(m_view->grab().toImage() != focused);
}

void TestVisualView::lockedRendersEverything()
{
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(9)));
    m_view->openBlockAtCaret();
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
    // A block open as source, the drag starting in it.
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(100, y1));
    QTest::mouseDClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(100, y1));
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, QPoint(100, y1));
    QCOMPARE(m_view->expandedBlockCount(), 1);
    const int y3 = y2 + 40; // The open paragraph is taller now.
    QTest::mousePress(m_view->viewport(), Qt::LeftButton, {}, QPoint(100, y1));
    QTest::mouseMove(m_view->viewport(), QPoint(300, y3));
    // Mid-drag nothing opens or closes, so nothing moves under the pointer.
    QVERIFY(m_view->textCursor().hasSelection());
    QCOMPARE(m_view->expandedBlockCount(), 1);
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, QPoint(300, y3));
    QVERIFY(m_view->textCursor().hasSelection());
    // The selection ended outside the open block: it closed, and the
    // selection shows in rendered text.
    QCOMPARE(m_view->expandedBlockCount(), 0);
}

void TestVisualView::tableIsRealignedWhenTheCaretLeavesIt()
{
    m_view->setReadOnly(false);
    m_view->setMarkdownEditing(true); // (DocumentTab does this for Markdown.)
    QTextCursor c(m_doc->findBlockByNumber(26));
    c.setPosition(c.block().position() + 2);
    m_view->setTextCursor(c);
    QTest::keyClicks(m_view, QStringLiteral("A much longer name "));
    // Typing leaves the table as it is.
    QCOMPARE(m_doc->findBlockByNumber(24).text(), QStringLiteral("| Platform | Status |"));
    QVERIFY(m_doc->findBlockByNumber(26).text().startsWith(QLatin1String("| A much longer name Linux")));
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(9)));
    const QString header = m_doc->findBlockByNumber(24).text();
    QVERIFY2(header.startsWith(QLatin1String("| Platform ")) && header.size() > 20, qPrintable(header));
    // All rows have one width again.
    QCOMPARE(m_doc->findBlockByNumber(26).text().size(), header.size());
    QCOMPARE(m_doc->findBlockByNumber(27).text().size(), header.size());
    // The caret stayed where it was put.
    QCOMPARE(m_view->textCursor().blockNumber(), 9);
    // Passing through a table without editing it changes nothing.
    const QString before = m_doc->toPlainText();
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(26)));
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(9)));
    QCOMPARE(m_doc->toPlainText(), before);
}

void TestVisualView::tableToolbarFollowsTheCaret()
{
    m_view->setReadOnly(false);
    m_view->setMarkdownEditing(true);
    const auto toolbar = [this]() -> QWidget * {
        QTest::qWait(5); // The update runs once the event is done.
        return m_view->viewport()->findChild<QWidget *>(QStringLiteral("TableToolbar"));
    };
    QVERIFY(!toolbar());
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(26)));
    QWidget *bar = toolbar();
    QVERIFY(bar && !bar->isHidden());
    // Above the table's first line, inside the viewport.
    QVERIFY(bar->y() >= 0 && bar->y() < m_view->viewport()->height());
    if (const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR"); !dir.isEmpty())
        QVERIFY(m_view->grab().save(dir + QStringLiteral("/table-toolbar.png")));
    // An operation button edits the table around the caret.
    const int lines = m_doc->blockCount();
    const auto buttons = bar->findChildren<QToolButton *>(QStringLiteral("TableToolButton"));
    QToolButton *below = nullptr;
    for (QToolButton *b : buttons) {
        if (b->toolTip().contains(QLatin1String("row below")))
            below = b;
    }
    QVERIFY(below && below->isEnabled());
    // As a real click arrives: at the window, routed to whatever is under it.
    QWidget *window = m_view->window();
    QVERIFY(window->childAt(below->mapTo(window, below->rect().center())) == below);
    QTest::mouseClick(window->windowHandle(), Qt::LeftButton, {}, below->mapTo(window, below->rect().center()));
    QCOMPARE(m_doc->blockCount(), lines + 1);
    QVERIFY(m_view->hasFocus()); // The click did not take the focus.
    // Not in a table: gone. Read-only: gone.
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(9)));
    QVERIFY(toolbar()->isHidden());
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(26)));
    QVERIFY(!toolbar()->isHidden());
    m_view->setReadOnly(true);
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(27)));
    QVERIFY(toolbar()->isHidden());
}

namespace {

// The open cell editor (closed ones wait to be deleted, hidden).
QPlainTextEdit *cellEditor(QWidget *viewport)
{
    for (auto *editor : viewport->findChildren<QPlainTextEdit *>(QStringLiteral("TableCellEditor"))) {
        if (!editor->isHidden())
            return editor;
    }
    return nullptr;
}

// Where cell (row, col) of the sample's table (line 24) is, in the viewport:
// the rows are 38 pixels apart, the first column is the narrower one.
QPoint cellPoint(VisualView *view, int row, int col)
{
    auto *probe = static_cast<ProbeView *>(view);
    qreal top = 0;
    qreal bottom = 0;
    if (!probe->boundsOfLine(24, &top, &bottom))
        return {};
    return QPoint(int(probe->textStart()) + (col == 0 ? 30 : 100), int(top) + 18 + 38 * row);
}

// Clicks a cell and returns the editor that opens over it, if any.
QPlainTextEdit *clickCell(VisualView *view, int row, int col)
{
    QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, cellPoint(view, row, col));
    return cellEditor(view->viewport());
}

} // namespace

void TestVisualView::clickingATableCellEditsIt()
{
    m_view->setReadOnly(false);
    m_view->setMarkdownEditing(true);
    QPlainTextEdit *editor = clickCell(m_view, 1, 0);
    QVERIFY(editor && editor->hasFocus());
    QCOMPARE(editor->toPlainText(), QStringLiteral("Linux"));
    if (const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR"); !dir.isEmpty()) {
        QTest::qWait(10);
        QVERIFY(m_view->grab().save(dir + QStringLiteral("/table-cell-editor.png")));
    }
    // The table stays rendered: no block was opened for editing, the caret did not move.
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QTest::keyClicks(editor, QStringLiteral(" Mint"));
    QTest::keyClick(editor, Qt::Key_Return);
    QVERIFY(!cellEditor(m_view->viewport()));
    // The cell is written and the table realigned, as one undo step.
    QCOMPARE(m_doc->findBlockByNumber(26).text(), QStringLiteral("| Linux Mint |   done |"));
    QCOMPARE(m_doc->findBlockByNumber(24).text().size(), m_doc->findBlockByNumber(26).text().size());
    m_doc->undo();
    QCOMPARE(m_doc->findBlockByNumber(26).text(), QStringLiteral("| Linux    | done   |"));
    // Escape drops the edit; a pipe in the text is escaped.
    editor = clickCell(m_view, 1, 0);
    QVERIFY(editor);
    QTest::keyClicks(editor, QStringLiteral("x"));
    QTest::keyClick(editor, Qt::Key_Escape);
    QCOMPARE(m_doc->findBlockByNumber(26).text(), QStringLiteral("| Linux    | done   |"));
    editor = clickCell(m_view, 1, 0);
    QVERIFY(editor);
    editor->selectAll();
    QTest::keyClicks(editor, QStringLiteral("a|b"));
    QTest::keyClick(editor, Qt::Key_Return);
    QVERIFY(m_doc->findBlockByNumber(26).text().startsWith(QLatin1String("| a\\|b ")));
}

void TestVisualView::toolbarWorksWithACellOpen()
{
    m_view->setReadOnly(false);
    m_view->setMarkdownEditing(true);
    QPlainTextEdit *editor = clickCell(m_view, 1, 0);
    QVERIFY(editor && editor->hasFocus());
    QTest::qWait(5);
    auto *bar = m_view->viewport()->findChild<QWidget *>(QStringLiteral("TableToolbar"));
    QVERIFY(bar && !bar->isHidden());
    QToolButton *below = nullptr;
    for (QToolButton *b : bar->findChildren<QToolButton *>(QStringLiteral("TableToolButton"))) {
        if (b->toolTip().contains(QLatin1String("row below")))
            below = b;
    }
    QVERIFY(below && below->isEnabled());
    // The ordinary pointer over the buttons, not the viewport's text cursor.
    QCOMPARE(below->cursor().shape(), Qt::ArrowCursor);
    QWidget *window = m_view->window();
    const QPoint at = below->mapTo(window, below->rect().center());
    QTest::keyClicks(editor, QStringLiteral(" Mint"));
    const int lines = m_doc->blockCount();
    // A real click: the press must not take the focus from the box (which
    // would close it and hide the toolbar before the release).
    QTest::mouseClick(window->windowHandle(), Qt::LeftButton, {}, at);
    QTest::qWait(5);
    // The text typed went in, then the row below the cell's was added.
    QCOMPARE(m_doc->blockCount(), lines + 1);
    QVERIFY(m_doc->findBlockByNumber(26).text().startsWith(QLatin1String("| Linux Mint |")));
    QCOMPARE(m_doc->findBlockByNumber(27).text().count(u'|'), 3);
    QVERIFY(m_doc->findBlockByNumber(27).text().remove(u'|').trimmed().isEmpty());
}

namespace {

const char *selectionTable = "Intro\n\n"
                             "| Fruit | Qty | Note |\n"
                             "|:------|----:|------|\n"
                             "| pear  | 3   | ripe |\n"
                             "| plum  | 4   | sour |\n"
                             "| fig   | 5   | soft |\n"
                             "| kiwi  | 6   | firm |\n"
                             "\nEnd";

// The cells of the table on line 2, read back from the source: one list of
// texts a row, without the delimiter line.
QList<QStringList> tableCells(QTextDocument *doc)
{
    QList<QStringList> rows;
    for (QTextBlock b = doc->findBlockByNumber(2); b.isValid() && b.text().contains(u'|'); b = b.next()) {
        if (b.blockNumber() == 3)
            continue;
        QStringList cells;
        for (const QString &cell : b.text().split(u'|'))
            cells << cell.trimmed();
        cells.removeFirst();
        cells.removeLast();
        rows << cells;
    }
    return rows;
}

// A menu entry by its text, or null.
QAction *menuAction(QMenu *menu, const QString &text)
{
    for (QAction *action : menu->actions()) {
        if (action->text() == text)
            return action;
    }
    return nullptr;
}

} // namespace

void TestVisualView::useSelectionTable()
{
    m_view->setReadOnly(false);
    m_view->setMarkdownEditing(true);
    m_doc->setPlainText(QString::fromUtf8(selectionTable));
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(0)));
    m_model->flush();
    QTest::qWait(20);
}

void TestVisualView::clickingAColumnEdgeSelectsTheColumn()
{
    useSelectionTable();
    const int expanded = m_view->expandedBlockCount(); // (The block with the caret shows its source.)
    QWidget *viewport = m_view->viewport();
    const QRect header = m_view->tableCellViewRect(2, 0, 1);
    QVERIFY(header.isValid());
    // Just above the header cell: the arrow pointing down, not the text cursor.
    const QPoint above(header.center().x(), header.top() - 6);
    QTest::mouseMove(viewport, above);
    QCOMPARE(viewport->cursor().shape(), Qt::BitmapCursor);
    QVERIFY(!static_cast<ProbeView *>(m_view)->hasTableSelection());
    const QImage plain = m_view->grab().toImage();
    QTest::mouseClick(viewport, Qt::LeftButton, {}, above);
    auto *probe = static_cast<ProbeView *>(m_view);
    QVERIFY(probe->hasTableSelection());
    // No cell box, no source: the table stays rendered; the column is shaded.
    QVERIFY(!cellEditor(viewport));
    QCOMPARE(m_view->expandedBlockCount(), expanded);
    const QImage shaded = m_view->grab().toImage();
    const QRect body = m_view->tableCellViewRect(2, 3, 1);
    QVERIFY(plain.pixel(body.center()) != shaded.pixel(body.center()));
    const QRect other = m_view->tableCellViewRect(2, 3, 0);
    QCOMPARE(plain.pixel(other.center()), shaded.pixel(other.center()));
    if (const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR"); !dir.isEmpty())
        QVERIFY(shaded.save(dir + QStringLiteral("/table-column-selected.png")));
    // Esc drops it.
    QTest::keyClick(viewport, Qt::Key_Escape);
    QVERIFY(!probe->hasTableSelection());
    QCOMPARE(m_view->grab().toImage().pixel(body.center()), plain.pixel(body.center()));
    // A click elsewhere drops it too; Delete clears the column's cells, as one step.
    QTest::mouseClick(viewport, Qt::LeftButton, {}, above);
    QVERIFY(probe->hasTableSelection());
    QTest::mouseClick(viewport, Qt::LeftButton, {}, QPoint(viewport->width() - 20, 10));
    QVERIFY(!probe->hasTableSelection());
    const QString before = m_doc->toPlainText();
    QTest::mouseClick(viewport, Qt::LeftButton, {}, above);
    QTest::keyClick(viewport, Qt::Key_Delete);
    QVERIFY(!probe->hasTableSelection());
    const QList<QStringList> cells = tableCells(m_doc);
    QCOMPARE(cells.size(), 5);
    for (const QStringList &row : cells) {
        QCOMPARE(row.at(1), QString());
        QVERIFY(!row.at(0).isEmpty() && !row.at(2).isEmpty());
    }
    m_doc->undo();
    QCOMPARE(m_doc->toPlainText(), before);
    m_model->flush(); // (The table is shown again.)
    QTest::qWait(20);
    // Shift+click on another column's edge extends the selection to it.
    QTest::mouseClick(viewport, Qt::LeftButton, {}, above);
    const QRect last = m_view->tableCellViewRect(2, 0, 2);
    QTest::mouseClick(viewport, Qt::LeftButton, Qt::ShiftModifier, QPoint(last.center().x(), last.top() - 6));
    QTest::keyClick(viewport, Qt::Key_Delete);
    for (const QStringList &row : tableCells(m_doc)) {
        QVERIFY(!row.at(0).isEmpty());
        QCOMPARE(row.at(1), QString());
        QCOMPARE(row.at(2), QString());
    }
    m_doc->undo();
    QCOMPARE(m_doc->toPlainText(), before);
    m_model->flush(); // (The table is shown again.)
    QTest::qWait(20);
}

void TestVisualView::draggingAlongTheLeftEdgeSelectsRows()
{
    useSelectionTable();
    const int expanded = m_view->expandedBlockCount(); // (The block with the caret shows its source.)
    QWidget *viewport = m_view->viewport();
    auto *probe = static_cast<ProbeView *>(m_view);
    const QString before = m_doc->toPlainText();
    const QRect first = m_view->tableCellViewRect(2, 2, 0);
    const QRect second = m_view->tableCellViewRect(2, 3, 0);
    QVERIFY(first.isValid() && second.isValid());
    const int x = first.left() - 6;
    QTest::mouseMove(viewport, QPoint(x, first.center().y()));
    QCOMPARE(viewport->cursor().shape(), Qt::BitmapCursor);
    QTest::mousePress(viewport, Qt::LeftButton, {}, QPoint(x, first.center().y()));
    QTest::mouseMove(viewport, QPoint(x, second.center().y()));
    QTest::mouseRelease(viewport, Qt::LeftButton, {}, QPoint(x, second.center().y()));
    QVERIFY(probe->hasTableSelection());
    QCOMPARE(m_view->expandedBlockCount(), expanded);

    QMenu menu;
    probe->populateSelectionMenu(&menu);
    QVERIFY(menuAction(&menu, QStringLiteral("Insert 2 Rows Above")));
    QVERIFY(menuAction(&menu, QStringLiteral("Insert 2 Rows Below")));
    // (Every column is in the rows.)
    QVERIFY(menuAction(&menu, QStringLiteral("Insert 3 Columns Left")));
    QVERIFY(!menuAction(&menu, QStringLiteral("Delete 3 Columns"))->isEnabled());
    QAction *remove = menuAction(&menu, QStringLiteral("Delete 2 Rows"));
    QVERIFY(remove && remove->isEnabled());
    remove->trigger();
    QVERIFY(!probe->hasTableSelection());
    const QList<QStringList> cells = tableCells(m_doc);
    QCOMPARE(cells.size(), 3);
    QCOMPARE(cells.at(1).at(0), QStringLiteral("pear"));
    QCOMPARE(cells.at(2).at(0), QStringLiteral("kiwi"));
    // One undo brings both back.
    m_doc->undo();
    QCOMPARE(m_doc->toPlainText(), before);
    m_model->flush(); // (The table is shown again.)
    QTest::qWait(20);

    // Rows inserted above and below, as many as are selected.
    QTest::mousePress(viewport, Qt::LeftButton, {}, QPoint(x, first.center().y()));
    QTest::mouseMove(viewport, QPoint(x, second.center().y()));
    QTest::mouseRelease(viewport, Qt::LeftButton, {}, QPoint(x, second.center().y()));
    QMenu again;
    probe->populateSelectionMenu(&again);
    menuAction(&again, QStringLiteral("Insert 2 Rows Below"))->trigger();
    QList<QStringList> inserted = tableCells(m_doc);
    QCOMPARE(inserted.size(), 7);
    QCOMPARE(inserted.at(3).at(0), QStringLiteral("fig"));
    QCOMPARE(inserted.at(4).at(0), QString());
    QCOMPARE(inserted.at(5).at(0), QString());
    QCOMPARE(inserted.at(6).at(0), QStringLiteral("kiwi"));
    m_doc->undo();
    QCOMPARE(m_doc->toPlainText(), before);
    m_model->flush(); // (The table is shown again.)
    QTest::qWait(20);
}

void TestVisualView::deletingRowsKeepsTheHeader()
{
    useSelectionTable();
    const int expanded = m_view->expandedBlockCount(); // (The block with the caret shows its source.)
    QWidget *viewport = m_view->viewport();
    auto *probe = static_cast<ProbeView *>(m_view);
    const QRect header = m_view->tableCellViewRect(2, 0, 0);
    const QRect bottom = m_view->tableCellViewRect(2, 4, 0);
    const int x = header.left() - 6;
    // The whole table, header to the last row.
    QTest::mousePress(viewport, Qt::LeftButton, {}, QPoint(x, header.center().y()));
    QTest::mouseMove(viewport, QPoint(x, bottom.center().y()));
    QTest::mouseRelease(viewport, Qt::LeftButton, {}, QPoint(x, bottom.center().y()));
    QVERIFY(probe->hasTableSelection());
    QMenu menu;
    probe->populateSelectionMenu(&menu);
    QAction *remove = menuAction(&menu, QStringLiteral("Delete 5 Rows"));
    QVERIFY(remove && remove->isEnabled());
    remove->trigger();
    // The header stays, and one empty row (a table with nothing under its header is none).
    const QList<QStringList> cells = tableCells(m_doc);
    QCOMPARE(cells.size(), 2);
    QCOMPARE(cells.at(0), (QStringList{"Fruit", "Qty", "Note"}));
    QCOMPARE(cells.at(1), (QStringList{"", "", ""}));
    // The header alone cannot be deleted.
    m_model->flush();
    QTest::qWait(20);
    const QRect only = m_view->tableCellViewRect(2, 0, 0);
    QVERIFY(only.isValid());
    QTest::mouseClick(viewport, Qt::LeftButton, {}, QPoint(only.left() - 6, only.center().y()));
    QVERIFY(probe->hasTableSelection());
    QMenu header_only;
    probe->populateSelectionMenu(&header_only);
    QVERIFY(!menuAction(&header_only, QStringLiteral("Delete Row"))->isEnabled());
    QVERIFY(!menuAction(&header_only, QStringLiteral("Insert Row Above"))->isEnabled());
    QVERIFY(menuAction(&header_only, QStringLiteral("Insert Row Below"))->isEnabled());
}

void TestVisualView::draggingFromCellToCellSelectsARectangle()
{
    useSelectionTable();
    const int expanded = m_view->expandedBlockCount(); // (The block with the caret shows its source.)
    QWidget *viewport = m_view->viewport();
    auto *probe = static_cast<ProbeView *>(m_view);
    const QString before = m_doc->toPlainText();
    // A click on a cell still opens its box.
    const QRect a = m_view->tableCellViewRect(2, 1, 1);
    QTest::mouseClick(viewport, Qt::LeftButton, {}, a.center());
    QVERIFY(cellEditor(viewport));
    QTest::keyClick(cellEditor(viewport), Qt::Key_Escape);
    QVERIFY(!probe->hasTableSelection());
    // A drag from (1, 1) to (2, 2) selects four cells.
    const QRect b = m_view->tableCellViewRect(2, 2, 2);
    QTest::mousePress(viewport, Qt::LeftButton, {}, a.center());
    QTest::mouseMove(viewport, a.center() + QPoint(QApplication::startDragDistance() + 3, 0));
    QTest::mouseMove(viewport, b.center());
    QTest::mouseRelease(viewport, Qt::LeftButton, {}, b.center());
    QVERIFY(probe->hasTableSelection());
    QVERIFY(!cellEditor(viewport));
    QCOMPARE(m_view->expandedBlockCount(), expanded);
    QCOMPARE(m_doc->toPlainText(), before);
    QMenu menu;
    probe->populateSelectionMenu(&menu);
    QAction *clear = menuAction(&menu, QStringLiteral("Clear Cells"));
    QVERIFY(clear && clear->isEnabled());
    QVERIFY(menuAction(&menu, QStringLiteral("Insert 2 Columns Right")));
    // Shown over the table.
    const QImage shaded = m_view->grab().toImage();
    if (const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR"); !dir.isEmpty())
        QVERIFY(shaded.save(dir + QStringLiteral("/table-cells-selected.png")));
    clear->trigger();
    const QList<QStringList> cells = tableCells(m_doc);
    QCOMPARE(cells.at(0), (QStringList{"Fruit", "Qty", "Note"}));
    QCOMPARE(cells.at(1), (QStringList{"pear", "", ""}));
    QCOMPARE(cells.at(2), (QStringList{"plum", "", ""}));
    QCOMPARE(cells.at(3), (QStringList{"fig", "5", "soft"}));
    m_doc->undo();
    QCOMPARE(m_doc->toPlainText(), before);
    m_model->flush(); // (The table is shown again.)
    QTest::qWait(20);
    // Backspace clears too, and dragging back out of the table's edge clamps.
    QTest::mousePress(viewport, Qt::LeftButton, {}, b.center());
    QTest::mouseMove(viewport, b.center() + QPoint(QApplication::startDragDistance() + 3, 0));
    QTest::mouseMove(viewport, QPoint(viewport->width() - 4, viewport->height() - 4));
    QTest::mouseRelease(viewport, Qt::LeftButton, {}, QPoint(viewport->width() - 4, viewport->height() - 4));
    QVERIFY(probe->hasTableSelection());
    QTest::keyClick(viewport, Qt::Key_Backspace);
    const QList<QStringList> after = tableCells(m_doc);
    QCOMPARE(after.at(1), (QStringList{"pear", "3", "ripe"}));
    QCOMPARE(after.at(2), (QStringList{"plum", "4", ""}));
    QCOMPARE(after.at(4), (QStringList{"kiwi", "6", ""}));
}

void TestVisualView::quickInsertAddsRowsAndColumns()
{
    useSelectionTable();
    const int expanded = m_view->expandedBlockCount(); // (The block with the caret shows its source.)
    QWidget *viewport = m_view->viewport();
    const QString before = m_doc->toPlainText();
    // Where rows 2 and 3 meet, at the table's left edge: the mark shows.
    const QRect upper = m_view->tableCellViewRect(2, 2, 0);
    const QRect lower = m_view->tableCellViewRect(2, 3, 0);
    const QPoint border(upper.left() - 6, (upper.bottom() + lower.top()) / 2);
    const QImage plain = m_view->grab().toImage();
    QTest::mouseMove(viewport, border);
    QCOMPARE(viewport->cursor().shape(), Qt::PointingHandCursor);
    const QImage marked = m_view->grab().toImage();
    QVERIFY(marked != plain);
    // (A line across the table at the border.)
    QVERIFY(marked.pixel(upper.center().x(), border.y()) != plain.pixel(upper.center().x(), border.y()));
    if (const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR"); !dir.isEmpty())
        QVERIFY(marked.save(dir + QStringLiteral("/table-quick-insert.png")));
    // Away from it, the mark is gone.
    QTest::mouseMove(viewport, QPoint(viewport->width() - 10, 10));
    QCOMPARE(m_view->grab().toImage(), plain);
    QTest::mouseClick(viewport, Qt::LeftButton, {}, border);
    QList<QStringList> rows = tableCells(m_doc);
    QCOMPARE(rows.size(), 6);
    QCOMPARE(rows.at(2).at(0), QStringLiteral("plum"));
    QCOMPARE(rows.at(3).at(0), QString());
    QCOMPARE(rows.at(4).at(0), QStringLiteral("fig"));
    m_doc->undo();
    QCOMPARE(m_doc->toPlainText(), before);
    m_model->flush(); // (The table is shown again.)
    QTest::qWait(20);

    // Where columns 0 and 1 meet, at the top edge.
    const QRect left = m_view->tableCellViewRect(2, 0, 0);
    const QRect right = m_view->tableCellViewRect(2, 0, 1);
    const QPoint top((left.right() + right.left()) / 2, left.top() - 6);
    QTest::mouseMove(viewport, top);
    QCOMPARE(viewport->cursor().shape(), Qt::PointingHandCursor);
    QTest::mouseClick(viewport, Qt::LeftButton, {}, top);
    rows = tableCells(m_doc);
    QCOMPARE(rows.at(0), (QStringList{"Fruit", "", "Qty", "Note"}));
    QCOMPARE(rows.at(1), (QStringList{"pear", "", "3", "ripe"}));
    m_doc->undo();
    QCOMPARE(m_doc->toPlainText(), before);
    m_model->flush(); // (The table is shown again.)
    QTest::qWait(20);
    // Not above the first row: the border under the header is the first one.
    const QRect headerRow = m_view->tableCellViewRect(2, 0, 0);
    QTest::mouseMove(viewport, QPoint(headerRow.left() - 6, headerRow.top() + 3));
    QCOMPARE(viewport->cursor().shape(), Qt::BitmapCursor);
}

void TestVisualView::tableSelectionNeedsAnEditableView()
{
    useSelectionTable();
    const int expanded = m_view->expandedBlockCount(); // (The block with the caret shows its source.)
    QWidget *viewport = m_view->viewport();
    auto *probe = static_cast<ProbeView *>(m_view);
    const QRect header = m_view->tableCellViewRect(2, 0, 1);
    const QRect a = m_view->tableCellViewRect(2, 1, 0);
    const QRect b = m_view->tableCellViewRect(2, 2, 1);
    const QPoint above(header.center().x(), header.top() - 6);
    const QPoint border(a.left() - 6, a.bottom());
    m_view->setReadOnly(true);
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(0)));
    QTest::qWait(20);
    const QImage plain = m_view->grab().toImage();
    QTest::mouseMove(viewport, above);
    QVERIFY(viewport->cursor().shape() != Qt::BitmapCursor);
    QTest::mouseClick(viewport, Qt::LeftButton, {}, above);
    QVERIFY(!probe->hasTableSelection());
    QTest::mouseMove(viewport, border);
    QCOMPARE(m_view->grab().toImage(), plain);
    QTest::mousePress(viewport, Qt::LeftButton, {}, a.center());
    QTest::mouseMove(viewport, a.center() + QPoint(QApplication::startDragDistance() + 3, 0));
    QTest::mouseMove(viewport, b.center());
    QTest::mouseRelease(viewport, Qt::LeftButton, {}, b.center());
    QVERIFY(!probe->hasTableSelection());
    // Not a plain-text view either.
    m_view->setReadOnly(false);
    m_view->setMarkdownEditing(false);
    QTest::mouseClick(viewport, Qt::LeftButton, {}, above);
    QVERIFY(!probe->hasTableSelection());
}

void TestVisualView::selectionMenuAndToolbarActOnTheSelection()
{
    useSelectionTable();
    const int expanded = m_view->expandedBlockCount(); // (The block with the caret shows its source.)
    QWidget *viewport = m_view->viewport();
    auto *probe = static_cast<ProbeView *>(m_view);
    const QString before = m_doc->toPlainText();
    const QRect c0 = m_view->tableCellViewRect(2, 0, 0);
    const QRect c1 = m_view->tableCellViewRect(2, 0, 1);
    QTest::mousePress(viewport, Qt::LeftButton, {}, QPoint(c0.center().x(), c0.top() - 6));
    QTest::mouseMove(viewport, QPoint(c1.center().x(), c1.top() - 6));
    QTest::mouseRelease(viewport, Qt::LeftButton, {}, QPoint(c1.center().x(), c1.top() - 6));
    QVERIFY(probe->hasTableSelection());
    QMenu menu;
    probe->populateSelectionMenu(&menu);
    QVERIFY(menuAction(&menu, QStringLiteral("Insert 2 Columns Left")));
    QVERIFY(menuAction(&menu, QStringLiteral("Insert 2 Columns Right")));
    QAction *remove = menuAction(&menu, QStringLiteral("Delete 2 Columns"));
    QVERIFY(remove && remove->isEnabled());
    QMenu *align = nullptr;
    for (QAction *action : menu.actions()) {
        if (action->menu() && action->text() == QStringLiteral("Align Columns"))
            align = action->menu();
    }
    QVERIFY(align);
    menuAction(align, QStringLiteral("Right"))->trigger();
    QCOMPARE(m_doc->findBlockByNumber(3).text(), QStringLiteral("| ----: | --: | ---- |"));
    m_doc->undo();
    QCOMPARE(m_doc->toPlainText(), before);
    m_model->flush(); // (The table is shown again.)
    QTest::qWait(20);

    // The toolbar's buttons act on the selection: "- Col" deletes both.
    QTest::mousePress(viewport, Qt::LeftButton, {}, QPoint(c0.center().x(), c0.top() - 6));
    QTest::mouseMove(viewport, QPoint(c1.center().x(), c1.top() - 6));
    QTest::mouseRelease(viewport, Qt::LeftButton, {}, QPoint(c1.center().x(), c1.top() - 6));
    QTest::qWait(10);
    auto *bar = viewport->findChild<QWidget *>(QStringLiteral("TableToolbar"));
    QVERIFY(bar && !bar->isHidden());
    QToolButton *column = nullptr;
    for (QToolButton *b : bar->findChildren<QToolButton *>(QStringLiteral("TableToolButton"))) {
        if (b->toolTip() == QLatin1String("Delete the column"))
            column = b;
    }
    QVERIFY(column && column->isEnabled());
    column->click();
    QCOMPARE(tableCells(m_doc).at(0), (QStringList{"Note"}));
    m_doc->undo();
    QCOMPARE(m_doc->toPlainText(), before);
    m_model->flush(); // (The table is shown again.)
    QTest::qWait(20);
}

void TestVisualView::htmlTableCellsAreEditedInPlace()
{
    m_view->setReadOnly(false);
    m_view->setMarkdownEditing(true);
    m_doc->setPlainText(QStringLiteral("Intro\n\n<div align=\"center\">\n<table>\n"
                                       "  <tr><th>Name</th><th>Qty</th></tr>\n"
                                       "  <tr><td>pear</td><td>3</td></tr>\n"
                                       "</table>\n</div>\n\nEnd"));
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(0)));
    m_model->flush();
    QTest::qWait(20);
    // (The <div> centers the table.) Click its first cell, the header.
    const QRect first = m_view->tableCellViewRect(2, 0, 0);
    QVERIFY(first.isValid());
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, first.center());
    QPlainTextEdit *editor = cellEditor(m_view->viewport());
    QVERIFY(editor);
    QCOMPARE(editor->toPlainText(), QStringLiteral("Name"));
    // The block stays rendered, and the toolbar shows for it.
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QTest::qWait(5);
    auto *bar = m_view->viewport()->findChild<QWidget *>(QStringLiteral("TableToolbar"));
    QVERIFY(bar && !bar->isHidden());
    // Down: the cell below, then a new text written back as it is.
    QTest::keyClick(editor, Qt::Key_Down);
    editor = cellEditor(m_view->viewport());
    QVERIFY(editor);
    QCOMPARE(editor->toPlainText(), QStringLiteral("pear"));
    editor->selectAll();
    QTest::keyClicks(editor, QStringLiteral("<b>ripe</b> pear"));
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(m_doc->findBlockByNumber(5).text(), QStringLiteral("  <tr><td><b>ripe</b> pear</td><td>3</td></tr>"));
    // The rest of the source is as it was.
    QCOMPARE(m_doc->findBlockByNumber(2).text(), QStringLiteral("<div align=\"center\">"));
    QCOMPARE(m_doc->findBlockByNumber(4).text(), QStringLiteral("  <tr><th>Name</th><th>Qty</th></tr>"));
}

void TestVisualView::mergedHtmlCellsOpenTheCellClicked()
{
    m_view->setReadOnly(false);
    m_view->setMarkdownEditing(true);
    m_doc->setPlainText(QStringLiteral("Intro\n\n<table>\n<tr><th colspan=\"2\">head</th></tr>\n"
                                       "<tr><td>left</td><td>right</td></tr>\n</table>\n\nEnd"));
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(0)));
    m_model->flush();
    QTest::qWait(20);
    // The header spans both columns: each body cell still opens its own box.
    for (const auto &[col, text] : {std::pair{0, QStringLiteral("left")}, std::pair{1, QStringLiteral("right")}}) {
        const QRect cell = m_view->tableCellViewRect(2, 1, col);
        QVERIFY(cell.isValid());
        QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, cell.center());
        QPlainTextEdit *editor = cellEditor(m_view->viewport());
        QVERIFY(editor);
        QCOMPARE(editor->toPlainText(), text);
        QTest::keyClick(editor, Qt::Key_Escape);
    }
}

void TestVisualView::insertedTableOpensItsFirstCell()
{
    m_view->setReadOnly(false);
    m_view->setMarkdownEditing(true);
    m_doc->setPlainText(QString());
    m_model->flush();
    m_view->insertTable(3, 2);
    // A table, a blank line and a line to type on after it.
    QCOMPARE(m_doc->blockCount(), 6);
    QVERIFY(m_doc->findBlockByNumber(0).text().startsWith(QLatin1String("| Column 1 |")));
    QCOMPARE(m_doc->findBlockByNumber(4).text(), QString());
    QCOMPARE(m_view->textCursor().blockNumber(), 5);
    // Shown as a table, with the first header's box open, its text selected.
    QCOMPARE(m_view->expandedBlockCount(), 0);
    QPlainTextEdit *editor = cellEditor(m_view->viewport());
    QVERIFY(editor && editor->hasFocus());
    QCOMPARE(editor->textCursor().selectedText(), QStringLiteral("Column 1"));
    QTest::keyClicks(editor, QStringLiteral("Fruit"));
    QTest::keyClick(editor, Qt::Key_Tab);
    editor = cellEditor(m_view->viewport());
    QVERIFY(editor);
    QCOMPARE(editor->toPlainText(), QStringLiteral("Column 2"));
    QTest::keyClick(editor, Qt::Key_Escape);
    QVERIFY(m_doc->findBlockByNumber(0).text().startsWith(QLatin1String("| Fruit ")));
    // One undo takes the cell back, another the table.
    m_doc->undo();
    m_doc->undo();
    QCOMPARE(m_doc->toPlainText(), QString());
}

void TestVisualView::cellEditorKeysMoveBetweenCells()
{
    m_view->setReadOnly(false);
    m_view->setMarkdownEditing(true);
    QPlainTextEdit *editor = clickCell(m_view, 1, 0);
    QVERIFY(editor);
    QTest::keyClick(editor, Qt::Key_Tab);
    editor = cellEditor(m_view->viewport());
    QVERIFY(editor && !editor->isHidden());
    QCOMPARE(editor->toPlainText(), QStringLiteral("done"));
    QTest::keyClick(editor, Qt::Key_Down);
    editor = cellEditor(m_view->viewport());
    QCOMPARE(editor->toPlainText(), QStringLiteral("next"));
    // Tab in the last cell adds a row and goes to its first cell.
    const int lines = m_doc->blockCount();
    QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(m_doc->blockCount(), lines + 1);
    editor = cellEditor(m_view->viewport());
    QVERIFY(editor && !editor->isHidden());
    QCOMPARE(editor->toPlainText(), QString());
    QTest::keyClicks(editor, QStringLiteral("BSD"));
    QTest::keyClick(editor, Qt::Key_Backtab);
    QVERIFY(m_doc->findBlockByNumber(28).text().startsWith(QLatin1String("| BSD ")));
    editor = cellEditor(m_view->viewport());
    QCOMPARE(editor->toPlainText(), QStringLiteral("next"));
    QTest::keyClick(editor, Qt::Key_Escape);
}

void TestVisualView::cellEditorOnlyInEditableViews()
{
    m_view->setMarkdownEditing(true);
    m_view->setReadOnly(true);
    QVERIFY(!clickCell(m_view, 1, 0));
    // A table that is not a pipe table (front matter) has no cell editor either.
    m_view->setReadOnly(false);
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(40, 30));
    QVERIFY(!cellEditor(m_view->viewport()));
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

void TestVisualView::selectingInALongTableKeepsTheScroll_data()
{
    QTest::addColumn<bool>("locked");
    QTest::newRow("locked preview") << true;
    QTest::newRow("editable preview") << false;
}

void TestVisualView::selectingInALongTableKeepsTheScroll()
{
    QFETCH(bool, locked);
    // A table far taller than the window, its top and bottom off screen.
    QString text = QStringLiteral("Intro paragraph.\n\n| Row | Value |\n|-----|-------|\n");
    for (int i = 1; i <= 300; ++i)
        text += QStringLiteral("| r%1 | value %1 |\n").arg(i);
    text += QStringLiteral("\nClosing paragraph.\n");
    QTemporaryDir dir;
    QFile file(dir.filePath(QStringLiteral("table.md")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(text.toUtf8());
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
    visual->setReadOnly(locked);
    source->setReadOnly(true);
    visual->setFocus();
    QTRY_VERIFY(visual->hasFocus());

    // Show the table's last rows, as a reader scrolled to them would.
    visual->scrollToLinePosition(290);
    source->followScroll(*visual);
    const qreal before = visual->topLinePosition();
    const qreal sourceBefore = source->topLinePosition();
    QVERIFY2(before > 280, qPrintable(QString::number(before)));

    QTest::mousePress(visual->viewport(), Qt::LeftButton, {}, QPoint(100, 100));
    QTest::mouseMove(visual->viewport(), QPoint(300, 160));
    // (Editable, a drag from a cell selects cells, not text.)
    QVERIFY(visual->textCursor().hasSelection() || !locked);
    QVERIFY(visual->textCursor().blockNumber() > 285 || !locked);
    QTest::mouseRelease(visual->viewport(), Qt::LeftButton, {}, QPoint(300, 160));
    QVERIFY(visual->textCursor().hasSelection() || !locked);
    QVERIFY2(qAbs(visual->topLinePosition() - before) < 3, qPrintable(QString::number(visual->topLinePosition())));
    QVERIFY2(qAbs(source->topLinePosition() - sourceBefore) < 3, qPrintable(QString::number(source->topLinePosition())));

    // The same from the source side: its selection is mirrored into the
    // preview, which must not go back to the table's top to show it.
    source->setFocus();
    QTRY_VERIFY(source->hasFocus());
    QTextDocument *doc = source->document();
    QTextCursor c(doc);
    c.setPosition(doc->findBlockByNumber(293).position() + 2);
    c.setPosition(doc->findBlockByNumber(296).position() + 4, QTextCursor::KeepAnchor);
    source->setTextCursor(c);
    QVERIFY(visual->textCursor().hasSelection());
    QVERIFY2(qAbs(visual->topLinePosition() - before) < 12, qPrintable(QString::number(visual->topLinePosition())));
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
    m_view->openBlockAtCaret();
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

void TestVisualView::oversizedDiagramsRenderOnce()
{
    // A diagram that cannot be drawn or cached ends as an error after one
    // render, instead of being rendered again and again.
    core::Block diagram = m_model->blocks()[11];
    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    const QString source = m_model->sourceText(int(diagram.first_line), int(diagram.last_line));

    // Rejected by the renderer: a source far over the length limit.
    ctx.devicePixelRatio = 1.25;
    const QString huge = QStringLiteral("flowchart LR\nA[\"%1\"] --> B\n").arg(QString(200000, u'x'));
    diagram.text = rust::String(huge.toStdString());
    const int before = DiagramNotifier::renderCount();
    QVERIFY(RenderedBlock::create(diagram, source, ctx)->isPending());
    QTRY_VERIFY(!RenderedBlock::create(diagram, source, ctx)->isPending());
    auto done = RenderedBlock::create(diagram, source, ctx);
    QCOMPARE(done->kind(), RenderedBlock::Kind::Code);
    QVERIFY(done->renderedText().contains(QStringLiteral("Diagram error: diagram too large")));
    // Stable: no further renders however long we wait and ask.
    QTest::qWait(300);
    QVERIFY(!RenderedBlock::create(diagram, source, ctx)->isPending());
    QCOMPARE(DiagramNotifier::renderCount(), before + 1);

    // Rendered fine but bigger than the cache can hold.
    diagram = m_model->blocks()[11];
    ctx.devicePixelRatio = 1.75;
    DiagramNotifier::setCacheLimitKiB(1);
    const int before2 = DiagramNotifier::renderCount();
    QVERIFY(RenderedBlock::create(diagram, source, ctx)->isPending());
    QTRY_VERIFY(!RenderedBlock::create(diagram, source, ctx)->isPending());
    done = RenderedBlock::create(diagram, source, ctx);
    QCOMPARE(done->kind(), RenderedBlock::Kind::Code);
    QVERIFY(done->renderedText().contains(QStringLiteral("Diagram error: diagram too large")));
    QTest::qWait(300);
    QVERIFY(!RenderedBlock::create(diagram, source, ctx)->isPending());
    QCOMPARE(DiagramNotifier::renderCount(), before2 + 1);
    DiagramNotifier::setCacheLimitKiB(64 * 1024);
}

void TestVisualView::onlyParseSettingsReparse()
{
    m_model->blocks();
    const int generation = m_model->parseGeneration();
    QSignalSpy spy(m_model, &MarkdownModel::optionsChanged);
    Settings &settings = Settings::instance();
    const QVariant lineHeight = settings.value(key::textLineHeight);
    settings.setValue(key::textLineHeight, lineHeight.toDouble() + 0.1);
    m_view->grab();
    QCOMPARE(m_model->parseGeneration(), generation); // ...without a parse.
    settings.setValue(key::textLineHeight, lineHeight);
    const bool breaks = settings.flag(key::breaks);
    settings.setValue(key::breaks, !breaks);
    m_view->grab();
    QCOMPARE(m_model->parseGeneration(), generation + 1);
    settings.setValue(key::breaks, breaks);
}

void TestVisualView::largeDocumentsParseInBackground()
{
    m_view->hide();
    QTextDocument doc;
    doc.setDocumentLayout(new DocumentLayout(&doc));
    doc.setPlainText(largeDocument());
    auto *model = new MarkdownModel(&doc);
    ProbeView view(model);
    view.resize(600, 500);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QVERIFY(matchesFreshParse(*model));
    const int generation = model->parseGeneration();
    // Connected after the view: sees what the view's request started.
    bool background = false;
    connect(model, &MarkdownModel::reparseDue, this, [&] { background = model->isParsingInBackground(); });
    int checked = 0;
    connect(model, &MarkdownModel::reparsed, this, [&] {
        QVERIFY(matchesFreshParse(*model));
        ++checked;
    });
    QTextCursor c(doc.findBlockByNumber(2));
    c.insertText(QStringLiteral("Typed "));
    QVERIFY(view.rowsConsistent());
    QVERIFY(view.lineIsRendered(2)); // The old render stands in meanwhile.
    QTRY_VERIFY(!model->hasPendingEdits());
    QVERIFY(background);
    QCOMPARE(checked, 1);
    QCOMPARE(model->parseGeneration(), generation + 1);
    // Only the blocks around the edit were parsed again.
    QVERIFY(model->lastParseSpliced());
    QVERIFY(view.rowsConsistent());
    QVERIFY(QString::fromUtf8(model->blocks()[1].html.data(), model->blocks()[1].html.size())
                .startsWith(QStringLiteral("<p>Typed A paragraph")));
}

void TestVisualView::editsDuringBackgroundParseAreKept()
{
    m_view->hide();
    QTextDocument doc;
    doc.setDocumentLayout(new DocumentLayout(&doc));
    doc.setPlainText(largeDocument());
    auto *model = new MarkdownModel(&doc);
    ProbeView view(model);
    view.resize(600, 500);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    model->blocks();
    const int generation = model->parseGeneration();
    // Every parse that is applied matches the document as it is then.
    int applied = 0;
    connect(model, &MarkdownModel::reparsed, this, [&] {
        QVERIFY(matchesFreshParse(*model));
        ++applied;
    });
    QTextCursor c(doc.findBlockByNumber(2));
    c.insertText(QStringLiteral("Edited "));
    model->flushInBackground();
    QVERIFY(model->isParsingInBackground());
    // Edits while the worker parses the older text: a heading near the top
    // that moves every block below it, and text deleted further down.
    c.setPosition(doc.findBlockByNumber(4).position());
    c.insertText(QStringLiteral("# Inserted\n\n"));
    QVERIFY(view.rowsConsistent());
    c.setPosition(doc.findBlockByNumber(20).position());
    c.setPosition(doc.findBlockByNumber(31).position(), QTextCursor::KeepAnchor);
    c.removeSelectedText();
    QVERIFY(view.rowsConsistent());
    QVERIFY(model->hasPendingEdits());
    // The stale result is dropped; the pause after the last edit parses again.
    QTRY_VERIFY(!model->hasPendingEdits());
    QTRY_VERIFY(!model->isParsingInBackground());
    QCOMPARE(applied, 1);
    QCOMPARE(model->parseGeneration(), generation + 1);
    QVERIFY(matchesFreshParse(*model));
    QVERIFY(view.rowsConsistent());
    const core::Block &heading = model->blocks()[model->blockAt(4)];
    QVERIFY(QString::fromUtf8(heading.html.data(), heading.html.size()).contains(QStringLiteral("Inserted</h1>")));
}

void TestVisualView::flushOvertakesBackgroundParse()
{
    m_view->hide();
    QTextDocument doc;
    doc.setDocumentLayout(new DocumentLayout(&doc));
    doc.setPlainText(largeDocument());
    MarkdownModel *model = new MarkdownModel(&doc);
    model->blocks();
    const int generation = model->parseGeneration();
    QTextCursor c(doc.findBlockByNumber(0));
    c.insertText(QStringLiteral("# Top\n\n"));
    model->flushInBackground();
    QVERIFY(model->isParsingInBackground());
    c.setPosition(2);
    c.insertText(QStringLiteral("More "));
    // A click (say on a task box) wants the current blocks now: flush() parses
    // the document as it is, without waiting for the worker.
    model->flush();
    QCOMPARE(model->parseGeneration(), generation + 1);
    QVERIFY(matchesFreshParse(*model));
    // The worker's result, of older text, changes nothing.
    QTRY_VERIFY(!model->isParsingInBackground());
    QTest::qWait(MarkdownModel::reparseDelay() + 50);
    QCOMPARE(model->parseGeneration(), generation + 1);
    QVERIFY(matchesFreshParse(*model));
    QCOMPARE(QString::fromUtf8(model->blocks()[0].html.data(), model->blocks()[0].html.size()),
             QStringLiteral("<h1><a name=\"more-top\"></a>More Top</h1>"));
}

void TestVisualView::largeDocumentsReparseIncrementally()
{
    // Edits of every kind through the document, each followed by a flush:
    // the blocks always equal a fresh parse, and most parses only parse the
    // blocks around the edit (core's tests cover the splicing in depth).
    m_view->hide();
    QTextDocument doc;
    doc.setDocumentLayout(new DocumentLayout(&doc));
    doc.setPlainText(largeDocument());
    MarkdownModel *model = new MarkdownModel(&doc);
    model->blocks();
    const QStringList inserts = {QStringLiteral("x"), QStringLiteral("\n"), QStringLiteral("\n\n"),
                                 QStringLiteral("# Heading\n\n"), QStringLiteral("- [ ] new task\n"),
                                 QStringLiteral("```\n"), QStringLiteral("é中"), QStringLiteral("> quote\n"),
                                 QStringLiteral("| a |\n|---|\n"), QStringLiteral("<div>\n")};
    quint32 seed = 12345;
    const auto random = [&seed](int n) {
        seed = seed * 1103515245 + 12345;
        return int((seed >> 8) % quint32(n));
    };
    int spliced = 0;
    for (int i = 0; i < 60; ++i) {
        QTextCursor c(&doc);
        c.setPosition(random(doc.characterCount() - 1));
        if (i % 3 == 2) {
            c.setPosition(qMin(c.position() + 1 + random(40), doc.characterCount() - 1), QTextCursor::KeepAnchor);
            c.removeSelectedText();
        } else {
            c.insertText(inserts[random(int(inserts.size()))]);
        }
        model->flush();
        QVERIFY2(matchesFreshParse(*model), qPrintable(QStringLiteral("edit %1").arg(i)));
        spliced += model->lastParseSpliced();
    }
    QVERIFY2(spliced > 30, qPrintable(QString::number(spliced)));
}

void TestVisualView::modelDeletedDuringBackgroundParse()
{
    // Closing a document while a worker parses it: the worker holds copies
    // only, and its result goes nowhere.
    for (int round = 0; round < 3; ++round) {
        auto *doc = new QTextDocument;
        doc->setDocumentLayout(new DocumentLayout(doc));
        doc->setPlainText(largeDocument());
        auto *model = new MarkdownModel(doc);
        model->blocks();
        QTextCursor(doc).insertText(QStringLiteral("x"));
        model->flushInBackground();
        QVERIFY(model->isParsingInBackground());
        if (round == 1)
            QTest::qWait(1); // Let the worker get going.
        delete doc;
    }
    QThreadPool::globalInstance()->waitForDone();
    QTest::qWait(20);
}

void TestVisualView::modeSwitchDuringBackgroundParse()
{
    m_view->hide();
    QTemporaryDir dir;
    QFile file(dir.filePath(QStringLiteral("large.md")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(largeDocument().toUtf8());
    file.close();
    DocumentTab tab;
    QVERIFY(tab.load(file.fileName()));
    tab.setViewMode(DocumentTab::ViewMode::SourceVisual);
    tab.resize(1200, 500);
    tab.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tab));
    MarkdownModel *model = tab.file()->markdownModel();
    QTextDocument *doc = tab.sourceView()->document();
    model->blocks();
    QTextCursor c(doc->findBlockByNumber(2));
    c.insertText(QStringLiteral("Split "));
    QTRY_VERIFY(model->isParsingInBackground() || !model->hasPendingEdits());
    // To Source while it runs, and an edit there: the result is stale.
    tab.setViewMode(DocumentTab::ViewMode::Source);
    c.insertText(QStringLiteral("Source "));
    QTRY_VERIFY(!model->isParsingInBackground());
    QTest::qWait(MarkdownModel::reparseDelay() + 50);
    // Source mode doesn't parse; showing the visual view does, at once.
    QVERIFY(model->hasPendingEdits());
    tab.setViewMode(DocumentTab::ViewMode::Visual);
    QVERIFY(!model->hasPendingEdits());
    QVERIFY(matchesFreshParse(*model));
    QVERIFY(QString::fromUtf8(model->blocks()[1].html.data(), model->blocks()[1].html.size())
                .startsWith(QStringLiteral("<p>Split Source A paragraph")));
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

// An <img> block of the given HTML in a folder, painted at a width on white.
static QImage paintHtml(const QString &html, const QString &folder, qreal width, qreal dpr = 1)
{
    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    ctx.width = width;
    ctx.devicePixelRatio = dpr;
    ctx.baseDirectory = folder;
    core::Block block;
    block.kind = core::BlockKind::Html;
    const QByteArray utf8 = html.toUtf8();
    block.html = rust::String(utf8.constData(), utf8.size());
    const auto rendered = RenderedBlock::create(block, html, ctx);
    QImage image(QSize(int(width * dpr), qMax(1, int(rendered->height() * dpr))), QImage::Format_RGB32);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::white);
    QPainter painter(&image);
    rendered->paint(painter, QPointF(0, 0), width);
    return image;
}

void TestVisualView::pictureCacheSettingResizesTheCache()
{
    Settings &settings = Settings::instance();
    settings.reset(key::pictureCacheMiB);
    MainWindow window; // Binds the setting to the cache.
    // 26 MB decoded, about 20 MiB kept: fits 128 MiB, not 16 MiB.
    QTemporaryDir dir;
    QImage picture(3000, 2200, QImage::Format_RGB32);
    for (int y = 0; y < picture.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(picture.scanLine(y));
        for (int x = 0; x < picture.width(); ++x)
            line[x] = qRgb(x % 256, y % 256, (x ^ y) % 256);
    }
    QVERIFY(picture.save(dir.filePath(QStringLiteral("big.png"))));
    const QString html = QStringLiteral("<p><img src=\"big.png\"></p>");

    int start = RenderedBlock::pictureFileReads();
    paintHtml(html, dir.path(), 400);
    paintHtml(html, dir.path(), 500);
    QCOMPARE(RenderedBlock::pictureFileReads(), start + 1); // Kept at the default size.

    settings.setValue(key::pictureCacheMiB, 16); // Shrinking forgets it, and it no longer fits.
    start = RenderedBlock::pictureFileReads();
    paintHtml(html, dir.path(), 600);
    paintHtml(html, dir.path(), 700);
    QCOMPARE(RenderedBlock::pictureFileReads(), start + 2);

    settings.setValue(key::pictureCacheMiB, 64); // Growing makes room again.
    start = RenderedBlock::pictureFileReads();
    paintHtml(html, dir.path(), 800);
    paintHtml(html, dir.path(), 900);
    QCOMPARE(RenderedBlock::pictureFileReads(), start + 1);
    settings.reset(key::pictureCacheMiB);
}

void TestVisualView::decodedPicturesServeEveryWidth()
{
    QTemporaryDir dir;
    // Not a flat colour, so scaling it shows in every pixel.
    QImage picture(1600, 400, QImage::Format_RGB32);
    for (int y = 0; y < picture.height(); ++y)
        for (int x = 0; x < picture.width(); ++x)
            picture.setPixel(x, y, qRgb(x % 256, (x / 7 + y) % 256, (x * y) % 256));
    const QString png = dir.filePath(QStringLiteral("a.png"));
    const QString copy = dir.filePath(QStringLiteral("b.png"));
    QVERIFY(picture.save(png));
    QVERIFY(QFile::copy(png, copy));
    QFile svg(dir.filePath(QStringLiteral("v.svg")));
    QVERIFY(svg.open(QIODevice::WriteOnly));
    svg.write(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="900" height="300"><defs>)svg"
              R"svg(<linearGradient id="g"><stop offset="0" stop-color="#f00"/><stop offset="1" stop-color="#00f"/></linearGradient></defs>)svg"
              R"svg(<circle cx="450" cy="150" r="140" fill="url(#g)"/></svg>)svg");
    svg.close();
    const QString a = QStringLiteral("<p><img src=\"a.png\"></p>");
    const QString b = QStringLiteral("<p><img src=\"b.png\"></p>");
    const QString v = QStringLiteral("<p><img src=\"v.svg\"></p>");

    // A new width scales the picture read before: no second read.
    const int start = RenderedBlock::pictureFileReads();
    const QImage first = paintHtml(a, dir.path(), 400);
    QCOMPARE(RenderedBlock::pictureFileReads(), start + 1);
    const QImage second = paintHtml(a, dir.path(), 500);
    paintHtml(a, dir.path(), 640, 1.5);
    QCOMPARE(RenderedBlock::pictureFileReads(), start + 1);
    QVERIFY(first != second);
    // Same pixels as a picture read afresh at that width (a copy of the file
    // is another file, so it is read and then scaled by itself).
    QCOMPARE(paintHtml(b, dir.path(), 500), second);
    QCOMPARE(RenderedBlock::pictureFileReads(), start + 2);
    QCOMPARE(paintHtml(b, dir.path(), 400), first);
    QCOMPARE(paintHtml(b, dir.path(), 700), paintHtml(a, dir.path(), 700));
    QCOMPARE(RenderedBlock::pictureFileReads(), start + 2);
    // The scaled picture is the decoded one scaled, as always.
    const QImage expected = QImage(png).scaledToWidth(500, Qt::SmoothTransformation);
    QVERIFY(second.height() >= expected.height());
    QCOMPARE(second.copy(0, 0, expected.width(), expected.height()).convertToFormat(QImage::Format_RGB32),
             expected.convertToFormat(QImage::Format_RGB32));

    // A changed file is read again.
    QImage red(1600, 400, QImage::Format_RGB32);
    red.fill(Qt::red);
    QVERIFY(red.save(png));
    QFile file(png);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.setFileTime(QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime));
    file.close();
    const int before = RenderedBlock::pictureFileReads();
    QVERIFY(countColor(paintHtml(a, dir.path(), 500), Qt::red) > 1000);
    QCOMPARE(RenderedBlock::pictureFileReads(), before + 1);

    // A vector picture is read once as well, and still drawn at every width.
    const int svgStart = RenderedBlock::pictureFileReads();
    const QImage small = paintHtml(v, dir.path(), 300);
    const QImage large = paintHtml(v, dir.path(), 600);
    QCOMPARE(RenderedBlock::pictureFileReads(), svgStart + 1);
    QVERIFY(countColor(small, Qt::white) < small.width() * small.height());
    QVERIFY(small != large);
    QVERIFY(large.height() > small.height());
}

void TestVisualView::codeBlocksArePaintedAsTheStockHighlighterDoes_data()
{
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("code");
    QTest::newRow("python") << "python"
                            << "def f(x):\n    # a comment\n    return [x * k for k in range(10) if k % 2 == 0]\n";
    QTest::newRow("cpp") << "cpp"
                         << "#include <vector>\nint main() {\n  std::vector<int> v{1, 2}; // two\n  return v[0] > 1 ? 0 : 1;\n}\n";
    QTest::newRow("json") << "json" << "{\"a\": [1, 2.5, true, null], \"b\": {\"c\": \"text\"}}\n";
    QTest::newRow("rust") << "rust" << "fn main() {\n    let s = \"hi\"; /* c */\n    println!(\"{}\", s);\n}\n";
    QTest::newRow("bash") << "bash" << "for f in *.md; do echo \"$f\" | sed 's/a/b/'; done\n";
    QTest::newRow("markdown") << "markdown" << "# Title\n\n*emphasis* **strong** `code` [link](http://x.y)\n- item\n";
    QTest::newRow("wrapping") << "text" << QString(300, u'w') + QStringLiteral(" and some more words to wrap\n");
    QTest::newRow("unknown language") << "no-such-language" << "plain text\n";
    QTest::newRow("none") << "" << "no language at all\n";
}

void TestVisualView::codeBlocksArePaintedAsTheStockHighlighterDoes()
{
    QFETCH(QString, language);
    QFETCH(QString, code);
    const QString source = QStringLiteral("```%1\n%2```").arg(language, code);
    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    ctx.width = 360;
    core::Block block;
    block.kind = core::BlockKind::Code;
    const QByteArray lang = language.toUtf8();
    const QByteArray text = code.toUtf8();
    block.lang = rust::String(lang.constData(), lang.size());
    block.text = rust::String(text.constData(), text.size());
    const auto rendered = RenderedBlock::create(block, source, ctx);
    QCOMPARE(rendered->kind(), RenderedBlock::Kind::Code);

    // The same document the way the stock KSyntaxHighlighting highlighter
    // does it, drawn the way RenderedBlock::paint does.
    QTextDocument reference;
    reference.setDocumentMargin(0);
    reference.setDefaultFont(ctx.codeFont);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    reference.setDefaultTextOption(option);
    reference.setPlainText(source);
    reference.setTextWidth(ctx.width - 20);
    KSyntaxHighlighting::SyntaxHighlighter highlighter(&reference);
    highlighter.setTheme(theme::syntaxTheme());
    highlighter.setDefinition(theme::syntaxRepository().definitionForName(QStringLiteral("Markdown")));
    highlighter.rehighlight();

    const QSize size(int(ctx.width), int(rendered->height()));
    QImage actual(size, QImage::Format_RGB32);
    QImage expected(size, QImage::Format_RGB32);
    actual.fill(Qt::white);
    expected.fill(Qt::white);
    {
        QPainter painter(&actual);
        rendered->paint(painter, QPointF(0, 0), ctx.width);
    }
    {
        QPainter painter(&expected);
        QPainterPath box;
        box.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(ctx.width, rendered->height() - 4)), 3, 3);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(box, theme::color(theme::Role::codeBlock));
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.translate(10, 5);
        QAbstractTextDocumentLayout::PaintContext paint;
        paint.palette.setColor(QPalette::Text, theme::color(theme::Role::text));
        reference.documentLayout()->draw(&painter, paint);
    }
    QCOMPARE(reference.size().height() + 10 + 4, rendered->height());
    QVERIFY(actual == expected);
    // Highlighted, not plain: more than the text colour and the box.
    QSet<QRgb> colors;
    for (int y = 0; y < actual.height(); ++y)
        for (int x = 0; x < actual.width(); ++x)
            colors.insert(actual.pixel(x, y));
    if (language == QLatin1String("python") || language == QLatin1String("cpp"))
        QVERIFY(colors.size() > 20);
}

void TestVisualView::renderCostCountsPicturesAndText()
{
    QTemporaryDir dir;
    QImage picture(1000, 500, QImage::Format_RGB32); // 2000 KiB.
    picture.fill(Qt::blue);
    QVERIFY(picture.save(dir.filePath(QStringLiteral("p.png"))));
    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    ctx.width = 1200; // Wide enough to keep the picture at full size.
    ctx.baseDirectory = dir.path();
    const auto cost = [&](const QString &html) {
        core::Block block;
        block.kind = core::BlockKind::Html;
        const QByteArray utf8 = html.toUtf8();
        block.html = rust::String(utf8.constData(), utf8.size());
        return RenderedBlock::create(block, html, ctx)->cost();
    };
    const int text = cost(QStringLiteral("<p>some words</p>"));
    // A paragraph takes tens of KiB, not bytes.
    QVERIFY2(text >= 30, qPrintable(QString::number(text)));
    const int one = cost(QStringLiteral("<p>some words <img src=\"p.png\"></p>"));
    QVERIFY2(one - text >= 1900 && one - text < 2200, qPrintable(QString::number(one - text)));
    // A narrower column shrinks the picture, and its cost.
    ctx.width = 250;
    const int narrow = cost(QStringLiteral("<p>some words <img src=\"p.png\"></p>"));
    QVERIFY2(narrow - text < 500, qPrintable(QString::number(narrow - text)));
    QVERIFY(narrow < one);
    // Text counts by its length, in code and prose.
    QVERIFY(cost(QStringLiteral("<p>%1</p>").arg(QString(16000, u'x'))) - text > 800);
    // A highlighted code block is no smaller than its text.
    core::Block code;
    code.kind = core::BlockKind::Code;
    const QString body(QString(4000, u'y'));
    const QString fenced = QStringLiteral("```\n%1\n```").arg(body);
    const QByteArray utf8 = body.toUtf8();
    code.text = rust::String(utf8.constData(), utf8.size());
    QVERIFY(RenderedBlock::create(code, fenced, ctx)->cost() > 400);
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

    // Every reading of a name: as written, decoded once or twice, file: URLs.
    for (const char *name :
         {"//host/s/a:b.png", "\\\\host\\share\\a:b.png", "\\\\host\\s\\x.png:z", "\\\\host@SSL\\s\\a:b.png",
          "%5C%5Chost%5Cs%5Ca:b.png", "%2F%2Fhost%2Fs%2Fa.png", "%255C%255Chost%255Cs%255Ca.png",
          "%252F%252Fhost/a.png", "file://host/s/a.png", "file:////host/s/a.png", "FILE:\\\\host\\s\\a.png",
          "file:/\\host\\s\\a.png", "file:///\\\\host\\s\\a.png", "file:%5C%5Chost%5Ca.png", " \t\\\\host\\x.png"})
        QVERIFY2(md::mayNameOtherMachine(QString::fromUtf8(name)), name);
    for (const char *name : {"x.png", "img/a:b.png", "/home/user/x.png", "C:/Users/x.png", "C:\\Users\\x.png",
                             "file:///home/user/x.png", "file:/home/user/x.png", "file:///C:/x.png",
                             "https://example.com//x.png", "data:image/png;base64,AAAA", "vn-math:0", "%41.png", ""})
        QVERIFY2(!md::mayNameOtherMachine(QString::fromUtf8(name)), name);
}

void TestVisualView::plainToolTipText()
{
    const QString img = QStringLiteral("<img src=\"//h/s/x.png\">");
    QCOMPARE(plainToolTip(img), QStringLiteral("<p style='white-space:pre'>&lt;img src=&quot;//h/s/x.png&quot;&gt;</p>"));
    QCOMPARE(plainToolTip(QStringLiteral("<b>bold</b> & co")),
             QStringLiteral("<p style='white-space:pre'>&lt;b&gt;bold&lt;/b&gt; &amp; co</p>"));
    // Shown as written: what the tooltip displays is the text itself.
    for (const QString &text : {img, QStringLiteral("a  <b>b</b>\n<i>c</i>"), QStringLiteral("mailto:a<img src=//h/s/x.png>")}) {
        QTextDocument shown;
        shown.setHtml(plainToolTip(text));
        QCOMPARE(shown.toPlainText(), text);
        for (QTextBlock block = shown.begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it)
                QVERIFY(!it.fragment().charFormat().isImageFormat() && !it.fragment().charFormat().isAnchor());
    }
    // Capped, the cut marked; surrogate pairs stay whole.
    const QString longText(2000, u'x');
    QTextDocument capped;
    capped.setHtml(plainToolTip(longText));
    QCOMPARE(capped.toPlainText(), QString(512, u'x') + QChar(0x2026));
    QCOMPARE(plainToolTip(QStringLiteral("short"), 3), QStringLiteral("<p style='white-space:pre'>sho\u2026</p>"));
    const QString emoji = QString(511, u'x') + QStringLiteral("😀😀");
    QVERIFY(plainToolTip(emoji).contains(QString(511, u'x') + QChar(0x2026)));
    QCOMPARE(plainToolTip(QString(512, u'y')), QStringLiteral("<p style='white-space:pre'>") + QString(512, u'y')
                                                   + QStringLiteral("</p>"));

    QCOMPARE(elideMiddle(QStringLiteral("abcdefghij"), 20), QStringLiteral("abcdefghij"));
    QCOMPARE(elideMiddle(QStringLiteral("abcdefghij"), 5), QStringLiteral("ab\u2026ij"));
    QCOMPARE(elideMiddle(QStringLiteral("abcdefghij"), 6), QStringLiteral("abc\u2026ij"));
    QCOMPARE(elideMiddle(QString(1000, u'z'), 400).size(), 400);
}

void TestVisualView::linkToolTipsArePlainText()
{
    // A link whose address is markup: hovering it must show the address,
    // not load the picture it names. On Linux "//{dir}" is the local
    // folder, so a picture that loaded would paint red.
    QTemporaryDir dir;
    QImage red(40, 40, QImage::Format_RGB32);
    red.fill(Qt::red);
    QVERIFY(red.save(dir.filePath(QStringLiteral("red.png"))));
    const QString href = QStringLiteral("x<img src=\"//%1/red.png\" width=\"40\" height=\"40\">").arg(dir.path().mid(1));
    QString escaped = href.toHtmlEscaped();
    m_doc->setPlainText(QStringLiteral("<p><a href=\"%1\">linklinklinklinklinklinklinklink</a></p>\n").arg(escaped));
    m_model->flush();
    m_view->clearFocus(); // Everything rendered.
    QToolTip::hideText();
    bool shown = false;
    for (int y = 0; y < 120 && !shown; y += 3) {
        for (int x = 20; x < 400 && !shown; x += 6) {
            QTest::mouseMove(m_view->viewport(), QPoint(x, y));
            shown = QToolTip::isVisible();
        }
    }
    QVERIFY(shown);
    QCOMPARE(QToolTip::text(), plainToolTip(href));
    QWidget *tip = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w->inherits("QTipLabel") && w->isVisible())
            tip = w;
    }
    QVERIFY(tip);
    QCOMPARE(countColor(tip->grab().toImage(), Qt::red), 0);
    QToolTip::hideText();
}

void TestVisualView::rawPictureNamesAreNotOpened_data()
{
    // Names that are not valid URLs (a ':' before the first '/'): Qt's image
    // handler opens them as written, file names in the working folder, and
    // looks for "@2x" files next to them on high-DPI screens. On Windows a
    // name starting "\\" is an SMB connection to a host. On Linux the
    // backslashes are part of a file name: each is made, red, in the working
    // folder (and "//{dir}" is the folder itself), so a name that slipped
    // through would paint red.
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("file"); // Made red in the folder.
    QTest::addColumn<qreal>("dpr");
    QTest::addColumn<bool>("loads");
    const auto row = [](const char *tag, const char *name, const char *file, qreal dpr = 1, bool loads = false) {
        QTest::newRow(tag) << QString::fromUtf8(name) << QString::fromUtf8(file) << dpr << loads;
    };
    row("control", "red.png", "red.png", 1, true);
    row("control 2x", "red.png", "red.png", 2, true);
    row("unc colon", R"(\\probe\share\a:b.png)", R"(\\probe\share\a:b.png)");
    row("unc stream", R"(\\probe\s\x.png:z)", R"(\\probe\s\x.png:z)");
    row("unc ssl", R"(\\probe@SSL\s\a:b.png)", R"(\\probe@SSL\s\a:b.png)");
    row("unc 2x", R"(\\probe\share\a:b.png)", R"(\\probe\share\a:b@2x.png)", 2);
    row("mixed slashes", R"(\/probe\s\a:b.png)", R"(\/probe\s\a:b.png)");
    row("encoded unc", "%5C%5Cprobe%5Cs%5Ca:b.png", "%5C%5Cprobe%5Cs%5Ca:b.png");
    row("encoded slashes", "%2F%2Fprobe%2Fs%2Fa:b.png", "%2F%2Fprobe%2Fs%2Fa:b.png");
    row("double encoded", "%255C%255Cprobe%255Cs%255Ca:b.png", "%255C%255Cprobe%255Cs%255Ca:b.png");
    row("file unc", R"(file:\\probe\s\a:b.png)", R"(file:\\probe\s\a:b.png)");
    row("slashes colon", "//{dir}/a:b.png", "a:b.png");
    row("slashes colon 2x", "//{dir}/a:b.png", "a:b@2x.png", 2);
    row("file host", "file://{dir}/a.png", "a.png");
    row("file host colon", "file://{dir}/a:b.png", "a:b.png");
    row("encoded host", "%2F%2F{encoded}%2Fa:b.png", "%2F%2F{encoded}%2Fa:b.png");
}

void TestVisualView::rawPictureNamesAreNotOpened()
{
    QFETCH(QString, name);
    QFETCH(QString, file);
    QFETCH(qreal, dpr);
    QFETCH(bool, loads);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString previous = QDir::currentPath();
    QVERIFY(QDir::setCurrent(dir.path()));
    const auto restore = qScopeGuard([&] { QDir::setCurrent(previous); });
    const QString folder = dir.path().mid(1);
    for (QString *s : {&name, &file}) {
        s->replace(QStringLiteral("{dir}"), folder);
        s->replace(QStringLiteral("{encoded}"), QString(folder).replace(u'/', QStringLiteral("%2F")));
    }
    QImage red(40, 40, QImage::Format_RGB32);
    red.fill(Qt::red);
#ifdef Q_OS_WIN
    // The names are UNC paths (making them would reach for host "probe"),
    // "file:" folders, which NTFS cannot have, or streams: not local files
    // here. What is checked is that none of them is read.
    if (!loads)
        file.clear();
#endif
    if (!file.isEmpty()) {
        const QString redPath = dir.filePath(file);
        QVERIFY(QDir().mkpath(QFileInfo(redPath).absolutePath()));
        QVERIFY2(red.save(redPath, "PNG"), qPrintable(redPath));
    }

    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    ctx.width = 400;
    ctx.devicePixelRatio = dpr;
    ctx.baseDirectory = dir.path();
    core::Block block;
    block.kind = core::BlockKind::Html;
    const QByteArray html = QStringLiteral(R"(<p><img src="%1" width="40" height="40"></p>)").arg(name).toUtf8();
    block.html = rust::String(html.constData(), html.size());
    const int reads = RenderedBlock::pictureFileReads();
    const auto rendered = RenderedBlock::create(block, QString::fromUtf8(html), ctx);
    QImage image(int(ctx.width * dpr), qMax(1, int(rendered->height() * dpr)), QImage::Format_RGB32);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        rendered->paint(painter, QPointF(0, 0), ctx.width);
    }
    QCOMPARE(countColor(image, Qt::red) > 0, loads);
    QCOMPARE(bool(rendered->pictureAt(QPointF(20, 20))), loads);
    if (!loads)
        QCOMPARE(RenderedBlock::pictureFileReads(), reads);
    // Listing the pictures for the remote-image banner opens nothing either.
    QVERIFY(RenderedBlock::remoteImages(block, ctx).isEmpty());
}

void TestVisualView::networkPathsAreNotOpened_data()
{
    // {dir} is the picture folder without its leading slash: on Linux
    // "//{dir}" names the same local folder as "/{dir}", so a path that
    // slipped through would paint red. On Windows it would be an SMB
    // connection to a host. {path} is the folder as an absolute path and
    // {url} as a file URL ("C:/..." and "file:///C:/..." on Windows, where
    // "/{dir}" would be "/:/Users/...").
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
    row("absolute", "={path}/red.png", false, true);
    row("file url", "={url}/red.png", false, true);
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
    row("link local style sheet", R"(<link rel="stylesheet" type="text/css" href="{path}/red.css"><p>xxxxxxxx</p>)",
        false, false);
    row("style import", R"(<style>@import url("//{dir}/red.css");</style><p>xxxxxxxx</p>)", false, false);
    row("svg with network picture", "=network.svg", false, false);
    // Declined, an SVG named by an absolute path would be opened by Qt's
    // image handler itself (QtSvg then loads the picture it names).
    row("absolute svg with network picture", "={path}/network.svg", false, false);
    row("file url svg with network picture", "={url}/network.svg", false, false);
    // The same, gzip-compressed (.svgz): the network path cannot be seen
    // without unpacking, QtSvg unpacks it by itself.
    row("svgz with local picture", "=local.svgz", false, true);
    row("svgz with data picture", "=data.svgz", false, true);
    row("svgz without picture", "=shape.svgz", false, true);
    row("svgz padded", "=padded.svgz", false, true);
    row("svgz with network picture", "=network.svgz", false, false);
    row("svgz in two members", "=members.svgz", false, false);
    row("svgz named png", "=disguised.png", false, false);
    row("svgz bomb", "=bomb.svgz", false, false);
    row("svgz cut short", "=truncated.svgz", false, false);
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
    // gzip-compressed variants.
    const auto svgText = [](const QString &href, const QString &padding = {}) {
        return QStringLiteral(R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" )"
                              R"(width="40" height="40"><!--%2--><image x="0" y="0" width="40" height="40" )"
                              R"(xlink:href="%1"/></svg>)")
            .arg(href, padding)
            .toUtf8();
    };
    const auto writeFile = [&](const QString &name, const QByteArray &bytes) {
        QFile file(dir.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    };
    const QString networkHref = u"//" + folder + QStringLiteral("/red.png");
    {
        QFile redFile(dir.filePath(QStringLiteral("red.png")));
        QVERIFY(redFile.open(QIODevice::ReadOnly));
        const QByteArray dataHref = "data:image/png;base64," + redFile.readAll().toBase64();
        // QtSvg takes a relative picture path of an .svgz from the working
        // folder, not from the file's, so these paths are absolute.
        const QString redPath = dir.filePath(QStringLiteral("red.png"));
        writeFile(QStringLiteral("local.svgz"), gzipped(svgText(redPath)));
        writeFile(QStringLiteral("data.svgz"), gzipped(svgText(QString::fromLatin1(dataHref))));
        writeFile(QStringLiteral("shape.svgz"),
                  gzipped(R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40">)"
                          R"(<rect width="40" height="40" fill="#ff0000"/></svg>)"));
        writeFile(QStringLiteral("padded.svgz"),
                  gzipped(svgText(redPath, QString(1024 * 1024, u' '))));
        writeFile(QStringLiteral("network.svgz"), gzipped(svgText(networkHref)));
        writeFile(QStringLiteral("disguised.png"), gzipped(svgText(networkHref)));
        // Cut after the first header's worth of bytes: not a whole file.
        writeFile(QStringLiteral("truncated.svgz"), gzipped(svgText(networkHref)).left(40));
        // The picture is local, but the SVG unpacks to more than the limit
        // (a few kilobytes of gzip for 40 MiB).
        writeFile(QStringLiteral("bomb.svgz"),
                  gzipped(svgText(redPath, QString(40 * 1024 * 1024, u' '))));
        // The network picture is in the second member, which QtSvg reads too.
        const QByteArray whole = svgText(networkHref);
        const qsizetype cut = whole.indexOf("<image");
        writeFile(QStringLiteral("members.svgz"), gzipped(whole.left(cut)) + gzipped(whole.mid(cut)));
    }
    source.replace(QStringLiteral("{dir}"), folder);
    source.replace(QStringLiteral("{encoded}"), QString(folder).replace(u'/', QStringLiteral("%2F")));
    source.replace(QStringLiteral("{path}"), dir.path());
    source.replace(QStringLiteral("{url}"), QUrl::fromLocalFile(dir.path()).toString());

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
    } else if (source.contains(QLatin1String("disguised"))) {
        // A .png by name: the picture is listed, but nothing was drawn (above).
    } else if (source.contains(QLatin1String("img")) || markdown) {
        // A click on a picture that did not load must not open it either.
        QCOMPARE(bool(rendered->pictureAt(QPointF(20, 20))), loads);
    }
}

namespace {

// An SVG whose group at each level holds `fanOut` <use>s of the level
// below, `depth` levels over a red square: fanOut^depth squares drawn.
QByteArray useBomb(int depth, int fanOut = 10)
{
    QByteArray svg = R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" )"
                     R"(width="40" height="40"><defs><rect id="l0" width="40" height="40" fill="#ff0000"/>)";
    for (int level = 1; level <= depth; ++level) {
        svg += "<g id=\"l" + QByteArray::number(level) + "\">";
        for (int i = 0; i < fanOut; ++i)
            svg += "<use xlink:href=\"#l" + QByteArray::number(level - 1) + "\"/>";
        svg += "</g>";
    }
    return svg + "</defs><use xlink:href=\"#l" + QByteArray::number(depth) + "\"/></svg>";
}

// The same with patterns filled by style sheet classes: each level's
// pattern holds `fanOut` squares filled with the pattern below.
QByteArray patternBomb(int depth, int fanOut = 10)
{
    QByteArray style = "<style>";
    QByteArray defs = R"(<pattern id="p0" width="4" height="4" patternUnits="userSpaceOnUse">)"
                      R"(<rect width="4" height="4" fill="#ff0000"/></pattern>)";
    for (int level = 1; level <= depth; ++level) {
        const QByteArray below = QByteArray::number(level - 1);
        style += ".c" + below + "{fill:url(#p" + below + ")}";
        defs += "<pattern id=\"p" + QByteArray::number(level)
                + R"(" width="40" height="40" patternUnits="userSpaceOnUse">)";
        for (int i = 0; i < fanOut; ++i)
            defs += "<rect class=\"c" + below + "\" width=\"4\" height=\"4\"/>";
        defs += "</pattern>";
    }
    return R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40">)" + style + "</style><defs>" + defs
           + "</defs><rect width=\"40\" height=\"40\" fill=\"url(#p" + QByteArray::number(depth) + ")\"/></svg>";
}

} // namespace

void TestVisualView::svgBombsAreRefused()
{
    // Small enough to draw: loads, as a download and as a file.
    QVERIFY(!decodePicture(useBomb(3)).isNull());
    QVERIFY(!decodePicture(useBomb(1, 1000)).isNull());

    QElapsedTimer timer;
    timer.start();
    QVERIFY(decodePicture(useBomb(12)).isNull());
    QVERIFY(decodePicture(gzipped(useBomb(12))).isNull());
    QVERIFY(decodePicture(patternBomb(12)).isNull());
    // Inside an SVG picture in an SVG.
    const QByteArray nested = R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40">)"
                              R"(<image width="40" height="40" href="data:image/svg+xml;base64,)"
                              + useBomb(12).toBase64() + R"("/></svg>)";
    QVERIFY(decodePicture(nested).isNull());
    // A marker drawn at every one of 100000 vertices.
    QByteArray markers = R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40"><defs>)"
                         R"(<marker id="m"><rect width="1" height="1" fill="#ff0000"/></marker></defs>)"
                         R"x(<path marker-mid="url(#m)" d="M0 0)x";
    for (int i = 0; i < 100000; ++i)
        markers += " L1 1";
    QVERIFY(decodePicture(markers + R"("/></svg>)").isNull());
    // Cycles end, and are refused as QtSvg refuses them.
    QVERIFY(decodePicture(R"(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" )"
                          R"(width="40" height="40"><g id="a"><use xlink:href="#b"/></g><g id="b">)"
                          R"(<use xlink:href="#a"/></g><use xlink:href="#a"/></svg>)")
                .isNull());
    QVERIFY(decodePicture(R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40"><g id="a">)"
                          R"(<use href="#a"/><rect width="40" height="40" fill="#ff0000"/></g></svg>)")
                .isNull());
    QVERIFY2(timer.elapsed() < 1000, QByteArray::number(timer.elapsed()).constData());
    // Nested too deep, and too many elements.
    const QByteArray svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40">)";
    const QByteArray square = R"(<rect width="40" height="40" fill="#ff0000"/>)";
    QVERIFY(!decodePicture(svg + QByteArray("<g>").repeated(60) + square + QByteArray("</g>").repeated(60) + "</svg>")
                 .isNull());
    QVERIFY(decodePicture(svg + QByteArray("<g>").repeated(80) + square + QByteArray("</g>").repeated(80) + "</svg>")
                .isNull());
    QVERIFY(decodePicture(svg + QByteArray("<g/>").repeated(600000) + "</svg>").isNull());
    // A small SVG inside one is drawn.
    QVERIFY(!decodePicture(svg + R"(<image width="40" height="40" href="data:image/svg+xml;base64,)"
                           + useBomb(1).toBase64() + R"("/></svg>)")
                 .isNull());

    // Picture files: the bomb is not drawn, the small one is.
    QTemporaryDir dir;
    const auto write = [&](const char *name, const QByteArray &bytes) {
        QFile file(dir.filePath(QString::fromLatin1(name)));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    };
    write("small.svg", useBomb(3));
    write("bomb.svg", useBomb(12));
    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    ctx.width = 400;
    ctx.baseDirectory = dir.path();
    for (const auto &[name, loads] : {std::pair{"small.svg", true}, std::pair{"bomb.svg", false}}) {
        core::Block block;
        block.kind = core::BlockKind::Html;
        const QByteArray html = QByteArray(R"(<img src=")") + name + R"(" width="40" height="40">)";
        block.html = rust::String(html.constData(), html.size());
        timer.restart();
        const auto rendered = RenderedBlock::create(block, QString::fromUtf8(html), ctx);
        QImage image(int(ctx.width), qMax(1, int(rendered->height())), QImage::Format_RGB32);
        image.fill(Qt::white);
        {
            QPainter painter(&image);
            rendered->paint(painter, QPointF(0, 0), ctx.width);
        }
        QVERIFY2(timer.elapsed() < 1000, name);
        QCOMPARE(countColor(image, Qt::red) > 0, loads);
    }

    // The SVGs that come with the app still load.
    QDir resources(QStringLiteral(VN_SOURCE_DIR "/resources"));
    int checked = 0;
    for (const QString &sub : {QStringLiteral("app"), QStringLiteral("icons")}) {
        QDir folder(resources.filePath(sub));
        for (const QFileInfo &info : folder.entryInfoList({QStringLiteral("*.svg")}, QDir::Files)) {
            QFile file(info.filePath());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QVERIFY2(!decodePicture(file.readAll()).isNull(), qPrintable(info.fileName()));
            ++checked;
        }
    }
    QVERIFY(checked > 10);
}

namespace {

// A document's first block, rendered at a width.
std::unique_ptr<RenderedBlock> renderFirstBlock(const QString &markdown, qreal width = 548)
{
    const QByteArray utf8 = markdown.toUtf8();
    const rust::Vec<core::Block> blocks = core::blocks(rust::Str(utf8.constData(), utf8.size()),
                                                       core::RenderOptions{false, true, true, true, 1});
    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    ctx.width = width;
    return RenderedBlock::create(blocks.at(0), markdown, ctx);
}

// What differs between two pictures, for a failure message: their sizes, or
// how many pixels differ, the rows and columns they span and the first one.
QString imageDifference(const QImage &actual, const QImage &expected)
{
    if (actual.size() != expected.size())
        return QStringLiteral("sizes %1x%2 and %3x%4")
            .arg(actual.width())
            .arg(actual.height())
            .arg(expected.width())
            .arg(expected.height());
    QString text;
    if (actual.format() != expected.format() || actual.devicePixelRatio() != expected.devicePixelRatio())
        text = QStringLiteral("formats %1 and %2, pixel ratios %3 and %4; ")
                   .arg(int(actual.format()))
                   .arg(int(expected.format()))
                   .arg(actual.devicePixelRatio())
                   .arg(expected.devicePixelRatio());
    const QImage a = actual.convertToFormat(QImage::Format_ARGB32);
    const QImage b = expected.convertToFormat(QImage::Format_ARGB32);
    qsizetype count = 0;
    QRect bounds;
    QPoint first(-1, -1);
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y)) {
                ++count;
                bounds |= QRect(x, y, 1, 1);
                if (first.x() < 0)
                    first = QPoint(x, y);
            }
        }
    }
    if (count == 0)
        return text + QStringLiteral("the same pixels");
    return text
           + QStringLiteral("%1 pixels differ, rows %2-%3, columns %4-%5; first at (%6, %7): #%8 against #%9")
                 .arg(count)
                 .arg(bounds.top())
                 .arg(bounds.bottom())
                 .arg(bounds.left())
                 .arg(bounds.right())
                 .arg(first.x())
                 .arg(first.y())
                 .arg(a.pixel(first), 8, 16, QLatin1Char('0'))
                 .arg(b.pixel(first), 8, 16, QLatin1Char('0'));
}

// `image` saved in the system's temporary folder (kept after the test) for a
// look at a failure; its path.
QString keptForInspection(const QImage &image, const QString &name)
{
    const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("vn-test-%1.png").arg(name));
    return image.save(path) ? path : QStringLiteral("(%1 not saved)").arg(path);
}

QImage paintBlock(const RenderedBlock &block, qreal width = 548)
{
    QImage image(QSize(int(width), qMax(1, qMin(4000, int(block.height()) + 1))), QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    block.paint(painter, QPointF(0, 0), width);
    return image;
}

// Turns the layout limits off for a scope.
struct LayoutLimitsOff
{
    LayoutLimitsOff() { RenderedBlock::setLayoutGuardEnabled(false); }
    ~LayoutLimitsOff() { RenderedBlock::setLayoutGuardEnabled(true); }
};

// The unit with %1 replaced by 0 .. times - 1, joined.
QString repeated(const QString &unit, int times, const QString &separator = {})
{
    QStringList list;
    list.reserve(times);
    for (int i = 0; i < times; ++i)
        list << unit.arg(i);
    return list.join(separator);
}

const QString newline = QStringLiteral("\n");

} // namespace

// Shapes Qt's rich text lays out in time far from linear (a 260 KB word in
// quotes squeezed to nothing took minutes, a list of 20 000 items 14 s)
// become their source in a code box, which is quick.
void TestVisualView::hugeBlocksFallBackToTheirSource_data()
{
    QTest::addColumn<QString>("markdown");
    QTest::newRow("word in nested quotes") << QString(QStringLiteral("> ")).repeated(50) + QString(260000, u'x');
    QTest::newRow("word in a quote") << QStringLiteral("> ") + QString(260000, u'x');
    QTest::newRow("list of 20000") << repeated(QStringLiteral("- item %1 of the list"), 20000, newline);
    QTest::newRow("table of 5000 rows")
        << QStringLiteral("| a | b | c |\n|---|---|---|\n") + repeated(QStringLiteral("| r%1 | x | y |"), 5000, newline);
    QTest::newRow("html nested 3000 deep") << QString(QStringLiteral("<blockquote>")).repeated(3000) + QStringLiteral("text")
                                                  + QString(QStringLiteral("</blockquote>")).repeated(3000);
    QTest::newRow("html table of 5000 cells")
        << QStringLiteral("<table>") + repeated(QStringLiteral("<tr><td>%1</td></tr>"), 5000) + QStringLiteral("</table>");
    QTest::newRow("code line of a million") << QStringLiteral("```\n") + QString(1000000, u'z') + QStringLiteral("\n```");
}

void TestVisualView::hugeBlocksFallBackToTheirSource()
{
    QFETCH(QString, markdown);
    const int before = RenderedBlock::guardedBlockCount();
    QElapsedTimer timer;
    timer.start();
    const auto block = renderFirstBlock(markdown);
    const QImage image = paintBlock(*block);
    const qint64 ms = timer.elapsed();
    QVERIFY2(ms < 3000, qPrintable(QStringLiteral("%1 ms").arg(ms)));
    QCOMPARE(block->kind(), RenderedBlock::Kind::Code);
    QVERIFY(block->height() > 20);
    QVERIFY(!image.isNull());
    // The source is what is shown, word for word.
    QCOMPARE(block->renderedText(), markdown);
    // (A code block is a code block either way.)
    if (!markdown.startsWith(QLatin1String("```")))
        QCOMPARE(RenderedBlock::guardedBlockCount(), before + 1);
}

// Everything an ordinary page holds, and then some, renders as before.
void TestVisualView::inlineMathSitsOnTheBaseline()
{
    // A formula no taller than the text leaves its line as high as without.
    QCOMPARE(renderFirstBlock(QStringLiteral("Some text $k = 32$ and more"))->height(),
             renderFirstBlock(QStringLiteral("Some text k = 32 and more"))->height());
    // The bottom of an "x" of the text and of the "x" a formula ends with
    // are on one row: the formula is on the text's baseline. Also after a
    // fraction, which reaches deeper than the text does.
    for (const QString tex : {QStringLiteral("x"), QStringLiteral("\\frac{a}{b}x")}) {
        const QImage image = paintBlock(*renderFirstBlock(QStringLiteral("x $%1$").arg(tex)));
        // Formulas are filled outlines, not hinted to whole pixels as the
        // text is: the edge of a thin stroke is lighter grey.
        const auto ink = [&](int x, int y) { return qGray(image.pixel(x, y)) < 200; };
        const auto columnHasInk = [&](int x) {
            for (int y = 0; y < image.height(); ++y)
                if (ink(x, y))
                    return true;
            return false;
        };
        const auto lowestInk = [&](int from, int to) {
            for (int y = image.height() - 1; y >= 0; --y)
                for (int x = from; x <= to; ++x)
                    if (ink(x, y))
                        return y;
            return -1;
        };
        int textStart = 0;
        while (textStart < image.width() && !columnHasInk(textStart))
            ++textStart;
        int textEnd = textStart;
        while (textEnd + 1 < image.width() && columnHasInk(textEnd + 1))
            ++textEnd;
        int mathEnd = image.width() - 1;
        while (mathEnd > textEnd && !columnHasInk(mathEnd))
            --mathEnd;
        QVERIFY2(mathEnd > textEnd + 3, qPrintable(tex));
        const int text = lowestInk(textStart, textEnd);
        const int math = lowestInk(mathEnd - 3, mathEnd);
        QVERIFY2(qAbs(text - math) <= 1, qPrintable(QStringLiteral("%1: text %2, formula %3").arg(tex).arg(text).arg(math)));
    }
}

// The rectangle of the pixels darker than the background.
static QRect inkBounds(const QImage &image)
{
    QRect bounds;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qGray(image.pixel(x, y)) < 200)
                bounds |= QRect(x, y, 1, 1);
    return bounds;
}

void TestVisualView::mathFollowsTheTextSize()
{
    // A heading's formula is as large as the heading's text.
    const QRect heading = inkBounds(paintBlock(*renderFirstBlock(QStringLiteral("# $x^2$"))));
    const QRect paragraph = inkBounds(paintBlock(*renderFirstBlock(QStringLiteral("$x^2$"))));
    QVERIFY(!heading.isEmpty() && !paragraph.isEmpty());
    QVERIFY2(heading.height() > paragraph.height() * 1.3,
             qPrintable(QStringLiteral("%1 %2").arg(heading.height()).arg(paragraph.height())));
}

void TestVisualView::wideMathShrinksToTheColumn()
{
    // Wider than the column: shrunk to it as a whole, not cut off.
    const QString markdown = QStringLiteral("$%1y$").arg(repeated(QStringLiteral("x_{%1}+"), 30));
    const QRect wide = inkBounds(paintBlock(*renderFirstBlock(markdown, 3000), 3000));
    const QRect narrow = inkBounds(paintBlock(*renderFirstBlock(markdown, 300), 300));
    QVERIFY2(wide.width() > 600, qPrintable(QString::number(wide.width())));
    QVERIFY2(narrow.width() <= 300 && narrow.width() > 270, qPrintable(QString::number(narrow.width())));
    const qreal shrink = qreal(narrow.width()) / wide.width();
    QVERIFY2(qAbs(narrow.height() - wide.height() * shrink) <= 3,
             qPrintable(QStringLiteral("%1 %2 %3").arg(narrow.height()).arg(wide.height()).arg(shrink)));
}

void TestVisualView::mathIsSharpWhenZoomed()
{
    // Painted four times as large (as a magnified view paints), formulas
    // are drawn at that size: edges stay a pixel wide, where a magnified
    // picture's blur over four.
    for (const QString &markdown : {QStringLiteral("$$\\sum_{i=1}^n \\frac{a_i}{b}$$"),
                                    QStringLiteral("Text $\\sum_{i=1}^n \\frac{a_i}{b}$ text")}) {
        const auto block = renderFirstBlock(markdown);
        QImage image(QSize(548, int(block->height()) + 1) * 4, QImage::Format_RGB32);
        image.fill(Qt::white);
        {
            QPainter painter(&image);
            painter.scale(4, 4);
            block->paint(painter, QPointF(0, 0), 548);
        }
        int partial = 0, full = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const int gray = qGray(image.pixel(x, y));
                full += gray < 60;
                partial += gray >= 60 && gray < 230;
            }
        }
        QVERIFY2(full > 0 && partial < full, qPrintable(QStringLiteral("%1: %2 partly, %3 fully").arg(markdown).arg(partial).arg(full)));
    }
}

void TestVisualView::lineSpacingIsTheTextsBesideFormulas()
{
    // More line spacing adds no more to a paragraph with a tall formula than
    // to one without: the space added is the text's, not the formula's
    // multiplied (a fraction's line once got the spacing of a fraction). It
    // may add less: the formula takes some of the new space between lines
    // before it moves them apart (TeX's rule; see placeMath).
    const auto height = [](const QString &markdown, qreal lineHeight) {
        const QByteArray utf8 = markdown.toUtf8();
        const rust::Vec<core::Block> blocks = core::blocks(rust::Str(utf8.constData(), utf8.size()),
                                                           core::RenderOptions{false, true, true, true, 1});
        RenderContext ctx;
        ctx.font = theme::textFont();
        ctx.codeFont = theme::editorFont();
        ctx.width = 300;
        ctx.lineHeight = lineHeight;
        return RenderedBlock::create(blocks.at(0), markdown, ctx)->height();
    };
    const QString words = QStringLiteral("Words that wrap onto a second line and then a third line of this paragraph ");
    const QString tall = words + QStringLiteral("$\\left(\\frac{\\frac{\\sum_k e^{s_{ik}}}{\\sqrt{d}}}{\\frac{c}{d}}\\right)$");
    const QString plain = words + QStringLiteral("x");
    const qreal tallAdded = height(tall, 2.0) - height(tall, 1.0);
    const qreal plainAdded = height(plain, 2.0) - height(plain, 1.0);
    QVERIFY2(height(tall, 1.0) > height(plain, 1.0) + 5, "the formula is not taller than the text");
    QVERIFY2(tallAdded <= plainAdded + 2 && tallAdded >= plainAdded / 2,
             qPrintable(QStringLiteral("%1 %2").arg(tallAdded).arg(plainAdded)));
}

void TestVisualView::smallFormulasKeepTheLineInEveryFont()
{
    // TeX's rule (BlockRenderer's placeMath): a formula moves its line away
    // from the others only by what it needs beyond the room the line has
    // anyway, the font's ascent above and descent and leading below, plus
    // half the space the line spacing adds. Every font reports that room
    // its own way (Calibri's small sizes, from bitmaps, as no descent and all
    // of it as leading; Cambria's as no descent and a negative leading);
    // none may make a line grow by more than that, or by less. A
    // superscript, a subscript, a unit or a pair of brackets fits in the
    // room of an ordinary font; a fraction in brackets reaches deeper.
    const auto reach = [](const QStringList &texs) {
        qreal up = 0, down = 0; // At the layout size.
        for (const QString &tex : texs) {
            const auto formula = math::layout(math::withMacros(tex), false);
            if (formula->fellBack)
                return std::pair<qreal, qreal>(-1, -1);
            up = std::max(up, formula->ascent);
            down = std::max(down, formula->depth);
        }
        return std::pair(up, down);
    };
    const QStringList small = {QStringLiteral("\\sigma^2"), QStringLiteral("2.5\\,\\mathrm{m}"),
                               QStringLiteral("\\lambda_{\\text{train}}"), QStringLiteral("(x)"), QStringLiteral("[0, 1]")};
    const QStringList deep = {QStringLiteral("\\left(\\frac{a}{b}\\right)")};
    const auto line = [](const QStringList &texs, bool formulas) {
        QString text = QStringLiteral("Text");
        for (const QString &tex : texs)
            text += formulas ? QStringLiteral(" $%1$ and").arg(tex) : QStringLiteral(" xy and");
        return text;
    };
    const auto height = [](const QString &text, const QFont &font) {
        const QByteArray utf8 = text.toUtf8();
        const rust::Vec<core::Block> blocks = core::blocks(rust::Str(utf8.constData(), utf8.size()),
                                                           core::RenderOptions{false, true, true, true, 1});
        RenderContext ctx;
        ctx.font = font;
        ctx.codeFont = theme::editorFont();
        ctx.width = 4000; // One line.
        ctx.lineHeight = 1.2;
        return RenderedBlock::create(blocks.at(0), text, ctx)->height();
    };
    int fonts = 0, grew = 0;
    QStringList report;
    for (const bool isDeep : {false, true}) {
        const QStringList &texs = isDeep ? deep : small;
        const auto [up, down] = reach(texs);
        QVERIFY(up > 0);
        for (const QString &family : QFontDatabase::families(QFontDatabase::Latin)) {
            QFont probe(family);
            if (QFontInfo(probe).family() != family || QFontDatabase::isPrivateFamily(family))
                continue; // Not this family, or not one to offer.
            // A font without the letters of the line (a symbol font): the
            // plain line would be in another font.
            const QFontMetricsF probeMetrics(probe);
            if (!std::all_of(u"Textandxy", u"Textandxy" + 9, [&](char16_t c) { return probeMetrics.inFont(QChar(c)); }))
                continue;
            fonts += !isDeep;
            for (int points = 9; points <= 14; ++points) {
                QFont font(family, points);
                const int pixels = QFontInfo(font).pixelSize();
                const QFontMetricsF metrics(font);
                const qreal scale = pixels * 1.1 / math::layoutPixelSize;
                const qreal leading = metrics.leading();
                const qreal slack = qCeil(metrics.ascent() + metrics.descent() + leading) * 0.2 / 2 + 1;
                const qreal needed = std::max<qreal>(0, up * scale - metrics.ascent() - slack)
                                     + std::max<qreal>(0, down * scale - metrics.descent() - leading - slack);
                const qreal growth = height(line(texs, true), font) - height(line(texs, false), font);
                if (growth > 0.5)
                    ++grew;
                // A pixel for rounding each way, two up when a depth is
                // reserved (in whole pixels).
                if (growth > needed + (needed > 0 ? 2 : 1) || growth < needed - 1)
                    report << QStringLiteral("%1 %2pt%3: grew %4, needed %5")
                                  .arg(family)
                                  .arg(points)
                                  .arg(isDeep ? QStringLiteral(" (deep)") : QString())
                                  .arg(growth)
                                  .arg(needed);
            }
        }
    }
    qInfo("%d fonts, %d sizes with a taller line", fonts, grew);
    QVERIFY2(report.isEmpty(), qPrintable(report.join(QStringLiteral("; "))));
}

void TestVisualView::aCellOfAFormulaAloneSitsOnTheTextsBaseline()
{
    // A line of formulas alone (a table heading "$k = 4$") has its baseline
    // where a line of text does: the bottom of "k = 4" is the bottom of
    // "Method" in the next cell. (Qt makes such a line as high as the
    // formulas' boxes, its baseline half way.)
    const QImage image = paintBlock(*renderFirstBlock(QStringLiteral("| Method | $k = 4$ |\n|---|---|\n| a | b |")));
    const auto dark = [&](int x, int y) { return qGray(image.pixel(x, y)) < 160; };
    // The heading row's ink, split where the gap between its two cells is.
    int top = -1, left = image.width(), right = -1;
    for (int y = 0; y < image.height() && top < 0; ++y)
        for (int x = 0; x < image.width(); ++x)
            if (dark(x, y)) { top = y; break; }
    QVERIFY(top >= 0);
    const int rowEnd = std::min(image.height(), top + 30);
    QList<bool> inked(image.width(), false);
    for (int y = top; y < rowEnd; ++y)
        for (int x = 0; x < image.width(); ++x)
            if (dark(x, y)) { inked[x] = true; left = std::min(left, x); right = std::max(right, x); }
    // The widest run of columns without ink between the two words.
    int gapStart = -1, gapEnd = -1, run = 0;
    for (int x = left; x <= right; ++x) {
        run = inked[x] ? 0 : run + 1;
        if (run > gapEnd - gapStart) { gapEnd = x; gapStart = x - run; }
    }
    const auto lowest = [&](int from, int to) {
        for (int y = rowEnd - 1; y >= top; --y)
            for (int x = from; x <= to; ++x)
                if (dark(x, y))
                    return y;
        return -1;
    };
    const int method = lowest(left, gapStart);
    const int formula = lowest(gapEnd, right);
    QVERIFY2(std::abs(method - formula) <= 1, qPrintable(QStringLiteral("Method %1, k = 4 %2").arg(method).arg(formula)));
}

void TestVisualView::ordinaryBlocksIgnoreTheLayoutLimits()
{
    const QStringList documents = {
        QString::fromUtf8(sample),
        repeated(QStringLiteral("- item %1 of the list"), 2000, newline),
        repeated(QStringLiteral("%1. item of the list"), 500, newline),
        QStringLiteral("| a | b | c | d |\n|---|---|---|---|\n") + repeated(QStringLiteral("| r%1 | x | y | z |"), 900, newline),
        QString(QStringLiteral("> ")).repeated(8) + QString(5000, u'w'),
        QString(QStringLiteral("> ")).repeated(8) + repeated(QStringLiteral("word%1"), 20000, QStringLiteral(" ")),
        QString(QStringLiteral("<blockquote>")).repeated(60) + QStringLiteral("text")
            + QString(QStringLiteral("</blockquote>")).repeated(60),
        QString(20000, QChar(0x4E2D)), // CJK: a break after every character.
        QStringLiteral("```cpp\n") + repeated(QStringLiteral("int x%1 = f(%1, \"abc\");"), 3000, newline) + QStringLiteral("\n```"),
        QStringLiteral("```\n") + QString(30000, u'q') + QStringLiteral("\n```"),
    };
    const int before = RenderedBlock::guardedBlockCount();
    for (const QString &markdown : documents) {
        const auto guarded = renderFirstBlock(markdown);
        const auto unguarded = [&] {
            LayoutLimitsOff off;
            return renderFirstBlock(markdown);
        }();
        QVERIFY2(guarded->kind() == unguarded->kind(), qPrintable(markdown.left(40)));
        QCOMPARE(guarded->height(), unguarded->height());
        QVERIFY2(paintBlock(*guarded) == paintBlock(*unguarded), qPrintable(markdown.left(40)));
        QVERIFY(guarded->lineAnchors() == unguarded->lineAnchors());
    }
    QCOMPARE(RenderedBlock::guardedBlockCount(), before);
}

// A code box wraps a very long run of text anywhere, which Qt lays out in
// linear time; for a run it would wrap in the same places anyway.
void TestVisualView::longRunsWrapTheSameWithTheLimits()
{
    const QString markdown = QStringLiteral("```\nshort line\n") + QString(70000, u'z') + QStringLiteral("\n```");
    const auto guarded = renderFirstBlock(markdown);
    const auto unguarded = [&] {
        LayoutLimitsOff off;
        return renderFirstBlock(markdown);
    }();
    QCOMPARE(guarded->height(), unguarded->height());
    QVERIFY(paintBlock(*guarded) == paintBlock(*unguarded));
    QVERIFY(guarded->lineAnchors() == unguarded->lineAnchors());
}

// The view remembers the render of each block it paints so that painting does
// not rebuild keys; scrolling through a long document must not keep them all.
void TestVisualView::scrollingKeepsOnlyRecentRenders()
{
    QStringList paragraphs;
    for (int i = 0; i < 3000; ++i)
        paragraphs << QStringLiteral("Paragraph %1 of a long document, with some **words** in it.").arg(i);
    m_doc->setPlainText(paragraphs.join(QStringLiteral("\n\n")));
    m_model->flush();
    QScrollBar *bar = m_view->verticalScrollBar();
    bar->setValue(0);
    const QImage top = m_view->grab().toImage();
    int most = 0;
    for (int value = 0; value <= bar->maximum(); value += 15) {
        bar->setValue(value);
        QVERIFY(!m_view->grab().isNull());
        most = qMax(most, m_view->memoizedRenderCount());
    }
    QVERIFY2(most > 10 && most <= 512, qPrintable(QString::number(most)));
    QVERIFY(bar->maximum() > 3000);
    // Blocks come back the way they were.
    bar->setValue(0);
    QCOMPARE(m_view->grab().toImage(), top);
}

// More pictures than the decoded-picture cache holds, shown one after the
// other at every new window width: the ones that fit keep serving, instead
// of all being pushed out just before they are needed again.
void TestVisualView::decodedPictureCacheSurvivesRoundsOfMorePictures()
{
    QTemporaryDir dir;
    QStringList blocks;
    for (int n = 0; n < 6; ++n) {
        QImage picture(300, 200, QImage::Format_RGB32);
        for (int y = 0; y < picture.height(); ++y)
            for (int x = 0; x < picture.width(); ++x)
                picture.setPixel(x, y, qRgb((x * 7 + n * 40) % 256, (y * 5 + x / 3) % 256, (x * y + n) % 256));
        QVERIFY(picture.save(dir.filePath(QStringLiteral("p%1.png").arg(n))));
        blocks << QStringLiteral("<p><img src=\"p%1.png\"></p>").arg(n);
    }
    // Four of the six fit (held as 24 bit: 176 KiB each).
    RenderedBlock::setDecodedPictureLimitKiB(4 * 176 + 8);
    const auto round = [&](int width) {
        const int start = RenderedBlock::pictureFileReads();
        QList<QImage> images;
        for (const QString &html : std::as_const(blocks))
            images << paintHtml(html, dir.path(), width);
        return std::pair(RenderedBlock::pictureFileReads() - start, images);
    };
    QCOMPARE(round(290).first, 6);
    for (int width = 280; width >= 250; width -= 10) {
        const auto [reads, images] = round(width);
        QCOMPARE(reads, 2);
        // The same pixels whether the picture was kept or read again.
        for (int n = 0; n < 6; ++n) {
            const QImage expected = QImage(dir.filePath(QStringLiteral("p%1.png").arg(n))).scaledToWidth(width, Qt::SmoothTransformation);
            QCOMPARE(images[n].copy(0, 0, expected.width(), expected.height()).convertToFormat(QImage::Format_RGB32),
                     expected.convertToFormat(QImage::Format_RGB32));
        }
    }
    // A few pictures looked at again and again are kept after all.
    blocks = blocks.mid(4);
    int last = -1;
    for (int width = 240; width >= 200 && last != 0; width -= 10) {
        const int start = RenderedBlock::pictureFileReads();
        for (const QString &html : std::as_const(blocks))
            paintHtml(html, dir.path(), width);
        last = RenderedBlock::pictureFileReads() - start;
    }
    QCOMPARE(last, 0);
    RenderedBlock::setDecodedPictureLimitKiB(128 * 1024);
}

// A block too big for the render cache (about 56 MiB laid out), shown with
// others: the view keeps what it shows rather than making the big one again
// for each of the others.
void TestVisualView::blocksOverTheBudgetAreRenderedOnce()
{
    m_doc->setPlainText(QStringLiteral("# Title\n\n") + repeated(QStringLiteral("word%1"), 90000, QStringLiteral(" "))
                        + QStringLiteral("\n\nAfter.\n"));
    m_view->clearFocus();
    m_model->flush();
    QVERIFY(!m_view->grab().isNull());
    const int made = m_view->renderCount();
    QVERIFY(made >= 3);
    for (int i = 0; i < 3; ++i) {
        m_view->viewport()->update();
        QVERIFY(!m_view->grab().isNull());
    }
    QCOMPARE(m_view->renderCount(), made);
}

namespace {

// A block painted into a window of it, starting `top` pixels down, either
// clipped to the window (as views paint) or not at all.
QImage paintWindow(const RenderedBlock &block, qreal top, bool clipped, qreal width = 548)
{
    QImage image(QSize(int(width), 300), QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    if (clipped)
        painter.setClipRect(image.rect());
    block.paint(painter, QPointF(0, -top), width);
    return image;
}

} // namespace

// Blocks far taller than the window draw only what shows, and that the same.
void TestVisualView::clippedPaintsMatchWholePaints()
{
    const QStringList documents = {
        QStringLiteral("```cpp\n") + repeated(QStringLiteral("int x%1 = f(%1, \"abc\");"), 3000, newline) + QStringLiteral("\n```"),
        repeated(QStringLiteral("- item %1 with *emphasis* and `code`"), 1500, newline),
        QStringLiteral("| a | b |\n|---|---|\n") + repeated(QStringLiteral("| r%1 | **x** |"), 1000, newline),
    };
    for (const QString &markdown : documents) {
        const auto block = renderFirstBlock(markdown);
        QVERIFY(block->height() > 10000);
        for (const qreal top : {0.0, 777.5, block->height() / 2, block->height() - 200}) {
            QVERIFY2(paintWindow(*block, top, true) == paintWindow(*block, top, false),
                     qPrintable(QStringLiteral("%1 at %2").arg(markdown.left(20)).arg(top)));
        }
    }
}

namespace {

// A fenced block too long to highlight: 12 000 lines of 40 characters.
QString longCodeBlock()
{
    return QStringLiteral("```\n") + repeated(QStringLiteral("value_%1 = compute(value_%1 - 1) + 1;   "), 12000, newline)
           + QStringLiteral("\n```");
}

} // namespace

// The parts of a long code box, one under the other, paint what the whole box
// does, wherever they start (row tops are fractional while scrolling).
void TestVisualView::longCodeBoxPartsLookLikeTheBox()
{
    const QString markdown = longCodeBlock();
    const QByteArray utf8 = markdown.toUtf8();
    const rust::Vec<core::Block> blocks = core::blocks(rust::Str(utf8.constData(), utf8.size()),
                                                       core::RenderOptions{false, true, true, true, 1});
    QVERIFY(RenderedBlock::isLongCodeBox(blocks.at(0), markdown.size()));
    const auto whole = renderFirstBlock(markdown);
    const QStringList lines = markdown.split(u'\n');
    std::vector<std::unique_ptr<RenderedBlock>> parts;
    std::vector<qreal> tops;
    qreal height = 0;
    for (int first = 0; first < lines.size(); first += RenderedBlock::partLines) {
        const int count = qMin(RenderedBlock::partLines, int(lines.size()) - first);
        RenderedBlock::Part part;
        part.first = first == 0;
        part.last = first + count == lines.size();
        part.wrapAnywhere = RenderedBlock::wrapsAnywhere(markdown);
        RenderContext ctx;
        ctx.font = theme::textFont();
        ctx.codeFont = theme::editorFont();
        ctx.width = 548;
        parts.push_back(RenderedBlock::createPart(lines.mid(first, count).join(newline), part, ctx));
        // Line anchors, moved into the whole box, are the box's.
        for (const auto &[line, y] : parts.back()->lineAnchors()) {
            const auto it = std::find_if(whole->lineAnchors().begin(), whole->lineAnchors().end(),
                                         [&](const auto &anchor) { return anchor.first == first + line; });
            QVERIFY(it != whole->lineAnchors().end());
            QVERIFY(qAbs(it->second - (height + y)) < 0.01);
        }
        tops.push_back(height);
        height += parts.back()->height();
    }
    QVERIFY(parts.size() > 50);
    QVERIFY(qAbs(height - whole->height()) < 0.01);
    const qreal width = 548;
    // Fractions over and under a half: clips snap to the nearest pixel.
    for (const qreal offset : {0.37, 0.75})
    for (const qreal top : {0.0, tops[1] - 150, tops[1] - 149.5, tops[30] - 123.25, height - 300}) {
        QImage expected(QSize(int(width), 300), QImage::Format_RGB32);
        expected.fill(Qt::white);
        QImage actual = expected;
        {
            QPainter painter(&expected);
            painter.setClipRect(expected.rect());
            whole->paint(painter, QPointF(0, offset - top), width);
        }
        {
            QPainter painter(&actual);
            painter.setClipRect(actual.rect());
            for (size_t i = 0; i < parts.size(); ++i) {
                if (tops[i] + parts[i]->height() > top && tops[i] < top + 300)
                    parts[i]->paint(painter, QPointF(0, offset - top + tops[i]), width);
            }
        }
        QVERIFY2(actual == expected, qPrintable(QStringLiteral("at %1, offset %2: %3").arg(top).arg(offset).arg(imageDifference(actual, expected))));
    }
}

// In a view, a long code box is rows of parts: only those on screen are laid
// out, and it shows, scrolls, hit-tests, selects and triple-clicks as the
// whole box did.
void TestVisualView::longCodeBoxesShowInParts()
{
    m_view->hide();
    const QString text = QStringLiteral("# Long code\n\n") + longCodeBlock() + QStringLiteral("\n\nAfter.\n");
    QTextDocument doc;
    doc.setDocumentLayout(new DocumentLayout(&doc));
    doc.setPlainText(text);
    auto *model = new MarkdownModel(&doc);
    ProbeView view(model);
    RenderedBlock::setPartsEnabled(false);
    ProbeView reference(model);
    reference.resize(600, 500);
    reference.show();
    QVERIFY(QTest::qWaitForWindowExposed(&reference));
    reference.grab();
    RenderedBlock::setPartsEnabled(true);
    view.resize(600, 500);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    const int codeFirst = 2;
    const int codeLast = codeFirst + 12001;
    QCOMPARE(view.renderedRowsOver(codeFirst, codeLast), (12002 + RenderedBlock::partLines - 1) / RenderedBlock::partLines);
    QCOMPARE(reference.renderedRowsOver(codeFirst, codeLast), 1);
    QVERIFY(!view.grab().isNull());
    QVERIFY2(view.memoizedRenderCount() < 8, qPrintable(QString::number(view.memoizedRenderCount())));
    // A selection over the edge of two parts, seen at the same place in both.
    QTextCursor selection(doc.findBlockByNumber(codeFirst + 590));
    selection.setPosition(doc.findBlockByNumber(codeFirst + 610).position() + 7, QTextCursor::KeepAnchor);
    view.setTextCursor(selection);
    reference.setTextCursor(selection);
    view.clearFocus();
    reference.clearFocus();
    for (const qreal position : {0.0, codeFirst + 585.5, codeFirst + 6000.25, qreal(codeLast - 10)}) {
        view.scrollToLinePosition(position);
        reference.scrollToLinePosition(position);
        QCOMPARE(view.topLinePosition(), reference.topLinePosition());
        // (The scroll bars differ: one row a part gives the bar finer steps.)
        const QImage parts = view.viewport()->grab().toImage();
        const QImage whole = reference.viewport()->grab().toImage();
        if (parts != whole) {
            const QString name = QStringLiteral("long-code-at-%1").arg(position);
            qInfo().noquote() << "at" << position << imageDifference(parts, whole) << "- saved as"
                              << keptForInspection(parts, name + QStringLiteral("-parts")) << "and"
                              << keptForInspection(whole, name + QStringLiteral("-whole"));
        }
        QVERIFY2(parts == whole, qPrintable(QString::number(position)));
        for (int y = 0; y < 500; y += 23)
            QCOMPARE(view.lineAt(y), reference.lineAt(y));
    }
    // A triple click takes the whole box, not a part.
    const QTextCursor unit = view.unitAt(doc.findBlockByNumber(codeFirst + 5000).position());
    QCOMPARE(unit.selectionStart(), doc.findBlockByNumber(codeFirst).position());
    QCOMPARE(unit.selectionEnd(), doc.findBlockByNumber(codeLast + 1).position());
    QCOMPARE(reference.unitAt(doc.findBlockByNumber(codeFirst + 5000).position()).selectionEnd(), unit.selectionEnd());
}

// A document over MarkdownModel::backgroundShowChars shown with nothing
// parsed: the view doesn't wait for the parse, it shows the source line by
// line until the worker is done, then the rendered blocks at the same place.
// A smaller one is rendered at once.
void TestVisualView::largeDocumentsShowTheirSourceUntilParsed()
{
    m_view->hide();
    QTextDocument small;
    small.setDocumentLayout(new DocumentLayout(&small));
    small.setPlainText(largeDocument());
    auto *smallModel = new MarkdownModel(&small);
    ProbeView smallView(smallModel);
    smallView.resize(600, 500);
    smallView.show();
    QVERIFY(QTest::qWaitForWindowExposed(&smallView));
    QVERIFY(!smallModel->hasPendingEdits());
    QVERIFY(smallView.lineIsRendered(13)); // Section 1's paragraph.

    QTextDocument doc;
    doc.setDocumentLayout(new DocumentLayout(&doc));
    doc.setPlainText(largeDocument(MarkdownModel::backgroundShowChars + 4096));
    auto *model = new MarkdownModel(&doc);
    int reparsed = 0;
    connect(model, &MarkdownModel::reparsed, this, [&] { ++reparsed; });
    ProbeView view(model);
    view.resize(600, 500);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QVERIFY(model->isAwaitingParse());
    QVERIFY(model->isParsingInBackground());
    QCOMPARE(model->parseGeneration(), 0);
    QVERIFY(view.rowsConsistent());
    QCOMPARE(view.rowCount(), doc.blockCount());
    QVERIFY(!view.lineIsRendered(13));
    QVERIFY(!view.grab().isNull());
    // Scrolled meanwhile: the same line stays at the top.
    const int line = 4000 * 11 + 2; // A section's paragraph.
    view.scrollToLine(line);
    QTRY_VERIFY(!model->hasPendingEdits());
    QCOMPARE(reparsed, 1);
    QVERIFY(!model->isAwaitingParse());
    QVERIFY(view.rowsConsistent());
    QVERIFY(view.rowCount() < doc.blockCount());
    QVERIFY(view.lineIsRendered(13));
    QVERIFY(view.lineIsRendered(line));
    QCOMPARE(view.topLine(), line);
    QVERIFY(matchesFreshParse(*model));
    QCOMPARE(model->parseGeneration(), 1);
}

// Typing in the source the view shows while the worker parses: its result is
// of older text, so the text as it is then is parsed from it, until the
// parse is of the text as it is.
void TestVisualView::editsWhileShowingTheSourceAreParsed()
{
    m_view->hide();
    QTextDocument doc;
    doc.setDocumentLayout(new DocumentLayout(&doc));
    doc.setPlainText(largeDocument(MarkdownModel::backgroundShowChars + 4096));
    auto *model = new MarkdownModel(&doc);
    int applied = 0;
    connect(model, &MarkdownModel::reparsed, this, [&] {
        QVERIFY(matchesFreshParse(*model));
        ++applied;
    });
    ProbeView view(model);
    view.resize(600, 500);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.setFocus();
    QVERIFY(model->isAwaitingParse());
    QTextCursor c(doc.findBlockByNumber(2));
    c.insertText(QStringLiteral("Typed "));
    QVERIFY(view.rowsConsistent());
    QVERIFY(!view.lineIsRendered(2));
    c.setPosition(doc.findBlockByNumber(50).position());
    c.insertText(QStringLiteral("# Inserted\n\n"));
    QVERIFY(view.rowsConsistent());
    QCOMPARE(view.rowCount(), doc.blockCount());
    QTRY_VERIFY(!model->hasPendingEdits());
    QTRY_VERIFY(!model->isParsingInBackground());
    QCOMPARE(applied, 1);
    QVERIFY(view.rowsConsistent());
    QVERIFY(QString::fromUtf8(model->blocks()[1].html.data(), model->blocks()[1].html.size())
                .startsWith(QStringLiteral("<p>Typed A paragraph")));
    const core::Block &heading = model->blocks()[model->blockAt(50)];
    QVERIFY(QString::fromUtf8(heading.html.data(), heading.html.size()).contains(QStringLiteral("Inserted</h1>")));
}

// Something that needs the blocks now (a click on a task box, a test) parses
// at once; the worker's result then changes nothing.
void TestVisualView::flushWhileShowingTheSource()
{
    m_view->hide();
    QTextDocument doc;
    doc.setDocumentLayout(new DocumentLayout(&doc));
    doc.setPlainText(largeDocument(MarkdownModel::backgroundShowChars + 4096));
    auto *model = new MarkdownModel(&doc);
    ProbeView view(model);
    view.resize(600, 500);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QVERIFY(model->isAwaitingParse());
    model->flush();
    QVERIFY(!model->isAwaitingParse());
    QCOMPARE(model->parseGeneration(), 1);
    QVERIFY(matchesFreshParse(*model));
    QVERIFY(view.lineIsRendered(13));
    QVERIFY(view.rowsConsistent());
    QTRY_VERIFY(!model->isParsingInBackground());
    QTest::qWait(20);
    QCOMPARE(model->parseGeneration(), 1);
    QVERIFY(matchesFreshParse(*model));
}

// Opening a large Markdown file in Source mode, then switching to Visual:
// the switch doesn't wait for the parse, and the caret stays in view.
void TestVisualView::switchingToVisualShowsTheSourceFirst()
{
    m_view->hide();
    QTemporaryDir dir;
    QFile file(dir.filePath(QStringLiteral("large.md")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(largeDocument(MarkdownModel::backgroundShowChars + 4096).toUtf8());
    file.close();
    DocumentTab tab;
    QVERIFY(tab.load(file.fileName()));
    tab.setViewMode(DocumentTab::ViewMode::Source);
    tab.resize(900, 600);
    tab.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tab));
    QTextDocument *doc = tab.document();
    const int line = doc->blockCount() / 2;
    tab.editor()->setTextCursor(QTextCursor(doc->findBlockByNumber(line)));
    MarkdownModel *model = tab.file()->markdownModel();
    tab.setViewMode(DocumentTab::ViewMode::Visual);
    QVERIFY(model->isAwaitingParse());
    QVERIFY(!tab.grab().isNull());
    QTRY_VERIFY(!model->hasPendingEdits());
    QVERIFY(matchesFreshParse(*model));
    VisualView *visual = tab.visualView();
    QCOMPARE(visual->textCursor().blockNumber(), line);
    // In view: on a line from the top one to the last one that shows.
    QVERIFY(!visual->grab().isNull());
    QVERIFY(visual->topLine() <= line);
    QVERIFY(line - visual->topLine() < 100);
}

// A mode switch shows the visual view in half the tab first (the splitter
// still holds the source view), at a width it keeps only until the layout
// catches up: blocks are rendered once, at the width that is painted, and
// the caret carried over from the source stays in view.
void TestVisualView::switchingToVisualRendersAtTheFinalWidthOnly()
{
    m_view->hide();
    QTemporaryDir dir;
    QFile file(dir.filePath(QStringLiteral("doc.md")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(largeDocument(60000).toUtf8());
    file.close();
    DocumentTab tab;
    QVERIFY(tab.load(file.fileName()));
    tab.setViewMode(DocumentTab::ViewMode::Source);
    tab.resize(900, 600);
    tab.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tab));
    QTextDocument *doc = tab.document();
    const int line = doc->blockCount() / 2 + 2;
    tab.editor()->setTextCursor(QTextCursor(doc->findBlockByNumber(line)));
    tab.setViewMode(DocumentTab::ViewMode::Visual);
    // As the event loop goes on: the layout first, then the paint.
    QCoreApplication::sendPostedEvents();
    VisualView *visual = tab.visualView();
    QVERIFY(!visual->grab().isNull());
    QVERIFY(visual->renderCount() > 3);
    QCOMPARE(visual->renderCount(), visual->memoizedRenderCount());
    QCOMPARE(visual->textCursor().blockNumber(), line);
    QVERIFY(visual->topLine() <= line);
    QVERIFY(line - visual->topLine() < 40);
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
