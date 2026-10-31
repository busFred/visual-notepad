// The source view's syntax highlighter: KSyntaxHighlighting's highlighting
// engine on a QTextDocument, like its SyntaxHighlighter, but able to colour a
// big text in slices from the event loop rather than all at once.
//
// QSyntaxHighlighter (which SyntaxHighlighter is) colours a whole document
// when it is attached, and every block an edit touches, synchronously: 0.6 s
// for 5 MB of Markdown, during which the window is frozen; it cannot be
// attached to a document with text without queueing exactly that. This class
// keeps the colours (QTextLayout formats) and line states (block user data)
// itself. With slicing on (setSliced), texts and edits above slicedFromChars
// are coloured a few milliseconds at a time instead, in order from the top
// (a line's colours follow from the state the previous one ended in), the
// lines a view shows first. Lines coloured ahead of the in-order pass, or
// edited ahead of it, are coloured again when it gets there, so the end
// result is what colouring everything at once gives.
//
// An edit recolours the blocks it touched, and the ones after for as long
// as a line ends in another state than it did (an opened comment); with
// slicing on, a long run of those goes on in slices.
//
// Lines longer than maxLineChars are coloured up to there only: a line is
// one call into the engine that cannot be split, and a 1 MB line takes two
// seconds, again on every keystroke in it (VS Code stops at 20,000
// characters, editor.maxTokenizationLineLength).
#pragma once

#include <KSyntaxHighlighting/AbstractHighlighter>

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>

#include <functional>
#include <utility>

class QTextBlock;
class QTimer;

namespace vn {

class Highlighter : public QObject, public KSyntaxHighlighting::AbstractHighlighter
{
    Q_OBJECT

public:
    explicit Highlighter(QObject *parent = nullptr);
    ~Highlighter() override;

    // Off by default: everything is coloured at once. The application turns
    // it on; tests that look at every colour right away leave it off.
    static void setSliced(bool on);
    static bool isSliced();
    static constexpr int slicedFromChars = 128 * 1024;
    static constexpr int maxLineChars = 10'000;

    // Colours `doc` (at once, or the first slice now and the rest later) and
    // keeps it coloured while it is edited.
    void attach(QTextDocument *doc);
    // Drops the colours and stops colouring.
    void detach();
    QTextDocument *document() const { return m_doc; }
    // Whether slices are still to come.
    bool isPending() const { return !m_frontier.isNull(); }

    // Line ranges (first, last; block numbers) the views show; they are
    // coloured before the rest.
    void setVisibleLines(std::function<QList<std::pair<int, int>>()> lines);

    void setDefinition(const KSyntaxHighlighting::Definition &def) override;
    void setTheme(const KSyntaxHighlighting::Theme &theme) override;

Q_SIGNALS:
    // Lines [first, last] have new colours, other than through an edit
    // (whose layout notification covers what it recoloured).
    void highlighted(int first, int last);

protected:
    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format) override;

private:
    // Colours one block from the state the previous one ended in. In order
    // means from the in-order pass or behind it; other blocks are marked as
    // coloured ahead. Returns whether the block had no state before or ends
    // in another one now.
    bool colour(const QTextBlock &block, bool inOrder);
    // Colours everything again: at once, or from the top in slices.
    void recolourAll();
    void clearAll();
    void settle();
    void onContentsChange(int from, int charsRemoved, int charsAdded);
    void onContentsChanged();
    void runSlice();
    // The in-order pass goes on from `position` (a block start) if that is
    // before where it is.
    void moveFrontierBack(int position);
    void stopSlices();
    bool slicesFor(int chars) const;
    const QTextCharFormat &textFormat(const KSyntaxHighlighting::Format &format);

    QPointer<QTextDocument> m_doc;
    std::function<QList<std::pair<int, int>>()> m_visibleLines;
    QTimer *m_timer;
    // Where the in-order pass goes on; null when there is none.
    QTextCursor m_frontier;
    // Up to where (or, with m_doneToEnd, to the end) the text was coloured in
    // order before an edit moved the pass back: there it may stop as soon as
    // a line ends as it did.
    QTextCursor m_doneTo;
    bool m_doneToEnd = false;
    // A big edit is complete when contentsChanged() follows; the first slice
    // runs then.
    bool m_sliceAfterEdit = false;
    // applyFormat()'s output for the block being coloured.
    QList<QTextLayout::FormatRange> m_ranges;
    // Qt formats by KSyntaxHighlighting format id (0: the default format).
    QHash<int, QTextCharFormat> m_formats;
};

} // namespace vn
