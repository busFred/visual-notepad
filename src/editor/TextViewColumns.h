// The state of TextView's columns (TextViewColumns.cpp); private to TextView.
#pragma once

#include "../markdown/ReadFlow.h"
#include "TextView.h"

#include <QElapsedTimer>
#include <QPointer>

#include <memory>
#include <vector>

class QAbstractButton;
class QLabel;
class QTimer;

namespace vn {

// The source lines as md::ReadFlow reads them: a row is a line, as tall as
// its wrapped rows, and it can be cut between any two of them (code has no
// "at least 2 lines" rule: a column ends where it is full).
class TextView::ColumnRows : public md::RowSource
{
public:
    explicit ColumnRows(TextView *view)
        : m_view(view)
    {
    }

    int count() override;
    int height(int row) override;
    int firstLine(int row) override { return row; }
    int lastLine(int row) override { return row; }
    int rowForLine(int line) override;
    QList<int> lineTops(int row) override;
    QList<int> cuts(int row) override;
    int minPieceLines(int row) override
    {
        Q_UNUSED(row);
        return 1;
    }
    int sourceLineAt(int row, int y) override
    {
        Q_UNUSED(y);
        return row;
    }
    int yOfLine(int row, int line) override
    {
        Q_UNUSED(row);
        Q_UNUSED(line);
        return 0;
    }

private:
    TextView *m_view;
};

struct TextView::Columns
{
    explicit Columns(TextView *view)
        : rows(view)
        , flow(&rows)
    {
    }

    ColumnRows rows;
    md::ReadFlow flow;
    ColumnLayout layout;
    int wanted = 0; // 0: as many as fit.
    int chars = 70;
    // Of the rows: a line added or removed, or every line laid out anew.
    int revision = 0;
    // Shown again from startLine at the next paint (turned on, a jump).
    bool needsStart = true;
    int startLine = 0;
    // The rows changed under the same numbering (colours, heights).
    bool dirty = false;
    // Only long lines' estimated heights settling made it dirty.
    bool settled = false;
    bool settling = false;
    // How far each column's lines are scrolled sideways (Word Wrap off).
    std::vector<qreal> offsets;
    struct BarDrag
    {
        bool active = false;
        int column = 0;
        qreal grab = 0; // Where on the thumb it was taken.
    } barDrag;
    int wheel = 0;
    bool wheelPixels = false;
    // What columnsChanged() last told: the lines on screen, the fit and the count.
    struct News
    {
        int first = -1;
        int last = -1;
        int fit = 0;
        int count = 0;
        bool shown = false;
        bool operator==(const News &) const = default;
    } news;
    bool newsPending = false;
    // A key press: the caret's column and the screen's start before it, and
    // whether the key held down has shown the note already (once a hold).
    struct Key
    {
        int column = -1;
        md::Position start;
        bool noted = false;
    } key;
    // "Caret moved to the next column", under the column it left.
    QPointer<QLabel> note;
    QPointer<QTimer> noteTimer;
    // When a drag past the last column last moved the view.
    QElapsedTimer dragShift;
    QPointer<QAbstractButton> back;
    QPointer<QAbstractButton> forward;
    QPointer<QAbstractButton> up;
    QPointer<QAbstractButton> down;
};

} // namespace vn
