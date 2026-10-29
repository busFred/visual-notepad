#include "editor/Indentation.h"

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>
#include <array>
#include <cstdlib>

namespace vn::indentation {

namespace {

struct SpacesDiff
{
    int spacesDiff = 0;
    bool looksLikeAlignment = false;
};

// Difference in spaces between two lines' indentation (spacesDiff in
// indentationGuesser.ts). aLength / bLength are the indentation lengths.
SpacesDiff spacesDiff(const QString &a, int aLength, const QString &b, int bLength)
{
    SpacesDiff result;

    // This can go both ways (e.g.): a = "\t", b = "\t    " counts 1 tab and
    // 4 spaces.
    int i = 0;
    for (; i < aLength && i < bLength; ++i) {
        if (a.at(i) != b.at(i))
            break;
    }

    int aSpaces = 0, aTabs = 0;
    for (int j = i; j < aLength; ++j)
        (a.at(j) == u' ' ? aSpaces : aTabs)++;
    int bSpaces = 0, bTabs = 0;
    for (int j = i; j < bLength; ++j)
        (b.at(j) == u' ' ? bSpaces : bTabs)++;

    if ((aSpaces > 0 && aTabs > 0) || (bSpaces > 0 && bTabs > 0))
        return result;

    const int tabsDiff = std::abs(aTabs - bTabs);
    const int spacesCount = std::abs(aSpaces - bSpaces);

    if (tabsDiff == 0) {
        // The difference might be alignment (const a = b + c,\n      d = ...),
        // which is not a hint for the indentation.
        result.spacesDiff = spacesCount;
        if (spacesCount > 0 && 0 <= bSpaces - 1 && bSpaces - 1 < a.size() && bSpaces < b.size()) {
            if (b.at(bSpaces) != u' ' && a.at(bSpaces - 1) == u' ' && a.at(a.size() - 1) == u',')
                result.looksLikeAlignment = true;
        }
        return result;
    }
    if (spacesCount % tabsDiff == 0)
        result.spacesDiff = spacesCount / tabsDiff;
    return result;
}

// Index of the first character that is not a space or tab; the length when
// there is none.
int indentationLength(const QString &line)
{
    int i = 0;
    while (i < line.size() && (line.at(i) == u' ' || line.at(i) == u'\t'))
        ++i;
    return i;
}

} // namespace

Settings guess(const QTextDocument *document, int defaultTabSize, bool defaultInsertSpaces)
{
    constexpr int maxLines = 10000;
    constexpr std::array<int, 7> allowedTabSizeGuesses = {2, 4, 6, 8, 3, 5, 7}; // prefer even
    constexpr int maxAllowedTabSizeGuess = 8;

    int linesIndentedWithTabs = 0;   // lines with at least one tab in the indentation
    int linesIndentedWithSpaces = 0; // lines with only (more than one) spaces
    QString previousLine;            // latest line that had non-whitespace characters
    int previousIndentation = 0;
    std::array<int, 9> spacesDiffCount{}; // tabSize scores

    int lineNumber = 0;
    for (QTextBlock block = document->begin(); block.isValid() && lineNumber < maxLines;
         block = block.next(), ++lineNumber) {
        const QString line = block.text();
        int tabs = 0, spaces = 0, indentation = -1;
        for (int j = 0; j < line.size(); ++j) {
            const QChar c = line.at(j);
            if (c == u'\t') {
                ++tabs;
            } else if (c == u' ') {
                ++spaces;
            } else {
                indentation = j;
                break;
            }
        }
        // Ignore empty or whitespace-only lines.
        if (indentation < 0)
            continue;

        if (tabs > 0)
            ++linesIndentedWithTabs;
        else if (spaces > 1)
            ++linesIndentedWithSpaces;

        const SpacesDiff diff = spacesDiff(previousLine, previousIndentation, line, indentation);
        // With defaultInsertSpaces and a diff equal to the tab size, an
        // alignment-looking line still counts (a nested "- item" list);
        // otherwise skip it entirely.
        if (diff.looksLikeAlignment && !(defaultInsertSpaces && defaultTabSize == diff.spacesDiff))
            continue;

        if (diff.spacesDiff <= maxAllowedTabSizeGuess)
            ++spacesDiffCount[diff.spacesDiff];
        previousLine = line;
        previousIndentation = indentation;
    }

    bool insertSpaces = defaultInsertSpaces;
    if (linesIndentedWithTabs != linesIndentedWithSpaces)
        insertSpaces = linesIndentedWithTabs < linesIndentedWithSpaces;

    int tabSize = defaultTabSize;
    // Guess the tab size only when inserting spaces.
    if (insertSpaces) {
        int score = 0;
        for (const int possible : allowedTabSizeGuesses) {
            if (spacesDiffCount[possible] > score) {
                score = spacesDiffCount[possible];
                tabSize = possible;
            }
        }
        // Let 2 win over 4 when it has at least 2/3 of the occurrences of 4
        // (YAML-like files with some deeper 4-space steps).
        if (tabSize == 4 && spacesDiffCount[4] > 0 && spacesDiffCount[2] > 0
            && spacesDiffCount[2] * 3 >= spacesDiffCount[4] * 2)
            tabSize = 2;
    }
    return {tabSize, insertSpaces};
}

bool convert(QTextDocument *document, int tabSize, bool toSpaces)
{
    tabSize = std::max(1, tabSize);
    const QString spaces(tabSize, u' ');

    struct Edit
    {
        int position;
        int length;
        QString text;
    };
    QList<Edit> edits;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        const QString line = block.text();
        const int length = indentationLength(line); // all of a whitespace-only line
        if (length == 0)
            continue;
        const QString original = line.left(length);
        QString converted = original;
        if (toSpaces)
            converted.replace(u'\t', spaces);
        else
            converted.replace(spaces, QStringLiteral("\t"));
        if (converted != original)
            edits.append({block.position(), length, converted});
    }
    if (edits.isEmpty())
        return false;

    // Back to front so earlier positions stay valid.
    QTextCursor cursor(document);
    cursor.beginEditBlock();
    for (auto it = edits.crbegin(); it != edits.crend(); ++it) {
        cursor.setPosition(it->position);
        cursor.setPosition(it->position + it->length, QTextCursor::KeepAnchor);
        cursor.insertText(it->text);
    }
    cursor.endEditBlock();
    return true;
}

} // namespace vn::indentation
