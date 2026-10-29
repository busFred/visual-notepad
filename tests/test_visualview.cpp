// The hybrid visual Markdown view, run offscreen. Set VN_SCREENSHOT_DIR to
// save renderings.
#include "Settings.h"
#include "Theme.h"
#include "editor/DocumentLayout.h"
#include "markdown/MarkdownModel.h"
#include "markdown/VisualView.h"

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
    void screenshots();

private:
    QTextDocument *m_doc = nullptr;
    KSyntaxHighlighting::SyntaxHighlighter *m_highlighter = nullptr;
    MarkdownModel *m_model = nullptr;
    VisualView *m_view = nullptr;
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

// Keep tests away from the user's settings file.
static void useTestSettings()
{
    static QTemporaryDir dir;
    vn::Settings::instance().useFile(dir.filePath(QStringLiteral("settings.ini")));
}
Q_COREAPP_STARTUP_FUNCTION(useTestSettings)

QTEST_MAIN(TestVisualView)
#include "test_visualview.moc"
