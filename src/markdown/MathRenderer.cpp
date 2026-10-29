#include "MathRenderer.h"

#include "../Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QStringList>
#include <QtMath>
#include <QtGlobal>

#include <cmath>
#include <exception>
#include <memory>
#include <string>

#include "core/formula.h"
#include "core/macro.h"
#include "latex.h"
#include "platform/qt/graphic_qt.h"
#include "render.h"

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

// MicroTeX lays glyphs out in 1pt units and draws them through a painter that
// is scaled by the text size. Rasterizing 1pt fonts under such a transform
// makes FreeType overflow and drop glyphs, so draw text at its final size
// under an unscaled painter instead.
class Graphics2D : public tex::Graphics2D_qt
{
public:
    using tex::Graphics2D_qt::Graphics2D_qt;

    void drawText(const std::wstring &text, float x, float y) override
    {
        const auto *font = static_cast<const tex::Font_qt *>(getFont());
        const qreal scale = sx();
        if (!font || scale <= 0)
            return;
        QFont qfont = font->getQFont();
        qfont.setPointSizeF(qfont.pointSizeF() * scale);
        QPainter *painter = getQPainter();
        painter->save();
        painter->scale(1 / scale, 1 / scale);
        painter->setFont(qfont);
        painter->drawText(QPointF(x * scale, y * scale), tex::wstring_to_QString(text));
        painter->restore();
    }

    void drawChar(wchar_t c, float x, float y) override { drawText(std::wstring(1, c), x, y); }
};

QString findResources()
{
    const QString app = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        app + QStringLiteral("/microtex/res"),
        app + QStringLiteral("/../microtex/res"),
        app + QStringLiteral("/../share/visnotepad/microtex/res"),
    };
#ifdef VN_MICROTEX_RES
    candidates << QStringLiteral(VN_MICROTEX_RES);
#endif
    for (const QString &dir : std::as_const(candidates)) {
        if (QFile::exists(dir + QStringLiteral("/.clatexmath-res_root")))
            return QDir::cleanPath(dir);
    }
    return {};
}

// Initializes MicroTeX once; a failure sticks and selects the text fallback.
bool ensureInitialized()
{
    static int state = 0; // 0 = not tried, 1 = ready, -1 = unavailable
    if (state != 0)
        return state > 0;
    state = -1;
    const QString res = findResources();
    if (res.isEmpty()) {
        qWarning("MicroTeX resources not found; math is shown as source text");
        return false;
    }
    try {
        tex::LaTeX::init(QFile::encodeName(res).toStdString());
        // Settings macros are prepended to every formula; let them redefine.
        tex::NewCommandMacro::_errIfConflict = false;
        state = 1;
    } catch (const std::exception &e) {
        qWarning("MicroTeX initialization failed: %s", e.what());
    }
    return state > 0;
}

QImage renderMicroTeX(const QString &tex, const Style &style)
{
    constexpr qreal padding = 1;
    constexpr int layoutWidth = 20000; // formulas never wrap
    if (!ensureInitialized())
        return {};
    try {
        tex::Formula formula(tex.toStdWString());
        tex::TeXRenderBuilder builder;
        // Qt converts the point sizes MicroTeX gives its fonts using the
        // paint device's DPI; the image below is set to 72 DPI so that
        // one point is one logical pixel.
        const auto fg = static_cast<tex::color>(style.color.rgba());
        std::unique_ptr<tex::TeXRender> render(
            builder.setStyle(style.display ? tex::TexStyle::display : tex::TexStyle::text)
                .setTextSize(static_cast<float>(style.pixelSize))
                .setWidth(tex::UnitType::pixel, layoutWidth, tex::Alignment::left)
                .setIsMaxWidth(true)
                .setLineSpace(tex::UnitType::pixel, 0)
                .setForeground(fg)
                .build(formula));
        const qreal width = render->getWidth();
        const qreal height = render->getHeight() + render->getDepth();
        if (width <= 0 || height <= 0)
            return {};

        const qreal dpr = style.devicePixelRatio;
        QImage image(QSize(qCeil((width + 2 * padding) * dpr), qCeil((height + 2 * padding) * dpr)),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.setDotsPerMeterX(2835); // 72 DPI
        image.setDotsPerMeterY(2835);
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
            Graphics2D g2(&painter);
            render->draw(g2, qRound(padding), qRound(padding));
        }
        return image;
    } catch (const std::exception &e) {
        qWarning("MicroTeX failed on \"%s\": %s", qPrintable(tex.left(60)), e.what());
    }
    return {};
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
    QImage image = renderMicroTeX(tex, style);
    return image.isNull() ? renderSource(tex, style) : image;
}

} // namespace vn::math
