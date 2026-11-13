// What a block is for collapsing, the text of its chip, the hint on a folded
// heading and the extent of a section (markdown/Folding.h). The blocks come
// from the real parser.
#include "markdown/Folding.h"

#include <QTest>

#include <vector>

using namespace vn;
using namespace vn::folding;

namespace {

rust::Vec<core::Block> parse(const QString &markdown)
{
    const QByteArray utf8 = markdown.toUtf8();
    return core::blocks(rust::Str(utf8.constData(), utf8.size()), core::RenderOptions{false, true, true, true, 1});
}

std::span<const core::Block> all(const rust::Vec<core::Block> &blocks)
{
    return {blocks.data(), blocks.size()};
}

// The only block of a document that is one block.
core::Block only(const QString &markdown)
{
    const auto blocks = parse(markdown);
    return blocks.size() == 1 ? blocks[0] : core::Block{};
}

} // namespace

class TestFolding : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void classifiesEachKind_data()
    {
        QTest::addColumn<QString>("markdown");
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("level");
        const auto kind = [](FoldKind k) { return int(k); };
        QTest::newRow("table") << "| a | b |\n|---|---|\n| 1 | 2 |\n" << kind(FoldKind::Table) << 0;
        QTest::newRow("raw table") << "<table><tr><td>x</td></tr></table>\n" << kind(FoldKind::Table) << 0;
        QTest::newRow("image") << "![A cat](cat.png)\n" << kind(FoldKind::Figure) << 0;
        QTest::newRow("image with a title") << "![A cat](cat.png \"Tom\")\n" << kind(FoldKind::Figure) << 0;
        QTest::newRow("linked image") << "[![A cat](cat.png)](https://example.com)\n" << kind(FoldKind::Figure) << 0;
        QTest::newRow("raw image") << "<p align=\"center\">\n  <img src=\"logo.png\" alt=\"Logo\">\n</p>\n"
                                   << kind(FoldKind::Figure) << 0;
        QTest::newRow("image and text") << "A cat: ![A cat](cat.png)\n" << kind(FoldKind::Other) << 0;
        QTest::newRow("text after image") << "![A cat](cat.png) is Tom\n" << kind(FoldKind::Other) << 0;
        QTest::newRow("inline formula") << "$x^2$\n" << kind(FoldKind::Other) << 0;
        QTest::newRow("equation") << "$$\nx^2 + y^2 = z^2\n$$\n" << kind(FoldKind::Equation) << 0;
        QTest::newRow("fenced code") << "```bash\nls\n```\n" << kind(FoldKind::Code) << 0;
        QTest::newRow("indented code") << "    ls\n" << kind(FoldKind::Code) << 0;
        QTest::newRow("mermaid") << "```mermaid\ngraph TD\n  A --> B\n```\n" << kind(FoldKind::Figure) << 0;
        QTest::newRow("Mermaid in capitals") << "```Mermaid\ngraph TD\n```\n" << kind(FoldKind::Figure) << 0;
        QTest::newRow("paragraph") << "Plain text.\n" << kind(FoldKind::Other) << 0;
        QTest::newRow("list") << "- one\n- two\n" << kind(FoldKind::Other) << 0;
        QTest::newRow("quote") << "> said\n" << kind(FoldKind::Other) << 0;
        for (int level = 1; level <= 6; ++level)
            QTest::newRow(qPrintable(QStringLiteral("heading %1").arg(level)))
                << QString(level, QLatin1Char('#')) + QStringLiteral(" Title\n") << kind(FoldKind::Heading) << level;
        QTest::newRow("setext heading") << "Title\n=====\n" << kind(FoldKind::Heading) << 1;
        QTest::newRow("raw heading") << "<h3 id=\"x\">Title</h3>\n" << kind(FoldKind::Heading) << 3;
    }

    void classifiesEachKind()
    {
        QFETCH(QString, markdown);
        QFETCH(int, kind);
        QFETCH(int, level);
        const Fold fold = classify(only(markdown));
        QCOMPARE(int(fold.kind), kind);
        QCOMPARE(fold.level, level);
    }

    void frontMatterAndHiddenBlocksAreOther()
    {
        const auto blocks = parse(QStringLiteral("---\ntitle: x\n---\n\n[ref]: https://example.com\n\ntext\n"));
        for (const core::Block &block : blocks)
            QCOMPARE(int(classify(block).kind), int(FoldKind::Other));
    }

    void tableChip()
    {
        const Chip chip = chipText(only(QStringLiteral("| Stage | Median | p95 | Failures | Cached |\n"
                                                       "|---|--:|--:|--:|---|\n"
                                                       "| Checkout | 4 s | 9 s | 0.1 % | yes |\n"
                                                       "| Configure | 11 s | 18 s | 0.4 % | yes |\n"
                                                       "| Compile | 142 s | 301 s | 1.9 % | partly |\n"
                                                       "| Test | 96 s | 188 s | 3.2 % | no |\n"
                                                       "| Package | 23 s | 41 s | 0.2 % | no |\n")));
        // The mockup's: 5 columns, the header and 5 rows.
        QCOMPARE(chip.label, QStringLiteral("Table · 5 × 6"));
        QCOMPARE(chip.detail, QStringLiteral("Stage, Median, p95, Failures, Cached"));
    }

    void tableChipHeaderOnly()
    {
        const Chip chip = chipText(only(QStringLiteral("| a & b | `c` | *d* |\n|---|---|---|\n")));
        QCOMPARE(chip.label, QStringLiteral("Table · 3 × 1"));
        QCOMPARE(chip.detail, QStringLiteral("a & b, c, d"));
    }

    void tableChipCutsLongHeaders()
    {
        QString header = QStringLiteral("|");
        QString rule = QStringLiteral("|");
        for (const char *name : {"Run", "Branch", "Commit", "Author", "Duration", "Status", "Runner", "Artifacts"}) {
            header += QStringLiteral(" %1 |").arg(QLatin1String(name));
            rule += QStringLiteral("---|");
        }
        const Chip chip = chipText(only(header + QLatin1Char('\n') + rule + QStringLiteral("\n| 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |\n")));
        QCOMPARE(chip.label, QStringLiteral("Table · 8 × 2"));
        QCOMPARE(chip.detail, QStringLiteral("Run, Branch, Commit, Author, Duration, Status, Runner…"));
        QVERIFY(chip.detail.size() <= 61);
    }

    void tableChipCutsALongFirstCell()
    {
        const Chip chip = chipText(only(QStringLiteral("| %1 |\n|---|\n").arg(QString(100, QLatin1Char('w')))));
        QCOMPARE(chip.detail.size(), 60);
        QVERIFY(chip.detail.endsWith(QChar(0x2026)));
    }

    void tableChipIgnoresNestedTables()
    {
        const Chip chip = chipText(only(QStringLiteral(
            "<table><tr><th>a</th><th>b</th></tr><tr><td>1</td><td><table><tr><td>x</td><td>y</td><td>z</td></tr>"
            "</table></td></tr></table>\n")));
        QCOMPARE(chip.label, QStringLiteral("Table · 2 × 2"));
        QCOMPARE(chip.detail, QStringLiteral("a, b"));
    }

    void tableChipWritesFormulasAsTeX()
    {
        const Chip chip = chipText(only(QStringLiteral("| $x^2$ | b |\n|---|---|\n")));
        QCOMPARE(chip.detail, QStringLiteral("x^2, b"));
    }

    void figureChip()
    {
        Chip chip = chipText(only(QStringLiteral("![The runner dashboard during a full build](dash.png)\n")));
        QCOMPARE(chip.label, QStringLiteral("Figure"));
        QCOMPARE(chip.detail, QStringLiteral("The runner dashboard during a full build"));
        chip = chipText(only(QStringLiteral("![Tom &amp; Jerry](a/b/cats.png?size=2)\n")));
        QCOMPARE(chip.detail, QStringLiteral("Tom & Jerry"));
        // No alt text: the title, then the file's name.
        chip = chipText(only(QStringLiteral("![](a/b/cats.png \"Tom\")\n")));
        QCOMPARE(chip.detail, QStringLiteral("Tom"));
        chip = chipText(only(QStringLiteral("![](a/b/cats.png?size=2)\n")));
        QCOMPARE(chip.detail, QStringLiteral("cats.png"));
    }

    void diagramChip()
    {
        const Chip chip = chipText(only(QStringLiteral("```mermaid\n\nflowchart TD\n  A --> B\n```\n")));
        QCOMPARE(chip.label, QStringLiteral("Diagram"));
        QCOMPARE(chip.detail, QStringLiteral("flowchart TD"));
    }

    void codeChip()
    {
        Chip chip = chipText(only(QStringLiteral("```bash\ncmake -S . -B build -DCACHE_DIR=/var/cache/ci\ncmake --build build\nctest\n```\n")));
        QCOMPARE(chip.label, QStringLiteral("Code · bash · 3 lines"));
        QCOMPARE(chip.detail, QStringLiteral("cmake -S . -B build -DCACHE_DIR=/var/cache/ci"));
        chip = chipText(only(QStringLiteral("```py\nprint(1)\n```\n")));
        QCOMPARE(chip.label, QStringLiteral("Code · py · 1 line"));
        // No language, a blank first line, and only the first word of the info.
        chip = chipText(only(QStringLiteral("```\n\n  indented\nx\n```\n")));
        QCOMPARE(chip.label, QStringLiteral("Code · 3 lines"));
        QCOMPARE(chip.detail, QStringLiteral("indented"));
        chip = chipText(only(QStringLiteral("```cpp title=x\nint a;\n```\n")));
        QCOMPARE(chip.label, QStringLiteral("Code · cpp · 1 line"));
        chip = chipText(only(QStringLiteral("    one\n    two\n")));
        QCOMPARE(chip.label, QStringLiteral("Code · 2 lines"));
    }

    void codeChipCutsLongLines()
    {
        const Chip chip = chipText(only(QStringLiteral("```\n%1\n```\n").arg(QString(100, QLatin1Char('x')))));
        QCOMPARE(chip.detail.size(), 60);
        QVERIFY(chip.detail.endsWith(QChar(0x2026)));
    }

    void equationChip()
    {
        Chip chip = chipText(only(QStringLiteral("$$\nT(n, k) = t_0 +\n  n/k \\cdot t_c\n$$\n")));
        QCOMPARE(chip.label, QStringLiteral("Equation"));
        QCOMPARE(chip.detail, QStringLiteral("T(n, k) = t_0 + n/k \\cdot t_c"));
        chip = chipText(only(QStringLiteral("$$\n%1\n$$\n").arg(QString(100, QLatin1Char('x')))));
        QCOMPARE(chip.detail.size(), 60);
        QVERIFY(chip.detail.endsWith(QChar(0x2026)));
    }

    void otherBlocksHaveNoChip()
    {
        for (const char *markdown : {"text\n", "# Title\n", "- a\n"}) {
            const Chip chip = chipText(only(QLatin1String(markdown)));
            QVERIFY(chip.label.isEmpty());
            QVERIFY(chip.detail.isEmpty());
        }
    }

    void hintCountsAndPluralises()
    {
        QCOMPARE(sectionHint(all(parse(QStringLiteral("a\n\nb\n\nc\n")))), QStringLiteral("3 paragraphs"));
        QCOMPARE(sectionHint(all(parse(QStringLiteral("a\n")))), QStringLiteral("1 paragraph"));
        QCOMPARE(sectionHintLine(all(parse(QStringLiteral("a\n\nb\n\nc\n")))), QStringLiteral("· 3 paragraphs hidden"));
        QCOMPARE(sectionHintLine(all(parse(QStringLiteral("- a\n\nb\n")))), QStringLiteral("· 1 list, 1 paragraph hidden"));
    }

    void hintKeepsOrderOfFirstAppearance()
    {
        QCOMPARE(sectionHint(all(parse(QStringLiteral("- a\n\nb\n\n- c\n\nd\n\ne\n")))),
                 QStringLiteral("2 lists, 3 paragraphs"));
        QCOMPARE(sectionHint(all(parse(QStringLiteral("| a |\n|---|\n\nx\n\ny\n")))),
                 QStringLiteral("1 table, 2 paragraphs"));
        QCOMPARE(sectionHint(all(parse(QStringLiteral("x\n\n| a |\n|---|\n\ny\n")))),
                 QStringLiteral("2 paragraphs, 1 table"));
    }

    void hintNamesEachKind()
    {
        const auto blocks = parse(QStringLiteral("text\n\n- list\n\n> quote\n\n---\n\n| a |\n|---|\n\n![x](x.png)\n\n"
                                                 "$$\ne\n$$\n\n```\ncode\n```\n\n```mermaid\ngraph TD\n```\n\n"
                                                 "### Sub\n\n<details>\nraw\n</details>\n"));
        QCOMPARE(sectionHint(all(blocks)),
                 QStringLiteral("1 paragraph, 1 list, 1 quote, 1 rule, 1 table, 2 figures, 1 equation, 1 code block, "
                                "1 heading, 1 other block"));
    }

    void hintLeavesOutHiddenBlocksAndIsEmptyWithoutBlocks()
    {
        QCOMPARE(sectionHint(all(parse(QStringLiteral("[ref]: https://example.com\n")))), QString());
        QCOMPARE(sectionHintLine(all(parse(QStringLiteral("[ref]: https://example.com\n")))), QString());
        QCOMPARE(sectionHint({}), QString());
        QCOMPARE(sectionHint(all(parse(QStringLiteral("[ref]: https://example.com\n\ntext\n")))), QStringLiteral("1 paragraph"));
    }

    void sectionEndStopsAtSameOrHigherLevel()
    {
        // 0 H1, 1 p, 2 H2, 3 p, 4 H3, 5 p, 6 H2, 7 p, 8 H1, 9 p
        const auto blocks = parse(QStringLiteral("# A\n\np\n\n## B\n\np\n\n### C\n\np\n\n## D\n\np\n\n# E\n\np\n"));
        QCOMPARE(int(blocks.size()), 10);
        QCOMPARE(sectionEnd(all(blocks), 0), 8);
        QCOMPARE(sectionEnd(all(blocks), 2), 6); // Past its H3, up to the next H2.
        QCOMPARE(sectionEnd(all(blocks), 4), 6); // The H3 ends where its H2 does.
        QCOMPARE(sectionEnd(all(blocks), 6), 8);
        QCOMPARE(sectionEnd(all(blocks), 8), 10); // To the end of the document.
    }

    void sectionEndEdgeCases()
    {
        // A heading right before one of its own level (or higher) has nothing under it.
        auto blocks = parse(QStringLiteral("## A\n## B\n\n# C\n"));
        QCOMPARE(sectionEnd(all(blocks), 0), 1);
        QCOMPARE(sectionEnd(all(blocks), 1), 2);
        QCOMPARE(sectionEnd(all(blocks), 2), 3);
        // A deeper heading does not end it; the last heading of the document runs to its end.
        blocks = parse(QStringLiteral("# A\n###### deep\n\nx\n"));
        QCOMPARE(sectionEnd(all(blocks), 0), 3);
        QCOMPARE(sectionEnd(all(blocks), 1), 3);
        // Text before the first heading belongs to no section; a block that is not a heading has none.
        blocks = parse(QStringLiteral("intro\n\n# A\n\nx\n"));
        QCOMPARE(sectionEnd(all(blocks), 0), 1);
        QCOMPARE(sectionEnd(all(blocks), 1), 3);
        // Out of range.
        QCOMPARE(sectionEnd(all(blocks), 3), 3);
        QCOMPARE(sectionEnd(all(blocks), -5), 0);
        QCOMPARE(sectionEnd({}, 0), 0);
    }

    void sectionEndWorksWithHint()
    {
        const auto blocks = parse(QStringLiteral("## Rollout\n\none\n\ntwo\n\nthree\n\n## Next\n"));
        const std::span<const core::Block> span = all(blocks);
        const int end = sectionEnd(span, 0);
        QCOMPARE(end, 4);
        QCOMPARE(sectionHintLine(span.subspan(1, end - 1)), QStringLiteral("· 3 paragraphs hidden"));
    }
};

QTEST_MAIN(TestFolding)
#include "test_folding.moc"
