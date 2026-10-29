#include "BlockRenderer.h"

#include "../Theme.h"
#include "MathRenderer.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QAbstractTextDocumentLayout>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QTextBlock>
#include <QUrl>

namespace vn {

namespace {

constexpr qreal codePadX = 10;
constexpr qreal codePadY = 5;
constexpr qreal blockGap = 4;

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

protected:
    QVariant loadResource(int type, const QUrl &url) override
    {
        if (type != QTextDocument::ImageResource)
            return QTextDocument::loadResource(type, url);
        const QString name = url.toString();
        if (name.startsWith(QLatin1String("vn-math"))) {
            const bool display = name.startsWith(QLatin1String("vn-math-display:"));
            const int index = name.section(u':', 1).toInt();
            math::Style style;
            style.pixelSize = QFontInfo(m_context.font).pixelSize() * (display ? 1.2 : 1.1);
            style.color = theme::color(theme::Role::text);
            style.devicePixelRatio = m_context.devicePixelRatio;
            style.display = display;
            return math::render(math::withMacros(m_math.value(index)), style);
        }
        const QString path = localPath(url);
        if (path.isEmpty())
            return QVariant();
        QImageReader reader(path);
        reader.setAutoTransform(true);
        QImage image = reader.read();
        if (image.isNull())
            return QVariant();
        // One image pixel per logical pixel, shrunk to fit the column.
        if (image.width() > m_context.width) {
            const qreal dpr = m_context.devicePixelRatio;
            image = image.scaledToWidth(int(m_context.width * dpr), Qt::SmoothTransformation);
            image.setDevicePixelRatio(dpr);
        }
        return image;
    }

private:
    QString localPath(const QUrl &url) const
    {
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
    return QStringLiteral(R"(
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
}

QImage mermaidImage(const QString &code, const RenderContext &context, QString *error)
{
    const QByteArray utf8 = code.toUtf8();
    const QByteArray themeName = context.mermaidTheme.toUtf8();
    try {
        const rust::Vec<uint8_t> png = core::mermaid_png(rust::Str(utf8.constData(), utf8.size()),
                                                         rust::Str(themeName.constData(), themeName.size()),
                                                         float(context.devicePixelRatio));
        QImage image = QImage::fromData(png.data(), int(png.size()), "PNG");
        image.setDevicePixelRatio(context.devicePixelRatio);
        return image;
    } catch (const std::exception &e) { // rust::Error
        *error = QString::fromUtf8(e.what());
        return {};
    }
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

std::unique_ptr<RenderedBlock> RenderedBlock::create(const core::Block &block, const QString &source,
                                                     const RenderContext &context)
{
    auto result = std::unique_ptr<RenderedBlock>(new RenderedBlock);
    RenderedBlock &r = *result;
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
        r.m_image = math::render(math::withMacros(text), style);
        r.m_centered = true;
        r.m_height = r.m_image.height() / r.m_image.devicePixelRatio() + 2 * codePadY + blockGap;
        return result;
    }
    case core::BlockKind::Code:
        if (lang.compare(QLatin1String("mermaid"), Qt::CaseInsensitive) == 0) {
            r.m_image = mermaidImage(text, context, &r.m_error);
            if (!r.m_image.isNull()) {
                r.m_kind = Kind::Image;
                r.m_centered = true;
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
        doc->setTextWidth(context.width);
        r.m_height = doc->size().height() + blockGap;
        r.m_doc = std::move(doc);
        return result;
    }
    }
}

QPointF RenderedBlock::contentOffset() const
{
    return m_kind == Kind::Code ? QPointF(codePadX, codePadY) : QPointF(0, 0);
}

void RenderedBlock::paint(QPainter &painter, const QPointF &topLeft, qreal width) const
{
    switch (m_kind) {
    case Kind::Empty:
        return;
    case Kind::Image: {
        const QSizeF size = m_image.size() / m_image.devicePixelRatio();
        const qreal x = m_centered ? topLeft.x() + qMax<qreal>(0, (width - size.width()) / 2) : topLeft.x();
        painter.drawImage(QRectF(QPointF(x, topLeft.y() + codePadY), size), m_image);
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

QString RenderedBlock::renderedText() const
{
    return m_doc ? m_doc->toPlainText() : QString();
}

} // namespace vn
