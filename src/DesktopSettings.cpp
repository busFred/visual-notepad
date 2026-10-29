#include "DesktopSettings.h"

#include <QFontDatabase>
#include <QGuiApplication>
#include <QHash>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vn::desktop {

namespace {

// Pango's style, variant, stretch and gravity words carry nothing we use;
// weights and slants map onto QFont.
const QHash<QString, QFont::Weight> &weights()
{
    static const QHash<QString, QFont::Weight> map = {
        {QStringLiteral("thin"), QFont::Thin},           {QStringLiteral("ultra-light"), QFont::ExtraLight},
        {QStringLiteral("extra-light"), QFont::ExtraLight}, {QStringLiteral("light"), QFont::Light},
        {QStringLiteral("semi-light"), QFont::Light},    {QStringLiteral("demi-light"), QFont::Light},
        {QStringLiteral("book"), QFont::Normal},         {QStringLiteral("regular"), QFont::Normal},
        {QStringLiteral("medium"), QFont::Medium},       {QStringLiteral("semi-bold"), QFont::DemiBold},
        {QStringLiteral("demi-bold"), QFont::DemiBold},  {QStringLiteral("bold"), QFont::Bold},
        {QStringLiteral("ultra-bold"), QFont::ExtraBold}, {QStringLiteral("extra-bold"), QFont::ExtraBold},
        {QStringLiteral("heavy"), QFont::Black},         {QStringLiteral("black"), QFont::Black},
        {QStringLiteral("ultra-black"), QFont::Black},   {QStringLiteral("extra-black"), QFont::Black},
    };
    return map;
}

bool isOtherStyleWord(const QString &word)
{
    static const QStringList words = {
        QStringLiteral("normal"),          QStringLiteral("roman"),           QStringLiteral("small-caps"),
        QStringLiteral("all-small-caps"),  QStringLiteral("petite-caps"),     QStringLiteral("all-petite-caps"),
        QStringLiteral("unicase"),         QStringLiteral("title-caps"),      QStringLiteral("ultra-condensed"),
        QStringLiteral("extra-condensed"), QStringLiteral("condensed"),       QStringLiteral("semi-condensed"),
        QStringLiteral("semi-expanded"),   QStringLiteral("expanded"),        QStringLiteral("extra-expanded"),
        QStringLiteral("ultra-expanded"),  QStringLiteral("not-rotated"),     QStringLiteral("south"),
        QStringLiteral("upside-down"),     QStringLiteral("north"),           QStringLiteral("rotated-left"),
        QStringLiteral("east"),            QStringLiteral("rotated-right"),   QStringLiteral("west"),
        QStringLiteral("with-color"),      QStringLiteral("without-color"),
    };
    return words.contains(word);
}

} // namespace

FontDescription parseFontDescription(const QString &text)
{
    FontDescription result;
    QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    // Variations (@…) and features (#…) come last.
    while (!words.isEmpty() && (words.last().startsWith(u'@') || words.last().startsWith(u'#')))
        words.removeLast();
    if (!words.isEmpty()) {
        QString size = words.last();
        const bool pixels = size.endsWith(QLatin1String("px"));
        if (pixels)
            size.chop(2);
        bool ok = false;
        const qreal value = size.toDouble(&ok);
        if (ok && value > 0) {
            result.size = value;
            result.pixels = pixels;
            words.removeLast();
        }
    }
    // Style words, from the end, until the family list: a word that isn't a
    // style, or one ending with the list's comma.
    while (words.size() > 1 && !words.last().endsWith(QLatin1Char(','))) {
        const QString word = words.last().toLower();
        if (const auto weight = weights().constFind(word); weight != weights().constEnd())
            result.weight = *weight;
        else if (word == QLatin1String("italic") || word == QLatin1String("oblique"))
            result.italic = true;
        else if (!isOtherStyleWord(word))
            break;
        words.removeLast();
    }
    for (const QString &family : words.join(QLatin1Char(' ')).split(QLatin1Char(','))) {
        if (const QString trimmed = family.trimmed(); !trimmed.isEmpty())
            result.families.append(trimmed);
    }
    return result;
}

QString interfaceSetting(const char *key)
{
#ifdef Q_OS_LINUX
    const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (desktop && (std::strstr(desktop, "KDE") || std::strstr(desktop, "LXQt")))
        return {};
    const char *schema = desktop && std::strstr(desktop, "Cinnamon") ? "org.cinnamon.desktop.interface"
                         : desktop && std::strstr(desktop, "MATE")   ? "org.mate.interface"
                                                                     : "org.gnome.desktop.interface";
    const QByteArray command = QByteArray("gsettings get ") + schema + ' ' + key + " 2>/dev/null";
    FILE *pipe = popen(command.constData(), "r");
    if (!pipe)
        return {};
    QByteArray output;
    char buffer[256];
    while (std::fgets(buffer, sizeof buffer, pipe))
        output += buffer;
    pclose(pipe);
    QString value = QString::fromUtf8(output).trimmed();
    // Strings print quoted ('Segoe UI 10'); numbers may carry a type ("uint32 0").
    if (value.size() >= 2 && value.startsWith(u'\'') && value.endsWith(u'\''))
        value = value.mid(1, value.size() - 2);
    return value;
#else
    Q_UNUSED(key);
    return {};
#endif
}

int cursorSize()
{
    const int size = interfaceSetting("cursor-size").section(QLatin1Char(' '), -1).toInt();
    return size > 0 && size <= 256 ? size : 0;
}

std::optional<QFont> uiFont()
{
    // Headless runs (tests) have no desktop, whatever gsettings says.
    if (QGuiApplication::platformName() == QLatin1String("offscreen"))
        return std::nullopt;
    const QString name = interfaceSetting("font-name");
    if (name.isEmpty())
        return std::nullopt;
    const FontDescription description = parseFontDescription(name);
    QStringList installed;
    for (const QString &family : description.families) {
        if (QFontDatabase::hasFamily(family))
            installed.append(family);
    }
    if (installed.isEmpty())
        return std::nullopt;
    QFont font;
    font.setFamilies(installed);
    if (description.size > 0) {
        if (description.pixels)
            font.setPixelSize(qRound(description.size));
        else
            font.setPointSizeF(description.size);
    }
    font.setWeight(description.weight);
    font.setItalic(description.italic);
    return font;
}

} // namespace vn::desktop
