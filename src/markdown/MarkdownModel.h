// The parsed block structure of a Markdown document, kept current with the
// QTextDocument. Shared by every visual view of the document.
#pragma once

#include "vn_core_bridge/lib.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <vector>

class QTextDocument;

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
    const core::Block &parsedBlock(int index) const { return m_blocks[index]; }
    // Bumped whenever spans() changes (edits and reparses).
    int revision() const { return m_revision; }
    // Bumped by every parse.
    int parseGeneration() const { return m_parseGeneration; }
    bool hasPendingEdits() const { return m_pending || m_dirty; }
    // Reparses now if edits are pending; emits reparsed if it did.
    void flush();
    static int reparseDelay() { return 120; }

    // Bumped whenever rendering options change, so views drop cached renders.
    int optionsRevision() const { return m_optionsRevision; }
    core::RenderOptions options() const;

    // Whether the user agreed to download the document's remote pictures,
    // for this session. Nothing is fetched while Undecided; Deny is final
    // (the views stop asking).
    enum class RemoteConsent { Undecided, Allow, Deny };
    RemoteConsent remoteConsent() const { return m_remoteConsent; }
    void setRemoteConsent(RemoteConsent consent);

Q_SIGNALS:
    void optionsChanged();
    void remoteConsentChanged();
    // Edits have settled; a visible view calls flush() (hidden ones don't,
    // so Source mode never parses).
    void reparseDue();
    // spans() and blocks() were replaced by a reparse.
    void reparsed();

private:
    void reparse();
    void documentChanged(int from, int removed, int added);

    QTextDocument *m_doc;
    rust::Vec<core::Block> m_blocks;
    std::vector<Span> m_spans;
    QTimer m_timer;
    int m_lineCount = 0;      // Lines when m_spans were last made consistent.
    bool m_dirty = true;      // Spans unusable: parse before reading them.
    bool m_pending = false;   // Spans shifted by edits, a reparse is due.
    int m_revision = 0;
    int m_parseGeneration = 0;
    int m_optionsRevision = 0;
    RemoteConsent m_remoteConsent = RemoteConsent::Undecided;
};

} // namespace vn
