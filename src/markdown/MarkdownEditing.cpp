#include "MarkdownEditing.h"

#include "vn_core_bridge/lib.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <iterator>
#include <string_view>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

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

bool isNetworkPath(const QString &path)
{
    // Windows takes either slash; two at the start name a host.
    const auto slash = [](QChar c) { return c == u'/' || c == u'\\'; };
    const QString p = path.trimmed();
    if (p.size() >= 2 && slash(p[0]) && slash(p[1]))
        return true; // Also "\\?\" and "\\.\".
    // "\??\UNC\host\share" (the object manager's own prefix) is a host too.
    return p.size() >= 4 && slash(p[0]) && p[1] == u'?' && p[2] == u'?' && slash(p[3]);
}

QString cleanLocalPath(const QString &path, bool windowsNames)
{
    if (!windowsNames)
        return QDir::cleanPath(QDir::fromNativeSeparators(path));
    // Either slash, on any system.
    QString clean = QDir::cleanPath(QString(path).replace(u'\\', u'/'));
    // Dropping "x. " can leave "." or "..", resolving ".." can leave a
    // component ending in a dot: until nothing changes.
    for (;;) {
        QStringList parts = clean.split(u'/');
        for (QString &part : parts) {
            QString spaced = part;
            while (spaced.endsWith(u' '))
                spaced.chop(1);
            if (spaced == u"." || spaced == u"..") {
                part = spaced; // ". " and ".. " too.
                continue;
            }
            // The empty first part of "/x" stays empty; one that empties
            // ("...") is the folder it is in.
            const bool empty = part.isEmpty();
            while (part.endsWith(u'.') || part.endsWith(u' '))
                part.chop(1);
            if (part.isEmpty() && !empty)
                part = u'.';
        }
        const QString next = QDir::cleanPath(parts.join(u'/'));
        if (next == clean)
            return clean;
        clean = next;
    }
}

bool mayNameOtherMachine(const QString &name)
{
    QString s = name.trimmed();
    for (int round = 0; round < 3; ++round) {
        if (isNetworkPath(s))
            return true;
        // "file:" read by hand (QUrl rejects some that Qt still opens): a
        // host after "file://", or a path that names one.
        if (s.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
            QStringView rest = QStringView(s).mid(5);
            if (rest.startsWith(QLatin1String("//"))) {
                rest = rest.mid(2);
                const qsizetype end = rest.indexOf(u'/');
                if (!rest.left(end < 0 ? rest.size() : end).trimmed().isEmpty())
                    return true; // A host.
                rest = rest.mid(end < 0 ? rest.size() : end);
            }
            if (isNetworkPath(rest.toString()))
                return true;
        }
        const QUrl url(s);
        if (url.isLocalFile() && (!url.host().isEmpty() || isNetworkPath(url.toLocalFile())))
            return true;
        s = QUrl::fromPercentEncoding(s.toUtf8()).trimmed();
    }
    return false;
}

namespace {

// Suffixes the OS runs, installs or follows (shortcuts) instead of showing
// them as documents. A deny-list: links to anything here ask first.
// Windows: executables, scripts for the built-in hosts (cmd, PowerShell,
// Windows Script Host, mshta), installers, control panel and console
// snap-ins, shortcuts and shell links, registry and setup files, packages,
// compiled help, search and library descriptors, troubleshooter packs;
// remote desktop and Citrix sessions, ClickOnce and Office add-ins, themes,
// update and provisioning packages, Access databases (they run VBA), disk
// images (mounting shows their programs), OneNote notebooks, Office files
// with macros, Java Web Start and SYLK spreadsheets.
// Linux/macOS: shell and interpreter scripts, self-extracting/AppImage
// binaries, launchers (.desktop, .command, .terminal), bundles and
// installers (.app, .pkg, .dmg, .deb, .rpm), Automator workflows/actions.
constexpr std::string_view launchableSuffixes[] = {
    // Windows
    "exe", "com", "scr", "pif", "bat", "cmd", "ps1", "psm1", "vbs", "vbe", "js", "jse", "wsf", "wsh", "msi", "msp",
    "mst", "msc", "hta", "cpl", "lnk", "url", "reg", "jar", "dll", "sys", "inf", "appx", "appxbundle", "msix",
    "msixbundle", "application", "gadget", "ws", "chm", "scf", "library-ms", "search-ms", "diagcab",
    "settingcontent-ms", "rdp", "ica", "appref-ms", "vsto", "xll", "xlam", "themepack", "deskthemepack", "msu", "ppkg",
    "mdb", "accdb", "accde", "mde", "ade", "adp", "iso", "img", "vhd", "vhdx", "one", "docm", "xlsm", "pptm", "dotm",
    "xltm", "potm", "ppam", "sldm", "jnlp", "slk",
    // Linux / macOS
    "sh", "bash", "zsh", "csh", "ksh", "run", "bin", "appimage", "desktop", "command", "app", "pkg", "dmg", "py",
    "pl", "rb", "deb", "rpm", "workflow", "action", "terminal",
};

// Suffixes that stay documents even with an execute bit, which files on
// FAT/NTFS/SMB mounts and from some archives get wholesale. Kept short: only
// types every desktop opens in a viewer, never in an interpreter.
constexpr std::string_view documentSuffixes[] = {
    "md", "markdown", "txt", "png", "jpg", "jpeg", "gif", "bmp", "webp", "svg", "svgz", "ico", "tif", "tiff", "avif",
    "pdf",
};

template<size_t N>
bool listed(const std::string_view (&list)[N], const QString &suffix)
{
    // Upper first: "\u017F" (long s) upper-cases to "S", as Windows compares.
    const QByteArray lower = suffix.toUpper().toLower().toUtf8();
    const std::string_view s(lower.constData(), size_t(lower.size()));
    return std::find(std::begin(list), std::end(list), s) != std::end(list);
}

// The file name as Windows resolves it: no trailing dots or spaces, no
// "::$DATA" / ":stream:$DATA" stream type.
QString windowsName(QString name)
{
    static const QRegularExpression streamType(QStringLiteral(":\\$DATA$"),
                                               QRegularExpression::CaseInsensitiveOption);
    for (;;) {
        const qsizetype before = name.size();
        while (name.endsWith(u'.') || name.endsWith(u' '))
            name.chop(1);
        name.remove(streamType);
        while (name.endsWith(u':'))
            name.chop(1);
        if (name.size() == before)
            return name;
    }
}

#ifdef Q_OS_WIN
// `path` with 8.3 short names ("SETTIN~1.SET") spelled out, if it exists.
QString longPathName(const QString &path)
{
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const DWORD size = GetLongPathNameW(native.c_str(), nullptr, 0); // With the null.
    if (size == 0)
        return path;
    std::wstring buffer(size, L'\0');
    const DWORD written = GetLongPathNameW(native.c_str(), buffer.data(), size); // Without.
    if (written == 0 || written >= size)
        return path;
    buffer.resize(written);
    return QDir::fromNativeSeparators(QString::fromStdWString(buffer));
}
#endif

bool hasLaunchableSuffix(const QString &path)
{
    const QString name = windowsName(path.section(QRegularExpression(QStringLiteral("[/\\\\]")), -1));
    // "a.txt:evil.exe" is a stream of a.txt and "C:evil.exe" a file on a
    // drive: check every part.
    const QStringList parts = name.split(u':');
    return std::any_of(parts.cbegin(), parts.cend(), [](const QString &part) {
        const QString trimmed = windowsName(part);
        const qsizetype dot = trimmed.lastIndexOf(u'.');
        if (dot < 0)
            return false;
        const QString suffix = trimmed.mid(dot + 1);
        // One Windows may fold to a listed one some other way: ask.
        const bool ascii = std::all_of(suffix.cbegin(), suffix.cend(), [](QChar c) { return c.unicode() < 0x80; });
        return !ascii || listed(launchableSuffixes, suffix);
    });
}

} // namespace

QString resolvedLocalPath(const QString &path)
{
#ifdef Q_OS_WIN
    return longPathName(cleanLocalPath(QFileInfo(path).absoluteFilePath(), true));
#else
    return cleanLocalPath(QFileInfo(path).absoluteFilePath(), false);
#endif
}

QString withVisibleControls(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        char32_t c = text[i].unicode();
        qsizetype length = 1;
        if (QChar::isHighSurrogate(c) && i + 1 < text.size() && text[i + 1].isLowSurrogate()) {
            c = QChar::surrogateToUcs4(text[i], text[i + 1]);
            length = 2;
        }
        const QChar::Category category = QChar::category(c);
        // The bidi controls are format characters; listed for clarity.
        const bool bidi = (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0x200E || c == 0x200F
                          || c == 0x061C;
        if (bidi || category == QChar::Other_Format || category == QChar::Other_Control
            || category == QChar::Other_Surrogate || category == QChar::Separator_Line
            || category == QChar::Separator_Paragraph) {
            out += QStringLiteral("\\u{%1}").arg(QString::number(uint(c), 16).toUpper().rightJustified(4, u'0'));
        } else {
            out += QStringView(text).mid(i, length);
        }
        i += length - 1;
    }
    return out;
}

bool isLaunchable(const QString &rawPath)
{
    // "evil.exe/." and "evil.exe/x/.." name evil.exe.
#ifdef Q_OS_WIN
    const QString path = longPathName(cleanLocalPath(rawPath, true));
#else
    const QString path = cleanLocalPath(rawPath, false);
    if (hasLaunchableSuffix(cleanLocalPath(rawPath, true)))
        return true;
#endif
    if (hasLaunchableSuffix(rawPath) || hasLaunchableSuffix(path))
        return true; // Even a directory: a macOS .app is one.
    const QFileInfo info(path);
    // The end of a chain of symlinks: empty if it does not exist.
    const QString target = info.isSymLink() ? info.canonicalFilePath() : QString();
    if (!target.isEmpty() && hasLaunchableSuffix(target))
        return true;
    // These follow symlinks.
    if (!info.exists() || info.isDir() || !info.isExecutable())
        return false;
    const QString name = windowsName(QFileInfo(target.isEmpty() ? path : target).fileName());
    const qsizetype dot = name.lastIndexOf(u'.');
    return dot < 0 || !listed(documentSuffixes, name.mid(dot + 1));
}

} // namespace vn::md
