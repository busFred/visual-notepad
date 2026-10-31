#include "Highlighter.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Format>
#include <KSyntaxHighlighting/State>
#include <KSyntaxHighlighting/Theme>

#include <QAbstractTextDocumentLayout>
#include <QElapsedTimer>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QTimer>

#include <algorithm>
#include <climits>

namespace vn {

namespace {

// Each slice colours for about this long, then lets the event loop run
// (typing, scrolling, painting) before the next.
constexpr qint64 sliceBudgetNs = 6'000'000;
// Lines a view shows are coloured from this far above, so that a code block
// or comment opened just before them is usually seen.
constexpr int leadInLines = 50;

bool &sliced()
{
    static bool on = false;
    return on;
}

// A block's line state: what the next line starts from.
struct BlockState : QTextBlockUserData
{
    explicit BlockState(KSyntaxHighlighting::State s)
        : state(std::move(s))
    {
    }
    KSyntaxHighlighting::State state;
    // Coloured ahead of the in-order pass, maybe from a wrong start.
    bool ahead = false;
};

BlockState *stateOf(const QTextBlock &block)
{
    return static_cast<BlockState *>(block.userData());
}

} // namespace

Highlighter::Highlighter(QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    m_timer->setSingleShot(true);
    m_timer->setInterval(0);
    connect(m_timer, &QTimer::timeout, this, &Highlighter::runSlice);
}

Highlighter::~Highlighter()
{
    // Without the colours: the document may be going too.
    stopSlices();
    if (m_doc)
        disconnect(m_doc, nullptr, this, nullptr);
}

void Highlighter::setSliced(bool on)
{
    sliced() = on;
}

bool Highlighter::isSliced()
{
    return sliced();
}

bool Highlighter::slicesFor(int chars) const
{
    return sliced() && chars > slicedFromChars;
}

void Highlighter::setVisibleLines(std::function<QList<std::pair<int, int>>()> lines)
{
    m_visibleLines = std::move(lines);
}

void Highlighter::attach(QTextDocument *doc)
{
    if (m_doc == doc)
        return;
    detach();
    if (!doc)
        return;
    m_doc = doc;
    connect(doc, &QTextDocument::contentsChange, this, &Highlighter::onContentsChange);
    connect(doc, &QTextDocument::contentsChanged, this, &Highlighter::onContentsChanged);
    recolourAll();
}

void Highlighter::detach()
{
    stopSlices();
    m_sliceAfterEdit = false;
    if (!m_doc)
        return;
    disconnect(m_doc, nullptr, this, nullptr);
    clearAll();
    settle();
    QTextDocument *doc = m_doc;
    m_doc = nullptr;
    Q_EMIT highlighted(0, doc->blockCount() - 1);
}

void Highlighter::setDefinition(const KSyntaxHighlighting::Definition &def)
{
    const bool changed = definition() != def;
    AbstractHighlighter::setDefinition(def);
    if (!changed)
        return;
    m_formats.clear();
    if (m_doc)
        recolourAll();
}

void Highlighter::setTheme(const KSyntaxHighlighting::Theme &theme)
{
    AbstractHighlighter::setTheme(theme);
    m_formats.clear();
    if (m_doc)
        recolourAll();
}

void Highlighter::clearAll()
{
    for (QTextBlock block = m_doc->begin(); block.isValid(); block = block.next()) {
        if (!block.layout()->formats().isEmpty())
            block.layout()->clearFormats();
        block.setUserData(nullptr);
    }
}

void Highlighter::settle()
{
    // QTextLayout::setFormats() records a change of the block in the
    // document, which the next edit would report as its own: every block
    // coloured since, as if edited (a 5 MB recolouring on the first
    // keystroke). Reported now instead, to nobody: the views hear of the
    // colours through highlighted(). Never inside an edit's notification,
    // where it would widen what the edit reports.
    const QSignalBlocker layoutBlocker(m_doc->documentLayout());
    m_doc->markContentsDirty(0, 0);
}

void Highlighter::recolourAll()
{
    stopSlices();
    clearAll();
    if (slicesFor(m_doc->characterCount())) {
        m_frontier = QTextCursor(m_doc);
        settle();
        Q_EMIT highlighted(0, m_doc->blockCount() - 1);
        runSlice();
        return;
    }
    for (QTextBlock block = m_doc->begin(); block.isValid(); block = block.next())
        colour(block, true);
    settle();
    Q_EMIT highlighted(0, m_doc->blockCount() - 1);
}

const QTextCharFormat &Highlighter::textFormat(const KSyntaxHighlighting::Format &format)
{
    // What KSyntaxHighlighting::SyntaxHighlighter makes of a format.
    const int id = format.isValid() ? format.id() : 0;
    auto it = m_formats.find(id);
    if (it != m_formats.end())
        return *it;
    const KSyntaxHighlighting::Theme t = theme();
    QTextCharFormat tf;
    // Always a foreground, so the palette never shows through.
    tf.setForeground(format.textColor(t));
    if (format.hasBackgroundColor(t))
        tf.setBackground(format.backgroundColor(t));
    if (format.isBold(t))
        tf.setFontWeight(QFont::Bold);
    if (format.isItalic(t))
        tf.setFontItalic(true);
    if (format.isUnderline(t))
        tf.setFontUnderline(true);
    if (format.isStrikeThrough(t))
        tf.setFontStrikeOut(true);
    return *m_formats.insert(id, tf);
}

void Highlighter::applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format)
{
    if (length == 0)
        return;
    const QTextCharFormat &tf = textFormat(format);
    // One range for neighbours alike, as QSyntaxHighlighter makes them.
    if (!m_ranges.isEmpty()) {
        QTextLayout::FormatRange &last = m_ranges.last();
        if (last.start + last.length == offset && last.format == tf) {
            last.length += length;
            return;
        }
    }
    m_ranges.append({offset, length, tf});
}

bool Highlighter::colour(const QTextBlock &block, bool inOrder)
{
    const BlockState *before = stateOf(block.previous());
    m_ranges.clear();
    const QString text = block.text();
    KSyntaxHighlighting::State state = highlightLine(QStringView(text).left(maxLineChars),
                                                     before ? before->state : KSyntaxHighlighting::State());
    QTextLayout *layout = block.layout();
    if (layout->formats() != m_ranges)
        layout->setFormats(m_ranges);
    BlockState *data = stateOf(block);
    if (!data) {
        data = new BlockState(std::move(state));
        data->ahead = !inOrder;
        QTextBlock(block).setUserData(data);
        return true;
    }
    if (inOrder)
        data->ahead = false;
    if (data->state == state)
        return false;
    data->state = std::move(state);
    return true;
}

void Highlighter::onContentsChange(int from, int charsRemoved, int charsAdded)
{
    const int frontier = m_frontier.isNull() ? INT_MAX : m_frontier.position();
    QTextBlock block = m_doc->findBlock(from);
    const QTextBlock lastBlock = m_doc->findBlock(from + charsAdded + (charsRemoved > 0 ? 1 : 0));
    const int end = lastBlock.isValid() ? lastBlock.position() + lastBlock.length() : m_doc->characterCount();

    if (slicesFor(charsAdded)) {
        // A big edit (a reload, a paste, undoing one) is coloured like a big
        // text: by the slices, the first one as soon as the edit is
        // complete. Its blocks count as not coloured yet.
        moveFrontierBack(block.position());
        for (; block.isValid() && block.position() < end; block = block.next()) {
            if (!block.layout()->formats().isEmpty())
                block.layout()->clearFormats();
            block.setUserData(nullptr);
        }
        m_sliceAfterEdit = true;
        return;
    }

    // As QSyntaxHighlighter: the blocks the edit touched, then on while a
    // line ends in another state than it did. Inside the change
    // notification, the layout hears of the blocks recoloured past the edit
    // along with it (QTextLayout::setFormats() records them).
    QElapsedTimer clock;
    clock.start();
    bool changed = false;
    while (block.isValid() && (block.position() < end || changed)) {
        if (block.position() >= end) {
            // Not coloured, or the in-order pass will be there anyway.
            if (!block.userData() || block.position() >= frontier)
                break;
            if (sliced() && clock.nsecsElapsed() > sliceBudgetNs) {
                moveFrontierBack(block.position());
                m_timer->start();
                break;
            }
        }
        changed = colour(block, block.position() < frontier);
        block = block.next();
    }
}

void Highlighter::onContentsChanged()
{
    if (m_sliceAfterEdit) {
        m_sliceAfterEdit = false;
        runSlice();
    }
}

void Highlighter::moveFrontierBack(int position)
{
    if (m_frontier.isNull()) {
        m_doneToEnd = true;
        m_frontier = QTextCursor(m_doc);
        m_frontier.setPosition(position);
        return;
    }
    if (position >= m_frontier.position())
        return;
    if (!m_doneToEnd && m_doneTo.isNull())
        m_doneTo = m_frontier;
    m_frontier.setPosition(position);
}

void Highlighter::runSlice()
{
    if (!m_doc || m_frontier.isNull())
        return;
    QElapsedTimer clock;
    clock.start();
    int first = INT_MAX;
    int last = -1;
    const auto colourLine = [&](const QTextBlock &block, int number, bool inOrder) {
        const bool changed = colour(block, inOrder);
        first = std::min(first, number);
        last = std::max(last, number);
        return changed;
    };

    QTextBlock next = m_doc->findBlock(m_frontier.position());
    int number = next.blockNumber();
    // What the views show first, where the pass has not been. Only blocks
    // without a state: one that has was coloured since.
    if (m_visibleLines) {
        for (const auto &[top, bottom] : m_visibleLines()) {
            int line = std::max(top - leadInLines, number);
            for (QTextBlock block = m_doc->findBlockByNumber(line); block.isValid() && line <= bottom;
                 block = block.next(), ++line) {
                if (!block.userData())
                    colourLine(block, line, false);
            }
        }
    }
    // Then on in order.
    while (next.isValid() && clock.nsecsElapsed() < sliceBudgetNs) {
        const BlockState *data = stateOf(next);
        const bool wasInOrder = data && !data->ahead;
        const bool changed = colourLine(next, number, true);
        if (wasInOrder && !changed && (m_doneToEnd || (!m_doneTo.isNull() && next.position() < m_doneTo.position()))) {
            // Back where the text was coloured in order, and this line ends
            // as it did: so does everything after it, up to where that was.
            if (m_doneToEnd) {
                next = QTextBlock();
                break;
            }
            next = m_doc->findBlock(m_doneTo.position());
            number = next.blockNumber();
            m_doneTo = QTextCursor();
            continue;
        }
        next = next.next();
        ++number;
        if (!m_doneTo.isNull() && next.isValid() && next.position() >= m_doneTo.position())
            m_doneTo = QTextCursor();
    }
    if (next.isValid())
        m_frontier.setPosition(next.position());
    else
        stopSlices();
    settle();
    if (last >= 0)
        Q_EMIT highlighted(first, last);
    if (!m_frontier.isNull())
        m_timer->start();
}

void Highlighter::stopSlices()
{
    m_timer->stop();
    m_frontier = QTextCursor();
    m_doneTo = QTextCursor();
    m_doneToEnd = false;
}

} // namespace vn
