#include "ReadFlow.h"

#include <QtGlobal>

#include <algorithm>

namespace vn::md {

namespace {

// A page back that cannot be done from the history looks for its start one
// line at a time; this many lines is more than any screen holds.
constexpr int maxSearchLines = 600;
constexpr size_t maxHistory = 256;

QList<int> twoEachSide(const QList<int> &lineTops)
{
    QList<int> cuts;
    const int n = int(lineTops.size());
    for (int k = 2; n - k >= 2; ++k)
        cuts.push_back(lineTops[k]);
    return cuts;
}

} // namespace

QList<int> paragraphCuts(const QList<int> &lineTops)
{
    return twoEachSide(lineTops);
}

QList<int> codeCuts(const QList<int> &lineTops)
{
    return twoEachSide(lineTops);
}

QList<int> tableCuts(const QList<int> &rowTops, int headerRows)
{
    QList<int> cuts;
    headerRows = qMax(0, headerRows);
    const int body = int(rowTops.size()) - headerRows;
    for (int j = 2; body - j >= 2; ++j)
        cuts.push_back(rowTops[headerRows + j]);
    return cuts;
}

int RowSource::sourceLineAt(int row, int y)
{
    const QList<int> t = lineTops(row);
    const int first = firstLine(row);
    const int last = lastLine(row);
    if (t.size() <= 1 || last <= first)
        return first;
    const int k = qBound(0, int(std::upper_bound(t.begin(), t.end(), y) - t.begin()) - 1, int(t.size()) - 1);
    return first + int(qint64(k) * (last - first + 1) / t.size());
}

int RowSource::yOfLine(int row, int line)
{
    const QList<int> t = lineTops(row);
    const int first = firstLine(row);
    const int last = lastLine(row);
    if (t.size() <= 1 || last <= first)
        return 0;
    const int k = qBound(0, int(qint64(line - first) * t.size() / (last - first + 1)), int(t.size()) - 1);
    return k == 0 ? 0 : t[k];
}

ReadFlow::ReadFlow(RowSource *source)
    : m_source(source)
{
}

void ReadFlow::setGeometry(const Geometry &geometry)
{
    m_geometry = geometry;
    m_geometry.columns = qMax(1, m_geometry.columns);
    reflow();
}

void ReadFlow::setKeptLines(int lines)
{
    m_keptLines = qBound(0, lines, maxKeptLines);
}

Position ReadFlow::endOfDocument() const
{
    return {m_source->count(), 0};
}

// The first place at or after position with something to show: past blank
// gap rows, and not past the end.
Position ReadFlow::skipGaps(Position position) const
{
    const int n = m_source->count();
    position.row = qMax(0, position.row);
    position.y = qMax(0, position.y);
    while (position.row < n && m_source->kind(position.row) == RowKind::Gap)
        position = {position.row + 1, 0};
    return position.row >= n ? endOfDocument() : position;
}

// A row's line tops: sorted, inside the row, and the row's own top first (a
// row can always start at its top, even if text starts lower).
QList<int> ReadFlow::tops(int row) const
{
    const int h = m_source->height(row);
    QList<int> t = m_source->lineTops(row);
    std::sort(t.begin(), t.end());
    t.erase(std::remove_if(t.begin(), t.end(), [h](int y) { return y <= 0 || y >= h; }), t.end());
    t.erase(std::unique(t.begin(), t.end()), t.end());
    t.prepend(0);
    return t;
}

// The next line top after position, over rows (gap rows have no lines).
std::optional<Position> ReadFlow::nextLine(Position position) const
{
    const int n = m_source->count();
    if (position.row >= n)
        return std::nullopt;
    const QList<int> t = tops(position.row);
    if (const auto it = std::upper_bound(t.begin(), t.end(), position.y); it != t.end())
        return Position{position.row, *it};
    for (int row = position.row + 1; row < n; ++row) {
        if (m_source->kind(row) != RowKind::Gap)
            return Position{row, 0};
    }
    return std::nullopt;
}

std::optional<Position> ReadFlow::prevLine(Position position) const
{
    const int n = m_source->count();
    int row = qMin(position.row, n);
    if (row < n) {
        const QList<int> t = tops(row);
        if (const auto it = std::lower_bound(t.begin(), t.end(), position.y); it != t.begin())
            return Position{row, *(it - 1)};
    }
    for (--row; row >= 0; --row) {
        if (m_source->kind(row) != RowKind::Gap)
            return Position{row, tops(row).back()};
    }
    return std::nullopt;
}

// lines line tops on, or the end of the document.
Position ReadFlow::forward(Position position, int lines) const
{
    for (int i = 0; i < lines; ++i) {
        const auto next = nextLine(position);
        if (!next)
            return endOfDocument();
        position = *next;
    }
    return position;
}

// lines line tops back, or the first line.
Position ReadFlow::back(Position position, int lines) const
{
    for (int i = 0; i < lines; ++i) {
        const auto previous = prevLine(position);
        if (!previous)
            break;
        position = *previous;
    }
    return position;
}

int ReadFlow::drawnHeight(const Segment &segment) const
{
    return segment.yTo - segment.yFrom + (segment.repeatHeader ? m_source->headerHeight(segment.row) : 0);
}

namespace {

// The header a table's part from y takes: the header row, but not for the
// part that has it already (y == 0).
int repeatedHeader(RowSource *source, int row, RowKind kind, int y)
{
    return kind == RowKind::Table && y > 0 ? qMax(0, source->headerHeight(row)) : 0;
}

// The legal cuts of a row, kept to inside it.
QList<int> legalCuts(RowSource *source, int row, RowKind kind, int h)
{
    if (kind == RowKind::Unbreakable || kind == RowKind::Gap)
        return {};
    QList<int> cuts = source->cuts(row);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::remove_if(cuts.begin(), cuts.end(), [h](int c) { return c <= 0 || c >= h; }), cuts.end());
    return cuts;
}

// A piece of a row between two cuts has at least 2 lines (table rows) in it:
// the cuts are 2 lines from the row's ends, but a row cut again, in a later
// column, must not leave a line alone either. a is where the piece starts and
// b where it ends; a piece that goes to the row's end is left to the cuts.
bool piece2Lines(const QList<int> &tops, int a, int b, int h)
{
    if (b >= h)
        return true;
    const auto lo = std::lower_bound(tops.begin(), tops.end(), a);
    const auto hi = std::lower_bound(tops.begin(), tops.end(), b);
    return hi - lo >= 2;
}

} // namespace

Screen ReadFlow::fill(Position start) const
{
    Screen screen;
    screen.columns.resize(m_geometry.columns);
    Position position = skipGaps(start);
    screen.start = position;
    for (Column &column : screen.columns) {
        if (position.row >= m_source->count())
            break;
        position = skipGaps(fillColumn(position, column));
    }
    screen.end = position;
    screen.atEnd = position.row >= m_source->count();
    screen.atStart = screen.start.row >= m_source->count() || !prevLine(screen.start);
    return screen;
}

// One column from position: whole rows while they fit, else the deepest cut
// that does, else the column ends early; a row taller than an empty column is
// cut where the column ends. Returns where the next column starts.
Position ReadFlow::fillColumn(Position position, Column &column) const
{
    const int n = m_source->count();
    const int limit = qMax(1, m_geometry.height);
    int row = position.row;
    int y = position.y;
    while (row < n) {
        const RowKind kind = m_source->kind(row);
        if (kind == RowKind::Gap && column.segments.empty()) {
            ++row;
            y = 0;
            continue;
        }
        const int h = m_source->height(row);
        if (y >= h) { // Nothing of it left (or nothing to it).
            ++row;
            y = 0;
            continue;
        }
        const int header = repeatedHeader(m_source, row, kind, y);
        const int room = limit - column.used;
        if (h - y + header <= room) {
            column.segments.push_back({row, y, h, header > 0});
            column.used += h - y + header;
            ++row;
            y = 0;
            continue;
        }
        const QList<int> cuts = legalCuts(m_source, row, kind, h);
        int cut = 0;
        if (!cuts.isEmpty()) {
            const QList<int> t = tops(row);
            for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) {
                if (*it > y && *it - y + header <= room && piece2Lines(t, y, *it, h)) {
                    cut = *it;
                    break;
                }
            }
        }
        if (cut > y) {
            column.segments.push_back({row, y, cut, header > 0});
            column.used += cut - y + header;
            y = cut;
            break;
        }
        if (column.segments.empty()) {
            // Taller than an empty column, whatever its kind: cut at the
            // deepest line top that fits, or where the column ends.
            const int reach = qMin(h, y + qMax(1, limit - header));
            int at = reach;
            if (reach < h) {
                const QList<int> t = tops(row);
                const auto it = std::upper_bound(t.begin(), t.end(), reach);
                if (it != t.begin() && *(it - 1) > y)
                    at = *(it - 1);
            }
            column.segments.push_back({row, y, at, header > 0});
            column.used += at - y + header;
            if (at >= h) {
                ++row;
                y = 0;
            } else {
                y = at;
            }
        }
        break; // Too tall for what is left: it starts the next column.
    }
    if (row < n)
        trimColumn(column, row, y);
    return {row, y};
}

// A heading is not left last in a column (the part it heads goes on in the
// next one), unless it is all the column has. Gap rows at the bottom are
// blank space: they go too. row, y is where the next column starts.
void ReadFlow::trimColumn(Column &column, int &row, int &y) const
{
    auto &segments = column.segments;
    for (;;) {
        int last = int(segments.size()) - 1;
        while (last >= 0 && m_source->kind(segments[last].row) == RowKind::Gap)
            --last;
        if (last < 0)
            break;
        const bool contentBefore = std::any_of(segments.begin(), segments.begin() + last, [this](const Segment &s) {
            return m_source->kind(s.row) != RowKind::Gap;
        });
        if (m_source->kind(segments[last].row) != RowKind::Heading || !contentBefore) {
            segments.resize(last + 1);
            break;
        }
        row = segments[last].row;
        y = segments[last].yFrom;
        segments.resize(last);
    }
    column.used = 0;
    for (const Segment &s : segments)
        column.used += drawnHeight(s);
}

LineRange ReadFlow::lineRange(const Screen &screen) const
{
    const Segment *first = nullptr;
    const Segment *last = nullptr;
    for (const Column &column : screen.columns) {
        for (const Segment &s : column.segments) {
            if (m_source->kind(s.row) == RowKind::Gap)
                continue;
            if (!first)
                first = &s;
            last = &s;
        }
    }
    if (!first) {
        const int line = lineOf(screen.start);
        return {line, line};
    }
    LineRange range;
    range.first = m_source->sourceLineAt(first->row, first->yFrom);
    if (last->yTo >= m_source->height(last->row)) {
        range.last = m_source->lastLine(last->row);
    } else {
        // Cut inside the row: the line of the last line top above the cut.
        const auto prior = prevLine({last->row, last->yTo});
        const int y = prior && prior->row == last->row ? qMax(prior->y, last->yFrom) : last->yFrom;
        range.last = m_source->sourceLineAt(last->row, y);
    }
    range.last = qMax(range.first, range.last);
    return range;
}

int ReadFlow::lineOf(Position position) const
{
    const int n = m_source->count();
    if (n == 0)
        return 0;
    if (position.row >= n)
        return m_source->lastLine(n - 1);
    return m_source->sourceLineAt(position.row, position.y);
}

Position ReadFlow::positionOfLine(int line) const
{
    const int n = m_source->count();
    if (n == 0)
        return {0, 0};
    const int row = qBound(0, m_source->rowForLine(line), n - 1);
    const int y = qBound(0, m_source->yOfLine(row, line), qMax(0, m_source->height(row) - 1));
    return {row, y};
}

// The kept lines are what the screen shows before until: segments before it
// whole, the one it falls in up to it.
void ReadFlow::markKept(Screen &screen, Position until) const
{
    for (Column &column : screen.columns) {
        for (Segment &s : column.segments) {
            if (s.row < until.row)
                s.keptTo = s.yTo;
            else if (s.row == until.row)
                s.keptTo = qBound(s.yFrom, until.y, s.yTo);
            else
                s.keptTo = 0;
            screen.hasKept = screen.hasKept || s.keptTo > s.yFrom;
        }
    }
}

// The start of the screen whose last column ends at end: the columns filled
// from the right, each from the bottom up, with the same ways of breaking as
// fillColumn (a row cut at the shallowest cut that fits, the headers of a
// table's parts counted). Not the heading rule, and gap rows at the top of a
// column are not skipped: a forward fill from here may end sooner, which
// startingBefore corrects.
Position ReadFlow::backwardStart(Position end) const
{
    const int n = m_source->count();
    if (n == 0)
        return {0, 0};
    const int limit = qMax(1, m_geometry.height);
    int row = end.row;
    int yEnd = end.y;
    if (row >= n) {
        row = n - 1;
        yEnd = m_source->height(row);
    }
    Position start = {row, yEnd};
    for (int c = 0; c < m_geometry.columns; ++c) {
        int used = 0;
        bool any = false;
        while (row >= 0) {
            if (yEnd <= 0) {
                if (--row < 0)
                    break;
                yEnd = m_source->height(row);
                continue;
            }
            const RowKind kind = m_source->kind(row);
            if (kind == RowKind::Gap && !any) { // Blank space at a column's bottom.
                yEnd = 0;
                continue;
            }
            const int room = limit - used;
            if (yEnd <= room) {
                used += yEnd;
                any = true;
                start = {row, 0};
                yEnd = 0;
                continue;
            }
            const int h = m_source->height(row);
            const int header = kind == RowKind::Table ? qMax(0, m_source->headerHeight(row)) : 0;
            const QList<int> cuts = legalCuts(m_source, row, kind, h);
            const QList<int> t = cuts.isEmpty() ? QList<int>() : tops(row);
            int cut = -1;
            for (const int c2 : cuts) {
                if (c2 < yEnd && yEnd - c2 + header <= room && piece2Lines(t, c2, yEnd, h)) {
                    cut = c2;
                    break;
                }
            }
            if (cut > 0) {
                start = {row, cut};
                yEnd = cut;
            } else if (!any) {
                // Taller than an empty column: its last stretch, from the
                // shallowest line top that fits.
                const int reach = qMax(0, yEnd - qMax(1, limit - header));
                int at = reach;
                const QList<int> lineTops = tops(row);
                const auto it = std::lower_bound(lineTops.begin(), lineTops.end(), reach);
                if (it != lineTops.end() && *it < yEnd)
                    at = *it;
                start = {row, at};
                yEnd = at;
            } else {
                start = {row, yEnd}; // It starts this column's right neighbour.
            }
            break;
        }
        if (row < 0)
            return {0, 0};
    }
    if (start.row < n && start.y >= m_source->height(start.row))
        start = {start.row + 1, 0};
    return start;
}

// The screen before one that starts at limit (or the last screen, with no
// limit) is the one that ends at end: the backward fill gives a start that is
// too early or just right (the rules only ever take room), so it moves on a line
// (or a cut) at a time until a forward fill from it reaches end. That costs a screen of
// rows a try and the tries are few.
Position ReadFlow::startingBefore(Position end, std::optional<Position> limit) const
{
    end = skipGaps(end);
    Position candidate = skipGaps(backwardStart(end)); // Blank rows at its top do not show.
    if (limit && candidate >= *limit)
        candidate = back(*limit, 1);
    for (int i = 0; i < maxSearchLines; ++i) {
        const Screen screen = fill(candidate);
        if (screen.end >= end)
            break;
        // The next line top, or sooner the end of the first part of a row cut
        // in the middle of a line (a figure taller than the column), which is
        // where a page can start as well.
        auto next = nextLine(candidate);
        if (!screen.empty()) {
            const Segment &first = screen.columns[0].segments[0];
            const Position cutEnd{first.row, first.yTo};
            if (first.yTo < m_source->height(first.row) && cutEnd > candidate && (!next || cutEnd < *next))
                next = cutEnd;
        }
        if (!next || (limit && *next >= *limit))
            break;
        candidate = *next;
    }
    return candidate;
}

const Screen &ReadFlow::show(Screen screen)
{
    m_keptUntil.reset();
    m_screen = std::move(screen);
    m_screenKey = m_geometry.key();
    m_hasScreen = true;
    m_startLine = lineOf(m_screen.start);
    return m_screen;
}

void ReadFlow::ensureScreen()
{
    if (!m_hasScreen)
        reflow();
}

const Screen &ReadFlow::reflow()
{
    const bool same = m_hasScreen && m_screenKey == m_geometry.key();
    if (!same)
        m_history.clear();
    // The same rows: the same place. Others: the place of the same source line.
    const Position from = same ? m_screen.start : positionOfLine(m_startLine);
    m_screen = fill(from);
    m_screenKey = m_geometry.key();
    m_hasScreen = true;
    if (same && m_keptUntil)
        markKept(m_screen, *m_keptUntil);
    else
        m_keptUntil.reset();
    if (same)
        m_startLine = lineOf(m_screen.start);
    return m_screen;
}

const Screen &ReadFlow::goTo(Position position)
{
    m_history.clear();
    return show(fill(position));
}

const Screen &ReadFlow::goToLine(int line)
{
    return goTo(positionOfLine(line));
}

const Screen &ReadFlow::toStart()
{
    return goTo({0, 0});
}

const Screen &ReadFlow::toEnd()
{
    m_history.clear();
    if (m_source->count() == 0)
        return show(fill({0, 0}));
    return show(fill(startingBefore(endOfDocument(), std::nullopt)));
}

const Screen &ReadFlow::pageForward()
{
    ensureScreen();
    if (m_screen.atEnd)
        return m_screen;
    // The next page starts N lines before this one ended (not before this one
    // started: a page always moves on). With fewer lines kept if the page
    // would end where this one did: a heading and what it heads can push the
    // kept lines and the heading back to the same place.
    const Position oldEnd = m_screen.end;
    Position start = oldEnd;
    Screen next;
    for (int kept = m_keptLines; kept >= 0; --kept) {
        start = back(oldEnd, kept);
        if (kept > 0 && start <= m_screen.start)
            continue;
        next = fill(start);
        if (next.end > oldEnd)
            break;
    }
    Step step{m_geometry.key(), m_screen.start, start};
    const bool kept = start < oldEnd;
    if (kept)
        markKept(next, oldEnd);
    show(std::move(next));
    if (kept)
        m_keptUntil = oldEnd;
    m_history.push_back(step);
    if (m_history.size() > maxHistory)
        m_history.erase(m_history.begin());
    return m_screen;
}

const Screen &ReadFlow::pageBack()
{
    ensureScreen();
    if (m_screen.atStart)
        return m_screen;
    // Where the turn forward came from, if it is still this layout and this
    // screen; otherwise work it out (and what is remembered is stale).
    if (!m_history.empty() && m_history.back().key == m_geometry.key() && m_history.back().to == m_screen.start) {
        const Position from = m_history.back().from;
        m_history.pop_back();
        return show(fill(from));
    }
    m_history.clear();
    // A forward page turn from the screen before this one starts N lines
    // before it ends, so it ends N lines past this one's start.
    const Position end = forward(m_screen.start, m_keptLines);
    return show(fill(startingBefore(end, m_screen.start)));
}

const Screen &ReadFlow::nudge(int lines)
{
    ensureScreen();
    Position start = m_screen.start;
    for (; lines > 0 && !m_screen.atEnd; --lines) {
        // Not past the last line: the screen would empty out.
        const auto next = nextLine(start);
        if (!next)
            break;
        start = *next;
        if (fill(start).atEnd)
            break;
    }
    for (; lines < 0; ++lines) {
        const auto previous = prevLine(start);
        if (!previous)
            break;
        start = *previous;
    }
    if (start == m_screen.start)
        return m_screen;
    return goTo(start);
}

} // namespace vn::md
