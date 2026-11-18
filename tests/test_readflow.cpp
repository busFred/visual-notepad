// The flow of read mode (markdown/ReadFlow): filling columns, the break rules,
// page turns and nudges, and keeping the place across a change of layout,
// against a made-up row source with plain heights (a line is 10 pixels).
#include "markdown/ReadFlow.h"

#include <QSet>
#include <QTest>

#include <algorithm>

using namespace vn::md;

namespace {

constexpr int lineHeight = 10;

QList<int> topsOf(int lines)
{
    QList<int> tops;
    for (int i = 0; i < lines; ++i)
        tops.push_back(i * lineHeight);
    return tops;
}

struct FakeRow
{
    RowKind kind = RowKind::Normal;
    int height = 0;
    QList<int> tops{0};
    QList<int> cuts;
    int first = 0;
    int last = 0;
    int header = 0;
};

// Rows with made-up heights. Source lines count up from 0 over the rows.
class FakeSource : public RowSource
{
public:
    std::vector<FakeRow> rows;
    QSet<int> touched; // Rows whose height, cuts or lines were asked for.

    int count() override { return int(rows.size()); }
    int height(int row) override { touched.insert(row); return rows[row].height; }
    int firstLine(int row) override { return rows[row].first; }
    int lastLine(int row) override { return rows[row].last; }
    int rowForLine(int line) override
    {
        const auto it = std::upper_bound(rows.begin(), rows.end(), line,
                                         [](int l, const FakeRow &r) { return l < r.first; });
        return it == rows.begin() ? 0 : int(it - rows.begin()) - 1;
    }
    RowKind kind(int row) override { touched.insert(row); return rows[row].kind; }
    QList<int> cuts(int row) override { touched.insert(row); return rows[row].cuts; }
    QList<int> lineTops(int row) override { touched.insert(row); return rows[row].tops; }
    int headerHeight(int row) override { return rows[row].header; }
    // As the visual view: a table's part may be one row.
    int minPieceLines(int row) override { return rows[row].kind == RowKind::Table ? 1 : 2; }

    int nextLine() const { return rows.empty() ? 0 : rows.back().last + 1; }

    // A paragraph of visual lines, showing sourceLines source lines.
    int para(int lines, int sourceLines = 0)
    {
        FakeRow r;
        r.height = lines * lineHeight;
        r.tops = topsOf(lines);
        r.cuts = paragraphCuts(r.tops);
        return add(r, sourceLines ? sourceLines : lines);
    }
    int heading() { return plain(RowKind::Heading, 2 * lineHeight, 1); }
    int gap(int height = 5) { return plain(RowKind::Gap, height, 1); }
    int figure(int height, QList<int> tops = {0}) { return plain(RowKind::Unbreakable, height, 1, tops); }
    // A block of ordinary rows that is one line of the flow, whatever its height.
    int block(int height) { return plain(RowKind::Normal, height, 1); }
    // A table: a header row of headerHeight and body rows of one line.
    int table(int body, int headerHeight = 2 * lineHeight)
    {
        FakeRow r;
        r.kind = RowKind::Table;
        r.header = headerHeight;
        r.tops = {0};
        for (int i = 0; i < body; ++i)
            r.tops.push_back(headerHeight + i * lineHeight);
        r.height = headerHeight + body * lineHeight;
        r.cuts = tableCuts(r.tops, 1);
        return add(r, body + 2);
    }
    // n rows of one line each: a row is a line.
    void singles(int n)
    {
        for (int i = 0; i < n; ++i)
            plain(RowKind::Normal, lineHeight, 1);
    }

private:
    int plain(RowKind kind, int height, int sourceLines, QList<int> tops = {0})
    {
        FakeRow r;
        r.kind = kind;
        r.height = height;
        r.tops = tops;
        return add(r, sourceLines);
    }
    int add(FakeRow r, int sourceLines)
    {
        r.first = nextLine();
        r.last = r.first + sourceLines - 1;
        rows.push_back(r);
        return int(rows.size()) - 1;
    }
};

Geometry geo(int columns, int height, int width = 100, int revision = 0)
{
    Geometry g;
    g.columns = columns;
    g.height = height;
    g.width = width;
    g.gap = 10;
    g.revision = revision;
    return g;
}

// A screen as text: the rows in each column ("3" whole, "3[0-70]" a part, "h3[100-140]"
// a part under the table's repeated header), columns apart by " | ".
QString describe(const Screen &screen, FakeSource &source)
{
    QStringList columns;
    for (const Column &column : screen.columns) {
        QStringList parts;
        for (const Segment &s : column.segments) {
            QString text = QString::number(s.row);
            if (s.yFrom != 0 || s.yTo != source.rows[s.row].height)
                text += QStringLiteral("[%1-%2]").arg(s.yFrom).arg(s.yTo);
            if (s.repeatHeader)
                text.prepend('h');
            parts.push_back(text);
        }
        if (!parts.isEmpty())
            columns.push_back(parts.join(' '));
    }
    return columns.join(QStringLiteral(" | "));
}

} // namespace

class TestReadFlow : public QObject
{
    Q_OBJECT

private slots:
    void wholeRowsFill();
    void cutAtDeepestFit();
    void unbreakableTallerThanColumn();
    void unbreakableMovesToNextColumn();
    void headingNotStranded();
    void gapSkippedAtTop();
    void paragraphCutRule();
    void codeCutRule();
    void onePieceLinesForEveryLineCuts();
    void tableCutRule();
    void tableHeaderRepeats();
    void tableCutsKeepOneRowEachSide();
    void tallTableRowBreaksBetweenItsLines();
    void pageForwardKeepsLines();
    void keptLinesSetting();
    void keptLinesSkipFiguresAndTallBlocks();
    void pageForwardStopsAtEnd();
    void pageBackThroughHistory();
    void pageBackAfterLayoutChange();
    void pageBackAfterNudgeOrRevision();
    void pagesNeverSkipAnything();
    void nudgeAcrossRows();
    void nudgeStopsAtEdges();
    void positionSurvivesColumnChange();
    void positionSurvivesWidthChange();
    void lineRange();
    void startAndEnd();
    void touchesAboutAScreen();
};

void TestReadFlow::wholeRowsFill()
{
    FakeSource source;
    for (int i = 0; i < 5; ++i)
        source.para(3);
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 100));
    // 3 x 30 fit in 100; a fourth does not and has no cut, so it starts the next.
    const Screen two = flow.fill({0, 0});
    QCOMPARE(describe(two, source), QStringLiteral("0 1 2 | 3 4"));
    QCOMPARE(two.columns[0].used, 90);
    QCOMPARE(two.columns[1].used, 60);
    QVERIFY(two.atStart);
    QVERIFY(two.atEnd);
    QCOMPARE(two.end.row, 5);
}

void TestReadFlow::cutAtDeepestFit()
{
    FakeSource source;
    source.para(3);
    source.para(10); // Cuts at 20..80.
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 100));
    const Screen s = flow.fill({0, 0});
    // 70 left: the deepest cut within it is 70 (3 lines remain).
    QCOMPARE(describe(s, source), QStringLiteral("0 1[0-70] | 1[70-100]"));
    QVERIFY(s.atStart);
    QVERIFY(s.atEnd);
    QCOMPARE(s.columns[0].used, 100);
}

void TestReadFlow::unbreakableTallerThanColumn()
{
    FakeSource source;
    source.figure(250, {0, 40, 80, 120, 160, 200, 240});
    ReadFlow flow(&source);
    flow.setGeometry(geo(3, 100));
    // Cut at the deepest line top within the column's height.
    QCOMPARE(describe(flow.fill({0, 0}), source), QStringLiteral("0[0-80] | 0[80-160] | 0[160-250]"));

    FakeSource plain;
    plain.figure(250);
    ReadFlow flat(&plain);
    flat.setGeometry(geo(3, 100));
    // No line top inside: cut where the column ends.
    QCOMPARE(describe(flat.fill({0, 0}), plain), QStringLiteral("0[0-100] | 0[100-200] | 0[200-250]"));
}

void TestReadFlow::unbreakableMovesToNextColumn()
{
    FakeSource source;
    source.figure(50);
    source.figure(60);
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 100));
    QCOMPARE(describe(flow.fill({0, 0}), source), QStringLiteral("0 | 1"));
}

void TestReadFlow::headingNotStranded()
{
    {
        FakeSource source;
        source.para(7);
        source.heading();
        source.para(5); // Cuts at 20 and 30 only: neither fits in the 10 left.
        ReadFlow flow(&source);
        flow.setGeometry(geo(2, 100));
        const Screen s = flow.fill({0, 0});
        QCOMPARE(describe(s, source), QStringLiteral("0 | 1 2"));
        QCOMPARE(s.columns[0].used, 70);
    }
    {
        // Blank space after the heading and before it goes with it.
        FakeSource source;
        source.para(7);
        source.gap();
        source.heading();
        source.para(5);
        ReadFlow flow(&source);
        flow.setGeometry(geo(2, 100));
        QCOMPARE(describe(flow.fill({0, 0}), source), QStringLiteral("0 | 2 3"));
    }
    {
        // Two headings in a row: both move.
        FakeSource source;
        source.para(7);
        source.heading();
        source.heading();
        ReadFlow flow(&source);
        flow.setGeometry(geo(2, 100));
        QCOMPARE(describe(flow.fill({0, 0}), source), QStringLiteral("0 | 1 2"));
    }
    {
        // A heading that is all a column has stays.
        FakeSource source;
        source.heading();
        source.para(9);
        ReadFlow flow(&source);
        flow.setGeometry(geo(2, 20));
        const Screen s = flow.fill({0, 0});
        QCOMPARE(describe(s, source).left(1), QStringLiteral("0"));
        QCOMPARE(s.columns[0].segments.size(), size_t(1));
    }
    {
        // The last column too: the heading starts the next page.
        FakeSource source;
        source.para(7);
        source.heading();
        source.para(5);
        ReadFlow flow(&source);
        flow.setGeometry(geo(1, 100));
        const Screen s = flow.fill({0, 0});
        QCOMPARE(describe(s, source), QStringLiteral("0"));
        QCOMPARE(s.end.row, 1);
        QVERIFY(!s.atEnd);
    }
}

void TestReadFlow::gapSkippedAtTop()
{
    FakeSource source;
    source.para(10);
    source.gap(15);
    source.para(3);
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 100));
    QCOMPARE(describe(flow.fill({0, 0}), source), QStringLiteral("0 | 2"));
    // A start on the gap is moved to what follows it.
    const Screen s = flow.fill({1, 0});
    QCOMPARE(describe(s, source), QStringLiteral("2"));
    QCOMPARE(s.start.row, 2);

    FakeSource leading;
    leading.gap();
    leading.para(2);
    ReadFlow second(&leading);
    second.setGeometry(geo(1, 100));
    QCOMPARE(second.screen().start.row, 1);
    QVERIFY(second.screen().atStart);
}

void TestReadFlow::paragraphCutRule()
{
    QCOMPARE(paragraphCuts(topsOf(0)), QList<int>());
    QCOMPARE(paragraphCuts(topsOf(3)), QList<int>());
    QCOMPARE(paragraphCuts(topsOf(4)), (QList<int>{20}));
    QCOMPARE(paragraphCuts(topsOf(6)), (QList<int>{20, 30, 40}));
    QCOMPARE(paragraphCuts(topsOf(10)), (QList<int>{20, 30, 40, 50, 60, 70, 80}));
}

void TestReadFlow::codeCutRule()
{
    QCOMPARE(codeCuts(topsOf(3)), QList<int>());
    QCOMPARE(codeCuts(topsOf(4)), (QList<int>{20}));
    QCOMPARE(codeCuts(topsOf(200)).size(), 197);
    QCOMPARE(codeCuts(topsOf(200)).first(), 20);
    QCOMPARE(codeCuts(topsOf(200)).last(), 1980);
}

// A source cut between any two lines (the source view's columns) asks for
// pieces of one line: a column is full to its last line, wherever a wrapped
// line falls; the default keeps two lines to a piece of a row cut twice.
void TestReadFlow::onePieceLinesForEveryLineCuts()
{
    class EveryLine : public FakeSource
    {
    public:
        int least = 2;
        int minPieceLines(int) override { return least; }
    };
    for (const int least : {2, 1}) {
        EveryLine source;
        source.least = least;
        source.singles(2);
        FakeRow wrapped;
        wrapped.height = 7 * lineHeight;
        wrapped.tops = topsOf(7);
        wrapped.cuts = wrapped.tops.mid(1); // After every line.
        wrapped.first = wrapped.last = source.nextLine();
        source.rows.push_back(wrapped);
        source.singles(10);
        ReadFlow flow(&source);
        flow.setGeometry(geo(3, 3 * lineHeight));
        const Screen &screen = flow.toStart();
        const Column &first = screen.columns[0];
        if (least == 2) {
            // One line of it would be alone at the bottom: it goes on in the next column.
            QCOMPARE(first.segments.size(), size_t(2));
            QCOMPARE(screen.columns[1].segments.front().yFrom, 0);
        } else {
            QCOMPARE(first.segments.size(), size_t(3));
            QCOMPARE(first.segments.back().row, 2);
            QCOMPARE(first.segments.back().yTo, lineHeight);
            QCOMPARE(first.used, 3 * lineHeight);
            QCOMPARE(screen.columns[1].segments.front().yFrom, lineHeight);
            QCOMPARE(screen.columns[1].segments.front().yTo, 4 * lineHeight);
            QCOMPARE(screen.columns[2].segments.front().yFrom, 4 * lineHeight);
        }
    }
}

void TestReadFlow::tableCutRule()
{
    // A header row of 20, then body rows of 10.
    const QList<int> five{0, 20, 30, 40, 50, 60};
    QCOMPARE(tableCuts(five, 1), (QList<int>{30, 40, 50, 60}));
    QCOMPARE(tableCuts({0, 20, 30}, 1), (QList<int>{30})); // 2 body rows: 1 + 1.
    QCOMPARE(tableCuts({0, 20}, 1), QList<int>());         // 1: no cut.
    QCOMPARE(tableCuts({0}, 1), QList<int>());
    // Two header rows.
    QCOMPARE(tableCuts({0, 10, 20, 30, 40}, 2), (QList<int>{30, 40}));
}


void TestReadFlow::tableHeaderRepeats()
{
    FakeSource source;
    source.table(12); // Header 20, body rows of 10: 140 high; cuts at 40..120.
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 100));
    const Screen s = flow.fill({0, 0});
    QCOMPARE(describe(s, source), QStringLiteral("0[0-100] | h0[100-140]"));
    QVERIFY(!s.columns[0].segments[0].repeatHeader);
    QVERIFY(s.columns[1].segments[0].repeatHeader);
    // The header counts against the column.
    QCOMPARE(s.columns[1].used, 40 + 20);
    QCOMPARE(flow.drawnHeight(s.columns[1].segments[0]), 60);

    // After other rows: the part that fits goes on, the rest repeats the header.
    FakeSource after;
    after.para(3);
    after.table(12);
    ReadFlow second(&after);
    second.setGeometry(geo(2, 100));
    QCOMPARE(describe(second.fill({0, 0}), after), QStringLiteral("0 1[0-70] | h1[70-140]"));
    QCOMPARE(second.fill({0, 0}).columns[1].used, 70 + 20);

    // A page that starts inside a table has the header too.
    QCOMPARE(describe(second.fill({1, 70}), after), QStringLiteral("h1[70-140]"));
}

void TestReadFlow::tableCutsKeepOneRowEachSide()
{
    FakeSource source;
    source.table(5); // 70 high; cuts at 30 to 60.
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 65));
    // The deepest: one row goes on.
    QCOMPARE(describe(flow.fill({0, 0}), source), QStringLiteral("0[0-60] | h0[60-70]"));
    flow.setGeometry(geo(2, 25));
    // Never the header alone: one body row with it, cut where the column ends.
    QVERIFY(describe(flow.fill({0, 0}), source).startsWith(QStringLiteral("0[0-25]")));

    // A table of one body row has no cut: it moves whole.
    FakeSource small;
    small.para(6);
    small.table(1);
    ReadFlow third(&small);
    third.setGeometry(geo(2, 70));
    QCOMPARE(describe(third.fill({0, 0}), small), QStringLiteral("0 | 1"));

    // A part cut again in a later column has a row at least.
    FakeSource tall;
    tall.table(12);
    ReadFlow fourth(&tall);
    fourth.setGeometry(geo(3, 45));
    const Screen s = fourth.fill({0, 0});
    for (const Column &column : s.columns) {
        for (const Segment &part : column.segments) {
            const int rows = (part.yTo - std::max(part.yFrom, 20)) / lineHeight;
            QVERIFY2(rows >= 1, qPrintable(describe(s, tall)));
        }
    }
}

// A table whose one body row is taller than a column: the header and the
// row's first lines, then the header again over the rest. Never the header
// alone in a column.
void TestReadFlow::tallTableRowBreaksBetweenItsLines()
{
    FakeSource source;
    FakeRow r;
    r.kind = RowKind::Table;
    r.header = 20;
    r.height = 20 + 15 * lineHeight; // One row of 15 lines: 170.
    r.tops = {0, 20};
    for (int i = 1; i < 15; ++i)
        r.tops.push_back(20 + i * lineHeight);
    r.cuts = r.tops.mid(3, 12); // Inside the row, 2 lines each side.
    r.first = 0;
    r.last = 2;
    source.rows.push_back(r);
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 100));
    QCOMPARE(describe(flow.fill({0, 0}), source), QStringLiteral("0[0-100] | h0[100-170]"));

    // After a paragraph, with only the header's room left: the table starts the next column.
    FakeSource after;
    after.para(8);
    after.rows.push_back(r);
    after.rows.back().first = after.rows.back().last = after.nextLine();
    ReadFlow second(&after);
    second.setGeometry(geo(2, 100));
    const QString d = describe(second.fill({0, 0}), after);
    QVERIFY2(!d.contains(QStringLiteral("1[0-20]")), qPrintable(d));
}

void TestReadFlow::pageForwardKeepsLines()
{
    FakeSource source;
    source.singles(30);
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 50)); // 10 lines a screen.
    QCOMPARE(flow.screen().start.row, 0);
    QCOMPARE(flow.screen().end.row, 10);
    QVERIFY(!flow.screen().hasKept);

    const Screen &next = flow.pageForward();
    QCOMPARE(next.start.row, 8); // The last 2 lines of the page stay.
    QCOMPARE(next.end.row, 18);
    QVERIFY(next.hasKept);
    QCOMPARE(next.columns[0].segments[0].keptTo, 10);
    QCOMPARE(next.columns[0].segments[1].keptTo, 10);
    QCOMPARE(next.columns[0].segments[2].keptTo, 0);
    int kept = 0;
    for (const Column &column : next.columns) {
        for (const Segment &s : column.segments)
            kept += s.keptTo > s.yFrom ? 1 : 0;
    }
    QCOMPARE(kept, 2);
    QCOMPARE(flow.startLine(), 8);
    QCOMPARE(flow.historySize(), 1);

    // Filled again in place (a render finished, the caret moved): still the
    // screen of the page turn. Filled from another revision: not.
    QVERIFY(flow.reflow().hasKept);
    QCOMPARE(flow.screen().columns[0].segments[1].keptTo, 10);
    QCOMPARE(flow.screen().start.row, 8);
    flow.setGeometry(geo(2, 50, 100, 1));
    QCOMPARE(flow.screen().start.row, 8);
    QVERIFY(!flow.screen().hasKept);
    QVERIFY(!flow.reflow().hasKept);

    // Any other move shows them normally.
    flow.pageBack();
    QVERIFY(flow.pageForward().hasKept);
    QVERIFY(!flow.nudge(1).hasKept);
    QVERIFY(!flow.reflow().hasKept);
}

void TestReadFlow::keptLinesSetting()
{
    FakeSource source;
    source.singles(30);
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 50));
    flow.setKeptLines(0);
    const Screen &none = flow.pageForward();
    QCOMPARE(none.start.row, 10);
    QVERIFY(!none.hasKept);

    flow.setKeptLines(5);
    QCOMPARE(flow.keptLines(), 5);
    const Screen &five = flow.pageForward(); // From the page 10..19.
    QCOMPARE(five.start.row, 15);
    int kept = 0;
    for (const Segment &s : five.columns[0].segments)
        kept += s.keptTo > s.yFrom ? 1 : 0;
    QCOMPARE(kept, 5);

    flow.setKeptLines(9);
    QCOMPARE(flow.keptLines(), 5);
    flow.setKeptLines(-1);
    QCOMPARE(flow.keptLines(), 0);

    // Lines kept from inside a row: the part of the row above the boundary.
    FakeSource tall;
    tall.para(30); // 300 high.
    ReadFlow second(&tall);
    second.setKeptLines(2);
    second.setGeometry(geo(1, 100));
    const Screen &page = second.pageForward(); // Ended at the cut at 100.
    QCOMPARE(page.start.row, 0);
    QCOMPARE(page.start.y, 80);
    QCOMPARE(page.columns[0].segments[0].keptTo, 100);
    QCOMPARE(page.columns[0].segments[0].yFrom, 80);
}

// Lines of text only come back at the top of the next page: not those of a
// figure (or equation, chip, short table), nor of a block taller than a
// quarter of the column.
void TestReadFlow::keptLinesSkipFiguresAndTallBlocks()
{
    const auto keptRows = [](const Screen &screen) {
        QList<int> rows;
        for (const Column &column : screen.columns) {
            for (const Segment &s : column.segments) {
                if (s.keptTo > s.yFrom)
                    rows.push_back(s.row);
            }
        }
        return rows;
    };

    // The page ends with a figure: nothing is kept.
    {
        FakeSource source;
        source.singles(8);
        source.figure(20); // Row 8: the page ends after it.
        source.singles(12);
        ReadFlow flow(&source);
        flow.setGeometry(geo(1, 100));
        QCOMPARE(flow.screen().end.row, 9);
        const Screen &next = flow.pageForward();
        QCOMPARE(next.start.row, 9);
        QVERIFY(!next.hasKept);
        QVERIFY(keptRows(next).isEmpty());
        // ... and back where it came from.
        QCOMPARE(flow.pageBack().start.row, 0);
    }
    // Text after the figure is kept, up to the figure: one line, then two.
    for (int after = 1; after <= 3; ++after) {
        FakeSource source;
        source.singles(7 - after + 1);
        source.figure(20);
        source.singles(after);
        source.singles(12);
        ReadFlow flow(&source);
        flow.setGeometry(geo(1, 100));
        const int figureRow = 8 - after;
        QCOMPARE(flow.screen().end.row, figureRow + 1 + after);
        const Screen &next = flow.pageForward();
        const int expected = qMin(after, 2);
        QCOMPARE(next.start.row, figureRow + 1 + after - expected);
        QCOMPARE(int(keptRows(next).size()), expected);
        QVERIFY(next.start.row > figureRow);
    }
    // The same for a block that is one line and taller than a quarter of the
    // column (25 of 100), but not for one that is as high as a quarter.
    {
        FakeSource source;
        source.singles(7);
        source.block(30);
        source.singles(12);
        ReadFlow flow(&source);
        flow.setGeometry(geo(1, 100));
        QCOMPARE(flow.screen().end.row, 8);
        const Screen &next = flow.pageForward();
        QCOMPARE(next.start.row, 8);
        QVERIFY(!next.hasKept);
    }
    {
        FakeSource source;
        source.singles(7);
        source.block(25);
        source.singles(12);
        ReadFlow flow(&source);
        flow.setGeometry(geo(1, 100));
        QCOMPARE(flow.screen().end.row, 8);
        const Screen &next = flow.pageForward();
        QVERIFY(next.hasKept);
        QCOMPARE(next.start.row, 6); // The block and the line before it.
    }
    // A paragraph's lines are lines whatever the paragraph's height.
    {
        FakeSource source;
        source.para(10);
        source.singles(12);
        ReadFlow flow(&source);
        flow.setGeometry(geo(1, 100));
        const Screen &next = flow.pageForward();
        QVERIFY(next.hasKept);
        QCOMPARE(next.start.row, 0); // The paragraph's last two lines.
        QCOMPARE(next.start.y, 80);
    }
    // Several columns: the last column's end is what counts.
    {
        FakeSource source;
        source.singles(18);
        source.figure(20); // Ends the second column.
        source.singles(30);
        ReadFlow flow(&source);
        flow.setGeometry(geo(2, 100));
        QCOMPARE(flow.screen().end.row, 19);
        const Screen &next = flow.pageForward();
        QCOMPARE(next.start.row, 19);
        QVERIFY(!next.hasKept);
    }
}

void TestReadFlow::pageForwardStopsAtEnd()
{
    FakeSource source;
    source.singles(30);
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 50));
    flow.pageForward(); // 8
    flow.pageForward(); // 16
    const Screen &last = flow.pageForward(); // 24: rows 24..29.
    QCOMPARE(last.start.row, 24);
    QVERIFY(last.atEnd);
    QCOMPARE(flow.pageForward().start.row, 24);
    QCOMPARE(flow.historySize(), 3);
}

void TestReadFlow::pageBackThroughHistory()
{
    FakeSource source;
    source.singles(30);
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 50));
    flow.pageForward();
    flow.pageForward();
    QCOMPARE(flow.screen().start.row, 16);
    const int touchedBefore = source.touched.size();
    Q_UNUSED(touchedBefore);

    QCOMPARE(flow.pageBack().start.row, 8);
    QVERIFY(!flow.screen().hasKept);
    QCOMPARE(flow.historySize(), 1);
    QCOMPARE(flow.pageBack().start.row, 0);
    QVERIFY(flow.screen().atStart);
    QCOMPARE(flow.pageBack().start.row, 0);
    QCOMPARE(flow.historySize(), 0);
}

void TestReadFlow::pageBackAfterLayoutChange()
{
    FakeSource source;
    source.singles(30);
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 50));
    flow.pageForward();
    flow.pageForward();
    QCOMPARE(flow.screen().start.row, 16);

    // Three columns: the old history means nothing, the place stays.
    flow.setGeometry(geo(3, 50));
    QCOMPARE(flow.historySize(), 0);
    QCOMPARE(flow.screen().start.row, 16);
    QCOMPARE(flow.startLine(), 16);
    QVERIFY(flow.screen().atEnd);

    // The screen before ends 2 lines past 16, so a page forward from it
    // comes back to 16 and nothing is skipped.
    const Screen &before = flow.pageBack();
    QCOMPARE(before.start.row, 3);
    QCOMPARE(before.end.row, 18);
    QCOMPARE(flow.pageForward().start.row, 16);
    QCOMPARE(flow.pageBack().start.row, 3);

    // A new height does the same.
    flow.setGeometry(geo(3, 30));
    QCOMPARE(flow.screen().start.row, 3);
    const Screen &earlier = flow.pageBack();
    QVERIFY(earlier.start.row < 3);
    QVERIFY(earlier.end.row >= 3);
}

void TestReadFlow::pageBackAfterNudgeOrRevision()
{
    FakeSource source;
    source.singles(30);
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 50));
    flow.pageForward();
    flow.nudge(1); // The remembered turn no longer leads here.
    QCOMPARE(flow.screen().start.row, 9);
    QCOMPARE(flow.historySize(), 0);
    QCOMPARE(flow.pageBack().start.row, 1);
    QCOMPARE(flow.screen().end.row, 11);

    // The same layout but other rows (an edit): worked out as well.
    flow.toStart();
    flow.pageForward();
    flow.pageForward();
    QCOMPARE(flow.historySize(), 2);
    flow.setGeometry(geo(2, 50, 100, 1));
    QCOMPARE(flow.historySize(), 0);
    QCOMPARE(flow.screen().start.row, 16);
    QCOMPARE(flow.pageBack().start.row, 8);
    QCOMPARE(flow.pageForward().start.row, 16);
}

// Every kind of row, in an order made up once: pages forward touch each other
// (the next starts no later than the last ended, and later than it started)
// and so do pages back (each ends no earlier than the one it came from started).
void TestReadFlow::pagesNeverSkipAnything()
{
  for (const quint32 first : {12345u, 7u, 99991u, 31337u, 424242u}) {
    FakeSource source;
    quint32 seed = first;
    const auto next = [&seed](int n) {
        seed = seed * 1664525u + 1013904223u;
        return int((seed >> 8) % quint32(n));
    };
    for (int i = 0; i < 300; ++i) {
        switch (next(12)) {
        case 0:
            source.heading();
            break;
        case 1:
            source.gap(5 + next(20));
            break;
        case 2:
            source.table(2 + next(14));
            break;
        case 3:
            source.figure(20 + next(150));
            break;
        default:
            source.para(1 + next(14));
        }
    }
    const int geometries[][2] = {{1, 80}, {1, 200}, {2, 100}, {3, 120}, {3, 60}, {2, 45}};
    for (const auto &g : geometries) {
        for (int kept = 0; kept <= 5; kept += 2) {
            ReadFlow flow(&source);
            flow.setKeptLines(kept);
            flow.setGeometry(geo(g[0], g[1]));
            const QByteArray label = QByteArray::number(g[0]) + "x" + QByteArray::number(g[1])
                + " kept " + QByteArray::number(kept);
            QVERIFY(flow.screen().atStart);
            int pages = 1;
            std::vector<Position> starts{flow.screen().start};
            while (!flow.screen().atEnd) {
                const Position oldStart = flow.screen().start;
                const Position oldEnd = flow.screen().end;
                flow.pageForward();
                QVERIFY2(flow.screen().start > oldStart, label);
                QVERIFY2(flow.screen().start <= oldEnd, label);
                QVERIFY2(flow.screen().end > oldEnd, label);
                starts.push_back(flow.screen().start);
                QVERIFY2(++pages < 3000, label);
            }
            // Back through the history, exactly (it holds the last 256 turns).
            const size_t oldest = starts.size() > 200 ? starts.size() - 200 : 0;
            for (size_t i = starts.size() - 1; i > oldest; --i) {
                QCOMPARE(flow.screen().start, starts[i]);
                flow.pageBack();
            }
            QCOMPARE(flow.screen().start, starts[oldest]);

            // Back from the end with nothing remembered.
            flow.toEnd();
            QVERIFY2(flow.screen().atEnd, label);
            int steps = 0;
            while (!flow.screen().atStart) {
                const Position oldStart = flow.screen().start;
                flow.pageBack();
                QVERIFY2(flow.screen().start < oldStart, label);
                QVERIFY2(flow.screen().end >= oldStart, label);
                QVERIFY2(++steps < 3000, label);
            }
        }
    }
  }
}

void TestReadFlow::nudgeAcrossRows()
{
    FakeSource source;
    source.para(3);  // Row 0, lines 0..2, tops 0 10 20.
    source.gap();    // Row 1.
    source.para(2);  // Row 2.
    source.para(4);  // Row 3.
    ReadFlow flow(&source);
    flow.setGeometry(geo(1, 40)); // Never the whole document on screen.

    QCOMPARE(flow.nudge(1).start, (Position{0, 10}));
    QCOMPARE(flow.nudge(1).start, (Position{0, 20}));
    // Over the row boundary, and past the gap row, which has no lines.
    QCOMPARE(flow.nudge(1).start, (Position{2, 0}));
    QCOMPARE(flow.nudge(1).start, (Position{2, 10}));
    QCOMPARE(flow.nudge(1).start, (Position{3, 0}));
    QCOMPARE(flow.nudge(-1).start, (Position{2, 10}));
    QCOMPARE(flow.nudge(-1).start, (Position{2, 0}));
    QCOMPARE(flow.nudge(-1).start, (Position{0, 20}));
    QCOMPARE(flow.nudge(-2).start, (Position{0, 0}));
    QCOMPARE(flow.nudge(-1).start, (Position{0, 0}));
    QCOMPARE(flow.nudge(3).start, (Position{2, 0}));
    QCOMPARE(flow.startLine(), 4);
    QVERIFY(!flow.screen().hasKept);
}

void TestReadFlow::nudgeStopsAtEdges()
{
    FakeSource source;
    source.singles(30);
    ReadFlow flow(&source);
    flow.setGeometry(geo(1, 50)); // 5 lines.
    QVERIFY(flow.screen().atStart);
    QCOMPARE(flow.nudge(-1).start.row, 0);
    QCOMPARE(flow.nudge(24).start.row, 24);
    QVERIFY(!flow.screen().atEnd);
    QCOMPARE(flow.nudge(1).start.row, 25); // The last line is the last on screen.
    QVERIFY(flow.screen().atEnd);
    QCOMPARE(flow.nudge(1).start.row, 25);
    QCOMPARE(flow.nudge(10).start.row, 25);
    QCOMPARE(flow.nudge(-26).start.row, 0);
    QCOMPARE(flow.nudge(100).start.row, 25); // Stops at the end.
}

void TestReadFlow::positionSurvivesColumnChange()
{
    FakeSource source;
    source.singles(100);
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 50));
    flow.goToLine(25);
    QCOMPARE(flow.screen().start.row, 25);
    flow.setGeometry(geo(3, 50));
    QCOMPARE(flow.screen().start.row, 25);
    QCOMPARE(flow.screen().columns.size(), size_t(3));
    flow.setGeometry(geo(1, 50));
    QCOMPARE(flow.screen().start.row, 25);
    QCOMPARE(flow.startLine(), 25);
    QCOMPARE(flow.positionOfLine(77), (Position{77, 0}));
    QCOMPARE(flow.lineOf({77, 0}), 77);
}

// 10 paragraphs of 8 source lines, as wide text (8 visual lines) or narrow...
// no: wider columns, fewer lines (4).
static void wrapped(FakeSource &source, int visualLines)
{
    source.rows.clear();
    for (int i = 0; i < 10; ++i)
        source.para(visualLines, 8);
}

void TestReadFlow::positionSurvivesWidthChange()
{
    FakeSource source;
    wrapped(source, 8);
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 100, 400));
    flow.goToLine(35); // Row 4 (lines 32..39), its 4th line.
    QCOMPARE(flow.screen().start, (Position{4, 30}));
    QCOMPARE(flow.startLine(), 35);

    // Twice as wide: half the lines. The same source line is on top (to a line).
    wrapped(source, 4);
    flow.setGeometry(geo(2, 100, 800));
    QCOMPARE(flow.screen().start.row, 4);
    QCOMPARE(flow.screen().start, (Position{4, 10}));
    QCOMPARE(flow.startLine(), 35); // Not worn down by the rounding.
    QVERIFY(qAbs(flow.lineOf(flow.screen().start) - 35) <= 1);

    // And back.
    wrapped(source, 8);
    flow.setGeometry(geo(2, 100, 400));
    QCOMPARE(flow.screen().start, (Position{4, 30}));

    // Rows rebuilt (an edit) under the same width: by source line too.
    source.rows.clear();
    source.para(4, 8);
    source.para(4, 8);
    for (int i = 0; i < 6; ++i)
        source.para(8, 8);
    flow.setGeometry(geo(2, 100, 400, 1));
    QCOMPARE(flow.screen().start.row, 4);
}

void TestReadFlow::lineRange()
{
    FakeSource source;
    source.singles(30);
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 50));
    QCOMPARE(flow.lineRange().first, 0);
    QCOMPARE(flow.lineRange().last, 9);
    flow.pageForward();
    QCOMPARE(flow.lineRange().first, 8);
    QCOMPARE(flow.lineRange().last, 17);
    flow.toEnd();
    QCOMPARE(flow.lineRange().first, 20);
    QCOMPARE(flow.lineRange().last, 29);

    // A cut paragraph: only the lines above the cut count.
    FakeSource tall;
    tall.para(10);
    ReadFlow cut(&tall);
    cut.setGeometry(geo(1, 70));
    QCOMPARE(cut.lineRange().first, 0);
    QCOMPARE(cut.lineRange().last, 6);
    cut.pageForward();
    QCOMPARE(cut.lineRange().first, 5);
    QCOMPARE(cut.lineRange().last, 9);
    cut.setGeometry(geo(2, 70));
    QCOMPARE(cut.lineRange().first, 5);
    QCOMPARE(cut.lineRange().last, 9);

    // Gap rows are not lines on screen.
    FakeSource gaps;
    gaps.gap();
    gaps.para(2);
    gaps.gap();
    ReadFlow g(&gaps);
    g.setGeometry(geo(1, 100));
    QCOMPARE(g.lineRange().first, 1);
    QCOMPARE(g.lineRange().last, 2);
}

void TestReadFlow::startAndEnd()
{
    FakeSource source;
    source.singles(30);
    source.gap(); // A blank line at the end of the file.
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 50));
    flow.pageForward();
    QCOMPARE(flow.toEnd().start.row, 20);
    QVERIFY(flow.screen().atEnd);
    QVERIFY(!flow.screen().atStart);
    QCOMPARE(flow.historySize(), 0);
    QCOMPARE(flow.toStart().start.row, 0);
    QVERIFY(flow.screen().atStart);
    QVERIFY(!flow.screen().atEnd);

    // The end with a row taller than the screen.
    FakeSource tall;
    tall.para(3);
    tall.figure(250);
    ReadFlow second(&tall);
    second.setGeometry(geo(2, 100));
    const Screen &end = second.toEnd();
    QVERIFY(end.atEnd);
    QCOMPARE(end.end.row, 2);
    QCOMPARE(describe(end, tall), QStringLiteral("1[50-150] | 1[150-250]"));

    // An empty document.
    FakeSource none;
    ReadFlow empty(&none);
    empty.setGeometry(geo(2, 100));
    QVERIFY(empty.screen().empty());
    QVERIFY(empty.toEnd().atEnd);
    QVERIFY(empty.toStart().atStart);
    QVERIFY(empty.pageForward().empty());
    QVERIFY(empty.pageBack().empty());
    QVERIFY(empty.nudge(1).empty());
    QCOMPARE(empty.lineRange().first, 0);
}

// Heights are the costly part: a long document is never read through.
void TestReadFlow::touchesAboutAScreen()
{
    FakeSource source;
    source.singles(100000);
    ReadFlow flow(&source);
    flow.setGeometry(geo(2, 500)); // 100 lines.
    source.touched.clear();
    flow.goToLine(50000);
    QVERIFY2(source.touched.size() < 130, qPrintable(QString::number(source.touched.size())));
    QCOMPARE(flow.screen().start.row, 50000);

    source.touched.clear();
    flow.pageForward();
    QVERIFY2(source.touched.size() < 130, qPrintable(QString::number(source.touched.size())));
    source.touched.clear();
    flow.nudge(1);
    QVERIFY2(source.touched.size() < 130, qPrintable(QString::number(source.touched.size())));
    source.touched.clear();
    flow.pageBack(); // Nothing remembered (the nudge): worked out.
    QVERIFY2(source.touched.size() < 400, qPrintable(QString::number(source.touched.size())));
    QVERIFY(flow.screen().start.row < 50000 + 100);
    source.touched.clear();
    flow.toEnd();
    QVERIFY2(source.touched.size() < 400, qPrintable(QString::number(source.touched.size())));
    QCOMPARE(flow.screen().end.row, 100000);
    source.touched.clear();
    flow.setGeometry(geo(3, 500));
    QVERIFY2(source.touched.size() < 400, qPrintable(QString::number(source.touched.size())));
    source.touched.clear();
    flow.toStart();
    QVERIFY2(source.touched.size() < 200, qPrintable(QString::number(source.touched.size())));
}

QTEST_APPLESS_MAIN(TestReadFlow)
#include "test_readflow.moc"
