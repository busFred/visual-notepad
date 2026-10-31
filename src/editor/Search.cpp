#include "Search.h"

#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>

#include <atomic>

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

namespace {

// Walks a QTextDocument's blocks.
class DocumentLines
{
public:
    explicit DocumentLines(QTextDocument *document)
        : m_document(document)
    {
    }
    // Starts at the line holding `from`; returns the offset of `from` in it.
    int seek(int from)
    {
        m_block = m_document->findBlock(from);
        load();
        return from - m_block.position();
    }
    bool valid() const { return m_block.isValid(); }
    int position() const { return m_block.position(); }
    bool empty() const { return m_block.length() <= 1; }
    QStringView text() const { return m_text; }
    void next()
    {
        m_block = m_block.next();
        load();
    }

private:
    void load()
    {
        if (!m_block.isValid() || m_block.length() <= 1) {
            m_text.clear();
            return;
        }
        m_text = m_block.text();
        m_text.replace(QChar::Nbsp, u' ');
    }

    QTextDocument *m_document;
    QTextBlock m_block;
    QString m_text;
};

bool isBlockSeparator(QChar c)
{
    // A QTextDocument's block separators, as stored in QTextDocument::toRawText().
    return c == QChar::ParagraphSeparator || c == QChar(0xfdd0) || c == QChar(0xfdd1);
}

// Walks the lines of a QTextDocument::toRawText() snapshot (no-break spaces
// already replaced); blocks are the runs between separators.
class TextLines
{
public:
    explicit TextLines(QStringView text)
        : m_all(text)
    {
    }
    int seek(int from)
    {
        from = qBound(0, from, int(m_all.size()));
        int start = from;
        while (start > 0 && !isBlockSeparator(m_all.at(start - 1)))
            --start;
        m_start = start;
        findEnd();
        return from - start;
    }
    bool valid() const { return m_start <= m_all.size(); }
    int position() const { return m_start; }
    bool empty() const { return m_end == m_start; }
    QStringView text() const { return m_all.mid(m_start, m_end - m_start); }
    void next()
    {
        m_start = m_end + 1;
        if (valid())
            findEnd();
    }

private:
    void findEnd()
    {
        int end = m_start;
        const int size = int(m_all.size());
        while (end < size && !isBlockSeparator(m_all.at(end)))
            ++end;
        m_end = end;
    }

    QStringView m_all;
    int m_start = 0;
    int m_end = 0;
};

template<class Lines>
QList<Range> scan(Lines &lines, const Compiled &query, int scopeStart, int scopeEnd, const std::atomic<bool> *cancel)
{
    QList<Range> matches;
    const Options &options = query.options();
    if (options.pattern.isEmpty() || !query.isValid())
        return matches;
    const Qt::CaseSensitivity sensitivity = options.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const auto isWord = [](QStringView text, int start, int end) {
        // Same test as QTextDocument::FindWholeWords.
        return (start == 0 || !text.at(start - 1).isLetterOrNumber())
               && (end == text.size() || !text.at(end).isLetterOrNumber());
    };

    // One pass over the lines; QTextDocument::find would copy a block's text
    // for every match.
    const int from = scopeStart >= 0 ? scopeStart : 0;
    int offset = lines.seek(from);
    for (; lines.valid(); lines.next(), offset = 0) {
        const int base = lines.position();
        if (scopeEnd >= 0 && base > scopeEnd)
            break;
        if (cancel && cancel->load(std::memory_order_relaxed))
            return {};
        if (lines.empty())
            continue; // Empty line: nothing non-empty can match.
        const QStringView text = lines.text();
        int pos = offset;
        while (pos >= 0 && pos <= text.size()) {
            int start;
            int end;
            if (options.regex) {
                const QRegularExpressionMatch m = query.regex().match(text, pos);
                if (!m.hasMatch())
                    break;
                start = int(m.capturedStart());
                end = int(m.capturedEnd());
            } else {
                start = int(text.indexOf(options.pattern, pos, sensitivity));
                if (start < 0)
                    break;
                end = start + int(options.pattern.size());
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

} // namespace

QList<Range> findAll(QTextDocument *document, const Compiled &query, int scopeStart, int scopeEnd)
{
    DocumentLines lines(document);
    return scan(lines, query, scopeStart, scopeEnd, nullptr);
}

QList<Range> findAllInText(QString rawText, const Compiled &query, int scopeStart, int scopeEnd,
                           const std::atomic<bool> *cancel)
{
    rawText.replace(QChar::Nbsp, u' ');
    TextLines lines(rawText);
    return scan(lines, query, scopeStart, scopeEnd, cancel);
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
