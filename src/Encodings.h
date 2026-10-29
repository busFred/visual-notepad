// Text encodings. An encoding is identified by a canonical name: a Qt/ICU
// codec name ("Big5", "windows-1252", "UTF-16LE", ...) or "UTF-8 BOM", our own
// name for UTF-8 with a byte order mark.
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace vn::encoding {

inline const QString utf8 = QStringLiteral("UTF-8");
inline const QString utf8Bom = QStringLiteral("UTF-8 BOM");
inline const QString utf16LE = QStringLiteral("UTF-16LE");
inline const QString utf16BE = QStringLiteral("UTF-16BE");

struct Entry
{
    QString name;  // Canonical name.
    QString label; // For menus.
};

// Whether a decoder for the name exists on this platform.
bool isValid(const QString &name);
// The canonical spelling of a codec name or of a name stored by an older
// version ("utf8", "utf8bom", "utf16le", "utf16be", "legacy"); empty if the
// encoding is unknown or unavailable here.
QString canonical(const QString &name);
// Friendly name, e.g. "Windows 1252"; the name itself for uncommon codecs.
QString label(const QString &name);

// The byte order mark written first and skipped when reading; empty if none.
QByteArray bom(const QString &name);
// Never fails: undecodable input becomes U+FFFD. A leading BOM is removed.
QString decode(const QByteArray &bytes, const QString &name);
// Includes the BOM. Empty optional if the text has characters the encoding
// cannot represent.
std::optional<QByteArray> encode(const QString &text, const QString &name);

// Menu groups in display order (Unicode first), only codecs valid here.
QList<QList<Entry>> curatedGroups();
// All other available codecs, sorted alphabetically.
QList<Entry> otherEntries();

// The encoding for non-UTF-8 files: the "files/legacyEncoding" setting, or
// the system's.
QString legacy();
// Chooses the encoding of a file's bytes: BOM, strict UTF-8, then (if enabled)
// a guess, then legacy().
QString detect(const QByteArray &bytes);

} // namespace vn::encoding
