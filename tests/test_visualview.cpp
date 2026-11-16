// The hybrid visual Markdown view, run offscreen. Set VN_SCREENSHOT_DIR to
// save renderings.
#include "DocumentTab.h"
#include "MainWindow.h"
#include "PlainText.h"
#include "Settings.h"
#include "ThreadCpuTime.h"
#include "markdown/MathRenderer.h"
#include "Theme.h"
#include "editor/DocumentLayout.h"
#include "markdown/BlockRenderer.h"
#include "markdown/BlockViewer.h"
#include "markdown/ImageViewer.h"
#include "markdown/MarkdownModel.h"
#include "markdown/TableLayout.h"
#include "markdown/VisualView.h"
#include "editor/TextView.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QAbstractButton>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QPainterPath>
#include <QRawFont>
#include <QScrollBar>
#include <QSet>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QRandomGenerator>
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
// Read mode's pages: headings, paragraphs that wrap over several lines in a
// column, and at the end a 60-row table and a last paragraph.
QString readDocument()
{
    const QString words = QStringLiteral("The quick brown fox jumps over the lazy dog while the cache keeps every "
                                         "result it has built so far. ");
    QString text = QStringLiteral("# Read mode\n\n");
    for (int i = 1; i <= 40; ++i) {
        if (i % 8 == 1)
            text += QStringLiteral("## Part %1\n\n").arg(i / 8 + 1);
        text += QStringLiteral("Paragraph %1. ").arg(i) + words.repeated(3).trimmed() + QStringLiteral("\n\n");
    }
    text += QStringLiteral("| Row | Value |\n|-----|-------|\n");
    for (int i = 1; i <= 60; ++i)
        text += QStringLiteral("| r%1 | value %1 |\n").arg(i);
    text += QStringLiteral("\nThe end.\n");
    return text;
}

// Read mode's wide blocks: a table of 10 columns, a formula and a short table,
// under headings; the table and formula are wider than a column of 70
// characters, the short one is not.
QString wideDocument(int rows = 8)
{
    QString text = QStringLiteral("# Run log\n\nThe last runs, one row each.\n\n");
    text += QStringLiteral("| Run | Branch | Commit | Runner | Cache | Checkout | Configure | Compile | Test | Package |\n"
                           "|---|---|---|---|---|---|---|---|---|---|\n");
    for (int i = 1; i <= rows; ++i)
        text += QStringLiteral("| #48%1 | feature_cache_%1_and_its_long_name | a41c9e%1 | runner-0%1 | partly | %1 s | 11 s | 138 s | 94 s | 22 s |\n")
                    .arg(i);
    QStringList terms;
    for (int i = 1; i <= 40; ++i)
        terms << QStringLiteral("x_{%1}").arg(i);
    text += QStringLiteral("\n$$\n") + terms.join(QStringLiteral(" + ")) + QStringLiteral("\n$$\n\n");
    text += QStringLiteral("## Stage timings\n\n| Stage | Median |\n|---|---|\n| Checkout | 4 s |\n\nThe end.\n");
    for (int i = 1; i <= 40; ++i)
        text += QStringLiteral("\nFiller paragraph %1, so that the document goes on below the screen.\n").arg(i);
    return text;
}

// Read mode's tables that squeeze into the column (long text in one column,
// short words in the others), and one that cannot (ten columns of long words
// that cannot wrap), each under a heading.
QString squeezeDocument()
{
    QString text = QStringLiteral("# Notes\n\n| Id | Description | Owner |\n|---|---|---|\n");
    const QString words =
        QStringLiteral("the quick brown fox jumps over the lazy dog and keeps running through the fields until night "
                       "falls and everybody goes home ");
    for (int i = 1; i <= 4; ++i)
        text += QStringLiteral("| %1 | %2 | ana |\n").arg(i).arg(words + words);
    text += QStringLiteral("\n## Hashes\n\n| a | b | c | d | e | f | g | h | i | j |\n|---|---|---|---|---|---|---|---|---|---|\n");
    const QString word = QStringLiteral("abcdefghijklmnop");
    text += QStringLiteral("| %1 |\n").arg(QStringList(10, word).join(QStringLiteral(" | ")));
    text += QStringLiteral("\nThe end.\n");
    for (int i = 1; i <= 40; ++i)
        text += QStringLiteral("\nFiller paragraph %1, so that the document goes on below the screen.\n").arg(i);
    return text;
}

// The first line of `text` that holds `needle`.
int lineHolding(const QString &text, const QString &needle)
{
    const QStringList lines = text.split(u'\n');
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].contains(needle))
            return i;
    }
    return -1;
}

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
    using VisualView::stripHeightAt;
    using VisualView::isFoldSpot;
    qreal columnWidth() const { return textWidth(); }
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

    // The rendered block of the row holding a line (for the wide-block tests).
    const RenderedBlock *blockOfLine(int line) { return renderedBlockOfLine(line); }
    qreal contentWidthOfLine(int line)
    {
        const RenderedBlock *block = blockOfLine(line);
        return block ? block->contentWidth() : -1;
    }
    qreal tableRightOfLine(int line)
    {
        const RenderedBlock *block = blockOfLine(line);
        const RenderedBlock::TableGrid *grid = block ? block->tableGrid() : nullptr;
        return grid ? grid->xs.back() : -1;
    }

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

// Counts what the view's viewport is asked to paint.
class PaintSpy : public QObject
{
public:
    QList<QRect> rects;

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Paint)
            rects.append(static_cast<QPaintEvent *>(event)->rect());
        return false;
    }
};

class TestVisualView : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();

    void blocksWithoutCaretAreRendered();
    void caretBlinkRepaintsOnlyTheCaretsRow();
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
    void patchedRowsEqualWholeRows_data();
    void patchedRowsEqualWholeRows();
    void patchedRowsWithCaretsEqualWholeRows();
    void patchedRowsAfterTyping();
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
    void bigPicturesArriveInTheirRoom();
    void pictureCachesCanBeReleased();
    void pictureCacheSettingResizesTheCache();
    void codeBlocksArePaintedAsTheStockHighlighterDoes_data();
    void codeBlocksArePaintedAsTheStockHighlighterDoes();
    void renderCostCountsPicturesAndText();
    void clickEnlargesPicture();
    void figuresFitTheirBox();
    void figureBoxFollowsTheViewHeight();
    void formulasAreNotFigures();
    void diagramsFitTheBox();
    void figureBarSizesOneFigure();
    void collapsedBlocksAreOneChip();
    void foldsFollowEditsAndGoWithTheirBlock();
    void foldedSectionsHideTheirBlocks();
    void caretsOpenFoldsOnlyWithFocus();
    void stripSpaceIsAlwaysReserved();
    void clicksCollapseAndOpen();
    void collapseAllLeavesSectionsAndExpandAllOpensThem();
    void copiedTabKeepsItsFolds();
    void blocksStartCollapsedWhenAsked();
    void readModeKeepsThePlace();
    void readColumnsFollowWidthCharactersAndZoom();
    void pageTurnsGreyKeptLinesOnlyGoingForward();
    void readNudgesByButtonsWheelAndKeys();
    void longTablesRepeatTheirHeader();
    void readFiguresFitTheColumnHeight();
    void findAndGoToLineShowTheirScreen();
    void readLineRangeIsTheScreens();
    void readModeClicks();
    void overflowIsOnlyForReadMode();
    void wideBlocksScrollSideways();
    void repeatedHeaderFollowsTheOffset();
    void openShowsAWideBlockInTheViewer();
    void tablesSqueezeBeforeTheyScroll();
    void tablesScrollWhenTheirLongestWordsDoNotFit();
    void viewersLeaveTheWindowUsable();
    void tableViewerIsTheRealTable();
    void tableGridLooksAsQtDrawsIt();
    void reflowedBlocksAreMadeBlocks();
    void tableLayoutIsQtsLayout();
    void tableLayoutBreaksOnlyChangedColumns();
    void equationViewerIsDrawnAsShapes();
    void readModeWaitsForTheFirstParse();
    void figureBarKeepsClicksAndPictureClickStillZooms();
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
    void gapsFollowTheMarkdownLineHeight();
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
    // Text in a view of the given size, locked, in read mode.
    void startReading(const QString &text, int width, int height);
    // Where segment `index` of column `column` (or the first of the row
    // showing `line`) is in the viewport.
    QRect segmentOfLine(int line, int *column = nullptr, int *index = nullptr);
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

void TestVisualView::caretBlinkRepaintsOnlyTheCaretsRow()
{
    // A blink repaints the row the caret is drawn in, not the whole view, and
    // the view painted there is what a whole repaint gives.
    // (The sample's diagram is drawn on a worker; when its picture arrives,
    // which on a busy machine is seconds in, the whole view is repainted, and
    // that is no blink.)
    QTRY_VERIFY_WITH_TIMEOUT(!m_view->hasPendingDiagrams(), 60000);
    QWidget *viewport = m_view->viewport();
    PaintSpy spy;
    viewport->installEventFilter(&spy);
    for (const int line : {4, 10, 24}) { // A blank line, a paragraph, a table row.
        m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(line)));
        QTest::qWait(100);
        const QImage before = viewport->grab().toImage();
        spy.rects.clear();
        QTRY_VERIFY_WITH_TIMEOUT(!spy.rects.isEmpty(), 3000);
        const QRect strip = spy.rects.first();
        QVERIFY2(strip.height() < viewport->height() / 2,
                 qPrintable(QStringLiteral("line %1: repainted %2x%3").arg(line).arg(strip.width()).arg(strip.height())));
        QImage composed = before;
        viewport->render(&composed, strip.topLeft(), QRegion(strip));
        QVERIFY2(composed == viewport->grab().toImage(), qPrintable(QStringLiteral("line %1").arg(line)));
    }
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
    auto *probe = static_cast<ProbeView *>(m_view);
    for (int y = 0; y < m_view->viewport()->height(); y += 2) {
        for (int x = 20; x < 140; x += 2) {
            QTest::mouseMove(m_view->viewport(), QPoint(x, y));
            // (A strip's label is a hand too, but not a box.)
            if (m_view->viewport()->cursor().shape() == Qt::PointingHandCursor && !probe->isFoldSpot(QPoint(x, y))) {
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
    // (The strip's room is above the table.)
    return QPoint(int(probe->textStart()) + (col == 0 ? 30 : 100), int(top + probe->stripHeightAt(24)) + 18 + 38 * row);
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
    // (The band reaches into the strip's room: it wins in a view that can edit.)
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
    // (Right of the strip's label, which a click on collapses.)
    const QPoint above(m_view->tableCellViewRect(2, 0, 2).right() - 1, header.top() - 6);
    const QPoint border(a.left() - 6, a.bottom());
    m_view->setReadOnly(true);
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(0)));
    QTest::qWait(20);
    QTest::mouseMove(viewport, above);
    QVERIFY(viewport->cursor().shape() != Qt::BitmapCursor);
    // (With the pointer over the table, its strip shows.)
    const QImage plain = m_view->grab().toImage();
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
    // Also when the column's width changes while it renders (a scroll bar
    // coming or going): the stand-in, laid out again at the new width, still
    // gives way to the picture.
    spy.clear();
    Settings::instance().setValue(key::mermaidTheme, QStringLiteral("forest"));
    m_view->grab();
    m_view->resize(m_view->width() - 20, m_view->height());
    const QImage narrower = m_view->grab().toImage();
    QTRY_VERIFY(spy.count() > 0);
    QTRY_VERIFY(m_view->grab().toImage() != narrower);
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
    // (Without the room for the strip a picture has over it.)
    const qreal strip = rendered->stripHeight();
    QImage image(QSize(int(width * dpr), qMax(1, int((rendered->height() - strip) * dpr))), QImage::Format_RGB32);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::white);
    QPainter painter(&image);
    rendered->paint(painter, QPointF(0, -strip), width);
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

// A big picture file is read on a worker thread when the context allows
// it: the block keeps its room blank meanwhile and is pending, and once the
// picture is in, the block made again is as tall and shows it. Small ones
// are read at once.
void TestVisualView::bigPicturesArriveInTheirRoom()
{
    QTemporaryDir dir;
    QImage big(1500, 900, QImage::Format_RGB32);
    big.fill(Qt::red);
    QVERIFY(big.save(dir.filePath(QStringLiteral("big.png"))));
    QImage small(60, 40, QImage::Format_RGB32);
    small.fill(Qt::red);
    QVERIFY(small.save(dir.filePath(QStringLiteral("small.png"))));
    const auto make = [&](const QString &name, qreal dpr) {
        RenderContext ctx;
        ctx.font = theme::textFont();
        ctx.codeFont = theme::editorFont();
        ctx.width = 500;
        ctx.devicePixelRatio = dpr;
        ctx.baseDirectory = dir.path();
        ctx.picturesLater = true;
        const QString html = QStringLiteral("<p>Before</p><p><img src=\"%1\"></p><p>after</p>").arg(name);
        core::Block block;
        block.kind = core::BlockKind::Html;
        const QByteArray utf8 = html.toUtf8();
        block.html = rust::String(utf8.constData(), utf8.size());
        return RenderedBlock::create(block, html, ctx);
    };
    const auto redPixels = [](const RenderedBlock &block, qreal dpr) {
        QImage image(QSize(int(500 * dpr), int(block.height() * dpr) + 1), QImage::Format_RGB32);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::white);
        {
            QPainter painter(&image);
            block.paint(painter, QPointF(0, 0), 500);
        }
        int count = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                count += QColor(image.pixel(x, y)) == QColor(Qt::red);
        return count;
    };
    for (const qreal dpr : {1.0, 1.5}) {
        RenderedBlock::releasePictureCaches();
        QSignalSpy ready(&DiagramNotifier::instance(), &DiagramNotifier::ready);
        const int reads = RenderedBlock::pictureFileReads();
        const auto waiting = make(QStringLiteral("big.png"), dpr);
        QVERIFY(waiting->isPending());
        QCOMPARE(redPixels(*waiting, dpr), 0);
        // (Other work may say it is ready too.)
        QTRY_VERIFY_WITH_TIMEOUT(!make(QStringLiteral("big.png"), dpr)->isPending(), 10000);
        QVERIFY(ready.count() > 0);
        const auto shown = make(QStringLiteral("big.png"), dpr);
        QCOMPARE(shown->height(), waiting->height());
        QVERIFY(redPixels(*shown, dpr) > 1000);
        QCOMPARE(RenderedBlock::pictureFileReads(), reads + 1);
    }
    const auto at = make(QStringLiteral("small.png"), 1);
    QVERIFY(!at->isPending());
    QCOMPARE(redPixels(*at, 1), 60 * 40);
}

// Forgotten, the pictures kept for later renders are read again.
void TestVisualView::pictureCachesCanBeReleased()
{
    QTemporaryDir dir;
    QImage picture(300, 200, QImage::Format_RGB32);
    picture.fill(Qt::blue);
    QVERIFY(picture.save(dir.filePath(QStringLiteral("p.png"))));
    const QString html = QStringLiteral("<p><img src=\"p.png\"></p>");
    const int start = RenderedBlock::pictureFileReads();
    const QImage first = paintHtml(html, dir.path(), 400);
    paintHtml(html, dir.path(), 500);
    QCOMPARE(RenderedBlock::pictureFileReads(), start + 1);
    RenderedBlock::releasePictureCaches();
    QCOMPARE(paintHtml(html, dir.path(), 400), first);
    QCOMPARE(RenderedBlock::pictureFileReads(), start + 2);
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

    // (Without the room for the strip a code box has over it.)
    const qreal strip = rendered->stripHeight();
    const QSize size(int(ctx.width), int(rendered->height() - strip));
    QImage actual(size, QImage::Format_RGB32);
    QImage expected(size, QImage::Format_RGB32);
    actual.fill(Qt::white);
    expected.fill(Qt::white);
    {
        QPainter painter(&actual);
        rendered->paint(painter, QPointF(0, -strip), ctx.width);
    }
    {
        QPainter painter(&expected);
        QPainterPath box;
        box.addRoundedRect(QRectF(QPointF(0, 0), QSizeF(ctx.width, rendered->height() - strip - 4)), 3, 3);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(box, theme::color(theme::Role::codeBlock));
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.translate(10, 5);
        QAbstractTextDocumentLayout::PaintContext paint;
        paint.palette.setColor(QPalette::Text, theme::color(theme::Role::text));
        reference.documentLayout()->draw(&painter, paint);
    }
    QCOMPARE(reference.size().height() + 10 + 4, rendered->height() - strip);
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
    // A window of its own, not a block on the one it came from.
    QCOMPARE(viewer->windowModality(), Qt::NonModal);
    QVERIFY(!viewer->isModal());
    QVERIFY(QApplication::activeModalWidget() == nullptr);
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

namespace {

// A flat picture file of the given size in a folder.
void savePicture(const QTemporaryDir &dir, const QString &name, int width, int height)
{
    QImage picture(width, height, QImage::Format_RGB32);
    picture.fill(Qt::darkGreen);
    QVERIFY(picture.save(dir.filePath(name)));
}

// The width of 70 characters of the body text the views make (at zoom 100%).
qreal seventyCharacters()
{
    return 70 * QFontMetricsF(theme::textFont()).averageCharWidth();
}

} // namespace

void TestVisualView::figuresFitTheirBox()
{
    QTemporaryDir dir;
    savePicture(dir, QStringLiteral("wide.png"), 2000, 500);
    savePicture(dir, QStringLiteral("tall.png"), 300, 2000);
    savePicture(dir, QStringLiteral("small.png"), 100, 80);
    m_view->resize(1200, 700);
    m_view->setBaseDirectory(dir.path());
    setDocument(QStringLiteral("![wide](wide.png)\n\n![tall](tall.png)\n\n![small](small.png)\n"), 0);
    m_view->setReadOnly(true);
    m_view->grab();
    auto *view = static_cast<ProbeView *>(m_view);
    const qreal column = view->columnWidth();
    const qreal chars = seventyCharacters();
    QVERIFY(chars < column); // The window is wide: the characters are the limit.

    // Wide: no wider than 70 characters, though the column has room; the
    // shape is kept.
    const auto wide = view->figuresAtLine(0);
    QCOMPARE(int(wide.size()), 1);
    QVERIFY2(qAbs(wide[0].rect.width() - chars) < 1.5, qPrintable(QString::number(wide[0].rect.width())));
    QVERIFY(qAbs(wide[0].rect.height() * 4 - wide[0].rect.width()) < 4);
    QCOMPARE(wide[0].percent, qRound(100 * wide[0].rect.width() / 2000));
    // Tall: no higher than three quarters of what the view shows.
    const auto tall = view->figuresAtLine(2);
    QCOMPARE(int(tall.size()), 1);
    const qreal shown = view->viewport()->height();
    QVERIFY2(tall[0].rect.height() <= shown * 0.75, qPrintable(QString::number(tall[0].rect.height())));
    QVERIFY(tall[0].rect.height() > shown * 0.75 - 20); // (Up to the step the box moves in.)
    QVERIFY(qAbs(tall[0].rect.width() / tall[0].rect.height() - 300.0 / 2000) < 0.01);
    // Small: never enlarged.
    const auto small = view->figuresAtLine(4);
    QCOMPARE(int(small.size()), 1);
    QCOMPARE(small[0].rect.size(), QSizeF(100, 80));
    QCOMPARE(small[0].percent, 100.0);
    // A narrow window: the column is the limit.
    m_view->resize(300, 700);
    setDocument(QStringLiteral("![wide](wide.png)\n"), 0);
    m_view->grab();
    const auto narrow = view->figuresAtLine(0);
    QCOMPARE(int(narrow.size()), 1);
    QVERIFY(narrow[0].rect.width() <= view->columnWidth() + 0.5);
    QVERIFY(narrow[0].rect.width() < chars);
}

void TestVisualView::figureBoxFollowsTheViewHeight()
{
    QTemporaryDir dir;
    savePicture(dir, QStringLiteral("tall.png"), 300, 2000);
    m_view->resize(900, 800);
    m_view->setBaseDirectory(dir.path());
    setDocument(QStringLiteral("Text\n\n![tall](tall.png)\n\nMore text\n"), 0);
    m_view->setReadOnly(true);
    m_view->grab();
    auto *view = static_cast<ProbeView *>(m_view);
    const qreal before = view->figuresAtLine(2).at(0).rect.height();
    QVERIFY(before <= view->viewport()->height() * 0.75);
    const int renders = view->renderCount();

    m_view->resize(900, 400);
    QTRY_VERIFY(view->viewport()->height() < 450);
    m_view->grab();
    const qreal after = view->figuresAtLine(2).at(0).rect.height();
    QVERIFY2(after <= view->viewport()->height() * 0.75, qPrintable(QString::number(after)));
    QVERIFY(after < before);
    // The row is as high as the picture is now (and the line spacing around it).
    qreal top = 0;
    qreal bottom = 0;
    QVERIFY(view->boundsOfLine(2, &top, &bottom));
    QVERIFY(bottom - top >= after);
    QVERIFY2(bottom - top < after * 1.8, qPrintable(QStringLiteral("%1 %2").arg(bottom - top).arg(after)));
    QVERIFY(view->renderCount() > renders);

    // A taller window grows it again, up to what the picture is.
    m_view->resize(900, 1200);
    QTRY_VERIFY(view->viewport()->height() > 1000);
    m_view->grab();
    const qreal grown = view->figuresAtLine(2).at(0).rect.height();
    QVERIFY(grown > before);
    QVERIFY(grown <= 2000);
    QVERIFY(grown <= view->viewport()->height() * 0.75);

    // Only blocks with figures render again when the height changes.
    const int plain = view->renderCount();
    m_view->resize(900, 1000);
    QTRY_VERIFY(view->viewport()->height() < 1100);
    m_view->grab();
    QVERIFY(view->renderCount() - plain <= 1); // The picture's block.
}

void TestVisualView::formulasAreNotFigures()
{
    setDocument(QStringLiteral("$$\n\\frac{a}{b} + c^{2} + d_{i}\n$$\n"), 0);
    const auto &blocks = m_model->blocks();
    QCOMPARE(int(blocks.size()), 1);
    QCOMPARE(blocks[0].kind, core::BlockKind::Math);
    const QString source = m_model->sourceText(0, 2);
    RenderContext plain;
    plain.font = theme::textFont();
    plain.codeFont = theme::editorFont();
    plain.width = 800;
    RenderContext boxed = plain;
    boxed.figureWidth = 40;
    boxed.figureHeight = 16;
    const auto a = RenderedBlock::create(blocks[0], source, plain);
    const auto b = RenderedBlock::create(blocks[0], source, boxed);
    QCOMPARE(a->kind(), RenderedBlock::Kind::Image);
    QCOMPARE(b->height(), a->height());
    QVERIFY(a->figures().empty());
    QVERIFY(b->figures().empty());
    QVERIFY(!RenderedBlock::mayHaveFigure(source));
}

void TestVisualView::diagramsFitTheBox()
{
    const QString source = QStringLiteral("```mermaid\ngraph LR\n  A[Start] --> B[A fairly long label] --> C[End]\n```\n");
    setDocument(source, 0);
    const auto &blocks = m_model->blocks();
    QCOMPARE(int(blocks.size()), 1);
    const QString text = m_model->sourceText(0, 3);
    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    ctx.width = 2000;
    ctx.mermaidTheme = Settings::instance().text(key::mermaidTheme);
    const auto made = [&] { return RenderedBlock::create(blocks[0], text, ctx); };
    QTRY_VERIFY(!made()->isPending());
    const auto own = made()->figures();
    QCOMPARE(int(own.size()), 1);
    const qreal natural = own[0].rect.width();
    QVERIFY(natural > 100); // Not enlarged, nor narrowed by the column.
    QCOMPARE(own[0].percent, 100.0);
    // A box narrower than the diagram.
    ctx.figureWidth = natural / 2;
    auto half = made()->figures();
    QVERIFY(qAbs(half[0].rect.width() - natural / 2) < 1);
    QVERIFY(qAbs(half[0].percent - 50) < 1);
    // A box higher than the diagram changes nothing; a lower one fits the height.
    ctx.figureWidth = 0;
    ctx.figureHeight = own[0].rect.height() * 3;
    QVERIFY(qAbs(made()->figures()[0].rect.width() - natural) < 1);
    ctx.figureHeight = own[0].rect.height() / 4;
    QVERIFY(qAbs(made()->figures()[0].rect.height() - own[0].rect.height() / 4) < 1);
    // The user's size, within the column.
    ctx.figureHeight = 0;
    ctx.figureWidth = natural / 2;
    ctx.figureScales.insert(RenderedBlock::figureId(text), {1.5});
    QVERIFY(qAbs(made()->figures()[0].rect.width() - natural * 0.75) < 1);
    // The picture to zoom is the diagram, wherever it is.
    const auto block = made();
    const QRectF rect = block->figures()[0].rect;
    QVERIFY(block->pictureAt(rect.center()));
}

void TestVisualView::figureBarSizesOneFigure()
{
    QTemporaryDir dir;
    savePicture(dir, QStringLiteral("a.png"), 2000, 500);
    savePicture(dir, QStringLiteral("b.png"), 2000, 600);
    m_view->resize(1200, 900);
    m_view->setBaseDirectory(dir.path());
    setDocument(QStringLiteral("![a](a.png)\n\n![b](b.png)\n"), 0);
    m_view->setReadOnly(true);
    m_view->grab();
    auto *view = static_cast<ProbeView *>(m_view);
    const auto pointOn = [&](int line) {
        qreal top = 0;
        qreal bottom = 0;
        view->boundsOfLine(line, &top, &bottom);
        const QRectF rect = view->figuresAtLine(line).at(0).rect;
        return QPoint(int(view->textStart() + rect.center().x()), int(top + rect.center().y()));
    };
    const auto barOf = [&]() { return view->viewport()->findChild<QWidget *>(QStringLiteral("FigureBar")); };
    const auto buttonOf = [&](const QString &text) {
        for (QToolButton *button : barOf()->findChildren<QToolButton *>()) {
            if (button->text() == text)
                return button;
        }
        return static_cast<QToolButton *>(nullptr);
    };
    QVERIFY(!barOf() || !barOf()->isVisible());

    const qreal fitted = view->figuresAtLine(0).at(0).rect.width();
    const qreal other = view->figuresAtLine(2).at(0).rect.width();
    // The bar shows while the pointer is on the figure, at its top right.
    QTest::mouseMove(view->viewport(), pointOn(0));
    QVERIFY(barOf() && barOf()->isVisible());
    qreal top = 0;
    qreal bottom = 0;
    view->boundsOfLine(0, &top, &bottom);
    const QRectF rect = view->figuresAtLine(0).at(0).rect;
    const QRect where = barOf()->geometry();
    QVERIFY(where.right() <= view->textStart() + rect.right());
    QVERIFY(where.right() > view->textStart() + rect.right() - 20);
    QVERIFY(where.top() >= top + rect.top() && where.top() < top + rect.top() + 20);
    auto *label = barOf()->findChild<QLabel *>();
    QVERIFY(label);
    QCOMPARE(label->text(), QStringLiteral("%1%").arg(qRound(100 * fitted / 2000)));
    QVERIFY(buttonOf(QStringLiteral("Fit")) && !buttonOf(QStringLiteral("Fit"))->isEnabled()); // Fitted already.
    QVERIFY(buttonOf(QStringLiteral("−")) && buttonOf(QStringLiteral("+")));

    // − takes a tenth off the fitted size; + gives it back and adds one.
    QTest::mouseClick(buttonOf(QStringLiteral("−")), Qt::LeftButton);
    QVERIFY2(qAbs(view->figuresAtLine(0).at(0).rect.width() - fitted * 0.9) < 1.5,
             qPrintable(QString::number(view->figuresAtLine(0).at(0).rect.width())));
    QVERIFY(barOf()->isVisible());
    QVERIFY(buttonOf(QStringLiteral("Fit"))->isEnabled());
    QCOMPARE(label->text(), QStringLiteral("%1%").arg(qRound(100 * view->figuresAtLine(0).at(0).rect.width() / 2000)));
    QTest::mouseClick(buttonOf(QStringLiteral("+")), Qt::LeftButton);
    QTest::mouseClick(buttonOf(QStringLiteral("+")), Qt::LeftButton);
    QVERIFY2(qAbs(view->figuresAtLine(0).at(0).rect.width() - fitted * 1.1) < 1.5,
             qPrintable(QString::number(view->figuresAtLine(0).at(0).rect.width())));
    // Only this figure changed.
    QCOMPARE(view->figuresAtLine(2).at(0).rect.width(), other);
    // Fit goes back to the fitted size.
    QTest::mouseClick(buttonOf(QStringLiteral("Fit")), Qt::LeftButton);
    QVERIFY(qAbs(view->figuresAtLine(0).at(0).rect.width() - fitted) < 1.5);
    QVERIFY(!buttonOf(QStringLiteral("Fit"))->isEnabled());

    // + stops at the column, − at a tenth of the fitted size.
    for (int i = 0; i < 100 && buttonOf(QStringLiteral("+"))->isEnabled(); ++i)
        QTest::mouseClick(buttonOf(QStringLiteral("+")), Qt::LeftButton);
    QVERIFY(!buttonOf(QStringLiteral("+"))->isEnabled());
    const qreal widest = view->figuresAtLine(0).at(0).rect.width();
    QVERIFY(widest <= view->columnWidth() + 0.5);
    QVERIFY(widest > view->columnWidth() - 2);
    for (int i = 0; i < 100 && buttonOf(QStringLiteral("−"))->isEnabled(); ++i)
        QTest::mouseClick(buttonOf(QStringLiteral("−")), Qt::LeftButton);
    QVERIFY(!buttonOf(QStringLiteral("−"))->isEnabled());
    QVERIFY(qAbs(view->figuresAtLine(0).at(0).rect.width() - fitted * 0.1) < 1.5);
    QTest::mouseClick(buttonOf(QStringLiteral("Fit")), Qt::LeftButton);

    // The second figure has a bar of its own, and keeps its size meanwhile.
    QTest::mouseMove(view->viewport(), pointOn(2));
    QVERIFY(barOf()->isVisible());
    QTest::mouseClick(buttonOf(QStringLiteral("−")), Qt::LeftButton);
    QVERIFY(view->figuresAtLine(2).at(0).rect.width() < other - 10);
    QVERIFY(qAbs(view->figuresAtLine(0).at(0).rect.width() - fitted) < 1.5);
    // The size is kept while the view re-renders (a new width, the same figure).
    view->resize(1190, 900);
    QTest::qWait(50);
    view->grab();
    QVERIFY(view->figuresAtLine(2).at(0).factor < 1);

    // Off the figures, the bar goes.
    QTest::mouseMove(view->viewport(), QPoint(3, 3));
    QVERIFY(!barOf()->isVisible());
}

void TestVisualView::figureBarKeepsClicksAndPictureClickStillZooms()
{
    QTemporaryDir dir;
    savePicture(dir, QStringLiteral("a.png"), 2000, 500);
    m_view->resize(1200, 900);
    m_view->setBaseDirectory(dir.path());
    setDocument(QStringLiteral("![a](a.png)\n"), 0);
    m_view->setReadOnly(true);
    m_view->grab();
    auto *view = static_cast<ProbeView *>(m_view);
    qreal top = 0;
    qreal bottom = 0;
    QVERIFY(view->boundsOfLine(0, &top, &bottom));
    const QRectF rect = view->figuresAtLine(0).at(0).rect;
    const QPoint center(int(view->textStart() + rect.center().x()), int(top + rect.center().y()));
    QTest::mouseMove(view->viewport(), center);
    auto *bar = view->viewport()->findChild<QWidget *>(QStringLiteral("FigureBar"));
    QVERIFY(bar && bar->isVisible());
    QVERIFY(view->window()->findChildren<ImageViewer *>().isEmpty());
    // The bar's buttons and the bar between them do not open the picture.
    for (QToolButton *button : bar->findChildren<QToolButton *>())
        QTest::mouseClick(button, Qt::LeftButton);
    QTest::mouseClick(bar, Qt::LeftButton, {}, QPoint(1, 1));
    QVERIFY(view->window()->findChildren<ImageViewer *>().isEmpty());
    // The picture itself does (the figure moved: ask again).
    QTest::mouseMove(view->viewport(), QPoint(3, 3));
    const QRectF now = view->figuresAtLine(0).at(0).rect;
    QVERIFY(view->boundsOfLine(0, &top, &bottom));
    QTest::mouseClick(view->viewport(), Qt::LeftButton, {},
                      QPoint(int(view->textStart() + now.center().x()), int(top + now.center().y())));
    const auto viewers = view->window()->findChildren<ImageViewer *>();
    QCOMPARE(viewers.size(), 1);
    QCOMPARE(viewers.first()->size().isEmpty(), false);
    viewers.first()->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
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
    QCOMPARE(bool(rendered->pictureAt(QPointF(20, 20 + rendered->stripHeight()))), loads);
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
        const auto picture = rendered->pictureAt(QPointF(20, 20 + rendered->stripHeight()));
        QVERIFY(picture);
        QCOMPARE(rendered->pictureImage(*picture).isNull(), !loads);
    } else if (source.contains(QLatin1String("disguised"))) {
        // A .png by name: the picture is listed, but nothing was drawn (above).
    } else if (source.contains(QLatin1String("img")) || markdown) {
        // A click on a picture that did not load must not open it either.
        QCOMPARE(bool(rendered->pictureAt(QPointF(20, 20 + rendered->stripHeight()))), loads);
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

    ThreadCpuTimer timer; // (Time worked, not time a busy machine kept it waiting.)
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
    ThreadCpuTimer timer;
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

// The room between paragraphs (a blank line's gap row) is body text's: the
// text font times the Markdown line spacing, at the view's zoom. The editor's
// spacing belongs to the source and never reaches this view.
void TestVisualView::gapsFollowTheMarkdownLineHeight()
{
    setDocument(QStringLiteral("One\n\nTwo\n\nThree\n\nFour\n"), 6);
    auto *view = static_cast<ProbeView *>(m_view);
    Settings &settings = Settings::instance();
    const QVariant keep = settings.value(key::textLineHeight);
    const auto gap = [&] {
        m_view->grab();
        qreal top = 0, bottom = 0;
        return view->boundsOfLine(1, &top, &bottom) ? bottom - top : -1;
    };
    const auto expected = [&](qreal factor) {
        QFont body = theme::textFont();
        body.setPointSizeF(body.pointSizeF() * m_view->font().pointSizeF() / theme::editorFont().pointSizeF());
        return QFontMetricsF(body).height() * factor * 0.6;
    };
    settings.setValue(key::textLineHeight, 1.2);
    const qreal tight = gap();
    QVERIFY2(qAbs(tight - expected(1.2)) < 1.5, qPrintable(QString::number(tight)));
    settings.setValue(key::textLineHeight, 2.0);
    const qreal loose = gap();
    QVERIFY2(qAbs(loose - expected(2.0)) < 1.5, qPrintable(QString::number(loose)));
    QVERIFY(loose > tight + 2);
    settings.setValue(key::textLineHeight, keep);
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
            // A font without the letters of the line itself (a symbol font,
            // the app's Visual Notepad Depth): the plain line would be in
            // another font. (QFontMetrics::inFont counts fallback fonts.)
            const QRawFont raw = QRawFont::fromFont(probe);
            if (!std::all_of(u"Textandxy", u"Textandxy" + 9, [&](char16_t c) { return raw.supportsCharacter(QChar(c)); }))
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
                // reserved (in whole pixels), two down where the layout rounds
                // a line's ascent and descent up to whole pixels (macOS), so
                // the line has room the font's measure does not show.
                if (growth > needed + (needed > 0 ? 2 : 1) || growth < needed - 2)
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
        part.strip = part.first; // (As in a view: the whole box has its strip.)
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
    // The pointer is not over either (a strip shows under it).
    for (ProbeView *shown : {&reference, &view}) {
        shown->setAttribute(Qt::WA_UnderMouse, false);
        shown->viewport()->setAttribute(Qt::WA_UnderMouse, false);
        shown->setHoverLine(-1);
    }
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

// The sample's blocks: code 19-22, table 24-27, equation 31-33, diagram
// 35-37; the "Goals" section starts on line 7 and the "Install" one on 15.
void TestVisualView::collapsedBlocksAreOneChip()
{
    auto *view = static_cast<ProbeView *>(m_view);
    m_view->grab();
    QCOMPARE(m_view->chipTextAt(24), QString());
    const QString open = m_view->renderedTextAt(24);
    QVERIFY(open.contains(QLatin1String("Platform")));
    QVERIFY(m_view->setFolded(24, true));
    QVERIFY(m_view->isFolded(24));
    m_view->grab();
    // One row for the block, which is not rendered: only the chip is.
    QCOMPARE(view->rowIndexOf(24), view->rowIndexOf(27));
    QCOMPARE(view->firstLineOfRow(view->rowIndexOf(26)), 24);
    QCOMPARE(m_view->renderedTextAt(24), QString());
    {
        // Nothing but the chip is rendered for it (the sample's diagram renders late).
        setDocument(QStringLiteral("Intro\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\nOutro\n"), 0);
        m_view->grab();
        const int renders = m_view->renderCount();
        QVERIFY(m_view->setFolded(2, true));
        m_view->grab();
        QVERIFY2(m_view->renderCount() - renders <= 1, qPrintable(QString::number(m_view->renderCount() - renders)));
        QVERIFY(m_view->setFolded(2, false));
        setDocument(QString::fromUtf8(sample), 0);
        QVERIFY(m_view->setFolded(24, true));
        m_view->grab();
    }
    const QStringList chip = m_view->chipTextAt(24).split(u'\n');
    QCOMPARE(chip.value(0), QStringLiteral("Table · 2 × 3"));
    QCOMPARE(chip.value(1), QStringLiteral("Platform, Status"));
    // A chip paints in the secondary colour of the style, and is not taller than a line of text.
    QVERIFY(view->heightOfRow(view->rowIndexOf(24)) < 60);
    QVERIFY(m_view->setFolded(24, false));
    QCOMPARE(m_view->foldCount(), 0);
    m_view->grab();
    QCOMPARE(m_view->renderedTextAt(24), open);
    // Not collapsible: a paragraph, a list.
    QVERIFY(!m_view->setFolded(9, true));
    QVERIFY(!m_view->setFolded(12, true));
    // Code and equations and diagrams collapse too.
    for (const int line : {19, 31, 35})
        QVERIFY2(m_view->setFolded(line, true), qPrintable(QString::number(line)));
    QVERIFY2(m_view->chipTextAt(19).startsWith(QStringLiteral("Code · python · 2 lines")), qPrintable(m_view->chipTextAt(19)));
    QVERIFY(m_view->chipTextAt(31).startsWith(QLatin1String("Equation")));
    QVERIFY(m_view->chipTextAt(35).startsWith(QLatin1String("Diagram")));
    QVERIFY(view->rowsConsistent());
}

void TestVisualView::foldsFollowEditsAndGoWithTheirBlock()
{
    QVERIFY(m_view->setFolded(24, true));
    m_view->grab();
    // A line above: the fold moves with its block.
    QTextCursor edit(m_doc);
    edit.insertText(QStringLiteral("Before.\n\n"));
    m_model->flush();
    m_view->grab();
    QVERIFY(m_view->isFolded(26));
    QVERIFY(!m_view->isFolded(24));
    QCOMPARE(m_view->foldCount(), 1);
    // A change inside the block (what a replace all in the source half does):
    // the chip stays, its summary is that of the new text.
    QTextCursor inside(m_doc->findBlockByNumber(28));
    inside.movePosition(QTextCursor::EndOfBlock);
    inside.insertText(QStringLiteral("x"));
    m_model->flush();
    m_view->grab();
    QCOMPARE(m_view->foldCount(), 1);
    QVERIFY(m_view->chipTextAt(26).startsWith(QLatin1String("Table")));
    // The block goes: so does its fold.
    QTextCursor remove(m_doc->findBlockByNumber(26));
    remove.setPosition(m_doc->findBlockByNumber(31).position(), QTextCursor::KeepAnchor);
    remove.removeSelectedText();
    m_model->flush();
    m_view->grab();
    QCOMPARE(m_view->foldCount(), 0);
    // Changed into something else (a paragraph): the same.
    // (Two lines were added above, and the table is gone: the code is on line 21.)
    QVERIFY(m_view->setFolded(21, true));
    m_view->grab();
    QTextCursor change(m_doc->findBlockByNumber(21));
    change.setPosition(m_doc->findBlockByNumber(21).position() + 3, QTextCursor::KeepAnchor);
    change.removeSelectedText(); // "python" stays, the fence is gone.
    m_model->flush();
    m_view->grab();
    QVERIFY(!m_view->isFolded(21));
}

void TestVisualView::foldedSectionsHideTheirBlocks()
{
    auto *view = static_cast<ProbeView *>(m_view);
    // Away from the section, so the caret does not open it.
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(4)));
    QVERIFY(m_view->setFolded(15, true));
    m_view->grab();
    QCOMPARE(m_view->foldHintAt(15), QStringLiteral("· 1 paragraph, 1 code block, 1 table, 1 quote, 1 equation, 1 figure hidden"));
    // The heading's row runs over every line of the section: the spans have no rows of their own.
    QCOMPARE(view->rowIndexOf(15), view->rowIndexOf(37));
    QCOMPARE(view->rowIndexOf(24), view->rowIndexOf(15));
    QVERIFY(view->rowsConsistent());
    QVERIFY(m_view->renderedTextAt(24).contains(QLatin1String("Install")));
    // A chip inside it is kept for when it opens.
    QVERIFY(m_view->setFolded(24, true));
    QCOMPARE(m_view->foldCount(), 2);
    // The scroll position still maps lines to places.
    m_view->scrollToLine(30);
    QVERIFY(m_view->topLinePosition() >= 15);
    // A jump into it (find, go to line, the outline) opens it.
    m_view->clearFocus();
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(31)));
    QVERIFY(m_view->isFolded(15));
    m_view->revealCaret();
    QVERIFY(!m_view->isFolded(15));
    QVERIFY(m_view->isFolded(24)); // (Not the chip, which the caret is not in.)
    m_view->grab();
    QCOMPARE(m_view->foldHintAt(15), QString());
    QVERIFY(view->rowIndexOf(15) != view->rowIndexOf(31));
    // Expand all opens blocks and sections.
    QVERIFY(m_view->setFolded(15, true));
    m_view->expandAll();
    QCOMPARE(m_view->foldCount(), 0);
}

void TestVisualView::caretsOpenFoldsOnlyWithFocus()
{
    // The caret comes into a collapsed block with the focus: it opens.
    QVERIFY(m_view->setFolded(24, true));
    QVERIFY(m_view->hasFocus());
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(26)));
    QVERIFY(!m_view->isFolded(24));
    // At the very start of the block it is on the chip, which stays.
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(4)));
    QVERIFY(m_view->setFolded(24, true));
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(24)));
    QVERIFY(m_view->isFolded(24));
    // A caret mirrored from the other half does not open it (the source
    // half's carets go where they like).
    TextView source(m_doc);
    source.setTextCursor(QTextCursor(m_doc->findBlockByNumber(26)));
    m_view->clearFocus();
    QVERIFY(!m_view->hasFocus());
    m_view->mirrorCursorsFrom(&source, true);
    QCOMPARE(m_view->textCursor().blockNumber(), 26);
    QVERIFY(m_view->isFolded(24));
    m_view->grab();
    QVERIFY(m_view->isFolded(24));
    // The chip takes no typing; Enter on it opens it.
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(24)));
    m_view->setFocus();
    QTRY_VERIFY(m_view->hasFocus());
    QVERIFY(m_view->isFolded(24));
    const int lines = m_doc->blockCount();
    QTest::keyClicks(m_view, QStringLiteral("x"));
    QCOMPARE(m_doc->blockCount(), lines);
    QCOMPARE(m_doc->findBlockByNumber(24).text(), QStringLiteral("| Platform | Status |"));
    QTest::keyClick(m_view, Qt::Key_Return);
    QVERIFY(!m_view->isFolded(24));
    // The arrows treat a chip as one block: Down from above lands on it, and the next goes past it.
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(17)));
    QVERIFY(m_view->setFolded(19, true));
    QVERIFY(m_view->setFolded(24, true));
    QTest::keyClick(m_view, Qt::Key_Down);
    QCOMPARE(m_view->textCursor().blockNumber(), 19);
    QTest::keyClick(m_view, Qt::Key_Down);
    QCOMPARE(m_view->textCursor().blockNumber(), 24);
    QTest::keyClick(m_view, Qt::Key_Down);
    QCOMPARE(m_view->textCursor().blockNumber(), 29);
    QVERIFY(m_view->isFolded(19) && m_view->isFolded(24));
    QTest::keyClick(m_view, Qt::Key_Up);
    QCOMPARE(m_view->textCursor().blockNumber(), 24);
    QTest::keyClick(m_view, Qt::Key_Up);
    QCOMPARE(m_view->textCursor().blockNumber(), 19);
    QVERIFY(m_view->isFolded(19) && m_view->isFolded(24));
}

void TestVisualView::stripSpaceIsAlwaysReserved()
{
    auto *view = static_cast<ProbeView *>(m_view);
    m_view->grab();
    // Tables, code, equations and diagrams have the room; text has none.
    for (const int line : {19, 24, 31, 35})
        QVERIFY2(view->stripHeightAt(line) > 0, qPrintable(QString::number(line)));
    for (const int line : {9, 12, 29})
        QCOMPARE(view->stripHeightAt(line), 0.0);
    qreal top = 0;
    qreal bottom = 0;
    for (const int line : {19, 24, 31, 35}) {
        m_view->setHoverLine(-1);
        m_view->grab();
        QVERIFY(view->boundsOfLine(line, &top, &bottom));
        const qreal height = bottom - top;
        m_view->setHoverLine(line);
        const QImage hovered = m_view->viewport()->grab().toImage();
        QVERIFY(view->boundsOfLine(line, &top, &bottom));
        QCOMPARE(bottom - top, height);
        // And the strip shows then: the row's top differs from the one without.
        m_view->setHoverLine(-1);
        QVERIFY(hovered != m_view->viewport()->grab().toImage());
    }
}

void TestVisualView::clicksCollapseAndOpen()
{
    auto *view = static_cast<ProbeView *>(m_view);
    for (const bool readOnly : {false, true}) {
        m_view->setReadOnly(readOnly);
        m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(4)));
        m_view->grab();
        // The strip's chevron collapses.
        QRect spot = m_view->foldSpotRect(31);
        QVERIFY(!spot.isNull());
        QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(spot.left() + 3, spot.center().y()));
        QVERIFY(m_view->isFolded(31));
        QVERIFY(view->rowsConsistent());
        m_view->grab();
        // A click on the chip opens it.
        spot = m_view->foldSpotRect(31);
        QVERIFY(!spot.isNull());
        QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, spot.center());
        QVERIFY(!m_view->isFolded(31));
        m_view->grab();
        // A heading's chevron, in the margin, folds its section.
        spot = m_view->foldSpotRect(7);
        QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, spot.center());
        QVERIFY(m_view->isFolded(7));
        m_view->grab();
        QCOMPARE(m_view->foldHintAt(7), QStringLiteral("· 1 paragraph, 1 list hidden"));
        spot = m_view->foldSpotRect(7);
        QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, spot.center());
        QVERIFY(!m_view->isFolded(7));
    }
    // The clicks leave the text as it was.
    QCOMPARE(m_doc->toPlainText(), QString::fromUtf8(sample));
}

void TestVisualView::collapseAllLeavesSectionsAndExpandAllOpensThem()
{
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(4)));
    m_view->collapseAllBlocks();
    // The front matter (shown as a table), code, table, equation, diagram:
    // five blocks, no section.
    QString folded;
    for (const auto &[line, kind] : m_view->foldState())
        folded += QStringLiteral("%1:%2 ").arg(line).arg(int(kind));
    QVERIFY2(m_view->foldCount() == 5, qPrintable(folded));
    for (const int line : {0, 19, 24, 31, 35})
        QVERIFY(m_view->isFolded(line));
    QVERIFY(!m_view->isFolded(7));
    QVERIFY(!m_view->isFolded(15));
    QVERIFY(m_view->setFolded(15, true));
    m_view->collapseAllBlocks();
    QCOMPARE(m_view->foldCount(), 6);
    m_view->expandAll();
    QCOMPARE(m_view->foldCount(), 0);
    m_view->grab();
    QVERIFY(m_view->renderedTextAt(24).contains(QLatin1String("Platform")));
}

void TestVisualView::copiedTabKeepsItsFolds()
{
    QTemporaryDir dir;
    QFile file(dir.filePath(QStringLiteral("folds.md")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(sample);
    file.close();
    DocumentTab tab;
    QVERIFY(tab.load(file.fileName()));
    tab.setViewMode(DocumentTab::ViewMode::Visual);
    tab.resize(760, 600);
    tab.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tab));
    VisualView *visual = tab.visualView();
    QVERIFY(visual);
    QVERIFY(visual->setFolded(24, true));
    QVERIFY(visual->setFolded(15, true));
    std::unique_ptr<DocumentTab> copy(tab.openAnotherView());
    QVERIFY(copy->visualView());
    QVERIFY(copy->visualView()->isFolded(24));
    QVERIFY(copy->visualView()->isFolded(15));
    QCOMPARE(copy->visualView()->foldCount(), 2);
    // And they are the copy's own.
    copy->visualView()->expandAll();
    QVERIFY(visual->isFolded(24));
}

void TestVisualView::blocksStartCollapsedWhenAsked()
{
    Settings &settings = Settings::instance();
    settings.setValue(key::blocksStartCollapsed, true);
    const auto restore = qScopeGuard([&] { settings.reset(key::blocksStartCollapsed); });
    QTextDocument doc;
    doc.setDocumentLayout(new DocumentLayout(&doc));
    doc.setPlainText(QString::fromUtf8(sample));
    MarkdownModel model(&doc);
    ProbeView view(&model);
    view.resize(760, 900);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.grab();
    QCOMPARE(view.foldCount(), 5);
    // Typed later: expanded.
    QTextCursor end(&doc);
    end.movePosition(QTextCursor::End);
    end.insertText(QStringLiteral("\n| a | b |\n|---|---|\n| 1 | 2 |\n"));
    model.flush();
    view.grab();
    QCOMPARE(view.foldCount(), 5);
    // A document replaced as a whole starts again.
    view.expandAll();
    view.documentReset();
    doc.setPlainText(QString::fromUtf8(sample));
    model.flush();
    view.grab();
    QCOMPARE(view.foldCount(), 5);
}

// ---------------------------------------------------------------------------
// Read mode

void TestVisualView::startReading(const QString &text, int width, int height)
{
    m_view->resize(width, height);
    setDocument(text, 0);
    m_view->setReadOnly(true);
    m_view->setReadMode(true);
    m_view->grab();
}

QRect TestVisualView::segmentOfLine(int line, int *column, int *index)
{
    auto *view = static_cast<ProbeView *>(m_view);
    const md::Screen &screen = m_view->readScreen();
    const int row = view->rowIndexOf(line);
    for (int c = 0; c < int(screen.columns.size()); ++c) {
        for (int i = 0; i < int(screen.columns[c].segments.size()); ++i) {
            if (screen.columns[c].segments[i].row != row)
                continue;
            if (column)
                *column = c;
            if (index)
                *index = i;
            return m_view->readSegmentRect(c, i);
        }
    }
    return {};
}

void TestVisualView::readModeKeepsThePlace()
{
    auto *view = static_cast<ProbeView *>(m_view);
    const QString text = readDocument();
    m_view->resize(760, 600);
    setDocument(text, 0);
    m_view->setReadOnly(true);
    m_view->grab();
    const int line = lineHolding(text, QStringLiteral("Paragraph 12."));
    m_view->scrollToLine(line);
    m_view->grab();
    m_view->setReadMode(true);
    QVERIFY(m_view->showsReadColumns());
    m_view->grab();
    QCOMPARE(m_view->readLineRange().first, line);
    QVERIFY(!m_view->verticalScrollBar()->isVisible());
    // A page on, then out of read mode: the view starts where the screen did.
    m_view->readPageForward();
    const int first = m_view->readLineRange().first;
    QVERIFY(first > line);
    m_view->setReadMode(false);
    m_view->grab();
    QVERIFY(!m_view->showsReadColumns());
    QCOMPARE(view->rowIndexOf(m_view->topLine()), view->rowIndexOf(first));
    // In again, and through changes of layout: the same line first.
    m_view->setReadMode(true);
    m_view->grab();
    QCOMPARE(m_view->readLineRange().first, first);
    m_view->setReadChars(50);
    m_view->grab();
    QCOMPARE(m_view->readLineRange().first, first);
    m_view->resize(1400, 500);
    QTRY_VERIFY(m_view->viewport()->width() > 1300);
    m_view->grab();
    QCOMPARE(m_view->readLineRange().first, first);
    QFont zoomed = m_view->font();
    zoomed.setPointSizeF(zoomed.pointSizeF() * 1.5);
    m_view->setFont(zoomed);
    m_view->grab();
    QCOMPARE(m_view->readLineRange().first, first);
}

void TestVisualView::readColumnsFollowWidthCharactersAndZoom()
{
    // As the view works it out: characters of the text font at the zoom, the
    // room left by the page arrows, 40 px between columns.
    const auto charWidth = [this] {
        QFont body = theme::textFont();
        body.setPointSizeF(body.pointSizeF() * m_view->font().pointSizeF() / theme::editorFont().pointSizeF());
        return QFontMetricsF(body).averageCharWidth();
    };
    const auto fits = [&](int chars) {
        const int room = m_view->viewport()->width() - 2 * 56;
        const int width = qMin(qRound(chars * charWidth()), room);
        return qMax(1, (room + 40) / (width + 40));
    };
    startReading(readDocument(), 1500, 600);
    QCOMPARE(m_view->readMaxColumns(), fits(70));
    QVERIFY(m_view->readMaxColumns() >= 2);
    QCOMPARE(m_view->readColumnCount(), m_view->readMaxColumns());
    QCOMPARE(int(m_view->readScreen().columns.size()), m_view->readColumnCount());
    QCOMPARE(m_view->readColumnRect(0).width(), qRound(70 * charWidth()));
    // Centred: the spare room is the margins.
    const QRect firstColumn = m_view->readColumnRect(0);
    const QRect lastColumn = m_view->readColumnRect(m_view->readColumnCount() - 1);
    QVERIFY(qAbs(firstColumn.left() - (m_view->viewport()->width() - lastColumn.right() - 1)) <= 1);
    QCOMPARE(m_view->readColumnRect(1).left() - firstColumn.right() - 1, 40);
    // The text is laid out at the column's width.
    QCOMPARE(static_cast<ProbeView *>(m_view)->columnWidth(), qreal(firstColumn.width()));

    // Fewer characters: more columns.
    const int at70 = m_view->readMaxColumns();
    m_view->setReadChars(50);
    m_view->grab();
    QCOMPARE(m_view->readMaxColumns(), fits(50));
    QVERIFY(m_view->readMaxColumns() > at70);
    // Zoomed in, characters are wider: fewer.
    const int at50 = m_view->readMaxColumns();
    QFont zoomed = m_view->font();
    zoomed.setPointSizeF(zoomed.pointSizeF() * 1.5);
    m_view->setFont(zoomed);
    m_view->grab();
    QCOMPARE(m_view->readMaxColumns(), fits(50));
    QVERIFY(m_view->readMaxColumns() < at50);
    // A number asked for, but never more than fit.
    m_view->setReadColumns(1);
    QCOMPARE(m_view->readColumnCount(), 1);
    QCOMPARE(int(m_view->readScreen().columns.size()), 1);
    m_view->setReadColumns(9);
    QCOMPARE(m_view->readColumnCount(), m_view->readMaxColumns());
    // A window narrower than a column: one, as wide as the room.
    m_view->setReadColumns(0);
    m_view->resize(400, 600);
    QTRY_VERIFY(m_view->viewport()->width() < 450);
    m_view->grab();
    QCOMPARE(m_view->readMaxColumns(), 1);
    QCOMPARE(m_view->readColumnRect(0).width(), m_view->viewport()->width() - 2 * 56);
}

void TestVisualView::pageTurnsGreyKeptLinesOnlyGoingForward()
{
    Settings::instance().reset(key::readKeptLines);
    const auto restore = qScopeGuard([] { Settings::instance().reset(key::readKeptLines); });
    startReading(readDocument(), 760, 600);
    const md::Screen first = m_view->readScreen();
    QVERIFY(first.atStart && !first.atEnd);
    QVERIFY(!first.hasKept);
    auto *back = m_view->viewport()->findChild<QAbstractButton *>(QStringLiteral("ReadPageBack"));
    auto *forward = m_view->viewport()->findChild<QAbstractButton *>(QStringLiteral("ReadPageForward"));
    QVERIFY(back && forward);
    QTRY_VERIFY(!back->isEnabled());
    QVERIFY(forward->isEnabled());

    // Forward: the last lines of the page before stay on top, marked by a line.
    m_view->readPageForward();
    const md::Screen second = m_view->readScreen();
    QVERIFY(second.hasKept);
    QVERIFY(second.start < first.end);
    QVERIFY(second.end > first.end);
    const md::Segment &top = second.columns[0].segments[0];
    QVERIFY(top.keptTo > top.yFrom);
    QVERIFY(back->isEnabled());
    // A click (the caret moves, the rows are looked at again) leaves them so.
    const QRect below = m_view->readSegmentRect(0, int(second.columns[0].segments.size()) - 1);
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(below.left() + 4, below.top() + 4));
    m_view->grab();
    QVERIFY(m_view->readScreen().hasKept);
    QCOMPARE(m_view->readScreen().start, second.start);
    // Painted as any text, with a thin line under them.
    m_view->grab();
    const QImage image = m_view->viewport()->grab().toImage();
    if (const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR"); !dir.isEmpty())
        image.save(dir + QStringLiteral("/read-mode-kept.png"));
    int keptBottom = -1;
    for (int i = 0; i < int(second.columns[0].segments.size()); ++i) {
        const md::Segment &s = second.columns[0].segments[i];
        if (s.keptTo > s.yFrom)
            keptBottom = m_view->readSegmentRect(0, i).top() + qMin(s.keptTo, s.yTo) - s.yFrom;
    }
    QVERIFY(keptBottom > 0);
    const QRect column = m_view->readColumnRect(0);
    QCOMPARE(QColor(image.pixel(column.left() + 2, keptBottom - 1)), theme::color(theme::Role::divider));
    // The text above it is as dark as the text below: not greyed.
    const auto darkest = [&](int from, int to) {
        int dark = 255;
        for (int y = from; y < to; ++y)
            for (int x = column.left(); x < column.right(); ++x)
                dark = qMin(dark, qGray(image.pixel(x, y)));
        return dark;
    };
    QVERIFY(qAbs(darkest(column.top(), keptBottom - 2) - darkest(keptBottom, keptBottom + 60)) < 40);

    // Back: the first screen again, nothing greyed.
    m_view->readPageBack();
    QCOMPARE(m_view->readScreen().start, first.start);
    QVERIFY(!m_view->readScreen().hasKept);
    // On again and a nudge: shown as any line.
    m_view->readPageForward();
    QVERIFY(m_view->readScreen().hasKept);
    m_view->readNudge(1);
    QVERIFY(!m_view->readScreen().hasKept);
    // A jump too.
    m_view->readPageForward();
    QVERIFY(m_view->readScreen().hasKept);
    m_view->readToStart();
    QVERIFY(!m_view->readScreen().hasKept);
    QCOMPARE(m_view->readScreen().start, first.start);

    // None kept: the next page starts where this one ended.
    Settings::instance().setValue(key::readKeptLines, 0);
    m_view->readPageForward();
    QVERIFY(!m_view->readScreen().hasKept);
    QCOMPARE(m_view->readScreen().start, first.end);

    // The last screen: forward goes nowhere.
    m_view->readToEnd();
    const md::Screen last = m_view->readScreen();
    QVERIFY(last.atEnd);
    m_view->readPageForward();
    QCOMPARE(m_view->readScreen().start, last.start);
    QTRY_VERIFY(!forward->isEnabled());
}

void TestVisualView::readNudgesByButtonsWheelAndKeys()
{
    startReading(readDocument(), 760, 600);
    m_view->readPageForward();
    m_view->readNudge(1); // (No kept lines to tell the screens apart.)
    const md::Position s0 = m_view->readScreen().start;
    m_view->readNudge(1);
    const md::Position s1 = m_view->readScreen().start;
    QVERIFY(s1 > s0);
    m_view->readNudge(-1);
    QCOMPARE(m_view->readScreen().start, s0);
    const auto start = [this] { return m_view->readScreen().start; };

    // The line buttons.
    auto *up = m_view->viewport()->findChild<QAbstractButton *>(QStringLiteral("ReadLineUp"));
    auto *down = m_view->viewport()->findChild<QAbstractButton *>(QStringLiteral("ReadLineDown"));
    QVERIFY(up && down && up->isVisible() && down->isVisible());
    QVERIFY(!up->toolTip().isEmpty() && !down->toolTip().isEmpty());
    QTest::mouseClick(down, Qt::LeftButton);
    QCOMPARE(start(), s1);
    QTest::mouseClick(up, Qt::LeftButton);
    QCOMPARE(start(), s0);
    // They cover no text: above the first column, below the last.
    QVERIFY(up->geometry().bottom() < m_view->readColumnRect(0).top());
    QVERIFY(down->geometry().top() > m_view->readColumnRect(0).bottom());

    // A notch of the wheel is a line; a touchpad's pixels add up to one.
    const auto notch = [this](int dy) {
        const QPointF pos(300, 200);
        QWheelEvent event(pos, m_view->viewport()->mapToGlobal(pos), QPoint(), QPoint(0, dy), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(m_view->viewport(), &event);
    };
    notch(-120);
    QCOMPARE(start(), s1);
    notch(60);
    QCOMPARE(start(), s1);
    notch(60);
    QCOMPARE(start(), s0);
    const auto pixels = [this](int dy) {
        const QPointF pos(300, 200);
        QWheelEvent event(pos, m_view->viewport()->mapToGlobal(pos), QPoint(0, dy), QPoint(0, dy * 8), Qt::NoButton,
                          Qt::NoModifier, Qt::ScrollUpdate, false);
        QApplication::sendEvent(m_view->viewport(), &event);
    };
    int moved = 0;
    for (int i = 0; i < 100 && start() == s0; ++i, moved += 2)
        pixels(-2);
    QCOMPARE(start(), s1);
    QVERIFY2(moved > 10, qPrintable(QString::number(moved))); // About a line of text.
    for (int i = 0; i < moved / 2; ++i)
        pixels(2);
    QCOMPARE(start(), s0);

    // The keys.
    QTest::keyClick(m_view, Qt::Key_Down);
    QCOMPARE(start(), s1);
    QTest::keyClick(m_view, Qt::Key_Up);
    QCOMPARE(start(), s0);
    QTest::keyClick(m_view, Qt::Key_Down, Qt::ControlModifier);
    QCOMPARE(start(), s1);
    QTest::keyClick(m_view, Qt::Key_Up, Qt::ControlModifier);
    QCOMPARE(start(), s0);
    m_view->readPageForward();
    const md::Position next = start();
    m_view->readPageBack();
    QCOMPARE(start(), s0);
    for (const auto &[key, mods] : {std::pair{Qt::Key_PageDown, Qt::NoModifier}, std::pair{Qt::Key_Space, Qt::NoModifier},
                                    std::pair{Qt::Key_Right, Qt::NoModifier}, std::pair{Qt::Key_PageDown, Qt::AltModifier}}) {
        QTest::keyClick(m_view, key, mods);
        QCOMPARE(start(), next);
        m_view->readPageBack();
        QCOMPARE(start(), s0);
    }
    for (const auto &[key, mods] : {std::pair{Qt::Key_PageUp, Qt::NoModifier}, std::pair{Qt::Key_Space, Qt::ShiftModifier},
                                    std::pair{Qt::Key_Left, Qt::NoModifier}, std::pair{Qt::Key_PageUp, Qt::AltModifier}}) {
        m_view->readPageForward();
        QCOMPARE(start(), next);
        QTest::keyClick(m_view, key, mods);
        QCOMPARE(start(), s0);
    }
    QTest::keyClick(m_view, Qt::Key_End, Qt::ControlModifier);
    QVERIFY(m_view->readScreen().atEnd);
    QTest::keyClick(m_view, Qt::Key_Home, Qt::ControlModifier);
    QVERIFY(m_view->readScreen().atStart);
    // The page arrows, and what TextView::scrollPage (Alt+PgDn) does.
    auto *forward = m_view->viewport()->findChild<QAbstractButton *>(QStringLiteral("ReadPageForward"));
    auto *back = m_view->viewport()->findChild<QAbstractButton *>(QStringLiteral("ReadPageBack"));
    QTest::mouseClick(forward, Qt::LeftButton);
    QVERIFY(!m_view->readScreen().atStart);
    QTest::mouseClick(back, Qt::LeftButton);
    QVERIFY(m_view->readScreen().atStart);
    m_view->scrollPage(true);
    QVERIFY(!m_view->readScreen().atStart);
    m_view->scrollPage(false);
    QVERIFY(m_view->readScreen().atStart);
    // Ctrl+wheel still zooms.
    QSignalSpy zoom(m_view, &TextView::zoomRequested);
    const QPointF pos(300, 200);
    QWheelEvent ctrl(pos, m_view->viewport()->mapToGlobal(pos), QPoint(), QPoint(0, 120), Qt::NoButton,
                     Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(m_view->viewport(), &ctrl);
    QCOMPARE(zoom.count(), 1);
    QVERIFY(m_view->readScreen().atStart);
}

void TestVisualView::longTablesRepeatTheirHeader()
{
    auto *view = static_cast<ProbeView *>(m_view);
    const QString text = longDocument(); // A 60-row table on lines 2 to 63.
    startReading(text, 760, 500);
    const int tableRow = view->rowIndexOf(2);
    // Taller than a column: it goes on in the next one (page) with its header.
    int column = -1;
    int index = -1;
    for (int page = 0; page < 6 && column < 0; ++page) {
        const md::Screen &screen = m_view->readScreen();
        for (int c = 0; c < int(screen.columns.size()) && column < 0; ++c) {
            for (int i = 0; i < int(screen.columns[c].segments.size()); ++i) {
                const md::Segment &s = screen.columns[c].segments[i];
                if (s.row == tableRow && s.repeatHeader) {
                    column = c;
                    index = i;
                    break;
                }
            }
        }
        if (column < 0)
            m_view->readPageForward();
    }
    QVERIFY(column >= 0);
    const md::Segment s = m_view->readScreen().columns[column].segments[index];
    // Cut between body rows, two at least on either side of it.
    const QRect body = m_view->readSegmentRect(column, index);
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(body.left() + 12, body.top() + 3));
    const int firstShown = m_view->textCursor().blockNumber();
    QVERIFY2(firstShown >= 6 && firstShown <= 63 - 1, qPrintable(QString::number(firstShown)));
    QVERIFY(s.yFrom > 0);
    // The header is painted above the part, and a click on it is a click on it.
    m_view->grab();
    const QImage image = m_view->viewport()->grab().toImage();
    if (const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR"); !dir.isEmpty())
        image.save(dir + QStringLiteral("/read-mode-table.png"));
    int ink = 0;
    for (int y = body.top() - 12; y < body.top(); ++y)
        for (int x = body.left(); x < body.left() + 100; ++x)
            ink += QColor(image.pixel(x, y)) != theme::color(theme::Role::editor);
    QVERIFY(ink > 20);
    const md::Position shown = m_view->readScreen().start;
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(body.left() + 12, body.top() - 8));
    QCOMPARE(m_view->textCursor().blockNumber(), 2);
    QCOMPARE(m_view->readScreen().start, shown); // (No page turn to the caret a click put there.)
    // From the table's top a line down is its first body row, under its header.
    m_view->readToStart();
    while (m_view->readScreen().start.row < tableRow)
        m_view->readNudge(1);
    QCOMPARE(m_view->readScreen().start, md::Position({tableRow, 0}));
    QCOMPARE(m_view->readLineRange().first, 2);
    m_view->readNudge(1);
    QVERIFY(m_view->readScreen().columns[0].segments[0].repeatHeader);
    QCOMPARE(m_view->readLineRange().first, 4);
    // The range starts at the first row shown (not the one before, whose
    // line ends a border's width below the cut), and a change of layout
    // starts the screen at that row again.
    m_view->readNudge(1);
    const int first = m_view->readLineRange().first;
    const md::Segment &part = m_view->readScreen().columns[0].segments[0];
    QCOMPARE(part.row, tableRow);
    const QRect top = m_view->readSegmentRect(0, 0);
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(top.left() + 12, top.top() + 4));
    QCOMPARE(m_view->textCursor().blockNumber(), first);
    QFont zoomed = m_view->font();
    zoomed.setPointSizeF(zoomed.pointSizeF() * 1.5);
    m_view->setFont(zoomed);
    m_view->grab();
    QCOMPARE(m_view->readLineRange().first, first);
    // The buttons follow the columns.
    auto *up = m_view->viewport()->findChild<QAbstractButton *>(QStringLiteral("ReadLineUp"));
    QTRY_COMPARE(up->x(), m_view->readColumnRect(0).left() - 4);
}

void TestVisualView::readFiguresFitTheColumnHeight()
{
    QTemporaryDir dir;
    savePicture(dir, QStringLiteral("tall.png"), 300, 2000);
    m_view->setBaseDirectory(dir.path());
    startReading(QStringLiteral("Text\n\n![tall](tall.png)\n\nMore text\n"), 1200, 800);
    auto *view = static_cast<ProbeView *>(m_view);
    const QRect column = m_view->readColumnRect(0);
    const RenderedBlock::Figure figure = view->figuresAtLine(2).at(0);
    // Three quarters of the column (in 16 px steps), not of the view.
    QVERIFY(figure.rect.height() <= column.height() * 0.75);
    QVERIFY(figure.rect.height() > column.height() * 0.75 - 17);
    QVERIFY(figure.rect.width() <= column.width());
    // In one piece on the screen.
    int c = -1;
    int i = -1;
    const QRect shown = segmentOfLine(2, &c, &i);
    QVERIFY(!shown.isNull());
    QCOMPARE(m_view->readScreen().columns[c].segments[i].yFrom, 0);
    QVERIFY(shown.height() >= figure.rect.height());
}

void TestVisualView::findAndGoToLineShowTheirScreen()
{
    const QString text = readDocument();
    startReading(text, 760, 600);
    const int line = lineHolding(text, QStringLiteral("Paragraph 30."));
    QVERIFY(m_view->readLineRange().second < line);
    // Find selects the match: the screen that starts with its line.
    QTextCursor match(m_doc->findBlockByNumber(line));
    match.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor);
    m_view->setTextCursor(match);
    QCOMPARE(m_view->readLineRange().first, line);
    // A match on this screen already: nothing moves; nor for a selection
    // that is partly on it (Select All).
    const md::Position start = m_view->readScreen().start;
    const int shown = m_view->readLineRange().second;
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(shown)));
    QCOMPARE(m_view->readScreen().start, start);
    m_view->selectAll();
    QCOMPARE(m_view->readScreen().start, start);
    // Go to line (and the outline) put the caret there and centre it.
    const int other = lineHolding(text, QStringLiteral("Paragraph 3."));
    QTextCursor caret(m_doc->findBlockByNumber(other));
    m_view->setTextCursor(caret);
    m_view->revealCaret();
    m_view->centerCursor();
    QCOMPARE(m_view->readLineRange().first, other);
    // A line in a folded section: the section opens and the screen shows it.
    const int heading = lineHolding(text, QStringLiteral("## Part 4"));
    const int inside = lineHolding(text, QStringLiteral("Paragraph 27."));
    QVERIFY(m_view->setFolded(heading, true));
    m_view->grab();
    m_view->setTextCursor(QTextCursor(m_doc->findBlockByNumber(inside)));
    m_view->revealCaret();
    m_view->centerCursor();
    QVERIFY(!m_view->isFolded(heading));
    const auto range = m_view->readLineRange();
    QVERIFY(range.first <= inside && inside <= range.second);
}

void TestVisualView::readLineRangeIsTheScreens()
{
    const QString text = readDocument();
    startReading(text, 1500, 600);
    // From the first line to the last one the screen shows.
    QCOMPARE(m_view->readLineRange().first, 0);
    const md::Screen &screen = m_view->readScreen();
    const md::Segment &last = screen.columns.back().segments.back();
    auto *view = static_cast<ProbeView *>(m_view);
    // (Its last row may be cut: the range ends in it, or at its end.)
    const int lastRow = last.row;
    const int lastLine = m_view->readLineRange().second;
    QVERIFY(view->rowIndexOf(lastLine) == lastRow || view->rowIndexOf(lastLine) == lastRow - 1);
    // The last screen ends with the last line that shows anything.
    m_view->readToEnd();
    QCOMPARE(m_view->readLineRange().second, lineHolding(text, QStringLiteral("The end.")));
    // Out of read mode: none.
    m_view->setReadMode(false);
    QCOMPARE(m_view->readLineRange(), std::make_pair(-1, -1));
}

void TestVisualView::readModeClicks()
{
    QTemporaryDir dir;
    savePicture(dir, QStringLiteral("pic.png"), 240, 140);
    m_view->setBaseDirectory(dir.path());
    const QString text = QStringLiteral("A paragraph with [a link](https://example.com/) in it.\n\n"
                                        "| a | b |\n|---|---|\n| 1 | 2 |\n\n![pic](pic.png)\n\nThe end.\n");
    startReading(text, 1500, 600);
    // The link: a pointing hand over it, and a click follows it.
    QSignalSpy links(m_view, &VisualView::linkActivated);
    const QRect paragraph = segmentOfLine(0);
    QVERIFY(!paragraph.isNull());
    int x = -1;
    for (int probe = paragraph.left(); probe < paragraph.right() && x < 0; probe += 3) {
        QTest::mouseMove(m_view->viewport(), QPoint(probe, paragraph.top() + 8));
        if (m_view->viewport()->cursor().shape() == Qt::PointingHandCursor)
            x = probe;
    }
    QVERIFY(x > paragraph.left());
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(x, paragraph.top() + 8));
    QCOMPARE(links.count(), 1);
    QCOMPARE(links.first().first().toString(), QStringLiteral("https://example.com/"));
    // A click on text puts the caret there; a drag selects.
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(paragraph.left() + 3, paragraph.top() + 8));
    QCOMPARE(m_view->textCursor().blockNumber(), 0);
    QTest::mousePress(m_view->viewport(), Qt::LeftButton, {}, QPoint(paragraph.left() + 1, paragraph.top() + 8));
    QTest::mouseMove(m_view->viewport(), QPoint(x - 10, paragraph.top() + 8));
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, QPoint(x - 10, paragraph.top() + 8));
    QVERIFY(m_view->textCursor().hasSelection());
    QVERIFY(m_view->selectedText().startsWith(QLatin1String("A paragraph")));

    // The table's strip folds it, and a click on its chip opens it.
    m_view->setHoverLine(2);
    m_view->grab();
    QRect spot = m_view->foldSpotRect(2);
    QVERIFY(!spot.isNull());
    QVERIFY(m_view->isFoldSpot(QPoint(spot.left() + 3, spot.center().y())));
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(spot.left() + 3, spot.center().y()));
    QVERIFY(m_view->isFolded(2));
    m_view->grab();
    spot = m_view->foldSpotRect(2);
    QVERIFY(!spot.isNull());
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, spot.center());
    QVERIFY(!m_view->isFolded(2));
    m_view->grab();

    // The picture opens in the viewer.
    const int pictureLine = lineHolding(text, QStringLiteral("![pic]"));
    const QRect picture = segmentOfLine(pictureLine);
    QVERIFY(!picture.isNull());
    const RenderedBlock::Figure figure = static_cast<ProbeView *>(m_view)->figuresAtLine(pictureLine).at(0);
    QVERIFY(m_view->window()->findChildren<ImageViewer *>().isEmpty());
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {},
                      picture.topLeft() + figure.rect.center().toPoint() + QPoint(0, 10));
    const auto viewers = m_view->window()->findChildren<ImageViewer *>();
    QCOMPARE(viewers.size(), 1);
    viewers.first()->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    // Pointing at it shows its bar, at its corner in the column.
    QTest::mouseMove(m_view->viewport(), picture.topLeft() + figure.rect.center().toPoint());
    auto *bar = m_view->viewport()->findChild<QWidget *>(QStringLiteral("FigureBar"));
    QVERIFY(bar && bar->isVisible());
    QVERIFY(bar->geometry().right() <= picture.left() + figure.rect.right() + 1);
    QVERIFY(bar->geometry().left() >= picture.left());
    QCOMPARE(m_doc->toPlainText(), text);
}

void TestVisualView::overflowIsOnlyForReadMode()
{
    auto *view = static_cast<ProbeView *>(m_view);
    const QString text = wideDocument();
    const int table = lineHolding(text, QStringLiteral("| Run"));
    const int formula = lineHolding(text, QStringLiteral("$$"));
    const int narrow = lineHolding(text, QStringLiteral("| Stage"));
    // The scrolling view fits both to the column, as it always did.
    m_view->resize(1000, 1000);
    setDocument(text, 0);
    m_view->grab();
    QVERIFY(!view->rendersOverflow());
    const qreal column = view->columnWidth();
    QVERIFY(view->contentWidthOfLine(table) <= column + 0.5);
    QVERIFY(view->tableRightOfLine(table) <= column + 1);
    QVERIFY(view->contentWidthOfLine(formula) <= column + 0.5);
    QVERIFY(view->readOpenRect(table).isNull());
    // Read mode keeps the formula's size and the table's natural width.
    startReading(text, 1000, 1000);
    QVERIFY(view->rendersOverflow());
    const qreal wide = view->columnWidth();
    QVERIFY(view->contentWidthOfLine(table) > wide + 20);
    QVERIFY(qAbs(view->contentWidthOfLine(table) - view->tableRightOfLine(table)) < 2);
    QVERIFY(view->contentWidthOfLine(formula) > wide + 20);
    // A table that fits is as before: no scrolling, no room for a scrollbar.
    QVERIFY(view->contentWidthOfLine(narrow) <= wide + 0.5);
    QVERIFY(view->tableRightOfLine(narrow) <= wide + 1);
    // The room for the scrollbar is the block's own: the height of the
    // wide table is more than the squeezed one's would be.
    const qreal bar = view->blockOfLine(table)->scrollTrack(wide).height();
    QVERIFY(bar > 0);
    QVERIFY(view->blockOfLine(narrow)->scrollTrack(wide).isNull());
    // The whole of the table, at natural width, for the viewer.
    QCOMPARE(view->blockOfLine(table)->contentSize().width(), view->contentWidthOfLine(table));
    // Out of read mode, as before.
    m_view->setReadMode(false);
    QVERIFY(!view->rendersOverflow());
    QVERIFY(view->contentWidthOfLine(table) <= view->columnWidth() + 0.5);
    QVERIFY(view->contentWidthOfLine(formula) <= view->columnWidth() + 0.5);
}

void TestVisualView::wideBlocksScrollSideways()
{
    auto *view = static_cast<ProbeView *>(m_view);
    const QString text = wideDocument();
    const int table = lineHolding(text, QStringLiteral("| Run"));
    const int formula = lineHolding(text, QStringLiteral("$$"));
    startReading(text, 1000, 1000);
    const auto start = [this] { return m_view->readScreen().start; };
    const auto wheel = [this](const QPoint &at, QPoint pixels, QPoint angle, Qt::KeyboardModifiers mods) {
        QWheelEvent event(at, m_view->viewport()->mapToGlobal(at), pixels, angle, Qt::NoButton, mods,
                          pixels.isNull() ? Qt::NoScrollPhase : Qt::ScrollUpdate, false);
        QApplication::sendEvent(m_view->viewport(), &event);
    };
    int tableColumn = 0;
    int tableIndex = 0;
    const QRect tableRect = segmentOfLine(table, &tableColumn, &tableIndex);
    const QRect formulaRect = segmentOfLine(formula);
    QVERIFY(!tableRect.isNull() && !formulaRect.isNull());
    const QPoint overTable = tableRect.center();
    const QPoint overFormula = formulaRect.center();
    const qreal range = view->contentWidthOfLine(table) - view->columnWidth();
    QCOMPARE(view->readOffsetOfLine(table), 0.0);

    // Shift+wheel scrolls the block under the pointer, not the page.
    const md::Position first = start();
    wheel(overTable, {}, QPoint(0, -120), Qt::ShiftModifier);
    const qreal one = view->readOffsetOfLine(table);
    QVERIFY(one > 20);
    QCOMPARE(view->readOffsetOfLine(formula), 0.0);
    QCOMPARE(start(), first);
    wheel(overTable, {}, QPoint(0, 120), Qt::ShiftModifier);
    QCOMPARE(view->readOffsetOfLine(table), 0.0);
    // A horizontal wheel or touchpad swipe does too, each block by itself.
    wheel(overFormula, {}, QPoint(-120, 0), Qt::NoModifier);
    QCOMPARE(view->readOffsetOfLine(table), 0.0);
    QCOMPARE(view->readOffsetOfLine(formula), one);
    wheel(overTable, QPoint(-40, 0), QPoint(-320, 0), Qt::NoModifier);
    QCOMPARE(view->readOffsetOfLine(table), 40.0);
    QCOMPARE(view->readOffsetOfLine(formula), one);
    QCOMPARE(start(), first);
    // A swipe that is mostly vertical is only a nudge.
    wheel(overTable, QPoint(-3, -40), QPoint(-24, -320), Qt::NoModifier);
    QCOMPARE(view->readOffsetOfLine(table), 40.0);
    QVERIFY(!(start() == first));
    m_view->readToStart();
    QCOMPARE(start(), first);
    // A plain vertical wheel still nudges lines (and leaves the offsets).
    const md::Position before = start();
    wheel(overTable, {}, QPoint(0, -120), Qt::NoModifier);
    QVERIFY(!(start() == before));
    QCOMPARE(view->readOffsetOfLine(table), 40.0);
    wheel(overTable, {}, QPoint(0, 120), Qt::NoModifier);
    QCOMPARE(start(), before);
    // Over the text around them, sideways does nothing.
    wheel(QPoint(tableRect.left() + 5, tableRect.top() - 4), {}, QPoint(0, -120), Qt::ShiftModifier);
    QCOMPARE(view->readOffsetOfLine(table), 40.0);
    // To the end and no further, and back.
    for (int i = 0; i < 100; ++i)
        wheel(overTable, {}, QPoint(0, -120), Qt::ShiftModifier);
    QCOMPARE(view->readOffsetOfLine(table), range);
    for (int i = 0; i < 100; ++i)
        wheel(overTable, {}, QPoint(0, 120), Qt::ShiftModifier);
    QCOMPARE(view->readOffsetOfLine(table), 0.0);

    // The thin scrollbar under the table: drawn, and its thumb dragged.
    const QRect bar = m_view->readBarRect(tableColumn, tableIndex);
    QVERIFY(!bar.isNull());
    QVERIFY(bar.height() <= 14);
    QCOMPARE(bar.width(), int(view->columnWidth()));
    QVERIFY(view->readBarRect(tableColumn, tableIndex + 1).isNull());
    const auto pixel = [this](const QPoint &p) { return QColor(m_view->viewport()->grab().toImage().pixel(p)); };
    const QPoint inThumb(bar.left() + 4, bar.top() + 6);
    QCOMPARE(pixel(inThumb), theme::color(theme::Role::panelTitle));
    QCOMPARE(pixel(QPoint(bar.right() - 2, bar.top() + 6)), theme::color(theme::Role::editor));
    QTest::mousePress(m_view->viewport(), Qt::LeftButton, {}, inThumb);
    QTest::mouseMove(m_view->viewport(), QPoint(bar.right() - 1, inThumb.y()));
    QCOMPARE(view->readOffsetOfLine(table), range);
    // (Half the way along, the pointer off the bar: the drag goes on.)
    const qreal thumbWidth = RenderedBlock::scrollThumb(bar, view->contentWidthOfLine(table), 0).width();
    const int half = bar.left() + 4 + int((bar.width() - thumbWidth) / 2);
    QTest::mouseMove(m_view->viewport(), QPoint(half, inThumb.y() + 40));
    QVERIFY(qAbs(view->readOffsetOfLine(table) - range / 2) < 2);
    QTest::mouseRelease(m_view->viewport(), Qt::LeftButton, {}, QPoint(half, inThumb.y() + 40));
    QCOMPARE(m_view->textCursor().blockNumber(), 0); // (No caret was put under the press.)
    const qreal dragged = view->readOffsetOfLine(table);
    QTest::mouseMove(m_view->viewport(), QPoint(bar.left() + 10, inThumb.y())); // (Released: it does not follow.)
    QCOMPARE(view->readOffsetOfLine(table), dragged);
    // A press on the track beside the thumb brings the thumb to it.
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, QPoint(bar.left() + 2, bar.top() + 6));
    QCOMPARE(view->readOffsetOfLine(table), 0.0);

    // The painted table slides: the first column leaves the column's edge.
    QCOMPARE(view->readOffsetOfLine(table), 0.0);
    wheel(overTable, QPoint(-60, 0), QPoint(-480, 0), Qt::NoModifier);
    const QImage slid = m_view->viewport()->grab().toImage();
    // (Cut at the column: nothing of it in the margin left of it.)
    for (int y = tableRect.top() + 12; y < tableRect.bottom() - 16; ++y) {
        for (int x = tableRect.left() - 20; x < tableRect.left(); ++x)
            QCOMPARE(QColor(slid.pixel(x, y)), theme::color(theme::Role::editor));
    }

    // The offsets go with the layout: a new column width starts them again.
    m_view->setReadChars(60);
    m_view->grab();
    QCOMPARE(view->readOffsetOfLine(table), 0.0);
    wheel(segmentOfLine(table).center(), {}, QPoint(0, -120), Qt::ShiftModifier);
    QVERIFY(view->readOffsetOfLine(table) > 0);
    m_view->setReadMode(false);
    m_view->setReadMode(true);
    m_view->grab();
    QCOMPARE(view->readOffsetOfLine(table), 0.0);
}

void TestVisualView::repeatedHeaderFollowsTheOffset()
{
    auto *view = static_cast<ProbeView *>(m_view);
    const QString text = wideDocument(60);
    const int table = lineHolding(text, QStringLiteral("| Run"));
    startReading(text, 760, 500);
    const int tableRow = view->rowIndexOf(table);
    // Page on to the part that goes on with the header repeated.
    int column = -1;
    int index = -1;
    for (int page = 0; page < 8 && column < 0; ++page) {
        const md::Screen &screen = m_view->readScreen();
        for (int c = 0; c < int(screen.columns.size()) && column < 0; ++c) {
            for (int i = 0; i < int(screen.columns[c].segments.size()); ++i) {
                const md::Segment &s = screen.columns[c].segments[i];
                if (s.row == tableRow && s.repeatHeader) {
                    column = c;
                    index = i;
                    break;
                }
            }
        }
        if (column < 0)
            m_view->readPageForward();
    }
    QVERIFY(column >= 0);
    const QRect body = m_view->readSegmentRect(column, index);
    const int width = int(view->columnWidth());
    const auto header = [&] {
        return m_view->viewport()->grab().toImage().copy(body.left(), body.top() - 18, width, 14);
    };
    const QImage still = header();
    QWheelEvent event(body.center(), m_view->viewport()->mapToGlobal(body.center()), QPoint(-40, 0), QPoint(-320, 0),
                      Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(m_view->viewport(), &event);
    QCOMPARE(view->readOffsetOfLine(table), 40.0);
    const QImage moved = header();
    // The header is the same, 40 pixels along (and not the same where it was).
    int ink = 0;
    for (int y = 0; y < still.height(); ++y) {
        for (int x = 0; x < width - 40; ++x) {
            QCOMPARE(moved.pixel(x, y), still.pixel(x + 40, y));
            ink += QColor(still.pixel(x + 40, y)) != theme::color(theme::Role::editor);
        }
    }
    QVERIFY(ink > 100);
    QVERIFY(moved != still);
}

void TestVisualView::openShowsAWideBlockInTheViewer()
{
    auto *view = static_cast<ProbeView *>(m_view);
    const QString text = wideDocument();
    const int table = lineHolding(text, QStringLiteral("| Run"));
    const int formula = lineHolding(text, QStringLiteral("$$"));
    const int narrow = lineHolding(text, QStringLiteral("| Stage"));
    startReading(text, 1000, 1000);
    const auto viewers = [this] { return m_view->window()->findChildren<BlockViewer *>(); };
    const auto close = [&] {
        for (BlockViewer *v : viewers())
            v->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(viewers().isEmpty());
    };
    // Only wide blocks have the button (and it shows with the strip, when pointed at).
    const QRect tableOpen = m_view->readOpenRect(table);
    const QRect formulaOpen = m_view->readOpenRect(formula);
    QVERIFY(!tableOpen.isNull() && !formulaOpen.isNull());
    QVERIFY(m_view->readOpenRect(narrow).isNull());
    QVERIFY(m_view->readOpenRect(0).isNull()); // (A heading.)
    const QRect tableRect = segmentOfLine(table);
    QVERIFY(tableOpen.right() <= tableRect.right() + 1 && tableOpen.right() > tableRect.right() - 4);
    QVERIFY(tableOpen.bottom() < tableRect.top() + int(view->stripHeightAt(table)));
    const auto inkIn = [&](const QRect &r) {
        const QImage image = m_view->viewport()->grab().toImage();
        int ink = 0;
        for (int y = r.top(); y <= r.bottom(); ++y)
            for (int x = r.left(); x <= r.right(); ++x)
                ink += QColor(image.pixel(x, y)) != theme::color(theme::Role::editor);
        return ink;
    };
    m_view->setHoverLine(-1);
    QCOMPARE(inkIn(tableOpen), 0);
    m_view->setHoverLine(table);
    QVERIFY(inkIn(tableOpen) > 40);
    m_view->setHoverLine(lineHolding(text, QStringLiteral("| Stage")));
    QCOMPARE(inkIn(tableOpen), 0);

    // The button, even slid or while the pointer is elsewhere on the strip.
    QVERIFY(m_view->isFoldSpot(tableOpen.center()));
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, tableOpen.center());
    QCOMPARE(viewers().size(), 1);
    QCOMPARE(viewers().first()->windowTitle(), QStringLiteral("Run log — table"));
    QVERIFY(!m_view->isFolded(table));
    close();
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, formulaOpen.center());
    QCOMPARE(viewers().size(), 1);
    QCOMPARE(viewers().first()->windowTitle(), QStringLiteral("Run log — equation"));
    close();

    // A click on the block where no text is (the formula; the table's padding,
    // beside the text of a cell) opens it too; on the text it puts the caret.
    const QRectF header = *view->blockOfLine(table)->tableCellRect(0, 0);
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {},
                      tableRect.topLeft() + QPoint(int(header.left()) + 12, int(header.center().y())));
    QVERIFY(viewers().isEmpty());
    QCOMPARE(m_view->textCursor().blockNumber(), table);
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {},
                      tableRect.topLeft() + QPoint(int(header.right()) - 3, int(header.center().y())));
    QCOMPARE(viewers().size(), 1);
    QCOMPARE(viewers().first()->windowTitle(), QStringLiteral("Run log — table"));
    close();
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, segmentOfLine(formula).center());
    QCOMPARE(viewers().size(), 1);
    QCOMPARE(viewers().first()->windowTitle(), QStringLiteral("Run log — equation"));
    close();
    // The short table's cell opens nothing.
    const QRect shortRect = segmentOfLine(narrow);
    const QRectF shortCell = *view->blockOfLine(narrow)->tableCellRect(0, 0);
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {},
                      shortRect.topLeft() + QPoint(int(shortCell.right()) - 3, int(shortCell.center().y())));
    QVERIFY(viewers().isEmpty());
    // A wide block with no heading above it: the viewer is named for what it shows.
    startReading(QStringLiteral("| a | b | c | d | c | d | e | f | g | h |\n|---|---|---|---|---|---|---|---|---|---|\n")
                     + QStringLiteral("| %1 |\n").arg(QString(180, u'w')),
                 1000, 600);
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {}, m_view->readOpenRect(0).center());
    QCOMPARE(viewers().size(), 1);
    QCOMPARE(viewers().first()->windowTitle(), QStringLiteral("table"));
    close();
}

namespace {

// How much of the colour changes along rows of a picture is gradual: the share
// of the pixels where the colour changes from the one before that lie in a run
// of more than `run` such pixels. Shapes drawn at a large size change colour
// within a pixel or two; a small picture enlarged smooths its edges over as
// many pixels as it is enlarged.
qreal gradualShare(const QImage &image, int run = 8)
{
    qsizetype changing = 0;
    qsizetype inLongRuns = 0;
    for (int y = 0; y < image.height(); ++y) {
        int length = 0;
        for (int x = 1; x <= image.width(); ++x) {
            const bool changes = x < image.width() && image.pixel(x, y) != image.pixel(x - 1, y);
            if (changes) {
                ++length;
                continue;
            }
            changing += length;
            if (length > run)
                inLongRuns += length;
            length = 0;
        }
    }
    return changing > 0 ? qreal(inLongRuns) / changing : 0;
}

// A table's column, as wide as the grid draws it.
qreal gridColumn(const RenderedBlock::TableGrid &grid, int col)
{
    return grid.xs[col + 1] - grid.xs[col];
}

} // namespace

// A table whose columns cannot all be as wide as their text asks fits the
// column by wrapping the long text (CSS 2.2 17.5.2.2: each column is as narrow
// as its longest word, and shares what is left in proportion to how much wider
// it would like to be): no scrolling, so no Open and no scrollbar.
void TestVisualView::tablesSqueezeBeforeTheyScroll()
{
    auto *view = static_cast<ProbeView *>(m_view);
    const QString text = squeezeDocument();
    const int squeezed = lineHolding(text, QStringLiteral("| Id"));
    startReading(text, 1000, 1000);
    const qreal column = view->columnWidth();

    // The same table with all the room it asks for.
    RenderContext wide;
    wide.font = theme::textFont();
    wide.codeFont = theme::editorFont();
    wide.width = 100000;
    wide.overflow = true;
    std::unique_ptr<RenderedBlock> natural;
    for (const core::Block &block : m_model->blocks()) {
        if (int(block.first_line) == squeezed) {
            const QString source = text.split(u'\n').mid(squeezed, int(block.last_line - block.first_line) + 1).join(u'\n');
            natural = RenderedBlock::create(block, source, wide);
        }
    }
    QVERIFY(natural);
    const RenderedBlock::TableGrid *plain = natural->tableGrid();
    QVERIFY(plain);
    QCOMPARE(plain->cols(), 3);
    QVERIFY(plain->xs.back() > column + 300); // Far too wide for the column as it stands.

    // In the view it takes the column (no wider, and none of it spare), with no scrolling.
    const RenderedBlock *block = view->blockOfLine(squeezed);
    const RenderedBlock::TableGrid *grid = block->tableGrid();
    QVERIFY(grid);
    QCOMPARE(grid->cols(), 3);
    QVERIFY(view->contentWidthOfLine(squeezed) <= column + 0.5);
    QVERIFY(view->tableRightOfLine(squeezed) <= column + 1);
    QVERIFY(view->tableRightOfLine(squeezed) > column - 4);
    QVERIFY(block->scrollTrack(column).isNull());
    QVERIFY(m_view->readOpenRect(squeezed).isNull());
    // The columns of short words are as wide as they were, the long text's
    // column gave up the room, and wrapped (the table is taller).
    QVERIFY(qAbs(gridColumn(*grid, 0) - gridColumn(*plain, 0)) < 1.5);
    QVERIFY(qAbs(gridColumn(*grid, 2) - gridColumn(*plain, 2)) < 1.5);
    QVERIFY(gridColumn(*grid, 1) < gridColumn(*plain, 1) - 300);
    QVERIFY(grid->ys.back() > 2 * plain->ys.back());
    // Words are not broken: the column is as wide as its longest word.
    const QFontMetricsF metrics(theme::textFont());
    QVERIFY(gridColumn(*grid, 1) >= metrics.horizontalAdvance(QStringLiteral("everybody")));
    if (const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR"); !dir.isEmpty())
        m_view->grab().save(dir + QStringLiteral("/read-table-squeezed.png"));
    // Out of read mode nothing changed: Qt squeezes the table as it did.
    m_view->setReadMode(false);
    QVERIFY(view->contentWidthOfLine(squeezed) <= view->columnWidth() + 0.5);
}

// ... and only when even the least widths are too wide does it scroll, at the
// width its text asks for (the table in the same document that does fit does not).
void TestVisualView::tablesScrollWhenTheirLongestWordsDoNotFit()
{
    auto *view = static_cast<ProbeView *>(m_view);
    const QString text = squeezeDocument();
    const int squeezed = lineHolding(text, QStringLiteral("| Id"));
    const int stuck = lineHolding(text, QStringLiteral("| a | b"));
    startReading(text, 1000, 1000);
    const qreal column = view->columnWidth();
    QVERIFY(view->contentWidthOfLine(stuck) > column + 20);
    QVERIFY(qAbs(view->contentWidthOfLine(stuck) - view->tableRightOfLine(stuck)) < 2);
    QVERIFY(!view->blockOfLine(stuck)->scrollTrack(column).isNull());
    // Only that one has the Open button.
    QVERIFY(!m_view->readOpenRect(stuck).isNull());
    QVERIFY(m_view->readOpenRect(squeezed).isNull());
    // The words are not broken: the table is as wide as ten of them side by side.
    const QFontMetricsF metrics(theme::textFont());
    QVERIFY(view->contentWidthOfLine(stuck) > 10 * metrics.horizontalAdvance(QStringLiteral("abcdefghijklmnop")));
}

namespace {

// Opens the viewer of the wide block at `line`, with the button.
BlockViewer *openViewer(VisualView *view, const QRect &open)
{
    const auto before = view->window()->findChildren<BlockViewer *>();
    QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, open.center());
    for (BlockViewer *viewer : view->window()->findChildren<BlockViewer *>()) {
        if (!before.contains(viewer))
            return viewer;
    }
    return nullptr;
}

void closeViewers(QWidget *window)
{
    for (BlockViewer *viewer : window->findChildren<BlockViewer *>())
        viewer->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

} // namespace

void TestVisualView::viewersLeaveTheWindowUsable()
{
    auto *view = static_cast<ProbeView *>(m_view);
    const QString text = wideDocument();
    const int table = lineHolding(text, QStringLiteral("| Run"));
    const int formula = lineHolding(text, QStringLiteral("$$"));
    startReading(text, 1000, 1000);
    const auto viewers = [this] { return m_view->window()->findChildren<BlockViewer *>(); };
    const QRect tableRect = segmentOfLine(table);
    const QRectF header = *view->blockOfLine(table)->tableCellRect(0, 0);

    BlockViewer *first = openViewer(m_view, m_view->readOpenRect(table));
    QVERIFY(first);
    QVERIFY(first->isVisible());
    // A window of its own: not modal, so the one under it takes input.
    QCOMPARE(first->windowModality(), Qt::NonModal);
    QVERIFY(!first->isModal());
    QVERIFY(QApplication::activeModalWidget() == nullptr);
    QVERIFY(m_view->window()->isEnabled());
    QCOMPARE(m_view->textCursor().blockNumber(), 0);
    QTest::mouseClick(m_view->viewport(), Qt::LeftButton, {},
                      tableRect.topLeft() + QPoint(int(header.left()) + 12, int(header.center().y())));
    QCOMPARE(m_view->textCursor().blockNumber(), table); // The click reached the table.
    const md::Position before = m_view->readScreen().start;
    QWheelEvent wheel(tableRect.center(), m_view->viewport()->mapToGlobal(tableRect.center()), {}, QPoint(0, -120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(m_view->viewport(), &wheel);
    QVERIFY(!(m_view->readScreen().start == before)); // ... and reading goes on while it is open.
    QVERIFY(first->isVisible());

    // Several at once, each closing by itself.
    m_view->readToStart();
    BlockViewer *second = openViewer(m_view, m_view->readOpenRect(formula));
    QVERIFY(second && second != first);
    QCOMPARE(viewers().size(), 2);
    second->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(viewers().size(), 1);
    QVERIFY(first->isVisible());
    // Escape closes the one it is pressed in.
    QTest::keyClick(first, Qt::Key_Escape);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(viewers().isEmpty());
}

void TestVisualView::tableViewerIsTheRealTable()
{
    auto *view = static_cast<ProbeView *>(m_view);
    const QString text = wideDocument();
    const int table = lineHolding(text, QStringLiteral("| Run"));
    startReading(text, 1000, 1000);
    const qreal natural = view->contentWidthOfLine(table);
    BlockViewer *viewer = openViewer(m_view, m_view->readOpenRect(table));
    QVERIFY(viewer);
    QCOMPARE(viewer->windowTitle(), QStringLiteral("Run log \u2014 table"));
    // The table itself, as laid out at its natural width, at its own size.
    const RenderedBlock &block = viewer->block();
    QCOMPARE(block.tableCells(), QSize(10, 9));
    QCOMPARE(viewer->scale(), 1.0);
    QCOMPARE(viewer->scaledSize().width(), natural);
    viewer->resize(520, 480); // Narrower than the table.
    auto *area = qobject_cast<QAbstractScrollArea *>(viewer->viewport()->parentWidget());
    QVERIFY(area);
    QVERIFY(area->horizontalScrollBar()->maximum() > 0); // Wider than the window: it scrolls.
    if (const QString dir = qEnvironmentVariable("VN_SCREENSHOT_DIR"); !dir.isEmpty())
        viewer->grab().save(dir + QStringLiteral("/table-viewer.png"));
    QCOMPARE(area->horizontalScrollBar()->maximum(), int(std::ceil(natural)) - viewer->viewport()->width());
    QVERIFY(viewer->selectedText().isEmpty());

    // Drag over text to select it.
    const QPointF from = viewer->toViewport(block.tableCellRect(1, 1)->center());
    const QPointF to = viewer->toViewport(block.tableCellRect(3, 1)->center());
    QTest::mousePress(viewer->viewport(), Qt::LeftButton, {}, from.toPoint());
    QTest::mouseMove(viewer->viewport(), to.toPoint());
    QTest::mouseRelease(viewer->viewport(), Qt::LeftButton, {}, to.toPoint());
    QVERIFY2(viewer->selectedText().contains(QStringLiteral("feature_cache_2_and_its_long_name")),
             qPrintable(viewer->selectedText()));
    // ... and select all, and copy.
    viewer->selectAll();
    QApplication::clipboard()->setText(QString());
    QTest::keyClick(viewer, Qt::Key_C, Qt::ControlModifier);
    const QString copied = QApplication::clipboard()->text();
    QVERIFY(copied.contains(QStringLiteral("#481")));
    QVERIFY(copied.contains(QStringLiteral("feature_cache_8_and_its_long_name")));
    QVERIFY(copied.contains(QStringLiteral("Package")));

    // Zooming lays the table out at the new size (the scrolling follows it),
    // with the same bar as the picture viewer's.
    const QPointF centre(viewer->viewport()->width() / 2.0, viewer->viewport()->height() / 2.0);
    viewer->zoomAt(centre, 2);
    QCOMPARE(viewer->scale(), 2.0);
    QCOMPARE(viewer->scaledSize().width(), 2 * natural);
    QCOMPARE(area->horizontalScrollBar()->maximum(), int(std::ceil(2 * natural)) - viewer->viewport()->width());
    auto *slider = viewer->findChild<QSlider *>();
    QVERIFY(slider && slider->value() > 0);
    QToolButton *fitButton = nullptr;
    QToolButton *actualButton = nullptr;
    for (QToolButton *button : viewer->findChildren<QToolButton *>()) {
        if (button->text() == QLatin1String("Fit"))
            fitButton = button;
        else if (button->text() == QLatin1String("1:1"))
            actualButton = button;
    }
    QVERIFY(fitButton && actualButton);
    fitButton->click();
    QVERIFY(viewer->scale() < 1);
    QVERIFY(qAbs(viewer->scale() - viewer->fitScale()) < 1e-9);
    QCOMPARE(area->horizontalScrollBar()->maximum(), 0); // All of it shows.
    actualButton->click();
    QCOMPARE(viewer->scale(), 1.0);

    // Painted at the size it is shown, not enlarged: at 8 times the edges of
    // letters and borders are as sharp as at 1 (no softness over many pixels).
    viewer->zoomAt(centre, 8);
    const QImage zoomed = viewer->viewport()->grab().toImage();
    QVERIFY(zoomed.size() == viewer->viewport()->size() * zoomed.devicePixelRatio());
    int ink = 0;
    const QColor editor = theme::color(theme::Role::editor);
    const QImage upper = zoomed.copy(0, 0, zoomed.width(), zoomed.height() - 80);
    for (int y = 0; y < upper.height(); y += 3)
        for (int x = 0; x < upper.width(); x += 3)
            ink += QColor(upper.pixel(x, y)) != editor;
    QVERIFY(ink > 200);
    QVERIFY2(gradualShare(upper) < 0.2, qPrintable(QString::number(gradualShare(upper))));
    closeViewers(m_view->window());
}

void TestVisualView::equationViewerIsDrawnAsShapes()
{
    auto *view = static_cast<ProbeView *>(m_view);
    const QString text = wideDocument();
    const int formula = lineHolding(text, QStringLiteral("$$"));
    startReading(text, 1000, 1000);
    BlockViewer *viewer = openViewer(m_view, m_view->readOpenRect(formula));
    QVERIFY(viewer);
    QCOMPARE(viewer->windowTitle(), QStringLiteral("Run log \u2014 equation"));
    QCOMPARE(viewer->block().kind(), RenderedBlock::Kind::Image);
    QCOMPARE(viewer->block().contentSize().width(), std::ceil(view->contentWidthOfLine(formula)));
    const QPointF centre(viewer->viewport()->width() / 2.0, viewer->viewport()->height() / 2.0);
    const QColor editor = theme::color(theme::Role::editor);
    const auto sharp = [&](qreal zoom, qreal *share) {
        viewer->fit();
        viewer->zoomAt(centre, zoom / viewer->scale());
        QCOMPARE(viewer->scale(), zoom);
        QCOMPARE(viewer->scaledSize().width(), zoom * viewer->block().contentSize().width());
        const QImage image = viewer->viewport()->grab().toImage();
        const QImage upper = image.copy(0, 0, image.width(), image.height() - 80);
        int ink = 0;
        for (int y = 0; y < upper.height(); y += 2)
            for (int x = 0; x < upper.width(); x += 2)
                ink += QColor(upper.pixel(x, y)) != editor;
        QVERIFY(ink > 100);
        *share = gradualShare(upper);
    };
    // At 1, 4 and 16 times its size the shapes have the same sharp edges.
    qreal one = 0;
    qreal four = 0;
    qreal sixteen = 0;
    sharp(1, &one);
    sharp(4, &four);
    sharp(16, &sixteen);
    QVERIFY2(four < 0.2 && sixteen < 0.2, qPrintable(QStringLiteral("%1 %2 %3").arg(one).arg(four).arg(sixteen)));
    closeViewers(m_view->window());
}

void TestVisualView::readModeWaitsForTheFirstParse()
{
    // A large document: its source stands in, scrolling, until the parse.
    m_view->hide();
    QTextDocument doc;
    doc.setDocumentLayout(new DocumentLayout(&doc));
    doc.setPlainText(largeDocument(MarkdownModel::backgroundShowChars + 4096));
    auto *model = new MarkdownModel(&doc);
    ProbeView view(model);
    view.resize(600, 500);
    view.setReadOnly(true);
    view.setReadMode(true);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    QVERIFY(model->isAwaitingParse());
    QVERIFY(!view.showsReadColumns());
    QCOMPARE(view.readLineRange(), std::make_pair(-1, -1));
    QVERIFY(!view.grab().isNull());
    view.scrollToLine(101); // (A paragraph: blank lines do not start a column.)
    // The place it scrolled to is where the columns start.
    QTRY_VERIFY_WITH_TIMEOUT(!model->isAwaitingParse(), 20000);
    QTRY_VERIFY((view.grab(), view.showsReadColumns()));
    const auto range = view.readLineRange();
    QVERIFY2(range.first == 101 && 101 <= range.second,
             qPrintable(QStringLiteral("%1-%2").arg(range.first).arg(range.second)));
    QVERIFY(!view.verticalScrollBar()->isVisible());
    view.readPageForward();
    QVERIFY(view.readLineRange().first > range.first);
}

// The renderer draws the lines and backgrounds of a table's cells itself (Qt
// draws them slowly); they look exactly as Qt draws them: at any width,
// fitted in read mode, scaled, with selections, under a clip.
void TestVisualView::tableGridLooksAsQtDrawsIt()
{
    QString longTable = QStringLiteral("| # | Name | Description |\n|---|---|---|\n");
    for (int i = 0; i < 12; ++i)
        longTable += QStringLiteral("| %1 | lorem ipsum | %2 |\n").arg(i).arg(QStringLiteral("dolor sit amet ").repeated(i % 4 + 1));
    const QStringList documents = {
        QStringLiteral("| Left | Centre | Right |\n|:---|:---:|---:|\n| ipsum elit | **ipsum** | 0 |\n| `x1` | do | 137 |\n"),
        QStringLiteral("---\ntitle: Tables\ntags: [a, b]\n---\n"),
        QStringLiteral("| one column |\n|---|\n| x |\n"),
        longTable,
    };
    const auto render = [](const QString &markdown, qreal width, bool overflow) {
        const QByteArray utf8 = markdown.toUtf8();
        // (Front matter as a table.)
        const rust::Vec<core::Block> blocks = core::blocks(rust::Str(utf8.constData(), utf8.size()),
                                                           core::RenderOptions{false, true, true, true, 2});
        RenderContext ctx;
        ctx.font = theme::textFont();
        ctx.codeFont = theme::editorFont();
        ctx.width = width;
        ctx.overflow = overflow;
        return RenderedBlock::create(blocks.at(0), markdown, ctx);
    };
    struct Paint
    {
        qreal scale;
        bool antialiased;
        QRectF clip;
        QPair<int, int> selected;
    };
    const QList<Paint> paints = {{1, false, {}, {0, 0}},          {1.5, true, {}, {0, 0}},
                                 {1, false, {}, {0, 100000}},     {1.25, false, {}, {3, 40}},
                                 {1, false, {0, 40.3, 2000, 61.6}, {0, 0}}, {0.75, true, {20, 0, 300, 2000}, {2, 9}}};
    const auto paint = [](const RenderedBlock &block, const Paint &p) {
        QImage image(QSize(int((block.contentWidth() + 20) * p.scale), int((block.height() + 20) * p.scale)),
                     QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, p.antialiased);
        painter.scale(p.scale, p.scale);
        if (!p.clip.isNull())
            painter.setClipRect(p.clip);
        RenderedSelection selection;
        if (p.selected.second > 0)
            selection.ranges.append(p.selected);
        block.paint(painter, QPointF(7, 5), block.contentWidth(), selection);
        return image;
    };
    for (const QString &markdown : documents) {
        for (const qreal width : {548.0, 200.0}) {
            for (const bool overflow : {false, true}) {
                RenderedBlock::setOwnTableGridEnabled(false);
                const auto qt = render(markdown, width, overflow);
                RenderedBlock::setOwnTableGridEnabled(true);
                const auto own = render(markdown, width, overflow);
                QVERIFY(qt->tableGrid());
                QCOMPARE(own->height(), qt->height());
                QCOMPARE(own->contentWidth(), qt->contentWidth());
                for (const Paint &p : paints) {
                    const QImage expected = paint(*qt, p);
                    const QImage actual = paint(*own, p);
                    QVERIFY2(actual == expected, qPrintable(QStringLiteral("%1 at %2: %3")
                                                                .arg(markdown.left(20))
                                                                .arg(width)
                                                                .arg(imageDifference(actual, expected))));
                }
            }
        }
    }
}

// A block laid out again at a new width (a window-edge drag) is the block
// made at that width: its height, anchors, cuts, grid and pixels. Blocks
// whose more than layout depends on the width are made again.
void TestVisualView::reflowedBlocksAreMadeBlocks()
{
    QString table = QStringLiteral("| # | Name | Description |\n|---|---|---|\n");
    for (int i = 0; i < 8; ++i)
        table += QStringLiteral("| %1 | lorem ipsum | %2 |\n").arg(i).arg(QStringLiteral("dolor sit amet ").repeated(i % 4 + 1));
    const QString words = QStringLiteral("lorem ipsum dolor sit amet consectetur ").repeated(12);
    const QStringList documents = {
        QStringLiteral("A paragraph of %1 with **bold** and `code`.\n").arg(words),
        QStringLiteral("> A quote of %1\n>\n> - with a list\n> - of two items\n").arg(words),
        QStringLiteral("```cpp\nint main() { return 0; } // %1\n```\n").arg(words),
        QStringLiteral("---\ntitle: %1\n---\n").arg(words),
        table,
    };
    const auto make = [](const QString &markdown, qreal width, bool overflow) {
        const QByteArray utf8 = markdown.toUtf8();
        const rust::Vec<core::Block> blocks = core::blocks(rust::Str(utf8.constData(), utf8.size()),
                                                           core::RenderOptions{false, true, true, true, 2});
        RenderContext ctx;
        ctx.font = theme::textFont();
        ctx.codeFont = theme::editorFont();
        ctx.width = width;
        ctx.overflow = overflow;
        return std::pair{RenderedBlock::create(blocks.at(0), markdown, ctx), ctx};
    };
    const auto picture = [](const RenderedBlock &block) {
        QImage image(QSize(int(block.contentWidth()) + 10, int(block.height()) + 10), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        block.paint(painter, QPointF(3, 2), block.contentWidth());
        return image;
    };
    for (const QString &markdown : documents) {
        for (const bool overflow : {false, true}) {
            auto [block, ctx] = make(markdown, 548, overflow);
            for (const qreal width : {300.0, 731.5, 120.0, 548.0}) {
                ctx.width = width;
                const QString what = QStringLiteral("%1 at %2").arg(markdown.left(16)).arg(width);
                QVERIFY2(block->reflow(ctx), qPrintable(what));
                const auto made = make(markdown, width, overflow).first;
                QCOMPARE(block->height(), made->height());
                QCOMPARE(block->contentWidth(), made->contentWidth());
                QVERIFY2(block->lineAnchors() == made->lineAnchors(), qPrintable(what));
                QVERIFY2(block->breaks().cuts == made->breaks().cuts, qPrintable(what));
                QVERIFY2(block->breaks().lineTops == made->breaks().lineTops, qPrintable(what));
                QCOMPARE(bool(block->tableGrid()), bool(made->tableGrid()));
                if (made->tableGrid()) {
                    QVERIFY(block->tableGrid()->xs == made->tableGrid()->xs);
                    QVERIFY(block->tableGrid()->ys == made->tableGrid()->ys);
                }
                const QImage actual = picture(*block), expected = picture(*made);
                QVERIFY2(actual == expected, qPrintable(what + u' ' + imageDifference(actual, expected)));
            }
        }
    }
    // A picture fits the column, a formula shrinks to it: made again.
    QTemporaryDir dir;
    QImage red(400, 100, QImage::Format_RGB32);
    red.fill(Qt::red);
    QVERIFY(red.save(dir.filePath(QStringLiteral("red.png"))));
    for (const QString &markdown : {QStringLiteral("![red](%1)\n").arg(dir.filePath(QStringLiteral("red.png"))),
                                    QStringLiteral("Text and $x^2$.\n")}) {
        auto [block, ctx] = make(markdown, 548, false);
        ctx.width = 300;
        QVERIFY(!block->reflow(ctx));
    }
}

namespace {

// A document of blocks of every kind that makes rows (long lists an item a
// row, a code box in parts, tables, gaps), for the edits of
// patchedRowsEqualWholeRows.
QString rowsDocument()
{
    QString text = QStringLiteral("# Title\n\nIntro paragraph with **bold** text.\nSecond line of it.\n\n## Items\n\n");
    text += listOf(QStringLiteral("- item %1 with `code` and [a link](https://example.com/%1)"), 40);
    text += QStringLiteral("\nBetween the lists.\n\n");
    text += listOf(QStringLiteral("%1. step %1\n   - detail of %1"), 24);
    text += QStringLiteral("\n### Code\n\n```text\n");
    for (int i = 0; i < 450; ++i)
        text += QStringLiteral("line %1 of a long code box\n").arg(i);
    text += QStringLiteral("```\n\n| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n\n> quoted\n> text\n\n");
    for (int i = 0; i < 12; ++i)
        text += QStringLiteral("## Section %1\n\nParagraph %1 of the section.\n\n- one\n- two\n\n").arg(i);
    text += QStringLiteral("<div>raw html</div>\n\nLast paragraph.\n");
    return text;
}

// One random edit of the document: a character typed or taken, a line
// broken or joined, lines added or removed, a block of Markdown pasted.
void randomEdit(QTextDocument *doc, QRandomGenerator &random)
{
    static const QStringList pieces = {
        QStringLiteral("a"), QStringLiteral("x y"), QStringLiteral("*"), QStringLiteral("#"), QStringLiteral("- "),
        QStringLiteral("`"), QStringLiteral("|"), QStringLiteral("\n"), QStringLiteral("\n\n"), QStringLiteral("1. "),
        QStringLiteral("  "), QStringLiteral("```\n"), QStringLiteral("- a\n- b\n\n"), QStringLiteral("para\n\npara 2\n"),
        QStringLiteral("> "), QStringLiteral("# Heading\n\n"),
    };
    QTextCursor c(doc);
    const int line = int(random.bounded(doc->blockCount()));
    const QTextBlock block = doc->findBlockByNumber(line);
    const int position = block.position() + int(random.bounded(block.length()));
    c.setPosition(qMin(position, doc->characterCount() - 1));
    switch (random.bounded(6)) {
    case 0:
    case 1:
    case 2:
        c.insertText(pieces.at(int(random.bounded(pieces.size()))));
        break;
    case 3:
        c.setPosition(qMin(c.position() + int(random.bounded(6)), doc->characterCount() - 1), QTextCursor::KeepAnchor);
        c.removeSelectedText();
        break;
    case 4: {
        // Whole lines removed.
        const int last = qMin(line + int(random.bounded(4)), doc->blockCount() - 1);
        c.setPosition(block.position());
        c.setPosition(doc->findBlockByNumber(last).position() + doc->findBlockByNumber(last).length() - 1,
                      QTextCursor::KeepAnchor);
        c.removeSelectedText();
        break;
    }
    default:
        c.movePosition(QTextCursor::EndOfBlock);
        c.insertText(QStringLiteral("\n") + pieces.at(int(random.bounded(pieces.size()))));
        break;
    }
}

} // namespace

void TestVisualView::patchedRowsEqualWholeRows_data()
{
    QTest::addColumn<bool>("folds");
    QTest::addColumn<uint>("seed");
    QTest::newRow("no folds, seed 1") << false << 1u;
    QTest::newRow("no folds, seed 2") << false << 2u;
    QTest::newRow("folds, seed 3") << true << 3u;
    QTest::newRow("folds, seed 4") << true << 4u;
}

// The rows an edit patches are those a whole build makes: a second view of
// the same document builds them all again on every edit.
void TestVisualView::patchedRowsEqualWholeRows()
{
    QFETCH(bool, folds);
    QFETCH(uint, seed);
    auto *view = static_cast<ProbeView *>(m_view);
    setDocument(rowsDocument(), 0);
    ProbeView whole(m_model);
    whole.setRowPatching(false);
    whole.resize(760, 900);
    whole.show();
    QVERIFY(QTest::qWaitForWindowExposed(&whole));
    // Neither has the focus: no caret opens a block or a fold.
    auto *other = new QLineEdit(&whole);
    other->setFocus();
    QVERIFY(QTest::qWaitForWindowActive(&whole));
    m_view->clearFocus();
    QVERIFY(!m_view->hasFocus());
    QVERIFY(!whole.hasFocus());
    m_model->flush();
    if (folds) {
        // Chips on some blocks, and a section folded after the edits' reach
        // (headings folded before them make the edits build all the rows).
        for (const int line : {2, 7, 47, 78, 120})
            m_view->setFolded(line, true), whole.setFolded(line, true);
        m_view->grab();
        whole.grab();
    }
    QCOMPARE(m_view->rowsDigest(), whole.rowsDigest());
    QRandomGenerator random(seed);
    int patched = 0;
    for (int step = 0; step < 400; ++step) {
        randomEdit(m_doc, random);
        // Now and then the pause ends and the document is parsed again.
        if (random.bounded(3) == 0)
            m_model->flush();
        patched += view->lastEditPatchedRows();
        const QString what = QStringLiteral("step %1").arg(step);
        QVERIFY2(whole.rowsConsistent(), qPrintable(what));
        QVERIFY2(view->rowsConsistent(), qPrintable(what));
        QVERIFY2(view->rowsDigest() == whole.rowsDigest(), qPrintable(what));
    }
    // Many edits were patched (the rest built them all: edits at another
    // place before the pause ends, a line count that differs from the
    // parse's for the first time, a heading folded above).
    QVERIFY2(patched > 30, qPrintable(QString::number(patched)));
}

// The same with the focus in the view: the caret opens the block it is in
// (while typing in source), a blank line it is on, or a fold. The edit's
// rows are compared with the same view's rows built from nothing.
void TestVisualView::patchedRowsWithCaretsEqualWholeRows()
{
    auto *view = static_cast<ProbeView *>(m_view);
    setDocument(rowsDocument(), 0);
    m_model->flush();
    for (const int line : {2, 47, 78, 120})
        view->setFolded(line, true);
    view->grab();
    QRandomGenerator random(7);
    int patched = 0;
    for (int step = 0; step < 300; ++step) {
        // The caret goes somewhere (and opens what is there).
        QTextCursor c(m_doc->findBlockByNumber(int(random.bounded(m_doc->blockCount()))));
        c.movePosition(QTextCursor::EndOfBlock);
        view->setTextCursor(c);
        if (random.bounded(4) == 0)
            view->openBlockAtCaret();
        if (random.bounded(3) == 0)
            m_model->flush();
        // An edit at the caret.
        if (random.bounded(2) == 0) {
            QTest::keyClicks(view, random.bounded(2) ? QStringLiteral("ab") : QStringLiteral("*"));
            if (random.bounded(3) == 0)
                QTest::keyClick(view, Qt::Key_Return);
        } else {
            randomEdit(m_doc, random);
        }
        const QString what = QStringLiteral("step %1").arg(step);
        patched += view->lastEditPatchedRows();
        QVERIFY2(view->rowsConsistent(), qPrintable(what));
        const QString digest = view->rowsDigest();
        view->rebuildRowsFully();
        QVERIFY2(digest == view->rowsDigest(), qPrintable(what));
    }
    QVERIFY2(patched > 20, qPrintable(QString::number(patched)));
}

// Typing in a paragraph of a large document patches the rows; so does a
// line break in it, and the rows are those of a whole build.
void TestVisualView::patchedRowsAfterTyping()
{
    auto *view = static_cast<ProbeView *>(m_view);
    setDocument(rowsDocument(), 0);
    m_model->flush();
    m_view->grab();
    QTextCursor c(m_doc->findBlockByNumber(2));
    c.movePosition(QTextCursor::EndOfBlock);
    m_view->setTextCursor(c);
    QTest::keyClicks(m_view, QStringLiteral("abc"));
    QVERIFY(m_view->lastEditPatchedRows());
    QVERIFY(view->rowsConsistent());
    QTest::keyClick(m_view, Qt::Key_Return);
    QVERIFY(view->rowsConsistent());
    QTest::keyClicks(m_view, QStringLiteral("def"));
    QVERIFY(m_view->lastEditPatchedRows());
    const QString patched = m_view->rowsDigest();
    m_view->rebuildRowsFully();
    QCOMPARE(patched, m_view->rowsDigest());
}

// Tables to compare TableLayout with Qt's layout on: alignment, inline
// formats, links, long cells, empty cells, right-to-left text, line breaks
// in cells, a wide table, a word too long for its column, front matter and
// an HTML table.
static QStringList layoutTables()
{
    const QStringList words = QStringLiteral("lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod "
                                             "tempor incididunt ut labore et dolore magna aliqua")
                                  .split(u' ');
    QString longCells = QStringLiteral("| H0 | H1 | H2 | H3 | H4 | H5 |\n|---|:---:|---|---:|---|---|\n");
    for (int row = 0; row < 9; ++row) {
        longCells += u'|';
        for (int col = 0; col < 6; ++col) {
            QStringList cell;
            for (int i = 0; i < ((row * 7 + col * 5) % 13) + 1; ++i)
                cell << words[(row * 3 + col * 11 + i * 5) % words.size()];
            longCells += u' ' + cell.join(u' ') + QStringLiteral(" |");
        }
        longCells += u'\n';
    }
    QString wide = QStringLiteral("|");
    QString rule = QStringLiteral("|");
    QString values = QStringLiteral("|");
    for (int col = 0; col < 30; ++col) {
        wide += QStringLiteral(" H%1 |").arg(col);
        rule += QStringLiteral("---|");
        values += QStringLiteral(" v%1 %2 |").arg(col).arg(words[col % words.size()]);
    }
    wide += u'\n' + rule + u'\n' + values + u'\n' + values + u'\n';
    return {
        QStringLiteral("| Left | Centre | Right |\n|:---|:---:|---:|\n| ipsum elit | **ipsum** | 0 |\n"
                       "| `x1` | do [a link](https://example.com) | 137 |\n"),
        QStringLiteral("---\ntitle: Tables\ntags: [a, b]\n---\n"),
        QStringLiteral("| one column |\n|---|\n| x |\n"),
        QStringLiteral("| a | b |\n|---|---|\n|  |  |\n| x |  |\n"),
        QStringLiteral("| עברית | العربية |\n|---|---|\n"
                       "| שלום עולם וכל אשר בו "
                       "| مرحبا بالعالم كله |\n"
                       "| mixed עברית text | 12 |\n"),
        QStringLiteral("| a | b |\n|---|---|\n| line one<br>line two | `code` and *em* ~~del~~ |\n"),
        longCells,
        wide,
        QStringLiteral("| x | supercalifragilisticexpialidocious_and_some_more_unbreakable_text |\n|---|---|\n| y | z |\n"),
        QStringLiteral("<table><tr><th>A</th><th>B</th></tr><tr><td><p>one</p><p>two</p></td><td>x</td></tr></table>\n"),
    };
}

static std::unique_ptr<RenderedBlock> layoutTable(const QString &markdown, qreal width, bool overflow, qreal zoom = 1)
{
    const QByteArray utf8 = markdown.toUtf8();
    const rust::Vec<core::Block> blocks = core::blocks(rust::Str(utf8.constData(), utf8.size()),
                                                       core::RenderOptions{false, true, true, true, 2});
    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.font.setPointSizeF(ctx.font.pointSizeF() * zoom);
    ctx.codeFont = theme::editorFont();
    ctx.codeFont.setPointSizeF(ctx.codeFont.pointSizeF() * zoom);
    ctx.width = width;
    ctx.overflow = overflow;
    return RenderedBlock::create(blocks.at(0), markdown, ctx);
}

// The first way two renderings of a table differ (empty if none): what they
// measure, where their lines and cells are, what points and positions give
// and their pixels.
static QString tableDifference(const RenderedBlock &actual, const RenderedBlock &expected)
{
    const auto same = [](const QRectF &a, const QRectF &b) {
        return a.x() == b.x() && a.y() == b.y() && a.width() == b.width() && a.height() == b.height();
    };
    const auto sameOptional = [&](const std::optional<QRectF> &a, const std::optional<QRectF> &b) {
        return a.has_value() == b.has_value() && (!a || same(*a, *b));
    };
    if (actual.height() != expected.height() || actual.contentWidth() != expected.contentWidth())
        return QStringLiteral("size %1x%2, expected %3x%4")
            .arg(actual.contentWidth()).arg(actual.height()).arg(expected.contentWidth()).arg(expected.height());
    if (actual.tableCells() != expected.tableCells() || actual.renderedText() != expected.renderedText())
        return QStringLiteral("cells or text");
    if (actual.lineAnchors() != expected.lineAnchors())
        return QStringLiteral("line anchors");
    const RenderedBlock::Breaks &a = actual.breaks(), &e = expected.breaks();
    if (a.cuts != e.cuts || a.lineTops != e.lineTops || a.table != e.table || a.headerTop != e.headerTop
        || a.headerHeight != e.headerHeight)
        return QStringLiteral("breaks");
    if (bool(actual.tableGrid()) != bool(expected.tableGrid())
        || (actual.tableGrid() && (actual.tableGrid()->xs != expected.tableGrid()->xs
                                   || actual.tableGrid()->ys != expected.tableGrid()->ys)))
        return QStringLiteral("grid");
    if (!sameOptional(actual.lastLineRect(), expected.lastLineRect()))
        return QStringLiteral("last line");
    for (int position = 0; position <= expected.renderedText().size() + 1; ++position) {
        if (!sameOptional(actual.caretRect(position), expected.caretRect(position)))
            return QStringLiteral("caret at %1").arg(position);
        if (actual.lineAt(position) != expected.lineAt(position))
            return QStringLiteral("line at %1").arg(position);
    }
    const qreal stepX = qMax(3.3, expected.contentWidth() / 70), stepY = qMax(2.9, expected.height() / 50);
    for (qreal y = -4; y < expected.height() + 4; y += stepY) {
        for (qreal x = -4; x < expected.contentWidth() + 4; x += stepX) {
            const QPointF point(x, y);
            if (actual.hitTest(point) != expected.hitTest(point))
                return QStringLiteral("hit at %1,%2: %3, expected %4").arg(x).arg(y).arg(actual.hitTest(point)).arg(expected.hitTest(point));
            if (actual.anchorAt(point) != expected.anchorAt(point) || actual.textAt(point) != expected.textAt(point))
                return QStringLiteral("anchor or text at %1,%2").arg(x).arg(y);
            const auto ac = actual.tableCellAt(point), ec = expected.tableCellAt(point);
            if (ac.has_value() != ec.has_value()
                || (ac && (ac->row != ec->row || ac->col != ec->col || !same(ac->rect, ec->rect))))
                return QStringLiteral("cell at %1,%2").arg(x).arg(y);
        }
    }
    struct Paint
    {
        qreal scale;
        bool antialiased;
        QRectF clip;
        QPair<int, int> selected;
    };
    const QList<Paint> paints = {{1, false, {}, {0, 0}},        {1.5, true, {}, {0, 0}},
                                 {1, false, {}, {0, 100000}},   {1.25, false, {}, {3, 40}},
                                 {2, true, {}, {7, 19}},        {1, false, {0, 40.3, 2000, 61.6}, {0, 0}},
                                 {0.75, true, {20, 0, 300, 2000}, {2, 9}}, {1, false, {0, 33, 4000, 90}, {1, 70}}};
    const auto paint = [](const RenderedBlock &block, const Paint &p) {
        QImage image(QSize(int((block.contentWidth() + 20) * p.scale), int((block.height() + 20) * p.scale)),
                     QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, p.antialiased);
        painter.scale(p.scale, p.scale);
        if (!p.clip.isNull())
            painter.setClipRect(p.clip);
        RenderedSelection selection;
        if (p.selected.second > 0)
            selection.ranges.append(p.selected);
        block.paint(painter, QPointF(7, 5), block.contentWidth(), selection);
        return image;
    };
    for (const Paint &p : paints) {
        const QImage a = paint(actual, p), e = paint(expected, p);
        if (a != e)
            return QStringLiteral("pixels at scale %1, selection %2-%3: %4")
                .arg(p.scale).arg(p.selected.first).arg(p.selected.second).arg(imageDifference(a, e));
    }
    return {};
}

// A table laid out by TableLayout is the table Qt lays out: its measures,
// lines, cells, hit tests, carets and pixels, made at a width and laid out
// again at others, in the scrolling view and in read mode, at three zooms.
void TestVisualView::tableLayoutIsQtsLayout()
{
    const QStringList documents = layoutTables();
    QRandomGenerator random(20261114);
    for (int i = 0; i < documents.size(); ++i) {
        const QString &markdown = documents[i];
        for (const qreal zoom : {1.0, 0.75, 1.75}) {
            for (const bool overflow : {false, true}) {
                const qreal first = zoom == 1 ? 548 : 400;
                const int before = TableLayout::cellsBroken();
                auto own = layoutTable(markdown, first, overflow, zoom);
                // Every pipe table and front matter takes the layout.
                if (i != documents.size() - 1)
                    QVERIFY2(TableLayout::cellsBroken() > before, qPrintable(markdown.left(20)));
                // Widths at random, some again, and the extremes.
                QList<qreal> widths = {first, 200, 731.5, 120};
                for (int n = 0; n < (zoom == 1 ? 6 : 2); ++n)
                    widths << (random.bounded(2) ? widths[random.bounded(widths.size())] : 60 + random.bounded(1200) / 1.5);
                for (int w = 0; w < widths.size(); ++w) {
                    const qreal width = widths[w];
                    const QString what = QStringLiteral("%1 at %2%3, zoom %4: ").arg(markdown.left(20)).arg(width)
                                             .arg(overflow ? QStringLiteral(" (read)") : QString()).arg(zoom);
                    RenderContext ctx;
                    ctx.font = theme::textFont();
                    ctx.font.setPointSizeF(ctx.font.pointSizeF() * zoom);
                    ctx.codeFont = theme::editorFont();
                    ctx.codeFont.setPointSizeF(ctx.codeFont.pointSizeF() * zoom);
                    ctx.width = width;
                    ctx.overflow = overflow;
                    QVERIFY2(own->reflow(ctx), qPrintable(what));
                    TableLayout::setEnabled(false);
                    const auto qt = layoutTable(markdown, width, overflow, zoom);
                    TableLayout::setEnabled(true);
                    const QString difference = tableDifference(*own, *qt);
                    QVERIFY2(difference.isEmpty(), qPrintable(what + difference));
                    // And made at that width (the first time it is seen).
                    if (widths.indexOf(width) == w) {
                        const auto made = layoutTable(markdown, width, overflow, zoom);
                        const QString madeDifference = tableDifference(*made, *qt);
                        QVERIFY2(madeDifference.isEmpty(), qPrintable(what + QStringLiteral("made: ") + madeDifference));
                    }
                }
            }
        }
    }
}

// A new width breaks the lines of a table again only in the columns whose
// width changed, and the same width breaks none.
void TestVisualView::tableLayoutBreaksOnlyChangedColumns()
{
    QString markdown = QStringLiteral("| # | Name | Description |\n|---|---|---|\n");
    for (int i = 0; i < 20; ++i)
        markdown += QStringLiteral("| %1 | ipsum | %2 |\n").arg(i).arg(QStringLiteral("dolor sit amet ").repeated(i % 5 + 2));
    auto block = layoutTable(markdown, 900, false);
    RenderContext ctx;
    ctx.font = theme::textFont();
    ctx.codeFont = theme::editorFont();
    ctx.overflow = false;
    for (const qreal width : {500.0, 480.0, 480.0, 300.0}) {
        const QSize cells = block->tableCells();
        const auto *grid = block->tableGrid();
        QVERIFY(grid);
        std::vector<qreal> xs = grid->xs;
        ctx.width = width;
        const int before = TableLayout::cellsBroken();
        QVERIFY(block->reflow(ctx));
        int changed = 0;
        for (int col = 0; col < cells.width(); ++col) {
            if (block->tableGrid()->xs[col + 1] - block->tableGrid()->xs[col] != xs[col + 1] - xs[col])
                ++changed;
        }
        QCOMPARE(TableLayout::cellsBroken() - before, changed * cells.height());
        QVERIFY(changed < cells.width()); // (The first two columns keep their widths.)
    }
}

QTEST_MAIN(TestVisualView)
#include "test_visualview.moc"
