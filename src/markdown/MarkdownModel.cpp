#include "MarkdownModel.h"

#include "../Settings.h"

#include <QTextBlock>
#include <QTextDocument>

namespace vn {

MarkdownModel::MarkdownModel(QTextDocument *document)
    : QObject(document)
    , m_doc(document)
{
    // contentsChange fires for text edits only (highlighter format changes go
    // to the layout alone), and before the views hear about the change.
    connect(document, &QTextDocument::contentsChange, this, [this] { m_dirty = true; });
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString &key) {
        if (key.startsWith(QLatin1String("markdown/"))) {
            ++m_optionsRevision;
            m_dirty = true;
            Q_EMIT optionsChanged();
        }
    });
}

core::RenderOptions MarkdownModel::options() const
{
    Settings &s = Settings::instance();
    const QString frontMatter = s.text(key::frontMatter);
    return core::RenderOptions{
        s.flag(key::breaks),
        s.flag(key::linkify),
        s.flag(key::typographer),
        s.flag(key::renderMath),
        static_cast<uint8_t>(frontMatter == QLatin1String("hide") ? 0 : frontMatter == QLatin1String("code") ? 1 : 2),
    };
}

const rust::Vec<core::Block> &MarkdownModel::blocks()
{
    if (m_dirty)
        reparse();
    return m_blocks;
}

int MarkdownModel::blockAt(int line)
{
    blocks();
    return line >= 0 && line < int(m_lineToBlock.size()) ? m_lineToBlock[line] : -1;
}

void MarkdownModel::reparse()
{
    QString text = m_doc->toRawText();
    text.replace(QChar::ParagraphSeparator, u'\n');
    const QByteArray utf8 = text.toUtf8();
    const core::RenderOptions opts = options();
    m_blocks = core::blocks(rust::Str(utf8.constData(), utf8.size()), opts);
    m_lineToBlock.assign(m_doc->blockCount(), -1);
    for (int i = 0; i < int(m_blocks.size()); ++i) {
        const core::Block &b = m_blocks[i];
        for (size_t line = b.first_line; line <= b.last_line && line < m_lineToBlock.size(); ++line)
            m_lineToBlock[line] = i;
    }
    m_dirty = false;
}

QString MarkdownModel::sourceText(int firstLine, int lastLine) const
{
    QStringList lines;
    for (QTextBlock b = m_doc->findBlockByNumber(firstLine); b.isValid() && b.blockNumber() <= lastLine; b = b.next())
        lines.append(b.text());
    return lines.join(u'\n');
}

} // namespace vn
