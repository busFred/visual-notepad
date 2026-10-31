//! Keeps raw HTML in a block from nesting deep enough to stall Qt.
//!
//! Qt's rich text engine slows down steeply with nesting: laying out tables
//! nested 20 deep takes seconds, and parsing elements nested a few thousand
//! deep (any element) takes seconds too. Raw HTML in a document is passed to
//! Qt as written, so `limit_nesting` escapes the `<` of every opening tag that
//! would nest too deep, which makes Qt show that tag as text.
//!
//! What "too deep" means has to be decided the way Qt's HTML parser
//! (QTextHtmlParser) builds its element tree, not the way a browser would: it
//! splits tags its own way, closes elements implicitly, and creates tables by
//! itself for `<td>` and `<tr>` outside a table (so `<td></tr>` repeated nests
//! tables without any `<table>` tag). `Tree` follows those rules. Where it
//! cannot be sure what Qt does it stays on the safe side: it keeps an upper
//! bound of what Qt has open, so it may escape too much in odd HTML but never
//! too little.

use std::collections::HashMap;

/// Tables nested deeper than this are escaped. Layout tables in READMEs nest
/// two or three levels; Qt takes about 0.1 s at 14 levels and 6 s at 20.
pub(super) const MAX_TABLE_DEPTH: usize = 4;

/// Elements nested deeper than this are escaped. Documents written by hand
/// stay far below; Qt's parser takes about 0.1 s at 5000 levels and grows
/// quadratically.
pub(super) const MAX_ELEMENT_DEPTH: usize = 256;

/// `html` with the `<` of opening tags that would nest too deep escaped, or
/// `None` if it can be used as it is. Linear time.
pub(super) fn limit_nesting(html: &str) -> Option<String> {
    let b = html.as_bytes();
    let n = b.len();
    let mut tree = Tree::default();
    let mut escapes: Vec<usize> = Vec::new();
    let mut i = 0;
    // Qt starts a tag at every `<`, which may be followed by spaces.
    while let Some(off) = b[i..].iter().position(|&c| c == b'<') {
        let lt = i + off;
        let j = skip_space(html, lt + 1);
        i = match b.get(j) {
            Some(b'!') => {
                if b[j + 1..].starts_with(b"--") {
                    // A comment runs to the next "-->".
                    find(b, j + 3, b"-->").map_or(n, |e| e + 3)
                } else {
                    find(b, j + 1, b">").map_or(n, |e| e + 1)
                }
            }
            Some(b'/') => {
                // The name follows the slash directly.
                let (word, after) = qt_word(html, j + 1);
                tree.close(word);
                find(b, after, b">").map_or(n, |e| e + 1)
            }
            _ => {
                let (word, mut after) = qt_word(html, j);
                if !tree.open(word) {
                    // Qt reads the rest of the tag as text.
                    escapes.push(lt);
                    lt + 1
                } else {
                    // Attributes are only read after a space.
                    if html[after..].starts_with(char::is_whitespace) {
                        after = qt_attributes(html, after);
                    }
                    let close = find(b, after, b">");
                    // Any `/` between the attributes and the `>` closes the
                    // element at once.
                    if b[after..close.unwrap_or(n)].contains(&b'/') {
                        tree.close_opened();
                    }
                    close.map_or(n, |e| e + 1)
                }
            }
        };
    }
    if escapes.is_empty() {
        return None;
    }
    let mut out = String::with_capacity(n + escapes.len() * 3);
    let mut copied = 0;
    for lt in escapes {
        out.push_str(&html[copied..lt]);
        out.push_str("&lt;");
        copied = lt + 1;
    }
    out.push_str(&html[copied..]);
    Some(out)
}

/// A tag name or attribute word as Qt reads it.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Word<'a> {
    Plain(&'a str),
    /// In quotes, without them.
    Quoted(&'a str),
}

fn skip_space(text: &str, from: usize) -> usize {
    from + text[from..]
        .find(|c: char| !c.is_whitespace())
        .unwrap_or(text.len() - from)
}

fn find(haystack: &[u8], from: usize, needle: &[u8]) -> Option<usize> {
    haystack
        .get(from..)?
        .windows(needle.len())
        .position(|w| w == needle)
        .map(|p| from + p)
}

/// The word at `from` and the position after it, as QTextHtmlParser's
/// parseWord reads it: quoted up to the matching quote (a single quote after
/// a backslash does not count), otherwise up to a space, `<`, `>`, `=` or
/// `/>`. Character references cannot hide any of these characters, so they
/// need no decoding to find the end.
fn qt_word(text: &str, from: usize) -> (Word<'_>, usize) {
    let b = text.as_bytes();
    match b.get(from) {
        Some(&q @ (b'"' | b'\'')) => {
            let close =
                (from + 1..b.len()).find(|&k| b[k] == q && (q == b'"' || b[k - 1] != b'\\'));
            match close {
                Some(k) => (Word::Quoted(&text[from + 1..k]), k + 1),
                None => (Word::Quoted(&text[from + 1..]), text.len()),
            }
        }
        _ => {
            let rest = &text[from..];
            let mut end = rest.len();
            for (k, c) in rest.char_indices() {
                if c == '>'
                    || c == '<'
                    || c == '='
                    || c.is_whitespace()
                    || (c == '/' && rest[k + 1..].starts_with('>'))
                {
                    end = k;
                    break;
                }
            }
            (Word::Plain(&rest[..end]), from + end)
        }
    }
}

/// Where QTextHtmlParser's parseAttributes stops, starting after a tag name
/// that a space follows.
fn qt_attributes(text: &str, mut pos: usize) -> usize {
    let b = text.as_bytes();
    loop {
        pos = skip_space(text, pos);
        if matches!(b.get(pos), None | Some(b'>' | b'/')) {
            return pos;
        }
        let (key, after) = qt_word(text, pos);
        pos = after;
        if matches!(key, Word::Plain("") | Word::Quoted("")) {
            return pos;
        }
        pos = skip_space(text, pos);
        if b.get(pos) == Some(&b'=') {
            pos = skip_space(text, pos + 1);
            pos = qt_word(text, pos).1;
        }
    }
}

/// An element name as Qt would see it.
enum Name {
    /// Lowercase.
    Known(String),
    /// No name; Qt's node for it never has children.
    Unnamed,
    /// Quoted, with character references or non-ASCII: could be anything.
    Odd,
}

fn name(word: Word) -> Name {
    match word {
        Word::Plain("") => Name::Unnamed,
        Word::Plain(w) if w.is_ascii() && !w.contains('&') => Name::Known(w.to_ascii_lowercase()),
        _ => Name::Odd,
    }
}

/// Elements that stop the walk of a new `<p>` up the tree (Qt's
/// DisplayBlock), as tested against Qt.
fn is_block(name: &str) -> bool {
    matches!(
        name,
        "p" | "div"
            | "li"
            | "ul"
            | "ol"
            | "blockquote"
            | "pre"
            | "center"
            | "dl"
            | "td"
            | "th"
            | "h1"
            | "h2"
            | "h3"
            | "h4"
            | "h5"
            | "h6"
    )
}

/// Elements that may or may not be blocks to Qt.
fn is_maybe_block(name: &str) -> bool {
    matches!(
        name,
        "dt" | "dd"
            | "thead"
            | "tbody"
            | "tfoot"
            | "caption"
            | "style"
            | "title"
            | "script"
            | "link"
    )
}

/// The open elements of Qt's tree, as far as they are known. `names` is a
/// subsequence of the elements Qt has open: in the same order, but up to
/// `missing` of Qt's may be absent (of which up to `missing_tables` tables)
/// after a step whose effect on Qt was not certain. While nothing is missing
/// the two are the same.
#[derive(Default)]
struct Tree {
    names: Vec<String>,
    /// Tables in `names[..=i]`.
    tables: Vec<usize>,
    /// Positions in `names` by name, innermost last.
    positions: HashMap<String, Vec<usize>>,
    /// For tables in `names`: whether a cell was opened in it, directly or
    /// in a table in it.
    had_cells: Vec<bool>,
    /// Positions of blocks and of maybe-blocks.
    blocks: Vec<usize>,
    maybe_blocks: Vec<usize>,
    missing: usize,
    missing_tables: usize,
    /// Whether the last open tag added its own element.
    opened: bool,
}

/// What an open tag does: keep `names[..keep]`, then add `push`.
struct Step<'a> {
    keep: usize,
    push: Vec<&'a str>,
    /// Qt may close fewer elements than `keep` says.
    unsure: bool,
    /// Elements, and tables among them, that Qt may open beyond `push`.
    extra: (usize, usize),
}

impl Tree {
    fn tables_in(&self, keep: usize) -> usize {
        keep.checked_sub(1).map_or(0, |k| self.tables[k])
    }

    fn nearest(&self, name: &str) -> Option<usize> {
        self.positions.get(name).and_then(|v| v.last().copied())
    }

    /// The innermost of the elements named `names` in `self.names[..keep]`.
    fn innermost(&self, names: &[&'static str], keep: usize) -> Option<(usize, &'static str)> {
        names
            .iter()
            .filter_map(|&n| {
                let at = self.positions.get(n)?.iter().rev().find(|&&at| at < keep)?;
                Some((*at, n))
            })
            .max()
    }

    /// Whether a table can go after `names[..keep]`: outside any table, in a
    /// cell, or in a table that has had no cell yet. Qt puts a table that is
    /// elsewhere in a table (such as right in a row) in the last cell before
    /// it, even one of a table closed since, so that tables placed like that
    /// one after another nest; HTML written for a browser has no reason to
    /// put a table there.
    fn can_hold_table(&self, keep: usize) -> bool {
        if self.missing > 0 {
            // A missing element may be a table or a row.
            return self.missing_tables == 0 && self.tables_in(keep) == 0;
        }
        const STRUCTURE: &[&str] = &[
            "table", "tr", "td", "th", "thead", "tbody", "tfoot", "caption",
        ];
        match self.innermost(STRUCTURE, keep) {
            None | Some((_, "td" | "th")) => true,
            Some(_) => self
                .innermost(&["table"], keep)
                .is_some_and(|(table, _)| !self.had_cells[table]),
        }
    }

    /// Closes `names[keep..]`. If `unsure`, Qt may keep them open; `credit`
    /// elements (and tables) are known to be closed in any case.
    fn close_to(&mut self, keep: usize, unsure: bool, credit: (usize, usize)) {
        if unsure {
            let tables = self.tables_in(self.names.len()) - self.tables_in(keep);
            self.missing = (self.missing + self.names.len() - keep).saturating_sub(credit.0);
            self.missing_tables = (self.missing_tables + tables).saturating_sub(credit.1);
        }
        while self.names.len() > keep {
            let name = self.names.pop().unwrap();
            let at = self.names.len();
            self.tables.pop();
            if let Some(v) = self.positions.get_mut(&name) {
                v.pop();
            }
            if self.had_cells.pop() == Some(true) && name == "table" {
                self.mark_cells();
            }
            if self.blocks.last() == Some(&at) {
                self.blocks.pop();
            }
            if self.maybe_blocks.last() == Some(&at) {
                self.maybe_blocks.pop();
            }
        }
    }

    fn push(&mut self, name: &str) {
        let at = self.names.len();
        let table = usize::from(name == "table");
        self.tables.push(self.tables_in(at) + table);
        self.positions.entry(name.to_string()).or_default().push(at);
        if is_block(name) {
            self.blocks.push(at);
        } else if is_maybe_block(name) {
            self.maybe_blocks.push(at);
        }
        if matches!(name, "td" | "th") {
            self.mark_cells();
        }
        self.had_cells.push(false);
        self.names.push(name.to_string());
    }

    /// Records a cell in the innermost open table.
    fn mark_cells(&mut self) {
        if let Some(table) = self.nearest("table") {
            self.had_cells[table] = true;
        }
    }

    /// Applies an opening tag unless that would nest too deep; returns
    /// whether it did.
    fn open(&mut self, word: Word) -> bool {
        self.opened = false;
        let name = match name(word) {
            Name::Unnamed => return true,
            Name::Known(name) => name,
            Name::Odd => String::new(),
        };
        let Some(step) = self.step(&name) else {
            return true;
        };
        let pushed_tables = step.push.iter().filter(|&&n| n == "table").count();
        if pushed_tables > 0 && !self.can_hold_table(step.keep) {
            return false;
        }
        let (mut missing, mut missing_tables) = (self.missing, self.missing_tables);
        if step.unsure {
            missing += self.names.len() - step.keep;
            missing_tables += self.tables_in(self.names.len()) - self.tables_in(step.keep);
        }
        missing += step.extra.0;
        missing_tables += step.extra.1;
        if step.keep + step.push.len() + missing > MAX_ELEMENT_DEPTH
            || self.tables_in(step.keep) + pushed_tables + missing_tables > MAX_TABLE_DEPTH
        {
            return false;
        }
        self.close_to(step.keep, step.unsure, (0, 0));
        self.missing += step.extra.0;
        self.missing_tables += step.extra.1;
        for name in &step.push {
            self.push(name);
        }
        self.opened = step.push.last() == Some(&name.as_str());
        true
    }

    /// Qt's QTextHtmlParser::resolveParent for a new element `name` (empty
    /// for an odd name), or `None` if it adds nothing that stays open.
    fn step<'a>(&self, name: &'a str) -> Option<Step<'a>> {
        let len = self.names.len();
        let exact = self.missing == 0;
        let step = |keep, push, unsure| Step {
            keep,
            push,
            unsure,
            extra: (0, 0),
        };
        Some(match name {
            // It could be any element, with up to two implicit ones (a table
            // and a row for a cell), and close any of the open ones.
            "" | "html" | "head" | "body" | "qt" => Step {
                keep: 0,
                push: Vec::new(),
                unsure: true,
                extra: (3, 1),
            },
            "img" | "br" | "hr" | "meta" => return None,
            "td" | "th" => match self.nearest("tr") {
                Some(row) => step(row + 1, vec![name], !exact),
                // No row: Qt adds a table and a row. If a row is missing
                // here, those stand for it and its table.
                None if exact => step(len, vec!["table", "tr", name], false),
                None => step(0, vec!["table", "tr", name], true),
            },
            "tr" => match self.nearest("table") {
                Some(_) => {
                    let parent = ["table", "thead", "tbody", "tfoot"]
                        .iter()
                        .filter_map(|n| self.nearest(n))
                        .max()
                        .unwrap();
                    step(parent + 1, vec!["tr"], !exact)
                }
                None if exact => step(len, vec!["table", "tr"], false),
                None => step(0, vec!["table", "tr"], true),
            },
            "thead" | "tbody" | "tfoot" | "caption" => step(
                self.nearest("table").map_or(0, |t| t + 1),
                vec![name],
                !exact,
            ),
            "dd" | "dt" => step(self.nearest("dl").map_or(0, |d| d + 1), vec![name], !exact),
            "p" => {
                // Closes the innermost block and what it holds if that block
                // is a paragraph.
                let block = self.blocks.last().copied();
                let passes_maybe = self.maybe_blocks.last().copied() > block;
                let in_p = block.filter(|&b| self.names[b] == "p");
                if exact {
                    match in_p {
                        Some(p) => step(p, vec!["p"], passes_maybe),
                        None => step(len, vec!["p"], false),
                    }
                } else {
                    let keep = match block {
                        Some(b) if in_p.is_some() => b,
                        Some(b) => b + 1,
                        None => 0,
                    };
                    step(keep, vec!["p"], true)
                }
            }
            "li" => {
                let keep = if self.names.last().is_some_and(|n| n == "li") {
                    len - 1
                } else {
                    len
                };
                step(keep, vec!["li"], !exact)
            }
            _ => step(len, vec![name], false),
        })
    }

    /// Closes the element just opened, for a self-closing tag.
    fn close_opened(&mut self) {
        if self.opened {
            self.close_to(self.names.len() - 1, false, (0, 0));
            self.opened = false;
        }
    }

    /// Qt's QTextHtmlParser::parseCloseTag: closes the innermost open element
    /// with that name and everything in it, or nothing if there is none.
    fn close(&mut self, word: Word) {
        let name = match name(word) {
            Name::Unnamed => return,
            Name::Known(name) => name,
            Name::Odd => return self.close_to(0, true, (0, 0)),
        };
        match self.nearest(&name) {
            // If elements are missing, Qt may close an inner one of the same
            // name instead; it does close one.
            Some(at) => {
                let table = usize::from(name == "table");
                self.close_to(at, self.missing > 0, (1, table));
            }
            None if self.missing > 0 => self.close_to(0, true, (0, 0)),
            None => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn limited(html: &str) -> String {
        limit_nesting(html).unwrap_or_else(|| html.to_string())
    }

    /// Opening tags that were escaped.
    fn escaped(html: &str) -> usize {
        limited(html).matches("&lt;").count() - html.matches("&lt;").count()
    }

    #[test]
    fn explicit_tables_are_limited() {
        let html = "<table><tr><td>".repeat(50);
        assert_eq!(escaped(&html), 50 - MAX_TABLE_DEPTH);
        // Closed tables do not count.
        let html = "<table><tr><td>x</td></tr></table>".repeat(50);
        assert_eq!(limit_nesting(&html), None);
        let html = format!(
            "<table><tr><td>{}</td></tr></table>",
            "<table><tr><td>x</table>".repeat(50)
        );
        assert_eq!(limit_nesting(&html), None);
    }

    #[test]
    fn implicit_tables_are_limited() {
        // Qt opens a table for a cell or row outside one; `</table>` may
        // close such a table instead of the one written.
        for unit in [
            "<td></tr>",
            "<th></tr>",
            "<table><td></table>",
            "<tr><th></tr><td></tr>",
        ] {
            let html = unit.repeat(50);
            let out = limited(&html);
            assert!(out.len() > html.len(), "{unit}");
            assert!(escaped(&html) >= 40, "{unit}: {out}");
        }
        // Rows and cells of one table do not nest.
        assert_eq!(limit_nesting(&"<tr><td>a<td>b".repeat(50)), None);
        assert_eq!(
            limit_nesting(&format!(
                "<table>{}</table>",
                "<tr><td>a</td><th>b</th></tr>".repeat(500)
            )),
            None
        );
    }

    #[test]
    fn hidden_closes_do_not_count() {
        // Closes that Qt does not read as such: in comments, in attribute
        // values, quoted, spelled with a character reference, or with a
        // space after the slash.
        for close in [
            "<!-- </table> -->",
            "< !-- </table> -->",
            "<span title=\"</table>\">",
            "<span title='</table>'>",
            "<span a = \"x\" title='</table>'>",
            "<span title='a\\'>x</table>'>",
            "<!DOCTYPE </table>>",
            "<span <table>",
            "</ table>",
        ] {
            let html = format!("<table><tr><td>{close}").repeat(10);
            assert_eq!(escaped(&html), 10 - MAX_TABLE_DEPTH, "{close}");
        }
        // Qt does read these as closes, but which element they close is not
        // worked out: they may only make the limit stricter.
        for close in ["</\"table\">", "</t&#97;ble>", "</T\u{c4}BLE>"] {
            let html = format!("<table><tr><td>{close}").repeat(10);
            assert!(escaped(&html) >= 10 - MAX_TABLE_DEPTH, "{close}");
        }
    }

    #[test]
    fn odd_opens_count() {
        for open in [
            "<TaBlE>",
            "<\ttable>",
            "<\"table\">",
            "<t&#97;ble>",
            "<\u{a0}table>",
            "<'x'>",
        ] {
            let html = format!("{open}<tr><td>").repeat(10);
            assert!(escaped(&html) >= 10 - MAX_TABLE_DEPTH, "{open}");
        }
    }

    #[test]
    fn deep_elements_are_limited() {
        for unit in ["<div>", "<b>", "<blockquote>", "<x-y>", "<input>"] {
            let html = unit.repeat(10_000);
            assert_eq!(escaped(&html), 10_000 - MAX_ELEMENT_DEPTH, "{unit}");
        }
        // A new item closes the previous one.
        let html = "<ul><li>".repeat(10_000);
        assert_eq!(escaped(&html), 10_000 - MAX_ELEMENT_DEPTH / 2);
        // Siblings and elements that close at once do not nest.
        for unit in [
            "<div></div>",
            "<br>",
            "<img src=x>",
            "<span/>",
            "<p>a",
            "<li>a",
            "<hr/>",
        ] {
            assert_eq!(limit_nesting(&unit.repeat(10_000)), None, "{unit}");
        }
    }

    #[test]
    fn ordinary_html_is_unchanged() {
        for html in [
            "<p align=\"center\"><img src=\"logo.png\" width=\"120\"><br><b>Name</b></p>",
            "<table><tr><td><table><tr><td><table border=1><tr><th>a > b</th></tr></table></td></tr></table></td></tr></table>",
            "<table><thead><tr><th>a<th>b<tbody><tr><td>1<td>2<tr><td>3<td>4</table>",
            "<TABLE width=\"100%\"><TR><TD><!-- <table> --><table class='x'><tr><td>a</td></tr></table></TD></TR></TABLE>",
            "<dl><dt>a<dd>b<dt>c<dd>d</dl><ul><li>a<li>b<ul><li>c</ul></ul>",
            "</table></table><table><tr><td>a",
            "<details><summary>More</summary><table><tr><td><p>a<p>b</td></tr></table></details>",
        ] {
            assert_eq!(limit_nesting(html), None, "{html}");
        }
    }

    #[test]
    fn malformed_html_is_scanned_in_linear_time() {
        let start = std::time::Instant::now();
        for unit in [
            "<",
            "<table",
            "<table ",
            "<a b='",
            "<a b=\"",
            "<!--",
            "<!",
            "</",
            "< ",
            "<a\t",
            "<'",
            "&#",
            "<td></tr>",
            "<div>",
            "</div>",
            "<p><b>",
            "<'\\'",
        ] {
            let _ = limit_nesting(&unit.repeat(200_000));
        }
        assert!(
            start.elapsed() < std::time::Duration::from_secs(10),
            "{:?}",
            start.elapsed()
        );
    }
}
