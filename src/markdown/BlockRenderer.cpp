#include "BlockRenderer.h"

#include "../Theme.h"
#include "MarkdownEditing.h"
#include "MathObject.h"
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
#include <QFontMetrics>
#include <QHash>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QPicture>
#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextList>
#include <QTextTable>
#include <QThreadPool>
#include <QUrl>
#include <QXmlStreamReader>
#include <QtMath>

#include <algorithm>
#include <functional>
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

// Formulas laid out, by TeX and style (inline or display); any size is drawn
// from one. Blocks are re-created after edits, zooming and option changes,
// and MicroTeX is slow enough to notice; GUI thread only.
std::shared_ptr<const math::Formula> cachedFormula(const QString &tex, bool display)
{
    using Kept = std::shared_ptr<const math::Formula>;
    static QCache<QString, Kept> cache(32 * 1024); // KiB
    const QString key = (display ? QStringLiteral("d|") : QStringLiteral("i|")) + tex;
    if (const Kept *hit = cache.object(key))
        return *hit;
    Kept formula = math::layout(tex, display);
    const qsizetype kib = 1 + formula->elements() * qsizetype(sizeof(QPainterPath::Element)) / 1024;
    cache.insert(key, new Kept(formula), kib);
    return formula;
}

// How much larger than the text around them formulas are drawn.
constexpr qreal inlineMathScale = 1.1;
constexpr qreal displayMathScale = 1.2;

// Largest SVG, once unpacked, that is scanned and opened. Pictures that are
// shown in a column are far smaller; a gzip bomb is not.
constexpr qint64 maxSvgBytes = 32 * 1024 * 1024;

// QtSvg draws an element again for every <use> of it, every <use> of a group
// holding that <use>, and so on: ten <use>s of a group of ten <use>s of ...,
// nine deep, is a 2 KB file that takes minutes on the GUI thread. So an SVG
// is refused when, with every <use>, pattern, mask, clip path, filter, marker
// (once per vertex) and other #id reference counted as the elements it
// repeats, it would draw more than this many elements...
constexpr quint64 maxSvgDrawn = 200000;
// ... or has more elements than this, or elements nested deeper than this.
constexpr int maxSvgElements = 500000;
constexpr int maxSvgDepth = 64;
// SVG pictures in an SVG (data: addresses), and in those, that are scanned;
// one nested deeper is refused.
constexpr int maxSvgNesting = 2;

// The bytes of a data: address, decoded the way QtSvg decodes an <image>'s
// (qDecodeDataUrl), or none.
QByteArray svgDataUrlBytes(const QString &href)
{
    const QUrl uri(href);
    if (uri.scheme() != u"data" || !uri.host().isEmpty())
        return {};
    const QByteArray encoded = uri.toEncoded(QUrl::RemoveScheme);
    const qsizetype comma = encoded.indexOf(',');
    if (comma < 0)
        return {};
    const QByteArray header = QByteArray::fromPercentEncoding(encoded.first(comma)).trimmed().toLower();
    QByteArray payload = QByteArray::fromPercentEncoding(encoded.sliced(comma + 1));
    if (!header.endsWith(";base64"))
        return payload;
    auto decoded = QByteArray::fromBase64Encoding(std::move(payload));
    return decoded ? decoded.decoded : QByteArray();
}

// The ids that url(#id) references in `text` name.
void addSvgUrlReferences(QStringView text, QStringList &ids)
{
    static const QRegularExpression url(QStringLiteral(R"(url\(\s*['"]?\s*#?([^)'"\s]+))"),
                                        QRegularExpression::CaseInsensitiveOption);
    if (!text.contains(u"url(", Qt::CaseInsensitive))
        return;
    for (auto it = url.globalMatchView(text); it.hasNext();)
        ids.append(it.next().captured(1));
}

// The number of numbers in path data or a point list: more than the vertices
// a marker is drawn at, never fewer.
quint64 svgVertexBound(QStringView data)
{
    quint64 count = 0;
    bool inNumber = false;
    for (const QChar c : data) {
        const bool digit = c.isDigit();
        if (digit && !inNumber)
            ++count;
        inNumber = digit;
    }
    return qMax<quint64>(1, count);
}

struct SvgElement
{
    QString tag; // Without a prefix.
    QString id;
    QString classes;
    QStringList refs;       // Ids it draws once each.
    QStringList markerRefs; // Ids it draws at each vertex (markers, maybe).
    QList<int> children;
    quint64 own = 1;      // Itself, and the SVG picture it shows, if any.
    quint64 vertices = 1; // At most.
};

// url(#id) references in a style sheet, by what they apply to. A selector is
// read as applying to every element with any name, class or id in it (more
// rather than fewer); one that is not that simple applies to every element.
struct SvgStyleReferences
{
    QHash<QString, QStringList> byTag, byClass, byId, markersByTag, markersByClass, markersById;
    QStringList all, allMarkers;
    bool tooMany = false; // More references than any picture needs: unsafe.
};

SvgStyleReferences svgStyleReferences(QString css)
{
    static const QRegularExpression comment(QStringLiteral(R"(/\*.*?\*/)"),
                                            QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression rule(QStringLiteral(R"(([^{}]*)\{([^{}]*)\})"));
    static const QRegularExpression token(QStringLiteral(R"(([.#]?)([^\s.#,>+~(){}]+))"));
    SvgStyleReferences references;
    qsizetype stored = 0;
    css.remove(comment);
    for (auto it = rule.globalMatch(css); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        QStringList ids;
        addSvgUrlReferences(match.capturedView(2), ids);
        if (ids.isEmpty())
            continue;
        const bool markers = match.capturedView(2).contains(u"marker", Qt::CaseInsensitive);
        const QStringView selector = match.capturedView(1);
        if (selector.contains(u'*') || selector.contains(u'[') || selector.contains(u':') || selector.contains(u'\\')
            || selector.trimmed().isEmpty()) {
            (markers ? references.allMarkers : references.all) += ids;
            stored += ids.size();
            if (stored > qsizetype(maxSvgElements)) {
                references.tooMany = true;
                return references;
            }
            continue;
        }
        for (auto t = token.globalMatchView(selector); t.hasNext();) {
            stored += ids.size();
            if (stored > qsizetype(maxSvgElements)) {
                references.tooMany = true;
                return references;
            }
            const QRegularExpressionMatch part = t.next();
            const QStringView kind = part.capturedView(1);
            const QString name = part.captured(2);
            auto &map = kind == u"." ? (markers ? references.markersByClass : references.byClass)
                        : kind == u"#" ? (markers ? references.markersById : references.byId)
                                       : (markers ? references.markersByTag : references.byTag);
            map[name] += ids;
        }
    }
    return references;
}

bool svgIsUnsafe(QIODevice &file, int nesting, quint64 &drawn);

// True unless the SVG elements `elements`, their ids `byId` and the style
// sheet `css` draw at most maxSvgDrawn elements; `drawn` is how many.
// References in a cycle are unsafe too (QtSvg discards such a file).
bool svgDrawsTooMuch(std::vector<SvgElement> &elements, const QHash<QString, QList<int>> &byId, const QString &css,
                     quint64 &drawn)
{
    // The elements that an id names, but not gradients: they draw nothing
    // of their own, and their stops may name them back in a style sheet.
    const auto targets = [&](const QString &id) {
        QList<int> found = byId.value(id);
        found.removeIf([&](int i) { return elements[size_t(i)].tag.endsWith(u"Gradient"); });
        return found;
    };
    struct Link
    {
        int target;
        bool perVertex;
    };
    std::vector<QList<Link>> links(elements.size());
    quint64 linkCount = 0; // Each is at least one more drawn element.
    const auto link = [&](size_t from, const QStringList &ids, bool perVertex) {
        for (const QString &id : ids) {
            for (const int target : targets(id)) {
                links[from].append({target, perVertex});
                if (++linkCount > maxSvgDrawn)
                    return false;
            }
        }
        return true;
    };
    SvgStyleReferences style;
    if (css.contains(u"url(", Qt::CaseInsensitive))
        style = svgStyleReferences(css);
    if (style.tooMany)
        return true;
    // Only ids that name something (each is looked up for every element).
    const auto prune = [&](QStringList &ids) { ids.removeIf([&](const QString &id) { return targets(id).isEmpty(); }); };
    prune(style.all);
    prune(style.allMarkers);
    for (auto *map : {&style.byTag, &style.byClass, &style.byId, &style.markersByTag, &style.markersByClass,
                      &style.markersById}) {
        for (QStringList &ids : *map)
            prune(ids);
    }
    for (size_t i = 0; i < elements.size(); ++i) {
        const SvgElement &element = elements[i];
        for (const int child : element.children)
            links[i].append({child, false});
        bool ok = link(i, element.refs, false) && link(i, element.markerRefs, true) && link(i, style.all, false)
                  && link(i, style.allMarkers, true) && link(i, style.byTag.value(element.tag), false)
                  && link(i, style.markersByTag.value(element.tag), true);
        if (ok && !element.id.isEmpty())
            ok = link(i, style.byId.value(element.id), false) && link(i, style.markersById.value(element.id), true);
        if (ok && !element.classes.isEmpty() && !(style.byClass.isEmpty() && style.markersByClass.isEmpty())) {
            for (const QString &name : element.classes.simplified().split(u' ', Qt::SkipEmptyParts))
                ok = ok && link(i, style.byClass.value(name), false) && link(i, style.markersByClass.value(name), true);
        }
        if (!ok)
            return true;
    }

    // Elements drawn by each element, children and references included,
    // depth first without recursion (reference chains can be long). Every
    // count is checked against the limit as it grows, so none overflows.
    enum : char { New, Open, Done };
    std::vector<char> state(elements.size(), New);
    std::vector<quint64> size(elements.size(), 0);
    std::vector<std::pair<int, qsizetype>> stack;
    for (size_t start = 0; start < elements.size(); ++start) {
        if (state[start] != New)
            continue;
        state[start] = Open;
        stack.push_back({int(start), 0});
        while (!stack.empty()) {
            const int node = stack.back().first;
            const qsizetype next = stack.back().second;
            const QList<Link> &out = links[size_t(node)];
            if (next < out.size()) {
                ++stack.back().second;
                const int target = out[next].target;
                if (state[size_t(target)] == Open)
                    return true; // A cycle.
                if (state[size_t(target)] == New) {
                    state[size_t(target)] = Open;
                    stack.push_back({target, 0});
                }
                continue;
            }
            const SvgElement &element = elements[size_t(node)];
            quint64 total = element.own;
            for (const Link &l : out) {
                total += size[size_t(l.target)] * (l.perVertex ? element.vertices : 1);
                if (total > maxSvgDrawn)
                    return true;
            }
            size[size_t(node)] = total;
            state[size_t(node)] = Done;
            stack.pop_back();
        }
    }
    drawn = elements.empty() ? 0 : size[0]; // The root holds every other element.
    return false;
}

// True if the SVG in `file` (open for reading, a file or a download) names
// a file on another machine, which the SVG plugin would open by itself:
// <image href> pictures and xml-stylesheet style sheets; or would take too
// long to draw (see maxSvgDrawn) or is nested too deep. SVG pictures inside
// it as data: addresses are scanned too (QtSvg shows those, but no SVG
// files). Read the way QtSvg reads it: a file that starts with the gzip magic
// is an .svgz, unpacked (every member) before it is parsed. One that cannot
// be unpacked, or is too big, is unsafe: it is not opened. `drawn` is set to
// the elements it draws. Any thread.
bool svgIsUnsafe(QIODevice &file, int nesting, quint64 &drawn)
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
    std::vector<SvgElement> elements;
    QHash<QString, QList<int>> byId;
    QList<int> open;
    QString css;
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::ProcessingInstruction) {
            const QRegularExpressionMatch match = href.matchView(xml.processingInstructionData());
            if (match.hasMatch() && md::isNetworkPath(match.captured(1)))
                return true;
        } else if (token == QXmlStreamReader::StartElement) {
            if (elements.size() >= size_t(maxSvgElements) || open.size() >= maxSvgDepth)
                return true;
            const int index = int(elements.size());
            if (!open.isEmpty())
                elements[size_t(open.last())].children.append(index);
            open.append(index);
            SvgElement element;
            const QStringView qualified = xml.qualifiedName();
            element.tag = qualified.mid(qualified.lastIndexOf(u':') + 1).toString();
            for (const QXmlStreamAttribute &attribute : xml.attributes()) {
                const QStringView name = attribute.qualifiedName();
                const QStringView value = attribute.value();
                if (name == u"href" || name.endsWith(u":href")) {
                    if (md::isNetworkPath(value.toString()))
                        return true;
                    // QtSvg takes the id after the first character: "#id".
                    const QString target = value.trimmed().toString();
                    if (!target.isEmpty())
                        element.refs.append(target.mid(1));
                    if (element.tag == u"image" && target.startsWith(u"data", Qt::CaseInsensitive)) {
                        QBuffer picture;
                        picture.setData(svgDataUrlBytes(target));
                        picture.open(QIODevice::ReadOnly);
                        if (QImageReader(&picture).format().startsWith("svg")) {
                            quint64 inner = 0;
                            picture.seek(0);
                            if (nesting >= maxSvgNesting || svgIsUnsafe(picture, nesting + 1, inner))
                                return true;
                            element.own += inner;
                        }
                    }
                } else if (name == u"d" || name == u"points") {
                    element.vertices = qMax(element.vertices, svgVertexBound(value));
                } else if (name == u"id" || name == u"xml:id") {
                    element.id = value.toString();
                    byId[element.id].append(index);
                } else if (name == u"class") {
                    element.classes = value.toString();
                }
                const bool markers = name.contains(u"marker", Qt::CaseInsensitive)
                                     || (name == u"style" && value.contains(u"marker", Qt::CaseInsensitive));
                addSvgUrlReferences(value, markers ? element.markerRefs : element.refs);
            }
            elements.push_back(std::move(element));
        } else if (token == QXmlStreamReader::EndElement) {
            if (!open.isEmpty())
                open.removeLast();
        } else if (token == QXmlStreamReader::Characters && !open.isEmpty()
                   && elements[size_t(open.last())].tag == u"style") {
            css += xml.text();
            css += u'\n';
        }
    }
    return svgDrawsTooMuch(elements, byId, css, drawn);
}

bool svgIsUnsafe(QIODevice &file)
{
    quint64 drawn = 0;
    return svgIsUnsafe(file, 0, drawn);
}

// Picture files read, for tests.
std::atomic<int> localImageReads{0};

// A picture file, unless it is an SVG that would reach another machine or
// take too long to draw.
QImage readImage(const QString &path)
{
    ++localImageReads;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (reader.format().startsWith("svg")) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly) && svgIsUnsafe(file))
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
// Qt matches names ignoring case, by Unicode rules: the long s and the Kelvin
// sign count as "s" and "k".
bool mayLoadPictures(const QString &html)
{
    return html.contains(QLatin1String("<img"), Qt::CaseInsensitive)
           || html.contains(QLatin1String("background"), Qt::CaseInsensitive)
           || html.contains(QLatin1String("style"), Qt::CaseInsensitive);
}

// The picture Qt's image handler shows for one it cannot read.
QImage brokenPicture()
{
    static const QImage image = [] {
        QImage icon(QStringLiteral(":/qt-project.org/styles/commonstyle/images/file-16.png"));
        if (icon.isNull()) {
            icon = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
            icon.fill(Qt::transparent);
        }
        return icon;
    }();
    return image;
}

// The same answer from the UTF-8 bytes, without making a QString (a scan
// of every block's HTML for remote pictures does this for each block): the
// names in ASCII letters of either case, and the text above for HTML with a
// character that folds to an ASCII letter.
bool mayLoadPictures(QByteArrayView html)
{
    const char *text = html.data();
    const qsizetype size = html.size();
    const auto at = [&](qsizetype i, QLatin1StringView word) {
        return size - i >= word.size() && qstrnicmp(text + i, word.data(), size_t(word.size())) == 0;
    };
    for (qsizetype i = 0; i < size; ++i) {
        switch (text[i] | 0x20) {
        case '<':
            if (at(i, "<img"_L1))
                return true;
            break;
        case 'b':
            if (at(i, "background"_L1))
                return true;
            break;
        case 's':
            if (at(i, "style"_L1))
                return true;
            break;
        default:
            // U+017F (long s) is C5 BF, U+212A (Kelvin sign) E2 84 AA.
            if ((text[i] == '\xC5' || text[i] == '\xE2')
                && (html.sliced(i).startsWith("\xC5\xBF") || html.sliced(i).startsWith("\xE2\x84\xAA")))
                return mayLoadPictures(QString::fromUtf8(html));
            break;
        }
    }
    return false;
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
        if (!m_math.isEmpty() && !m_listing)
            m_mathObject = MathObject::install(this);
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

    // Formulas (the pictures "vn-math:N" inline, "vn-math-display:N" on a
    // line of their own in a list or quote) become MathObject characters:
    // drawn as shapes, sharp at any zoom, on the baseline of the text around
    // them and as large as it (a heading's are a heading's size). One wider
    // than the column shrinks to it, its baseline still on the text's. (The
    // HTML cannot ask for the alignment. Centred instead, a formula pushed
    // its line apart above and below.)
    //
    // Line spacing follows TeX's rule (\baselineskip, \lineskiplimit): a
    // formula that fits in the room its line has anyway (the font's ascent
    // above the baseline, its descent and leading below, and half the space
    // the line spacing adds on either side) changes nothing, as a superscript,
    // a subscript or a pair of brackets does not; a taller one moves the
    // lines apart by what it needs beyond that. The room is the font's own
    // measure, however a font divides it (Calibri's small sizes have no
    // descent and all their room below as leading; Cambria's, no descent and
    // a negative leading), and a formula within it is drawn on the baseline
    // the line was laid out with; a deeper one gets its depth from a font
    // made for that (MathObject::reach), not from a larger text font. The
    // paragraphs space their lines as a web page does: the space the line
    // spacing adds is the text's, not a tall formula's multiplied.
    void placeMath()
    {
        if (!m_mathObject)
            return;
        // One change, and none to undo: a change of format for each formula,
        // each recorded to be undone, took longer than the rest of the block.
        const bool undo = isUndoRedoEnabled();
        setUndoRedoEnabled(false);
        QTextCursor cursor(this);
        cursor.beginEditBlock();
        QList<QTextBlock> withMath;
        for (QTextBlock block = begin(); block.isValid(); block = block.next()) {
            // Half the space the line spacing adds between lines, and a
            // pixel more (TeX's \lineskiplimit: lines may come that close),
            // so that a formula a fraction of a pixel too deep does not
            // move them apart by a whole pixel.
            const qreal slack = std::max<qreal>(0, lineGap(block) / 2) + 1;
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                const QTextCharFormat old = fragment.charFormat();
                if (!old.isImageFormat())
                    continue;
                const QString name = old.toImageFormat().name();
                const bool display = name.startsWith(QLatin1String("vn-math-display:"));
                if (!display && !name.startsWith(QLatin1String("vn-math:")))
                    continue;
                const auto formula = cachedFormula(math::withMacros(m_math.value(name.section(u':', 1).toInt())), display);
                // The font Qt measures the character with: the text's (as the
                // document resolves it).
                const QFont font = old.font();
                qreal scale = QFontInfo(font).pixelSize() * (display ? displayMathScale : inlineMathScale)
                              / math::layoutPixelSize;
                if (formula->width * scale > m_context.width && m_context.width > 0)
                    scale = m_context.width / formula->width;
                QTextCharFormat format = old;
                for (const int property : {int(QTextFormat::ImageName), int(QTextFormat::ImageWidth),
                                           int(QTextFormat::ImageHeight), int(QTextFormat::ImageQuality)})
                    format.clearProperty(property);
                format.setObjectType(MathObject::type);
                // Qt's line reaches as far above and below the baseline as the
                // farthest of its parts, and its text's leading (which may be
                // negative) is drawn under the descent (QTextLine's layout).
                const QFontMetricsF metrics(font);
                const qreal ascent = metrics.ascent();
                const qreal descent = metrics.descent();
                const qreal leading = metrics.leading();
                const qreal up = formula->ascent * scale;
                const qreal down = formula->depth * scale;
                MathObject::Placed placed{formula, scale};
                if (up <= ascent + slack && down <= descent + leading + slack) {
                    format.setVerticalAlignment(QTextCharFormat::AlignTop);
                    placed.box = ascent + descent;
                    placed.above = ascent;
                } else {
                    // The line grows by what the formula needs beyond the room.
                    placed.withinLine = false;
                    placed.above = std::max(ascent, up - slack);
                    format.setVerticalAlignment(QTextCharFormat::AlignBaseline);
                    // The line keeps its text's leading (an object adds an
                    // ascent and a descent only): the formula's character
                    // reaches as far below the baseline as the leading
                    // leaves to reach.
                    const qreal depth = down - slack - leading;
                    if (depth > descent)
                        MathObject::reach(format, depth);
                }
                format.setProperty(MathObject::indexProperty, m_mathObject->add(placed));
                cursor.setPosition(fragment.position());
                cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
                cursor.setCharFormat(format);
                if (withMath.isEmpty() || withMath.last() != block)
                    withMath.append(block);
            }
        }
        // A line height of N% makes a line N% as high as its tallest part
        // (QTextBlockFormat::lineHeight): a line with a fraction got the
        // spacing of a fraction. Instead, every line gets the spacing the
        // text's height gives, added: the same for a line of text.
        for (const QTextBlock &block : std::as_const(withMath)) {
            QTextBlockFormat format = block.blockFormat();
            if (format.lineHeightType() != QTextBlockFormat::ProportionalHeight)
                continue;
            format.setLineHeight(lineGap(block), QTextBlockFormat::LineDistanceHeight);
            cursor.setPosition(block.position());
            cursor.setBlockFormat(format);
        }
        cursor.endEditBlock();
        setUndoRedoEnabled(undo);
    }

    // The space a block's line height of N% adds between lines of its text
    // (none for other line heights).
    static qreal lineGap(const QTextBlock &block)
    {
        const QTextBlockFormat format = block.blockFormat();
        if (format.lineHeightType() != QTextBlockFormat::ProportionalHeight)
            return 0;
        const QFontMetricsF metrics(block.charFormat().font());
        // As Qt measures a line's height for the proportion (its leading may
        // be negative).
        const qreal text = qCeil(metrics.ascent() + metrics.descent() + metrics.leading());
        return text * (format.lineHeight() / 100 - 1);
    }

    // Path elements of the formulas: what they cost to keep and draw.
    qsizetype mathElements() const { return m_mathObject ? m_mathObject->elements() : 0; }

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
                if (allowedPictureName(format.name()))
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
    // Every http(s) address asked for so far that may be fetched once
    // allowed (not those of this machine or a local network, which never are).
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
        // Formulas are MathObjects (placeMath) by the time anything is
        // drawn: a name left over is blank.
        if (url.toString().startsWith(QLatin1String("vn-math")))
            return standIn(type, url);
        const QString path = localPath(url);
        if (path.isEmpty())
            return QVariant();
        const QImage image = cachedLocalImage(path, m_context.width, m_context.devicePixelRatio);
        if (image.isNull()) {
            // Declined, Qt's image handler would read the file itself (an SVG
            // naming another machine included): show what it shows then.
            return QFileInfo::exists(path) ? keep(type, url, brokenPicture()) : QVariant();
        }
        return keep(type, url, image);
    }

    // Whether Qt may be given a picture name: those made here (math, the
    // blocked stand-in), web addresses (asked for only with consent), data
    // URLs, and files localPath() accepts. Anything else, invalid URLs above
    // all (Qt's image handler opens their text as a file name), is not.
    bool allowedPictureName(const QString &name) const
    {
        static const QRegularExpression internal(QStringLiteral("^(vn-math(-display)?:\\d+|vn-blocked:)$"));
        if (md::mayNameOtherMachine(name))
            return false;
        const QUrl url(name);
        if (!url.isValid())
            return false;
        if (internal.match(name).hasMatch())
            return true;
        if (RemoteImages::isWebUrl(url))
            return !url.host().isEmpty();
        if (url.scheme() == QLatin1String("data"))
            return true;
        return !localPath(url).isEmpty();
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
        if (const QString address = url.toString();
            !m_remote.contains(address) && RemoteImages::redirectAllowed(QUrl(), url))
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
        if (!url.isValid() || namesOtherMachine(url) || md::mayNameOtherMachine(url.toString()))
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
    MathObject *m_mathObject = nullptr; // Owned by the document; with formulas only.
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
std::atomic<bool> partsEnabled{true};

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

// Long runs of text with no place to break: wrapping at a boundary first and
// then anywhere rescans the run for every line (see the limits above).
// Wrapping anywhere puts the lines in the same places for such a run.
bool wrapsCodeAnywhere(const QString &source)
{
    return layoutGuard && runSquaresOf(source) > codeRunSquares;
}

// A huge block is not highlighted: that costs twice the layout.
bool highlightsCode(qsizetype sourceChars)
{
    return !layoutGuard || sourceChars <= highlightMax;
}

std::unique_ptr<QTextDocument> codeDocument(const QString &source, const QString &definition,
                                            const RenderContext &context, qreal width, bool wrapAnywhere,
                                            bool highlight)
{
    auto doc = std::make_unique<QTextDocument>();
    doc->setDocumentMargin(0);
    doc->setDefaultFont(context.codeFont);
    QTextOption option;
    option.setWrapMode(wrapAnywhere ? QTextOption::WrapAnywhere : QTextOption::WrapAtWordBoundaryOrAnywhere);
    doc->setDefaultTextOption(option);
    doc->setPlainText(source);
    doc->setTextWidth(width);
    if (!highlight)
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
        if (svgIsUnsafe(scanned))
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
    r.m_doc = codeDocument(source, QStringLiteral("Markdown"), context, context.width - 2 * codePadX,
                           wrapsCodeAnywhere(source), highlightsCode(source.size()));
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

void RenderedBlock::setPartsEnabled(bool on)
{
    partsEnabled = on;
}

bool RenderedBlock::isLongCodeBox(const core::Block &block, qsizetype sourceChars)
{
    if (!partsEnabled || highlightsCode(sourceChars))
        return false;
    switch (block.kind) {
    case core::BlockKind::Code:
        // Not a Mermaid diagram (an image, or its source with the error below).
        return QString::fromUtf8(block.lang.data(), block.lang.size()).trimmed().compare(QLatin1String("mermaid"),
                                                                                       Qt::CaseInsensitive) != 0;
    case core::BlockKind::FrontMatter:
        return true;
    case core::BlockKind::Html:
        return tooHeavyForRichText(block.html.data(), block.html.size());
    default:
        return false;
    }
}

bool RenderedBlock::wrapsAnywhere(const QString &source)
{
    return wrapsCodeAnywhere(source);
}

std::unique_ptr<RenderedBlock> RenderedBlock::createPart(const QString &source, const Part &part,
                                                         const RenderContext &context)
{
    auto result = std::unique_ptr<RenderedBlock>(new RenderedBlock);
    RenderedBlock &r = *result;
    r.m_width = context.width;
    r.m_kind = Kind::Code;
    r.m_part = part;
    // As codeBox() makes the whole box: the same text options, unhighlighted.
    // A part above another ends in its last line break, so that a selection
    // that runs on shows it as the box does; the empty line after it is
    // below the part.
    r.m_doc = codeDocument(part.last ? source : source + u'\n', QString(), context, context.width - 2 * codePadX,
                           part.wrapAnywhere, false);
    const qreal textHeight = part.last ? r.m_doc->size().height()
                                       : r.m_doc->documentLayout()->blockBoundingRect(r.m_doc->lastBlock()).top();
    r.m_height = textHeight + (part.first ? codePadY : 0) + (part.last ? codePadY + blockGap : 0);
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
    return create(block, source, context, false);
}

std::unique_ptr<RenderedBlock> RenderedBlock::createItem(const core::Block &block, const QString &source,
                                                         const RenderContext &context)
{
    return create(block, source, context, true);
}

std::unique_ptr<RenderedBlock> RenderedBlock::create(const core::Block &block, const QString &source,
                                                     const RenderContext &context, bool item)
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
        // Drawn as shapes (see paint()), shrunk to the column if wider.
        r.m_kind = Kind::Image;
        r.m_formula = cachedFormula(math::withMacros(text), true);
        r.m_formulaScale = QFontInfo(context.font).pixelSize() * displayMathScale / math::layoutPixelSize;
        if (r.m_formula->width * r.m_formulaScale > context.width && context.width > 0)
            r.m_formulaScale = context.width / r.m_formula->width;
        r.m_centered = true;
        r.m_height = r.pictureSize().height() + 2 * codePadY + blockGap;
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
        doc->placeMath();
        doc->fitImages(context.width);
        if (item && !doc->begin().textList() && doc->blockCount() == 1 && doc->begin().text().isEmpty()) {
            // An empty item, which Qt drops: an empty line of the list as one
            // with text would make it (the text taken out again).
            const qsizetype li = html.indexOf(QLatin1String("<li>"));
            if (li >= 0) {
                doc->setHtml(QString(html).insert(li + 4, u'x'));
                QTextCursor text(doc.get());
                text.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
                text.removeSelectedText();
            }
        }
        if (item) {
            // The item's list is the one at the first level (its own lists
            // are deeper). Its marker is drawn by paintItemMarker instead,
            // with the number the item has in the whole list.
            r.m_item.made = true;
            const QTextBlock first = doc->begin();
            for (QTextBlock b = first; b.isValid() && !r.m_item.list; b = b.next()) {
                if (QTextList *list = b.textList(); list && list->format().indent() == 1)
                    r.m_item.list = list;
            }
            if (QTextList *list = r.m_item.list) {
                r.m_item.regular = first.textList() == list && list->count() == 1;
                QTextListFormat format = list->format();
                r.m_item.style = format.style();
                format.setStyle(QTextListFormat::ListStyleUndefined);
                list->setFormat(format);
            }
            r.m_item.topMargin = first.blockFormat().topMargin();
            r.m_item.bottomMargin = doc->lastBlock().blockFormat().bottomMargin();
        }
        doc->setTextWidth(context.width);
        // An item ends where the next starts (see itemAdvance).
        r.m_height = doc->size().height() + (item ? 0 : blockGap);
        // Backgrounds are asked for when painted: start their downloads now.
        if (context.allowRemoteImages && mayLoadPictures(QByteArrayView(block.html.data(), qsizetype(block.html.size()))))
            doc->loadPaintedPictures();
        r.m_pending = doc->hasPendingDownload();
        r.m_pictureBytes = doc->pictureBytes() // Resolved by the layout (and the dry paint).
                           + doc->mathElements() * qsizetype(sizeof(QPainterPath::Element));
        r.m_doc = std::move(doc);
        r.collectLineAnchors(0);
        return result;
    }
    }
}

bool RenderedBlock::mayAskForPictures(const core::Block &block)
{
    return block.kind == core::BlockKind::Html
           && mayLoadPictures(QByteArrayView(block.html.data(), qsizetype(block.html.size())));
}

QStringList RenderedBlock::remoteImages(const core::Block &block, const RenderContext &context)
{
    if (!mayAskForPictures(block))
        return {};
    const QString html = QString::fromUtf8(block.html.data(), block.html.size());
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

QSizeF RenderedBlock::pictureSize() const
{
    if (m_formula)
        return QSizeF(m_formula->width, m_formula->height()) * m_formulaScale;
    return m_image.size() / m_image.devicePixelRatio();
}

QPointF RenderedBlock::contentOffset() const
{
    return m_kind == Kind::Code ? QPointF(codePadX, m_part.first ? codePadY : 0) : QPointF(0, 0);
}

void RenderedBlock::paint(QPainter &painter, const QPointF &topLeft, qreal width, const RenderedSelection &selection) const
{
    switch (m_kind) {
    case Kind::Empty:
        return;
    case Kind::Image: {
        const QSizeF size = pictureSize();
        const qreal x = m_centered ? topLeft.x() + qMax<qreal>(0, (width - size.width()) / 2) : topLeft.x();
        const QRectF imageRect(QPointF(x, topLeft.y() + codePadY), size);
        if (m_formula) {
            m_formula->draw(painter, imageRect.topLeft() + QPointF(0, m_formula->ascent * m_formulaScale),
                            m_formulaScale, theme::color(theme::Role::text));
        } else {
            painter.drawImage(imageRect, m_image);
        }
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
        // A part of a box (see createPart) paints the part of the whole box
        // that it covers: the box runs on past the edges it shares with the
        // other parts.
        constexpr qreal radius = 3;
        const qreal top = m_part.first ? topLeft.y() : topLeft.y() - 2 * radius;
        const qreal bottom = m_part.last ? topLeft.y() + m_height - blockGap : topLeft.y() + m_height + 2 * radius;
        QPainterPath box;
        box.addRoundedRect(QRectF(topLeft.x(), top, width, bottom - top), radius, radius);
        painter.save();
        // Clipped at the shared edges only: the clip snaps to whole pixels,
        // which would drop the antialiased row of an outer edge at a
        // fractional position (one the whole box paints).
        if (!m_part.first || !m_part.last) {
            const qreal clipTop = m_part.first ? top - 1 : topLeft.y();
            const qreal clipBottom = m_part.last ? bottom + 1 : topLeft.y() + m_height;
            painter.setClipRect(QRectF(topLeft.x() - 1, clipTop, width + 2, clipBottom - clipTop), Qt::IntersectClip);
        }
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(box, theme::color(theme::Role::codeBlock));
        painter.restore();
        [[fallthrough]];
    }
    case Kind::Rich: {
        painter.save();
        painter.translate(topLeft + contentOffset());
        QAbstractTextDocumentLayout::PaintContext ctx;
        // Only what shows: a block can be far taller than the window, and
        // the layout draws every line it is not told to skip.
        if (painter.hasClipping())
            ctx.clip = painter.clipBoundingRect();
        ctx.palette.setColor(QPalette::Text, theme::color(theme::Role::text));
        const QColor selectionText = theme::color(theme::Role::selectionText);
        if (selection.runsOn && m_item.made) {
            // In the whole list a selection that goes on to the next item
            // takes the line break after this one: Qt marks it (in
            // QTextLayout::draw) by a quarter of the line's height after the
            // last line. This item ends its document, which has no break.
            const QTextBlock last = m_doc->lastBlock();
            const QTextLayout *layout = last.layout();
            if (layout && layout->lineCount() > 0) {
                QRectF line = layout->lineAt(layout->lineCount() - 1).naturalTextRect();
                line.translate(layout->position());
                line.setBottom(qCeil(line.bottom()));
                QPainterPath mark;
                mark.addRect(QRectF(line.right(), line.top(), line.height() / 4, line.height()));
                painter.save();
                painter.setPen(Qt::NoPen);
                painter.setBrush(theme::color(theme::Role::selection));
                painter.drawPath(mark);
                painter.restore();
            }
        }
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

qreal RenderedBlock::itemAdvance(const RenderedBlock *next) const
{
    if (!m_item.made)
        return m_height;
    if (!next || !next->m_item.made)
        return m_height + blockGap;
    // As QTextDocumentLayout spaces two blocks: the larger of the margins
    // between them, in its fixed point (1/64 px).
    return m_height + qRound(qMax(m_item.bottomMargin, next->m_item.topMargin) * 64) / 64.0;
}

void RenderedBlock::paintItemMarker(QPainter &painter, const QPointF &topLeft, bool ordered, int number,
                                    bool selected) const
{
    if (!m_item.list)
        return;
    // The marker as Qt draws it for a whole list (its size and place come
    // from the item's font), with the number the item has in the whole list.
    // Numbers for an ordered list, else the list's bullet.
    int style = m_item.style;
    if (ordered)
        style = QTextListFormat::ListDecimal;
    else if (style == QTextListFormat::ListDecimal)
        style = QTextListFormat::ListDisc;
    painter.save();
    painter.translate(topLeft + contentOffset());
    painter.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < m_item.list->count(); ++i) {
        const QTextBlock block = m_item.list->item(i);
        const QTextLayout *layout = block.layout();
        if (!layout || layout->lineCount() == 0)
            continue;
        const QTextCharFormat format = block.charFormat();
        const QFont font = format.font();
        const QFontMetrics metrics(font);
        const QTextLine line = layout->lineAt(0);
        const bool rtl = block.textDirection() == Qt::RightToLeft;
        // The text's first line, in whole pixels; the marker's line has the
        // marker's font, its top where that font's ascent meets the line's.
        const QRectF text = line.naturalTextRect();
        const QPoint start = layout->position().toPoint() + text.topLeft().toPoint();
        const int lineTop = start.y() + qRound(line.ascent()) - metrics.ascent();
        const int textEdge = rtl ? start.x() + int(text.width()) : start.x();

        QString label;
        int width = 0;
        int height = 0;
        switch (style) {
        case QTextListFormat::ListDecimal:
            label = rtl ? QStringLiteral(".%1").arg(number + i) : QStringLiteral("%1.").arg(number + i);
            width = metrics.horizontalAdvance(label);
            height = metrics.height();
            break;
        case QTextListFormat::ListDisc:
        case QTextListFormat::ListCircle:
        case QTextListFormat::ListSquare:
            width = height = metrics.lineSpacing() / 3;
            break;
        default:
            continue;
        }
        // A space between the marker and the text, on the text's start side.
        const int space = metrics.horizontalAdvance(u' ');
        const qreal left = rtl ? textEdge + space : textEdge - space - width;
        const QRectF box(left, lineTop + metrics.height() / 2.0 - height / 2.0, width, height);

        QBrush ink;
        if (selected) {
            // The selection's colours, as paint() uses them.
            ink = theme::color(theme::Role::selectionText);
            painter.fillRect(box, theme::color(theme::Role::selection));
        } else {
            ink = format.foreground();
            if (ink.style() == Qt::NoBrush)
                ink = theme::color(theme::Role::text);
        }
        if (style == QTextListFormat::ListDecimal) {
            // Laid out as the item's text is (fractional metrics, as on
            // macOS, place the baseline off a whole pixel).
            QTextLayout number(label, font, m_doc->documentLayout()->paintDevice());
            QTextOption option(Qt::AlignLeft | Qt::AlignAbsolute);
            option.setTextDirection(block.textDirection());
            number.setTextOption(option);
            number.beginLayout();
            QTextLine numberLine = number.createLine();
            if (numberLine.isValid())
                numberLine.setLeadingIncluded(true);
            number.endLayout();
            painter.setPen(QPen(ink, 0));
            number.draw(&painter, QPointF(box.left(), lineTop));
        } else if (style == QTextListFormat::ListSquare) {
            painter.fillRect(box, ink);
        } else if (style == QTextListFormat::ListCircle) {
            painter.setPen(QPen(ink, 0));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(box.translated(0.5, 0.5));
        } else {
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink);
            painter.drawEllipse(box);
        }
    }
    painter.restore();
}

int RenderedBlock::hitTest(const QPointF &point) const
{
    if (!m_doc)
        return -1;
    return m_doc->documentLayout()->hitTest(point - contentOffset(), Qt::FuzzyHit);
}

QTextTable *RenderedBlock::theTable() const
{
    if (m_kind != Kind::Rich || !m_doc)
        return nullptr;
    // The root frame holds the table directly and nothing else but the empty
    // paragraphs around it.
    QTextTable *table = nullptr;
    for (QTextFrame *frame : m_doc->rootFrame()->childFrames()) {
        if (auto *t = qobject_cast<QTextTable *>(frame)) {
            if (table)
                return nullptr;
            table = t;
        } else {
            return nullptr;
        }
    }
    if (!table)
        return nullptr;
    for (QTextBlock b = m_doc->begin(); b.isValid() && b.position() < table->firstPosition(); b = b.next()) {
        if (!b.text().isEmpty())
            return nullptr;
    }
    for (QTextBlock b = m_doc->findBlock(table->lastPosition() + 1); b.isValid(); b = b.next()) {
        if (!b.text().isEmpty())
            return nullptr;
    }
    return table;
}

QSize RenderedBlock::tableCells() const
{
    const QTextTable *table = theTable();
    return table ? QSize(table->columns(), table->rows()) : QSize();
}

std::optional<QRectF> RenderedBlock::tableCellRect(int row, int col) const
{
    const QTextTable *table = theTable();
    if (!table || row < 0 || col < 0 || row >= table->rows() || col >= table->columns())
        return std::nullopt;
    QAbstractTextDocumentLayout *layout = m_doc->documentLayout();
    // The text of a cell. The cells themselves are not laid out anywhere one
    // can ask, so their rectangles are made from the text: they tile the table.
    const auto content = [&](int r, int c) {
        QRectF rect;
        const QTextTableCell cell = table->cellAt(r, c);
        if (!cell.isValid())
            return rect;
        for (QTextBlock b = cell.firstCursorPosition().block(); b.isValid() && b.position() <= cell.lastPosition(); b = b.next())
            rect = rect.united(layout->blockBoundingRect(b));
        return rect;
    };
    const QRectF own = content(row, col);
    if (own.isNull())
        return std::nullopt;
    // A cell starts a padding and a border before its text, and goes on up
    // to where the next one starts; the last cells are as far on as the
    // one before, or as far as their text and padding.
    const qreal edge = table->format().cellPadding() + 1.5;
    const qreal top = own.top() - edge;
    const qreal left = own.left() - edge;
    const qreal bottom = row + 1 < table->rows() ? content(row + 1, col).top() - edge
                         : row > 0              ? top + (own.top() - content(row - 1, col).top())
                                                : own.bottom() + edge;
    const qreal right = col + 1 < table->columns() ? content(row, col + 1).left() - edge : own.right() + edge;
    return QRectF(QPointF(left, top), QPointF(right, bottom)).translated(contentOffset());
}

int RenderedBlock::TableGrid::colAt(qreal x) const
{
    const auto it = std::upper_bound(xs.begin() + 1, xs.end() - 1, x);
    return int(it - (xs.begin() + 1));
}

int RenderedBlock::TableGrid::rowAt(qreal y) const
{
    const auto it = std::upper_bound(ys.begin() + 1, ys.end() - 1, y);
    return int(it - (ys.begin() + 1));
}

const RenderedBlock::TableGrid *RenderedBlock::tableGrid() const
{
    if (!m_tableGrid) {
        m_tableGrid.emplace();
        const QSize size = tableCells();
        if (size.width() > 0 && size.height() > 0) {
            TableGrid grid;
            for (int col = 0; col < size.width(); ++col) {
                const auto rect = tableCellRect(0, col);
                if (!rect)
                    return nullptr;
                grid.xs.push_back(rect->left());
                if (col + 1 == size.width())
                    grid.xs.push_back(rect->right());
            }
            for (int row = 0; row < size.height(); ++row) {
                const auto rect = tableCellRect(row, 0);
                if (!rect)
                    return nullptr;
                grid.ys.push_back(rect->top());
                if (row + 1 == size.height())
                    grid.ys.push_back(rect->bottom());
            }
            // Merged cells (an HTML table's colspan, rowspan) give columns or
            // rows no width of their own: no grid for such a table.
            const auto increasing = [](const std::vector<qreal> &v) {
                return std::adjacent_find(v.begin(), v.end(), std::greater_equal<qreal>()) == v.end();
            };
            if (increasing(grid.xs) && increasing(grid.ys))
                *m_tableGrid = std::move(grid);
        }
    }
    return m_tableGrid->has_value() ? &**m_tableGrid : nullptr;
}

std::optional<RenderedBlock::TableCellHit> RenderedBlock::tableCellAt(const QPointF &point) const
{
    const TableGrid *grid = tableGrid();
    if (!grid) {
        // No grid (merged cells): the first cell that holds the point.
        const QTextTable *table = theTable();
        for (int row = 0; table && row < table->rows(); ++row) {
            for (int col = 0; col < table->columns(); ++col) {
                const auto rect = tableCellRect(row, col);
                if (rect && rect->contains(point))
                    return TableCellHit{row, col, *rect};
            }
        }
        return std::nullopt;
    }
    if (point.x() < grid->xs.front() || point.x() >= grid->xs.back() || point.y() < grid->ys.front()
        || point.y() >= grid->ys.back())
        return std::nullopt;
    // The cells tile the table (see tableCellRect): the one holding the point.
    const int row = grid->rowAt(point.y());
    const int col = grid->colAt(point.x());
    return TableCellHit{row, col, QRectF(QPointF(grid->xs[col], grid->ys[row]), QPointF(grid->xs[col + 1], grid->ys[row + 1]))};
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
        const QSizeF size = pictureSize();
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
    if (m_formula) {
        // Shapes: drawn larger rather than magnified, as diagrams are.
        math::Style style;
        style.pixelSize = 3 * m_formulaScale * math::layoutPixelSize;
        style.color = theme::color(theme::Role::text);
        return math::toImage(*m_formula, style);
    }
    QImage image = m_image;
    image.setDevicePixelRatio(1);
    return image;
}

std::optional<QRectF> RenderedBlock::caretRect(int position) const
{
    if (!m_doc || (m_kind != Kind::Rich && m_kind != Kind::Code))
        return std::nullopt;
    position = qBound(0, position, m_doc->characterCount() - 1);
    const QTextBlock block = m_doc->findBlock(position);
    const QTextLayout *layout = block.isValid() ? block.layout() : nullptr;
    if (!layout)
        return std::nullopt;
    const int inBlock = position - block.position();
    const QTextLine line = layout->lineForTextPosition(inBlock);
    if (!line.isValid())
        return std::nullopt;
    // The block's rectangle has the layout's origin, in document coordinates
    // (table cells included), as collectLineAnchors reads it.
    const QPointF origin = m_doc->documentLayout()->blockBoundingRect(block).topLeft() + contentOffset();
    return QRectF(origin.x() + line.cursorToX(inBlock), origin.y() + line.y(), 2, line.height());
}

std::pair<int, int> RenderedBlock::lineAt(int position) const
{
    if (!m_doc)
        return {0, 0};
    position = qBound(0, position, m_doc->characterCount() - 1);
    const QTextBlock block = m_doc->findBlock(position);
    const QTextLayout *layout = block.isValid() ? block.layout() : nullptr;
    const QTextLine line = layout ? layout->lineForTextPosition(position - block.position()) : QTextLine();
    if (!line.isValid())
        return {position, position};
    return {block.position() + line.textStart(), block.position() + line.textStart() + line.textLength()};
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
    if (m_formula) // Shared with the formula cache, as pictures are.
        kib += int(m_formula->elements() * qsizetype(sizeof(QPainterPath::Element)) / 1024);
    kib += int(m_pictureBytes / 1024);
    if (m_doc) {
        const int characters = m_doc->characterCount();
        kib += m_kind == Kind::Code ? 20 + characters / 10 : 40 + characters / 16;
    }
    return kib;
}

} // namespace vn
