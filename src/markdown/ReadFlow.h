// The flow of read mode: which part of the document each column of a screen
// shows, and how a page turn, a nudge of one line or a change of layout moves
// from one screen to the next. No widgets here: the rows come from a
// RowSource (the visual view implements it) and everything is in pixels from
// the top of a row, so the rules can be tested with made-up heights.
//
// A document is a list of rows (a block, one item of a long list, a part of
// a long code block, a blank-line gap). Heights are costly (a row is rendered
// when its height is first asked for), so every operation here reads about
// one screen of rows around its position and never walks the document; the
// one place that could, the jump to the end, works backwards from the last
// row (see ReadFlow::toEnd).
#pragma once

#include <QList>

#include <compare>
#include <optional>
#include <vector>

namespace vn::md {

enum class RowKind {
    Normal,
    Heading,     // Never ends a column on its own.
    Gap,         // Blank lines between blocks; skipped at the top of a column.
    Table,       // Breaks between body rows only; the continued part repeats the header.
    Unbreakable, // Figures, equations, short tables: only cut when taller than a column.
};

// Where a screen starts, or ends: a row and a place in it, in pixels from the
// row's top, normally the top of a visual line. {count, 0} is the end of the
// document.
struct Position
{
    int row = 0;
    int y = 0;
    friend auto operator<=>(const Position &, const Position &) = default;
};

// What the flow reads. The view implements this; heights and cuts may be
// worked out on demand, so nothing here is const. Row numbers are valid for
// one revision of the rows (Geometry::revision); source lines survive it.
class RowSource
{
public:
    virtual ~RowSource() = default;

    virtual int count() = 0;
    // Height at the current column width.
    virtual int height(int row) = 0;
    // The first and last source line (0-based) the row shows.
    virtual int firstLine(int row) = 0;
    virtual int lastLine(int row) = 0;
    // The row that shows a source line (the nearest, if no row does).
    virtual int rowForLine(int line) = 0;

    virtual RowKind kind(int row) { return RowKind::Normal; }
    // Legal places to break the row, sorted, from its top. Empty: in one
    // piece. See paragraphCuts, codeCuts and tableCuts for the rules.
    virtual QList<int> cuts(int row) { Q_UNUSED(row); return {}; }
    // The top of each visual line, sorted (a table: each row). Used to nudge by
    // a line, to keep lines on a page turn, and to cut a row that is too tall.
    virtual QList<int> lineTops(int row) { Q_UNUSED(row); return {0}; }
    // A table's header row, repeated where the table continues in a column.
    virtual int headerHeight(int row) { Q_UNUSED(row); return 0; }

    // The source line shown at a place in a row, and the place that shows a
    // source line. The defaults spread the row's source lines evenly over its
    // visual lines (exact for code, where each source line is one); a source
    // that knows better overrides them.
    virtual int sourceLineAt(int row, int y);
    virtual int yOfLine(int row, int line);
};

// Break rules the renderer applies when it builds a row's cuts.
// Cut after line k of n only if k >= 2 and n - k >= 2. lineTops are the tops
// of the n lines; the cuts are the tops of the line after the cut.
QList<int> paragraphCuts(const QList<int> &lineTops);
// Code: at least 2 lines each side as well.
QList<int> codeCuts(const QList<int> &lineTops);
// Between body rows only, at least 2 body rows each side. rowTops are the tops
// of every row, the header rows (headerRows of them) first.
QList<int> tableCuts(const QList<int> &rowTops, int headerRows = 1);

// The input of a layout. The column width is already in the row heights;
// width and revision are here to tell when a remembered position (a row
// number) no longer means what it did.
struct Geometry
{
    int columns = 1;
    int height = 0; // Of a column.
    int width = 0;  // Of a column.
    int gap = 0;    // Between columns; the view places them, the flow does not use it.
    int revision = 0; // Of the rows.

    struct Key
    {
        int columns, height, width, revision;
        friend bool operator==(const Key &, const Key &) = default;
    };
    Key key() const { return {columns, height, width, revision}; }
};

// A part of a row in a column: from yFrom to yTo of the row.
struct Segment
{
    int row = 0;
    int yFrom = 0;
    int yTo = 0;
    // The table's header row goes above this part (it continues a table).
    // Its height counts against the column.
    bool repeatHeader = false;
    // From yFrom to keptTo is lines kept from the previous page, to be greyed.
    // keptTo <= yFrom: none.
    int keptTo = 0;
};

struct Column
{
    std::vector<Segment> segments;
    int used = 0; // Height taken, headers included.
};

struct Screen
{
    std::vector<Column> columns; // Always as many as the geometry has.
    Position start;              // The first place shown (gap rows skipped).
    Position end;                // The place the next column would start at.
    bool atStart = true;         // Nothing before start.
    bool atEnd = true;           // Nothing from end on.
    bool hasKept = false;        // Some segment has kept lines.

    bool empty() const { return columns.empty() || columns.front().segments.empty(); }
};

// Source lines on a screen, 0-based and inclusive.
struct LineRange
{
    int first = 0;
    int last = 0;
};

class ReadFlow
{
public:
    static constexpr int maxKeptLines = 5;

    explicit ReadFlow(RowSource *source);

    // The layout. A different layout (or revision) keeps the first source
    // line on screen and fills again from it, and forgets the page history.
    void setGeometry(const Geometry &geometry);
    const Geometry &geometry() const { return m_geometry; }
    // Lines of the previous page that stay at the top of the next, 0 to 5.
    void setKeptLines(int lines);
    int keptLines() const { return m_keptLines; }

    // The screen from start, as the rules make it. Changes nothing here.
    Screen fill(Position start) const;

    // The current screen. Empty until the first move or reflow().
    const Screen &screen() const { return m_screen; }
    // The first source line on screen: what survives a change of layout.
    int startLine() const { return m_startLine; }
    LineRange lineRange(const Screen &screen) const;
    LineRange lineRange() const { return lineRange(m_screen); }

    // Fills again after the rows changed under the same geometry (a render
    // finished, an edit): from the same place if the revision is the same, else
    // from the same source line. Lines kept by the page turn that made the
    // screen stay marked when it is filled from the same place.
    const Screen &reflow();
    // The moves. Each returns the screen it shows; one that cannot move (page
    // forward at the end) shows the same screen again.
    const Screen &pageForward(); // Keeps the last N lines, marked kept.
    const Screen &pageBack();
    const Screen &nudge(int lines); // Negative: up. Clears the kept lines.
    const Screen &toStart();
    // The last screen. Fills backwards from the last row, so only about a
    // screen of rows is measured however long the document is.
    const Screen &toEnd();
    const Screen &goToLine(int line);
    const Screen &goTo(Position position);

    // Source line <-> place. Survives reflow because lines do.
    int lineOf(Position position) const;
    Position positionOfLine(int line) const;
    // Height a segment takes in its column.
    int drawnHeight(const Segment &segment) const;

    // Page turns remembered for going back.
    int historySize() const { return int(m_history.size()); }

private:
    struct Step
    {
        Geometry::Key key;
        Position from; // The screen that was left...
        Position to;   // ...and the start of the one turned to.
    };

    Position endOfDocument() const;
    Position skipGaps(Position position) const;
    QList<int> tops(int row) const;
    std::optional<Position> nextLine(Position position) const;
    std::optional<Position> prevLine(Position position) const;
    Position forward(Position position, int lines) const;
    Position back(Position position, int lines) const;

    Position fillColumn(Position position, Column &column) const;
    void trimColumn(Column &column, int &row, int &y) const;
    Position startingBefore(Position end, std::optional<Position> limit) const;
    Position backwardStart(Position end) const;
    void markKept(Screen &screen, Position until) const;

    const Screen &show(Screen screen);
    void ensureScreen();

    RowSource *m_source;
    Geometry m_geometry;
    int m_keptLines = 2;
    Screen m_screen;
    Geometry::Key m_screenKey{};
    bool m_hasScreen = false;
    int m_startLine = 0;
    std::vector<Step> m_history;
    // The end of the page turned from, while the screen is the one a page
    // turn made (its kept lines are those before it).
    std::optional<Position> m_keptUntil;
};

} // namespace vn::md
