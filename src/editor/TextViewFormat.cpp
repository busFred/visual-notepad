// Markdown formatting commands of TextView: inline toggles, headings, quote
// and list prefixes, links and code blocks. Each command is one undo step and
// does nothing in a read-only view.
#include "TextView.h"

#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>

namespace vn {

namespace {

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == u'_';
}

// Indent and block quote markers at the start of a line.
int leadLength(const QString &text)
{
    static const QRegularExpression lead(QStringLiteral("^[ \\t]*(?:>[ ]?[ \\t]*)*"));
    return lead.match(text).capturedLength();
}

int indentLength(const QString &text)
{
    int i = 0;
    while (i < text.size() && (text.at(i) == u' ' || text.at(i) == u'\t'))
        ++i;
    return i;
}

struct HeadingInfo
{
    int indent = 0;
    int level = 0;
    int length = 0; // The marker and the blanks after it.
};

HeadingInfo headingOf(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("^([ \\t]*)(#{1,6})(?:[ \\t]+|$)"));
    HeadingInfo info;
    info.indent = indentLength(text);
    const auto m = re.match(text);
    if (m.hasMatch()) {
        info.level = m.captured(2).size();
        info.length = m.capturedLength() - m.capturedLength(1);
    }
    return info;
}

} // namespace

void TextView::replaceRange(int from, int to, const QString &text)
{
    QTextCursor c(m_doc);
    c.setPosition(from);
    c.setPosition(to, QTextCursor::KeepAnchor);
    c.insertText(text);
}

void TextView::formatMarkdown(MarkdownFormat format)
{
    if (!canEdit())
        return;
    switch (format) {
    case MarkdownFormat::Bold:
        toggleInline(QStringLiteral("**"), QStringLiteral("**"));
        break;
    case MarkdownFormat::Italic:
        toggleInline(QStringLiteral("*"), QStringLiteral("*"));
        break;
    case MarkdownFormat::Underline:
        toggleInline(QStringLiteral("<u>"), QStringLiteral("</u>"));
        break;
    case MarkdownFormat::Strikethrough:
        toggleInline(QStringLiteral("~~"), QStringLiteral("~~"));
        break;
    case MarkdownFormat::InlineCode:
        toggleInline(QStringLiteral("`"), QStringLiteral("`"));
        break;
    case MarkdownFormat::Heading1:
    case MarkdownFormat::Heading2:
    case MarkdownFormat::Heading3:
    case MarkdownFormat::Heading4:
    case MarkdownFormat::Heading5:
    case MarkdownFormat::Heading6:
        setHeading(int(format) - int(MarkdownFormat::Heading1) + 1);
        break;
    case MarkdownFormat::HeadingUp:
        shiftHeading(-1);
        break;
    case MarkdownFormat::HeadingDown:
        shiftHeading(1);
        break;
    case MarkdownFormat::Quote:
    case MarkdownFormat::OrderedList:
    case MarkdownFormat::BulletList:
    case MarkdownFormat::TaskList:
        toggleLinePrefix(format);
        break;
    case MarkdownFormat::Link:
        insertLink();
        break;
    case MarkdownFormat::CodeBlock:
        wrapInCodeBlock();
        break;
    }
}

void TextView::toggleInline(const QString &open, const QString &close)
{
    const int olen = open.size();
    const int clen = close.size();
    const bool stars = open == close && open.at(0) == u'*';
    const int end = m_doc->characterCount() - 1;
    const auto slice = [&](int from, int length) {
        if (from < 0 || length <= 0 || from + length > end)
            return QString();
        QTextCursor c(m_doc);
        c.setPosition(from);
        c.setPosition(from + length, QTextCursor::KeepAnchor);
        return c.selectedText();
    };
    // `*` and `**` share a character: one run of k stars around the text means
    // italic for 1, bold for 2 and both from 3 on.
    const auto starsPresent = [&](int k) { return olen == 2 ? k >= 2 : (k == 1 || k >= 3); };
    const auto runBefore = [&](int pos) {
        int n = 0;
        while (pos - n - 1 >= 0 && m_doc->characterAt(pos - n - 1) == u'*')
            ++n;
        return n;
    };
    const auto runAfter = [&](int pos) {
        int n = 0;
        while (pos + n < end && m_doc->characterAt(pos + n) == u'*')
            ++n;
        return n;
    };

    beginEdit();
    for (Caret &caret : m_carets) {
        QTextCursor &cur = caret.cursor;
        int s = cur.selectionStart();
        int e = cur.selectionEnd();
        const bool selected = s != e;
        const int caretPos = cur.position();
        bool word = false;
        if (!selected) {
            const QTextBlock block = cur.block();
            const QString text = block.text();
            int pos = cur.positionInBlock();
            int a = pos;
            int b = pos;
            while (a > 0 && isWordChar(text.at(a - 1)))
                --a;
            while (b < text.size() && isWordChar(text.at(b)))
                ++b;
            if (a == b) {
                // Not in a word: remove an empty pair around the caret or add one.
                if (slice(s - olen, olen) == open && slice(s, clen) == close) {
                    replaceRange(s, s + clen, QString());
                    replaceRange(s - olen, s, QString());
                    cur.setPosition(s - olen);
                } else {
                    replaceRange(s, s, open + close);
                    cur.setPosition(s + olen);
                }
                continue;
            }
            s = block.position() + a;
            e = block.position() + b;
            word = true;
        }

        enum { Wrap, Outside, Inside } mode = Wrap;
        if (stars) {
            const int k = qMin(runBefore(s), runAfter(e));
            const QString text = slice(s, e - s);
            int lead = 0;
            int trail = 0;
            while (lead < text.size() && text.at(lead) == u'*')
                ++lead;
            while (trail < text.size() && text.at(text.size() - 1 - trail) == u'*')
                ++trail;
            const int inner = text.size() > 2 * olen ? qMin(qMin(lead, trail), (int(text.size()) - 1) / 2) : 0;
            if (starsPresent(k))
                mode = Outside;
            else if (inner > 0 && starsPresent(inner))
                mode = Inside;
        } else if (slice(s - olen, olen) == open && slice(e, clen) == close) {
            mode = Outside;
        } else {
            const QString text = slice(s, e - s);
            if (text.size() >= olen + clen && text.startsWith(open) && text.endsWith(close))
                mode = Inside;
        }

        int newS = s;
        int newE = e;
        switch (mode) {
        case Wrap:
            replaceRange(e, e, close);
            replaceRange(s, s, open);
            newS = s + olen;
            newE = e + olen;
            break;
        case Outside:
            replaceRange(e, e + clen, QString());
            replaceRange(s - olen, s, QString());
            newS = s - olen;
            newE = e - olen;
            break;
        case Inside:
            replaceRange(e - clen, e, QString());
            replaceRange(s, s + olen, QString());
            newE = e - olen - clen;
            break;
        }
        if (word) {
            const int pos = caretPos + (mode == Wrap ? olen : mode == Outside ? -olen : 0);
            cur.setPosition(pos);
        } else {
            cur.setPosition(newS);
            cur.setPosition(newE, QTextCursor::KeepAnchor);
        }
        caret.desiredX = -1;
    }
    endEdit();
    m_expandHistory.clear();
    caretsChanged();
}

void TextView::setHeading(int level)
{
    const QList<int> lines = caretLines();
    const bool skipBlank = lines.size() > 1;
    bool allAtLevel = true;
    QList<QTextBlock> targets;
    for (int line : lines) {
        const QTextBlock block = m_doc->findBlockByNumber(line);
        if (skipBlank && block.text().trimmed().isEmpty())
            continue;
        targets.append(block);
        if (headingOf(block.text()).level != level)
            allAtLevel = false;
    }
    if (targets.isEmpty())
        return;
    const int wanted = allAtLevel ? 0 : level;
    beginEdit();
    for (const QTextBlock &block : std::as_const(targets)) {
        const HeadingInfo h = headingOf(block.text());
        const int from = block.position() + h.indent;
        replaceRange(from, from + h.length, wanted ? QString(wanted, u'#') + u' ' : QString());
    }
    endEdit();
    caretsChanged();
}

void TextView::shiftHeading(int direction)
{
    // Down makes the heading smaller (H1 -> H2, H6 -> plain text); up makes it
    // larger (H2 -> H1, plain text -> H6, H1 stays).
    const QList<int> lines = caretLines();
    const bool skipBlank = lines.size() > 1;
    beginEdit();
    for (int line : lines) {
        const QTextBlock block = m_doc->findBlockByNumber(line);
        if (skipBlank && block.text().trimmed().isEmpty())
            continue;
        const HeadingInfo h = headingOf(block.text());
        int level = h.level;
        if (direction > 0)
            level = level == 6 ? 0 : level + 1;
        else
            level = level == 0 ? 6 : qMax(1, level - 1);
        if (level == h.level)
            continue;
        const int from = block.position() + h.indent;
        replaceRange(from, from + h.length, level ? QString(level, u'#') + u' ' : QString());
    }
    endEdit();
    caretsChanged();
}

void TextView::toggleLinePrefix(MarkdownFormat kind)
{
    const auto ranges = caretLineRanges();
    QList<QList<QTextBlock>> groups;
    int count = 0;
    for (const auto &[first, last] : ranges) {
        QList<QTextBlock> group;
        for (int line = first; line <= last; ++line)
            group.append(m_doc->findBlockByNumber(line));
        count += int(group.size());
        groups.append(group);
    }
    const bool multi = count > 1;
    const auto skipped = [&](const QTextBlock &b) { return multi && b.text().trimmed().isEmpty(); };
    const auto itemOf = [](const QTextBlock &b) {
        md::ListLine item = md::parseListLine(b.text());
        if (item.isQuote)
            item.valid = false;
        return item;
    };

    // Whether every line already has the format (then it is removed).
    bool all = true;
    bool anyTarget = false;
    bool anyUnchecked = false;
    for (const auto &group : std::as_const(groups)) {
        for (const QTextBlock &b : group) {
            if (skipped(b))
                continue;
            anyTarget = true;
            const md::ListLine item = itemOf(b);
            switch (kind) {
            case MarkdownFormat::Quote: {
                static const QRegularExpression quoted(QStringLiteral("^[ \\t]*>"));
                all = all && quoted.match(b.text()).hasMatch();
                break;
            }
            case MarkdownFormat::BulletList:
                all = all && item.valid && !item.ordered;
                break;
            case MarkdownFormat::OrderedList:
                all = all && item.valid && item.ordered;
                break;
            default:
                all = all && item.valid && !item.box.isEmpty();
                if (item.valid && item.box.startsWith(u"[ ]"))
                    anyUnchecked = true;
                break;
            }
        }
    }
    if (!anyTarget)
        return;

    beginEdit();
    for (const auto &group : std::as_const(groups)) {
        int number = 0;
        for (const QTextBlock &b : group) {
            if (skipped(b)) {
                if (kind == MarkdownFormat::Quote && !all)
                    replaceRange(b.position(), b.position(), QStringLiteral(">"));
                continue;
            }
            const QString text = b.text();
            const int base = b.position();
            const md::ListLine item = itemOf(b);
            const int markerAt = base + (item.valid ? int(item.lead.size()) : leadLength(text));
            const int afterMarker = base + (item.valid ? int(item.lead.size() + item.marker.size() + item.spacing.size())
                                                       : 0);
            const int contentAt = base + item.contentStart;
            switch (kind) {
            case MarkdownFormat::Quote:
                if (all) {
                    static const QRegularExpression quote(QStringLiteral("^([ \\t]*)>[ ]?"));
                    const auto m = quote.match(text);
                    replaceRange(base + m.capturedLength(1), base + m.capturedLength(0), QString());
                } else {
                    replaceRange(base, base, QStringLiteral("> "));
                }
                break;
            case MarkdownFormat::BulletList:
                if (all)
                    replaceRange(markerAt, contentAt, QString());
                else if (item.valid)
                    replaceRange(markerAt, markerAt + item.marker.size(), QStringLiteral("-"));
                else
                    replaceRange(markerAt, markerAt, QStringLiteral("- "));
                break;
            case MarkdownFormat::OrderedList: {
                ++number;
                if (all)
                    replaceRange(markerAt, contentAt, QString());
                else if (item.valid)
                    replaceRange(markerAt, markerAt + item.marker.size(), QString::number(number) + u'.');
                else
                    replaceRange(markerAt, markerAt, QString::number(number) + QStringLiteral(". "));
                break;
            }
            default:
                if (all) {
                    // Toggle every box: check them all unless all are checked.
                    const int box = contentAt - int(item.box.size()) + 1;
                    replaceRange(box, box + 1, anyUnchecked ? QStringLiteral("x") : QStringLiteral(" "));
                } else if (item.valid && !item.box.isEmpty()) {
                    // Already a task.
                } else if (item.valid) {
                    replaceRange(afterMarker, afterMarker, QStringLiteral("[ ] "));
                } else {
                    replaceRange(markerAt, markerAt, QStringLiteral("- [ ] "));
                }
                break;
            }
        }
    }
    endEdit();
    caretsChanged();
}

void TextView::insertLink()
{
    beginEdit();
    for (Caret &caret : m_carets) {
        QTextCursor &cur = caret.cursor;
        const int s = cur.selectionStart();
        const int e = cur.selectionEnd();
        if (s == e) {
            replaceRange(s, s, QStringLiteral("[](url)"));
            cur.setPosition(s + 1);
        } else {
            replaceRange(e, e, QStringLiteral("](url)"));
            replaceRange(s, s, QStringLiteral("["));
            cur.setPosition(e + 3);
            cur.setPosition(e + 6, QTextCursor::KeepAnchor);
        }
        caret.desiredX = -1;
    }
    endEdit();
    m_expandHistory.clear();
    caretsChanged();
}

void TextView::wrapInCodeBlock()
{
    const auto ranges = caretLineRanges();
    QList<QTextCursor> cursors;
    beginEdit();
    // Back to front keeps the line numbers and positions of earlier ranges;
    // the cursors kept for the info strings follow the later edits.
    for (auto it = ranges.crbegin(); it != ranges.crend(); ++it) {
        const QTextBlock first = m_doc->findBlockByNumber(it->first);
        const QTextBlock last = m_doc->findBlockByNumber(it->second);
        const QString indent = first.text().left(indentLength(first.text()));
        const int start = first.position();
        const int lastEnd = last.position() + last.length() - 1;
        replaceRange(lastEnd, lastEnd, u'\n' + indent + QStringLiteral("```"));
        replaceRange(start, start, indent + QStringLiteral("```\n"));
        QTextCursor place(m_doc);
        place.setPosition(start + int(indent.size()) + 3);
        cursors.prepend(place);
    }
    endEdit();
    m_expandHistory.clear();
    setCursors(cursors, 0);
}

} // namespace vn
