// A document layout that lays nothing out. Views do their own layout; this
// class only forwards QTextDocument's change notifications to them. Qt sends
// both text edits and highlighter format changes through
// QAbstractTextDocumentLayout::documentChanged(), and only emits
// QTextDocument::contentsChange() when a layout is installed.
//
// The change reported can be wider than the edit: blocks the highlighter
// coloured again inside it are added (QTextLayout::setFormats() records
// them). textChange() has the edit alone, as contentsChange() gave it.
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

    struct Change
    {
        int from = -1; // -1: not known (only formats may have changed).
        int charsRemoved = 0;
        int charsAdded = 0;
    };
    // While changed() is emitted: the part of the document whose text
    // changed. Outside it the text is as before.
    Change textChange() const { return m_textChange; }

Q_SIGNALS:
    // Same arguments as documentChanged(); covers every change in one edit.
    void changed(int from, int charsRemoved, int charsAdded);

protected:
    void documentChanged(int from, int charsRemoved, int charsAdded) override;

private:
    Change m_textChange;
};

} // namespace vn
