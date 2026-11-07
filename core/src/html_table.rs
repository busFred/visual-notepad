//! Source-preserving edits of HTML tables. Offsets in the model are bytes;
//! offsets crossing the bridge are lines and UTF-16 columns.

use crate::ffi::{TableCell, TableEdit, TableInfo, TableOp};
use crate::table::{bit, refused, unchanged_ends, unfound_info};
use std::ops::Range;

#[derive(Clone, Debug)]
struct Tag {
    range: Range<usize>,
    name: String,
    end: bool,
    attrs: Vec<Attribute>,
}

#[derive(Clone, Debug)]
struct Attribute {
    name: String,
    range: Range<usize>,
    value: Range<usize>,
}

/// Scan tags, respecting quoted attributes and skipping comments entirely.
fn tags(src: &str) -> Vec<Tag> {
    let b = src.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        if b[i] != b'<' {
            i += 1;
            continue;
        }
        if src[i..].starts_with("<!--") {
            i = src[i + 4..].find("-->").map_or(b.len(), |n| i + 4 + n + 3);
            continue;
        }
        let start = i;
        i += 1;
        let end = b.get(i) == Some(&b'/');
        if end {
            i += 1;
        }
        let name = i;
        while i < b.len() && (b[i].is_ascii_alphanumeric() || b[i] == b'-') {
            i += 1;
        }
        if i == name || !b.get(i).is_some_and(|c| c.is_ascii_whitespace() || *c == b'>' || *c == b'/') {
            continue;
        }
        let name = src[name..i].to_ascii_lowercase();
        let mut attrs = Vec::new();
        while i < b.len() && b[i] != b'>' {
            if b[i].is_ascii_whitespace() || b[i] == b'/' {
                i += 1;
                continue;
            }
            let a = i;
            while i < b.len() && !b[i].is_ascii_whitespace() && !b"=/>".contains(&b[i]) {
                i += 1;
            }
            if a == i {
                i += 1;
                continue;
            }
            let key = src[a..i].to_ascii_lowercase();
            let name_end = i;
            while i < b.len() && b[i].is_ascii_whitespace() {
                i += 1;
            }
            let mut value = i..i;
            if b.get(i) == Some(&b'=') {
                i += 1;
                while i < b.len() && b[i].is_ascii_whitespace() {
                    i += 1;
                }
                if b.get(i).is_some_and(|c| *c == b'\'' || *c == b'"') {
                    let quote = b[i];
                    i += 1;
                    let v = i;
                    while i < b.len() && b[i] != quote {
                        i += 1;
                    }
                    value = v..i;
                    if i < b.len() {
                        i += 1;
                    }
                } else {
                    let v = i;
                    while i < b.len() && !b[i].is_ascii_whitespace() && b[i] != b'>' {
                        i += 1;
                    }
                    value = v..i;
                }
            }
            let end = if src[a..i].contains('=') { i } else { name_end };
            attrs.push(Attribute { name: key, range: a..end, value });
        }
        if i == b.len() {
            break;
        }
        i += 1;
        out.push(Tag { range: start..i, name, end, attrs });
    }
    out
}

#[derive(Clone)]
struct Cell {
    tag: Tag,
    content: Range<usize>,
    close: Option<Range<usize>>,
    end: usize,
    col: usize,
    colspan: usize,
    rowspan: usize,
}

impl Cell {
    fn text_range(&self, src: &str) -> Range<usize> {
        let content = &src[self.content.clone()];
        let trimmed = content.trim();
        if trimmed.is_empty() {
            return self.content.start..self.content.start;
        }
        let start = self.content.start + content.len() - content.trim_start().len();
        start..start + trimmed.len()
    }
    fn range(&self) -> Range<usize> {
        self.tag.range.start..self.end
    }
    fn attr<'a>(&self, src: &'a str, key: &str) -> Option<&'a str> {
        self.tag.attrs.iter().find(|a| a.name == key).map(|a| &src[a.value.clone()])
    }
    fn alignment(&self, src: &str) -> u8 {
        let code = |s: &str| match s.trim().to_ascii_lowercase().as_str() {
            "left" => 1,
            "center" => 2,
            "right" => 3,
            _ => 0,
        };
        if let Some(a) = self.attr(src, "align") {
            return code(a);
        }
        self.attr(src, "style")
            .and_then(|s| {
                s.split(';').find_map(|d| {
                    let (k, v) = d.split_once(':')?;
                    k.trim().eq_ignore_ascii_case("text-align").then(|| code(v))
                })
            })
            .unwrap_or(0)
    }
}

struct Row {
    tag: Tag,
    end: usize,
    section: usize,
    head: bool,
    cells: Vec<Cell>,
}
impl Row {
    fn range(&self) -> Range<usize> {
        self.tag.range.start..self.end
    }
}

struct Table {
    open: Tag,
    close: Tag,
    rows: Vec<Row>,
    grid: Vec<Vec<Option<(usize, usize)>>>,
    cols: usize,
    merged: bool,
    first: usize,
    last: usize,
}

/// Newline positions use the editor's literal `\n` convention.
fn position(src: &str, offset: usize) -> (usize, usize) {
    let before = &src[..offset];
    (before.bytes().filter(|&b| b == b'\n').count(), before.rsplit('\n').next().unwrap_or("").encode_utf16().count())
}
fn offset(src: &str, line: usize, column: usize) -> usize {
    let start = src.split_inclusive('\n').take(line).map(str::len).sum::<usize>();
    let mut units = 0;
    let mut end = start;
    for c in src[start.min(src.len())..].chars().take_while(|&c| c != '\n') {
        if units >= column {
            break;
        }
        units += c.len_utf16();
        end += c.len_utf8();
    }
    end.min(src.len())
}

fn finish_cell(row: &mut Option<Row>, cell: &mut Option<Cell>, at: usize, close: Option<Range<usize>>) {
    if let Some(mut c) = cell.take() {
        c.content.end = at;
        c.end = close.as_ref().map_or(at, |r| r.end);
        c.close = close;
        if let Some(r) = row {
            r.end = c.end;
            r.cells.push(c);
        }
    }
}
fn finish_row(rows: &mut Vec<Row>, row: &mut Option<Row>, end: Option<usize>) {
    if let Some(mut r) = row.take() {
        if let Some(end) = end {
            r.end = end;
        }
        rows.push(r);
    }
}

fn parse(src: &str, open: Tag, close: Tag, tokens: &[Tag]) -> Table {
    let mut rows = Vec::new();
    let mut row = None;
    let mut cell = None;
    let mut section = 0;
    let mut head = false;
    for t in tokens {
        match t.name.as_str() {
            "td" | "th" => {
                finish_cell(&mut row, &mut cell, t.range.start, t.end.then(|| t.range.clone()));
                if !t.end && row.is_some() {
                    let span = |key| {
                        t.attrs
                            .iter()
                            .find(|a| a.name == key)
                            .and_then(|a| src[a.value.clone()].parse::<usize>().ok())
                            .unwrap_or(1)
                            .clamp(1, 65534)
                    };
                    cell = Some(Cell {
                        tag: t.clone(),
                        content: t.range.end..t.range.end,
                        close: None,
                        end: t.range.end,
                        col: 0,
                        colspan: span("colspan"),
                        rowspan: span("rowspan"),
                    });
                }
            }
            "tr" | "thead" | "tbody" | "tfoot" | "table" => {
                finish_cell(&mut row, &mut cell, t.range.start, None);
                finish_row(&mut rows, &mut row, (t.name == "tr" && t.end).then_some(t.range.end));
                if t.name == "tr" && !t.end {
                    row = Some(Row { tag: t.clone(), end: t.range.end, section, head, cells: Vec::new() });
                } else if matches!(t.name.as_str(), "thead" | "tbody" | "tfoot") {
                    section = if t.end { 0 } else { t.range.start + 1 };
                    head = !t.end && t.name == "thead";
                }
            }
            _ => {}
        }
    }
    let count = rows.len();
    let mut grid = vec![Vec::new(); count];
    let mut merged = false;
    for (r, row) in rows.iter_mut().enumerate() {
        let mut col = 0;
        for (c, cell) in row.cells.iter_mut().enumerate() {
            while grid[r].get(col).is_some_and(Option::is_some) {
                col += 1;
            }
            cell.col = col;
            merged |= cell.colspan > 1 || cell.rowspan > 1;
            for slots in grid.iter_mut().take((r + cell.rowspan).min(count)).skip(r) {
                slots.resize(slots.len().max(col + cell.colspan), None);
                for slot in &mut slots[col..col + cell.colspan] {
                    *slot = Some((r, c));
                }
            }
            col += cell.colspan;
        }
    }
    let cols = grid.iter().map(Vec::len).max().unwrap_or(0);
    for r in &mut grid {
        r.resize(cols, None);
    }
    Table {
        first: position(src, open.range.start).0,
        last: position(src, close.range.end).0,
        open,
        close,
        rows,
        grid,
        cols,
        merged,
    }
}

/// Find a complete, non-nested table beginning at a CommonMark HTML block.
fn find(src: &str, line: usize) -> Option<Table> {
    let tokens = tags(src);
    let mut fence = None;
    let mut start = 0;
    let mut blocked_until = 0;
    for (i, text) in src.split('\n').enumerate() {
        if i > line {
            break;
        }
        let trimmed = text.trim_start_matches([' ', '>']);
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
        let spaces = text.bytes().take_while(|&b| b == b' ').count();
        if fence.is_none()
            && spaces <= 3
            && start + spaces >= blocked_until
            && let Some(n) = tokens
                .binary_search_by_key(&(start + spaces), |t| t.range.start)
                .ok()
                .filter(|&n| tokens[n].name == "table" && !tokens[n].end)
        {
            let mut nested = false;
            let mut depth = 1;
            let end = (n + 1..tokens.len()).find(|&j| {
                if tokens[j].name != "table" {
                    return false;
                }
                if !tokens[j].end {
                    nested = true;
                    depth += 1;
                } else {
                    depth -= 1;
                }
                depth == 0
            })?;
            blocked_until = tokens[end].range.end;
            let table = parse(src, tokens[n].clone(), tokens[end].clone(), &tokens[n + 1..=end]);
            if line <= table.last {
                return (!nested).then_some(table);
            }
        }
        start += text.len() + 1;
    }
    None
}

impl Table {
    fn header(&self) -> bool {
        self.rows.first().is_some_and(|r| r.head || (!r.cells.is_empty() && r.cells.iter().all(|c| c.tag.name == "th")))
    }
    fn cell(&self, row: usize, col: usize) -> Option<&Cell> {
        let (r, c) = self.grid.get(row)?.get(col)?.as_ref()?;
        self.rows.get(*r)?.cells.get(*c)
    }
    fn at(&self, at: usize) -> (usize, usize) {
        for (r, row) in self.rows.iter().enumerate() {
            if row.tag.range.start <= at && at < row.end {
                let c = row
                    .cells
                    .iter()
                    .find(|c| c.tag.range.start <= at && at < c.end)
                    .or_else(|| row.cells.iter().rev().find(|c| c.tag.range.start <= at))
                    .or_else(|| row.cells.first());
                return (r, c.map_or(0, |c| c.col));
            }
        }
        (0, 0)
    }
    fn whole_lines(&self, src: &str) -> bool {
        let start = src[..self.open.range.start].rfind('\n').map_or(0, |n| n + 1);
        let end = src[self.close.range.end..].find('\n').map_or(src.len(), |n| self.close.range.end + n);
        src[start..self.open.range.start].trim().is_empty() && src[self.close.range.end..end].trim().is_empty()
    }
    fn to_pipe(&self, src: &str) -> bool {
        self.whole_lines(src)
            && position(src, self.open.range.start).1 == 0
            && src.split('\n').enumerate().all(|(i, l)| (self.first..=self.last).contains(&i) || l.trim().is_empty())
            && self.rows.iter().flat_map(|r| &r.cells).all(|c| {
                let text = src[c.content.clone()].to_ascii_lowercase();
                !text.contains("<pre") && !text.contains("<table")
            })
    }
    fn accepts(&self, src: &str, op: TableOp, row: usize, col: usize) -> bool {
        let rows = self.rows.len();
        if op == TableOp::DeleteTable {
            return self.whole_lines(src);
        }
        if row >= rows || col >= self.cols {
            return false;
        }
        if self.merged && op != TableOp::SetCell && op != TableOp::DeleteTable {
            return false;
        }
        match op {
            TableOp::RowAbove => row > 0 || !self.header(),
            TableOp::DeleteRow => rows > 1 && (row > 0 || !self.header()),
            TableOp::DeleteColumn => self.cols > 1,
            TableOp::DeleteTable => self.whole_lines(src),
            TableOp::RowUp => row > usize::from(self.header()) && self.rows[row].section == self.rows[row - 1].section,
            TableOp::RowDown => {
                (row > 0 || !self.header()) && row + 1 < rows && self.rows[row].section == self.rows[row + 1].section
            }
            TableOp::ColumnBefore => col > 0,
            TableOp::ColumnAfter => col + 1 < self.cols,
            TableOp::SortAscending | TableOp::SortDescending => {
                let body = &self.rows[usize::from(self.header())..];
                body.len() > 1 && body.iter().all(|r| r.section == body[0].section)
            }
            TableOp::ToHtml => false,
            TableOp::ToPipe => self.to_pipe(src),
            TableOp::SetCell => self.cell(row, col).is_some(),
            TableOp::RowBelow
            | TableOp::ColumnLeft
            | TableOp::ColumnRight
            | TableOp::AlignNone
            | TableOp::AlignLeft
            | TableOp::AlignCenter
            | TableOp::AlignRight
            | TableOp::Transpose
            | TableOp::Realign => true,
            _ => false,
        }
    }
}

/// The table and cell around an editor position, with accepted operations.
pub fn info(src: &str, line: usize, column: usize) -> TableInfo {
    let Some(t) = find(src, line) else {
        return unfound_info();
    };
    let (row, col) = t.at(offset(src, line, column));
    let mut available = 0;
    for repr in 0..=TableOp::ToPipe.repr {
        let op = TableOp { repr };
        if op != TableOp::Realign && t.accepts(src, op, row, col) {
            available |= bit(op);
        }
    }
    TableInfo {
        found: true,
        first_line: t.first,
        last_line: t.last,
        rows: t.rows.len(),
        cols: t.cols,
        row,
        col,
        alignment: t.cell(usize::from(t.rows.len() > 1), col).map_or(0, |c| c.alignment(src)),
        aligned: true,
        html: true,
        available,
    }
}

/// The trimmed source of the cell covering a grid slot.
pub fn cell(src: &str, line: usize, row: usize, col: usize) -> TableCell {
    let none = || TableCell { found: false, line: 0, start: 0, end_line: 0, end: 0, text: String::new() };
    let Some(t) = find(src, line) else {
        return none();
    };
    let Some(c) = t.cell(row, col) else {
        return none();
    };
    let range = c.text_range(src);
    let (line, start) = position(src, range.start);
    let (end_line, end) = position(src, range.end);
    TableCell { found: true, line, start, end_line, end, text: src[range].into() }
}

/// Apply nonoverlapping replacements from right to left.
fn replace(src: &str, mut changes: Vec<(Range<usize>, String)>) -> String {
    changes.sort_by_key(|(r, _)| r.start);
    let mut out = src.to_string();
    for (range, value) in changes.into_iter().rev() {
        out.replace_range(range, &value);
    }
    out
}

/// Whole-line chunks include their trailing newline, when present.
fn chunk(src: &str, range: Range<usize>) -> (Range<usize>, bool) {
    let start = src[..range.start].rfind('\n').map_or(0, |n| n + 1);
    let end = src[range.end..].find('\n').map_or(src.len(), |n| range.end + n);
    if src[start..range.start].trim().is_empty() && src[range.end..end].trim().is_empty() {
        (start..(end + usize::from(end < src.len())), true)
    } else {
        (range, false)
    }
}

fn delete_chunk(src: &str, range: Range<usize>, previous: Option<usize>, next: Option<usize>) -> Range<usize> {
    let (mut range, lines) = chunk(src, range);
    if !lines {
        if let Some(p) = previous.filter(|&p| src[p..range.start].trim().is_empty()) {
            range.start = p;
        } else if let Some(n) = next.filter(|&n| src[range.end..n].trim().is_empty()) {
            range.end = n;
        }
    }
    range
}

fn align_name(code: u8) -> &'static str {
    match code {
        1 => "left",
        2 => "center",
        3 => "right",
        _ => "",
    }
}
fn empty_tag(c: &Cell, src: &str, name: &str) -> String {
    let align = c.attr(src, "align").map_or(String::new(), |a| format!(" align=\"{a}\""));
    format!("<{name}{align}>")
}
fn empty_cell(c: &Cell, src: &str) -> String {
    format!(
        "{}{}",
        empty_tag(c, src, &c.tag.name),
        if c.close.is_some() { format!("</{}>", c.tag.name) } else { String::new() }
    )
}
fn empty_row(row: &Row, src: &str, head: bool) -> String {
    let name = if head { "th" } else { "td" };
    let base = row.tag.range.start;
    let mut changes = vec![(0..row.tag.range.end - base, "<tr>".into())];
    for c in &row.cells {
        changes.push((c.tag.range.start - base..c.tag.range.end - base, empty_tag(c, src, name)));
        changes.push((c.content.start - base..c.content.end - base, String::new()));
        if let Some(close) = &c.close {
            changes.push((close.start - base..close.end - base, format!("</{name}>")));
        }
    }
    replace(&src[row.range()], changes)
}

/// Insert with the reference's indentation or the adjacent whitespace gap.
fn insert_chunk(src: &str, reference: Range<usize>, value: String, after: bool, gap: &str) -> (Range<usize>, String) {
    let (range, lines) = chunk(src, reference.clone());
    let at = if after { range.end } else { range.start };
    if lines {
        let indent = &src[range.start..reference.start];
        (at..at, format!("{indent}{value}\n"))
    } else if after {
        (at..at, format!("{gap}{value}"))
    } else {
        (at..at, format!("{value}{gap}"))
    }
}

fn set_align(src: &str, c: &Cell, code: u8) -> String {
    let mut changes = Vec::new();
    let base = c.tag.range.start;
    let mut found = false;
    for a in &c.tag.attrs {
        if a.name == "align" {
            found = true;
            if code == 0 {
                let mut start = a.range.start;
                while start > base && src.as_bytes()[start - 1].is_ascii_whitespace() {
                    start -= 1;
                }
                changes.push((start - base..a.range.end - base, String::new()));
            } else {
                // Replace the attribute, retaining its original quoting when possible.
                let quoted = a.value.start > 0 && matches!(src.as_bytes()[a.value.start - 1], b'\'' | b'"');
                let value = if quoted { align_name(code).into() } else { format!("\"{}\"", align_name(code)) };
                if a.value.is_empty() && !src[a.range.clone()].contains('=') {
                    changes.push((a.range.start - base..a.range.end - base, format!("align=\"{}\"", align_name(code))));
                } else {
                    changes.push((a.value.start - base..a.value.end - base, value));
                }
            }
        } else if a.name == "style" {
            let style = &src[a.value.clone()];
            let mut kept = String::new();
            for declaration in style.split_inclusive(';') {
                let key = declaration.split_once(':').map(|(k, _)| k.trim());
                if !key.is_some_and(|k| k.eq_ignore_ascii_case("text-align")) {
                    kept.push_str(declaration);
                }
            }
            if kept != style {
                if kept.trim().is_empty() {
                    let mut start = a.range.start;
                    while start > base && src.as_bytes()[start - 1].is_ascii_whitespace() {
                        start -= 1;
                    }
                    changes.push((start - base..a.range.end - base, String::new()));
                } else {
                    changes.push((a.value.start - base..a.value.end - base, kept));
                }
            }
        }
    }
    if !found && code != 0 {
        let mut at = c.tag.range.end - base - 1;
        if src.as_bytes()[base + at - 1] == b'/' {
            at -= 1;
        }
        changes.push((at..at, format!(" align=\"{}\"", align_name(code))));
    }
    replace(&src[c.tag.range.clone()], changes)
}

fn plain(src: &str) -> String {
    let tokens = tags(src);
    let mut out = String::new();
    let mut at = 0;
    for t in tokens {
        out.push_str(&src[at..t.range.start]);
        at = t.range.end;
    }
    out.push_str(&src[at..]);
    while let Some(start) = out.find("<!--") {
        let end = out[start + 4..].find("-->").map_or(out.len(), |n| start + 4 + n + 3);
        out.replace_range(start..end, "");
    }
    out
}

/// Edit only the necessary source ranges, returning the containing whole lines.
#[cfg(test)]
pub fn edit(src: &str, line: usize, op: TableOp, row: usize, col: usize, arg: &str) -> TableEdit {
    edit_rect(src, line, op, row, col, 1, 1, arg)
}

/// `edit`, with `SetCell` setting all the cells of the `span_rows` x
/// `span_cols` rectangle that starts at (`row`, `col`).
#[allow(clippy::too_many_arguments)]
pub(super) fn edit_rect(
    src: &str,
    line: usize,
    op: TableOp,
    row: usize,
    col: usize,
    span_rows: usize,
    span_cols: usize,
    arg: &str,
) -> TableEdit {
    let Some(t) = find(src, line) else {
        return refused();
    };
    if !t.accepts(src, op, row, col) {
        return refused();
    }
    if op == TableOp::DeleteTable {
        return TableEdit {
            ok: true,
            changed: true,
            remove: true,
            first_line: t.first,
            last_line: t.last,
            replacement: String::new(),
            caret_line: t.first,
            caret_column: 0,
            keep_before: 0,
            keep_after: 0,
        };
    }
    let (mut target_row, mut target_col) = (row, col);
    let mut changes = Vec::new();
    match op {
        TableOp::SetCell => {
            let value = arg.trim().replace("\r\n", "\n").replace(['\r', '\n'], "<br>");
            // (A merged cell is in several places of the grid: set it once.)
            let mut done = std::collections::HashSet::new();
            for r in row..(row + span_rows).min(t.rows.len()) {
                for c in col..(col + span_cols).min(t.cols) {
                    if let Some(cell) = t.cell(r, c) {
                        let range = cell.text_range(src);
                        if done.insert(range.start) {
                            changes.push((range, value.clone()));
                        }
                    }
                }
            }
        }
        TableOp::RowAbove | TableOp::RowBelow => {
            let after = op == TableOp::RowBelow;
            target_row = row + usize::from(after);
            target_col = 0;
            let next_section = after && row + 1 < t.rows.len() && t.rows[row].section != t.rows[row + 1].section;
            let reference = if next_section { &t.rows[row + 1] } else { &t.rows[row] };
            let value = empty_row(&t.rows[row], src, reference.head);
            let gap = if row + 1 < t.rows.len() { &src[t.rows[row].end..t.rows[row + 1].tag.range.start] } else { "" };
            let gap = if gap.trim().is_empty() { gap } else { "" };
            changes.push(insert_chunk(src, reference.range(), value, after && !next_section, gap));
        }
        TableOp::DeleteRow => {
            let r = &t.rows[row];
            changes.push((
                delete_chunk(
                    src,
                    r.range(),
                    row.checked_sub(1).map(|n| t.rows[n].end),
                    t.rows.get(row + 1).map(|r| r.tag.range.start),
                ),
                String::new(),
            ));
            target_row = row.min(t.rows.len() - 2);
        }
        TableOp::RowUp | TableOp::RowDown => {
            target_row = if op == TableOp::RowUp { row - 1 } else { row + 1 };
            let (a, b) = (&t.rows[row], &t.rows[target_row]);
            changes.push((a.range(), src[b.range()].into()));
            changes.push((b.range(), src[a.range()].into()));
        }
        TableOp::ColumnLeft
        | TableOp::ColumnRight
        | TableOp::DeleteColumn
        | TableOp::ColumnBefore
        | TableOp::ColumnAfter => {
            target_col = match op {
                TableOp::ColumnRight | TableOp::ColumnAfter => col + 1,
                TableOp::ColumnBefore => col - 1,
                TableOp::DeleteColumn => col.min(t.cols - 2),
                _ => col,
            };
            for r in &t.rows {
                let index = r.cells.iter().position(|c| c.col == col);
                if op == TableOp::ColumnLeft || op == TableOp::ColumnRight {
                    if let Some(c) = index.and_then(|i| r.cells.get(i)).or_else(|| r.cells.last()) {
                        let i = index.unwrap_or(r.cells.len() - 1);
                        let gap = if i + 1 < r.cells.len() {
                            &src[c.end..r.cells[i + 1].tag.range.start]
                        } else if i > 0 {
                            &src[r.cells[i - 1].end..c.tag.range.start]
                        } else {
                            ""
                        };
                        let gap = if gap.trim().is_empty() { gap } else { "" };
                        changes.push(insert_chunk(
                            src,
                            c.range(),
                            empty_cell(c, src),
                            index.is_none() || op == TableOp::ColumnRight,
                            gap,
                        ));
                    } else {
                        let name = if r.head { "th" } else { "td" };
                        changes.push((r.tag.range.end..r.tag.range.end, format!("<{name}></{name}>")));
                    }
                } else if let Some(i) = index {
                    let c = &r.cells[i];
                    if op == TableOp::DeleteColumn {
                        changes.push((
                            delete_chunk(
                                src,
                                c.range(),
                                i.checked_sub(1).map(|n| r.cells[n].end),
                                r.cells.get(i + 1).map(|c| c.tag.range.start),
                            ),
                            String::new(),
                        ));
                    } else if let Some(other) = r.cells.iter().find(|c| c.col == target_col) {
                        changes.push((c.range(), src[other.range()].into()));
                        changes.push((other.range(), src[c.range()].into()));
                    }
                }
            }
        }
        TableOp::AlignNone | TableOp::AlignLeft | TableOp::AlignCenter | TableOp::AlignRight => {
            let code = match op {
                TableOp::AlignLeft => 1,
                TableOp::AlignCenter => 2,
                TableOp::AlignRight => 3,
                _ => 0,
            };
            for r in &t.rows {
                if let Some(c) = r.cells.iter().find(|c| c.col == col) {
                    changes.push((c.tag.range.clone(), set_align(src, c, code)));
                }
            }
        }
        TableOp::SortAscending | TableOp::SortDescending => {
            let start = usize::from(t.header());
            let mut order: Vec<usize> = (start..t.rows.len()).collect();
            let keys: Vec<String> = (0..t.rows.len())
                .map(|r| t.cell(r, col).map_or(String::new(), |c| plain(&src[c.text_range(src)])))
                .collect();
            order.sort_by(|&a, &b| {
                let cmp = match (crate::table::number(&keys[a]), crate::table::number(&keys[b])) {
                    (Some(x), Some(y)) => x.partial_cmp(&y).unwrap_or(std::cmp::Ordering::Equal),
                    _ => keys[a].to_lowercase().cmp(&keys[b].to_lowercase()),
                };
                if op == TableOp::SortDescending { cmp.reverse() } else { cmp }
            });
            for (slot, &r) in t.rows[start..].iter().zip(&order) {
                changes.push((slot.range(), src[t.rows[r].range()].into()));
            }
        }
        TableOp::Transpose => {
            let line_start = src[..t.open.range.start].rfind('\n').map_or(0, |n| n + 1);
            let indent = &src[line_start..t.open.range.start];
            let mut out = src[t.open.range.clone()].to_string();
            for c in 0..t.cols {
                out.push_str(&format!("\n{indent}  <tr>"));
                for r in 0..t.rows.len() {
                    if let Some(cell) = t.cell(r, c) {
                        out.push_str(&format!(
                            "\n{indent}    {}{}</{}>",
                            &src[cell.tag.range.clone()],
                            &src[cell.content.clone()],
                            cell.tag.name
                        ));
                    } else {
                        out.push_str(&format!("\n{indent}    <td></td>"));
                    }
                }
                out.push_str(&format!("\n{indent}  </tr>"));
            }
            out.push_str(&format!("\n{indent}{}", &src[t.close.range.clone()]));
            changes.push((t.open.range.start..t.close.range.end, out));
            target_row = 0;
            target_col = 0;
        }
        TableOp::ToPipe => {
            let cells: Vec<Vec<String>> = (0..t.rows.len())
                .map(|r| {
                    (0..t.cols)
                        .map(|c| {
                            t.cell(r, c).map_or(String::new(), |cell| {
                                src[cell.content.clone()]
                                    .split_whitespace()
                                    .collect::<Vec<_>>()
                                    .join(" ")
                                    .replace('|', "\\|")
                            })
                        })
                        .collect()
                })
                .collect();
            let aligns: Vec<_> = (0..t.cols)
                .map(|c| match t.cell(usize::from(t.rows.len() > 1), c).map_or(0, |c| c.alignment(src)) {
                    1 => (true, false),
                    2 => (true, true),
                    3 => (false, true),
                    _ => (false, false),
                })
                .collect();
            let out = crate::format::aligned_rows(cells.iter().map(|r| ("", r.as_slice())), cells.len(), "", &aligns)
                .join("\n");
            let start = src[..t.open.range.start].rfind('\n').map_or(0, |n| n + 1);
            changes.push((start..t.close.range.end, out));
            target_row = 0;
            target_col = 0;
        }
        TableOp::Realign => {}
        _ => return refused(),
    }
    let out = replace(src, changes);
    // The unchanged window suffix determines the new last table line even
    // when a conversion or a cell edit changes its number of lines.
    let old_lines: Vec<&str> = src.split('\n').collect();
    let new_lines: Vec<&str> = out.split('\n').collect();
    let new_end = new_lines.len() - (old_lines.len() - t.last - 1);
    let replacement = new_lines[t.first..new_end].join("\n");
    let output: Vec<String> = new_lines[t.first..new_end].iter().map(|s| (*s).into()).collect();
    let (keep_before, keep_after) = unchanged_ends(&old_lines[t.first..=t.last], &output);
    let c = crate::table::cell(&out, t.first, target_row, target_col);
    TableEdit {
        ok: true,
        changed: out != src,
        remove: false,
        first_line: t.first,
        last_line: t.last,
        replacement,
        caret_line: if c.found { c.line } else { t.first },
        caret_column: if c.found { c.start } else { 0 },
        keep_before,
        keep_after,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const INLINE: &str =
        "<table><tr><th>a</th><th>b</th></tr><tr><td>10</td><td>x</td></tr><tr><td>2</td><td>y</td></tr></table>";
    const LINES: &str = "<table id='t'>\n  <thead>\n    <tr class='h'>\n      <th>a</th>\n      <th>b</th>\n    </tr>\n  </thead>\n  <tbody>\n    <tr data-x='1'>\n      <td>10</td>\n      <td>x</td>\n    </tr>\n    <tr>\n      <td>2</td>\n      <td>y</td>\n    </tr>\n  </tbody>\n</table>";
    const OMITTED: &str = "<table><thead><tr><th>a<th>b</thead><tbody><tr><td>10<td>x<tr><td>2<td>y</tbody></table>";
    fn run(src: &str, op: TableOp, row: usize, col: usize) -> TableEdit {
        edit(src, 0, op, row, col, "")
    }
    fn value(src: &str, row: usize, col: usize) -> String {
        cell(src, 0, row, col).text
    }

    #[test]
    fn ranges_of_html_rows_and_cells() {
        use crate::table::edit_range;
        let apply = |src: &str, e: &TableEdit| {
            let lines: Vec<&str> = src.split('\n').collect();
            let mut out: Vec<&str> = lines[..e.first_line].to_vec();
            out.extend(e.replacement.split('\n'));
            out.extend(&lines[e.last_line + 1..]);
            out.join("\n")
        };
        let e = edit_range(LINES, 0, TableOp::SetCell, 0, 0, 2, 2, "");
        let out = apply(LINES, &e);
        assert!(out.contains("<th></th>") && out.contains("<td></td>") && !out.contains("10") && !out.contains(">x<"));
        assert!(out.contains(">y<"));
        assert!(out.contains("<tr data-x='1'>"));
        assert_eq!(out.matches("<td></td>").count(), 2);
        // Both body rows: one empty row stays, as a body row.
        let e = edit_range(LINES, 0, TableOp::DeleteRow, 1, 0, 5, 1, "");
        let out = apply(LINES, &e);
        assert!(e.ok && out.matches("<tr").count() == 2 && out.contains("<td></td>") && !out.contains("10"));
        // The header row is not deleted.
        let e = edit_range(LINES, 0, TableOp::DeleteRow, 0, 0, 2, 1, "");
        let out = apply(LINES, &e);
        assert!(out.contains("<th>a</th>") && !out.contains("10") && out.contains(">2<"));
        let e = edit_range(LINES, 0, TableOp::RowBelow, 1, 0, 2, 1, "");
        assert_eq!(apply(LINES, &e).matches("<tr").count(), 5);
        let e = edit_range(INLINE, 0, TableOp::ColumnRight, 0, 0, 1, 2, "");
        assert_eq!(apply(INLINE, &e).matches("<td></td>").count(), 4);
        // Merged cells are set once.
        let merged = "<table><tr><th colspan=\"2\">h</th></tr><tr><td>a</td><td>b</td></tr></table>";
        let e = edit_range(merged, 0, TableOp::SetCell, 0, 0, 1, 2, "");
        assert_eq!(apply(merged, &e), "<table><tr><th colspan=\"2\"></th></tr><tr><td>a</td><td>b</td></tr></table>");
        assert!(!edit_range(merged, 0, TableOp::RowBelow, 1, 0, 2, 1, "").ok);
    }

    #[test]
    fn finding_requires_a_complete_block_outside_fences() {
        for src in [
            "```\n<table><tr><td>x</table>\n```",
            "~~~\n<table></table>\n~~~",
            "x <table></table>",
            "    <table></table>",
            "<table><tr><td>x",
            "<table><table></table></table>",
        ] {
            assert!(!info(src, usize::from(src.starts_with('`') || src.starts_with('~')), 0).found, "{src}");
        }
        let src = format!("before\n{LINES}\nafter");
        let t = info(&src, 10, 11);
        assert!(t.found && t.html);
        assert_eq!((t.first_line, t.last_line, t.rows, t.cols), (1, 18, 3, 2));
        let result = edit(&src, 10, TableOp::SetCell, 1, 0, "new");
        assert_eq!(result.replacement, LINES.replace(">10<", ">new<"));
        assert_eq!((result.first_line, result.last_line), (1, 18));
        assert!(!info(&src, 0, 0).found);
        assert!(!info(&src, 19, 0).found);
        assert!(info("   <TABLE a='>'><TR><TD>x</TABLE>", 0, 0).found);
    }

    #[test]
    fn positions_ranges_and_header_rules() {
        let t = info(LINES, 10, 11);
        assert_eq!((t.row, t.col), (1, 1));
        assert_eq!(info(LINES, 8, 5).row, 1);
        assert_eq!(info(LINES, 8, 5).col, 0);
        let header = info(LINES, 3, 10);
        assert_eq!(header.available & bit(TableOp::DeleteRow), 0);
        assert_eq!(header.available & bit(TableOp::Realign), 0);
        assert_eq!(header.available & bit(TableOp::ToHtml), 0);
        let ordinary = "<table><tr><td>😀\n  z</td></tr><tr><td>q</td></tr></table>";
        let c = cell(ordinary, 0, 0, 0);
        assert_eq!((c.line, c.start, c.end_line, c.end), (0, 15, 1, 3));
        assert_eq!(c.text, "😀\n  z");
        assert!(info(ordinary, 0, 15).available & bit(TableOp::DeleteRow) != 0);
        assert!(run(ordinary, TableOp::RowAbove, 0, 0).ok);
        assert!(run(ordinary, TableOp::RowDown, 0, 0).ok);
        let c = cell("<table><tr><td> \n </td></tr></table>", 0, 0, 0);
        assert_eq!((c.line, c.start, c.end_line, c.end), (0, 15, 0, 15));
    }

    #[test]
    fn operations_work_in_all_source_layouts() {
        for src in [LINES, INLINE, OMITTED] {
            for (op, row, col) in [
                (TableOp::RowAbove, 1, 0),
                (TableOp::RowBelow, 0, 0),
                (TableOp::ColumnLeft, 1, 0),
                (TableOp::ColumnRight, 1, 0),
                (TableOp::DeleteRow, 1, 0),
                (TableOp::DeleteColumn, 1, 0),
                (TableOp::DeleteTable, 0, 0),
                (TableOp::RowUp, 2, 0),
                (TableOp::RowDown, 1, 0),
                (TableOp::ColumnBefore, 1, 1),
                (TableOp::ColumnAfter, 1, 0),
                (TableOp::AlignNone, 1, 0),
                (TableOp::AlignLeft, 1, 0),
                (TableOp::AlignCenter, 1, 0),
                (TableOp::AlignRight, 1, 0),
                (TableOp::SortAscending, 1, 0),
                (TableOp::SortDescending, 1, 0),
                (TableOp::Transpose, 1, 0),
                (TableOp::Realign, 1, 0),
                (TableOp::SetCell, 1, 0),
                (TableOp::ToPipe, 1, 0),
            ] {
                let r = edit(src, 0, op, row, col, "new\ntext");
                assert!(r.ok, "{op:?}: {src}");
                if r.remove {
                    assert!(r.replacement.is_empty());
                    continue;
                }
                let t = crate::table::info(&r.replacement, 0, 0);
                assert!(t.found, "{op:?}: {}", r.replacement);
                match op {
                    TableOp::RowAbove | TableOp::RowBelow => {
                        assert_eq!(t.rows, 4);
                        assert_eq!(value(&r.replacement, 1, 0), "");
                    }
                    TableOp::ColumnLeft | TableOp::ColumnRight => assert_eq!(t.cols, 3),
                    TableOp::DeleteRow => {
                        assert_eq!(t.rows, 2);
                        assert_eq!(value(&r.replacement, 1, 0), "2");
                    }
                    TableOp::DeleteColumn => assert_eq!(t.cols, 1),
                    TableOp::RowUp | TableOp::RowDown | TableOp::SortAscending => {
                        assert_eq!(value(&r.replacement, 1, 0), "2")
                    }
                    TableOp::ColumnBefore | TableOp::ColumnAfter => assert_eq!(value(&r.replacement, 1, 0), "x"),
                    TableOp::SortDescending => assert_eq!(value(&r.replacement, 1, 0), "10"),
                    TableOp::Transpose => assert_eq!((t.rows, t.cols), (2, 3)),
                    TableOp::Realign => assert!(!r.changed),
                    TableOp::SetCell => assert_eq!(value(&r.replacement, 1, 0), "new<br>text"),
                    _ => {}
                }
                let caret = crate::table::cell(
                    &r.replacement,
                    0,
                    match op {
                        TableOp::Transpose | TableOp::ToPipe => 0,
                        TableOp::RowUp => 1,
                        TableOp::RowDown => 2,
                        TableOp::RowBelow => 1,
                        _ => row.min(t.rows - 1),
                    },
                    match op {
                        TableOp::Transpose | TableOp::ToPipe | TableOp::RowAbove | TableOp::RowBelow => 0,
                        TableOp::ColumnRight | TableOp::ColumnAfter => 1,
                        TableOp::ColumnBefore => 0,
                        _ => col.min(t.cols - 1),
                    },
                );
                assert_eq!((r.caret_line, r.caret_column), (caret.line, caret.start), "{op:?}");
            }
            assert!(!run(src, TableOp::ToHtml, 0, 0).ok);
        }
    }

    #[test]
    fn attributes_comments_and_alignment_survive() {
        let src = "<table ID='T'><!--keep--><tr class='r'><th>a</th><th>b</th></tr><tr><td data-x='>' align='left' style='color: red; text-align: center; font-size: 2px;'>10</td><!--gap--><td>x</td></tr></table>";
        let r = run(src, TableOp::AlignRight, 1, 0);
        assert_eq!(
            r.replacement,
            src.replace("<th>a", "<th align=\"right\">a")
                .replace("align='left'", "align='right'")
                .replace(" text-align: center;", "")
        );
        let r = run(&r.replacement, TableOp::AlignNone, 1, 0);
        assert!(!r.replacement.contains("align="));
        assert!(r.replacement.contains("style='color: red; font-size: 2px;'"));
        assert!(r.replacement.contains("<!--gap-->"));
        let src = "<table><tr><td align=left style='text-align:right;'>a</td></tr></table>";
        assert_eq!(run(src, TableOp::AlignNone, 0, 0).replacement, "<table><tr><td>a</td></tr></table>");
        assert!(run(src, TableOp::AlignCenter, 0, 0).replacement.contains("align=\"center\""));
        let r = edit(src, 0, TableOp::SetCell, 0, 0, " x\r\ny\rz\nw ");
        assert_eq!(r.replacement, src.replace(">a<", ">x<br>y<br>z<br>w<"));
    }

    #[test]
    fn merged_grid_only_allows_text_and_table_deletion() {
        let src = "<table><tr><th rowspan='2'>a</th><th colspan=2>b</th></tr><tr><td>c</td><td>d</td></tr></table>";
        let t = info(src, 0, src.find(">c<").unwrap() + 1);
        assert_eq!((t.rows, t.cols, t.row, t.col), (2, 3, 1, 1));
        assert_eq!(t.available, bit(TableOp::SetCell) | bit(TableOp::DeleteTable));
        assert_eq!(value(src, 1, 0), "a");
        assert_eq!(value(src, 0, 2), "b");
        for repr in 0..=TableOp::ToPipe.repr {
            let op = TableOp { repr };
            assert_eq!(run(src, op, 1, 0).ok, op == TableOp::SetCell || op == TableOp::DeleteTable, "{op:?}");
        }
        assert!(edit(src, 0, TableOp::SetCell, 1, 0, "new").replacement.contains(">new</th>"));
    }

    #[test]
    fn conversion_and_context_restrictions() {
        let src = "<table><tr><th>a</th><th>b</th></tr><tr><td align='center'> x  | y </td><td>z</td></tr></table>";
        assert_eq!(
            run(src, TableOp::ToPipe, 0, 0).replacement,
            "|   a    | b   |\n| :----: | --- |\n| x \\| y | z   |"
        );
        for surrounding in [format!("before\n{src}"), format!("{src}\nafter"), format!("{src} suffix")] {
            let line = usize::from(surrounding.starts_with("before"));
            assert!(!edit(&surrounding, line, TableOp::ToPipe, 0, 0, "").ok);
        }
        assert!(!run(&src.replace(" x  | y ", "<pre>x</pre>"), TableOp::ToPipe, 0, 0).ok);
        assert!(!run(&format!("{src} suffix"), TableOp::DeleteTable, 0, 0).ok);
        assert!(run(&format!("  {src}  "), TableOp::DeleteTable, 0, 0).ok);
    }

    #[test]
    fn ragged_rows_and_inline_gaps_are_preserved() {
        let src = "<table><tr><th>a</th>  <th>b</th></tr>\n<tr id='x'><td>1</td></tr>\n<tr><td>2</td>  <td>3</td></tr></table>";
        let r = run(src, TableOp::ColumnRight, 1, 1);
        assert!(r.replacement.contains("<tr id='x'><td>1</td><td></td></tr>"));
        assert_eq!(info(&r.replacement, 0, 0).cols, 3);
        let r = run(src, TableOp::DeleteColumn, 1, 0);
        assert_eq!(r.replacement, "<table><tr><th>b</th></tr>\n<tr id='x'></tr>\n<tr><td>3</td></tr></table>");
        let r = run(src, TableOp::ColumnAfter, 0, 0);
        assert_eq!(
            r.replacement,
            "<table><tr><th>b</th>  <th>a</th></tr>\n<tr id='x'><td>1</td></tr>\n<tr><td>3</td>  <td>2</td></tr></table>"
        );
        let r = edit(&format!("{src} trailing"), 0, TableOp::SetCell, 1, 0, "9");
        assert_eq!(r.replacement, format!("{} trailing", src.replace(">1<", ">9<")));
        let src = "<table><tr><td align style='text-align: right;'>a</td></tr></table>";
        assert_eq!(run(src, TableOp::AlignNone, 0, 0).replacement, "<table><tr><td>a</td></tr></table>");
        assert!(!run("  <table><tr><td>a</td></tr></table>", TableOp::ToPipe, 0, 0).ok);
    }

    #[test]
    fn multiline_omissions_and_availability_match_edits() {
        let omitted =
            "<table>\n<thead>\n<tr><th>a<th>b\n</thead>\n<tbody>\n<tr><td>10<td>x\n<tr><td>2<td>y\n</tbody>\n</table>";
        for src in [LINES, INLINE, OMITTED, omitted] {
            let t = find(src, 0).unwrap();
            for (row, r) in t.rows.iter().enumerate() {
                let (line, column) = position(src, r.tag.range.start);
                assert_eq!(info(src, line, column).row, row);
                for col in 0..t.cols {
                    let c = cell(src, 0, row, col);
                    let i = info(src, c.line, c.start);
                    for repr in 0..=TableOp::ToPipe.repr {
                        let op = TableOp { repr };
                        let result = run(src, op, row, col);
                        assert_eq!(
                            i.available & bit(op) != 0,
                            result.ok && op != TableOp::Realign,
                            "{op:?} {row} {col}: {src}"
                        );
                        if result.ok && !result.remove {
                            assert!(
                                crate::table::info(&result.replacement, 0, 0).found,
                                "{op:?}: {}",
                                result.replacement
                            );
                        }
                    }
                }
            }
        }
        assert!(!info("<table>\n<table>\n</table>\n</table>", 3, 0).found);
        assert!(!info("<table>\n<table>\n</table>", 1, 0).found);
        assert!(run("<table></table>", TableOp::DeleteTable, 0, 0).ok);
    }

    #[test]
    fn sections_limit_moves_and_sorting() {
        let src = "<table><thead><tr><td>a</td></tr></thead><tbody><tr><td>b</td></tr></tbody><tfoot><tr><td>c</td></tr></tfoot></table>";
        assert!(!run(src, TableOp::RowUp, 1, 0).ok);
        assert!(!run(src, TableOp::RowDown, 1, 0).ok);
        assert!(!run(src, TableOp::SortAscending, 1, 0).ok);
        let r = run(src, TableOp::RowBelow, 0, 0);
        assert!(r.replacement.contains("<tbody><tr><td></td></tr><tr><td>b"));
        let src = "<table><tr><td>2</td></tr><tr><td>1</td></tr></table>";
        assert_eq!(value(&run(src, TableOp::SortAscending, 0, 0).replacement, 0, 0), "1");
        assert!(!run("<table><tr><td>a</td></tr></table>", TableOp::DeleteRow, 0, 0).ok);
    }
}
