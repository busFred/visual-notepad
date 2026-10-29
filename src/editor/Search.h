// Find and replace over a QTextDocument, following VS Code's find widget
// semantics (match case, whole word, regex, preserve case).
#pragma once

#include <QList>
#include <QString>
#include <QTextCursor>

class QTextDocument;

namespace vn::search {

struct Options
{
    QString pattern;
    bool caseSensitive = false;
    bool wholeWord = false;
    bool regex = false;
};

// Most matches collected; VS Code stops counting at the same number.
constexpr int maxMatches = 19999;

// False if the regex does not compile.
bool isValid(const Options &options);

// Non-empty matches in document order, optionally limited to [scopeStart,
// scopeEnd). Matches never span lines.
QList<QTextCursor> findAll(QTextDocument *document, const Options &options, int scopeStart = -1,
                           int scopeEnd = -1);

// The text that replaces a match. In regex mode $0-$9 and $& insert groups,
// and \n, \t and \\ are escapes. With preserveCase the replacement copies the
// match's casing (UPPER, lower or Capitalized).
QString replacementFor(const Options &options, const QTextCursor &match, const QString &replacement,
                       bool preserveCase);

} // namespace vn::search
