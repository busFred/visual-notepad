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

// Pipe tables, as text (the work is done by the Rust core, table.rs). Lines
// are 0-based; columns are UTF-16 offsets into a line. Rows count from the
// header (0), which includes the delimiter line; columns from 0.
enum class TableOp {
    RowAbove, RowBelow, ColumnLeft, ColumnRight,
    DeleteRow, DeleteColumn, DeleteTable,
    MoveRowUp, MoveRowDown, MoveColumnLeft, MoveColumnRight,
    AlignNone, AlignLeft, AlignCenter, AlignRight,
    SortAscending, SortDescending, Transpose, Realign,
    SetCell,
    ToHtml, ToPipe // Convert a pipe table to an HTML <table>, and back.
};
enum class TableAlignment { None, Left, Center, Right };

struct TableInfo
{
    bool found = false;
    int firstLine = 0;
    int lastLine = 0; // Included.
    int rows = 0;     // With the header.
    int cols = 0;
    int row = 0;      // Of the position asked about.
    int col = 0;
    TableAlignment alignment = TableAlignment::None; // Of that column.
    bool aligned = true; // Realign would change nothing.
    bool html = false;   // An HTML <table>, not a pipe table.
    quint32 available = 0; // Bit i: the core accepts core::TableOp i here.
};

struct TableEdit
{
    bool ok = false;
    bool changed = false;
    bool remove = false; // The lines go, nothing replaces them.
    int firstLine = 0;
    int lastLine = 0; // Included; replaced by `replacement` (lines joined by \n).
                      // Only the lines that change: a part of the table.
    QString replacement;
    int caretLine = 0; // In the text after the edit.
    int caretColumn = 0;
};

struct TableCell
{
    bool found = false;
    int line = 0;
    int start = 0;   // Column of the cell's text in `line`.
    int endLine = 0; // An HTML cell can take several lines.
    int end = 0;     // Column in `endLine`.
    QString text;
};

// Whether `op` applies to the cell `info` names (not the header row for
// "above", not the only column for delete, ...): the core says.
bool tableOpAvailable(const TableInfo &info, TableOp op);
// `text` is the whole document with "\n" line breaks.
TableInfo tableInfo(const QString &text, int line, int column);
// `row` and `col` name the cell the operation is about; `arg` is the new
// text of the cell for SetCell.
TableEdit tableEdit(const QString &text, int line, TableOp op, int row, int col, const QString &arg = {});
TableCell tableCell(const QString &text, int line, int row, int col);
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
// of the two, "\\?\UNC\..."), and for Windows object manager paths ("\??\",
// "\\?\", "\\.\", either slash), which can name one too. Opening one connects
// to the host; on Windows over SMB, which hands it the user's password hash.
// Never open these for a document.
bool isNetworkPath(const QString &path);
// True if a picture or link name from a document may reach another machine
// in any reading Qt or the OS gives it: as written, percent-decoded once or
// twice, and as a file: URL ("file://host/x", "file:////host/x",
// "file:\\host\x"), with either slash. "@2x" variants share the start, so
// they are covered too.
bool mayNameOtherMachine(const QString &name);
// `path` with "." and ".." resolved (QDir::cleanPath) and, with
// `windowsNames`, trailing dots and spaces dropped from every component the
// way Windows drops them: the file the OS opens for it, by a name that says
// which (no "evil.exe/." ending in "."). Checks and opening use this one.
QString cleanLocalPath(const QString &path, bool windowsNames);
// The absolute form of local `path` that is checked and opened: cleaned
// (with Windows' rules on Windows) and, on Windows, with 8.3 short names
// spelled out.
QString resolvedLocalPath(const QString &path);
// True if handing local `path` to the OS (QDesktopServices) could run a
// program rather than show a document: a launchable suffix (see
// launchableSuffixes in the .cpp; case-insensitive, ignoring what Windows
// ignores: trailing dots and spaces, "::$DATA", alternate stream names; any
// non-ASCII suffix counts), on the cleaned path (cleanLocalPath) or, for a
// symlink, on its target; or an existing non-directory file with an execute
// bit, unless its suffix is a plain document type.
bool isLaunchable(const QString &path);
// `text` with characters that change how the rest reads but show nothing of
// their own spelled out as "\u{202E}": bidi controls, other format characters
// (zero width, soft hyphen, tags), control characters, lone surrogates and
// line separators.
QString withVisibleControls(const QString &text);

} // namespace vn::md
