#include "LineLayout.h"

#include <QPainter>
#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <QtMath>

#include <algorithm>
#include <climits>

namespace vn {

namespace {

// Pieces are about this long; what is left at the end, up to pieceMax, is
// not cut.
constexpr int pieceTarget = 4096;
constexpr int pieceMax = pieceTarget + pieceTarget / 2;
// How far each way from a join its nearest strong characters are looked
// for (a right-to-left run could reach across it).
constexpr int strongReach = 256;
// The text read around a stretch being cut, for that and for graphemes; an
// edit cuts again the pieces it comes this close to (it can make their
// joins unsafe).
constexpr int context = strongReach + 64;
// Laid-out pieces kept per line.
constexpr int maxLive = 64;

enum class Strong : char { None, Left, Right };

struct Scan
{
    bool rtl = false; // Right-to-left characters (Arabic digits included).
    bool blocks = false; // Something that keeps the line whole.
    Strong strong = Strong::None; // The first strong direction.
};

Scan scan(QStringView text)
{
    Scan s;
    const qsizetype size = text.size();
    for (qsizetype i = 0; i < size; ++i) {
        char32_t c = text[i].unicode();
        if (c < 0x0590) { // No right-to-left or explicit direction below Hebrew.
            if (s.strong == Strong::None && QChar::direction(c) == QChar::DirL)
                s.strong = Strong::Left;
            continue;
        }
        if (QChar::isHighSurrogate(c) && i + 1 < size && text[i + 1].isLowSurrogate())
            c = QChar::surrogateToUcs4(char16_t(c), text[++i].unicode());
        if (c == QChar::LineSeparator) {
            s.blocks = true;
            continue;
        }
        switch (QChar::direction(c)) {
        case QChar::DirL:
            if (s.strong == Strong::None)
                s.strong = Strong::Left;
            break;
        case QChar::DirR:
        case QChar::DirAL:
            if (s.strong == Strong::None)
                s.strong = Strong::Right;
            s.rtl = true;
            break;
        case QChar::DirAN:
            s.rtl = true;
            break;
        case QChar::DirLRE:
        case QChar::DirLRO:
        case QChar::DirRLE:
        case QChar::DirRLO:
        case QChar::DirPDF:
        case QChar::DirLRI:
        case QChar::DirRLI:
        case QChar::DirFSI:
        case QChar::DirPDI:
            s.blocks = true;
            break;
        default:
            break;
        }
    }
    return s;
}

// Characters that never shape with a neighbour: Latin, Greek, Cyrillic and
// the like, and Chinese, Japanese and Korean.
bool simple(char16_t c)
{
    return c < 0x0590 || (c >= 0x3040 && c < 0xA000) || (c >= 0xAC00 && c < 0xD7A4) || (c >= 0xF900 && c < 0xFB00)
           || (c >= 0xFF00 && c < 0xFFF0);
}

// Part of a block's text. (Not with a QTextCursor: setting its position
// itemizes the whole block for the cursor's x.)
QString textOf(const QTextBlock &block, int from, int to)
{
    const QTextDocument *document = block.document();
    const int base = block.position();
    QString text(to - from, Qt::Uninitialized);
    for (int i = from; i < to; ++i)
        text[i - from] = document->characterAt(base + i);
    return text;
}

bool sameSetup(const LineLayout::Setup &a, const LineLayout::Setup &b)
{
    return a.wrap == b.wrap && a.wrapWidth == b.wrapWidth && a.lineHeight == b.lineHeight && a.linePad == b.linePad
           && a.font == b.font && a.option.tabStopDistance() == b.option.tabStopDistance()
           && a.option.flags() == b.option.flags() && a.option.wrapMode() == b.option.wrapMode();
}

} // namespace

struct LineLayout::Piece
{
    int start = 0; // In the line.
    QString text;
    // Word Wrap off: the layout's width, and where a last tab starts in it
    // (-1 if the piece does not end with one); where the piece starts in
    // the row and how wide it is there (a last tab to its tab stop).
    qreal em = -1; // Measured total advance in average-character units.
    qreal natural = 0;
    qreal tabX = -1;
    qreal x = 0;
    qreal width = 0;
    // Word Wrap on: its first row in the line, and how many.
    int firstRow = 0;
    int rows = 1;
    Scan scan;
    bool measured = false;
    enum class Reuse { New, Same, Other };
    Reuse reuse = Reuse::New;
    // The highlighter's colours it was laid out with (positions in it).
    QList<QTextLayout::FormatRange> formats;
    std::unique_ptr<QTextLayout> layout; // Dropped when not used for a while.
    quint64 used = 0;
};

// Where the pieces of a stretch of the line end. The text is the stretch
// with some context each side; positions are in the line.
class LineLayout::Cutter
{
public:
    Cutter(QStringView text, int offset, int lineLength)
        : m_text(text)
        , m_offset(offset)
        , m_lineLength(lineLength)
        , m_rtl(scan(text).rtl)
    {
    }

    // The end of the piece from start, end at most (both joins); -1 if no
    // join will do.
    int next(int start, int end) const
    {
        // A piece that does not start on the tab grid holds a tab only as
        // its last character.
        int limit = end;
        if (start > 0 && at(start - 1) != u'\t') {
            const int tab = firstTab(start);
            if (tab + 1 < end)
                limit = tab + 1;
        }
        if (limit == end && end - start <= pieceMax)
            return end;
        // Near the size wanted, best first: after a tab, a space,
        // punctuation, then between two simple characters.
        const int hi = qMin(start + pieceTarget, limit);
        const int lo = qMin(start + pieceTarget * 3 / 4, hi);
        int best = -1;
        int bestRank = -1;
        for (int p = hi; p >= lo && bestRank < 3; --p) {
            const int r = rank(p);
            if (r > bestRank && safe(p)) {
                best = p;
                bestRank = r;
            }
        }
        if (best > start)
            return best;
        // None there: the first join after, else the last before.
        for (int p = hi + 1; p < limit; ++p) {
            if (rank(p) >= 0 && safe(p))
                return p;
        }
        if (limit == end)
            return end;
        if (safe(limit))
            return limit;
        for (int p = lo - 1; p > start; --p) {
            if (rank(p) >= 0 && safe(p))
                return p;
        }
        return -1;
    }

private:
    char16_t at(int position) const { return m_text[position - m_offset].unicode(); }

    // The first tab from a position on (INT_MAX - 1 if none). Pieces are
    // cut in order, so the last answer usually stands.
    int firstTab(int from) const
    {
        if (m_tabFrom < 0 || from < m_tabFrom || m_tab < from) {
            const qsizetype i = m_text.indexOf(u'\t', from - m_offset);
            m_tab = i < 0 ? INT_MAX - 1 : m_offset + int(i);
            m_tabFrom = from;
        }
        return m_tab;
    }

    int rank(int p) const
    {
        const char16_t before = at(p - 1);
        if (before == u'\t')
            return 3;
        if (before == u' ')
            return 2;
        if (before > 0x20 && before < 0x7f && !QChar::isLetterOrNumber(before))
            return 1;
        return simple(before) && simple(at(p)) ? 0 : -1;
    }

    bool safe(int p) const { return graphemeBoundary(p) && clearOfRtl(p); }

    bool graphemeBoundary(int p) const
    {
        const char16_t a = at(p - 1);
        const char16_t b = at(p);
        if (a < 0x80 && b < 0x80)
            return !(a == u'\r' && b == u'\n');
        if (QChar::isLowSurrogate(b))
            return false;
        const int from = qMax(m_offset, p - 64);
        const int to = qMin(m_offset + int(m_text.size()), p + 64);
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, m_text.mid(from - m_offset, to - from));
        finder.setPosition(p - from);
        return finder.isAtBoundary();
    }

    // Whether the nearest strong characters each side are left-to-right
    // (or the line's ends) with nothing right-to-left between: then the
    // bidi algorithm gives everything around the join the same level in a
    // piece as in the whole line.
    bool clearOfRtl(int p) const
    {
        if (!m_rtl)
            return true;
        const auto direction = [this](int &i, bool back) {
            char32_t c = at(i);
            if (back && QChar::isLowSurrogate(c) && i - 1 >= m_offset && QChar::isHighSurrogate(at(i - 1)))
                c = QChar::surrogateToUcs4(at(--i), char16_t(c));
            else if (!back && QChar::isHighSurrogate(c) && i + 1 < m_offset + m_text.size() && QChar::isLowSurrogate(at(i + 1)))
                c = QChar::surrogateToUcs4(char16_t(c), at(++i));
            return QChar::direction(c);
        };
        const auto rtl = [](QChar::Direction d) {
            return d == QChar::DirR || d == QChar::DirAL || d == QChar::DirAN;
        };
        for (int i = p - 1, n = 0;; --i, ++n) {
            if (i < 0)
                break;
            if (i < m_offset || n >= strongReach)
                return false;
            const QChar::Direction d = direction(i, true);
            if (d == QChar::DirL)
                break;
            if (rtl(d))
                return false;
        }
        for (int i = p, n = 0;; ++i, ++n) {
            if (i >= m_lineLength)
                break;
            if (i >= m_offset + m_text.size() || n >= strongReach)
                return false;
            const QChar::Direction d = direction(i, false);
            if (d == QChar::DirL)
                break;
            if (rtl(d))
                return false;
        }
        return true;
    }

    QStringView m_text;
    int m_offset;
    int m_lineLength;
    bool m_rtl;
    mutable int m_tabFrom = -1;
    mutable int m_tab = 0;
};

// ---------------------------------------------------------------------------
// Rows

int LineLayout::Line::textStart() const
{
    LineLayout &l = *m_layout;
    if (l.m_whole)
        return l.m_whole->lineAt(m_row).textStart();
    if (!l.m_setup.wrap)
        return 0;
    int k = 0;
    const QTextLine line = l.rowLine(m_row, &k);
    return l.layoutStart(k) + line.textStart();
}

int LineLayout::Line::textLength() const
{
    LineLayout &l = *m_layout;
    if (l.m_whole)
        return l.m_whole->lineAt(m_row).textLength();
    if (!l.m_setup.wrap)
        return l.m_length + (l.m_preeditPiece >= 0 ? int(l.m_preedit.size()) : 0);
    return l.rowLine(m_row).textLength();
}

qreal LineLayout::Line::y() const
{
    const LineLayout &l = *m_layout;
    if (l.m_whole)
        return l.m_whole->lineAt(m_row).y();
    return l.m_setup.linePad + m_row * l.m_setup.lineHeight;
}

qreal LineLayout::Line::ascent() const
{
    LineLayout &l = *m_layout;
    if (l.m_whole)
        return l.m_whole->lineAt(m_row).ascent();
    return (l.m_setup.wrap ? l.rowLine(m_row) : l.use(0)->lineAt(0)).ascent();
}

qreal LineLayout::Line::naturalTextWidth() const
{
    LineLayout &l = *m_layout;
    if (l.m_whole)
        return l.m_whole->lineAt(m_row).naturalTextWidth();
    if (!l.m_setup.wrap)
        return l.width();
    return l.rowLine(m_row).naturalTextWidth();
}

qreal LineLayout::Line::cursorToX(int position) const
{
    LineLayout &l = *m_layout;
    if (l.m_whole)
        return l.m_whole->lineAt(m_row).cursorToX(position);
    if (l.m_setup.wrap) {
        int k = 0;
        const QTextLine line = l.rowLine(m_row, &k);
        return line.cursorToX(position - l.layoutStart(k));
    }
    const int at = l.pieceAt(position);
    if (at > 0 && position == l.layoutStart(at)) {
        // A selection clipped at a piece join only needs its left edge.
        // The next piece may be outside the paint clip and still estimated.
        l.settleAround(position - 1);
        return l.m_pieces[at].x;
    }
    l.settleAround(position);
    const int k = l.pieceAt(position);
    const Piece &p = l.m_pieces[k];
    const int local = qMax(0, position - l.layoutStart(k));
    if (local >= l.layoutLength(k))
        return p.x + p.width;
    return p.x + l.use(k)->lineAt(0).cursorToX(local);
}

int LineLayout::Line::xToCursor(qreal x) const
{
    LineLayout &l = *m_layout;
    if (l.m_whole)
        return l.m_whole->lineAt(m_row).xToCursor(x);
    if (l.m_setup.wrap) {
        int k = 0;
        const QTextLine line = l.rowLine(m_row, &k);
        return l.layoutStart(k) + line.xToCursor(x);
    }
    l.settleTo(x);
    const int k = l.pieceAtX(x);
    const Piece &p = l.m_pieces[k];
    const qreal local = x - p.x;
    if (p.tabX >= 0 && local >= p.tabX) {
        // The last tab, as wide as the line has it: its nearer edge.
        const int end = l.layoutStart(k) + l.layoutLength(k);
        return local - p.tabX < (p.width - p.tabX) / 2 ? end - 1 : end;
    }
    return l.layoutStart(k) + l.use(k)->lineAt(0).xToCursor(local);
}

// ---------------------------------------------------------------------------
// The line

LineLayout::LineLayout() = default;
LineLayout::~LineLayout() = default;

QTextLayout *LineLayout::whole()
{
    if (!m_whole) {
        clearPieces();
        m_whole = std::make_unique<QTextLayout>();
    }
    return m_whole.get();
}

void LineLayout::clearPieces()
{
    m_pieces.clear();
    m_formats.clear();
    m_formatsEnd = 0;
    m_length = 0;
    m_live = 0;
    m_unmeasured = 0;
    m_em = m_chars = m_measuredWidth = 0;
    m_measuredRows = 0;
    m_preeditPiece = -1;
    m_preedit.clear();
    m_preeditFormats.clear();
}

bool LineLayout::build(const QTextBlock &block, const Setup &setup)
{
    return rebuild(block, setup, nullptr, 0, nullptr, 0);
}

bool LineLayout::rebuild(const QTextBlock &block, const Setup &setup, LineLayout *before, int prefix,
                         LineLayout *after, int suffix)
{
    m_whole.reset();
    clearPieces();
    const auto fail = [this] {
        clearPieces();
        return false;
    };
    m_setup = setup;
    m_ref = qMax<qreal>(0.01, QFontMetricsF(setup.font).averageCharWidth());
    m_formats = block.layout()->formats();
    for (const QTextLayout::FormatRange &f : std::as_const(m_formats))
        m_formatsEnd = qMax(m_formatsEnd, f.start + f.length);
    const int n = block.length() - 1;
    m_length = n;

    const auto usable = [&](LineLayout *l) { return l && l->inPieces(); };
    if (!usable(before))
        before = nullptr;
    if (!usable(after))
        after = nullptr;
    m_fill = before ? before->m_fill : after ? after->m_fill : 0.9;
    // Keep stale pieces too; their measures become estimates below.
    // Their pieces as the text has them, without text being composed.
    if (before)
        before->setPreedit(0, {}, {});
    if (after)
        after->setPreedit(0, {}, {});
    int formatsReach = m_formatsEnd;

    // The pieces clear of the edit, at their places in the line.
    std::vector<Piece> kept;
    if (before) {
        formatsReach = qMax(formatsReach, before->m_formatsEnd);
        const bool unchanged = prefix >= n && n == before->m_length;
        for (Piece &p : before->m_pieces) {
            if (!unchanged && p.start + int(p.text.size()) > prefix - context)
                break;
            p.reuse = sameSetup(before->m_setup, setup) ? Piece::Reuse::Same : Piece::Reuse::Other;
            if (p.reuse == Piece::Reuse::Other) {
                p.layout.reset();
                p.measured = false;
            }
            kept.push_back(std::move(p));
        }
    }
    if (after) {
        formatsReach = qMax(formatsReach, after->m_formatsEnd);
        const bool unchanged = suffix >= n && n == after->m_length;
        const int from = after->m_length - suffix; // Where the common end starts in it.
        for (Piece &p : after->m_pieces) {
            if (p.text.isNull()) // Taken above (the same line).
                continue;
            if (unchanged || p.start >= from + context) {
                p.reuse = sameSetup(after->m_setup, setup) ? Piece::Reuse::Same : Piece::Reuse::Other;
                if (p.reuse == Piece::Reuse::Other) {
                    p.layout.reset();
                    p.measured = false;
                }
                kept.push_back(std::move(p));
                kept.back().start += n - after->m_length;
            }
        }
    }

    // The stretches between them are cut anew; one too short for a piece
    // takes a neighbour along.
    std::vector<Piece> pieces;
    pieces.reserve(kept.size() + 4);
    int pos = 0;
    size_t next = 0;
    const QString whole = kept.empty() ? block.text() : QString();
    while (pos < n) {
        if (next < kept.size() && kept[next].start == pos) {
            pos += int(kept[next].text.size());
            pieces.push_back(std::move(kept[next++]));
            continue;
        }
        int end = next < kept.size() ? kept[next].start : n;
        while (end - pos < pieceTarget / 2 && next < kept.size()) {
            // (Not in one expression: which side of + reads `next` first is
            // up to the compiler, and MSVC took the next piece's start.)
            end = kept[next].start + int(kept[next].text.size());
            ++next;
        }
        if (end - pos < pieceTarget / 2 && !pieces.empty()) {
            pos = pieces.back().start;
            pieces.pop_back();
        }
        const int from = whole.isNull() ? qMax(0, pos - context) : 0;
        const int to = whole.isNull() ? qMin(n, end + context) : n;
        const QString text = whole.isNull() ? textOf(block, from, to) : whole;
        if (text.size() != to - from)
            return fail();
        const Cutter cutter(text, from, n);
        while (pos < end) {
            const int cut = cutter.next(pos, end);
            if (cut < 0)
                return fail();
            Piece piece;
            piece.start = pos;
            piece.text = text.mid(pos - from, cut - pos);
            piece.scan = scan(piece.text);
            pieces.push_back(std::move(piece));
            pos = cut;
        }
    }
    m_pieces = std::move(pieces);

    for (const Piece &p : m_pieces) {
        if (p.em >= 0) { m_em += p.em; m_chars += p.text.size(); }
        if (!p.measured) ++m_unmeasured;
    }
    const bool fresh = !before && !after;
    for (int k = 0; k < int(m_pieces.size()); ++k) {
        Piece &p = m_pieces[k];
        m_clock = qMax(m_clock, p.used);
        if (p.measured) {
            if (p.layout) ++m_live;
            if (p.start < formatsReach && formatsFor(p.start, int(p.text.size())) != p.formats) {
                if (p.layout) { p.layout.reset(); --m_live; }
                p.measured = false;
                ++m_unmeasured;
            }
        } else if (fresh ? k == 0 : p.reuse == Piece::Reuse::New) {
            layOut(k, true);
        } else if (fresh || p.reuse == Piece::Reuse::Other) {
            estimate(p);
        } // Same setup: keep an unmeasured piece's existing estimate.
    }
    if (!finish())
        return fail();
    trim(maxLive);
    return true;
}

bool LineLayout::finish()
{
    if (m_pieces.empty())
        return false;
    Strong strong = Strong::None;
    for (const Piece &p : m_pieces) {
        if (p.scan.blocks)
            return false;
        if (strong == Strong::None)
            strong = p.scan.strong;
    }
    // A right-to-left line is laid out whole.
    if (strong == Strong::Right)
        return false;
    place(0);
    return true;
}

std::pair<int, int> LineLayout::common(const QString &text) const
{
    if (!inPieces())
        return {0, 0};
    const QStringView view(text);
    int prefix = 0;
    for (const Piece &p : m_pieces) {
        const QStringView part = view.mid(prefix, p.text.size());
        const auto [a, b] = std::mismatch(part.begin(), part.end(), p.text.cbegin(), p.text.cend());
        prefix += int(a - part.begin());
        if (a != part.end() || part.size() < p.text.size())
            break;
    }
    const int room = qMin(int(text.size()), m_length) - prefix;
    int suffix = 0;
    for (auto it = m_pieces.rbegin(); it != m_pieces.rend() && suffix < room; ++it) {
        const int size = int(it->text.size());
        const int take = qMin(size, room - suffix);
        const QStringView mine = QStringView(it->text).right(take);
        const QStringView theirs = view.mid(text.size() - suffix - take, take);
        int same = 0;
        while (same < take && mine[take - 1 - same] == theirs[take - 1 - same])
            ++same;
        suffix += same;
        if (same < size)
            break;
    }
    return {prefix, suffix};
}

void LineLayout::formatsChanged(const QTextBlock &block)
{
    if (!inPieces())
        return;
    int reach = m_formatsEnd;
    m_formats = block.layout()->formats();
    m_formatsEnd = 0;
    for (const QTextLayout::FormatRange &f : std::as_const(m_formats))
        m_formatsEnd = qMax(m_formatsEnd, f.start + f.length);
    reach = qMax(reach, m_formatsEnd);
    for (Piece &p : m_pieces) {
        if (p.start >= reach) break;
        if (!p.measured || formatsFor(p.start, int(p.text.size())) == p.formats) continue;
        if (p.layout) { p.layout.reset(); --m_live; }
        p.measured = false; // Keep the old rows until shown.
        ++m_unmeasured;
    }
}

void LineLayout::setPreedit(int position, const QString &text, const QList<QTextLayout::FormatRange> &formats)
{
    if (!inPieces())
        return;
    if (text.isEmpty()) {
        if (m_preeditPiece < 0)
            return;
        const int k = m_preeditPiece;
        m_preeditPiece = -1;
        m_preedit.clear();
        m_preeditFormats.clear();
        measure(k, true);
        return;
    }
    // The piece the caret is in (at a join, the one after).
    int k = 0;
    for (int lo = 0, hi = int(m_pieces.size()) - 1; lo <= hi;) {
        const int mid = (lo + hi) / 2;
        if (m_pieces[mid].start <= position) {
            k = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    const int start = m_pieces[k].start;
    QList<QTextLayout::FormatRange> local;
    for (QTextLayout::FormatRange f : formats) {
        f.start -= start;
        local.append(f);
    }
    if (k == m_preeditPiece && position - start == m_preeditPosition && text == m_preedit && local == m_preeditFormats)
        return;
    const int old = m_preeditPiece;
    m_preeditPiece = k;
    m_preeditPosition = position - start;
    m_preedit = text;
    m_preeditFormats = local;
    if (old >= 0 && old != k)
        measure(old, true);
    measure(k, true);
    trim(maxLive);
}

// ---------------------------------------------------------------------------
// Pieces

int LineLayout::layoutStart(int piece) const
{
    return m_pieces[piece].start + (m_preeditPiece >= 0 && piece > m_preeditPiece ? int(m_preedit.size()) : 0);
}

int LineLayout::layoutLength(int piece) const
{
    return int(m_pieces[piece].text.size()) + (piece == m_preeditPiece ? int(m_preedit.size()) : 0);
}

int LineLayout::pieceAt(int position) const
{
    int lo = 0;
    int hi = int(m_pieces.size()) - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (layoutStart(mid) <= position)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

int LineLayout::pieceAtX(qreal x) const
{
    const auto it = std::upper_bound(m_pieces.begin(), m_pieces.end(), x,
                                     [](qreal value, const Piece &p) { return value < p.x; });
    return qMax(0, int(it - m_pieces.begin()) - 1);
}

int LineLayout::pieceAtRow(int row) const
{
    const auto it = std::upper_bound(m_pieces.begin(), m_pieces.end(), row,
                                     [](int value, const Piece &p) { return value < p.firstRow; });
    return qMax(0, int(it - m_pieces.begin()) - 1);
}

QTextLine LineLayout::rowLine(int row, int *piece)
{
    // The row was counted with the piece's measures as they were: laying an
    // estimated piece out may give it fewer rows. The row stays in its piece.
    const int k = pieceAtRow(row);
    QTextLayout *layout = use(k);
    if (piece)
        *piece = k;
    const int last = qMax(0, layout->lineCount() - 1);
    return layout->lineAt(qBound(0, row - m_pieces[k].firstRow, last));
}

QTextLayout *LineLayout::use(int piece)
{
    Piece &p = m_pieces[piece];
    if (!p.layout) {
        measure(piece, true);
        trim(maxLive);
    }
    p.used = ++m_clock;
    return p.layout.get();
}

bool LineLayout::layOut(int piece, bool keep)
{
    Piece &p = m_pieces[piece];
    auto layout = std::make_unique<QTextLayout>();
    layout->setText(p.text);
    layout->setFont(m_setup.font);
    QTextOption option = m_setup.option;
    // The line is left-to-right (see finish()), a piece alone might not be.
    option.setTextDirection(Qt::LeftToRight);
    layout->setTextOption(option);
    layout->setCacheEnabled(true);
    p.formats = formatsFor(p.start, int(p.text.size()));
    QList<QTextLayout::FormatRange> formats = p.formats;
    if (piece == m_preeditPiece) {
        layout->setPreeditArea(m_preeditPosition, m_preedit);
        formats += m_preeditFormats;
    }
    layout->setFormats(formats);
    layout->beginLayout();
    qreal y = m_setup.linePad;
    int rows = 0;
    for (QTextLine line = layout->createLine(); line.isValid(); line = layout->createLine()) {
        line.setLineWidth(m_setup.wrapWidth);
        line.setPosition({0, y});
        y += m_setup.lineHeight;
        ++rows;
    }
    layout->endLayout();

    const int oldRows = p.rows;
    const qreal oldNatural = p.natural;
    const qreal oldTabX = p.tabX;
    if (!p.measured) {
        --m_unmeasured;
        qreal advance = 0;
        for (int i = 0; i < rows; ++i) advance += layout->lineAt(i).naturalTextWidth();
        if (p.em >= 0) { m_em -= p.em; m_chars -= p.text.size(); }
        p.em = advance / m_ref;
        m_em += p.em;
        m_chars += p.text.size();
        m_measuredWidth += advance;
        m_measuredRows += qMax(1, rows);
        if (m_setup.wrap && m_measuredRows > 0 && m_setup.wrapWidth > 0)
            m_fill = qBound<qreal>(0.1, m_measuredWidth / (m_measuredRows * m_setup.wrapWidth), 1.0);
    }
    p.rows = qMax(1, rows);
    p.measured = true;
    if (!m_setup.wrap && rows > 0) {
        const QTextLine line = layout->lineAt(0);
        p.natural = line.naturalTextWidth();
        p.tabX = p.text.endsWith(u'\t') && piece != m_preeditPiece ? line.cursorToX(int(p.text.size()) - 1) : -1;
    }
    if (keep) {
        if (!p.layout)
            ++m_live;
        p.layout = std::move(layout);
        p.used = ++m_clock;
    } else if (p.layout) {
        p.layout.reset();
        --m_live;
    }
    return p.rows != oldRows || p.natural != oldNatural || p.tabX != oldTabX;
}

void LineLayout::estimate(Piece &p)
{
    const qreal width = (p.em >= 0 ? p.em : p.text.size() * (m_chars > 0 ? m_em / m_chars : 1)) * m_ref;
    p.tabX = -1;
    p.natural = width;
    p.rows = m_setup.wrap ? qMax(1, qCeil(width / qMax<qreal>(1, m_setup.wrapWidth * m_fill))) : 1;
}

bool LineLayout::resetup(const QTextBlock &block, const Setup &setup)
{
    if (!inPieces() || m_pieces.empty() || block.length() - 1 != m_length) return false;
#ifndef QT_NO_DEBUG
    const QString text = block.text();
    for (const Piece &p : m_pieces)
        Q_ASSERT(QStringView(text).mid(p.start, p.text.size()) == p.text);
#endif
    Setup geometry = setup;
    geometry.lineHeight = m_setup.lineHeight;
    geometry.linePad = m_setup.linePad;
    const bool metricsOnly = sameSetup(m_setup, geometry);
    const bool oldRows = m_setup.font == setup.font && m_setup.wrap == setup.wrap
        && m_setup.wrapWidth == setup.wrapWidth;
    m_setup = setup;
    m_ref = qMax<qreal>(0.01, QFontMetricsF(setup.font).averageCharWidth());
    m_live = 0;
    m_measuredWidth = 0;
    m_measuredRows = 0;
    for (Piece &p : m_pieces) {
        p.layout.reset();
        if (!metricsOnly) {
            p.measured = false;
            if (!oldRows) estimate(p);
        }
    }
    m_unmeasured = int(std::count_if(m_pieces.begin(), m_pieces.end(), [](const Piece &p) { return !p.measured; }));
    place(0);
    ++m_generation;
    formatsChanged(block);
    return true;
}

void LineLayout::measure(int k, bool keep)
{
    const int oldEnd = m_pieces[k].firstRow + m_pieces[k].rows;
    const int oldRows = m_pieces[k].rows;
    const bool unmeasured = !m_pieces[k].measured;
    const bool changed = layOut(k, keep);
    if (changed || unmeasured) {
        place(k);
        ++m_generation;
        const int delta = m_setup.wrap ? m_pieces[k].rows - oldRows : 0;
        if (m_listener) m_listener(this, oldEnd, delta);
    }
}

int LineLayout::windowAnchor(int top, int end) const
{
    if (m_pieces.empty()) return 0;
    const int first = pieceAtRow(qMax(0, top - 2));
    const int last = pieceAtRow(qMax(top, end + 1));
    for (int k = first; k <= last; ++k)
        if (m_pieces[k].measured) return m_pieces[k].start;
    return m_pieces[pieceAtRow(qMax(0, top))].start;
}

LineLayout::Settlement LineLayout::settleWindowStep(int top, int end, int anchor)
{
    if (!inPieces() || m_pieces.empty() || !m_setup.wrap) return {};
    const int first = pieceAtRow(qMax(0, top - 2));
    const int last = pieceAtRow(qMax(top, end + 1));
    const int anchored = anchor == -2 ? first : anchor == INT_MAX ? int(m_pieces.size())
        : pieceAt(anchor == -1 ? windowAnchor(top, end) : anchor);
    for (int k = first; k <= last; ++k) {
        if (m_pieces[k].measured) continue;
        const int oldEnd = m_pieces[k].firstRow + m_pieces[k].rows;
        const int oldRows = m_pieces[k].rows;
        measure(k, true);
        trim(maxLive);
        return {true, k < anchored && oldEnd > top ? m_pieces[k].rows - oldRows : 0};
    }
    return {};
}

void LineLayout::settleAround(int position)
{
    if (!inPieces() || m_pieces.empty()) return;
    const int k = pieceAt(position);
    if (!m_setup.wrap) {
        for (int i = 0; i <= k; ++i)
            if (!m_pieces[i].measured) measure(i, i == k);
    } else {
        for (int i = qMax(0, k - 1); i <= qMin(k + 1, int(m_pieces.size()) - 1); ++i)
            if (!m_pieces[i].measured) measure(i, true);
    }
    trim(maxLive);
}

void LineLayout::settleTo(qreal x)
{
    if (!inPieces() || m_setup.wrap) return;
    for (int k = 0; k < int(m_pieces.size()) && m_pieces[k].x <= x; ++k)
        if (!m_pieces[k].measured) {
            measure(k, true);
            trim(maxLive);
        }
}

bool LineLayout::settleOne(int nearRow)
{
    if (!inPieces() || m_pieces.empty() || !m_unmeasured) return false;
    const int near = m_setup.wrap ? pieceAtRow(nearRow) : pieceAtX(nearRow);
    for (int d = 0; d < int(m_pieces.size()); ++d) {
        for (int k : {near + d, near - d}) {
            if (k < 0 || k >= int(m_pieces.size()) || m_pieces[k].measured) continue;
            measure(k, false);
            return true;
        }
    }
    return false;
}

void LineLayout::trim(int keep)
{
    if (m_live <= keep)
        return;
    std::vector<std::pair<quint64, int>> live;
    for (int k = 0; k < int(m_pieces.size()); ++k) {
        if (m_pieces[k].layout)
            live.push_back({m_pieces[k].used, k});
    }
    std::sort(live.begin(), live.end());
    const size_t drop = live.size() - size_t(keep) * 3 / 4;
    for (size_t i = 0; i < drop; ++i)
        m_pieces[live[i].second].layout.reset();
    m_live = int(live.size() - drop);
}

void LineLayout::place(int from)
{
    from = qBound(0, from, int(m_pieces.size()));
    if (m_setup.wrap) {
        int row = from > 0 ? m_pieces[from - 1].firstRow + m_pieces[from - 1].rows : 0;
        for (int k = from; k < int(m_pieces.size()); ++k) {
            m_pieces[k].firstRow = row;
            row += m_pieces[k].rows;
        }
        return;
    }
    qreal x = from > 0 ? m_pieces[from - 1].x + m_pieces[from - 1].width : 0;
    for (int k = from; k < int(m_pieces.size()); ++k) {
        Piece &p = m_pieces[k];
        p.x = x;
        p.width = p.tabX >= 0 ? tabStop(x + p.tabX) - x : p.natural;
        x += p.width;
    }
}

qreal LineLayout::tabStop(qreal x) const
{
    // As QTextEngine::calculateTabWidth(), in its 1/64 px units.
    qint64 tab = qRound64(m_setup.option.tabStopDistance() * 64);
    if (tab <= 0)
        tab = 80 * 64;
    const qint64 at = qRound64(x * 64);
    return qreal((at / tab + 1) * tab) / 64;
}

QList<QTextLayout::FormatRange> LineLayout::formatsFor(int start, int length) const
{
    QList<QTextLayout::FormatRange> result;
    if (start >= m_formatsEnd)
        return result;
    for (const QTextLayout::FormatRange &f : m_formats) {
        const int s = qMax(f.start, start);
        const int e = qMin(f.start + f.length, start + length);
        if (s < e)
            result.append({s - start, e - s, f.format});
    }
    return result;
}

// ---------------------------------------------------------------------------
// Measures and painting

QList<int> LineLayout::pieceStarts() const
{
    QList<int> starts;
    for (const Piece &p : m_pieces)
        starts.append(p.start);
    return starts;
}

int LineLayout::lineCount() const
{
    if (m_whole)
        return m_whole->lineCount();
    if (m_pieces.empty())
        return 0;
    if (!m_setup.wrap)
        return 1;
    return m_pieces.back().firstRow + m_pieces.back().rows;
}

LineLayout::Line LineLayout::lineAt(int row)
{
    if (row < 0 || row >= lineCount())
        return {};
    return {this, row};
}

LineLayout::Line LineLayout::lineForTextPosition(int position)
{
    if (m_whole) {
        const QTextLine line = m_whole->lineForTextPosition(position);
        return line.isValid() ? Line(this, line.lineNumber()) : Line();
    }
    const int total = m_length + (m_preeditPiece >= 0 ? int(m_preedit.size()) : 0);
    if (m_pieces.empty() || position < 0 || position > total)
        return {};
    if (!m_setup.wrap)
        return {this, 0};
    settleAround(position);
    const int k = pieceAt(position);
    const QTextLine line = use(k)->lineForTextPosition(position - layoutStart(k));
    return line.isValid() ? Line(this, m_pieces[k].firstRow + line.lineNumber()) : Line();
}

qreal LineLayout::width() const
{
    if (m_whole)
        return m_whole->lineCount() > 0 ? m_whole->lineAt(0).naturalTextWidth() : 0;
    if (m_pieces.empty())
        return 0;
    return m_pieces.back().x + m_pieces.back().width;
}

std::pair<int, int> LineLayout::visibleRange(int firstRow, int endRow, qreal x1, qreal x2)
{
    if (m_whole || m_pieces.empty())
        return {0, INT_MAX};
    int first = 0;
    int last = 0;
    if (m_setup.wrap) {
        first = pieceAtRow(firstRow);
        last = pieceAtRow(qMax(firstRow, endRow - 1));
    } else {
        first = pieceAtX(x1);
        last = pieceAtX(x2);
    }
    return {layoutStart(first), layoutStart(last) + layoutLength(last)};
}

void LineLayout::draw(QPainter *painter, const QPointF &origin, const QList<QTextLayout::FormatRange> &selections,
                      const QRectF &clip)
{
    if (m_whole)
        return m_whole->draw(painter, origin, selections, clip);
    if (m_pieces.empty())
        return;
    int first = 0;
    int last = int(m_pieces.size()) - 1;
    if (clip.isValid()) {
        if (m_setup.wrap) {
            first = pieceAtRow(qFloor((clip.top() - origin.y()) / m_setup.lineHeight));
            last = pieceAtRow(qFloor((clip.bottom() - origin.y()) / m_setup.lineHeight));
        } else {
            first = pieceAtX(clip.left() - origin.x());
            last = pieceAtX(clip.right() - origin.x());
        }
    }
    for (int k = first; k <= last; ++k) {
        const int start = layoutStart(k);
        const int end = start + layoutLength(k);
        QList<QTextLayout::FormatRange> local;
        for (const QTextLayout::FormatRange &range : selections) {
            const int s = qMax(range.start, start);
            const int e = qMin(range.start + range.length, end);
            if (s < e)
                local.append({s - start, e - s, range.format});
        }
        QTextLayout *layout = use(k);
        const Piece &p = m_pieces[k];
        const QPointF at = origin + (m_setup.wrap ? QPointF(0, p.firstRow * m_setup.lineHeight) : QPointF(p.x, 0));
        layout->draw(painter, at, local, clip);
    }
}

void LineLayout::drawCursor(QPainter *painter, const QPointF &origin, int position, int width)
{
    if (m_whole)
        return m_whole->drawCursor(painter, origin, position, width);
    if (m_pieces.empty())
        return;
    const int k = pieceAt(position);
    const int local = qMax(0, position - layoutStart(k));
    QTextLayout *layout = use(k);
    const Piece &p = m_pieces[k];
    if (m_setup.wrap) {
        layout->drawCursor(painter, origin + QPointF(0, p.firstRow * m_setup.lineHeight), local, width);
        return;
    }
    if (p.tabX >= 0 && local >= layoutLength(k)) {
        // After a last tab as wide as the line has it.
        const QTextLine line = layout->lineAt(0);
        painter->fillRect(QRectF(origin.x() + p.x + p.width, origin.y() + line.y(), width, line.height()),
                          painter->pen().brush());
        return;
    }
    layout->drawCursor(painter, origin + QPointF(p.x, 0), local, width);
}

} // namespace vn
