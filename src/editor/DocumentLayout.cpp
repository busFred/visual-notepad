#include "DocumentLayout.h"

namespace vn {

DocumentLayout::DocumentLayout(QTextDocument *document)
    : QAbstractTextDocumentLayout(document)
{
}

void DocumentLayout::documentChanged(int from, int charsRemoved, int charsAdded)
{
    Q_EMIT changed(from, charsRemoved, charsAdded);
}

} // namespace vn
