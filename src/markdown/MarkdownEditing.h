// Markdown editing helpers shared by the editor views: list continuation,
// the Rust-backed formatter and link utilities.
#pragma once

#include <QList>
#include <QString>

namespace vn::md {

// A line that starts a list item or a block quote.
struct ListLine
{
    bool valid = false;
    // Everything before the item marker: indent and quote markers ("> ").
    QString lead;
    // The marker itself ("-", "12.", "1)") and the spaces after it; empty for
    // plain quotes.
    QString marker;
    QString spacing;
    // "[ ] " or "[x] " (with trailing space) for task items.
    QString box;
    bool ordered = false;
    bool isQuote = false;     // No list marker, only quote markers.
    int number = 0;
    QChar delimiter;          // '.' or ')' for ordered items.
    int contentStart = 0;     // Index in the line where the item text begins.
    bool empty = false;       // Nothing after the marker (and box).

    // The prefix that starts the next item after this one.
    QString nextPrefix() const;
};

ListLine parseListLine(const QString &line);

// A link destination found in the text, as document positions.
struct LinkTarget
{
    int start = 0;
    int end = 0;
    QString dest;
};

QList<LinkTarget> linkTargets(const QString &text);
QString formatDocument(const QString &text);
QString rebaseLinks(const QString &text, const QString &fromDir, const QString &toDir);
// GitHub-style anchors of the headings in `text` (lower case), plus explicit
// HTML ids and names.
QStringList anchorsOf(const QString &text);
// The destination to open for a click at `position`: a link destination or a
// bare URL on that line. Empty if there is none.
QString linkAt(const QList<LinkTarget> &targets, const QString &lineText, int linePosition, int position);

// Markdown link text for a file, relative to `documentDir` (may be empty).
QString linkForFile(const QString &filePath, const QString &documentDir);
// Percent-encodes characters that would end a Markdown destination.
QString encodeDestination(const QString &path);
// True for a path on another machine ("//host/share", "\\host\share", mixes
// of the two, "\\?\UNC\..."). Opening one connects to the host; on Windows
// over SMB, which hands it the user's password hash. Never open these for a
// document.
bool isNetworkPath(const QString &path);

} // namespace vn::md
