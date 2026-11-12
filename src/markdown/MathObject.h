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
        // Within the room its line has anyway (see BlockRenderer's
        // placeMath): aligned to the line's top, so Qt leaves it out of the
        // line's height, and drawn on the line's baseline.
        bool withinLine = true;
        // Its box's height then: the line's text, ascent and descent (Qt
        // makes a line as high as the box of a top-aligned object; the
        // formula is drawn whole, past its box).
        qreal box = 0;
        // Aligned to the baseline otherwise: how far its box reaches above it
        // (the formula's top may reach a little further, into the space
        // between lines). Within the line: its text's ascent, the least the
        // baseline is below the line's top (a line of formulas alone, which
        // Qt makes as high as their boxes and puts the baseline half way).
        qreal above = 0;
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

    // A formula within its line's room is aligned to the line's top, which
    // Qt leaves out of the line's height (QTextLine's layout skips AlignTop
    // objects), and is drawn on the line's baseline, its ascent below that
    // top. Any other is aligned to the baseline: Qt puts the bottom of its
    // box the descent of its character's font below the baseline
    // (QTextDocumentLayout::resizeInlineObject), so the box reaches that far
    // down and Placed::above up; a formula deeper than its text's font
    // reaches gets a font for its character that reaches as deep (reach()).
    QSizeF intrinsicSize(QTextDocument *document, int position, const QTextFormat &format) override;
    void drawObject(QPainter *painter, const QRectF &rect, QTextDocument *document, int position,
                    const QTextFormat &format) override;

    // Gives a character's format the font Visual Notepad Depth (bundled; see
    // scripts/make_depth_font.py) at the whole pixel size at or above
    // `depth`: no ascent, a descent of its size, no leading, on every system.
    // (A text font is no measure: Calibri and Cambria at most small sizes
    // come from bitmaps measured as no descent and a leading of their own,
    // even a negative one, and a larger size jumps by pixels.) False, and the
    // format unchanged, when the font is missing.
    static bool reach(QTextCharFormat &format, qreal depth);

private:
    QList<Placed> m_placed;
};

} // namespace vn
