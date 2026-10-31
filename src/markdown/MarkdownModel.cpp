#include "MarkdownModel.h"

#include "../Settings.h"

#include <QFutureWatcher>
#include <QPromise>
#include <QTextBlock>
#include <QTextDocument>
#include <QThreadPool>

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

std::shared_ptr<const core::Document> shared(rust::Box<core::Document> document)
{
    return std::shared_ptr<const core::Document>(document.into_raw(), [](const core::Document *d) {
        (void)rust::Box<core::Document>::from_raw(const_cast<core::Document *>(d));
    });
}

// The blocks of a document's text (from QTextDocument::toRawText); given the
// parse of an earlier version, only the blocks around the edits are parsed
// again (when that gives the same blocks). Only values, no Qt objects: also
// runs on a worker thread.
std::shared_ptr<const core::Document> parseText(QString text, const core::RenderOptions &options,
                                                const std::shared_ptr<const core::Document> &previous)
{
    text.replace(QChar::ParagraphSeparator, u'\n');
    const QByteArray utf8 = text.toUtf8();
    const rust::Str markdown(utf8.constData(), utf8.size());
    return shared(previous ? core::reparse(*previous, markdown, options) : core::parse(markdown, options));
}

} // namespace

// A worker's parse of the text as it was at input serial `serial`.
struct MarkdownModel::ParseResult
{
    quint64 serial;
    std::shared_ptr<const core::Document> parsed;
};

MarkdownModel::MarkdownModel(QTextDocument *document)
    : QObject(document)
    , m_doc(document)
{
    m_job.watcher = new QFutureWatcher<std::shared_ptr<ParseResult>>(this);
    connect(m_job.watcher, &QFutureWatcher<std::shared_ptr<ParseResult>>::finished, this,
            &MarkdownModel::backgroundParseDone);
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
            ++m_inputSerial;
        } else if (!affectsRendering(key)) {
            return;
        }
        ++m_optionsRevision;
        Q_EMIT optionsChanged();
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
    flush();
    return m_parsed->blocks();
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

void MarkdownModel::flushInBackground()
{
    if (!m_dirty && !m_pending)
        return;
    // Unusable spans can't wait: the views need them now. Small documents
    // parse in a few milliseconds.
    if (m_dirty || m_doc->characterCount() <= backgroundParseChars) {
        flush();
        return;
    }
    if (m_job.running) {
        m_job.again = true;
        return;
    }
    m_job.running = true;
    m_job.again = false;
    // The worker gets copies and shared, immutable values only: the text
    // (toRawText is a deep copy), the options, the last parse and the promise
    // it shares with the watcher. The model may be gone by the time it
    // finishes; then nobody watches the result.
    auto promise = std::make_shared<QPromise<std::shared_ptr<ParseResult>>>();
    promise->start();
    m_job.watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, serial = m_inputSerial, text = m_doc->toRawText(),
                                          options = options(), previous = m_parsed] {
        promise->addResult(std::make_shared<ParseResult>(ParseResult{serial, parseText(text, options, previous)}));
        promise->finish();
    });
}

void MarkdownModel::backgroundParseDone()
{
    m_job.running = false;
    const std::shared_ptr<ParseResult> result =
        m_job.watcher->future().resultCount() > 0 ? m_job.watcher->result() : nullptr;
    // Only a parse of the text as it is now may replace the spans; one of
    // older text, or one that a synchronous flush() overtook, is dropped.
    if (result && result->serial == m_inputSerial && m_pending && !m_dirty) {
        apply(result->parsed);
        Q_EMIT reparsed();
        return;
    }
    if (m_job.again)
        flushInBackground();
}

void MarkdownModel::documentChanged(int from, int, int added)
{
    ++m_inputSerial;
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
    // Small documents parse whole, as they always did.
    const bool large = m_doc->characterCount() > backgroundParseChars;
    apply(parseText(m_doc->toRawText(), options(), large ? m_parsed : nullptr));
}

void MarkdownModel::apply(std::shared_ptr<const core::Document> parsed)
{
    // Freeing a large document's blocks takes milliseconds (about 3 ms at
    // 10 MB): the thread pool does it.
    if (m_parsed && m_parsed->blocks().size() > 4096)
        QThreadPool::globalInstance()->start([old = std::move(m_parsed)] {});
    m_parsed = std::move(parsed);
    const rust::Vec<core::Block> &blocks = m_parsed->blocks();
    m_lineCount = m_doc->blockCount();
    m_spans.clear();
    m_spans.reserve(blocks.size());
    int next = 0;
    for (int i = 0; i < int(blocks.size()); ++i) {
        const core::Block &b = blocks[i];
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
