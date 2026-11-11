// Draws the formulas of a rich text document as shapes (see MathRenderer.h),
// sharp at any size and on the baseline of the text around them. A formula
// is a character of the document whose format has objectType type and the
// index of the formula (from add()) in property indexProperty.
#pragma once

#include "MathRenderer.h"

#include <QList>
#include <QObject>
#include <QTextObjectInterface>

#include <memory>

namespace vn {

class MathObject : public QObject, public QTextObjectInterface
{
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    static constexpr int type = QTextFormat::UserObject + 1;
    static constexpr int indexProperty = QTextFormat::UserProperty + 1;

    struct Placed
    {
        std::shared_ptr<const math::Formula> formula;
        qreal scale = 1; // Of the formula's layout size.
        // The ascent of the text's font: a line of formulas only has its
        // baseline where a line of text would.
        qreal minAscent = 0;
    };

    using QObject::QObject;

    // Registers a document's handler for formulas, owned by the document.
    static MathObject *install(QTextDocument *document);

    // The index to give a formula's format.
    int add(Placed placed);
    // The formula of a format with objectType type, or null.
    const Placed *placed(const QTextFormat &format) const;
    // Path elements of every formula: what drawing them costs.
    qsizetype elements() const;

    // Qt places an object aligned to the baseline with its bottom edge the
    // descent of its character's font below the baseline
    // (QTextDocumentLayout::resizeInlineObject): the formula's box reaches
    // that far down, its baseline on the text's. A formula deeper than that
    // needs a larger font on its character (see deepen()). Above, the box
    // reaches the formula's top or the text's, whichever is higher.
    QSizeF intrinsicSize(QTextDocument *document, int position, const QTextFormat &format) override;
    void drawObject(QPainter *painter, const QRectF &rect, QTextDocument *document, int position,
                    const QTextFormat &format) override;

    // Gives a character's format (whose font is `font`) a pixel size whose
    // descent is at least `depth`; false, and the format unchanged, when a
    // few tries find none.
    static bool deepen(QTextCharFormat &format, const QFont &font, qreal depth);

private:
    // How far the box of a formula reaches above the baseline.
    static qreal above(const Placed &placed);

    QList<Placed> m_placed;
};

} // namespace vn
