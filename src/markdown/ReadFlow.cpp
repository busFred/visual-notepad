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
    for (int j = 1; body - j >= 1; ++j)
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
        position = {position.row + 1, 0, position.floatRow};
    if (position.row >= n)
        return {n, 0, position.floatRow};
    return position;
}

ReadFlow::RowCache &ReadFlow::cachedRow(int row) const
{
    auto it = std::find_if(m_rowCache.begin(), m_rowCache.end(), [row](const RowCache &entry) { return entry.row == row; });
    if (it == m_rowCache.end()) {
        m_rowCache.push_front(RowCache{.row = row});
        if (m_rowCache.size() > 8)
            m_rowCache.pop_back();
    } else {
        m_rowCache.splice(m_rowCache.begin(), m_rowCache, it);
    }
    return m_rowCache.front();
}

// A row's line tops: sorted, inside the row, and the row's own top first (a
// row can always start at its top, even if text starts lower).
QList<int> ReadFlow::tops(int row) const
{
    RowCache &entry = cachedRow(row);
    if (entry.hasTops)
        return entry.tops;
    const int h = m_source->height(row);
    QList<int> t = m_source->lineTops(row);
    std::sort(t.begin(), t.end());
    t.erase(std::remove_if(t.begin(), t.end(), [h](int y) { return y <= 0 || y >= h; }), t.end());
    t.erase(std::unique(t.begin(), t.end()), t.end());
    t.prepend(0);
    entry.tops = t;
    entry.hasTops = true;
    return t;
}

// The legal cuts of a row, kept to inside it.
QList<int> ReadFlow::legalCuts(int row, RowKind kind, int h) const
{
    if (kind == RowKind::Unbreakable || kind == RowKind::Picture || kind == RowKind::Gap)
        return {};
    RowCache &entry = cachedRow(row);
    if (entry.hasCuts)
        return entry.cuts;
    QList<int> cuts = m_source->cuts(row);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::remove_if(cuts.begin(), cuts.end(), [h](int c) { return c <= 0 || c >= h; }), cuts.end());
    entry.cuts = cuts;
    entry.hasCuts = true;
    return cuts;
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

// Whether the line at position (a line top) may be kept for the next page: a
// line of text, as high as a line (not one of a figure, equation, chip or
// short table, which are one "line" tall as themselves, nor a line more than
// a quarter of the column high).
bool ReadFlow::keepable(Position position) const
{
    const RowKind kind = m_source->kind(position.row);
    if (kind == RowKind::Unbreakable || kind == RowKind::Picture || kind == RowKind::Gap)
        return false;
    const QList<int> t = tops(position.row);
    // Not a table's header: the part after it repeats it anyway.
    if (kind == RowKind::Table && t.size() > 1 && position.y < t[1])
        return false;
    const auto next = std::upper_bound(t.begin(), t.end(), position.y);
    const int bottom = next != t.end() ? *next : m_source->height(position.row);
    return (bottom - position.y) * 4 <= qMax(1, m_geometry.height);
}

// Where the lines kept from a page that ends at end start: at most `lines`
// lines back, and not back over a line that is not keepable (so none, if the
// last one is not). `count` is how many lines that is.
Position ReadFlow::keptStart(Position end, int lines, int *count) const
{
    Position start = end;
    int kept = 0;
    for (; kept < lines; ++kept) {
        const auto previous = prevLine(start);
        if (!previous || !keepable(*previous))
            break;
        start = *previous;
    }
    if (count)
        *count = kept;
    return start;
}

int ReadFlow::drawnHeight(const Segment &segment) const
{
    switch (segment.part) {
    case Segment::Part::Row:
        break;
    case Segment::Part::Chip:
        return m_source->chipHeight();
    case Segment::Part::FromNote:
    case Segment::Part::OnNote:
        return m_source->noteHeight();
    }
    return segment.yTo - segment.yFrom + (segment.repeatHeader ? m_source->headerHeight(segment.row) : 0);
}

namespace {

// The header a table's part from y takes: the header row, but not for the
// part that has it already (y == 0).
int repeatedHeader(RowSource *source, int row, RowKind kind, int y)
{
    return kind == RowKind::Table && y > 0 ? qMax(0, source->headerHeight(row)) : 0;
}

// A piece of a row between two cuts has at least 2 lines (table rows) in it:
// the cuts are 2 lines from the row's ends, but a row cut again, in a later
// column, must not leave a line alone either. a is where the piece starts and
// b where it ends; a piece that goes to the row's end is left to the cuts.
// (The source can ask for fewer: RowSource::minPieceLines.)
bool piece2Lines(const QList<int> &tops, int a, int b, int h, int least = 2)
{
    if (b >= h)
        return true;
    const auto lo = std::lower_bound(tops.begin(), tops.end(), a);
    const auto hi = std::lower_bound(tops.begin(), tops.end(), b);
    return hi - lo >= least;
}

} // namespace

Screen ReadFlow::fill(Position start) const
{
    Screen screen;
    screen.columns.resize(m_geometry.columns);
    const int n = m_source->count();
    Position position = skipGaps(start);
    screen.start = position;
    // Pictures moved on, waiting for the top of a column.
    int pending = position.floatRow >= 0 && position.floatRow < n ? position.floatRow : -1;
    position.floatRow = -1;
    for (int c = 0; c < int(screen.columns.size()); ++c) {
        Column &column = screen.columns[c];
        const bool last = c + 1 == int(screen.columns.size());
        if (pending >= 0) {
            pending = placePictures(pending, column, c == 0, last);
            if (pending >= 0)
                continue; // (The column is theirs.)
        }
        if (position.row >= n)
            break;
        position = skipGaps(fillColumn(position, column, pending));
    }
    screen.end = position;
    screen.end.floatRow = pending;
    screen.atEnd = position.row >= n && pending < 0;
    screen.atStart = screen.start.floatRow < 0 && (screen.start.row >= n || !prevLine(screen.start));
    return screen;
}

std::pair<int, int> ReadFlow::pictureGroup(int row) const
{
    const int n = m_source->count();
    const auto isPicture = [this](int r) { return m_source->kind(r) == RowKind::Picture; };
    int first = row;
    for (int r = row - 1; r >= 0; --r) {
        if (isPicture(r))
            first = r;
        else if (m_source->kind(r) != RowKind::Gap)
            break;
    }
    int last = row;
    for (int r = row + 1; r < n; ++r) {
        if (isPicture(r))
            last = r;
        else if (m_source->kind(r) != RowKind::Gap)
            break;
    }
    return {first, last};
}

// Pictures move only if each of them, with a note over and under it, fits
// an empty column: one taller is cut where columns end, in its place.
bool ReadFlow::canMove(int row) const
{
    const int limit = qMax(1, m_geometry.height);
    const int notes = 2 * m_source->noteHeight();
    const int last = pictureGroup(row).second;
    for (int r = row; r <= last; ++r) {
        if (m_source->kind(r) == RowKind::Picture && m_source->height(r) + notes > limit)
            return false;
    }
    return true;
}

// Moved pictures at the top of a column, from row on in its group, under a
// note (from the page before if page, else from the column before). Those
// that do not fit wait for the next column, with a note saying so: the row
// returned, or -1 if all are placed.
int ReadFlow::placePictures(int row, Column &column, bool page, bool lastColumn) const
{
    const int limit = qMax(1, m_geometry.height);
    const int note = m_source->noteHeight();
    const int last = pictureGroup(row).second;
    column.segments.push_back({row, 0, 0, false, 0, Segment::Part::FromNote, page});
    column.used += note;
    int r = row;
    bool any = false;
    while (r <= last) {
        // The gap rows before the next picture go with it.
        int next = r;
        int gaps = 0;
        while (next <= last && m_source->kind(next) != RowKind::Picture)
            gaps += m_source->height(next++);
        if (next > last) {
            r = next;
            break;
        }
        if (!any)
            gaps = 0; // (Not at the top of the column.)
        const int h = m_source->height(next);
        const bool more = next < last;
        const int need = gaps + h + (more ? note : 0);
        if (any && column.used + need > limit)
            break;
        if (any) {
            for (int g = r; g < next; ++g) {
                column.segments.push_back({g, 0, m_source->height(g)});
                column.used += m_source->height(g);
            }
        }
        column.segments.push_back({next, 0, h});
        column.used += h;
        any = true;
        r = next + 1;
    }
    while (r <= last && m_source->kind(r) != RowKind::Picture)
        ++r;
    if (r > last) {
        // All placed: the blank after the group, if it fits, before the text.
        const int n = m_source->count();
        for (int g = last + 1; g < n && m_source->kind(g) == RowKind::Gap; ++g) {
            if (column.used + m_source->height(g) > limit)
                break;
            column.segments.push_back({g, 0, m_source->height(g)});
            column.used += m_source->height(g);
        }
        return -1;
    }
    column.segments.push_back({r, 0, 0, false, 0, Segment::Part::OnNote, lastColumn});
    column.used += note;
    return r;
}

// One column from position: whole rows while they fit, else the deepest cut
// that does, else the column ends early; a row taller than an empty column is
// cut where the column ends. Returns where the next column starts.
Position ReadFlow::fillColumn(Position position, Column &column, int &pending) const
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
        // A picture after pictures moved on waits for them: the text stops.
        if (kind == RowKind::Picture && pending >= 0)
            break;
        const int header = repeatedHeader(m_source, row, kind, y);
        const int room = limit - column.used;
        if (kind == RowKind::Picture && y == 0 && h > room && !column.segments.empty()
            && m_source->chipHeight() > 0 && m_source->chipHeight() <= room && canMove(row)) {
            // It moves to the top of the next column; a chip here, and what
            // follows it fills the room.
            const int last = pictureGroup(row).second;
            column.segments.push_back({row, 0, 0, false, 0, Segment::Part::Chip});
            column.used += m_source->chipHeight();
            pending = row;
            row = last + 1;
            y = 0;
            continue;
        }
        if (h - y + header <= room) {
            column.segments.push_back({row, y, h, header > 0});
            column.used += h - y + header;
            ++row;
            y = 0;
            continue;
        }
        const QList<int> cuts = legalCuts(row, kind, h);
        int cut = 0;
        if (!cuts.isEmpty()) {
            const QList<int> t = tops(row);
            const int least = m_source->minPieceLines(row);
            for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) {
                if (*it > y && *it - y + header <= room && piece2Lines(t, y, *it, h, least)) {
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
                // (Not after a table's header alone: something of its body too.)
                const int least = kind == RowKind::Table && y == 0 && t.size() > 1 ? t[1] : y;
                if (it != t.begin() && *(it - 1) > least)
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
            if (m_source->kind(s.row) == RowKind::Gap || s.part != Segment::Part::Row)
                continue;
            // (In document order: moved pictures come after text that follows them.)
            if (!first || std::pair(s.row, s.yFrom) < std::pair(first->row, first->yFrom))
                first = &s;
            if (!last || std::pair(s.row, s.yTo) > std::pair(last->row, last->yTo))
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
    if (position.floatRow >= 0 && position.floatRow < n)
        return m_source->firstLine(position.floatRow);
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
            const QList<int> cuts = legalCuts(row, kind, h);
            const QList<int> t = cuts.isEmpty() ? QList<int>() : tops(row);
            const int least = m_source->minPieceLines(row);
            int cut = -1;
            for (const int c2 : cuts) {
                if (c2 < yEnd && yEnd - c2 + header <= room && piece2Lines(t, c2, yEnd, h, least)) {
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
    m_rowCache.clear();
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
    m_rowCache.clear(); // (The rows may have changed since: a new start.)
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
    // Only lines of text are kept (see keepable): after a figure, none of it.
    const Position oldEnd = m_screen.end;
    Position start = oldEnd;
    Screen next;
    int available = 0;
    // (Not when it starts with pictures moved on: they come first.)
    if (oldEnd.floatRow < 0)
        keptStart(oldEnd, m_keptLines, &available);
    for (int kept = available; kept >= 0; --kept) {
        start = keptStart(oldEnd, kept);
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
    // A forward page turn from the screen before this one starts the lines
    // it keeps before it ends, so it ends that many lines past this one's
    // start: the most (up to N) that are keepable lines and that it would
    // keep from just there.
    Position end = m_screen.start;
    for (int kept = m_keptLines; kept > 0; --kept) {
        const Position candidate = forward(m_screen.start, kept);
        int count = 0;
        const Position start = keptStart(candidate, m_keptLines, &count);
        if (count == kept && start == m_screen.start) {
            end = candidate;
            break;
        }
    }
    return show(fill(startingBefore(end, m_screen.start)));
}

const Screen &ReadFlow::nudge(int lines)
{
    ensureScreen();
    Position start = m_screen.start;
    if (start.floatRow >= 0) // (From the pictures, in their place.)
        start = {start.floatRow, 0};
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
