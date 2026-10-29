// The parsed block structure of a Markdown document, kept current with the
// QTextDocument. Shared by every visual view of the document.
#pragma once

#include "vn_core_bridge/lib.h"

#include <QObject>
#include <QString>

class QTextDocument;

namespace vn {

class MarkdownModel : public QObject
{
    Q_OBJECT

public:
    explicit MarkdownModel(QTextDocument *document);

    QTextDocument *document() const { return m_doc; }

    // Top-level blocks in document order (reparsed lazily after edits).
    const rust::Vec<core::Block> &blocks();
    // Index of the block containing line, or -1.
    int blockAt(int line);
    // The source text of lines [first, last], joined with '\n'.
    QString sourceText(int firstLine, int lastLine) const;

    // Bumped whenever rendering options change, so views drop cached renders.
    int optionsRevision() const { return m_optionsRevision; }
    core::RenderOptions options() const;

Q_SIGNALS:
    void optionsChanged();

private:
    void reparse();

    QTextDocument *m_doc;
    rust::Vec<core::Block> m_blocks;
    std::vector<int> m_lineToBlock;
    bool m_dirty = true;
    int m_optionsRevision = 0;
};

} // namespace vn
