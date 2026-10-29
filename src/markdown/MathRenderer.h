// Renders TeX math to an image. Uses MicroTeX when available; otherwise
// draws the TeX source in an italic serif face so the text stays readable.
#pragma once

#include <QColor>
#include <QImage>
#include <QString>

namespace vn::math {

struct Style
{
    qreal pixelSize = 16;
    QColor color = Qt::black;
    qreal devicePixelRatio = 1;
    bool display = false;
};

// Never returns a null image for non-empty input.
QImage render(const QString &tex, const Style &style);

// Prepends \newcommand definitions for the user's macros from Settings.
QString withMacros(const QString &tex);

} // namespace vn::math
