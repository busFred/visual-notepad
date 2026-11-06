//! Editing of GFM pipe tables as text: find the table around a line, and
//! rewrite it (rows, columns, alignment, sorting, one cell) as aligned text.
//!
//! Rows are numbered from the header (row 0) down; the delimiter line is not
//! a row. Columns are numbered from 0. Lines are 0-based and columns are
//! UTF-16 offsets within a line, the units of the Qt document.

use crate::ffi::{TableCell, TableEdit, TableInfo, TableOp};
use crate::format::{aligned_rows, delimiter_alignment, split_row, table_end};

/// Lines `[first, end)` of the table that holds `line`, if one does. Skips
/// fenced code, like the formatter.
#[cfg(test)]
pub fn find(text: &str, line: usize) -> Option<(usize, usize)> {
    let lines: Vec<&str> = text.split('\n').collect();
    find_in(&lines, line)
}

fn find_in(lines: &[&str], line: usize) -> Option<(usize, usize)> {
    let mut fence: Option<(char, usize)> = None;
    let mut i = 0;
    while i < lines.len() && i <= line {
        let trimmed = lines[i].trim_start_matches([' ', '>']);
        if let Some((ch, len)) = fence {
            let run = trimmed.chars().take_while(|&c| c == ch).count();
            if run >= len && trimmed[run..].trim().is_empty() {
                fence = None;
            }
        } else if let Some(ch) = trimmed.chars().next().filter(|&c| c == '`' || c == '~') {
            let run = trimmed.chars().take_while(|&c| c == ch).count();
            if run >= 3 {
                fence = Some((ch, run));
            }
        }
        if fence.is_none()
            && let Some(end) = table_end(lines, i)
        {
            if line < end {
                return Some((i, end));
            }
            i = end;
            continue;
        }
        i += 1;
    }
    None
}

/// A table as cells. `cells[0]` is the header; every row has `cols` cells.
struct Table {
    /// Text before the first pipe of the header line (list or quote markers).
    header_prefix: String,
    /// Text before the first pipe of the delimiter line; new rows get it.
    new_prefix: String,
    /// Per row, as written; `header_prefix` for the header.
    prefixes: Vec<String>,
    cells: Vec<Vec<String>>,
    /// (left colon, right colon) per column.
    align: Vec<(bool, bool)>,
}

impl Table {
    fn parse(lines: &[&str]) -> Option<Table> {
        let (header_prefix, header) = split_row(lines.first()?)?;
        let (new_prefix, delimiter) = split_row(lines.get(1)?)?;
        let cols = header.len();
        let align = delimiter
            .iter()
            .map(|c| delimiter_alignment(c).unwrap_or((false, false)))
            .collect::<Vec<_>>();
        if cols == 0 || align.len() != cols {
            return None;
        }
        let mut prefixes = vec![header_prefix.to_string()];
        let mut cells = vec![header];
        for line in &lines[2..] {
            let (prefix, mut row) = split_row(line)?;
            row.resize(cols, String::new());
            prefixes.push(prefix.to_string());
            cells.push(row);
        }
        Some(Table {
            header_prefix: header_prefix.to_string(),
            new_prefix: new_prefix.to_string(),
            prefixes,
            cells,
            align,
        })
    }

    fn rows(&self) -> usize {
        self.cells.len()
    }

    fn cols(&self) -> usize {
        self.align.len()
    }

    fn blank_row(&self) -> Vec<String> {
        vec![String::new(); self.cols()]
    }

    /// The aligned lines of the table, header, delimiter, then body rows.
    fn lines(&self) -> Vec<String> {
        let rows = self.prefixes.iter().zip(&self.cells).map(|(p, c)| (p.as_str(), c.as_slice()));
        aligned_rows(rows, self.rows(), &self.new_prefix, &self.align)
    }

    fn insert_row(&mut self, at: usize) {
        let blank = self.blank_row();
        self.cells.insert(at, blank);
        self.prefixes.insert(at, self.new_prefix.clone());
    }

    fn swap_rows(&mut self, a: usize, b: usize) {
        self.cells.swap(a, b);
        self.prefixes.swap(a, b);
    }
}

/// Char indices of the pipes that are not escaped.
fn pipes(line: &str) -> Vec<usize> {
    let mut found = Vec::new();
    let mut escaped = false;
    for (i, c) in line.chars().enumerate() {
        if escaped {
            escaped = false;
        } else if c == '\\' {
            escaped = true;
        } else if c == '|' {
            found.push(i);
        }
    }
    found
}

fn utf16_to_char(line: &str, offset: usize) -> usize {
    let mut units = 0;
    for (i, c) in line.chars().enumerate() {
        if units >= offset {
            return i;
        }
        units += c.len_utf16();
    }
    line.chars().count()
}

fn char_to_utf16(line: &str, index: usize) -> usize {
    line.chars().take(index).map(char::len_utf16).sum()
}

/// The column of the cell holding char `index` of a row of `cols` columns.
fn column_at(line: &str, index: usize, cols: usize) -> usize {
    let before = pipes(line).iter().filter(|&&p| p < index).count();
    before.saturating_sub(1).min(cols.saturating_sub(1))
}

/// Char range of the text of cell `col` in `line` (trimmed). An empty cell
/// gives an empty range where its text would go.
fn cell_range(line: &str, col: usize) -> Option<(usize, usize)> {
    let p = pipes(line);
    let (open, close) = (*p.get(col)?, *p.get(col + 1)?);
    let chars: Vec<char> = line.chars().collect();
    let mut start = open + 1;
    let mut end = close;
    while start < end && chars[start].is_whitespace() {
        start += 1;
    }
    while end > start && chars[end - 1].is_whitespace() {
        end -= 1;
    }
    if start == end {
        // Empty: after the space that follows the pipe, if there is one.
        start = if open + 1 < close && chars[open + 1] == ' ' { open + 2 } else { open + 1 };
        end = start;
    }
    Some((start, end))
}

fn align_code(align: (bool, bool)) -> u8 {
    match align {
        (false, false) => 0,
        (true, false) => 1,
        (true, true) => 2,
        (false, true) => 3,
    }
}

/// The line of `table` that holds `row`.
fn line_of_row(row: usize) -> usize {
    if row == 0 { 0 } else { row + 1 }
}

fn unfound_info() -> TableInfo {
    TableInfo {
        found: false,
        first_line: 0,
        last_line: 0,
        rows: 0,
        cols: 0,
        row: 0,
        col: 0,
        alignment: 0,
        aligned: true,
    }
}

/// Tables with more lines than this are not checked for `aligned` (which
/// takes realigning the whole table): they say they are not.
const ALIGNED_CHECK_MAX: usize = 500;

pub fn info(text: &str, line: usize, column: usize) -> TableInfo {
    let lines: Vec<&str> = text.split('\n').collect();
    let Some((first, end)) = find_in(&lines, line) else {
        return unfound_info();
    };
    // Asked after every keystroke in a table: only the header and the
    // delimiter line are read (table_end checked the delimiter's cells).
    let (Some((_, header)), Some((_, delimiter))) = (split_row(lines[first]), split_row(lines[first + 1])) else {
        return unfound_info();
    };
    let cols = header.len();
    if cols == 0 || delimiter.len() != cols {
        return unfound_info();
    }
    let at = line - first;
    // The delimiter line counts as the header.
    let row = at.saturating_sub(1);
    let index = utf16_to_char(lines[line], column);
    let col = column_at(lines[line], index, cols);
    let alignment = delimiter_alignment(&delimiter[col]).unwrap_or((false, false));
    let aligned = end - first <= ALIGNED_CHECK_MAX
        && Table::parse(&lines[first..end]).is_some_and(|table| table.lines() == lines[first..end]);
    TableInfo {
        found: true,
        first_line: first,
        last_line: end - 1,
        rows: end - first - 1,
        cols,
        row,
        col,
        alignment: align_code(alignment),
        aligned,
    }
}

pub fn cell(text: &str, line: usize, row: usize, col: usize) -> TableCell {
    let none = TableCell { found: false, line: 0, start: 0, end: 0, text: String::new() };
    let lines: Vec<&str> = text.split('\n').collect();
    let Some((first, end)) = find_in(&lines, line) else {
        return none;
    };
    let target = first + line_of_row(row);
    if target >= end {
        return none;
    }
    let source = lines[target];
    let Some((start, stop)) = cell_range(source, col) else {
        return none;
    };
    TableCell {
        found: true,
        line: target,
        start: char_to_utf16(source, start),
        end: char_to_utf16(source, stop),
        text: source.chars().skip(start).take(stop - start).collect(),
    }
}

/// A cell's text as it goes into a row: one line, pipes escaped.
fn clean_cell(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut escaped = false;
    let flat = text.replace("\r\n", "\n").replace(['\r', '\n'], "<br>");
    for c in flat.trim().chars() {
        if escaped {
            escaped = false;
        } else if c == '\\' {
            escaped = true;
        } else if c == '|' {
            out.push('\\');
        }
        out.push(c);
    }
    out
}

fn number(cell: &str) -> Option<f64> {
    cell.trim().replace(',', "").parse().ok()
}

fn sort_body(table: &mut Table, col: usize, descending: bool) {
    let mut body: Vec<(String, Vec<String>)> = table
        .prefixes
        .drain(1..)
        .zip(table.cells.drain(1..))
        .collect();
    body.sort_by(|a, b| {
        let (x, y) = (&a.1[col], &b.1[col]);
        let order = match (number(x), number(y)) {
            (Some(p), Some(q)) => p.partial_cmp(&q).unwrap_or(std::cmp::Ordering::Equal),
            _ => x.to_lowercase().cmp(&y.to_lowercase()),
        };
        if descending { order.reverse() } else { order }
    });
    for (prefix, row) in body {
        table.prefixes.push(prefix);
        table.cells.push(row);
    }
}

fn refused() -> TableEdit {
    TableEdit {
        ok: false,
        changed: false,
        remove: false,
        first_line: 0,
        last_line: 0,
        replacement: String::new(),
        caret_line: 0,
        caret_column: 0,
        keep_before: 0,
        keep_after: 0,
    }
}

/// Applies `op` to the table holding `line`. `row` and `col` name the cell
/// the operation is about; `arg` is the new text for `SetCell`.
pub fn edit(text: &str, line: usize, op: TableOp, row: usize, col: usize, arg: &str) -> TableEdit {
    let lines: Vec<&str> = text.split('\n').collect();
    let Some((first, end)) = find_in(&lines, line) else {
        return refused();
    };
    let Some(mut table) = Table::parse(&lines[first..end]) else {
        return refused();
    };
    let (rows, cols) = (table.rows(), table.cols());
    if row >= rows || col >= cols {
        return refused();
    }
    let (mut at_row, mut at_col) = (row, col);
    match op {
        TableOp::RowAbove => {
            if row == 0 {
                return refused();
            }
            table.insert_row(row);
        }
        TableOp::RowBelow => {
            table.insert_row(row + 1);
            at_row = row + 1;
        }
        TableOp::ColumnLeft => {
            for r in &mut table.cells {
                r.insert(col, String::new());
            }
            table.align.insert(col, (false, false));
        }
        TableOp::ColumnRight => {
            for r in &mut table.cells {
                r.insert(col + 1, String::new());
            }
            table.align.insert(col + 1, (false, false));
            at_col = col + 1;
        }
        TableOp::DeleteRow => {
            if row == 0 {
                return refused();
            }
            table.cells.remove(row);
            table.prefixes.remove(row);
            at_row = row.min(table.rows() - 1);
        }
        TableOp::DeleteColumn => {
            if cols == 1 {
                return refused();
            }
            for r in &mut table.cells {
                r.remove(col);
            }
            table.align.remove(col);
            at_col = col.min(cols - 2);
        }
        TableOp::DeleteTable => {
            return TableEdit {
                ok: true,
                changed: true,
                remove: true,
                first_line: first,
                last_line: end - 1,
                replacement: String::new(),
                caret_line: first,
                caret_column: 0,
                keep_before: 0,
                keep_after: 0,
            };
        }
        TableOp::RowUp => {
            if row < 2 {
                return refused();
            }
            table.swap_rows(row, row - 1);
            at_row = row - 1;
        }
        TableOp::RowDown => {
            if row == 0 || row + 1 >= rows {
                return refused();
            }
            table.swap_rows(row, row + 1);
            at_row = row + 1;
        }
        TableOp::ColumnBefore => {
            if col == 0 {
                return refused();
            }
            for r in &mut table.cells {
                r.swap(col, col - 1);
            }
            table.align.swap(col, col - 1);
            at_col = col - 1;
        }
        TableOp::ColumnAfter => {
            if col + 1 >= cols {
                return refused();
            }
            for r in &mut table.cells {
                r.swap(col, col + 1);
            }
            table.align.swap(col, col + 1);
            at_col = col + 1;
        }
        TableOp::AlignNone => table.align[col] = (false, false),
        TableOp::AlignLeft => table.align[col] = (true, false),
        TableOp::AlignCenter => table.align[col] = (true, true),
        TableOp::AlignRight => table.align[col] = (false, true),
        TableOp::SortAscending | TableOp::SortDescending => {
            if rows < 3 {
                return refused();
            }
            sort_body(&mut table, col, op == TableOp::SortDescending);
        }
        TableOp::Transpose => {
            let swapped: Vec<Vec<String>> = (0..cols)
                .map(|c| table.cells.iter().map(|r| r[c].clone()).collect())
                .collect();
            let new_cols = rows;
            table.prefixes = vec![table.header_prefix.clone()];
            table.prefixes.extend(vec![table.new_prefix.clone(); cols - 1]);
            table.cells = swapped;
            table.align = vec![(false, false); new_cols];
            at_row = 0;
            at_col = 0;
        }
        TableOp::Realign => {}
        TableOp::SetCell => table.cells[row][col] = clean_cell(arg),
        _ => return refused(),
    }
    let output = table.lines();
    let original = &lines[first..end];
    let (keep_before, keep_after) = unchanged_ends(original, &output);
    // Caret at the start of the text of the cell the operation ended in.
    let caret_line = first + line_of_row(at_row.min(table.rows() - 1));
    let target = &output[line_of_row(at_row.min(table.rows() - 1))];
    let caret_column = cell_range(target, at_col.min(table.cols() - 1))
        .map_or(0, |(start, _)| char_to_utf16(target, start));
    TableEdit {
        ok: true,
        changed: output.len() != original.len() || keep_before < output.len(),
        remove: false,
        first_line: first,
        last_line: end - 1,
        replacement: output.join("\n"),
        caret_line,
        caret_column,
        keep_before,
        keep_after,
    }
}

/// How many lines at the start and at the end the old and the new lines of
/// a table have in common, leaving at least one of each between: only those
/// need replacing, and a realigned table is mostly as it was.
fn unchanged_ends(old: &[&str], new: &[String]) -> (usize, usize) {
    let most = old.len().min(new.len()).saturating_sub(1);
    let before = old.iter().zip(new).take(most).take_while(|(a, b)| **a == b.as_str()).count();
    let after = old
        .iter()
        .rev()
        .zip(new.iter().rev())
        .take(most - before)
        .take_while(|(a, b)| **a == b.as_str())
        .count();
    if old.len() == new.len() && before == most && old[most] == new[most] {
        return (new.len(), 0); // Nothing changed.
    }
    (before, after)
}

#[cfg(test)]
mod tests {
    use super::*;

    const T: &str = "| a   | b   |\n| --- | --- |\n| 1   | 2   |\n| 3   | 4   |";

    fn run(text: &str, line: usize, op: TableOp, row: usize, col: usize) -> TableEdit {
        edit(text, line, op, row, col, "")
    }

    #[test]
    fn finds_the_table_around_a_line() {
        let doc = format!("intro\n\n{T}\n\nafter");
        assert_eq!(find(&doc, 2), Some((2, 6)));
        assert_eq!(find(&doc, 5), Some((2, 6)));
        assert_eq!(find(&doc, 0), None);
        assert_eq!(find(&doc, 7), None);
    }

    #[test]
    fn code_fences_hold_no_tables() {
        let doc = "```\n| a | b |\n|---|---|\n| 1 | 2 |\n```\n";
        assert_eq!(find(doc, 2), None);
    }

    #[test]
    fn info_gives_row_column_and_alignment() {
        let doc = "x\n| a | b |\n| :-: | --: |\n| 1 | 2 |";
        // Column 8 is inside the second cell of the last line.
        let i = info(doc, 3, 8);
        assert!(i.found);
        assert_eq!((i.first_line, i.last_line, i.rows, i.cols), (1, 3, 2, 2));
        assert_eq!((i.row, i.col, i.alignment), (1, 1, 3));
        assert!(!i.aligned);
        // The delimiter line counts as the header.
        assert_eq!(info(doc, 2, 0).row, 0);
        assert!(!info("plain", 0, 0).found);
    }

    #[test]
    fn long_tables_are_not_checked_for_alignment() {
        // ALIGNED_CHECK_MAX lines in all: the header, its delimiter and the rows.
        let mut doc = String::from("| a | b |\n|---|---|\n");
        for _ in 0..ALIGNED_CHECK_MAX - 2 {
            doc.push_str("| 1 | 2 |\n");
        }
        let short = run(&doc, 0, TableOp::Realign, 0, 0).replacement;
        assert!(info(&short, 0, 0).aligned);
        assert_eq!(info(&short, 0, 0).rows, ALIGNED_CHECK_MAX - 1);
        let long = format!("{short}\n| 1   | 2   |");
        assert!(!info(&long, 0, 0).aligned);
        assert_eq!(info(&long, 0, 0).rows, ALIGNED_CHECK_MAX);
    }

    #[test]
    fn column_is_clamped_outside_the_pipes() {
        let doc = "  | a | b |  \n|---|---|\n";
        assert_eq!(info(doc, 0, 0).col, 0);
        assert_eq!(info(doc, 0, 13).col, 1);
    }

    #[test]
    fn cell_spans_use_utf16_columns() {
        let doc = "| 😀 | b |\n|---|---|\n";
        let c = cell(doc, 0, 0, 1);
        assert!(c.found);
        assert_eq!(c.text, "b");
        // "| 😀 | " is 7 UTF-16 units.
        assert_eq!((c.line, c.start, c.end), (0, 7, 8));
        assert!(!cell(doc, 0, 0, 2).found);
        assert!(!cell(doc, 0, 5, 0).found);
    }

    #[test]
    fn empty_cells_give_the_spot_for_text() {
        let c = cell("|   |\n|---|\n", 0, 0, 0);
        assert!(c.found);
        assert_eq!((c.start, c.end), (2, 2));
    }

    #[test]
    fn rows_are_inserted_and_deleted() {
        let r = run(T, 2, TableOp::RowBelow, 1, 0);
        assert_eq!(
            r.replacement,
            "| a   | b   |\n| --- | --- |\n| 1   | 2   |\n|     |     |\n| 3   | 4   |"
        );
        assert_eq!((r.caret_line, r.caret_column), (3, 2));
        // A row above the header cannot be.
        assert!(!run(T, 0, TableOp::RowAbove, 0, 0).ok);
        let r = run(T, 2, TableOp::RowAbove, 1, 1);
        assert!(r.replacement.contains("| 1   | 2   |\n"));
        assert_eq!((r.caret_line, r.caret_column), (2, 8));
        let r = run(T, 3, TableOp::DeleteRow, 2, 0);
        assert_eq!(r.replacement, "| a   | b   |\n| --- | --- |\n| 1   | 2   |");
        assert_eq!(r.caret_line, 2);
        assert!(!run(T, 0, TableOp::DeleteRow, 0, 0).ok);
    }

    #[test]
    fn columns_are_inserted_deleted_and_moved() {
        let r = run(T, 0, TableOp::ColumnRight, 0, 0);
        assert_eq!(
            r.replacement,
            "| a   |     | b   |\n| --- | --- | --- |\n| 1   |     | 2   |\n| 3   |     | 4   |"
        );
        assert_eq!(r.caret_column, 8);
        let r = run(T, 0, TableOp::ColumnLeft, 0, 1);
        assert!(r.replacement.starts_with("| a   |     | b   |"));
        let r = run(T, 0, TableOp::DeleteColumn, 0, 0);
        assert_eq!(r.replacement, "| b   |\n| --- |\n| 2   |\n| 4   |");
        assert!(!run(&r.replacement, 0, TableOp::DeleteColumn, 0, 0).ok);
        let r = run(T, 0, TableOp::ColumnBefore, 0, 1);
        assert!(r.replacement.starts_with("| b   | a   |"));
        assert!(!run(T, 0, TableOp::ColumnBefore, 0, 0).ok);
        assert!(!run(T, 0, TableOp::ColumnAfter, 0, 1).ok);
    }

    #[test]
    fn rows_move_but_the_header_stays() {
        let r = run(T, 3, TableOp::RowUp, 2, 0);
        assert_eq!(r.replacement, "| a   | b   |\n| --- | --- |\n| 3   | 4   |\n| 1   | 2   |");
        assert_eq!(r.caret_line, 2);
        assert!(!run(T, 2, TableOp::RowUp, 1, 0).ok);
        assert!(!run(T, 3, TableOp::RowDown, 2, 0).ok);
        assert!(run(T, 2, TableOp::RowDown, 1, 0).ok);
    }

    #[test]
    fn alignment_is_set_per_column() {
        let r = run(T, 0, TableOp::AlignRight, 0, 1);
        assert_eq!(r.replacement, "| a   |   b |\n| --- | --: |\n| 1   |   2 |\n| 3   |   4 |");
        let r = run(T, 0, TableOp::AlignCenter, 0, 0);
        assert!(r.replacement.contains("| :-: | --- |"));
        let r = run(&r.replacement, 0, TableOp::AlignNone, 0, 0);
        assert_eq!(r.replacement, T);
        assert!(run(T, 0, TableOp::Realign, 0, 0).ok && !run(T, 0, TableOp::Realign, 0, 0).changed);
    }

    #[test]
    fn sorting_is_numeric_for_numbers_and_stable() {
        let doc = "| n | s |\n|---|---|\n| 10 | b |\n| 9 | B |\n| 2 | a |";
        let r = run(doc, 0, TableOp::SortAscending, 0, 0);
        assert_eq!(
            r.replacement,
            "| n   | s   |\n| --- | --- |\n| 2   | a   |\n| 9   | B   |\n| 10  | b   |"
        );
        let r = run(doc, 0, TableOp::SortDescending, 0, 1);
        assert!(r.replacement.ends_with("| 2   | a   |"));
        // Text sorts without regard to case; equal ones keep their order.
        let at = r.replacement.find("| 10").unwrap();
        assert!(at < r.replacement.find("| 9").unwrap());
    }

    #[test]
    fn transposing_swaps_rows_and_columns() {
        let r = run(T, 0, TableOp::Transpose, 0, 0);
        assert_eq!(r.replacement, "| a   | 1   | 3   |\n| --- | --- | --- |\n| b   | 2   | 4   |");
    }

    #[test]
    fn setting_a_cell_keeps_it_on_one_line_with_pipes_escaped() {
        let r = edit(T, 0, TableOp::SetCell, 1, 0, "x | y\nz");
        assert_eq!(r.replacement.lines().nth(2).unwrap(), "| x \\| y<br>z | 2   |");
        let r = edit(T, 0, TableOp::SetCell, 1, 0, "a\\|b");
        assert!(r.replacement.contains("| a\\|b | 2"));
        let r = edit(T, 0, TableOp::SetCell, 0, 0, "");
        assert!(r.replacement.starts_with("|     | b   |"));
    }

    #[test]
    fn short_and_long_rows_are_made_even() {
        let doc = "| a | b |\n|---|---|\n| 1 |\n| 1 | 2 | 3 |";
        let r = run(doc, 0, TableOp::Realign, 0, 0);
        assert_eq!(
            r.replacement,
            "| a   | b   |\n| --- | --- |\n| 1   |     |\n| 1   | 2   |"
        );
    }

    #[test]
    fn quoted_and_listed_tables_keep_their_prefix() {
        let doc = "> | a | b |\n> |---|---|\n> | 1 | 2 |";
        let r = run(doc, 2, TableOp::RowBelow, 1, 0);
        assert!(r.replacement.ends_with("> | 1   | 2   |\n> |     |     |"), "{}", r.replacement);
        let doc = "- | a | b |\n  |---|---|\n  | 1 | 2 |";
        let r = run(doc, 0, TableOp::RowBelow, 0, 0);
        assert_eq!(
            r.replacement,
            "- | a   | b   |\n  | --- | --- |\n  |     |     |\n  | 1   | 2   |"
        );
    }

    #[test]
    fn only_the_changed_lines_are_marked_for_replacing() {
        // A cell no wider than its column changes one line.
        let r = edit(T, 0, TableOp::SetCell, 1, 0, "9");
        assert_eq!((r.keep_before, r.keep_after), (2, 1));
        // A wider one, every line (the column widens).
        let r = edit(T, 0, TableOp::SetCell, 1, 0, "wider");
        assert_eq!((r.keep_before, r.keep_after), (0, 0));
        // Inserted and deleted rows: the lines around them are kept, and at
        // least one line of the old and the new table is replaced.
        let r = run(T, 2, TableOp::RowBelow, 1, 0);
        assert_eq!((r.keep_before, r.keep_after), (3, 0));
        let r = run(T, 3, TableOp::DeleteRow, 2, 0);
        assert_eq!((r.keep_before, r.keep_after), (2, 0));
        let r = run(T, 3, TableOp::RowBelow, 2, 0);
        assert_eq!((r.keep_before, r.keep_after), (3, 0));
        // Nothing changed: all kept.
        let r = run(T, 0, TableOp::Realign, 0, 0);
        assert!(!r.changed);
        assert_eq!(r.keep_before, 4);
    }


    #[test]
    fn deleting_the_table_removes_its_lines() {
        let r = run(T, 1, TableOp::DeleteTable, 0, 0);
        assert!(r.ok && r.remove);
        assert_eq!((r.first_line, r.last_line), (0, 3));
    }

    #[test]
    fn nothing_changes_outside_tables() {
        assert!(!run("just text", 0, TableOp::Realign, 0, 0).ok);
    }
}
