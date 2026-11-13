#include "markdown/Folding.h"

#include <QCoreApplication>
#include <QList>
#include <QRegularExpression>
#include <QStringList>
#include <QTextDocumentFragment>

#include <algorithm>
#include <array>

namespace vn::folding {

namespace {

// Longest chip detail kept (in characters); the view elides it to its width.
constexpr int maxDetail = 60;

QString fromRust(const rust::String &s)
{
    return QString::fromUtf8(s.data(), s.size());
}

QString translate(const char *text)
{
    return QCoreApplication::translate("vn::folding", text);
}

// The text cut to `max` characters, ending in an ellipsis if it was longer.
QString elide(const QString &text, int max)
{
    if (text.size() <= max)
        return text;
    qsizetype cut = max - 1;
    if (cut > 0 && text.at(cut).isLowSurrogate()) // Not between the halves of a pair.
        --cut;
    return text.left(cut).trimmed() + QChar(0x2026);
}

bool isMermaid(const core::Block &block)
{
    return block.kind == core::BlockKind::Code
           && fromRust(block.lang).trimmed().compare(QLatin1String("mermaid"), Qt::CaseInsensitive) == 0;
}

// One tag of the block's HTML, found by scanning (the HTML is what the parser
// wrote, or a raw HTML block of the document: Qt's own parser is too heavy
// for something that runs for every block).
struct Tag
{
    QString name; // Lower case.
    bool closing = false;
    qsizetype begin = 0; // Of the '<'.
    qsizetype end = 0;   // After the '>'.
    QString attributes;
};

// The first tag at or after `from`, past comments and any '<' that is text.
bool nextTag(const QString &html, qsizetype from, Tag *tag)
{
    const qsizetype size = html.size();
    while (true) {
        const qsizetype lt = html.indexOf(QLatin1Char('<'), from);
        if (lt < 0)
            return false;
        if (html.mid(lt, 4) == QLatin1String("<!--")) {
            const qsizetype close = html.indexOf(QLatin1String("-->"), lt + 4);
            if (close < 0)
                return false;
            from = close + 3;
            continue;
        }
        qsizetype i = lt + 1;
        bool closing = false;
        if (i < size && html.at(i) == QLatin1Char('/')) {
            closing = true;
            ++i;
        }
        const qsizetype nameStart = i;
        while (i < size && html.at(i).isLetterOrNumber())
            ++i;
        if (i == nameStart) { // "a < b", "<!doctype".
            from = lt + 1;
            continue;
        }
        const qsizetype attributesStart = i;
        QChar quote;
        for (; i < size; ++i) {
            const QChar c = html.at(i);
            if (quote.isNull()) {
                if (c == QLatin1Char('"') || c == QLatin1Char('\''))
                    quote = c;
                else if (c == QLatin1Char('>'))
                    break;
            } else if (c == quote) {
                quote = QChar();
            }
        }
        if (i >= size)
            return false;
        tag->name = html.mid(nameStart, attributesStart - nameStart).toLower();
        tag->closing = closing;
        tag->begin = lt;
        tag->end = i + 1;
        tag->attributes = html.mid(attributesStart, i - attributesStart);
        return true;
    }
}

// The text of an HTML fragment as one line (entities decoded).
QString plain(const QString &fragment)
{
    return QTextDocumentFragment::fromHtml(fragment).toPlainText().simplified();
}

QString attribute(const Tag &tag, const char *name)
{
    const QRegularExpression pattern(QStringLiteral("(?:^|\\s)%1\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s\"'>]+))")
                                         .arg(QLatin1String(name)),
                                     QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = pattern.match(tag.attributes);
    if (!m.hasMatch())
        return {};
    for (int group = 1; group <= 3; ++group) {
        if (m.hasCaptured(group))
            return plain(m.captured(group));
    }
    return {};
}

// The block's HTML with the TeX of its inline formulas (images that name an
// index in the block) in their places, so a cell's text reads as TeX.
QString htmlWithTeX(const core::Block &block)
{
    QString html = fromRust(block.html);
    if (block.inline_math.empty() || !html.contains(QLatin1String("vn-math")))
        return html;
    static const QRegularExpression image(QStringLiteral("<img[^>]*src=\"vn-math(?:-display)?:(\\d+)\"[^>]*>"));
    QString result;
    qsizetype last = 0;
    for (auto it = image.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        result += QStringView(html).mid(last, m.capturedStart() - last);
        const size_t index = m.captured(1).toULongLong();
        if (index < block.inline_math.size())
            result += fromRust(block.inline_math[index]).toHtmlEscaped();
        last = m.capturedEnd();
    }
    return result + QStringView(html).mid(last);
}

// The first tag of the HTML, if there is only whitespace and comments before it.
bool firstTag(const QString &html, Tag *tag)
{
    if (!nextTag(html, 0, tag) || tag->closing)
        return false;
    static const QRegularExpression comment(QStringLiteral("<!--.*?-->"), QRegularExpression::DotMatchesEverythingOption);
    return html.left(tag->begin).remove(comment).trimmed().isEmpty();
}

// A picture on its own: only pictures (not formulas) in plain wrappers, no
// text. Fills the text that names the first one.
struct Picture
{
    int count = 0;
    QString name;
};

bool isPictureOnly(const QString &html, Picture *picture)
{
    static constexpr std::array wrappers{QLatin1String("p"), QLatin1String("div"), QLatin1String("center"),
                                         QLatin1String("a"), QLatin1String("br"),  QLatin1String("span")};
    Tag tag;
    qsizetype pos = 0;
    while (nextTag(html, pos, &tag)) {
        if (!QStringView(html).mid(pos, tag.begin - pos).trimmed().isEmpty())
            return false; // Text beside the picture.
        pos = tag.end;
        if (tag.name == QLatin1String("img")) {
            const QString src = attribute(tag, "src");
            if (src.startsWith(QLatin1String("vn-math")))
                return false;
            if (picture->count++ == 0) {
                picture->name = attribute(tag, "alt");
                if (picture->name.isEmpty())
                    picture->name = attribute(tag, "title");
                if (picture->name.isEmpty()) // The file's name.
                    picture->name = src.left(src.indexOf(QRegularExpression(QStringLiteral("[?#]")))).section(
                        QLatin1Char('/'), -1);
            }
        } else if (std::find(wrappers.begin(), wrappers.end(), QLatin1String(tag.name.toLatin1())) == wrappers.end()) {
            return false;
        }
    }
    return picture->count > 0 && QStringView(html).mid(pos).trimmed().isEmpty();
}

// The blocks of the HTML that stand for something collapsible, from its first tag.
Fold classifyHtml(const QString &html)
{
    Tag first;
    if (!firstTag(html, &first))
        return {};
    const QString &name = first.name;
    if (name == QLatin1String("table"))
        return {FoldKind::Table, 0};
    if (name.size() == 2 && name.at(0) == QLatin1Char('h') && name.at(1) >= QLatin1Char('1')
        && name.at(1) <= QLatin1Char('6'))
        return {FoldKind::Heading, name.at(1).digitValue()};
    if (name == QLatin1String("p") || name == QLatin1String("div") || name == QLatin1String("center")
        || name == QLatin1String("a") || name == QLatin1String("img")) {
        Picture picture;
        if (isPictureOnly(html, &picture))
            return {FoldKind::Figure, 0};
    }
    return {};
}

struct TableShape
{
    int columns = 0;
    int rows = 0;
    QStringList header;
};

// Cells and rows of the outermost table (cells of tables inside cells do not
// count); the cells of its first row are the header.
TableShape tableShape(const core::Block &block)
{
    const QString html = htmlWithTeX(block);
    TableShape shape;
    Tag tag;
    qsizetype pos = 0;
    int depth = 0;
    int cells = 0;
    qsizetype cellStart = -1;
    const auto endCell = [&](qsizetype at) {
        if (cellStart >= 0 && shape.rows == 1)
            shape.header << plain(html.mid(cellStart, at - cellStart));
        cellStart = -1;
    };
    while (nextTag(html, pos, &tag)) {
        pos = tag.end;
        if (tag.name == QLatin1String("table")) {
            if (!tag.closing) {
                ++depth;
            } else if (depth == 1) {
                endCell(tag.begin);
                break;
            } else {
                --depth;
            }
            continue;
        }
        if (depth != 1)
            continue;
        if (tag.name == QLatin1String("tr")) {
            endCell(tag.begin);
            if (!tag.closing) {
                ++shape.rows;
                cells = 0;
            }
        } else if (tag.name == QLatin1String("td") || tag.name == QLatin1String("th")) {
            endCell(tag.begin);
            if (!tag.closing) {
                shape.columns = std::max(shape.columns, ++cells);
                cellStart = tag.end;
            }
        }
    }
    endCell(html.size());
    return shape;
}

// The header cells in a row of at most maxDetail characters, ", " between
// them; an ellipsis after the last that fits if more were left out.
QString joinCells(const QStringList &cells)
{
    QString text;
    for (const QString &cell : cells) {
        if (cell.isEmpty())
            continue;
        const QString next = text.isEmpty() ? cell : text + QStringLiteral(", ") + cell;
        if (next.size() > maxDetail) {
            if (text.isEmpty())
                return elide(cell, maxDetail);
            return text + QChar(0x2026);
        }
        text = next;
    }
    return text;
}

QString linesText(int n)
{
    return n == 1 ? translate("1 line") : translate("%1 lines").arg(n);
}

QString firstNonBlankLine(const QString &text)
{
    for (const QStringView line : QStringView(text).split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed().toString();
        if (!trimmed.isEmpty())
            return trimmed;
    }
    return {};
}

// What the hint calls a block; the order is the enum's.
enum Noun { Paragraph, List, Table, Figure, Equation, CodeBlock, Heading, Quote, Rule, Other, nounCount };

struct NounText
{
    const char *one;
    const char *many;
};

constexpr std::array<NounText, nounCount> nounTexts{{
    {QT_TRANSLATE_NOOP("vn::folding", "1 paragraph"), QT_TRANSLATE_NOOP("vn::folding", "%1 paragraphs")},
    {QT_TRANSLATE_NOOP("vn::folding", "1 list"), QT_TRANSLATE_NOOP("vn::folding", "%1 lists")},
    {QT_TRANSLATE_NOOP("vn::folding", "1 table"), QT_TRANSLATE_NOOP("vn::folding", "%1 tables")},
    {QT_TRANSLATE_NOOP("vn::folding", "1 figure"), QT_TRANSLATE_NOOP("vn::folding", "%1 figures")},
    {QT_TRANSLATE_NOOP("vn::folding", "1 equation"), QT_TRANSLATE_NOOP("vn::folding", "%1 equations")},
    {QT_TRANSLATE_NOOP("vn::folding", "1 code block"), QT_TRANSLATE_NOOP("vn::folding", "%1 code blocks")},
    {QT_TRANSLATE_NOOP("vn::folding", "1 heading"), QT_TRANSLATE_NOOP("vn::folding", "%1 headings")},
    {QT_TRANSLATE_NOOP("vn::folding", "1 quote"), QT_TRANSLATE_NOOP("vn::folding", "%1 quotes")},
    {QT_TRANSLATE_NOOP("vn::folding", "1 rule"), QT_TRANSLATE_NOOP("vn::folding", "%1 rules")},
    {QT_TRANSLATE_NOOP("vn::folding", "1 other block"), QT_TRANSLATE_NOOP("vn::folding", "%1 other blocks")},
}};

// The noun of a block, or -1 for one that shows nothing (link definitions).
int nounOf(const core::Block &block)
{
    if (block.kind == core::BlockKind::Hidden)
        return -1;
    switch (classify(block).kind) {
    case FoldKind::Table:
        return Table;
    case FoldKind::Figure:
        return Figure;
    case FoldKind::Equation:
        return Equation;
    case FoldKind::Code:
        return CodeBlock;
    case FoldKind::Heading:
        return Heading;
    case FoldKind::Other:
        break;
    }
    if (block.kind != core::BlockKind::Html)
        return Other;
    Tag first;
    if (!firstTag(fromRust(block.html), &first))
        return Other;
    if (first.name == QLatin1String("p"))
        return Paragraph;
    if (first.name == QLatin1String("ul") || first.name == QLatin1String("ol"))
        return List;
    if (first.name == QLatin1String("blockquote"))
        return Quote;
    if (first.name == QLatin1String("hr"))
        return Rule;
    return Other;
}

} // namespace

Fold classify(const core::Block &block)
{
    switch (block.kind) {
    case core::BlockKind::Math:
        return {FoldKind::Equation, 0};
    case core::BlockKind::Code:
        return {isMermaid(block) ? FoldKind::Figure : FoldKind::Code, 0};
    case core::BlockKind::Html:
        return classifyHtml(fromRust(block.html));
    default:
        return {};
    }
}

Chip chipText(const core::Block &block)
{
    switch (classify(block).kind) {
    case FoldKind::Table: {
        const TableShape shape = tableShape(block);
        Chip chip;
        chip.label = shape.rows > 0 ? translate("Table · %1 × %2").arg(shape.columns).arg(shape.rows)
                                    : translate("Table");
        chip.detail = joinCells(shape.header);
        return chip;
    }
    case FoldKind::Figure: {
        if (isMermaid(block))
            return {translate("Diagram"), elide(firstNonBlankLine(fromRust(block.text)), maxDetail)};
        Picture picture;
        isPictureOnly(fromRust(block.html), &picture);
        return {translate("Figure"), elide(picture.name, maxDetail)};
    }
    case FoldKind::Equation:
        return {translate("Equation"), elide(fromRust(block.text).simplified(), maxDetail)};
    case FoldKind::Code: {
        QString text = fromRust(block.text);
        while (text.endsWith(QLatin1Char('\n')) || text.endsWith(QLatin1Char('\r')))
            text.chop(1);
        const int lines = text.isEmpty() ? 0 : int(text.count(QLatin1Char('\n'))) + 1;
        const QString lang = fromRust(block.lang).trimmed().section(QLatin1Char(' '), 0, 0);
        Chip chip;
        chip.label = lang.isEmpty() ? translate("Code · %1").arg(linesText(lines))
                                    : translate("Code · %1 · %2").arg(lang, linesText(lines));
        chip.detail = elide(firstNonBlankLine(text).replace(QLatin1Char('\t'), QLatin1Char(' ')), maxDetail);
        return chip;
    }
    case FoldKind::Heading:
    case FoldKind::Other:
        break;
    }
    return {};
}

QString sectionHint(std::span<const core::Block> hidden)
{
    std::array<int, nounCount> counts{};
    QList<int> order;
    for (const core::Block &block : hidden) {
        const int noun = nounOf(block);
        if (noun < 0)
            continue;
        if (counts[noun]++ == 0)
            order.append(noun);
    }
    QStringList parts;
    for (const int noun : order) {
        const NounText &text = nounTexts[noun];
        parts << (counts[noun] == 1 ? translate(text.one) : translate(text.many).arg(counts[noun]));
    }
    return parts.join(QStringLiteral(", "));
}

QString sectionHintLine(std::span<const core::Block> hidden)
{
    const QString hint = sectionHint(hidden);
    return hint.isEmpty() ? QString() : translate("· %1 hidden").arg(hint);
}

int sectionEnd(std::span<const core::Block> blocks, int heading)
{
    const int count = int(blocks.size());
    if (heading < 0 || heading >= count)
        return std::clamp(heading + 1, 0, count);
    const Fold start = classify(blocks[heading]);
    if (start.kind != FoldKind::Heading)
        return heading + 1;
    for (int i = heading + 1; i < count; ++i) {
        const Fold fold = classify(blocks[i]);
        if (fold.kind == FoldKind::Heading && fold.level <= start.level)
            return i;
    }
    return count;
}

} // namespace vn::folding
