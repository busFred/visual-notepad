#include "BlockRenderer.h"

#include "../Theme.h"
#include "MarkdownEditing.h"
#include "MathRenderer.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QAbstractTextDocumentLayout>
#include <QCache>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextLayout>
#include <QThreadPool>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>
#include <atomic>

namespace vn {

namespace {

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

// True if the SVG at `path` names a file on another machine, which the SVG
// plugin would open by itself: <image href> pictures and xml-stylesheet
// style sheets. Read the way QtSvg reads it.
bool svgNamesNetworkPath(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    static const QRegularExpression href(QStringLiteral(R"(href\s*=\s*["']([^"']*))"),
                                         QRegularExpression::CaseInsensitiveOption);
    QXmlStreamReader xml(&file);
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
    if (reader.format().startsWith("svg") && svgNamesNetworkPath(path))
        return {};
    return reader.read();
}

// Local pictures, read and scaled to the column, by file version and size.
// Decoding is far slower than scaling (77 ms against 1 ms for a 4000 by 1200
// PNG) and every new window width needs a new size, so the decoded files are
// kept too, by file version, until their total size passes a limit. SVGs are
// kept as decoded as well: they are read at their own size, never at the
// column's, so a bigger or smaller column scales the same pixels either way.
QImage cachedLocalImage(const QString &path, qreal width, qreal dpr)
{
    static QCache<QString, QImage> cache(64 * 1024);      // KiB
    static QCache<QString, QImage> originals(128 * 1024); // KiB
    const QFileInfo info(path);
    const QString version = QStringLiteral("%1|%2|").arg(info.lastModified().toMSecsSinceEpoch()).arg(info.size()) + path;
    const QString key = QStringLiteral("%1|%2|").arg(width).arg(dpr) + version;
    if (const QImage *hit = cache.object(key))
        return *hit;
    QImage image;
    if (const QImage *original = originals.object(version)) {
        image = *original;
    } else {
        image = readImage(path);
        if (image.isNull())
            return image; // Not cached: the file may appear later.
        originals.insert(version, new QImage(image), imageCost(image));
    }
    // One image pixel per logical pixel, shrunk to fit the column.
    if (image.width() > width) {
        image = image.scaledToWidth(int(width * dpr), Qt::SmoothTransformation);
        image.setDevicePixelRatio(dpr);
    }
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

// Rich text with images resolved locally: math placeholders and files.
class ResourceDocument : public QTextDocument
{
public:
    ResourceDocument(const core::Block &block, const RenderContext &context)
        : m_context(context)
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

    // The file a picture's source names, or empty (math, remote pictures).
    QString pathOf(const QString &source) const
    {
        return source.startsWith(QLatin1String("vn-math")) ? QString() : localPath(QUrl(source));
    }

protected:
    QVariant loadResource(int type, const QUrl &url) override
    {
        // Style sheets (<link>, @import) and the rest: the base class would
        // open any path, even on another machine. The app sets its own.
        if (type != QTextDocument::ImageResource)
            return QVariant();
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
        addResource(type, url, image);
        return image;
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
    QStringList m_math;
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

std::unique_ptr<QTextDocument> codeDocument(const QString &source, const QString &definition,
                                            const RenderContext &context, qreal width)
{
    auto doc = std::make_unique<QTextDocument>();
    doc->setDocumentMargin(0);
    doc->setDefaultFont(context.codeFont);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    doc->setDefaultTextOption(option);
    doc->setPlainText(source);
    doc->setTextWidth(width);
    auto *highlighter = new KSyntaxHighlighting::SyntaxHighlighter(doc.get());
    highlighter->setTheme(theme::syntaxTheme());
    highlighter->setDefinition(theme::syntaxRepository().definitionForName(definition));
    highlighter->rehighlight();
    return doc;
}

} // namespace

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
    case core::BlockKind::FrontMatter: {
        // The fences stay visible (muted) and the Markdown definition
        // highlights the fenced language, as in the source view.
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
    case core::BlockKind::Html:
    default: {
        r.m_kind = Kind::Rich;
        auto doc = std::make_unique<ResourceDocument>(block, context);
        doc->setDocumentMargin(0);
        doc->setDefaultFont(context.font);
        doc->setDefaultStyleSheet(styleSheet(context));
        doc->setHtml(QString::fromUtf8(block.html.data(), block.html.size()));
        doc->dropNetworkImages();
        doc->fitImages(context.width);
        doc->setTextWidth(context.width);
        r.m_height = doc->size().height() + blockGap;
        r.m_doc = std::move(doc);
        r.collectLineAnchors(0);
        return result;
    }
    }
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
    const QString path = source.isEmpty() ? QString() : static_cast<const ResourceDocument *>(m_doc.get())->pathOf(source);
    if (path.isEmpty())
        return std::nullopt;
    return Picture{QFileInfo(path).fileName(), path};
}

QImage RenderedBlock::pictureImage(const Picture &picture) const
{
    if (!picture.path.isEmpty())
        return fullSizeImage(picture.path);
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
    // Laid-out text takes very roughly 128 bytes a character.
    int kib = 1;
    if (!m_image.isNull())
        kib += int(m_image.sizeInBytes() / 1024);
    if (m_doc)
        kib += m_doc->characterCount() / 8;
    return kib;
}

} // namespace vn
