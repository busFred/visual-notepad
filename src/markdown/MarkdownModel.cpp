#include "MarkdownModel.h"

#include "../Settings.h"

#include <QTextBlock>
#include <QTextDocument>

#include <algorithm>

namespace vn {

namespace {

// Edits over more lines than this (pastes, replace all, reloads) mark the
// spans unusable instead: the next read reparses, as one big edit costs a
// parse anyway.
constexpr int maxShiftedLines = 2000;

// Settings that change what core::blocks returns (see options()); the other
// markdown/ settings that affect rendering only need new renders.
bool affectsParse(const QString &key)
{
    return key == QLatin1String(key::breaks) || key == QLatin1String(key::linkify)
        || key == QLatin1String(key::typographer) || key == QLatin1String(key::renderMath)
        || key == QLatin1String(key::frontMatter);
}

bool affectsRendering(const QString &key)
{
    return key == QLatin1String(key::textFontFamily) || key == QLatin1String(key::textFontSize)
        || key == QLatin1String(key::textLineHeight) || key == QLatin1String(key::mathMacros)
        || key == QLatin1String(key::mermaidTheme);
}

} // namespace

MarkdownModel::MarkdownModel(QTextDocument *document)
    : QObject(document)
    , m_doc(document)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(reparseDelay());
    connect(&m_timer, &QTimer::timeout, this, &MarkdownModel::reparseDue);
    // contentsChange fires for text edits only (highlighter format changes go
    // to the layout alone), and before the views hear about the change.
    connect(document, &QTextDocument::contentsChange, this, &MarkdownModel::documentChanged);
    connect(&Settings::instance(), &Settings::changed, this, [this](const QString &key) {
        if (affectsParse(key)) {
            m_dirty = true;
            ++m_revision;
        } else if (!affectsRendering(key)) {
            return;
        }
        ++m_optionsRevision;
        Q_EMIT optionsChanged();
    });
}

void MarkdownModel::setRemoteConsent(RemoteConsent consent)
{
    if (m_remoteConsent == consent)
        return;
    m_remoteConsent = consent;
    Q_EMIT remoteConsentChanged();
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
    flush();
    return m_blocks;
}

int MarkdownModel::blockAt(int line)
{
    flush();
    const int i = spanAt(line);
    return i >= 0 ? m_spans[i].block : -1;
}

const std::vector<MarkdownModel::Span> &MarkdownModel::spans()
{
    // Silent: the caller is a view rebuilding its rows from this.
    if (m_dirty)
        reparse();
    return m_spans;
}

int MarkdownModel::spanAt(int line)
{
    const auto &all = spans();
    auto it = std::upper_bound(all.begin(), all.end(), line, [](int l, const Span &s) { return l < s.first; });
    if (it == all.begin())
        return -1;
    --it;
    return line <= it->last ? int(it - all.begin()) : -1;
}

void MarkdownModel::flush()
{
    if (!m_dirty && !m_pending)
        return;
    reparse();
    Q_EMIT reparsed();
}

void MarkdownModel::documentChanged(int from, int, int added)
{
    if (m_dirty)
        return;
    // As in TextView: lines [first, oldLast] were replaced by [first, lastNew].
    const int newCount = m_doc->blockCount();
    const int delta = newCount - m_lineCount;
    const int first = qMax(0, m_doc->findBlock(from).blockNumber());
    const int endPos = qBound(0, from + added, m_doc->characterCount() - 1);
    const int lastNew = qMax(first, m_doc->findBlock(endPos).blockNumber());
    const int oldLast = lastNew - delta;
    ++m_revision;
    if (oldLast < first || oldLast >= m_lineCount || lastNew - first > maxShiftedLines
        || oldLast - first > maxShiftedLines) {
        m_dirty = true;
        m_timer.stop();
        return;
    }

    // Spans before the edit stay, spans after it move by delta, and the
    // edited lines plus every span they touched become one unparsed span.
    std::vector<Span> spans;
    spans.reserve(m_spans.size() + 1);
    Span edited{first, lastNew, -1, -1};
    int touched = 0;
    int previous = -1;
    bool placed = false;
    const auto place = [&] {
        edited.previous = touched == 1 ? previous : -1;
        spans.push_back(edited);
        placed = true;
    };
    for (const Span &s : m_spans) {
        if (s.last < first) {
            spans.push_back(s);
        } else if (s.first > oldLast) {
            if (!placed)
                place();
            spans.push_back({s.first + delta, s.last + delta, s.block, s.previous});
        } else {
            edited.first = qMin(edited.first, s.first);
            edited.last = qMax(edited.last, s.last > oldLast ? s.last + delta : lastNew);
            ++touched;
            previous = s.block >= 0 ? s.block : s.previous;
        }
    }
    if (!placed)
        place();
    m_spans = std::move(spans);
    m_lineCount = newCount;
    m_pending = true;
    m_timer.start();
}

void MarkdownModel::reparse()
{
    QByteArray utf8;
    {
        QString text = m_doc->toRawText();
        text.replace(QChar::ParagraphSeparator, u'\n');
        utf8 = text.toUtf8();
    }
    const core::RenderOptions opts = options();
    m_blocks = core::blocks(rust::Str(utf8.constData(), utf8.size()), opts);
    m_lineCount = m_doc->blockCount();
    m_spans.clear();
    m_spans.reserve(m_blocks.size());
    int next = 0;
    for (int i = 0; i < int(m_blocks.size()); ++i) {
        const core::Block &b = m_blocks[i];
        const int first = qMax(int(b.first_line), next);
        const int last = int(qMin<size_t>(b.last_line, size_t(m_lineCount - 1)));
        if (last < first)
            continue;
        m_spans.push_back({first, last, i, -1});
        next = last + 1;
    }
    m_dirty = false;
    m_pending = false;
    m_timer.stop();
    ++m_revision;
    ++m_parseGeneration;
}

QString MarkdownModel::sourceText(int firstLine, int lastLine) const
{
    QString text;
    int line = firstLine;
    for (QTextBlock b = m_doc->findBlockByNumber(firstLine); b.isValid() && line <= lastLine; b = b.next(), ++line) {
        if (line > firstLine)
            text += u'\n';
        text += b.text();
    }
    return text;
}

} // namespace vn
