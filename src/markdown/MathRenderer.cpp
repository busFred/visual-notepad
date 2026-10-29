#include "MathRenderer.h"

#include "../Settings.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>

namespace vn::math {

namespace {

QImage renderSource(const QString &tex, const Style &style)
{
    QFont font(QStringLiteral("serif"));
    font.setStyleHint(QFont::Serif);
    font.setItalic(true);
    font.setPixelSize(qRound(style.pixelSize));
    const QFontMetricsF fm(font);
    const QString text = tex.simplified();
    const QSizeF size(fm.horizontalAdvance(text) + 4, fm.height());
    QImage image((size * style.devicePixelRatio).toSize(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(style.devicePixelRatio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(style.color);
    painter.drawText(QPointF(2, fm.ascent()), text);
    return image;
}

} // namespace

QString withMacros(const QString &tex)
{
    const QVariantMap macros = Settings::instance().value(key::mathMacros).toMap();
    QString prefix;
    for (auto it = macros.cbegin(); it != macros.cend(); ++it) {
        const QString command = it.key().trimmed();
        if (command.isEmpty())
            continue;
        prefix += QStringLiteral("\\newcommand{%1}{%2}").arg(command, it.value().toString());
    }
    return prefix + tex;
}

QImage render(const QString &tex, const Style &style)
{
    return renderSource(tex, style);
}

} // namespace vn::math
