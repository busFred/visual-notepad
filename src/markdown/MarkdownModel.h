// The parsed block structure of a Markdown document, kept current with the
// QTextDocument. Shared by every visual view of the document.
#pragma once

#include "vn_core_bridge/lib.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <vector>

class QTextDocument;
template<typename T> class QFutureWatcher;

namespace vn {

class MarkdownModel : public QObject
{
    Q_OBJECT

public:
    explicit MarkdownModel(QTextDocument *document);

    QTextDocument *document() const { return m_doc; }

    // Top-level blocks in document order, reparsed first if the document
    // changed since the last parse.
    const rust::Vec<core::Block> &blocks();
    // Index into blocks() of the block containing line, or -1 (also
    // reparses first).
    int blockAt(int line);
    // The source text of lines [first, last], joined with '\n'.
    QString sourceText(int firstLine, int lastLine) const;

    // Views don't wait for a reparse on every keystroke: they read spans,
    // the last parse's blocks with their lines moved by the edits since.
    // Lines an edit touched become one span with block -1; previous is the
    // block it replaced when exactly one did (its old rendering can stand in
    // until the reparse), else -1. A reparse follows reparseDelay() after the
    // last edit (see reparseDue) and emits reparsed.
    struct Span
    {
        int first = 0;
        int last = 0;
        int block = -1;
        int previous = -1;
    };
    const std::vector<Span> &spans();
    // Index into spans() of the span containing line, or -1. Doesn't reparse
    // unless nothing usable is parsed yet.
    int spanAt(int line);
    // A block of the last parse (valid until the next reparsed signal).
    const core::Block &parsedBlock(int index) const { return m_parsed->blocks()[index]; }
    // Bumped whenever spans() changes (edits and reparses).
    int revision() const { return m_revision; }
    // Bumped by every parse.
    int parseGeneration() const { return m_parseGeneration; }
    bool hasPendingEdits() const { return m_pending || m_dirty; }
    // Reparses now if edits are pending; emits reparsed if it did. Always
    // synchronous: when it returns, blocks() and spans() match the document.
    void flush();
    // As flush(), except that a large document (see backgroundParseChars) is
    // parsed on a worker thread: reparsed is emitted later, once the result
    // is applied. A result for text that has been edited since is dropped
    // (the edits keep their spans), and the parse runs again if asked for
    // meanwhile.
    void flushInBackground();
    // A worker thread is parsing the document.
    bool isParsingInBackground() const { return m_job.running; }
    // Whether the last parse only parsed the blocks around the edits again
    // (see core::reparse); for tests.
    bool lastParseSpliced() const { return m_parsed && m_parsed->spliced(); }
    static int reparseDelay() { return 120; }
    // Parsing costs about 18 ms per MB; past this many characters edit
    // pauses parse off the GUI thread.
    static constexpr qsizetype backgroundParseChars = 256 * 1024;

    // Bumped whenever rendering options change, so views drop cached renders.
    int optionsRevision() const { return m_optionsRevision; }
    core::RenderOptions options() const;

Q_SIGNALS:
    void optionsChanged();
    // Edits have settled; a visible view calls flush() (hidden ones don't,
    // so Source mode never parses).
    void reparseDue();
    // spans() and blocks() were replaced by a reparse.
    void reparsed();

private:
    struct ParseResult;

    void reparse();
    void apply(std::shared_ptr<const core::Document> parsed);
    void documentChanged(int from, int removed, int added);
    void backgroundParseDone();

    QTextDocument *m_doc;
    // The last parse; also what the next parse of a large document starts from.
    std::shared_ptr<const core::Document> m_parsed;
    std::vector<Span> m_spans;
    QTimer m_timer;
    int m_lineCount = 0;      // Lines when m_spans were last made consistent.
    bool m_dirty = true;      // Spans unusable: parse before reading them.
    bool m_pending = false;   // Spans shifted by edits, a reparse is due.
    int m_revision = 0;
    int m_parseGeneration = 0;
    int m_optionsRevision = 0;
    // Bumped by every edit and every change of a parse setting: a parse of
    // the text taken at one value is current while it stays the same.
    quint64 m_inputSerial = 0;
    struct
    {
        QFutureWatcher<std::shared_ptr<ParseResult>> *watcher = nullptr;
        bool running = false;
        bool again = false; // Asked for while a parse of older text ran.
    } m_job;
};

} // namespace vn
