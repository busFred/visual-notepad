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

QTextDocument::FindFlags findFlags(const Options &options)
{
    QTextDocument::FindFlags flags;
    if (options.caseSensitive)
        flags |= QTextDocument::FindCaseSensitively;
    if (options.wholeWord)
        flags |= QTextDocument::FindWholeWords;
    return flags;
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

bool isValid(const Options &options)
{
    return !options.regex || regexFor(options).isValid();
}

QList<QTextCursor> findAll(QTextDocument *document, const Options &options, int scopeStart, int scopeEnd)
{
    QList<QTextCursor> matches;
    if (options.pattern.isEmpty() || !isValid(options))
        return matches;
    const QRegularExpression re = options.regex ? regexFor(options) : QRegularExpression();
    const auto flags = findFlags(options);
    int from = scopeStart >= 0 ? scopeStart : 0;
    while (matches.size() < maxMatches) {
        QTextCursor found = options.regex ? document->find(re, from, flags) : document->find(options.pattern, from, flags);
        if (found.isNull())
            break;
        if (scopeEnd >= 0 && found.selectionEnd() > scopeEnd)
            break;
        if (found.hasSelection()) {
            matches.append(found);
            from = found.selectionEnd();
        } else {
            // Empty regex match (e.g. ^): step past it.
            from = found.position() + 1;
            if (from >= document->characterCount())
                break;
        }
    }
    return matches;
}

QString replacementFor(const Options &options, const QTextCursor &match, const QString &replacement,
                       bool preserveCase)
{
    const QString matched = match.selectedText();
    QString result;
    if (!options.regex) {
        result = replacement;
    } else {
        // Re-run the regex on the match's line to get its groups.
        const QTextBlock block = match.document()->findBlock(match.selectionStart());
        const int offset = match.selectionStart() - block.position();
        const QRegularExpressionMatch m =
            regexFor(options).match(block.text(), offset, QRegularExpression::NormalMatch,
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
