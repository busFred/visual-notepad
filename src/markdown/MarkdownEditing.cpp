#include "MarkdownEditing.h"

#include "vn_core_bridge/lib.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace vn::md {

namespace {

rust::Str str(const QByteArray &utf8)
{
    return rust::Str(utf8.constData(), size_t(utf8.size()));
}

QString toQString(const rust::String &s)
{
    return QString::fromUtf8(s.data(), qsizetype(s.size()));
}

bool isBlank(QChar c)
{
    return c == u' ' || c == u'\t';
}

bool isThematicBreak(QStringView rest)
{
    static const QRegularExpression re(QStringLiteral("^(?:(?:-[ \\t]*){3,}|(?:\\*[ \\t]*){3,}|(?:_[ \\t]*){3,})$"));
    return re.matchView(rest).hasMatch();
}

} // namespace

QString ListLine::nextPrefix() const
{
    QString next = lead;
    if (!isQuote) {
        next += ordered ? QString::number(number + 1) + delimiter : marker;
        next += spacing;
        if (!box.isEmpty())
            next += QStringLiteral("[ ] ");
    }
    return next;
}

ListLine parseListLine(const QString &line)
{
    ListLine info;
    int i = 0;
    const auto skipBlanks = [&] {
        while (i < line.size() && isBlank(line.at(i)))
            ++i;
    };

    // Leading indent and block quote markers.
    skipBlanks();
    bool quote = false;
    while (i < line.size() && line.at(i) == u'>') {
        quote = true;
        ++i;
        if (i < line.size() && line.at(i) == u' ')
            ++i;
        // A following marker may be indented after the quote.
        const int save = i;
        skipBlanks();
        if (i >= line.size() || line.at(i) != u'>')
            i = save;
    }
    const int markerStart = i;
    info.lead = line.left(markerStart);
    // Without a quote the indent is part of the lead already.

    int j = markerStart;
    if (j < line.size() && (line.at(j) == u'-' || line.at(j) == u'*' || line.at(j) == u'+')) {
        info.marker = line.at(j);
        ++j;
    } else {
        int digits = 0;
        while (j < line.size() && line.at(j).isDigit() && digits < 9) {
            ++j;
            ++digits;
        }
        if (digits > 0 && j < line.size() && (line.at(j) == u'.' || line.at(j) == u')')) {
            info.ordered = true;
            info.number = line.mid(markerStart, digits).toInt();
            info.delimiter = line.at(j);
            info.marker = line.mid(markerStart, digits + 1);
            ++j;
        } else {
            j = markerStart;
        }
    }

    if (info.marker.isEmpty()) {
        if (!quote)
            return {};
        info.isQuote = true;
        info.valid = true;
        info.contentStart = markerStart;
        // The content of "> text": a trailing space is part of the prefix.
        info.empty = line.mid(markerStart).trimmed().isEmpty();
        return info;
    }

    // The marker must be followed by blanks.
    const int spaceStart = j;
    while (j < line.size() && isBlank(line.at(j)))
        ++j;
    if (j == spaceStart)
        return {};
    info.spacing = line.mid(spaceStart, j - spaceStart);
    if (isThematicBreak(QStringView(line).mid(markerStart)))
        return {};

    // Task box.
    if (j + 2 < line.size() && line.at(j) == u'[' && (line.at(j + 1) == u' ' || line.at(j + 1) == u'x'
                                                       || line.at(j + 1) == u'X')
        && line.at(j + 2) == u']' && (j + 3 == line.size() || isBlank(line.at(j + 3)))) {
        int k = j + 3;
        while (k < line.size() && isBlank(line.at(k)))
            ++k;
        info.box = line.mid(j, 3) + u' ';
        j = k;
    }
    info.contentStart = j;
    info.empty = line.mid(j).trimmed().isEmpty();
    info.valid = true;
    return info;
}

QList<LinkTarget> linkTargets(const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    QList<LinkTarget> result;
    for (const auto &t : core::link_targets(str(utf8)))
        result.append({int(t.start), int(t.end), toQString(t.dest)});
    return result;
}

QString formatDocument(const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    return toQString(core::format_markdown(str(utf8)));
}

QString rebaseLinks(const QString &text, const QString &fromDir, const QString &toDir)
{
    const QByteArray utf8 = text.toUtf8();
    const QByteArray from = fromDir.toUtf8();
    const QByteArray to = toDir.toUtf8();
    return toQString(core::rebase_links(str(utf8), str(from), str(to)));
}

QStringList anchorsOf(const QString &text)
{
    const QByteArray utf8 = text.toUtf8();
    QStringList anchors;
    QHash<QString, int> seen;
    for (const auto &heading : core::outline(str(utf8))) {
        const QByteArray headingText(heading.text.data(), qsizetype(heading.text.size()));
        QString slug = toQString(core::heading_slug(str(headingText)));
        const int n = seen[slug]++;
        if (n > 0)
            slug += u'-' + QString::number(n);
        anchors.append(slug.toLower());
    }
    static const QRegularExpression htmlAnchor(QStringLiteral("\\b(?:id|name)\\s*=\\s*[\"']([^\"']+)[\"']"),
                                               QRegularExpression::CaseInsensitiveOption);
    for (auto it = htmlAnchor.globalMatch(text); it.hasNext();)
        anchors.append(it.next().captured(1).toLower());
    return anchors;
}

QString linkAt(const QList<LinkTarget> &targets, const QString &lineText, int linePosition, int position)
{
    for (const LinkTarget &t : targets) {
        if (position >= t.start && position <= t.end)
            return t.dest;
    }
    static const QRegularExpression url(QStringLiteral("(?:https?|ftp)://[^\\s<>\"'`]+|mailto:[^\\s<>\"'`]+"));
    for (auto it = url.globalMatch(lineText); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        QString found = m.captured();
        // Trailing punctuation and closing brackets belong to the sentence.
        while (!found.isEmpty() && QStringLiteral(".,;:!?)]}").contains(found.back()))
            found.chop(1);
        const int start = linePosition + int(m.capturedStart());
        if (position >= start && position <= start + found.size())
            return found;
    }
    return {};
}

QString encodeDestination(const QString &path)
{
    QString out;
    out.reserve(path.size());
    for (QChar c : path) {
        switch (c.unicode()) {
        case ' ':
            out += QStringLiteral("%20");
            break;
        case '(':
            out += QStringLiteral("%28");
            break;
        case ')':
            out += QStringLiteral("%29");
            break;
        case '<':
            out += QStringLiteral("%3C");
            break;
        case '>':
            out += QStringLiteral("%3E");
            break;
        default:
            out += c;
        }
    }
    return out;
}

QString linkForFile(const QString &filePath, const QString &documentDir)
{
    static const QSet<QString> imageSuffixes = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                                QStringLiteral("gif"), QStringLiteral("svg"), QStringLiteral("webp"),
                                                QStringLiteral("bmp")};
    const QFileInfo info(filePath);
    // relativeFilePath returns the absolute path if there is no relative one
    // (e.g. a different drive).
    const QString target = documentDir.isEmpty() ? QDir::fromNativeSeparators(info.absoluteFilePath())
                                                 : QDir(documentDir).relativeFilePath(info.absoluteFilePath());
    const QString dest = encodeDestination(target);
    if (imageSuffixes.contains(info.suffix().toLower()))
        return QStringLiteral("![](%1)").arg(dest);
    return QStringLiteral("[%1](%2)").arg(info.fileName(), dest);
}

} // namespace vn::md
