#include "DocumentLayout.h"

#include <QTextDocument>

namespace vn {

DocumentLayout::DocumentLayout(QTextDocument *document)
    : QAbstractTextDocumentLayout(document)
{
    // Emitted just before documentChanged() for the same edit, with the
    // edit alone (the highlighter's colouring in its slot is added to what
    // documentChanged() gets).
    connect(document, &QTextDocument::contentsChange, this,
            [this](int from, int charsRemoved, int charsAdded) { m_textChange = {from, charsRemoved, charsAdded}; });
}

void DocumentLayout::documentChanged(int from, int charsRemoved, int charsAdded)
{
    Q_EMIT changed(from, charsRemoved, charsAdded);
    m_textChange = {};
}

} // namespace vn
