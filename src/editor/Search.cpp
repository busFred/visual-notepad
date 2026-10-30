#include "Search.h"

#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>

namespace vn::search {

namespace {

QRegularExpression regexFor(const Options &options)
{
    QRegularExpression::PatternOptions flags = QRegularExpression::UseUnicodePropertiesOption;
    if (!options.caseSensitive)
        flags |= QRegularExpression::CaseInsensitiveOption;
    return QRegularExpression(options.pattern, flags);
}

QString applyCase(const QString &match, const QString &replacement)
{
    if (match.isEmpty() || replacement.isEmpty())
        return replacement;
    if (match == match.toUpper() && match != match.toLower())
        return replacement.toUpper();
    if (match == match.toLower() && match != match.toUpper())
        return replacement.toLower();
    if (match.at(0).isUpper() && match.mid(1) == match.mid(1).toLower())
        return replacement.at(0).toUpper() + replacement.mid(1).toLower();
    return replacement;
}

} // namespace

Compiled::Compiled(const Options &options)
    : m_options(options)
{
    if (options.regex) {
        m_regex = regexFor(options);
        m_valid = m_regex.isValid();
    }
}

bool isValid(const Options &options)
{
    return Compiled(options).isValid();
}

QList<Range> findAll(QTextDocument *document, const Compiled &query, int scopeStart, int scopeEnd)
{
    QList<Range> matches;
    const Options &options = query.options();
    if (options.pattern.isEmpty() || !query.isValid())
        return matches;
    const Qt::CaseSensitivity sensitivity = options.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const auto isWord = [](const QString &text, int start, int end) {
        // Same test as QTextDocument::FindWholeWords.
        return (start == 0 || !text.at(start - 1).isLetterOrNumber())
               && (end == text.size() || !text.at(end).isLetterOrNumber());
    };

    // One pass over the blocks; QTextDocument::find would copy a block's text
    // for every match.
    const int from = scopeStart >= 0 ? scopeStart : 0;
    QTextBlock block = document->findBlock(from);
    int offset = from - block.position();
    for (; block.isValid(); block = block.next(), offset = 0) {
        const int base = block.position();
        if (scopeEnd >= 0 && base > scopeEnd)
            break;
        if (block.length() <= 1)
            continue; // Empty line: nothing non-empty can match.
        QString text = block.text();
        text.replace(QChar::Nbsp, u' ');
        int pos = offset;
        while (pos >= 0 && pos <= text.size()) {
            int start;
            int end;
            if (options.regex) {
                const QRegularExpressionMatch m = query.regex().match(text, pos);
                if (!m.hasMatch())
                    break;
                start = m.capturedStart();
                end = m.capturedEnd();
            } else {
                start = text.indexOf(options.pattern, pos, sensitivity);
                if (start < 0)
                    break;
                end = start + options.pattern.size();
            }
            if (options.wholeWord && !isWord(text, start, end)) {
                pos = options.regex ? start + 1 : end + 1;
                continue;
            }
            if (scopeEnd >= 0 && base + end > scopeEnd)
                return matches;
            if (end > start) {
                matches.append({base + start, base + end});
                if (matches.size() >= maxMatches)
                    return matches;
                pos = end;
            } else {
                pos = start + 1; // Empty regex match (e.g. ^): step past it.
            }
        }
    }
    return matches;
}

QList<Range> findAll(QTextDocument *document, const Options &options, int scopeStart, int scopeEnd)
{
    return findAll(document, Compiled(options), scopeStart, scopeEnd);
}

QString replacementFor(QTextDocument *document, const Compiled &query, Range match, const QString &replacement,
                       bool preserveCase, LineText *line)
{
    if (!query.options().regex && !preserveCase)
        return replacement; // Neither the match nor its line is needed.
    const QTextBlock block = document->findBlock(match.start);
    LineText local;
    LineText &cache = line ? *line : local;
    if (cache.blockPosition != block.position()) {
        cache.blockPosition = block.position();
        cache.text = block.text();
    }
    const int offset = match.start - block.position();
    const QString matched = cache.text.mid(offset, match.end - match.start);
    QString result;
    if (!query.options().regex) {
        result = replacement;
    } else {
        // Re-run the regex on the match's line to get its groups.
        const QRegularExpressionMatch m =
            query.regex().match(cache.text, offset, QRegularExpression::NormalMatch,
                                QRegularExpression::AnchorAtOffsetMatchOption);
        for (int i = 0; i < replacement.size(); ++i) {
            const QChar c = replacement.at(i);
            const QChar next = i + 1 < replacement.size() ? replacement.at(i + 1) : QChar();
            if (c == u'$' && next.isDigit()) {
                result += m.hasMatch() ? m.captured(next.digitValue()) : QString();
                ++i;
            } else if (c == u'$' && next == u'&') {
                result += matched;
                ++i;
            } else if (c == u'\\' && next == u'n') {
                result += u'\n';
                ++i;
            } else if (c == u'\\' && next == u't') {
                result += u'\t';
                ++i;
            } else if (c == u'\\' && next == u'\\') {
                result += u'\\';
                ++i;
            } else {
                result += c;
            }
        }
    }
    return preserveCase ? applyCase(matched, result) : result;
}

} // namespace vn::search

namespace vn::search {

QString replacementFor(QTextDocument *document, const Options &options, Range match, const QString &replacement,
                       bool preserveCase)
{
    return replacementFor(document, Compiled(options), match, replacement, preserveCase);
}

} // namespace vn::search
