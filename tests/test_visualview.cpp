// The hybrid visual Markdown view, run offscreen. Set VN_SCREENSHOT_DIR to
// save renderings.
#include "Settings.h"
#include "Theme.h"
#include "editor/DocumentLayout.h"
#include "markdown/BlockRenderer.h"
#include "markdown/MarkdownModel.h"
#include "markdown/VisualView.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QApplication>
#include <QPainter>
#include <QRegularExpression>
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
    void screenshots();
    void lineMarkersAreInvisible();
    void scrollSyncLinesUpTableRows();
    void scrollSyncIsContinuous_data();
    void scrollSyncIsContinuous();
    void scrollSyncEnds();

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
};

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

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestVisualView)
#include "test_visualview.moc"
