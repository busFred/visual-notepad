#include "BlockRenderer.h"

#include "../Theme.h"
#include "Folding.h"
#include "MarkdownEditing.h"
#include "MathObject.h"
#include "MathRenderer.h"
#include "ReadFlow.h"
#include "RemoteImages.h"
#include "TableLayout.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QStyle>
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
#include <tuple>

namespace vn {

namespace {

using namespace Qt::StringLiterals;

constexpr qreal codePadX = 10;
constexpr qreal codePadY = 5;
constexpr qreal blockGap = 4;
// A chip is this much higher than the text in it (and this much wider than
// its content on each side); the strip over a block is this much higher.
constexpr qreal chipExtraHeight = 8;
constexpr qreal chipPadX = 10;
constexpr qreal chipChevronGap = 7;
constexpr qreal chevronSize = 8;
constexpr qreal stripExtraHeight = 4;
// The scrollbar under a block wider than its column: as high as the style's
// (QScrollBar:horizontal in Theme.cpp), a line above its thumb included.
constexpr qreal scrollBarHeight = 12;
constexpr qreal scrollThumbMargin = 2;
constexpr qreal scrollThumbMin = 24;
// Wide enough for a table's natural width to be laid out unwrapped.
constexpr qreal unboundedWidth = 1e6;

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

    // Whether find() would find it (without counting as a request).
    bool contains(const QString &version) const { return m_entries.contains(version); }

    QImage find(const QString &version)
    {
        ++m_requests;
        const auto it = m_entries.find(version);
        if (it == m_entries.end())
            return {};
        it->used = ++m_clock;
        return it->compact ? it->image.convertToFormat(QImage::Format_RGB32) : it->image;
    }

    // `image` was just read because find() had nothing. False if it is not
    // kept.
    bool insert(const QString &version, const QImage &image)
    {
        const bool compact = image.format() == QImage::Format_RGB32;
        if (!admit(version, compact ? imageCost(image) * 3 / 4 : imageCost(image), false))
            return false;
        store(version, compactForm(image), compact);
        return true;
    }

    // As insert(), for a picture read on a worker thread for a render to
    // come (so wanted again soon), given in the form kept (`kept`, see
    // compactForm; `compact`: it was 32 bit RGB) and what it costs so (`kib`).
    bool insertRead(const QString &version, const QImage &kept, bool compact, qsizetype kib)
    {
        if (!admit(version, kib, true))
            return false;
        store(version, kept, compact);
        return true;
    }

    // What insert() takes from a picture: whether it is kept 24 bit, and its cost.
    static std::pair<bool, qsizetype> costOf(const QImage &image)
    {
        const bool compact = image.format() == QImage::Format_RGB32;
        return {compact, compact ? imageCost(image) * 3 / 4 : imageCost(image)};
    }

    // The form a picture is kept in (any thread).
    static QImage compactForm(const QImage &image)
    {
        return image.format() == QImage::Format_RGB32 ? image.convertToFormat(QImage::Format_RGB888) : image;
    }

    void clear()
    {
        m_entries.clear();
        m_reads.clear();
        m_kib = 0;
    }

private:
    // Whether a picture of `kib` decoded is to be kept (see above), with
    // room made for it.
    bool admit(const QString &version, qsizetype kib, bool wanted)
    {
        if (kib > m_limit)
            return false; // Does not fit at all.
        bool again = wanted;
        for (auto it = m_reads.begin(); it != m_reads.end(); ++it) {
            if (it->first == version) {
                again = again || m_requests - it->second <= qMax<quint64>(4, m_entries.size());
                m_reads.erase(it);
                break;
            }
        }
        m_reads.push_back({version, m_requests});
        if (m_reads.size() > 64)
            m_reads.pop_front();
        if (m_kib + kib > m_limit && !again)
            return false; // Full, and not shown to be wanted again soon.
        evictFor(kib);
        return true;
    }

    void store(const QString &version, const QImage &kept, bool compact)
    {
        Entry entry;
        entry.compact = compact;
        entry.image = kept;
        entry.kib = imageCost(entry.image);
        entry.used = ++m_clock;
        m_kib += entry.kib;
        m_entries.insert(version, entry);
    }

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

// Where a figure of the given own size lands: fitted to the box (never
// enlarged), then scaled by what the user made of it, within the column.
struct FigureFit
{
    QSizeF size;
    qreal factor = 1;
    qreal maxFactor = 1;
    qreal fittedWidth = 0;
};

FigureFit fitFigure(const QSizeF &natural, const RenderContext &context, qreal factor)
{
    FigureFit fit;
    if (natural.width() <= 0 || natural.height() <= 0) {
        fit.size = natural;
        return fit;
    }
    const qreal column = qMax<qreal>(1, context.width);
    const qreal box = context.figureWidth > 0 ? qMin(column, context.figureWidth) : column;
    qreal scale = qMin<qreal>(1, box / natural.width());
    if (context.figureHeight > 0)
        scale = qMin(scale, context.figureHeight / natural.height());
    fit.fittedWidth = natural.width() * scale;
    fit.maxFactor = qMax<qreal>(1, column / fit.fittedWidth);
    fit.factor = qBound(RenderedBlock::minFigureFactor, factor, fit.maxFactor);
    fit.size = natural * (scale * fit.factor);
    return fit;
}

// The key of a file's current version, for caches.
QString fileVersion(const QString &path)
{
    const QFileInfo info(path);
    return QStringLiteral("%1|%2|").arg(info.lastModified().toMSecsSinceEpoch()).arg(info.size()) + path;
}

// A local picture at its own size, read once (see above); null if unreadable.
QImage decodedLocalImage(const QString &path)
{
    const QString version = fileVersion(path);
    QImage image = decodedPictures().find(version);
    if (image.isNull()) {
        image = readImage(path);
        if (!image.isNull())
            decodedPictures().insert(version, image);
    }
    return image;
}

// Local pictures scaled to a column (see cachedLocalImage), by scaledKey().
QCache<QString, QImage> &scaledLocalImages()
{
    static QCache<QString, QImage> cache(64 * 1024); // KiB
    return cache;
}

QString scaledKey(const QString &version, qreal width, qreal dpr)
{
    return QStringLiteral("%1|%2|").arg(width).arg(dpr) + version;
}

// Picture files at least this big are read on the thread pool when the
// context allows it (RenderContext::picturesLater): reading one of 12
// million pixels takes 0.1 s.
constexpr qint64 bigPicturePixels = 1000 * 1000;

// Big picture files on their way (by file version): asked for (waiting or
// being read), or to be read on the GUI thread as small ones are
// (unreadable, or too big for the decoded pictures to keep, which is where
// a read one goes). Two are read at a time, the latest asked for first: a
// page turned past should not hold up the one shown, and each takes its
// size in memory while it is read. Of those waiting only the latest few
// are kept (asked for again, the others wait again). GUI thread only.
struct LaterPictures
{
    struct Request
    {
        QString path;
        QString version;
        QString key; // Of the scaled pictures.
        qreal width = 0;
        qreal dpr = 1;
    };
    QSet<QString> reading;
    QSet<QString> now;
    std::deque<Request> waiting;
    int running = 0;
    static constexpr int runningMax = 2;
    static constexpr size_t waitingMax = 4;
    // Bumped by releasePictureCaches(): what is being read is not wanted.
    std::atomic<int> generation{0};
};

LaterPictures &laterPictures()
{
    static LaterPictures pictures;
    return pictures;
}

bool isBig(const QSize &size)
{
    return size.isValid() && qint64(size.width()) * size.height() >= bigPicturePixels;
}

// The size of a picture file as readImage() turns it, from its header; null
// for an SVG (read on the GUI thread, checked first) or when unknown.
QSize headerSize(const QString &path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (reader.format().startsWith("svg"))
        return {};
    QSize size = reader.size();
    if (size.isValid() && (reader.transformation() & QImageIOHandler::TransformationRotate90))
        size.transpose();
    return size;
}

// Starts reading the latest pictures asked for, as many as may be read at a
// time: each is read and fitted to the column it was asked for at on the
// thread pool; when it is in (with the decoded and the scaled pictures),
// DiagramNotifier::ready tells the views.
void readWaiting()
{
    LaterPictures &later = laterPictures();
    while (later.running < LaterPictures::runningMax && !later.waiting.empty()) {
        const LaterPictures::Request request = later.waiting.back();
        later.waiting.pop_back();
        ++later.running;
        // Made here, on the GUI thread, so the result is delivered there.
        DiagramNotifier *notifier = &DiagramNotifier::instance();
        const int generation = later.generation;
        QThreadPool::globalInstance()->start([request, notifier, generation] {
            QImage fitted, kept;
            bool compact = false;
            qsizetype kib = 0;
            if (laterPictures().generation == generation) {
                const QImage image = readImage(request.path);
                if (!image.isNull()) {
                    fitted = fitToColumn(image, request.width, request.dpr);
                    std::tie(compact, kib) = DecodedPictures::costOf(image);
                    kept = DecodedPictures::compactForm(image);
                }
            }
            QMetaObject::invokeMethod(notifier, [request, fitted, kept, compact, kib, generation]() {
                LaterPictures &later = laterPictures();
                --later.running;
                if (later.generation == generation && later.reading.remove(request.version)) {
                    if (kept.isNull() || !decodedPictures().insertRead(request.version, kept, compact, kib))
                        later.now.insert(request.version);
                    if (!fitted.isNull())
                        scaledLocalImages().insert(request.key, new QImage(fitted), imageCost(fitted));
                    Q_EMIT DiagramNotifier::instance().ready();
                }
                readWaiting();
            });
        });
    }
}

// Asks for a big picture file to be read (once), for a column `width` wide.
void readLater(const QString &path, const QString &version, const QString &key, qreal width, qreal dpr)
{
    LaterPictures &later = laterPictures();
    if (later.reading.contains(version))
        return;
    later.reading.insert(version);
    later.waiting.push_back({path, version, key, width, dpr});
    while (later.waiting.size() > LaterPictures::waitingMax) {
        later.reading.remove(later.waiting.front().version);
        later.waiting.pop_front();
    }
    readWaiting();
}

// Whether a big picture file is to be read later, and starts that (for a
// column `width` wide): when the context allows it, it is not decoded
// already, and it is big.
bool readsLater(const QString &path, const QString &version, const QSize &size, bool later, qreal width, qreal dpr)
{
    if (!later || laterPictures().now.contains(version) || decodedPictures().contains(version) || !isBig(size))
        return false;
    readLater(path, version, scaledKey(version, width, dpr), width, dpr);
    return true;
}

// The size a local picture has (null if unreadable), without keeping or
// asking for the decoded picture when it is known: a figure's size depends
// on it. `decoded` gets the picture if it had to be read, for the caller to
// scale (read once, even if the cache does not keep it). A big one read
// later (see readsLater) has the size its file's header says until it is in.
QCache<QString, QSize> &pictureSizes()
{
    static QCache<QString, QSize> sizes(4096);
    return sizes;
}

QSize localPictureSize(const QString &path, QImage *decoded, bool later = false)
{
    QCache<QString, QSize> &sizes = pictureSizes();
    const QString version = fileVersion(path);
    if (const QSize *known = sizes.object(version))
        return *known;
    // (Read for its size only: started by cachedLocalImage, at the width
    // the figure gets.)
    if (later && laterPictures().reading.contains(version))
        return headerSize(path);
    if (later && !laterPictures().now.contains(version) && !decodedPictures().contains(version)) {
        if (const QSize header = headerSize(path); isBig(header))
            return header;
    }
    *decoded = decodedLocalImage(path);
    if (decoded->isNull())
        return {};
    sizes.insert(version, new QSize(decoded->size()));
    return decoded->size();
}

// Local pictures, read and scaled to the column, by file version and size.
// The decoded files are kept too (see above). SVGs are kept as decoded as
// well: they are read at their own size, never at the column's, so a bigger
// or smaller column scales the same pixels either way. A big picture read
// later (see readsLater; `size` is its own) is blank until it is in, as big
// as it will be, and `waiting` is set.

QImage cachedLocalImage(const QString &path, qreal width, qreal dpr, QImage decoded = {}, bool later = false,
                        const QSize &size = {}, bool *waiting = nullptr)
{
    QCache<QString, QImage> &cache = scaledLocalImages();
    const QString version = fileVersion(path);
    const QString key = scaledKey(version, width, dpr);
    if (const QImage *hit = cache.object(key))
        return *hit;
    if (decoded.isNull() && readsLater(path, version, size, later, width, dpr)) {
        *waiting = true;
        // As fitToColumn() would size it (and QImage::scaledToWidth).
        QSize blankSize = size;
        const bool scaled = size.width() > width;
        if (scaled) {
            const int w = int(width * dpr);
            blankSize = QSize(w, qRound(qreal(w) / size.width() * size.height()));
        }
        QImage blank(blankSize, QImage::Format_ARGB32_Premultiplied);
        blank.fill(Qt::transparent);
        if (scaled)
            blank.setDevicePixelRatio(dpr);
        return blank;
    }
    QImage image = decoded.isNull() ? decodedLocalImage(path) : decoded;
    if (image.isNull())
        return image; // Not cached: the file may appear later.
    image = fitToColumn(image, width, dpr);
    cache.insert(key, new QImage(image), imageCost(image));
    return image;
}

// Downloaded pictures scaled to the column, by address (null until
// downloaded). Their decoded full-size form is RemoteImages' to keep, as
// DecodedPictures keeps the files': decoded once on a worker thread when they
// arrive, in a bounded cache of its own (shared with the zoom viewer).
QCache<QString, QImage> &scaledRemoteImages()
{
    static QCache<QString, QImage> cache(32 * 1024); // KiB
    return cache;
}

QImage cachedRemoteImage(const QUrl &url, qreal width, qreal dpr)
{
    QCache<QString, QImage> &cache = scaledRemoteImages();
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

// Pictures carried in documents (data:) as decoded (null: unreadable) and as
// fitted to a column, by address. Every rebuild of a block (an edit, a new
// width, zoom) asked for them again, and decoding a big one takes long. The
// key is a hash of the address; the address itself is kept (shared with the
// document, not copied) to tell two that hash alike apart. GUI thread only.
struct DataPicture
{
    QString address;
    QImage image;
};

std::atomic<int> dataPictureDecodes{0};

QString dataPictureKey(const QString &address)
{
    return QStringLiteral("%1:%2").arg(address.size()).arg(qHash(address, size_t(0x5eed)));
}

QImage decodeDataAddress(const QString &address); // Below decodePicture().

struct DataPictures
{
    QCache<QString, DataPicture> decoded{64 * 1024}; // KiB
    QCache<QString, DataPicture> fitted{32 * 1024};  // KiB
};

DataPictures &dataPictures()
{
    static DataPictures pictures;
    return pictures;
}

QImage cachedDataPicture(const QString &address, qreal width, qreal dpr)
{
    QCache<QString, DataPicture> &decoded = dataPictures().decoded;
    QCache<QString, DataPicture> &fitted = dataPictures().fitted;
    const auto cost = [&](const QImage &image) { return imageCost(image) + address.size() / 512 + 1; };
    const QString key = dataPictureKey(address);
    const QString sized = QStringLiteral("%1|%2|").arg(width).arg(dpr) + key;
    if (const DataPicture *hit = fitted.object(sized); hit && hit->address == address)
        return hit->image;
    QImage image;
    if (const DataPicture *hit = decoded.object(key); hit && hit->address == address) {
        image = hit->image;
    } else {
        image = decodeDataAddress(address);
        decoded.insert(key, new DataPicture{address, image}, cost(image));
    }
    if (!image.isNull())
        image = fitToColumn(image, width, dpr);
    fitted.insert(sized, new DataPicture{address, image}, cost(image));
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
// Font sizes raw HTML can ask for, in points and pixels. A tiny size
// (0.1pt) makes Qt load every fallback font looking for glyphs, which froze
// the window for seconds; a huge one makes a layout as costly. Real text
// stays well inside.
constexpr qreal minFontPoints = 4;
constexpr qreal maxFontPoints = 200;
constexpr int minFontPixels = 5;
constexpr int maxFontPixels = 270;

// Brings every font size of a document set from HTML inside those bounds.
// Only HTML that sets sizes is walked (the scan is cheap; most blocks never
// mention one).
void clampFontSizes(QTextDocument *doc, const QString &html)
{
    if (!html.contains(QLatin1String("size"), Qt::CaseInsensitive)
        && !html.contains(QLatin1String("<big"), Qt::CaseInsensitive)
        && !html.contains(QLatin1String("<small"), Qt::CaseInsensitive))
        return;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat format = fragment.charFormat();
            QTextCharFormat fix;
            if (format.hasProperty(QTextFormat::FontPointSize)) {
                const qreal points = format.fontPointSize();
                if (points < minFontPoints || points > maxFontPoints)
                    fix.setFontPointSize(qBound(minFontPoints, points, maxFontPoints));
            }
            if (format.hasProperty(QTextFormat::FontPixelSize)) {
                const int pixels = format.intProperty(QTextFormat::FontPixelSize);
                if (pixels < minFontPixels || pixels > maxFontPixels)
                    fix.setProperty(QTextFormat::FontPixelSize, qBound(minFontPixels, pixels, maxFontPixels));
            }
            if (fix.propertyCount() == 0)
                continue;
            QTextCursor cursor(doc);
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(fix);
        }
    }
}

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

    // The figures (pictures read from files or downloaded; not formulas)
    // take the size the box and the user give them (fitFigure), and the
    // picture shown is made for that size, so that it is sharp. An HTML
    // width or height counts as the picture's own size. A picture not
    // loaded yet (a download in progress, a file not read) is left for
    // loadResource().
    void sizeFigures(const QList<qreal> &factors)
    {
        struct Spot
        {
            int position;
            int length;
            QTextImageFormat format;
        };
        std::vector<Spot> spots;
        for (QTextBlock block = begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.charFormat().isImageFormat())
                    continue;
                const QTextImageFormat format = fragment.charFormat().toImageFormat();
                if (!format.name().startsWith(QLatin1String("vn-")) && allowedPictureName(format.name()))
                    spots.push_back({fragment.position(), fragment.length(), format});
            }
        }
        const qreal dpr = m_context.devicePixelRatio;
        for (size_t index = 0; index < spots.size(); ++index) {
            QTextImageFormat format = spots[index].format;
            const QString name = format.name();
            const QUrl url(name);
            QSizeF own;
            QString path;
            QImage decoded; // If reading it was needed to know its size.
            if (RemoteImages::isWebUrl(url)) {
                RemoteImages &remote = RemoteImages::instance();
                if (!m_context.allowRemoteImages || remote.state(url) != RemoteImages::State::Loaded)
                    continue;
                own = remote.fullImage(url).size();
                if (const QString address = url.toString();
                    !m_remote.contains(address) && RemoteImages::redirectAllowed(QUrl(), url))
                    m_remote.append(address);
            } else {
                path = localPath(url);
                if (path.isEmpty())
                    continue;
                own = localPictureSize(path, &decoded, m_context.picturesLater);
            }
            const QSize natural = own.toSize(); // The picture's own, before the HTML's.
            if (own.isEmpty())
                continue;
            const qreal aspect = own.height() / own.width();
            if (format.width() > 0 && format.height() > 0)
                own = QSizeF(format.width(), format.height());
            else if (format.width() > 0)
                own = QSizeF(format.width(), format.width() * aspect);
            else if (format.height() > 0)
                own = QSizeF(format.height() / aspect, format.height());
            const FigureFit fit = fitFigure(own, m_context, int(index) < factors.size() ? factors[int(index)] : 1);
            const qreal width = qMax<qreal>(1, fit.size.width());
            bool waiting = false;
            const QImage image = path.isEmpty() ? cachedRemoteImage(url, width, dpr)
                                                : cachedLocalImage(path, width, dpr, decoded, m_context.picturesLater,
                                                                   natural, &waiting);
            m_pending = m_pending || waiting;
            if (image.isNull())
                continue;
            // The picture's own pixels, unless it is made for another size
            // (a picture smaller than the figure is drawn enlarged).
            QSizeF shown = image.size() / image.devicePixelRatio();
            if (qAbs(shown.width() - fit.size.width()) > 1.5)
                shown = fit.size;
            const QString fresh = QStringLiteral("vn-figure:%1").arg(m_figureNames.size());
            m_figureNames.insert(fresh, name);
            m_pictureBytes += image.sizeInBytes();
            addResource(QTextDocument::ImageResource, QUrl(fresh), image);
            format.setName(fresh);
            format.setWidth(shown.width());
            format.setHeight(shown.height());
            QTextCursor cursor(this);
            cursor.setPosition(spots[index].position);
            cursor.setPosition(spots[index].position + spots[index].length, QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(format);
            RenderedBlock::Figure figure;
            figure.index = int(index);
            figure.rect.setSize(shown);
            figure.percent = qRound(100 * shown.width() / own.width());
            figure.factor = fit.factor;
            figure.maxFactor = fit.maxFactor;
            figure.fittedWidth = fit.fittedWidth;
            m_figures.push_back({spots[index].position, figure});
        }
        // A line height of N% makes the line of a figure N% as high as the
        // figure (QTextBlockFormat::lineHeight), far more than the box it was
        // fitted to. As for formulas (placeMath), a paragraph with figures
        // spaces its lines by its text's height instead.
        QTextCursor cursor(this);
        QTextBlock last;
        for (const auto &[position, figure] : m_figures) {
            const QTextBlock block = findBlock(position);
            if (block == last)
                continue;
            last = block;
            QTextBlockFormat format = block.blockFormat();
            if (format.lineHeightType() != QTextBlockFormat::ProportionalHeight)
                continue;
            format.setLineHeight(lineGap(block), QTextBlockFormat::LineDistanceHeight);
            cursor.setPosition(block.position());
            cursor.setBlockFormat(format);
        }
    }

    // The figures sizeFigures() sized, with where each is in the text.
    const std::vector<std::pair<int, RenderedBlock::Figure>> &figures() const { return m_figures; }

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

    // Pictures carried in the document (data:) are named "vn-data:N"
    // instead: Qt looks a picture up by its name at every paint (parsing it
    // as an address and comparing it with the ones it has), and a data
    // address is the whole picture, often megabytes. Run before
    // dropNetworkImages(), which would read each address again (a data
    // address never names another machine, and it is allowed when valid);
    // such a name written in the text is blocked here, as it was there.
    void nameDataPictures()
    {
        QHash<QString, int> numbers;
        const bool undo = isUndoRedoEnabled();
        setUndoRedoEnabled(false);
        for (QTextBlock block = begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.charFormat().isImageFormat())
                    continue;
                QTextImageFormat format = fragment.charFormat().toImageFormat();
                const QString name = format.name();
                if (name.startsWith(QLatin1String("vn-data:"))) {
                    format.setName(QStringLiteral("vn-blocked:"));
                } else if (name.startsWith(QLatin1String("data:"), Qt::CaseInsensitive)) {
                    // As QUrl reads the scheme (loadResource takes the rest).
                    auto number = numbers.find(name);
                    if (number == numbers.end()) {
                        if (!QUrl(name).isValid())
                            continue; // Blocked by dropNetworkImages().
                        number = numbers.insert(name, int(m_dataNames.size()));
                        m_dataNames.append(name);
                    }
                    format.setName(QStringLiteral("vn-data:%1").arg(*number));
                } else {
                    continue;
                }
                QTextCursor cursor(this);
                cursor.setPosition(fragment.position());
                cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
                cursor.mergeCharFormat(format);
            }
        }
        setUndoRedoEnabled(undo);
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
        const QString name = m_figureNames.value(source, source);
        return name.startsWith(QLatin1String("vn-math")) ? QString() : localPath(QUrl(name));
    }

    // The address of a picture that was downloaded for this block, or empty.
    QUrl downloadedUrlOf(const QString &source) const
    {
        const QUrl url(m_figureNames.value(source, source));
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
        // A picture carried in the document, by the short name
        // nameDataPictures() gave it.
        if (const QString name = url.toString(); name.startsWith(QLatin1String("vn-data:")))
            return keep(type, url, dataPicture(m_dataNames.value(name.mid(8).toInt())));
        // From here on no picture is ever declined: declined, Qt's image
        // handler opens the name itself, relative to the working folder and
        // past the SVG check (a name that differs once decoded, a picture
        // missing beside the document but present where the app started).
        // What cannot be shown is shown broken, as Qt would show it.
        if (url.scheme() == QLatin1String("data"))
            return keep(type, url, dataPicture(url.toString(QUrl::FullyEncoded)));
        const QString path = localPath(url);
        if (path.isEmpty())
            return keep(type, url, brokenPicture());
        const QImage image = cachedLocalImage(path, m_context.width, m_context.devicePixelRatio);
        return keep(type, url, image.isNull() ? brokenPicture() : image);
    }

    // Whether Qt may be given a picture name: those made here (math, the
    // blocked stand-in), web addresses (asked for only with consent), data
    // URLs, and files localPath() accepts. Anything else, invalid URLs above
    // all (Qt's image handler opens their text as a file name), is not.
    bool allowedPictureName(const QString &name) const
    {
        static const QRegularExpression internal(QStringLiteral("^(vn-math(-display)?:\\d+|vn-data:\\d+|vn-blocked:)$"));
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

    // A picture carried in the document (data:), decoded as files are (an
    // SVG naming another machine refused; see decodeDataAddress) and fitted
    // to the column; broken if it cannot be read.
    QImage dataPicture(const QString &address) const
    {
        const QImage image = cachedDataPicture(address, m_context.width, m_context.devicePixelRatio);
        return image.isNull() ? brokenPicture() : image;
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
        // Never declined (see loadResource): missing is shown broken. Not
        // kept, so a later render asks again.
        if (!m_context.allowRemoteImages)
            return brokenPicture();
        RemoteImages &remote = RemoteImages::instance();
        switch (remote.request(url)) {
        case RemoteImages::State::Loading:
            m_pending = true;
            return brokenPicture();
        case RemoteImages::State::Loaded: {
            const QImage image = cachedRemoteImage(url, m_context.width, m_context.devicePixelRatio);
            return image.isNull() ? QVariant(brokenPicture()) : keep(type, url, image);
        }
        default:
            return brokenPicture();
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
    QStringList m_dataNames; // The addresses of "vn-data:N", by N.
    // The names sizeFigures() gave pictures, with the names they had.
    QHash<QString, QString> m_figureNames;
    std::vector<std::pair<int, RenderedBlock::Figure>> m_figures;
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
std::atomic<bool> ownTableGrid{true};

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

// The one table a document of rich text is (RenderedBlock::tableCells), or
// null: its root frame holds the table directly and nothing else but the
// empty paragraphs around it.
QTextTable *onlyTable(const QTextDocument &doc)
{
    QTextTable *table = nullptr;
    for (QTextFrame *frame : doc.rootFrame()->childFrames()) {
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
    for (QTextBlock b = doc.begin(); b.isValid() && b.position() < table->firstPosition(); b = b.next()) {
        if (!b.text().isEmpty())
            return nullptr;
    }
    for (QTextBlock b = doc.findBlock(table->lastPosition() + 1); b.isValid(); b = b.next()) {
        if (!b.text().isEmpty())
            return nullptr;
    }
    return table;
}

// Lays the one table of `doc` out in a column `width` wide the way browsers
// lay out an automatic table (CSS 2.2, 17.5.2.2). Each column has a least
// width (its longest word, a formula or a picture, which cannot wrap) and a
// widest (its text on one line). The table takes its widest, if that fits the
// column. If not, but the least widths do, each column gets its least plus a
// share of what is left in proportion to how much wider it would like to
// be, so that long text wraps and short columns stay as they are. Returns 0
// when the table fits, else its widest width, laid out so (the caller scrolls
// it). Cells that span columns or rows have no width of their own: such a
// table is left to Qt, which squeezes its widest columns first.
qreal fitTable(QTextDocument &doc, QTextTable &table, qreal width)
{
    // Words wrap at their ends only (Qt's default breaks them anywhere, so
    // its narrowest layout would not tell the longest word).
    QTextOption option = doc.defaultTextOption();
    option.setWrapMode(QTextOption::WordWrap);
    doc.setDefaultTextOption(option);

    // A table laid out by TableLayout tells what its widest layout would
    // be without being laid out so, from what it keeps of its cells.
    TableLayout *own = qobject_cast<TableLayout *>(doc.documentLayout());
    TableLayout::Columns widestColumns;
    if (own)
        widestColumns = own->columnsAt(unboundedWidth);
    else
        doc.setTextWidth(unboundedWidth);
    const qreal ideal = own ? widestColumns.idealWidth : doc.idealWidth();
    const qreal widest = std::ceil(ideal);
    if (widest <= width) {
        doc.setTextWidth(width);
        return 0;
    }
    const int columns = table.columns();
    bool plain = columns > 0;
    for (int row = 0; plain && row < table.rows(); ++row) {
        for (int col = 0; col < columns; ++col) {
            const QTextTableCell cell = table.cellAt(row, col);
            if (cell.columnSpan() != 1 || cell.rowSpan() != 1)
                plain = false;
        }
    }
    // The text of each column (as wide in every row): from the first row.
    QAbstractTextDocumentLayout *layout = doc.documentLayout();
    const auto texts = [&] {
        QList<QRectF> rects;
        for (int col = 0; col < columns; ++col) {
            if (own)
                rects.push_back(QRectF(widestColumns.textLeft[size_t(col)], 0, widestColumns.textWidth[size_t(col)], 0));
            else
                rects.push_back(layout->blockBoundingRect(table.cellAt(0, col).firstCursorPosition().block()));
        }
        return rects;
    };
    // The widest layout gives each column its widest width (its text on
    // one line).
    const QList<QRectF> most = plain ? texts() : QList<QRectF>();
    // What a column takes around its text: the padding and border between
    // one column's text and the next one's.
    const qreal around = columns > 1 && plain ? most[1].left() - most[0].right() : 2 * table.format().cellPadding() + 1;
    // The narrowest layout gives each column its least width: Qt makes it
    // the widest of its cells' longest unbreakable pieces (their
    // QTextLayout::minimumWidth, the same at any width) and their padding,
    // in whole pixels (at least one), in its fixed point (1/64 px). Those
    // are known from the widest layout, unless a paragraph in a cell has
    // margins of its own or a cell holds a frame (then the table is laid out
    // narrowest to see).
    QList<qreal> least;
    bool known = plain;
    // Formulas are objects in the text, each in a frame of its own that Qt
    // counts in its cell's least width as well.
    QHash<int, int> objects; // Widest object by cell (row * columns + column), in 1/64 px.
    for (QTextFrame *frame : table.childFrames()) {
        if (frame->firstPosition() <= frame->lastPosition()) {
            known = false; // A frame in the text (a table in a table).
            break;
        }
        const QTextTableCell cell = table.cellAt(frame->firstPosition());
        int &widest = objects[cell.row() * columns + cell.column()];
        widest = qMax(widest, int(layout->frameBoundingRect(frame).width() * 64));
    }
    for (int col = 0; known && col < columns; ++col) {
        int columnMin = 64;
        for (int row = 0; row < table.rows() && known; ++row) {
            const QTextTableCell cell = table.cellAt(row, col);
            int cellMin = objects.value(row * columns + col);
            // (TableLayout's cells have no margins, lists or objects.)
            if (own)
                cellMin = own->leastWidth(row, col);
            for (QTextBlock b = own ? QTextBlock() : cell.firstCursorPosition().block();
                 b.isValid() && b.position() <= cell.lastPosition(); b = b.next()) {
                const QTextBlockFormat format = b.blockFormat();
                if (b.textList() || format.leftMargin() != 0 || format.rightMargin() != 0 || format.indent() != 0
                    || format.textIndent() != 0) {
                    known = false;
                    break;
                }
                cellMin = qMax(cellMin, int(b.layout()->minimumWidth() * 64));
            }
            const int padded = cellMin + int(around * 64);
            columnMin = qMax(columnMin, (padded + 63) & -64);
        }
        least.push_back(columnMin / 64.0 - around);
    }
    qreal narrowest = 0;
    if (known) {
        // The table as wide as its columns' least widths, and as much around
        // them as now.
        qreal mostSum = 0, leastSum = 0;
        for (int col = 0; col < columns; ++col) {
            mostSum += most[col].width();
            leastSum += least[col];
        }
        narrowest = std::ceil(ideal - mostSum + leastSum);
    } else {
        // Every column as narrow as it goes.
        doc.setTextWidth(1);
        narrowest = std::ceil(doc.idealWidth());
        least.clear();
        if (plain) {
            for (const QRectF &text : texts())
                least.push_back(text.width());
        }
    }
    if (narrowest > width)
        return widest;
    if (!plain) {
        // Cells that span columns or rows: left to Qt.
        doc.setTextWidth(width);
        return 0;
    }
    qreal wanted = 0;
    for (int col = 0; col < columns; ++col)
        wanted += most[col].width() - least[col];
    // What is left of the column past the narrowest layout, in Qt's fixed
    // point (1/64 px; the shares rounded down so that they never add up to more).
    const qreal spare = std::floor((width - narrowest) * 64) / 64;
    QList<QTextLength> lengths;
    for (int col = 0; col < columns; ++col) {
        const qreal share = wanted > 0 ? std::floor(spare * (most[col].width() - least[col]) / wanted * 64) / 64 : 0;
        lengths.push_back(QTextLength(QTextLength::FixedLength, least[col] + around + share));
    }
    QTextTableFormat format = table.format();
    format.setColumnWidthConstraints(lengths);
    table.setFormat(format);
    doc.setTextWidth(width);
    return 0;
}

// Qt's rules for the edges of a table with collapsed borders
// (QTextDocumentLayout, qtextdocumentlayout.cpp: cellEdgeData, axisEdgeData,
// checkJoinedEdge, prioritizedEdgeAnchorOffset, drawCellBorder), for a table
// of single cells whose every edge is a 1 px line: where each edge of a cell
// is drawn, to draw it there. The names and steps are Qt's.
namespace grid {

enum Edge { TopEdge, RightEdge, BottomEdge, LeftEdge, NumEdges }; // QCss::Edge
enum EdgeClass { ClassInvalid, ClassNone, ClassGrid, ClassTableBorder, ClassExplicit };

struct Cell
{
    int row = -1;
    int column = -1;
    bool isValid() const { return row >= 0; }
};

struct EdgeData
{
    qreal width = 0;
    Cell cell;
    Edge edge = NumEdges;
    EdgeClass edgeClass = ClassInvalid;
    bool operator<(const EdgeData &other) const
    {
        if (width < other.width)
            return true;
        if (width > other.width)
            return false;
        if (edgeClass < other.edgeClass)
            return true;
        if (edgeClass > other.edgeClass)
            return false;
        if (edge == TopEdge && other.edge == BottomEdge)
            return true;
        if (edge == BottomEdge && other.edge == TopEdge)
            return false;
        if (edge == LeftEdge && other.edge == RightEdge)
            return true;
        return false;
    }
    bool operator>(const EdgeData &other) const { return other < *this; }
};

EdgeData larger(const EdgeData &a, const EdgeData &b) // qMax
{
    return a < b ? b : a;
}

struct Shape
{
    int rows = 0;
    int columns = 0;

    Cell adjacentCell(const Cell &cell, Edge edge) const
    {
        int dr = 0, dc = 0;
        switch (edge) {
        case LeftEdge: dc = -1; break;
        case RightEdge: dc = 1; break;
        case TopEdge: dr = -1; break;
        default: dr = 1; break;
        }
        const int row = cell.row + dr, column = cell.column + dc;
        if (column < 0 || row < 0 || column >= columns || row >= rows)
            return {};
        return {row, column};
    }

    // Every cell sets a 1 px border on every edge.
    static EdgeData cellEdgeData(const Cell &cell, Edge edge)
    {
        if (!cell.isValid())
            return {};
        return {1, cell, edge, ClassExplicit};
    }

    static Edge adjacentEdge(Edge edge)
    {
        switch (edge) {
        case TopEdge: return BottomEdge;
        case RightEdge: return LeftEdge;
        case BottomEdge: return TopEdge;
        default: return RightEdge;
        }
    }

    static bool isSameAxis(Edge e1, Edge e2) { return e1 == e2 || e1 == adjacentEdge(e2); }
    static bool isVerticalAxis(Edge e) { return e % 2 > 0; }

    static bool sharesAxis(const Cell &cell, Edge edge, const Cell &competingCell, Edge competingCellEdge)
    {
        switch (edge) {
        case TopEdge: return cell.row == competingCell.row + (competingCellEdge == BottomEdge ? 1 : 0);
        case BottomEdge: return cell.row + 1 == competingCell.row + (competingCellEdge == TopEdge ? 0 : 1);
        case LeftEdge: return cell.column == competingCell.column + (competingCellEdge == RightEdge ? 1 : 0);
        default: return cell.column + 1 == competingCell.column + (competingCellEdge == LeftEdge ? 0 : 1);
        }
    }

    EdgeData axisEdgeData(const Cell &cell, Edge edge) const
    {
        EdgeData result = cellEdgeData(cell, edge);
        const Cell ac = adjacentCell(cell, edge);
        result = larger(result, cellEdgeData(ac, adjacentEdge(edge)));
        bool mustCheckThirdCell = false;
        if (ac.isValid()) {
            if (edge == TopEdge || edge == BottomEdge)
                mustCheckThirdCell = !sharesAxis(cell, LeftEdge, ac, LeftEdge);
            else
                mustCheckThirdCell = !sharesAxis(cell, TopEdge, ac, TopEdge);
        }
        if (mustCheckThirdCell)
            result = larger(result, cellEdgeData(adjacentCell(ac, adjacentEdge(edge)), edge));
        return result;
    }

    void checkJoinedEdge(const Cell &cell, Edge competingEdge, const EdgeData &edgeData, bool couldHaveContinuation,
                         EdgeData *maxCompetingEdgeData, EdgeData *maxOrthogonalEdgeData) const
    {
        const EdgeData competingEdgeData = axisEdgeData(cell, competingEdge);
        if (competingEdgeData > edgeData) {
            *maxCompetingEdgeData = competingEdgeData;
        } else if (competingEdgeData.width == edgeData.width) {
            if ((isSameAxis(edgeData.edge, competingEdge) && couldHaveContinuation)
                || (!isVerticalAxis(edgeData.edge) && isVerticalAxis(competingEdge)))
                *maxCompetingEdgeData = competingEdgeData;
        }
        if (maxOrthogonalEdgeData && competingEdgeData.width > maxOrthogonalEdgeData->width)
            *maxOrthogonalEdgeData = competingEdgeData;
    }

    qreal prioritizedEdgeAnchorOffset(const Cell &cell, const EdgeData &edgeData, Edge orthogonalEdge,
                                      bool couldHaveContinuation) const
    {
        EdgeData maxCompetingEdgeData;
        EdgeData maxOrthogonalEdgeData;
        checkJoinedEdge(cell, orthogonalEdge, edgeData, false, &maxCompetingEdgeData, &maxOrthogonalEdgeData);
        Cell competingCell = adjacentCell(cell, orthogonalEdge);
        if (competingCell.isValid())
            checkJoinedEdge(competingCell, edgeData.edge, edgeData, couldHaveContinuation, &maxCompetingEdgeData, nullptr);
        competingCell = adjacentCell(cell, edgeData.edge);
        if (competingCell.isValid() && sharesAxis(cell, orthogonalEdge, competingCell, orthogonalEdge))
            checkJoinedEdge(competingCell, orthogonalEdge, edgeData, false, &maxCompetingEdgeData, &maxOrthogonalEdgeData);
        const bool hasPriority = edgeData > maxCompetingEdgeData;
        const qreal offset = maxOrthogonalEdgeData.width / 2.0; // collapseOffset
        return hasPriority ? -offset : offset;
    }

    // The rectangle drawCellBorder fills for an edge of a cell drawn in
    // `borderRect` (the cell's rectangle in whole pixels).
    QRectF edgeRect(const Cell &cell, const QRectF &borderRect, Edge edge) const
    {
        QPointF p1, p2;
        qreal wh = 0, wv = 0;
        const EdgeData edgeData = axisEdgeData(cell, edge);
        switch (edge) {
        case TopEdge:
            wv = edgeData.width;
            p1 = borderRect.topLeft() + QPointF(qFloor(prioritizedEdgeAnchorOffset(cell, edgeData, LeftEdge, false)), 0);
            p2 = borderRect.topRight() + QPointF(-qCeil(prioritizedEdgeAnchorOffset(cell, edgeData, RightEdge, true)), 0);
            break;
        case BottomEdge:
            wv = edgeData.width;
            p1 = borderRect.bottomLeft() + QPointF(qFloor(prioritizedEdgeAnchorOffset(cell, edgeData, LeftEdge, false)), -wv);
            p2 = borderRect.bottomRight() + QPointF(-qCeil(prioritizedEdgeAnchorOffset(cell, edgeData, RightEdge, true)), -wv);
            break;
        case LeftEdge:
            wh = edgeData.width;
            p1 = borderRect.topLeft() + QPointF(0, qFloor(prioritizedEdgeAnchorOffset(cell, edgeData, TopEdge, false)));
            p2 = borderRect.bottomLeft() + QPointF(0, -qCeil(prioritizedEdgeAnchorOffset(cell, edgeData, BottomEdge, true)));
            break;
        default:
            wh = edgeData.width;
            p1 = borderRect.topRight() + QPointF(-wh, qFloor(prioritizedEdgeAnchorOffset(cell, edgeData, TopEdge, false)));
            p2 = borderRect.bottomRight() + QPointF(-wh, -qCeil(prioritizedEdgeAnchorOffset(cell, edgeData, BottomEdge, true)));
            break;
        }
        // Half the edge out of the cell, to share the place of the next
        // cell's (in whole pixels).
        const qreal half = edgeData.width / 2.0;
        QPointF offset;
        switch (edge) {
        case TopEdge: offset = QPointF(0, -qCeil(half)); break;
        case BottomEdge: offset = QPointF(0, qFloor(half)); break;
        case LeftEdge: offset = QPointF(-qCeil(half), 0); break;
        default: offset = QPointF(qFloor(half), 0); break;
        }
        p1 += offset;
        p2 += offset;
        return QRectF(p1.x(), p1.y(), p2.x() + wh - p1.x(), p2.y() + wv - p1.y());
    }
};

} // namespace grid

// Whether a document of rich text holds a picture (fitted to the column
// when laid out) or a background picture, or another object in its text.
bool asksForPictures(const QTextDocument &doc)
{
    const auto background = [](const QTextFormat &format) { return format.hasProperty(QTextFormat::BackgroundImageUrl); };
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        if (background(block.blockFormat()))
            return true;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat format = it.fragment().charFormat();
            if (format.isImageFormat() || format.objectType() >= QTextFormat::UserObject || background(format))
                return true;
        }
    }
    QList<QTextFrame *> frames = {doc.rootFrame()};
    while (!frames.isEmpty()) {
        QTextFrame *frame = frames.takeLast();
        if (background(frame->frameFormat()))
            return true;
        if (auto *table = qobject_cast<QTextTable *>(frame)) {
            for (int row = 0; row < table->rows(); ++row) {
                for (int col = 0; col < table->columns(); ++col) {
                    if (background(table->cellAt(row, col).format()))
                        return true;
                }
            }
        }
        frames += frame->childFrames();
    }
    return false;
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

namespace {

// A data: address as loadResource() gets it: the picture in it, decoded as a
// file is (decodePicture), or null.
QImage decodeDataAddress(const QString &address)
{
    ++dataPictureDecodes;
    // Big enough for any picture worth embedding in a text file.
    constexpr qsizetype maxEncoded = 32 * 1024 * 1024;
    const QByteArray encoded = QUrl(address).toEncoded(QUrl::FullyEncoded);
    const qsizetype comma = encoded.indexOf(',');
    if (comma < 0 || encoded.size() > maxEncoded)
        return {};
    const QByteArray header = encoded.mid(5, comma - 5).toLower(); // After "data:".
    const QByteArray body = QByteArray::fromPercentEncoding(encoded.mid(comma + 1));
    const QByteArray data = header.endsWith(";base64") ? QByteArray::fromBase64(body) : body;
    return decodePicture(data);
}

} // namespace

int RenderedBlock::dataPictureDecodes()
{
    return vn::dataPictureDecodes;
}

void RenderedBlock::releasePictureCaches()
{
    decodedPictures().clear();
    pictureSizes().clear();
    scaledLocalImages().clear();
    scaledRemoteImages().clear();
    dataPictures().decoded.clear();
    dataPictures().fitted.clear();
    LaterPictures &later = laterPictures();
    ++later.generation; // (What is being read is dropped when it is in.)
    later.waiting.clear();
    later.reading.clear();
    later.now.clear();
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
    r.m_sourceLines = int(source.count(u'\n')) + 1;
    r.collectLineAnchors(r.m_sourceLines);
    r.m_reflows = true;
    return result;
}

// A table of single cells framed by 1 px lines of one colour, as pipe tables
// and front matter are: Qt draws those lines slowly (for each edge of each
// cell it looks its neighbours up by position, and it does so again to lay
// the table out), so the block draws them, and the cells' backgrounds under
// them, itself, exactly as Qt would (see grid above). Qt is left a table that
// takes the same room and draws nothing but the text: a collapsed border of
// 1 px is half a pixel in each cell next to it, so the cells get that as
// padding, and the table's outermost half pixels (each rounded to one) as
// margins; with no border to collapse, the table's borders are kept apart,
// which Qt handles without the look-ups. Anything else (merged cells, other
// borders, cells aligned to the middle or bottom, something drawn under the
// table, fractional padding) keeps Qt's borders.
struct RenderedBlock::OwnGrid
{
    QTextTable *table = nullptr;
    QBrush line;
    std::vector<QBrush> backgrounds; // By row, then column; NoBrush: none.
    qreal padLeft = 0, padTop = 0, padRight = 0; // Of every cell, with the half pixel.
    qreal padding = 0, leftMargin = 0, topMargin = 0, rightMargin = 0, bottomMargin = 0; // The table's, in Qt's whole pixels.
    // Where Qt has put the cells, worked out for a layout (see place()).
    struct Place
    {
        QSizeF size;
        qreal textWidth = -1;
        QPointF offset; // The table's top-left as Qt draws it (whole pixels).
        std::vector<qreal> xs, ys; // Cell edges from it: columns + 1, rows + 1.
        std::vector<int> fixedYs;  // The tops of the rows in Qt's 1/64 px.
        bool overflows = false;    // Some text is wider than its cell.
    };
    mutable Place placed;

    static bool exactPixels(qreal v) { return v * 64 == std::floor(v * 64); }
    static qreal qtPixels(qreal v) { return std::floor((std::floor(v * 64) + 32) / 64); } // QFixed::fromReal().round()

    static std::shared_ptr<OwnGrid> take(QTextDocument &doc, QTextTable &table)
    {
        if (!ownTableGrid)
            return nullptr;
        QTextTableFormat format = table.format();
        const int rows = table.rows(), columns = table.columns();
        if (rows <= 0 || columns <= 0 || !format.borderCollapse() || format.border() != 0 || format.cellSpacing() != 0
            || !table.childFrames().isEmpty() || format.background().style() != Qt::NoBrush
            || format.leftMargin() < 0 || format.topMargin() < 0 || format.rightMargin() < 0 || format.bottomMargin() < 0
            || doc.rootFrame()->frameFormat().background().style() != Qt::NoBrush)
            return nullptr;
        // Nothing drawn under the table (Qt draws the paragraphs before it
        // first), and nothing but selections drawn of those paragraphs
        // (see paint()).
        for (QTextBlock b = doc.begin(); b.isValid(); b = b.next()) {
            if (QTextCursor(b).currentTable() == &table)
                continue;
            if (b.blockFormat().background().style() != Qt::NoBrush)
                return nullptr;
            if (b.position() < table.firstPosition()
                && (b.textList() || b.blockFormat().hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth)))
                return nullptr;
        }
        static const int borders[4][3] = {
            {QTextFormat::TableCellTopBorder, QTextFormat::TableCellTopBorderStyle, QTextFormat::TableCellTopBorderBrush},
            {QTextFormat::TableCellRightBorder, QTextFormat::TableCellRightBorderStyle, QTextFormat::TableCellRightBorderBrush},
            {QTextFormat::TableCellBottomBorder, QTextFormat::TableCellBottomBorderStyle, QTextFormat::TableCellBottomBorderBrush},
            {QTextFormat::TableCellLeftBorder, QTextFormat::TableCellLeftBorderStyle, QTextFormat::TableCellLeftBorderBrush}};
        static const int paddings[4] = {QTextFormat::TableCellTopPadding, QTextFormat::TableCellRightPadding,
                                        QTextFormat::TableCellBottomPadding, QTextFormat::TableCellLeftPadding};
        auto grid = std::make_shared<OwnGrid>();
        grid->table = &table;
        qreal pad[4] = {-1, -1, -1, -1};
        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < columns; ++col) {
                const QTextTableCell cell = table.cellAt(row, col);
                const QTextTableCellFormat f = cell.format().toTableCellFormat();
                if (cell.rowSpan() != 1 || cell.columnSpan() != 1 || f.verticalAlignment() == QTextCharFormat::AlignMiddle
                    || f.verticalAlignment() == QTextCharFormat::AlignBottom)
                    return nullptr;
                for (int edge = 0; edge < 4; ++edge) {
                    const QBrush brush = qvariant_cast<QBrush>(f.property(borders[edge][2]));
                    if (!f.hasProperty(borders[edge][1]) || f.intProperty(borders[edge][1]) != QTextFrameFormat::BorderStyle_Solid
                        || !f.hasProperty(borders[edge][0]) || f.doubleProperty(borders[edge][0]) != 1
                        || brush.style() != Qt::SolidPattern || (grid->line.style() != Qt::NoBrush && brush != grid->line))
                        return nullptr;
                    grid->line = brush;
                    const qreal p = f.hasProperty(paddings[edge]) ? f.doubleProperty(paddings[edge]) : format.cellPadding();
                    if (!exactPixels(p) || p < 0 || (pad[edge] >= 0 && pad[edge] != p))
                        return nullptr;
                    pad[edge] = p;
                }
                const QBrush background = f.background();
                if (background.style() != Qt::NoBrush && background.style() != Qt::SolidPattern)
                    return nullptr;
                grid->backgrounds.push_back(background);
            }
        }
        // The same layout without the borders.
        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < columns; ++col) {
                QTextTableCell cell = table.cellAt(row, col);
                QTextTableCellFormat f = cell.format().toTableCellFormat();
                for (int edge = 0; edge < 4; ++edge) {
                    for (const int property : borders[edge])
                        f.clearProperty(property);
                    f.setProperty(paddings[edge], pad[edge] + 0.5);
                }
                f.clearProperty(QTextFormat::BackgroundBrush);
                cell.setFormat(f);
            }
        }
        format.setBorderCollapse(false);
        format.setLeftMargin(format.leftMargin() + 1);
        format.setTopMargin(format.topMargin() + 1);
        format.setRightMargin(format.rightMargin() + 1);
        format.setBottomMargin(format.bottomMargin() + 1);
        table.setFormat(format);
        grid->padTop = pad[0] + 0.5;
        grid->padRight = pad[1] + 0.5;
        grid->padLeft = pad[3] + 0.5;
        grid->padding = qtPixels(format.padding());
        grid->leftMargin = qtPixels(format.leftMargin());
        grid->topMargin = qtPixels(format.topMargin());
        grid->rightMargin = qtPixels(format.rightMargin());
        grid->bottomMargin = qtPixels(format.bottomMargin());
        return grid;
    }

    // Where the cells are in the document's present layout: from where Qt
    // put each column's and each row's text (the cells' padding before it),
    // and the table's size (its padding and margins after the last cell).
    const Place &place(QTextDocument &doc) const
    {
        QAbstractTextDocumentLayout *layout = doc.documentLayout();
        const QSizeF size = doc.size(); // (Lays it out.)
        if (placed.size == size && placed.textWidth == doc.textWidth() && !placed.xs.empty())
            return placed;
        placed = {};
        placed.size = size;
        placed.textWidth = doc.textWidth();
        const int rows = table->rows(), columns = table->columns();
        // A cell's position in the table, plus the table's in the document.
        const auto origin = [&](int row, int col) {
            const QTextBlock b = table->cellAt(row, col).firstCursorPosition().block();
            return layout->blockBoundingRect(b).topLeft() - b.layout()->position();
        };
        const QPointF first = origin(0, 0);
        const qreal column0 = leftMargin + padding, row0 = topMargin + padding;
        const QPointF at = first - QPointF(column0 + padLeft, row0 + padTop);
        placed.offset = QPointF(at.toPoint());
        for (int col = 0; col < columns; ++col)
            placed.xs.push_back(column0 + origin(0, col).x() - first.x());
        for (int row = 0; row < rows; ++row)
            placed.ys.push_back(row0 + origin(row, 0).y() - first.y());
        const QSizeF frame = layout->frameBoundingRect(table).size();
        placed.xs.push_back(frame.width() - 2 * padding - rightMargin);
        placed.ys.push_back(frame.height() - 2 * padding - bottomMargin);
        for (const qreal y : placed.ys)
            placed.fixedYs.push_back(qRound(y * 64));
        // A line wider than its cell (a word or a picture that does not fit
        // a fixed column) runs over the edge, and Qt draws the next cell
        // over it.
        for (int row = 0; row < rows && !placed.overflows; ++row) {
            for (int col = 0; col < columns && !placed.overflows; ++col) {
                const QTextTableCell cell = table->cellAt(row, col);
                const qreal left = at.x() + placed.xs[col] + padLeft - 0.5;
                const qreal right = at.x() + placed.xs[col + 1] - padRight + 0.5;
                for (QTextBlock b = cell.firstCursorPosition().block();
                     b.isValid() && b.position() <= cell.lastPosition() && !placed.overflows; b = b.next()) {
                    const QTextLayout *lines = b.layout();
                    const qreal x = layout->blockBoundingRect(b).left();
                    for (int i = 0; i < lines->lineCount(); ++i) {
                        const QTextLine line = lines->lineAt(i);
                        const QRectF text = line.naturalTextRect();
                        if (x + text.left() < left || x + text.right() > right)
                            placed.overflows = true;
                    }
                }
            }
        }
        return placed;
    }

    // The document with the table as QTextDocumentLayout draws it (drawFrame,
    // drawTableCell): cell by cell, the background, the edges, the text.
    // Without a selection no text reaches into another cell or onto an
    // edge, so the backgrounds and edges go first and Qt draws all the text
    // in one go. A selection that runs on past the end of a paragraph shows
    // a mark a quarter of the line high after it, which can reach over the
    // cell's right edge (and the next cell's background, at larger sizes),
    // where Qt draws the next cell over it (as over a line wider than its
    // cell); so then Qt draws one cell's text at a time, the others hidden (hiding a paragraph without marking the
    // layout dirty leaves the layout as it is, and drawBlock skips it). The
    // paragraphs above the table are drawn first, here, and hidden too: Qt
    // lays the empty one before a table out on the table's top edge, and a
    // selection over the table takes in its end, whose mark runs on under
    // the first cells.
    void paint(QPainter &painter, QTextDocument &doc, const QAbstractTextDocumentLayout::PaintContext &ctx) const
    {
        QAbstractTextDocumentLayout *layout = doc.documentLayout();
        const QRectF &clip = ctx.clip;
        const Place &p = place(doc);
        const bool oneByOne = !ctx.selections.isEmpty() || p.overflows;
        std::vector<QTextBlock> hidden;
        const auto hide = [&](QTextBlock b) {
            if (b.isVisible()) {
                b.setVisible(false);
                hidden.push_back(b);
            }
        };
        if (oneByOne) {
            // As drawBlock() draws the paragraphs of the document's frame.
            const QRectF frame = layout->frameBoundingRect(doc.rootFrame());
            const QRectF frameClip = clip.isValid() ? (clip & frame) : frame;
            for (QTextBlock b = doc.begin(); b.isValid() && b.position() < table->firstPosition(); b = b.next()) {
                if (!b.isVisible())
                    continue;
                hide(b);
                const QTextLayout *lines = b.layout();
                const QRectF r = lines->boundingRect().translated(lines->position());
                if (clip.isValid() && (r.bottom() < clip.y() || r.top() > clip.bottom()))
                    continue;
                QList<QTextLayout::FormatRange> ranges;
                for (const auto &selection : std::as_const(ctx.selections)) {
                    const int start = selection.cursor.selectionStart() - b.position();
                    const int end = selection.cursor.selectionEnd() - b.position();
                    if (start < b.length() && end > 0 && end > start)
                        ranges.append({start, end - start, selection.format});
                }
                const QPen pen = painter.pen();
                painter.setPen(ctx.palette.color(QPalette::Text));
                lines->draw(&painter, QPointF(), ranges, frameClip);
                painter.setPen(pen);
            }
            for (QTextBlock b = doc.findBlock(table->firstPosition()); b.isValid() && b.position() <= table->lastPosition();
                 b = b.next())
                hide(b);
        }
        // The cells Qt draws, as drawFrame picks them.
        const int rows = table->rows(), columns = table->columns();
        const QPointF off = p.offset;
        const QSizeF frame = layout->frameBoundingRect(table).size();
        int firstRow = 0, lastRow = rows;
        if (clip.isValid()
            && (off.y() > clip.bottom() || off.y() + frame.height() < clip.top() || off.x() > clip.right()
                || off.x() + frame.width() < clip.left())) {
            lastRow = 0;
        } else if (clip.isValid()) {
            const auto fixed = [](qreal v) { return int(v * 64); }; // QFixed::fromReal
            const auto rowsBegin = p.fixedYs.begin(), rowsEnd = p.fixedYs.end() - 1;
            auto it = std::lower_bound(rowsBegin, rowsEnd, fixed(clip.top() - off.y()));
            if (it != rowsEnd && it != rowsBegin)
                firstRow = int(--it - rowsBegin);
            it = std::upper_bound(rowsBegin, rowsEnd, fixed(clip.bottom() - off.y()));
            if (it != rowsEnd)
                lastRow = int(++it - rowsBegin);
        }
        const grid::Shape shape{rows, columns};
        for (int row = firstRow; row < lastRow; ++row) {
            for (int col = 0; col < columns; ++col) {
                const QRectF cellRect(off.x() + p.xs[col], off.y() + p.ys[row], p.xs[col + 1] - p.xs[col],
                                      p.ys[row + 1] - p.ys[row]);
                if (clip.isValid() && !cellRect.adjusted(-0.5, -0.5, 0.5, 0.5).intersects(clip))
                    continue;
                const QBrush &background = backgrounds[size_t(row * columns + col)];
                if (background.style() != Qt::NoBrush) {
                    painter.save();
                    painter.setBrushOrigin(cellRect.topLeft());
                    painter.fillRect(cellRect, background);
                    painter.restore();
                }
                const QRectF borderRect = cellRect.toRect();
                if (borderRect.isValid()) {
                    const bool turnOffAntialiasing = !(painter.renderHints() & QPainter::Antialiasing);
                    painter.setRenderHint(QPainter::Antialiasing);
                    for (const grid::Edge edge : {grid::TopEdge, grid::LeftEdge, grid::RightEdge, grid::BottomEdge}) {
                        painter.save();
                        painter.setPen(Qt::NoPen);
                        painter.setBrush(line);
                        painter.drawRect(shape.edgeRect({row, col}, borderRect, edge));
                        painter.restore();
                    }
                    if (turnOffAntialiasing)
                        painter.setRenderHint(QPainter::Antialiasing, false);
                }
                if (!oneByOne)
                    continue;
                // This cell's text alone, with the clip Qt draws it with
                // (only the clip's height is narrowed, to the row, so that
                // Qt looks at a few rows of cells only).
                const QTextTableCell cell = table->cellAt(row, col);
                std::vector<QTextBlock> shown;
                for (QTextBlock b = cell.firstCursorPosition().block(); b.isValid() && b.position() <= cell.lastPosition();
                     b = b.next()) {
                    b.setVisible(true);
                    shown.push_back(b);
                }
                QAbstractTextDocumentLayout::PaintContext one = ctx;
                QRectF band(QPointF(-1e9, cellRect.top()), QPointF(1e9, cellRect.bottom()));
                one.clip = clip.isValid() ? (band & clip) : band;
                layout->draw(&painter, one);
                for (QTextBlock &b : shown)
                    b.setVisible(false);
            }
        }
        if (!oneByOne)
            layout->draw(&painter, ctx);
        for (QTextBlock &b : hidden)
            b.setVisible(true);
    }
};

void RenderedBlock::setOwnTableGridEnabled(bool on)
{
    ownTableGrid = on;
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
    if (part.first && part.strip)
        r.m_strip = stripHeight(context);
    // As codeBox() makes the whole box: the same text options, unhighlighted.
    // A part above another ends in its last line break, so that a selection
    // that runs on shows it as the box does; the empty line after it is
    // below the part.
    r.m_doc = codeDocument(part.last ? source : source + u'\n', QString(), context, context.width - 2 * codePadX,
                           part.wrapAnywhere, false);
    const qreal textHeight = part.last ? r.m_doc->size().height()
                                       : r.m_doc->documentLayout()->blockBoundingRect(r.m_doc->lastBlock()).top();
    r.m_height = r.m_strip + textHeight + (part.first ? codePadY : 0) + (part.last ? codePadY + blockGap : 0);
    r.m_sourceLines = int(source.count(u'\n')) + 1;
    r.collectLineAnchors(r.m_sourceLines);
    r.m_reflows = true;
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
    auto result = build(block, source, context, item);
    if (item)
        return result;
    switch (folding::classify(block).kind) {
    case folding::FoldKind::Table:
    case folding::FoldKind::Figure:
    case folding::FoldKind::Equation:
    case folding::FoldKind::Code:
        // The room for the strip is the block's own: its parts (anchors,
        // heights) are laid out as they were, one strip lower.
        result->m_strip = stripHeight(context);
        result->m_height += result->m_strip;
        for (LineAnchor &anchor : result->m_lineAnchors)
            anchor.second += result->m_strip;
        break;
    default:
        break;
    }
    return result;
}

qreal RenderedBlock::stripHeight(const RenderContext &context)
{
    return std::ceil(QFontMetricsF(context.font).height()) + stripExtraHeight;
}

std::unique_ptr<RenderedBlock> RenderedBlock::createChip(const ChipInfo &info, const RenderContext &context)
{
    auto result = std::unique_ptr<RenderedBlock>(new RenderedBlock);
    RenderedBlock &r = *result;
    r.m_kind = Kind::Chip;
    r.m_width = context.width;
    const QFontMetricsF metrics(context.font);
    const qreal height = std::ceil(metrics.height()) + chipExtraHeight;
    r.m_chipFont = context.font;
    r.m_chipLabel = info.label;
    const qreal labelWidth = metrics.horizontalAdvance(info.label);
    const qreal lead = chipPadX + chevronSize + chipChevronGap;
    const qreal gap = metrics.horizontalAdvance(QStringLiteral("  "));
    // The detail takes what is left of the column.
    const qreal room = context.width - lead - labelWidth - gap - chipPadX;
    if (!info.detail.isEmpty() && room > metrics.averageCharWidth() * 3)
        r.m_chipDetail = metrics.elidedText(info.detail, Qt::ElideRight, room);
    qreal width = lead + labelWidth + chipPadX;
    if (!r.m_chipDetail.isEmpty())
        width += gap + metrics.horizontalAdvance(r.m_chipDetail);
    r.m_chipRect = QRectF(0, 0, qMin(width, qMax<qreal>(context.width, 1)), height);
    r.m_height = height + 2 * blockGap;
    return result;
}

void RenderedBlock::paintChevron(QPainter &painter, const QPointF &center, bool open, const QColor &color)
{
    // As the mockups draw it: a 8 × 8 box holding a "›" (collapsed) or "⌄".
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    const qreal h = chevronSize / 2;
    QPainterPath path;
    if (open) {
        path.moveTo(center + QPointF(-h + 1, -h / 2 + 0.5));
        path.lineTo(center + QPointF(0, h / 2 + 0.5));
        path.lineTo(center + QPointF(h - 1, -h / 2 + 0.5));
    } else {
        path.moveTo(center + QPointF(-h / 2 + 0.5, -h + 1));
        path.lineTo(center + QPointF(h / 2 + 0.5, 0));
        path.lineTo(center + QPointF(-h / 2 + 0.5, h - 1));
    }
    painter.drawPath(path);
    painter.restore();
}

void RenderedBlock::drawSecondaryText(QPainter &painter, const QRectF &rect, int flags, const QString &text)
{
    // The style draws disabled text (with the etch the theme asks for).
    QApplication::style()->drawItemText(&painter, rect.toRect(), flags, QApplication::palette(), false, text,
                                        QPalette::Text);
}

std::unique_ptr<RenderedBlock> RenderedBlock::build(const core::Block &block, const QString &source,
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
        // Drawn as shapes (see paint()), shrunk to the column if wider (in
        // read mode it keeps its size, and the view scrolls it).
        r.m_kind = Kind::Image;
        r.m_formula = cachedFormula(math::withMacros(text), true);
        r.m_formulaScale = QFontInfo(context.font).pixelSize() * displayMathScale / math::layoutPixelSize;
        const bool wide = r.m_formula->width * r.m_formulaScale > context.width && context.width > 0;
        if (wide && !context.overflow)
            r.m_formulaScale = context.width / r.m_formula->width;
        r.m_centered = true;
        r.m_height = r.pictureSize().height() + 2 * codePadY + blockGap;
        if (wide && context.overflow) {
            r.m_contentWidth = r.pictureSize().width();
            r.m_scrollRoom = scrollBarHeight;
            r.m_height += r.m_scrollRoom;
        }
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
                // Fit the figure box (and what the user made of it), by
                // changing the ratio the pixels are drawn at.
                const QSizeF own = QSizeF(r.m_image.size()) / r.m_image.devicePixelRatio();
                const QList<qreal> factors = context.figureScales.value(figureId(source));
                const FigureFit fit = fitFigure(own, context, factors.isEmpty() ? 1 : factors.first());
                if (fit.size.width() > 0 && qAbs(fit.size.width() - own.width()) > 0.01)
                    r.m_image.setDevicePixelRatio(r.m_image.devicePixelRatio() * own.width() / fit.size.width());
                r.m_diagramFigure.percent = qRound(100 * fit.size.width() / own.width());
                r.m_diagramFigure.factor = fit.factor;
                r.m_diagramFigure.maxFactor = fit.maxFactor;
                r.m_diagramFigure.fittedWidth = fit.fittedWidth;
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
        clampFontSizes(doc.get(), html);
        doc->nameDataPictures();
        doc->dropNetworkImages();
        doc->placeMath();
        doc->sizeFigures(context.figureScales.value(figureId(source)));
        doc->fitImages(context.width);
        if (item && !doc->begin().textList() && doc->blockCount() == 1 && doc->begin().text().isEmpty()) {
            // An empty item, which Qt drops: an empty line of the list as one
            // with text would make it (the text taken out again).
            const qsizetype li = html.indexOf(QLatin1String("<li>"));
            if (li >= 0) {
                doc->setHtml(QString(html).insert(li + 4, u'x'));
                clampFontSizes(doc.get(), html);
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
        QTextTable *single = item ? nullptr : onlyTable(*doc);
        if (single)
            r.m_ownGrid = OwnGrid::take(*doc, *single);
        // A table of plain text keeps its cells' layouts from one width to
        // the next (see TableLayout); one with pictures or formulas, which
        // fit the column, is made again at each width anyway.
        if (r.m_ownGrid && block.inline_math.empty() && !mayLoadPictures(html) && TableLayout::suits(*doc, *single))
            doc->setDocumentLayout(new TableLayout(doc.get(), single));
        QTextTable *only = context.overflow ? single : nullptr;
        r.m_fitsTable = only != nullptr;
        if (only)
            r.m_tableConstraints = only->format().property(QTextFormat::TableColumnWidthConstraints);
        if (only) {
            // A table fits the column as a browser fits one (see fitTable),
            // and keeps the width its cells ask for, scrolled sideways, only
            // when even its narrowest layout is wider than the column.
            if (const qreal wide = fitTable(*doc, *only, context.width); wide > 0) {
                doc->setTextWidth(wide);
                r.m_contentWidth = wide;
                r.m_scrollRoom = scrollBarHeight;
            }
        } else {
            doc->setTextWidth(context.width);
        }
        // An item ends where the next starts (see itemAdvance).
        r.m_height = doc->size().height() + (item ? 0 : blockGap) + r.m_scrollRoom;
        // Backgrounds are asked for when painted: start their downloads now.
        if (context.allowRemoteImages && mayLoadPictures(QByteArrayView(block.html.data(), qsizetype(block.html.size()))))
            doc->loadPaintedPictures();
        r.m_pending = doc->hasPendingDownload();
        r.m_pictureBytes = doc->pictureBytes() // Resolved by the layout (and the dry paint).
                           + doc->mathElements() * qsizetype(sizeof(QPainterPath::Element));
        r.m_reflows = block.inline_math.empty() && !r.m_pending && r.m_pictureBytes == 0 && !asksForPictures(*doc);
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
    clampFontSizes(&doc, html);
    // As in create(): Qt's image handler looks for "@2x" files by name first.
    doc.nameDataPictures();
    doc.dropNetworkImages();
    doc.setTextWidth(context.width);
    doc.loadPaintedPictures();
    return doc.remoteUrls();
}

bool RenderedBlock::reflow(const RenderContext &context)
{
    if (!m_reflows || !m_doc || (m_kind != Kind::Rich && m_kind != Kind::Code))
        return false;
    m_width = context.width;
    m_tableGrid.reset();
    m_breaks.reset();
    m_lineAnchors.clear();
    if (m_kind == Kind::Code) {
        // As codeBox() and createPart() lay it out.
        m_doc->setTextWidth(context.width - 2 * codePadX);
        const qreal textHeight = m_part.last ? m_doc->size().height()
                                             : m_doc->documentLayout()->blockBoundingRect(m_doc->lastBlock()).top();
        m_height = m_strip + textHeight + (m_part.first ? codePadY : 0) + (m_part.last ? codePadY + blockGap : 0);
        collectLineAnchors(m_sourceLines);
        return true;
    }
    // As build() lays it out (and create() adds the strip).
    m_contentWidth = 0;
    m_scrollRoom = 0;
    QTextTable *table = m_fitsTable ? onlyTable(*m_doc) : nullptr;
    if (table) {
        // The widths the table had before fitTable() set its own.
        QTextTableFormat format = table->format();
        if (m_tableConstraints.isValid())
            format.setProperty(QTextFormat::TableColumnWidthConstraints, m_tableConstraints);
        else
            format.clearProperty(QTextFormat::TableColumnWidthConstraints);
        table->setFormat(format);
        if (const qreal wide = fitTable(*m_doc, *table, context.width); wide > 0) {
            m_doc->setTextWidth(wide);
            m_contentWidth = wide;
            m_scrollRoom = scrollBarHeight;
        }
    } else {
        m_doc->setTextWidth(context.width);
    }
    m_height = m_doc->size().height() + (m_item.made ? 0 : blockGap) + m_scrollRoom + m_strip;
    collectLineAnchors(0);
    return true;
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
    return m_kind == Kind::Code ? QPointF(codePadX, m_strip + (m_part.first ? codePadY : 0)) : QPointF(0, m_strip);
}

void RenderedBlock::paint(QPainter &painter, const QPointF &topLeft, qreal width, const RenderedSelection &selection) const
{
    switch (m_kind) {
    case Kind::Empty:
        return;
    case Kind::Chip: {
        const QRectF box = m_chipRect.translated(topLeft);
        const QFontMetricsF metrics(m_chipFont);
        painter.save();
        painter.setFont(m_chipFont);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        const QRectF outline = box.adjusted(0.5, 0.5, -0.5, -0.5);
        path.addRoundedRect(outline, outline.height() / 2, outline.height() / 2);
        painter.fillPath(path, theme::color(selection.hot ? theme::Role::buttonHover : theme::Role::sideBar));
        painter.setPen(QPen(theme::color(selection.caretOn ? theme::Role::accent : theme::Role::divider), 1));
        painter.drawPath(path);
        paintChevron(painter, QPointF(box.left() + chipPadX + chevronSize / 2, box.center().y()), false,
                     theme::color(theme::Role::text));
        const QRectF textRect(box.left() + chipPadX + chevronSize + chipChevronGap, box.top(),
                              box.width(), box.height());
        const qreal labelWidth = metrics.horizontalAdvance(m_chipLabel);
        painter.setPen(theme::color(theme::Role::text));
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, m_chipLabel);
        if (!m_chipDetail.isEmpty()) {
            const qreal x = textRect.left() + labelWidth + metrics.horizontalAdvance(QStringLiteral("  "));
            drawSecondaryText(painter, QRectF(x, box.top(), box.right() - chipPadX - x + 1, box.height()),
                              Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, m_chipDetail);
        }
        painter.restore();
        return;
    }
    case Kind::Image: {
        const QSizeF size = pictureSize();
        const qreal x = m_centered ? topLeft.x() + qMax<qreal>(0, (width - size.width()) / 2) : topLeft.x();
        const QRectF imageRect(QPointF(x, topLeft.y() + m_strip + codePadY), size);
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
        const qreal top = m_part.first ? topLeft.y() + m_strip : topLeft.y() - 2 * radius;
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
        if (m_ownGrid)
            m_ownGrid->paint(painter, *m_doc, ctx);
        else
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
    return onlyTable(*m_doc);
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
    // (The last row: as far as the text of its fullest cell, and its padding.)
    const auto lastBottom = [&] {
        qreal b = own.bottom();
        for (int c = 0; c < table->columns(); ++c) {
            const QRectF r = content(row, c);
            if (!r.isNull())
                b = qMax(b, r.bottom());
        }
        return b + edge;
    };
    const qreal bottom = row + 1 < table->rows() ? content(row + 1, col).top() - edge : lastBottom();
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

const RenderedBlock::Breaks &RenderedBlock::breaks() const
{
    if (m_breaks)
        return *m_breaks;
    Breaks &b = m_breaks.emplace();
    if (!m_doc || (m_kind != Kind::Rich && m_kind != Kind::Code))
        return b;
    QAbstractTextDocumentLayout *layout = m_doc->documentLayout();
    const qreal top = contentOffset().y();
    // The tops of a paragraph's lines, as collectLineAnchors reads them.
    const auto linesOf = [&](const QTextBlock &block) {
        QList<int> tops;
        const QTextLayout *lines = block.layout();
        const qreal y = top + layout->blockBoundingRect(block).top();
        for (int i = 0; lines && i < lines->lineCount(); ++i)
            tops.push_back(int(std::floor(y + lines->lineAt(i).y())));
        return tops;
    };
    const auto sorted = [](QList<int> &list) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    };
    if (m_kind == Kind::Code) {
        for (QTextBlock block = m_doc->begin(); block.isValid(); block = block.next())
            b.lineTops += linesOf(block);
        sorted(b.lineTops);
        b.cuts = md::codeCuts(b.lineTops);
        return b;
    }
    if (const TableGrid *grid = tableGrid()) {
        b.table = true;
        for (int row = 0; row < grid->rows(); ++row)
            b.lineTops.push_back(int(std::floor(grid->ys[row])));
        b.headerTop = int(std::floor(grid->ys[0]));
        b.headerHeight = int(std::ceil(grid->ys[1])) - b.headerTop;
        b.cuts = md::tableCuts(b.lineTops, 1);
        // A body row taller than a few lines may break between its lines
        // too, where no cell has a line across the break: at least 2 lines of
        // its fullest cell on each side.
        const QTextTable *table = theTable();
        for (int row = 1; table && row < grid->rows(); ++row) {
            std::vector<std::pair<qreal, qreal>> lines; // Top, bottom.
            int most = 0;
            std::vector<std::vector<qreal>> tops(table->columns());
            for (int col = 0; col < table->columns(); ++col) {
                const QTextTableCell cell = table->cellAt(row, col);
                if (!cell.isValid())
                    continue;
                for (QTextBlock block = cell.firstCursorPosition().block();
                     block.isValid() && block.position() <= cell.lastPosition(); block = block.next()) {
                    const QTextLayout *text = block.layout();
                    const qreal y = top + layout->blockBoundingRect(block).top();
                    for (int i = 0; text && i < text->lineCount(); ++i) {
                        const QTextLine line = text->lineAt(i);
                        lines.push_back({y + line.y(), y + line.y() + line.height()});
                        tops[col].push_back(y + line.y());
                    }
                }
                most = qMax(most, int(tops[col].size()));
            }
            if (most < 2)
                continue;
            for (const auto &[lineTop, lineBottom] : lines) {
                Q_UNUSED(lineBottom);
                const int cut = int(std::floor(lineTop));
                if (cut <= std::ceil(grid->ys[row]) || cut >= grid->ys[row + 1])
                    continue;
                const bool across = std::any_of(lines.begin(), lines.end(), [cut](const auto &l) {
                    return l.first < cut - 0.5 && l.second > cut + 0.5;
                });
                if (across)
                    continue;
                int above = 0, below = 0;
                for (const auto &column : tops) {
                    const int n = int(std::count_if(column.begin(), column.end(), [cut](qreal t) { return t < cut - 0.5; }));
                    above = qMax(above, n);
                    below = qMax(below, int(column.size()) - n);
                }
                if (above == 0)
                    continue; // (The row's first line: its top is the row's.)
                b.lineTops.push_back(cut);
                if (above >= 2 && below >= 2)
                    b.cuts.push_back(cut);
            }
        }
        sorted(b.lineTops);
        sorted(b.cuts);
        if (table) {
            const QTextTableCellFormat f = table->cellAt(0, 0).format().toTableCellFormat();
            const QBrush brush = qvariant_cast<QBrush>(f.property(QTextFormat::TableCellBottomBorderBrush));
            if (brush.style() == Qt::SolidPattern)
                b.lineColor = brush.color();
        }
        return b;
    }
    for (QTextBlock block = m_doc->begin(); block.isValid(); block = block.next()) {
        if (QTextCursor(block).currentTable())
            continue; // (An HTML table among other text: in one piece.)
        const QList<int> tops = linesOf(block);
        if (tops.isEmpty())
            continue;
        if (!b.lineTops.isEmpty())
            b.cuts.push_back(tops.front());
        b.cuts += md::paragraphCuts(tops);
        b.lineTops += tops;
    }
    sorted(b.lineTops);
    sorted(b.cuts);
    return b;
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
        if (!QRectF(QPointF(x, m_strip + codePadY), size).contains(point))
            return std::nullopt;
        return Picture{m_diagramCode.isNull() ? QObject::tr("Equation") : QObject::tr("Diagram"), {}};
    }
    if (m_kind != Kind::Rich)
        return std::nullopt;
    const QString source = m_doc->documentLayout()->imageAt(point - contentOffset());
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

QString RenderedBlock::figureId(const QString &source)
{
    return QStringLiteral("%1:%2").arg(source.size()).arg(qHash(source));
}

bool RenderedBlock::mayHaveFigure(const QString &source)
{
    return source.contains(QLatin1String("!["))
           || source.contains(QLatin1String("<img"), Qt::CaseInsensitive)
           || source.contains(QLatin1String("mermaid"), Qt::CaseInsensitive);
}

std::vector<RenderedBlock::Figure> RenderedBlock::figures() const
{
    std::vector<Figure> result;
    if (m_kind == Kind::Image && !m_diagramCode.isNull()) {
        // Centred in the column, as paint() draws it.
        const QSizeF size = pictureSize();
        const qreal x = m_centered ? qMax<qreal>(0, (m_width - size.width()) / 2) : 0;
        Figure figure = m_diagramFigure;
        figure.rect = QRectF(QPointF(x, m_strip + codePadY), size);
        result.push_back(figure);
    } else if (m_kind == Kind::Rich) {
        const auto *doc = static_cast<const ResourceDocument *>(m_doc.get());
        for (const auto &[position, found] : doc->figures()) {
            // The picture stands on its line's baseline.
            const QTextBlock block = m_doc->findBlock(position);
            const QTextLayout *layout = block.isValid() ? block.layout() : nullptr;
            const QTextLine line = layout ? layout->lineForTextPosition(position - block.position()) : QTextLine();
            if (!line.isValid())
                continue;
            const QPointF origin = m_doc->documentLayout()->blockBoundingRect(block).topLeft() + contentOffset();
            Figure figure = found;
            const QSizeF size = found.rect.size();
            figure.rect = QRectF(QPointF(origin.x() + line.cursorToX(position - block.position()),
                                         origin.y() + line.y() + line.ascent() - size.height()),
                                 size);
            result.push_back(figure);
        }
    }
    return result;
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

QRectF RenderedBlock::scrollTrack(qreal width) const
{
    if (m_scrollRoom <= 0)
        return {};
    return QRectF(0, m_height - blockGap - m_scrollRoom, width, m_scrollRoom);
}

QRectF RenderedBlock::scrollThumb(const QRectF &track, qreal content, qreal offset)
{
    if (content <= track.width() || track.isEmpty())
        return {};
    const qreal width = qBound(qMin(scrollThumbMin, track.width()), track.width() * track.width() / content,
                               track.width());
    const qreal range = content - track.width();
    const qreal along = qBound<qreal>(0, offset / range, 1) * (track.width() - width);
    // Under the line that sets the track off, between its margins.
    return QRectF(track.left() + along, track.top() + 1 + scrollThumbMargin, width,
                  track.height() - 1 - 2 * scrollThumbMargin);
}

void RenderedBlock::paintScrollBar(QPainter &painter, const QRectF &track, qreal content, qreal offset, bool hot)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillRect(track, theme::color(theme::Role::editor));
    painter.fillRect(QRectF(track.left(), track.top(), track.width(), 1), theme::color(theme::Role::sideBar));
    painter.fillRect(scrollThumb(track, content, offset), theme::color(hot ? theme::Role::divider : theme::Role::panelTitle));
    painter.restore();
}

QSizeF RenderedBlock::contentSize() const
{
    return QSizeF(contentWidth(), qMax<qreal>(0, m_height - m_strip - m_scrollRoom - blockGap));
}

void RenderedBlock::paintContent(QPainter &painter, const QPointF &topLeft, const RenderedSelection &selection) const
{
    paint(painter, topLeft - QPointF(0, m_strip), contentWidth(), selection);
}

bool RenderedBlock::textAt(const QPointF &point) const
{
    if (!m_doc || (m_kind != Kind::Rich && m_kind != Kind::Code))
        return false;
    return m_doc->documentLayout()->hitTest(point - contentOffset(), Qt::ExactHit) >= 0;
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

std::optional<QRectF> RenderedBlock::lastLineRect() const
{
    if (!m_doc || (m_kind != Kind::Rich && m_kind != Kind::Code))
        return std::nullopt;
    const QTextLayout *layout = m_doc->lastBlock().layout();
    if (!layout || layout->lineCount() == 0)
        return std::nullopt;
    const QTextLine line = layout->lineAt(layout->lineCount() - 1);
    // The layout's origin is in document coordinates, as in caretRect().
    const QPointF origin = m_doc->documentLayout()->blockBoundingRect(m_doc->lastBlock()).topLeft() + contentOffset();
    QRectF rect = line.naturalTextRect();
    rect.translate(origin);
    return rect;
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
        // A table that keeps its cells' shaped text (see TableLayout): some
        // 40 bytes a character more.
        if (qobject_cast<const TableLayout *>(m_doc->documentLayout()))
            kib += characters / 25;
    }
    return kib;
}

} // namespace vn
