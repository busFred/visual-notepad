#include "Encodings.h"

#include "Settings.h"

#include <QHash>
#include <QSet>
#include <QStringConverter>
#include <QStringDecoder>
#include <QStringEncoder>

#include <algorithm>

#ifdef VN_HAVE_ICU
#include <unicode/ucsdet.h>
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <langinfo.h>
#endif

namespace vn::encoding {

namespace {

constexpr int binarySniffSize = 8192;
constexpr int guessSampleSize = 64 * 1024;
constexpr int minGuessConfidence = 25;

const QByteArray utf8BomBytes("\xEF\xBB\xBF");
const QByteArray utf16LeBomBytes("\xFF\xFE");
const QByteArray utf16BeBomBytes("\xFE\xFF");

struct Curated
{
    const char *name;
    const char *label;
};

// Names are as ICU knows them; every one must decode on the platform to be
// listed. Groups are separated in the menus.
const QList<QList<Curated>> &curatedTable()
{
    static const QList<QList<Curated>> table = {
        {{"UTF-8", "UTF-8"}, {"UTF-8 BOM", "UTF-8 with BOM"}, {"UTF-16LE", "UTF-16 LE"}, {"UTF-16BE", "UTF-16 BE"}},
        {{"windows-1252", "Western (Windows 1252)"},
         {"ISO-8859-1", "Western (ISO 8859-1)"},
         {"ISO-8859-15", "Western (ISO 8859-15)"},
         {"macintosh", "Western (Mac Roman)"}},
        {{"windows-1250", "Central European (Windows 1250)"},
         {"ISO-8859-2", "Central European (ISO 8859-2)"},
         {"windows-1251", "Cyrillic (Windows 1251)"},
         {"ISO-8859-5", "Cyrillic (ISO 8859-5)"},
         {"KOI8-R", "Cyrillic (KOI8-R)"},
         {"KOI8-U", "Cyrillic (KOI8-U)"},
         {"IBM866", "Cyrillic (CP 866)"},
         {"IBM852", "Central European (CP 852)"}},
        {{"windows-1253", "Greek (Windows 1253)"}, {"ISO-8859-7", "Greek (ISO 8859-7)"}},
        {{"windows-1254", "Turkish (Windows 1254)"}, {"ISO-8859-9", "Turkish (ISO 8859-9)"}},
        {{"windows-1255", "Hebrew (Windows 1255)"}, {"ISO-8859-8", "Hebrew (ISO 8859-8)"}},
        {{"windows-1256", "Arabic (Windows 1256)"}, {"ISO-8859-6", "Arabic (ISO 8859-6)"}},
        {{"windows-1257", "Baltic (Windows 1257)"},
         {"ISO-8859-4", "Baltic (ISO 8859-4)"},
         {"ISO-8859-13", "Baltic (ISO 8859-13)"}},
        {{"windows-1258", "Vietnamese (Windows 1258)"}},
        {{"windows-874", "Thai (Windows 874)"}, {"TIS-620", "Thai (TIS-620)"}},
        {{"Big5", "Traditional Chinese (Big5)"}, {"Big5-HKSCS", "Traditional Chinese (Big5-HKSCS)"}},
        {{"GB18030", "Simplified Chinese (GB18030)"},
         {"GBK", "Simplified Chinese (GBK)"},
         {"GB2312", "Simplified Chinese (GB 2312)"}},
        {{"Shift_JIS", "Japanese (Shift JIS)"}, {"EUC-JP", "Japanese (EUC-JP)"}, {"ISO-2022-JP", "Japanese (ISO-2022-JP)"}},
        {{"EUC-KR", "Korean (EUC-KR)"}},
        {{"IBM437", "DOS (CP437)"}, {"IBM850", "DOS (CP850)"}, {"IBM865", "DOS (CP865)"}},
    };
    return table;
}

// Spellings of codecs that the curated list already offers.
bool isCuratedAlias(const QString &lower)
{
    static const QSet<QString> aliases = {QStringLiteral("windows-874-2000"), QStringLiteral("us-ascii")};
    return aliases.contains(lower);
}

QByteArray codecName(const QString &name)
{
    return (name == utf8Bom ? utf8 : name).toLatin1();
}

bool isUnicodeFamily(const QString &name)
{
    return name.startsWith(QLatin1String("UTF-"), Qt::CaseInsensitive);
}

// Case-insensitive map from a lower-cased name to the spelling Qt lists.
const QHash<QString, QString> &knownNames()
{
    static const QHash<QString, QString> names = [] {
        QHash<QString, QString> map;
        for (const QString &name : QStringConverter::availableCodecs())
            map.insert(name.toLower(), name);
        map.insert(utf8Bom.toLower(), utf8Bom);
        return map;
    }();
    return names;
}

bool decoderExists(const QString &name);

// The spelling Qt lists for a codec name (any case), or an alias ICU
// resolves but does not list; empty if it cannot be decoded here.
QString lookup(const QString &name)
{
    const QString trimmed = name.trimmed();
    const QString known = knownNames().value(trimmed.toLower());
    if (!known.isEmpty())
        return decoderExists(known) ? known : QString();
    return !trimmed.isEmpty() && decoderExists(trimmed) ? trimmed : QString();
}

bool decoderExists(const QString &name)
{
    // Constructing decoders for every codec is not free; remember the answers.
    static QHash<QString, bool> cache;
    auto it = cache.find(name);
    if (it == cache.end())
        it = cache.insert(name, QStringDecoder(codecName(name).constData()).isValid());
    return it.value();
}

QString systemLegacy()
{
#ifdef Q_OS_WIN
    // The ANSI code page; ICU knows it as "windows-<number>" or an alias.
    const UINT page = GetACP();
    if (page != CP_UTF8) {
        const QString name = lookup(QStringLiteral("windows-%1").arg(page));
        if (!name.isEmpty())
            return name;
        if (QStringDecoder(QStringConverter::System).isValid())
            return QStringLiteral("Locale");
    }
    return QStringLiteral("windows-1252");
#else
    // A UTF-8 (or C) locale says nothing about old files; windows-1252 is a
    // superset of ISO 8859-1 for printable text and what such files usually use.
    const QString codeset = QString::fromLatin1(nl_langinfo(CODESET));
    const QString name = lookup(codeset);
    const bool unhelpful = name.isEmpty() || isUnicodeFamily(name) || name == QLatin1String("US-ASCII")
                           || codeset == QLatin1String("ANSI_X3.4-1968");
    return unhelpful ? QStringLiteral("windows-1252") : name;
#endif
}

#ifdef VN_HAVE_ICU
// ICU's guess for the bytes, or an empty string.
QString guess(const QByteArray &bytes)
{
    UErrorCode status = U_ZERO_ERROR;
    UCharsetDetector *detector = ucsdet_open(&status);
    if (U_FAILURE(status))
        return {};
    QString result;
    ucsdet_setText(detector, bytes.constData(), int(qMin<qsizetype>(bytes.size(), guessSampleSize)), &status);
    const UCharsetMatch *match = U_SUCCESS(status) ? ucsdet_detect(detector, &status) : nullptr;
    if (match && U_SUCCESS(status) && ucsdet_getConfidence(match, &status) >= minGuessConfidence) {
        // The file has no NUL bytes, so a UTF-16/32 verdict is spurious.
        const QString name = lookup(QString::fromLatin1(ucsdet_getName(match, &status)));
        if (U_SUCCESS(status) && !isUnicodeFamily(name))
            result = name;
    }
    ucsdet_close(detector);
    return result;
}
#else
QString guess(const QByteArray &)
{
    return {};
}
#endif

} // namespace

bool isValid(const QString &name)
{
    return !name.isEmpty() && decoderExists(name);
}

QString canonical(const QString &name)
{
    const QString lower = name.trimmed().toLower();
    if (lower.isEmpty())
        return {};
    // Names written by older versions.
    if (lower == QLatin1String("utf8"))
        return utf8;
    if (lower == QLatin1String("utf8bom"))
        return utf8Bom;
    if (lower == QLatin1String("utf16le"))
        return utf16LE;
    if (lower == QLatin1String("utf16be"))
        return utf16BE;
    if (lower == QLatin1String("legacy"))
        return legacy();
    return lookup(name);
}

QString label(const QString &name)
{
    for (const auto &group : curatedTable()) {
        for (const Curated &c : group) {
            if (name == QLatin1String(c.name))
                return QString::fromLatin1(c.label);
        }
    }
    return name;
}

QByteArray bom(const QString &name)
{
    if (name == utf8Bom)
        return utf8BomBytes;
    if (name == utf16LE)
        return utf16LeBomBytes;
    if (name == utf16BE)
        return utf16BeBomBytes;
    return {};
}

QString decode(const QByteArray &bytes, const QString &name)
{
    const QByteArray mark = bom(name);
    QStringDecoder decoder(codecName(name).constData(), QStringDecoder::Flag::Stateless);
    if (!decoder.isValid())
        return QString::fromUtf8(bytes);
    QByteArrayView view(bytes);
    if (!mark.isEmpty() && bytes.startsWith(mark))
        view = view.sliced(mark.size());
    return decoder(view);
}

std::optional<QByteArray> encode(const QString &text, const QString &name)
{
    QStringEncoder encoder(codecName(name).constData(), QStringEncoder::Flag::Stateless);
    if (!encoder.isValid())
        return std::nullopt;
    const QByteArray encoded = encoder(text);
    if (encoder.hasError())
        return std::nullopt;
    // Some encoders substitute '?' without flagging an error, so other
    // encodings are verified by decoding the result again.
    if (!isUnicodeFamily(name) && name != utf8Bom
        && QStringDecoder(codecName(name).constData(), QStringDecoder::Flag::Stateless).decode(encoded) != text)
        return std::nullopt;
    return bom(name) + encoded;
}

QList<QList<Entry>> curatedGroups()
{
    static const QList<QList<Entry>> groups = [] {
        QList<QList<Entry>> result;
        for (const auto &group : curatedTable()) {
            QList<Entry> entries;
            for (const Curated &c : group) {
                const QString name = QString::fromLatin1(c.name);
                if (isValid(name))
                    entries.append({name, label(name)});
            }
            if (!entries.isEmpty())
                result.append(entries);
        }
        return result;
    }();
    return groups;
}

QList<Entry> otherEntries()
{
    static const QList<Entry> others = [] {
        QSet<QString> shown;
        for (const auto &group : curatedGroups()) {
            for (const Entry &e : group)
                shown.insert(e.name.toLower());
        }
        const QSet<QString> unicode = {utf8.toLower(),   utf16LE.toLower(),  utf16BE.toLower(),
                                       QStringLiteral("utf-32le"), QStringLiteral("utf-32be"), QStringLiteral("utf-7")};
        QList<Entry> result;
        QSet<QString> seen;
        for (const QString &name : QStringConverter::availableCodecs()) {
            const QString lower = name.toLower();
            // "Locale" depends on the machine; the bare UTF-16/32 names and
            // their variants are ambiguous about byte order and BOM.
            if (lower == QLatin1String("locale") || (lower.startsWith(QLatin1String("utf")) && !unicode.contains(lower)))
                continue;
            if (shown.contains(lower) || isCuratedAlias(lower) || seen.contains(lower) || !isValid(name))
                continue;
            seen.insert(lower);
            result.append({name, name});
        }
        std::sort(result.begin(), result.end(),
                  [](const Entry &a, const Entry &b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
        return result;
    }();
    return others;
}

QString legacy()
{
    const QString chosen = Settings::instance().text(key::legacyEncoding);
    if (!chosen.isEmpty()) {
        const QString name = lookup(chosen);
        if (!name.isEmpty())
            return name;
    }
    static const QString system = systemLegacy();
    return system;
}

QString detect(const QByteArray &bytes)
{
    for (const QString &name : {utf8Bom, utf16LE, utf16BE}) {
        if (bytes.startsWith(bom(name)))
            return name;
    }
    // Files with NUL bytes that are not UTF-16 are treated as legacy text,
    // like Notepad.
    if (bytes.left(binarySniffSize).contains('\0'))
        return legacy();
    QStringDecoder utf8Decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString decoded = utf8Decoder(bytes); // Decoding is lazy; force it.
    Q_UNUSED(decoded);
    if (!utf8Decoder.hasError())
        return utf8;
    if (Settings::instance().flag(key::guessEncoding)) {
        const QString guessed = guess(bytes);
        if (!guessed.isEmpty())
            return guessed;
    }
    return legacy();
}

} // namespace vn::encoding
