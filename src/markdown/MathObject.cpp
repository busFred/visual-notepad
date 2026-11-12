#include "MathObject.h"

#include "../Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QPainter>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QtMath>

#include <algorithm>

namespace vn {

MathObject *MathObject::install(QTextDocument *document)
{
    auto *handler = new MathObject(document);
    document->documentLayout()->registerHandler(type, handler);
    return handler;
}

int MathObject::add(Placed placed)
{
    m_placed.append(std::move(placed));
    return int(m_placed.size() - 1);
}

const MathObject::Placed *MathObject::placed(const QTextFormat &format) const
{
    bool ok = false;
    const int index = format.property(indexProperty).toInt(&ok);
    if (!ok || index < 0 || index >= m_placed.size() || !m_placed.at(index).formula)
        return nullptr;
    return &m_placed.at(index);
}

qsizetype MathObject::elements() const
{
    qsizetype count = 0;
    for (const Placed &p : m_placed)
        count += p.formula ? p.formula->elements() : 0;
    return count;
}

QSizeF MathObject::intrinsicSize(QTextDocument *, int, const QTextFormat &format)
{
    const Placed *p = placed(format);
    if (!p)
        return {};
    const qreal width = p->formula->width * p->scale;
    if (p->withinLine)
        return QSizeF(width, p->box);
    // The font Qt measures the descent with (resizeInlineObject).
    const qreal descent = QFontMetricsF(format.toCharFormat().font()).descent();
    return QSizeF(width, p->above + descent);
}

void MathObject::drawObject(QPainter *painter, const QRectF &rect, QTextDocument *document, int position,
                            const QTextFormat &format)
{
    const Placed *p = placed(format);
    if (!p)
        return;
    // The colour of the text around it (a link's, a footnote's).
    const QBrush brush = format.toCharFormat().foreground();
    const QColor text = brush.style() == Qt::SolidPattern ? brush.color() : theme::color(theme::Role::text);
    qreal baseline = rect.top() + p->above;
    if (p->withinLine) {
        // The box's top is the line's (QTextLine::draw), its baseline the
        // line's ascent below.
        const QTextBlock block = document->findBlock(position);
        const QTextLayout *layout = block.isValid() ? block.layout() : nullptr;
        const QTextLine line = layout ? layout->lineForTextPosition(position - block.position()) : QTextLine();
        baseline = rect.top() + std::max(line.isValid() ? line.ascent() : 0, p->above);
    }
    p->formula->draw(*painter, QPointF(rect.left(), baseline), p->scale, text);
}

bool MathObject::deepen(QTextCharFormat &format, const QFont &font, qreal depth)
{
    QFont deeper = font;
    const int size = QFontInfo(font).pixelSize();
    int pixels = size;
    qreal descent = QFontMetricsF(font).descent();
    while (descent < depth) {
        if (++pixels > qMin(4 * size, 4096))
            return false;
        deeper.setPixelSize(pixels);
        descent = QFontMetricsF(deeper).descent();
    }
    if (pixels == size)
        return true;
    // Every size the format had, so that the pixel size is the one used.
    format.clearProperty(QTextFormat::FontPointSize);
    format.clearProperty(QTextFormat::FontPixelSize);
    format.clearProperty(QTextFormat::FontSizeAdjustment);
    format.setFont(deeper);
    return true;
}

} // namespace vn
