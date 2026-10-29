// Per-file indentation: guessing it from the content and converting between
// spaces and tabs. Ports VS Code's behaviour.
#pragma once

class QTextDocument;

namespace vn::indentation {

struct Settings
{
    int tabSize = 4;
    bool insertSpaces = true;
    bool operator==(const Settings &) const = default;
};

// Port of guessIndentation in VS Code's
// src/vs/editor/common/model/indentationGuesser.ts. Looks at the first 10000
// lines; whatever it cannot decide comes back as the defaults.
Settings guess(const QTextDocument *document, int defaultTabSize, bool defaultInsertSpaces);

// Port of IndentationToSpacesCommand / IndentationToTabsCommand
// (src/vs/editor/contrib/indentation/browser/indentation.ts): rewrites the
// indentation of every line as one undoable edit. Returns whether any line
// changed.
bool convert(QTextDocument *document, int tabSize, bool toSpaces);

} // namespace vn::indentation
