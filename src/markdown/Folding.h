// What a parsed block is for collapsing, and the words that stand for it
// while it is collapsed: the chip of a table, figure, equation or code
// block, and the hint on a folded heading. No widgets: everything here reads
// the blocks the parser made.
#pragma once

#include "vn_core_bridge/lib.h"

#include <QString>

#include <span>

namespace vn::folding {

enum class FoldKind {
    Table,
    Figure,   // An image on its own, or a Mermaid diagram.
    Equation, // Display math.
    Code,     // Fenced or indented code, except Mermaid.
    Heading,  // Folds the section under it; see Fold::level.
    Other,    // Not collapsible by itself.
};

struct Fold
{
    FoldKind kind = FoldKind::Other;
    int level = 0; // 1 to 6 for a Heading, else 0.
    bool operator==(const Fold &) const = default;
};

Fold classify(const core::Block &block);

// The chip of a collapsed block: label ("Table · 5 × 6", "Code · bash · 3
// lines", "Equation", "Figure", "Diagram") and detail, the secondary text
// after it (a table's header cells, the first line of code, the TeX of an
// equation, the alt text of a picture). Both are one line; the detail is cut
// to a length that suits a chip (the view elides it further to the width it
// has). Empty for blocks that are not Table, Figure, Equation or Code.
//
// A table's size is columns × rows with the header row counted in the rows,
// as the design mockups show it (5 columns, a header and 5 rows: "5 × 6").
struct Chip
{
    QString label;
    QString detail;
};

Chip chipText(const core::Block &block);

// What the blocks of a folded section are, by kind in order of first
// appearance: "3 paragraphs", "1 list, 1 paragraph", "1 table, 2 paragraphs".
// Hidden blocks (link definitions) are not counted; empty if nothing is left.
QString sectionHint(std::span<const core::Block> hidden);

// The hint as it shows after a folded heading: "· 3 paragraphs hidden".
QString sectionHintLine(std::span<const core::Block> hidden);

// One past the last block of the section under the heading at `heading`:
// the blocks up to the next heading of the same or a higher level (a lower
// number), or the end. The section's blocks are [heading + 1, result), none
// when the next block is already such a heading. A block that is not a
// heading has no section: heading + 1.
int sectionEnd(std::span<const core::Block> blocks, int heading);

} // namespace vn::folding
