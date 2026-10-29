// A document layout that lays nothing out. Views do their own layout; this
// class only forwards QTextDocument's change notifications to them. Qt sends
// both text edits and highlighter format changes through
// QAbstractTextDocumentLayout::documentChanged(), and only emits
// QTextDocument::contentsChange() when a layout is installed.
#pragma once

#include <QAbstractTextDocumentLayout>

namespace vn {

class DocumentLayout : public QAbstractTextDocumentLayout
{
    Q_OBJECT

public:
    explicit DocumentLayout(QTextDocument *document);

    void draw(QPainter *, const PaintContext &) override {}
    int hitTest(const QPointF &, Qt::HitTestAccuracy) const override { return -1; }
    int pageCount() const override { return 1; }
    QSizeF documentSize() const override { return {}; }
    QRectF frameBoundingRect(QTextFrame *) const override { return {}; }
    QRectF blockBoundingRect(const QTextBlock &) const override { return {}; }

Q_SIGNALS:
    // Same arguments as documentChanged(); covers every change in one edit.
    void changed(int from, int charsRemoved, int charsAdded);

protected:
    void documentChanged(int from, int charsRemoved, int charsAdded) override;
};

} // namespace vn
