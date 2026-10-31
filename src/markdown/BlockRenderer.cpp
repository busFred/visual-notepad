#include "BlockRenderer.h"

#include "../Theme.h"
#include "MarkdownEditing.h"
#include "MathRenderer.h"
#include "RemoteImages.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QCache>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QPicture>
#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextLayout>
#include <QThreadPool>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>
#include <atomic>
#include <deque>

namespace vn {

namespace {

using namespace Qt::StringLiterals;

constexpr qreal codePadX = 10;
constexpr qreal codePadY = 5;
constexpr qreal blockGap = 4;

qsizetype imageCost(const QImage &image)
{
    return qMax<qsizetype>(1, image.sizeInBytes() / 1024);
}

// Rendered math by TeX and style. Blocks are re-created after edits and
// option changes, and MicroTeX is slow enough to notice; GUI thread only.
QImage cachedMath(const QString &tex, const math::Style &style)
{
    static QCache<QString, QImage> cache(32 * 1024); // KiB
    const QString key = QStringLiteral("%1|%2|%3|%4|")
                            .arg(style.pixelSize)
                            .arg(style.color.rgba())
                            .arg(style.devicePixelRatio)
                            .arg(int(style.display))
                        + tex;
    if (const QImage *hit = cache.object(key))
        return *hit;
    const QImage image = math::render(tex, style);
    cache.insert(key, new QImage(image), imageCost(image));
    return image;
}

// Largest SVG, once unpacked, that is scanned and opened. Pictures that are
// shown in a column are far smaller; a gzip bomb is not.
constexpr qint64 maxSvgBytes = 32 * 1024 * 1024;

// True if the SVG in `file` (open for reading, a file or a download) names
// a file on another machine, which the SVG plugin would open by itself:
// <image href> pictures and xml-stylesheet style sheets. Read the way QtSvg
// reads it: a file that starts with the gzip magic is an .svgz, unpacked
// (every member) before it is parsed. One that cannot be unpacked, or is too
// big, counts as naming one: it is not opened. Any thread.
bool svgNamesNetworkPath(QIODevice &file)
{
    QBuffer unpacked;
    QIODevice *source = &file;
    if (file.peek(2) == "\x1f\x8b") {
        const QByteArray packed = file.read(maxSvgBytes + 1);
        if (packed.size() > maxSvgBytes)
            return true;
        const rust::Vec<uint8_t> text = core::gunzip_bounded(
            rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(packed.constData()), size_t(packed.size())),
            size_t(maxSvgBytes));
        if (text.empty())
            return true;
        unpacked.setData(reinterpret_cast<const char *>(text.data()), qsizetype(text.size()));
        unpacked.open(QIODevice::ReadOnly);
        source = &unpacked;
    }
    static const QRegularExpression href(QStringLiteral(R"(href\s*=\s*["']([^"']*))"),
                                         QRegularExpression::CaseInsensitiveOption);
    QXmlStreamReader xml(source);
    xml.setNamespaceProcessing(false);
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::ProcessingInstruction) {
            const QRegularExpressionMatch match = href.matchView(xml.processingInstructionData());
            if (match.hasMatch() && md::isNetworkPath(match.captured(1)))
                return true;
        } else if (token == QXmlStreamReader::StartElement) {
            for (const QXmlStreamAttribute &attribute : xml.attributes()) {
                const QStringView name = attribute.qualifiedName();
                if ((name == u"href" || name.endsWith(u":href")) && md::isNetworkPath(attribute.value().toString()))
                    return true;
            }
        }
    }
    return false;
}

// Picture files read, for tests.
std::atomic<int> localImageReads{0};

// A picture file, unless it is an SVG that would reach another machine.
QImage readImage(const QString &path)
{
    ++localImageReads;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (reader.format().startsWith("svg")) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly) && svgNamesNetworkPath(file))
            return {};
    }
    return reader.read();
}

// Decoded picture files, by file version, for as long as their total size
// stays within a limit. Decoding is far slower than scaling (100 ms against
// 1 ms for a 3000 by 2000 PNG) and every new window width needs a new size.
// GUI thread only.
//
// Opaque pictures are kept as 24 bit RGB, a quarter smaller; they are widened
// again for scaling (which converts them anyway), so the scaled pixels are
// the same.
//
// A page with more big pictures than fit, shown picture after picture again
// and again (every window width), would take every picture out of a plain
// least-recently-used cache just before it is needed again: no read is ever
// saved. So when the cache is full, a picture only replaces older ones if it
// is asked for again soon after it was last read (within as many requests as
// the cache holds pictures, the way a few pictures being looked at are); a
// picture read for the first time in a longer round leaves the cache as it
// is, so the pictures in it keep serving rounds.
class DecodedPictures
{
public:
    void setLimitKiB(qsizetype kib)
    {
        m_limit = kib;
        clear();
    }

    QImage find(const QString &version)
    {
        ++m_requests;
        const auto it = m_entries.find(version);
        if (it == m_entries.end())
            return {};
        it->used = ++m_clock;
        return it->compact ? it->image.convertToFormat(QImage::Format_RGB32) : it->image;
    }

    // `image` was just read because find() had nothing.
    void insert(const QString &version, const QImage &image)
    {
        const bool compact = image.format() == QImage::Format_RGB32;
        const qsizetype kib = compact ? imageCost(image) * 3 / 4 : imageCost(image);
        if (kib > m_limit)
            return; // Does not fit at all.
        bool again = false;
        for (auto it = m_reads.begin(); it != m_reads.end(); ++it) {
            if (it->first == version) {
                again = m_requests - it->second <= qMax<quint64>(4, m_entries.size());
                m_reads.erase(it);
                break;
            }
        }
        m_reads.push_back({version, m_requests});
        if (m_reads.size() > 64)
            m_reads.pop_front();
        if (m_kib + kib > m_limit && !again)
            return; // Full, and not shown to be wanted again soon.
        evictFor(kib);
        Entry entry;
        entry.compact = compact;
        entry.image = compact ? image.convertToFormat(QImage::Format_RGB888) : image;
        entry.kib = imageCost(entry.image);
        entry.used = ++m_clock;
        m_kib += entry.kib;
        m_entries.insert(version, entry);
    }

    void clear()
    {
        m_entries.clear();
        m_reads.clear();
        m_kib = 0;
    }

private:
    struct Entry
    {
        QImage image;
        qsizetype kib = 0;
        quint64 used = 0;
        bool compact = false;
    };

    // Least recently used out until `kib` more fit.
    void evictFor(qsizetype kib)
    {
        while (m_kib + kib > m_limit && !m_entries.isEmpty()) {
            auto oldest = m_entries.begin();
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
                if (it->used < oldest->used)
                    oldest = it;
            }
            m_kib -= oldest->kib;
            m_entries.erase(oldest);
        }
    }

    QHash<QString, Entry> m_entries;
    std::deque<std::pair<QString, quint64>> m_reads; // Versions read lately, with the request count then.
    quint64 m_requests = 0;
    qsizetype m_kib = 0;
    qsizetype m_limit = 128 * 1024; // KiB
    quint64 m_clock = 0;
};

DecodedPictures &decodedPictures()
{
    static DecodedPictures pictures;
    return pictures;
}

// One image pixel per logical pixel, shrunk to fit the column.
QImage fitToColumn(QImage image, qreal width, qreal dpr)
{
    if (image.width() > width) {
        image = image.scaledToWidth(int(width * dpr), Qt::SmoothTransformation);
        image.setDevicePixelRatio(dpr);
    }
    return image;
}

// Local pictures, read and scaled to the column, by file version and size.
// The decoded files are kept too (see above). SVGs are kept as decoded as
// well: they are read at their own size, never at the column's, so a bigger
// or smaller column scales the same pixels either way.
QImage cachedLocalImage(const QString &path, qreal width, qreal dpr)
{
    static QCache<QString, QImage> cache(64 * 1024); // KiB
    const QFileInfo info(path);
    const QString version = QStringLiteral("%1|%2|").arg(info.lastModified().toMSecsSinceEpoch()).arg(info.size()) + path;
    const QString key = QStringLiteral("%1|%2|").arg(width).arg(dpr) + version;
    if (const QImage *hit = cache.object(key))
        return *hit;
    QImage image = decodedPictures().find(version);
    if (image.isNull()) {
        image = readImage(path);
        if (image.isNull())
            return image; // Not cached: the file may appear later.
        decodedPictures().insert(version, image);
    }
    image = fitToColumn(image, width, dpr);
    cache.insert(key, new QImage(image), imageCost(image));
    return image;
}

// Downloaded pictures scaled to the column, by address (null until
// downloaded). Their decoded full-size form is RemoteImages' to keep, as
// DecodedPictures keeps the files': decoded once on a worker thread when they
// arrive, in a bounded cache of its own (shared with the zoom viewer).
QImage cachedRemoteImage(const QUrl &url, qreal width, qreal dpr)
{
    static QCache<QString, QImage> cache(32 * 1024); // KiB
    RemoteImages &remote = RemoteImages::instance();
    const QString key = QStringLiteral("%1|%2|%3|").arg(remote.generation()).arg(width).arg(dpr)
                        + url.toString(QUrl::FullyEncoded);
    if (const QImage *hit = cache.object(key))
        return *hit;
    const QImage image = fitToColumn(remote.fullImage(url), width, dpr);
    if (!image.isNull())
        cache.insert(key, new QImage(image), imageCost(image));
    return image;
}

// A picture file at full size, for the zoom viewer.
QImage fullSizeImage(const QString &path)
{
    return readImage(path);
}

// Mermaid diagrams by theme, scale and code; filled by worker threads.
struct Diagram
{
    QImage image;
    QString error;
};

QCache<QString, Diagram> &diagramCache()
{
    static QCache<QString, Diagram> cache(64 * 1024); // KiB
    return cache;
}

// Renders started, for tests.
std::atomic<int> diagramRenders{0};

// True with the diagram if it is rendered; otherwise starts rendering it
// (once) on the thread pool and returns false.
bool mermaidDiagram(const QString &code, const RenderContext &context, Diagram *result)
{
    static QSet<QString> pending;
    const qreal dpr = context.devicePixelRatio;
    const QString key = context.mermaidTheme + u'|' + QString::number(dpr) + u'|' + code;
    if (const Diagram *hit = diagramCache().object(key)) {
        *result = *hit;
        return true;
    }
    if (pending.contains(key))
        return false;
    pending.insert(key);
    // Made here, on the GUI thread, so the result is delivered there.
    DiagramNotifier *notifier = &DiagramNotifier::instance();
    QThreadPool::globalInstance()->start([key, dpr, notifier, utf8 = code.toUtf8(),
                                          themeName = context.mermaidTheme.toUtf8()] {
        // mermaid_image is plain Rust (no Qt), and the QImage is built off the GUI thread.
        Diagram diagram;
        ++diagramRenders;
        try {
            const core::RgbaImage raw = core::mermaid_image(rust::Str(utf8.constData(), utf8.size()),
                                                            rust::Str(themeName.constData(), themeName.size()),
                                                            float(dpr));
            // tiny-skia pixmaps are premultiplied RGBA8888; copy so the image owns its pixels.
            diagram.image = QImage(raw.pixels.data(), int(raw.width), int(raw.height), int(raw.width) * 4,
                                   QImage::Format_RGBA8888_Premultiplied).copy();
            // A diagram too big for the requested ratio comes back drawn at a lower one.
            diagram.image.setDevicePixelRatio(raw.scale);
        } catch (const std::exception &e) { // rust::Error
            diagram.error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(notifier, [key, diagram]() mutable {
            // Every finished render must be found again, or the view would start
            // it over and over: what the cache cannot hold becomes an error.
            qsizetype cost = imageCost(diagram.image);
            if (cost > diagramCache().maxCost()) {
                diagram.image = QImage();
                diagram.error = QObject::tr("diagram too large");
                cost = 1;
            }
            diagramCache().insert(key, new Diagram(diagram), cost);
            pending.remove(key);
            Q_EMIT DiagramNotifier::instance().ready();
        }, Qt::QueuedConnection);
    });
    return false;
}

// True for a picture or style sheet source on another machine:
// "//host/x.png", "file://host/x.png", "file:////host/x.png",
// "%2F%2Fhost/x.png", "\\host\x.png". On Windows opening one is an SMB
// connection that hands the host the user's password hash.
bool namesOtherMachine(const QUrl &url)
{
    const QString scheme = url.scheme();
    const bool file = scheme == QLatin1String("file");
    if (!file && scheme.size() > 1)
        return false; // Not a file (web pictures); single letters are Windows drives.
    if ((scheme.isEmpty() || file) && !url.host().isEmpty())
        return true;
    return md::isNetworkPath(file ? url.toLocalFile() : QUrl::fromPercentEncoding(url.toString().toUtf8()));
}

// Whether HTML can make Qt ask for a picture: an img tag, a background
// attribute or a style. (Tag and attribute names are never entity-encoded.)
bool mayLoadPictures(const QString &html)
{
    return html.contains(QLatin1String("<img"), Qt::CaseInsensitive)
           || html.contains(QLatin1String("background"), Qt::CaseInsensitive)
           || html.contains(QLatin1String("style"), Qt::CaseInsensitive);
}

// Rich text with images resolved here: math placeholders, files, and
// downloads when the user agreed to them. A listing document only collects
// the web addresses Qt asks for: every picture is a blank stand-in, so
// listing reads, decodes and renders nothing.
class ResourceDocument : public QTextDocument
{
public:
    ResourceDocument(const core::Block &block, const RenderContext &context, bool listing = false)
        : m_context(context)
        , m_listing(listing)
    {
        for (const auto &tex : block.inline_math)
            m_math.append(QString::fromUtf8(tex.data(), tex.size()));
    }

    // Pictures wider than the column (an explicit width="1060" in the HTML)
    // shrink to it, keeping their proportions.
    void fitImages(qreal width)
    {
        for (QTextBlock block = begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.charFormat().isImageFormat())
                    continue;
                QTextImageFormat format = fragment.charFormat().toImageFormat();
                if (format.width() <= width)
                    continue;
                if (format.height() > 0)
                    format.setHeight(format.height() * width / format.width());
                format.setWidth(width);
                QTextCursor cursor(this);
                cursor.setPosition(fragment.position());
                cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
                cursor.mergeCharFormat(format);
            }
        }
    }

    // Pictures on other machines lose their source before layout: when
    // loadResource() declines one, Qt's image handler opens the source
    // itself (and looks for an "@2x" file next to it on high-DPI screens).
    void dropNetworkImages()
    {
        for (QTextBlock block = begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.charFormat().isImageFormat())
                    continue;
                QTextImageFormat format = fragment.charFormat().toImageFormat();
                if (!namesOtherMachine(QUrl(format.name())))
                    continue;
                format.setName(QStringLiteral("vn-blocked:")); // Neither a file nor a host; shows as broken.
                QTextCursor cursor(this);
                cursor.setPosition(fragment.position());
                cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
                cursor.mergeCharFormat(format);
            }
        }
    }

    // Bytes of the pictures (math, files) the document has resolved so far.
    qsizetype pictureBytes() const { return m_pictureBytes; }

    // Some picture is still downloading.
    bool hasPendingDownload() const { return m_pending; }
    // Every http(s) address asked for so far, allowed or not.
    const QStringList &remoteUrls() const { return m_remote; }
    // Pictures behind backgrounds are only asked for when painting: paint
    // once, into nothing.
    void loadPaintedPictures()
    {
        QPicture picture;
        QPainter painter(&picture);
        documentLayout()->draw(&painter, QAbstractTextDocumentLayout::PaintContext());
    }

    // The file a picture's source names, or empty (math, remote pictures).
    QString pathOf(const QString &source) const
    {
        return source.startsWith(QLatin1String("vn-math")) ? QString() : localPath(QUrl(source));
    }

    // The address of a picture that was downloaded for this block, or empty.
    QUrl downloadedUrlOf(const QString &source) const
    {
        const QUrl url(source);
        const bool loaded = m_context.allowRemoteImages && RemoteImages::isWebUrl(url)
                            && RemoteImages::instance().state(url) == RemoteImages::State::Loaded;
        return loaded ? url : QUrl();
    }

protected:
    QVariant loadResource(int type, const QUrl &url) override
    {
        // Style sheets (<link>, @import) and the rest: the base class would
        // open any path, even on another machine. The app sets its own.
        if (type != QTextDocument::ImageResource)
            return QVariant();
        if (RemoteImages::isWebUrl(url))
            return remoteImage(type, url);
        if (m_listing)
            return standIn(type, url);
        const QString name = url.toString();
        if (name.startsWith(QLatin1String("vn-math"))) {
            const bool display = name.startsWith(QLatin1String("vn-math-display:"));
            const int index = name.section(u':', 1).toInt();
            math::Style style;
            style.pixelSize = QFontInfo(m_context.font).pixelSize() * (display ? 1.2 : 1.1);
            style.color = theme::color(theme::Role::text);
            style.devicePixelRatio = m_context.devicePixelRatio;
            style.display = display;
            return keep(type, url, cachedMath(math::withMacros(m_math.value(index)), style));
        }
        const QString path = localPath(url);
        if (path.isEmpty())
            return QVariant();
        const QImage image = cachedLocalImage(path, m_context.width, m_context.devicePixelRatio);
        if (image.isNull())
            return QVariant();
        return keep(type, url, image);
    }

private:
    // QTextDocument::resource() only remembers what the base loadResource
    // loads; without this every paint would load the picture again.
    QVariant keep(int type, const QUrl &url, const QImage &image)
    {
        m_pictureBytes += image.sizeInBytes();
        addResource(type, url, image);
        return image;
    }

    // Any picture, while listing (declining it would make Qt open the name).
    QVariant standIn(int type, const QUrl &url)
    {
        static const QImage blank = [] {
            QImage image(1, 1, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            return image;
        }();
        addResource(type, url, blank);
        return blank;
    }

    // Without the user's agreement nothing is requested and the picture
    // stays missing; with it, the download starts now and this block is
    // pending until it ends.
    QVariant remoteImage(int type, const QUrl &url)
    {
        if (const QString address = url.toString(); !m_remote.contains(address))
            m_remote.append(address);
        if (m_listing)
            return standIn(type, url);
        if (!m_context.allowRemoteImages)
            return QVariant();
        RemoteImages &remote = RemoteImages::instance();
        switch (remote.request(url)) {
        case RemoteImages::State::Loading:
            m_pending = true;
            return QVariant();
        case RemoteImages::State::Loaded: {
            const QImage image = cachedRemoteImage(url, m_context.width, m_context.devicePixelRatio);
            return image.isNull() ? QVariant() : keep(type, url, image);
        }
        default:
            return QVariant();
        }
    }

    QString localPath(const QUrl &url) const
    {
        if (namesOtherMachine(url))
            return {};
        if (url.scheme() == QLatin1String("file"))
            return url.toLocalFile();
        if (!url.scheme().isEmpty() && url.scheme().size() > 1) // Windows drive letters parse as schemes.
            return {};
        const QString path = QUrl::fromPercentEncoding(url.toString().toUtf8());
        if (QFileInfo(path).isAbsolute())
            return path;
        return m_context.baseDirectory.isEmpty() ? QString() : QDir(m_context.baseDirectory).filePath(path);
    }

    RenderContext m_context;
    bool m_listing = false;
    QStringList m_math;
    qsizetype m_pictureBytes = 0;
    QStringList m_remote;
    bool m_pending = false;
};

QString hex(theme::Role role)
{
    return theme::color(role).name();
}

QString styleSheet(const RenderContext &context)
{
    const int px = QFontInfo(context.font).pixelSize();
    // Every rich block asks for it, nearly always with the same inputs.
    static QString lastKey;
    static QString lastSheet;
    const QString key = QStringLiteral("%1|%2|%3|").arg(px).arg(context.lineHeight).arg(context.codeFont.family())
                        + hex(theme::Role::accent) + hex(theme::Role::text) + hex(theme::Role::codeBlock)
                        + hex(theme::Role::divider) + hex(theme::Role::sideBar) + hex(theme::Role::mutedText);
    if (key == lastKey)
        return lastSheet;
    lastKey = key;
    lastSheet = QStringLiteral(R"(
h1 { font-size: %1px; font-weight: 600; margin: 0; }
h2 { font-size: %2px; font-weight: 600; margin: 0; }
h3 { font-size: %3px; font-weight: 600; margin: 0; }
h4, h5, h6 { font-size: %4px; font-weight: 600; margin: 0; }
p, li, blockquote, td, th { line-height: %13%; }
p { margin: 0; }
a { color: %5; text-decoration: underline; }
a.task { color: %6; text-decoration: none; }
code { font-family: "%7"; background-color: %8; }
pre { font-family: "%7"; background-color: %8; margin: 4px 0; }
blockquote { color: #555555; margin: 0 0 0 14px; }
ul, ol { margin: 0; }
li { margin: 1px 0; }
table { border-color: %9; border-style: solid; border-collapse: collapse; margin: 2px 0; }
th { background-color: %10; font-weight: 600; }
td, th { border: 1px solid %9; }
table.frontmatter th { text-align: left; color: %11; font-weight: normal; }
div.footnote { font-size: %12px; color: #444444; }
)")
        .arg(qRound(px * 1.55))
        .arg(qRound(px * 1.2))
        .arg(qRound(px * 1.07))
        .arg(px)
        .arg(hex(theme::Role::accent), hex(theme::Role::text), context.codeFont.family(), hex(theme::Role::codeBlock), hex(theme::Role::divider))
        .arg(hex(theme::Role::sideBar), hex(theme::Role::mutedText))
        .arg(qRound(px * 0.9))
        .arg(qRound(context.lineHeight * 100));
    return lastSheet;
}

// The Qt format of a syntax format. The stock SyntaxHighlighter works these
// out for every format of its definition and of each definition that one
// includes, in every highlighter: for Markdown (which includes the fenced
// languages) thousands of them, 0.2 ms and 280 KiB for each code block. The
// theme is the same for all blocks and a block uses a few formats, so they
// are made on demand and shared. Built as KSyntaxHighlighting does.
QTextCharFormat sharedTextFormat(const KSyntaxHighlighting::Format &format, const KSyntaxHighlighting::Theme &theme)
{
    static QHash<int, QTextCharFormat> formats; // By format id, which is unique in the repository.
    static QString formatsTheme;
    if (formatsTheme != theme.name()) {
        formats.clear();
        formatsTheme = theme.name();
    }
    if (const auto it = formats.constFind(format.id()); it != formats.constEnd())
        return *it;
    QTextCharFormat tf;
    // Always set the foreground color to avoid palette issues.
    tf.setForeground(format.textColor(theme));
    if (format.hasBackgroundColor(theme))
        tf.setBackground(format.backgroundColor(theme));
    if (format.isBold(theme))
        tf.setFontWeight(QFont::Bold);
    if (format.isItalic(theme))
        tf.setFontItalic(true);
    if (format.isUnderline(theme))
        tf.setFontUnderline(true);
    if (format.isStrikeThrough(theme))
        tf.setFontStrikeOut(true);
    formats.insert(format.id(), tf);
    return tf;
}

class CodeHighlighter : public KSyntaxHighlighting::SyntaxHighlighter
{
public:
    using KSyntaxHighlighting::SyntaxHighlighter::SyntaxHighlighter;

protected:
    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format) override
    {
        if (length > 0)
            setFormat(offset, length, sharedTextFormat(format, theme()));
    }
};

// Layout limits. Qt lays text out in time that is far from linear in some
// shapes (measured at a 600 px column, Release build):
//  - an unbreakable run of characters: each line rescans the rest of the run,
//    so a run of R characters takes about 2e-8 ms * R^2 (100 KB: 0.2 s,
//    260 KB: 1 s), and in a column squeezed to nothing by nested quotes (a
//    line holds one letter) about 2e-6 ms * R^2 (20 KB: 1 s, 260 KB: 150 s);
//  - the items of one list: 2000 take 0.12 s, 5000 0.85 s, 20000 14 s;
//  - table cells: about 50 us each (4000 take 0.2 to 0.4 s);
//  - nested containers: 100 deep take 5 ms, 1000 deep 0.25 s, 5000 deep 9 s.
// A block past a limit is shown as its source in a code box, which has none
// of these costs (plain text, linear; a run is wrapped anywhere). Ordinary
// documents are far below every limit: the longest word, list, table and
// nesting of a normal page are 100 times smaller.
constexpr double richRunSquares = 1e8;   // Sum over unbreakable runs of length^2 (a 10 000 character word).
constexpr double listSquares = 7e6;      // Sum over lists of items^2 (2500 items, 0.2 s).
constexpr int listItemsMax = 20000;
constexpr int tableCellsMax = 4000;
constexpr int nestingMax = 100;
constexpr double codeRunSquares = 2.5e9; // A code box wraps a longer run anywhere, which is linear.
constexpr qsizetype highlightMax = 400 * 1000; // Characters; highlighting costs twice the layout.

std::atomic<bool> layoutGuard{true};
std::atomic<int> guardedBlocks{0};

bool isOneOf(QLatin1StringView name, std::initializer_list<QLatin1StringView> names)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool isInlineTag(QLatin1StringView name)
{
    return isOneOf(name, {"a"_L1, "b"_L1, "i"_L1, "u"_L1, "s"_L1, "em"_L1, "strong"_L1, "code"_L1, "span"_L1,
                          "del"_L1, "ins"_L1, "sup"_L1, "sub"_L1, "mark"_L1, "small"_L1, "kbd"_L1, "abbr"_L1,
                          "font"_L1, "big"_L1, "tt"_L1});
}

bool isContainerTag(QLatin1StringView name)
{
    return isOneOf(name, {"blockquote"_L1, "ul"_L1, "ol"_L1, "table"_L1, "div"_L1, "details"_L1, "section"_L1,
                          "article"_L1, "dl"_L1, "figure"_L1, "center"_L1});
}

// True if the HTML would take Qt too long to lay out (see above). One pass
// over the bytes, stopping at the first limit passed.
bool tooHeavyForRichText(const char *html, qsizetype size)
{
    double runSquares = 0, listSq = 0;
    qsizetype run = 0;
    int depth = 0, cells = 0, items = 0;
    std::vector<int> lists; // Items so far of the open lists.
    const auto endRun = [&] {
        runSquares += double(run) * double(run);
        run = 0;
    };
    const auto endList = [&] {
        listSq += double(lists.back()) * double(lists.back());
        lists.pop_back();
    };
    for (qsizetype i = 0; i < size; ++i) {
        const unsigned char c = html[i];
        if (c == '<') {
            if (size - i > 3 && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
                const qsizetype end = QByteArrayView(html, size).indexOf("-->", i + 4);
                if (end < 0)
                    break;
                i = end + 2;
                continue;
            }
            qsizetype j = i + 1;
            const bool closing = j < size && html[j] == '/';
            if (closing)
                ++j;
            char name[12] = {};
            int length = 0;
            while (j < size && ((html[j] >= 'a' && html[j] <= 'z') || (html[j] >= 'A' && html[j] <= 'Z')
                                || (html[j] >= '0' && html[j] <= '9'))) {
                if (length < 11)
                    name[length] = char(html[j] | 0x20);
                ++length;
                ++j;
            }
            if (length == 0) { // A '<' that is text.
                ++run;
                continue;
            }
            if (length > 11) // No tag that matters has a name this long.
                length = 0;
            // The rest of the tag; quotes may hold a '>'.
            char quote = 0;
            for (; j < size; ++j) {
                if (quote) {
                    if (html[j] == quote)
                        quote = 0;
                } else if (html[j] == '"' || html[j] == '\'') {
                    quote = html[j];
                } else if (html[j] == '>') {
                    break;
                }
            }
            const bool selfClosing = j > 0 && j < size && html[j - 1] == '/';
            const QLatin1StringView tag(name, length);
            // (An empty name matches nothing below.)
            i = j;
            if (!isInlineTag(tag))
                endRun();
            if (closing) {
                if (isContainerTag(tag) && depth > 0)
                    --depth;
                if ((tag == "ul"_L1 || tag == "ol"_L1) && !lists.empty())
                    endList();
            } else if (!selfClosing) {
                if (isContainerTag(tag) && ++depth > nestingMax)
                    return true;
                if (tag == "ul"_L1 || tag == "ol"_L1) {
                    lists.push_back(0);
                } else if (tag == "li"_L1) {
                    if (!lists.empty())
                        ++lists.back();
                    else
                        listSq += 1;
                    if (++items > listItemsMax)
                        return true;
                } else if (tag == "td"_L1 || tag == "th"_L1) {
                    if (++cells > tableCellsMax)
                        return true;
                }
            }
            if (runSquares > richRunSquares || listSq > listSquares)
                return true;
            continue;
        }
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '-' || c == '/' || c >= 0xE0) {
            endRun(); // Places Qt can break a line (CJK and the like are lenient).
            if (runSquares > richRunSquares)
                return true;
        } else if ((c & 0xC0) != 0x80) {
            ++run; // A character (not its continuation bytes).
        }
    }
    endRun();
    while (!lists.empty())
        endList();
    return runSquares > richRunSquares || listSq > listSquares;
}

// Sum over the unbreakable runs of the text (a code line is a paragraph) of
// their length squared.
double runSquaresOf(const QString &text)
{
    double sum = 0;
    qsizetype run = 0;
    for (const QChar ch : text) {
        const char16_t c = ch.unicode();
        if (c <= u' ' || c == u'-' || c == u'/' || c >= 0x0E00) {
            sum += double(run) * double(run);
            run = 0;
        } else {
            ++run;
        }
    }
    return sum + double(run) * double(run);
}

std::unique_ptr<QTextDocument> codeDocument(const QString &source, const QString &definition,
                                            const RenderContext &context, qreal width)
{
    auto doc = std::make_unique<QTextDocument>();
    doc->setDocumentMargin(0);
    doc->setDefaultFont(context.codeFont);
    const bool guard = layoutGuard;
    QTextOption option;
    // Long runs of text with no place to break: wrapping at a boundary first
    // and then anywhere rescans the run for every line (see the limits above).
    // Wrapping anywhere puts the lines in the same places for such a run.
    option.setWrapMode(guard && runSquaresOf(source) > codeRunSquares ? QTextOption::WrapAnywhere
                                                                       : QTextOption::WrapAtWordBoundaryOrAnywhere);
    doc->setDefaultTextOption(option);
    doc->setPlainText(source);
    doc->setTextWidth(width);
    // A huge block is not highlighted: that costs twice the layout.
    if (guard && source.size() > highlightMax)
        return doc;
    auto *highlighter = new CodeHighlighter(doc.get());
    highlighter->setTheme(theme::syntaxTheme());
    // Highlights the whole text by itself when the definition changes.
    highlighter->setDefinition(theme::syntaxRepository().definitionForName(definition));
    return doc;
}

} // namespace

QImage decodePicture(const QByteArray &data)
{
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    if (!reader.canRead())
        return {};
    if (reader.format().startsWith("svg")) {
        QBuffer scanned;
        scanned.setData(data);
        scanned.open(QIODevice::ReadOnly);
        if (svgNamesNetworkPath(scanned))
            return {};
    }
    return reader.read();
}

int RenderedBlock::pictureFileReads()
{
    return localImageReads;
}

int DiagramNotifier::renderCount()
{
    return diagramRenders;
}

void DiagramNotifier::setCacheLimitKiB(int kib)
{
    diagramCache().setMaxCost(kib);
}

DiagramNotifier &DiagramNotifier::instance()
{
    // Never deleted: worker threads may post to it while the app quits.
    static auto *notifier = new DiagramNotifier;
    return *notifier;
}

// The block as its source in a code box: the fences stay visible (muted) and
// the Markdown definition highlights the fenced language, as in the source
// view. Also what blocks too heavy for rich text become.
std::unique_ptr<RenderedBlock> RenderedBlock::codeBox(std::unique_ptr<RenderedBlock> result, const QString &source,
                                                      const RenderContext &context)
{
    RenderedBlock &r = *result;
    r.m_kind = Kind::Code;
    r.m_doc = codeDocument(source, QStringLiteral("Markdown"), context, context.width - 2 * codePadX);
    if (!r.m_error.isEmpty()) {
        QTextCursor end(r.m_doc.get());
        end.movePosition(QTextCursor::End);
        QTextCharFormat errorFormat;
        errorFormat.setForeground(QColor(0xA3, 0x3A, 0x1C));
        end.insertText(QStringLiteral("\n") + QObject::tr("Diagram error: %1").arg(r.m_error), errorFormat);
    }
    r.m_height = r.m_doc->size().height() + 2 * codePadY + blockGap;
    r.collectLineAnchors(int(source.count(u'\n')) + 1);
    return result;
}

void RenderedBlock::setDecodedPictureLimitKiB(int kib)
{
    decodedPictures().setLimitKiB(kib);
}

int RenderedBlock::guardedBlockCount()
{
    return guardedBlocks;
}

void RenderedBlock::setLayoutGuardEnabled(bool on)
{
    layoutGuard = on;
}

std::unique_ptr<RenderedBlock> RenderedBlock::create(const core::Block &block, const QString &source,
                                                     const RenderContext &context)
{
    auto result = std::unique_ptr<RenderedBlock>(new RenderedBlock);
    RenderedBlock &r = *result;
    r.m_width = context.width;
    const QString lang = QString::fromUtf8(block.lang.data(), block.lang.size()).trimmed();
    const QString text = QString::fromUtf8(block.text.data(), block.text.size());

    switch (block.kind) {
    case core::BlockKind::Hidden:
        r.m_kind = Kind::Empty;
        r.m_height = 0;
        return result;
    case core::BlockKind::Math: {
        math::Style style;
        style.pixelSize = QFontInfo(context.font).pixelSize() * 1.2;
        style.color = theme::color(theme::Role::text);
        style.devicePixelRatio = context.devicePixelRatio;
        style.display = true;
        r.m_kind = Kind::Image;
        r.m_image = cachedMath(math::withMacros(text), style);
        r.m_centered = true;
        r.m_height = r.m_image.height() / r.m_image.devicePixelRatio() + 2 * codePadY + blockGap;
        return result;
    }
    case core::BlockKind::Code:
        if (lang.compare(QLatin1String("mermaid"), Qt::CaseInsensitive) == 0) {
            Diagram diagram;
            r.m_pending = !mermaidDiagram(text, context, &diagram);
            r.m_image = diagram.image;
            r.m_error = diagram.error;
            if (!r.m_image.isNull()) {
                r.m_kind = Kind::Image;
                r.m_centered = true;
                r.m_diagramCode = text;
                r.m_diagramTheme = context.mermaidTheme;
                const qreal w = r.m_image.width() / r.m_image.devicePixelRatio();
                if (w > context.width) // Shrink wide diagrams to fit.
                    r.m_image.setDevicePixelRatio(r.m_image.devicePixelRatio() * w / context.width);
                r.m_height = r.m_image.height() / r.m_image.devicePixelRatio() + 2 * codePadY + blockGap;
                return result;
            }
        }
        [[fallthrough]];
    case core::BlockKind::FrontMatter:
        return codeBox(std::move(result), source, context);
    case core::BlockKind::Html:
    default: {
        // Too much for Qt's rich text layout: show the source instead.
        if (layoutGuard && tooHeavyForRichText(block.html.data(), block.html.size())) {
            ++guardedBlocks;
            return codeBox(std::move(result), source, context);
        }
        r.m_kind = Kind::Rich;
        auto doc = std::make_unique<ResourceDocument>(block, context);
        doc->setDocumentMargin(0);
        doc->setDefaultFont(context.font);
        doc->setDefaultStyleSheet(styleSheet(context));
        const QString html = QString::fromUtf8(block.html.data(), block.html.size());
        doc->setHtml(html);
        doc->dropNetworkImages();
        doc->fitImages(context.width);
        doc->setTextWidth(context.width);
        r.m_height = doc->size().height() + blockGap;
        // Backgrounds are asked for when painted: start their downloads now.
        if (context.allowRemoteImages && mayLoadPictures(html))
            doc->loadPaintedPictures();
        r.m_pending = doc->hasPendingDownload();
        r.m_pictureBytes = doc->pictureBytes(); // Resolved by the layout (and the dry paint).
        r.m_doc = std::move(doc);
        r.collectLineAnchors(0);
        return result;
    }
    }
}

QStringList RenderedBlock::remoteImages(const core::Block &block, const RenderContext &context)
{
    if (block.kind != core::BlockKind::Html)
        return {};
    const QString html = QString::fromUtf8(block.html.data(), block.html.size());
    if (!mayLoadPictures(html))
        return {};
    // Shown as its source (see create()): it asks for no pictures, and laying
    // it out would take as long as create() avoids.
    if (layoutGuard && tooHeavyForRichText(block.html.data(), block.html.size()))
        return {};
    RenderContext asking = context;
    asking.allowRemoteImages = false;
    ResourceDocument doc(block, asking, true);
    doc.setDocumentMargin(0);
    doc.setDefaultFont(context.font);
    doc.setHtml(html);
    // As in create(): Qt's image handler looks for "@2x" files by name first.
    doc.dropNetworkImages();
    doc.setTextWidth(context.width);
    doc.loadPaintedPictures();
    return doc.remoteUrls();
}

void RenderedBlock::collectLineAnchors(int sourceLines)
{
    QAbstractTextDocumentLayout *layout = m_doc->documentLayout();
    const qreal top = contentOffset().y();
    if (m_kind == Kind::Code) {
        // One paragraph per source line (wrapped lines are interpolated);
        // a diagram error appended after the source is not a source line.
        QTextBlock block = m_doc->begin().next();
        for (int line = 1; line < sourceLines && block.isValid(); ++line, block = block.next())
            m_lineAnchors.push_back({line, top + layout->blockBoundingRect(block).top()});
        return;
    }
    // Empty named anchors attach their names to the next character; its
    // line's top is where the source line starts. blockBoundingRect is in
    // document coordinates, table cells included.
    static const QLatin1String prefix("vn-line:");
    for (QTextBlock block = m_doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QStringList names = fragment.charFormat().anchorNames();
            for (const QString &name : names) {
                if (!name.startsWith(prefix))
                    continue;
                const int line = QStringView(name).mid(prefix.size()).toInt();
                const QTextLine textLine = block.layout()->lineForTextPosition(fragment.position() - block.position());
                const qreal y = layout->blockBoundingRect(block).top() + (textLine.isValid() ? textLine.y() : 0);
                if (line > 0)
                    m_lineAnchors.push_back({line, top + y});
            }
        }
    }
    std::stable_sort(m_lineAnchors.begin(), m_lineAnchors.end(),
                     [](const LineAnchor &a, const LineAnchor &b) { return a.first < b.first; });
}

QPointF RenderedBlock::contentOffset() const
{
    return m_kind == Kind::Code ? QPointF(codePadX, codePadY) : QPointF(0, 0);
}

void RenderedBlock::paint(QPainter &painter, const QPointF &topLeft, qreal width, const RenderedSelection &selection) const
{
    switch (m_kind) {
    case Kind::Empty:
        return;
    case Kind::Image: {
        const QSizeF size = m_image.size() / m_image.devicePixelRatio();
        const qreal x = m_centered ? topLeft.x() + qMax<qreal>(0, (width - size.width()) / 2) : topLeft.x();
        const QRectF imageRect(QPointF(x, topLeft.y() + codePadY), size);
        painter.drawImage(imageRect, m_image);
        if (selection.whole) {
            // A light tint and an outline keep the picture readable on
            // themes whose selection colour is dark.
            QColor tint = theme::color(theme::Role::selection);
            const QColor outline = tint;
            tint.setAlpha(56);
            painter.fillRect(imageRect, tint);
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, false);
            painter.setPen(QPen(outline, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(imageRect.toAlignedRect().adjusted(0, 0, -1, -1));
            painter.restore();
        }
        return;
    }
    case Kind::Code: {
        QPainterPath box;
        box.addRoundedRect(QRectF(topLeft, QSizeF(width, m_height - blockGap)), 3, 3);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(box, theme::color(theme::Role::codeBlock));
        painter.restore();
        [[fallthrough]];
    }
    case Kind::Rich: {
        painter.save();
        painter.translate(topLeft + contentOffset());
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, theme::color(theme::Role::text));
        const QColor selectionText = theme::color(theme::Role::selectionText);
        for (const auto &range : selection.ranges) {
            QAbstractTextDocumentLayout::Selection sel;
            sel.cursor = QTextCursor(m_doc.get());
            sel.cursor.setPosition(qBound(0, range.first, m_doc->characterCount() - 1));
            sel.cursor.setPosition(qBound(0, range.second, m_doc->characterCount() - 1), QTextCursor::KeepAnchor);
            sel.format.setBackground(theme::color(theme::Role::selection));
            // Like the source rows: only a distinct selection colour replaces
            // the text (and syntax) colours.
            if (selectionText != theme::color(theme::Role::text))
                sel.format.setForeground(selectionText);
            ctx.selections.append(sel);
        }
        m_doc->documentLayout()->draw(&painter, ctx);
        painter.restore();
        return;
    }
    }
}

int RenderedBlock::hitTest(const QPointF &point) const
{
    if (!m_doc)
        return -1;
    return m_doc->documentLayout()->hitTest(point - contentOffset(), Qt::FuzzyHit);
}

QString RenderedBlock::anchorAt(const QPointF &point) const
{
    if (m_kind != Kind::Rich)
        return {};
    return m_doc->documentLayout()->anchorAt(point - contentOffset());
}

std::optional<RenderedBlock::Picture> RenderedBlock::pictureAt(const QPointF &point) const
{
    if (m_kind == Kind::Image) {
        const QSizeF size = m_image.size() / m_image.devicePixelRatio();
        // paint() centres these in the column.
        const qreal x = m_centered ? qMax<qreal>(0, (m_width - size.width()) / 2) : 0;
        if (!QRectF(QPointF(x, codePadY), size).contains(point))
            return std::nullopt;
        return Picture{m_diagramCode.isNull() ? QObject::tr("Equation") : QObject::tr("Diagram"), {}};
    }
    if (m_kind != Kind::Rich)
        return std::nullopt;
    const QString source = m_doc->documentLayout()->imageAt(point);
    if (source.isEmpty())
        return std::nullopt;
    const auto *doc = static_cast<const ResourceDocument *>(m_doc.get());
    if (const QString path = doc->pathOf(source); !path.isEmpty())
        return Picture{QFileInfo(path).fileName(), path, {}};
    const QUrl url = doc->downloadedUrlOf(source);
    if (url.isEmpty())
        return std::nullopt;
    const QString name = QFileInfo(url.path()).fileName();
    return Picture{name.isEmpty() ? url.host() : name, {}, url.toString(QUrl::FullyEncoded)};
}

QImage RenderedBlock::pictureImage(const Picture &picture) const
{
    if (!picture.path.isEmpty())
        return fullSizeImage(picture.path);
    if (!picture.url.isEmpty())
        return RemoteImages::instance().fullImage(QUrl(picture.url)); // Not downloaded again.
    if (!m_diagramCode.isNull()) {
        // The diagram is vector: render it larger rather than magnify pixels.
        try {
            const QByteArray code = m_diagramCode.toUtf8();
            const QByteArray theme = m_diagramTheme.toUtf8();
            const core::RgbaImage raw = core::mermaid_image(rust::Str(code.constData(), code.size()),
                                                            rust::Str(theme.constData(), theme.size()), 3.0f);
            return QImage(raw.pixels.data(), int(raw.width), int(raw.height), int(raw.width) * 4,
                          QImage::Format_RGBA8888_Premultiplied).copy();
        } catch (const std::exception &) { // rust::Error
        }
    }
    QImage image = m_image;
    image.setDevicePixelRatio(1);
    return image;
}

QString RenderedBlock::renderedText() const
{
    if (m_renderedText.isNull() && m_doc)
        m_renderedText = m_doc->toPlainText();
    return m_renderedText;
}

int RenderedBlock::cost() const
{
    // Laid-out text measured on the heap: rich text takes some 40 KiB of
    // document structures plus about 64 bytes a character, a highlighted
    // code block 20 KiB plus about 100 bytes a character. Pictures in the
    // text are whole images (often shared with the picture caches, which
    // still keeps them alive when this block is dropped, so it is a bit
    // generous).
    int kib = 1;
    if (!m_image.isNull())
        kib += int(m_image.sizeInBytes() / 1024);
    kib += int(m_pictureBytes / 1024);
    if (m_doc) {
        const int characters = m_doc->characterCount();
        kib += m_kind == Kind::Code ? 20 + characters / 10 : 40 + characters / 16;
    }
    return kib;
}

} // namespace vn
