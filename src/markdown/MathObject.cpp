#include "MathObject.h"

#include "../Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QPainter>
#include <QTextDocument>
#include <QtMath>

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
    // The font Qt measures the descent with (resizeInlineObject).
    const qreal descent = QFontMetricsF(format.toCharFormat().font()).descent();
    return QSizeF(p->formula->width * p->scale, above(*p) + descent);
}

void MathObject::drawObject(QPainter *painter, const QRectF &rect, QTextDocument *, int, const QTextFormat &format)
{
    const Placed *p = placed(format);
    if (!p)
        return;
    // The colour of the text around it (a link's, a footnote's).
    const QBrush brush = format.toCharFormat().foreground();
    const QColor text = brush.style() == Qt::SolidPattern ? brush.color() : theme::color(theme::Role::text);
    p->formula->draw(*painter, QPointF(rect.left(), rect.top() + above(*p)), p->scale, text);
}

qreal MathObject::above(const Placed &placed)
{
    return qMax(placed.formula->ascent * placed.scale, placed.minAscent);
}

bool MathObject::deepen(QTextCharFormat &format, const QFont &font, qreal depth)
{
    qreal descent = QFontMetricsF(font).descent();
    QFont deeper = font;
    int pixels = QFontInfo(font).pixelSize();
    for (int tries = 0; tries < 8 && descent < depth; ++tries) {
        pixels = qMax(pixels + 1, qCeil(pixels * depth / qMax<qreal>(1, descent)));
        if (pixels > 4096)
            return false;
        deeper.setPixelSize(pixels);
        descent = QFontMetricsF(deeper).descent();
    }
    if (descent < depth)
        return false;
    // Every size the format had, so that the pixel size is the one used.
    format.clearProperty(QTextFormat::FontPointSize);
    format.clearProperty(QTextFormat::FontPixelSize);
    format.clearProperty(QTextFormat::FontSizeAdjustment);
    format.setFont(deeper);
    return true;
}

} // namespace vn
