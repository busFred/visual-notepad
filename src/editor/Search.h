// Find and replace over a QTextDocument, following VS Code's find widget
// semantics (match case, whole word, regex, preserve case).
#pragma once

#include <QList>
#include <QRegularExpression>
#include <QString>

class QTextDocument;

namespace vn::search {

struct Options
{
    QString pattern;
    bool caseSensitive = false;
    bool wholeWord = false;
    bool regex = false;
};

// A match as document positions [start, end). Plain ints, not QTextCursors:
// Qt adjusts every live cursor on each edit, so thousands of them make every
// keystroke and every replacement slow.
struct Range
{
    int start = 0;
    int end = 0;
    bool operator==(const Range &other) const = default;
};

// Options with the regular expression compiled once, to share between
// validation, searching and replacing.
class Compiled
{
public:
    explicit Compiled(const Options &options);
    const Options &options() const { return m_options; }
    bool isValid() const { return m_valid; }
    const QRegularExpression &regex() const { return m_regex; }

private:
    Options m_options;
    QRegularExpression m_regex;
    bool m_valid = true;
};

// Most matches collected; VS Code stops counting at the same number.
constexpr int maxMatches = 19999;

// False if the regex does not compile.
bool isValid(const Options &options);

// Non-empty matches in document order, optionally limited to [scopeStart,
// scopeEnd). Matches never span lines.
QList<Range> findAll(QTextDocument *document, const Compiled &query, int scopeStart = -1, int scopeEnd = -1);
QList<Range> findAll(QTextDocument *document, const Options &options, int scopeStart = -1, int scopeEnd = -1);

// The line a replacement last read, so consecutive matches on one line do not
// copy it again.
struct LineText
{
    int blockPosition = -1;
    QString text;
};

// The text that replaces a match. In regex mode $0-$9 and $& insert groups,
// and \n, \t and \\ are escapes. With preserveCase the replacement copies the
// match's casing (UPPER, lower or Capitalized).
QString replacementFor(QTextDocument *document, const Compiled &query, Range match, const QString &replacement,
                       bool preserveCase, LineText *line = nullptr);
QString replacementFor(QTextDocument *document, const Options &options, Range match, const QString &replacement,
                       bool preserveCase);

} // namespace vn::search
