// The layout of one source line, as TextView measures, paints and hit-tests
// it.
//
// A line is one QTextLayout, made by the view as before (whole()). A very
// long one (minified JSON, a log on one line: more than piecesFrom
// characters) is laid out in pieces of a few thousand characters instead,
// each its own QTextLayout: an edit lays out again the piece it touched, not
// the whole line (half a second a keystroke on a 3 MB line), and painting
// draws the pieces in sight. With Word Wrap off the pieces sit side by side
// on one row; with it on each piece wraps on its own, so each piece starts a
// row (a row can end at a join a little before where the whole line would
// have wrapped it).
//
// Joins go where nothing reads across them: between two characters that
// are not one grapheme and do not shape together, after a tab, a space or
// punctuation if there is one near the size wanted, never inside a
// right-to-left run (the nearest strong characters each side are
// left-to-right). Tabs keep their stops: a piece that does not start right
// after a tab (on the line's tab grid) holds a tab only as its last
// character, whose width runs to the stop the whole line has there. A line
// whose layout cannot be cut like that (a right-to-left one, explicit
// direction marks, line separators, a right-to-left run with no join for
// thousands of characters) is laid out whole.
//
// Only the layouts of the pieces used last are kept; the others keep their
// text and measures and are laid out again when needed.
#pragma once

#include <QFont>
#include <QList>
#include <QString>
#include <QTextLayout>
#include <QTextOption>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

class QPainter;
class QTextBlock;

namespace vn {

class LineLayout
{
public:
    // Lines longer than this are laid out in pieces.
    static constexpr int piecesFrom = 16 * 1024;

    // How the view lays its lines out.
    struct Setup
    {
        QFont font;
        QTextOption option;
        bool wrap = false;
        qreal wrapWidth = 0; // The width rows wrap at (a huge one with Word Wrap off).
        qreal lineHeight = 0;
        qreal linePad = 0;
    };

    // A row of the line, as QTextLine is one of a QTextLayout. Positions are
    // in the line (with the text being composed, if any, as QTextLayout has
    // them); x from the line's left.
    class Line
    {
    public:
        Line() = default;
        bool isValid() const { return m_layout != nullptr; }
        int lineNumber() const { return m_row; }
        int textStart() const;
        int textLength() const;
        qreal y() const;
        qreal ascent() const;
        qreal naturalTextWidth() const;
        qreal cursorToX(int position) const;
        int xToCursor(qreal x) const;

    private:
        friend class LineLayout;
        Line(LineLayout *layout, int row)
            : m_layout(layout)
            , m_row(row)
        {
        }
        LineLayout *m_layout = nullptr;
        int m_row = 0;
    };

    LineLayout();
    ~LineLayout();
    LineLayout(const LineLayout &) = delete;
    LineLayout &operator=(const LineLayout &) = delete;

    // The line laid out whole: the caller fills the QTextLayout in. Drops
    // any pieces.
    QTextLayout *whole();
    bool inPieces() const { return !m_whole; }

    // Lays the block out in pieces. False if it cannot be cut (see above):
    // nothing is kept then.
    bool build(const QTextBlock &block, const Setup &setup);
    // The block after an edit: its first `prefix` characters are the first
    // ones of `before`, its last `suffix` the last ones of `after` (either
    // may be null, or both the same). Their pieces clear of the edit are
    // taken over, the rest is cut and laid out anew. False as build().
    bool rebuild(const QTextBlock &block, const Setup &setup, LineLayout *before, int prefix, LineLayout *after,
                 int suffix);
    // How many characters at the start and at the end the text has in
    // common with the line as laid out (not overlapping).
    std::pair<int, int> common(const QString &text) const;
    // The highlighter coloured the block again: the pieces its colours
    // reach (or reached) are laid out again.
    void formatsChanged(const QTextBlock &block);
    // Text being composed at a position of the line; an empty text ends it.
    void setPreedit(int position, const QString &text, const QList<QTextLayout::FormatRange> &formats);
    bool hasPreedit() const { return m_preeditPiece >= 0; }

    // Reuse text and cuts across changes of width/font/options.
    bool resetup(const QTextBlock &block, const Setup &setup);
    using Listener = std::function<void(LineLayout *, int, int)>;
    void setListener(Listener listener) { m_listener = std::move(listener); }
    struct Settlement { bool measured = false; int extraTopRows = 0; };
    // A stable text anchor for repeated settlement of a window.
    int windowAnchor(int topRow, int endRow) const;
    // anchor: text position; -1 = automatic, -2 = top, INT_MAX = bottom.
    Settlement settleWindowStep(int topRow, int endRow, int anchor = -1);
    void settleAround(int position);
    void settleTo(qreal x);
    bool settleOne(int nearRow);
    int unmeasuredCount() const { return m_unmeasured; }
    quint64 generation() const { return m_generation; }
    bool wraps() const { return m_setup.wrap; }
    const Setup &setup() const { return m_setup; }
    bool hasSetup(const Setup &setup) const;
    qreal lineHeight() const { return m_setup.lineHeight; }

    int length() const { return m_length; } // Without the text being composed.
    // Where the pieces start (for tests).
    QList<int> pieceStarts() const;
    int lineCount() const;
    Line lineAt(int row);
    Line lineForTextPosition(int position);
    // Word Wrap off: the width of the row.
    qreal width() const;
    // The positions the given rows can show between x1 and x2 (all of the
    // rows for a line laid out whole: only pieces narrow them).
    std::pair<int, int> visibleRange(int firstRow, int endRow, qreal x1, qreal x2);

    void draw(QPainter *painter, const QPointF &origin, const QList<QTextLayout::FormatRange> &selections,
              const QRectF &clip);
    void drawCursor(QPainter *painter, const QPointF &origin, int position, int width);

private:
    struct Piece;
    class Cutter;

    void clearPieces();
    int layoutStart(int piece) const;
    int layoutLength(int piece) const;
    int pieceAt(int position) const;
    int pieceAtX(qreal x) const;
    int pieceAtRow(int row) const;
    // The piece laid out (and recently used).
    QTextLayout *use(int piece);
    // Word Wrap on: the row's line in its piece, laid out (a row past the
    // piece's measured rows gives its last one).
    QTextLine rowLine(int row, int *piece = nullptr);
    // Lays the piece out and takes its measures (keeping the layout or
    // not); true if they changed.
    bool layOut(int piece, bool keep);
    void measure(int piece, bool keep);
    void estimate(Piece &piece);
    void trim(int keep);
    // Positions and rows from the piece on.
    void place(int from);
    bool finish();
    QList<QTextLayout::FormatRange> formatsFor(int start, int length) const;
    qreal tabStop(qreal x) const;

    std::unique_ptr<QTextLayout> m_whole;
    std::vector<Piece> m_pieces;
    Setup m_setup;
    Listener m_listener;
    qreal m_ref = 1, m_fill = 0.9;
    qreal m_em = 0, m_chars = 0, m_measuredWidth = 0;
    int m_measuredRows = 0, m_unmeasured = 0;
    quint64 m_generation = 0;
    QList<QTextLayout::FormatRange> m_formats; // The block's, from the highlighter.
    int m_formatsEnd = 0;
    int m_length = 0;
    quint64 m_clock = 0;
    int m_live = 0;
    // The piece holding the text being composed.
    int m_preeditPiece = -1;
    int m_preeditPosition = 0; // In the piece.
    QString m_preedit;
    QList<QTextLayout::FormatRange> m_preeditFormats; // In the piece.
};

} // namespace vn
