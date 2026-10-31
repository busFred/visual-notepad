//! Incremental parsing: after an edit, only a window of blocks around the
//! edited lines is parsed again and spliced into the previous block list.
//! The result is always exactly what `blocks()` returns for the new text;
//! whenever that cannot be guaranteed, the whole document is parsed.
//!
//! Why splicing is exact. CommonMark is parsed line by line: what a line
//! becomes depends only on the line and on the blocks still open before it.
//! Take a line L that starts a top-level block, with a blank line L-1 that
//! no block covers (block ranges are contiguous, so no block runs from
//! before L-1 into L). Every earlier block is closed while L is processed
//! and the blocks begun at L are children of the document, as in a fresh
//! parse. The blank line has closed paragraphs and block quotes, so nothing
//! can claim L lazily or as a setext underline or table header; L starts at
//! column 0 with no list marker or `>` (see `starts_cleanly`), so no list
//! item or indented code can continue onto it. Fenced code and HTML blocks
//! that would swallow L are ruled out by checking that a block starts at L.
//! Lines from L on are then parsed exactly as in a document that starts at
//! L, and the earlier blocks exactly as in a document that ends before L.
//!
//! The window is the unchanged lines [A, B) around the edit with such
//! boundaries: A is checked in the previous parse (the lines up to and
//! including A are unchanged, so the parse up to A is too); B is checked in
//! the previous parse and, for the new text, by parsing the window together
//! with line B and requiring that line to start a block (it also makes the
//! window's last blocks close the way they do in the whole document, not at
//! the end of the text).
//!
//! Some things reach across the whole document; they fall back to a full
//! parse: link reference and footnote definitions (any `]:` in the text),
//! `\r` line ends, front matter that the edit could create, change or
//! remove, and different options. Heading anchors count earlier headings
//! with the same slug: the window starts from the counts before it, and
//! blocks after it must not have a slug whose count the edit changed. Task
//! checkboxes carry their absolute line, which is moved with the block.

use std::collections::{HashMap, HashSet};
use std::sync::Arc;

use comrak::nodes::NodeValue;
use comrak::{Arena, parse_document};

use super::{Html, Meta, parse};
use crate::ffi::{Block, RenderOptions};
use crate::md;

/// Windows up to this size are always parsed incrementally; larger ones only
/// when they are under half the document (else a full parse costs about the
/// same).
const SMALL_WINDOW: usize = 64 * 1024;

/// A parsed document, kept so that the next version of its text can be
/// parsed incrementally.
pub struct Document {
    text: String,
    starts: Vec<usize>,
    options: [u8; 5],
    blocks: Vec<Block>,
    /// Shared with the documents made from this one.
    meta: Vec<Arc<Meta>>,
    front_matter_end: Option<usize>,
    /// The text has nothing that can make a block depend on far away lines
    /// (see `is_local`).
    local: bool,
    /// Made by splicing a window into the previous blocks.
    spliced: bool,
}

impl Document {
    pub fn parse(text: &str, options: &RenderOptions) -> Document {
        let parsed = parse(text, options);
        Document {
            text: text.to_owned(),
            starts: md::line_starts(text),
            options: options_key(options),
            blocks: parsed.blocks,
            meta: parsed.meta.into_iter().map(Arc::new).collect(),
            front_matter_end: parsed.front_matter_end,
            local: is_local(text),
            spliced: false,
        }
    }

    pub fn blocks(&self) -> &Vec<Block> {
        &self.blocks
    }

    /// Whether the blocks were spliced rather than parsed from scratch.
    pub fn spliced(&self) -> bool {
        self.spliced
    }
}

/// The blocks of `text`, a new version of `previous`'s text.
pub fn reparse(previous: &Document, text: &str, options: &RenderOptions) -> Document {
    splice(previous, text, options).unwrap_or_else(|| Document::parse(text, options))
}

fn options_key(o: &RenderOptions) -> [u8; 5] {
    [
        o.breaks as u8,
        o.linkify as u8,
        o.typographer as u8,
        o.math as u8,
        o.front_matter,
    ]
}

/// No link reference or footnote definitions (both need `]:`), and lines
/// that end with `\n` only.
fn is_local(text: &str) -> bool {
    !text.contains("]:") && !text.contains('\r')
}

/// Whether the text could start with front matter (comrak skips a BOM).
fn may_have_front_matter(text: &str) -> bool {
    text.starts_with('\u{feff}') || text.starts_with("---")
}

fn common_prefix(a: &[u8], b: &[u8]) -> usize {
    const CHUNK: usize = 4096;
    let n = a.len().min(b.len());
    let mut i = 0;
    while i + CHUNK <= n && a[i..i + CHUNK] == b[i..i + CHUNK] {
        i += CHUNK;
    }
    while i < n && a[i] == b[i] {
        i += 1;
    }
    i
}

/// Length of the common suffix, at most `max`.
fn common_suffix(a: &[u8], b: &[u8], max: usize) -> usize {
    const CHUNK: usize = 4096;
    let mut i = 0;
    while i + CHUNK <= max
        && a[a.len() - i - CHUNK..a.len() - i] == b[b.len() - i - CHUNK..b.len() - i]
    {
        i += CHUNK;
    }
    while i < max && a[a.len() - 1 - i] == b[b.len() - 1 - i] {
        i += 1;
    }
    i
}

/// The line starts of the new text, from those of the old: the same up to
/// line `lo0`, found again in the changed lines, moved after line `lo1`.
fn new_line_starts(
    old_starts: &[usize],
    old_len: usize,
    new: &[u8],
    lo0: usize,
    lo1: usize,
) -> Vec<usize> {
    let moved = |start: usize| (start + new.len()) - old_len;
    let tail = &old_starts[lo1 + 1..];
    // The line break before the first moved line is in the common suffix.
    let scan_end = tail.first().map_or(new.len(), |&start| moved(start) - 1);
    let mut starts = Vec::with_capacity(old_starts.len() + 16);
    starts.extend_from_slice(&old_starts[..=lo0]);
    let from = old_starts[lo0];
    for (i, &c) in new[from..scan_end].iter().enumerate() {
        if c == b'\n' {
            starts.push(from + i + 1);
        }
    }
    starts.extend(tail.iter().map(|&start| moved(start)));
    starts
}

/// Index of the line holding byte `offset`.
fn line_of(starts: &[usize], offset: usize) -> usize {
    starts.partition_point(|&s| s <= offset) - 1
}

fn line_bytes<'t>(text: &'t [u8], starts: &[usize], line: usize) -> &'t [u8] {
    let end = starts.get(line + 1).copied().unwrap_or(text.len());
    &text[starts[line]..end]
}

fn is_blank(text: &[u8], starts: &[usize], line: usize) -> bool {
    line_bytes(text, starts, line)
        .iter()
        .all(|&c| matches!(c, b' ' | b'\t' | b'\n'))
}

/// Whether a block that starts on this line, after a blank line, cannot
/// continue anything open before it: no indentation, list marker, block
/// quote marker or setext underline (see the module comment).
fn starts_cleanly(text: &[u8], starts: &[usize], line: usize) -> bool {
    match line_bytes(text, starts, line).first() {
        Some(&c) => {
            c.is_ascii_alphabetic()
                || c >= 0x80
                || matches!(
                    c,
                    b'#' | b'`'
                        | b'~'
                        | b'<'
                        | b'|'
                        | b'$'
                        | b'!'
                        | b'['
                        | b'\\'
                        | b'('
                        | b'"'
                        | b'\''
                        | b'&'
                )
        }
        None => false,
    }
}

/// Whether line `line` of the previous text can bound a window: a block
/// starts there, and the line before is blank and in no block.
fn is_boundary(prev: &Document, index: usize, line: usize) -> bool {
    let text = prev.text.as_bytes();
    line > 0
        && is_blank(text, &prev.starts, line - 1)
        && (index == 0 || prev.blocks[index - 1].last_line + 1 < line)
        && starts_cleanly(text, &prev.starts, line)
}

fn add_slugs<'m>(counts: &mut HashMap<&'m str, isize>, meta: &'m [Arc<Meta>], sign: isize) {
    for m in meta {
        for s in &m.slugs {
            *counts.entry(s.as_str()).or_default() += sign;
        }
    }
}

/// The block (and meta) moved down by `delta` lines; `None` if a task
/// checkbox's line cannot be moved.
fn shifted(block: &Block, meta: &Arc<Meta>, delta: isize) -> Option<(Block, Arc<Meta>)> {
    let mut b = block.clone();
    b.first_line = block.first_line.checked_add_signed(delta)?;
    b.last_line = block.last_line.checked_add_signed(delta)?;
    let mut m = Arc::clone(meta);
    if delta != 0 {
        let tasks = meta.tasks.as_ref()?;
        if !tasks.is_empty() {
            let html = block.html.as_bytes();
            let mut out = String::with_capacity(block.html.len() + tasks.len());
            let mut new_tasks = Vec::with_capacity(tasks.len());
            let mut last = 0;
            for &at in tasks {
                let digits = html[at..].iter().take_while(|c| c.is_ascii_digit()).count();
                let line: usize = block.html[at..at + digits].parse().ok()?;
                out.push_str(&block.html[last..at]);
                new_tasks.push(out.len());
                out.push_str(&line.checked_add_signed(delta)?.to_string());
                last = at + digits;
            }
            out.push_str(&block.html[last..]);
            b.html = out;
            m = Arc::new(Meta {
                slugs: meta.slugs.clone(),
                tasks: Some(new_tasks),
            });
        }
    }
    Some((b, m))
}

fn splice(prev: &Document, text: &str, options: &RenderOptions) -> Option<Document> {
    if options_key(options) != prev.options || !prev.local || !is_local(text) {
        return None;
    }
    let old = prev.text.as_bytes();
    let new = text.as_bytes();
    if old == new {
        return Some(Document {
            text: prev.text.clone(),
            starts: prev.starts.clone(),
            blocks: prev.blocks.clone(),
            meta: prev.meta.clone(),
            spliced: true,
            ..*prev
        });
    }
    let p = common_prefix(old, new);
    let s = common_suffix(old, new, old.len().min(new.len()) - p);

    // Front matter: comrak finds it from the start of the text. Without `\r`
    // it is the same as before when the edit begins after it and the byte
    // that follows it (which decides whether a blank line belongs to it).
    let front_matter = may_have_front_matter(&prev.text) || may_have_front_matter(text);
    if front_matter && !prev.front_matter_end.is_some_and(|end| p > end) {
        return None;
    }

    // Old lines [0, lo0) and new lines [0, lo0) are the same, as are old
    // lines after lo1 and new lines after lo1 + delta.
    let lo0 = line_of(&prev.starts, p);
    let lo1 = line_of(&prev.starts, old.len() - s);
    let starts = new_line_starts(&prev.starts, old.len(), new, lo0, lo1);
    let delta = starts.len() as isize - prev.starts.len() as isize;
    debug_assert_eq!(
        line_of(&starts, new.len() - s) as isize - lo1 as isize,
        delta
    );

    // Window start: line `a`, the first line of block `first` (or the start
    // of the text), before the first changed line.
    let blocks = &prev.blocks;
    let mut first = blocks.partition_point(|b| b.first_line < lo0);
    let a = loop {
        if first == 0 {
            if front_matter {
                return None;
            }
            break 0;
        }
        first -= 1;
        let line = blocks[first].first_line;
        if is_boundary(prev, first, line) {
            break line;
        }
    };
    // Window end: line `b`, the first line of block `end`, after the last
    // changed line and the blank line after it; or the end of the text.
    let mut end = blocks.partition_point(|b| b.first_line < lo1 + 2);
    while end < blocks.len() && !is_boundary(prev, end, blocks[end].first_line) {
        end += 1;
    }
    let b_new = (end < blocks.len())
        .then(|| blocks[end].first_line.checked_add_signed(delta))
        .flatten();
    if end < blocks.len() && b_new.is_none() {
        return None;
    }

    let window_start = starts[a];
    let window_end = b_new.map_or(new.len(), |b| starts[b]);
    let size = window_end - window_start;
    if size > SMALL_WINDOW && 2 * size > new.len() {
        return None;
    }

    // The window and line b, after a blank line unless the window starts the
    // text (so that it is not taken for front matter).
    let (source, offset) = {
        let tail = b_new.map_or(new.len(), |b| {
            starts.get(b + 1).copied().unwrap_or(new.len())
        });
        if a == 0 {
            (text[..tail].to_owned(), 0)
        } else {
            (format!("\n{}", &text[window_start..tail]), a - 1)
        }
    };
    let arena = Arena::new();
    let root = parse_document(&arena, &source, &md::options(options.math, options.linkify));
    let b_parsed = b_new.map(|b| b - offset);
    let mut nodes = Vec::new();
    let mut found_b = b_parsed.is_none();
    for node in root.children() {
        if matches!(
            node.data().value,
            NodeValue::FootnoteDefinition(_) | NodeValue::FrontMatter(_)
        ) {
            return None;
        }
        let (f, l) = md::line_range(node);
        if nodes.is_empty() && a > 0 && f != 1 {
            // Line a starts a block, as it did before.
            return None;
        }
        if let Some(b) = b_parsed {
            if f >= b {
                found_b = f == b;
                break;
            }
            if l + 1 >= b {
                return None;
            }
        }
        nodes.push((node, f, l));
    }
    if !found_b {
        return None;
    }

    // Heading slugs: the window starts from the counts before it.
    let no_footnotes = HashMap::new();
    let mut counts: HashMap<String, usize> = HashMap::new();
    let render = |counts: HashMap<String, usize>| {
        let mut renderer = Html::new(options, &no_footnotes, offset, counts);
        nodes
            .iter()
            .map(|&(node, f, l)| renderer.block(node, f, l))
            .collect::<Vec<_>>()
    };
    let mut window = render(HashMap::new());
    let in_window: HashSet<&str> = window
        .iter()
        .flat_map(|(_, m)| m.slugs.iter().map(String::as_str))
        .collect();
    if !in_window.is_empty() {
        for m in &prev.meta[..first] {
            for s in &m.slugs {
                if in_window.contains(s.as_str()) {
                    *counts.entry(s.clone()).or_default() += 1;
                }
            }
        }
    }
    drop(in_window);
    if !counts.is_empty() {
        window = render(counts);
    }
    // Blocks after the window keep their anchors if no slug count changed.
    let mut change: HashMap<&str, isize> = HashMap::new();
    add_slugs(&mut change, &prev.meta[first..end], -1);
    for (_, m) in &window {
        for s in &m.slugs {
            *change.entry(s.as_str()).or_default() += 1;
        }
    }
    change.retain(|_, n| *n != 0);
    if !change.is_empty()
        && prev.meta[end..]
            .iter()
            .any(|m| m.slugs.iter().any(|s| change.contains_key(s.as_str())))
    {
        return None;
    }

    let mut new_blocks = Vec::with_capacity(blocks.len() + window.len() + 1 - (end - first));
    let mut new_meta = Vec::with_capacity(new_blocks.capacity());
    new_blocks.extend_from_slice(&blocks[..first]);
    new_meta.extend_from_slice(&prev.meta[..first]);
    for (block, meta) in window {
        new_blocks.push(block);
        new_meta.push(Arc::new(meta));
    }
    for (block, meta) in blocks[end..].iter().zip(&prev.meta[end..]) {
        let (block, meta) = shifted(block, meta, delta)?;
        new_blocks.push(block);
        new_meta.push(meta);
    }
    Some(Document {
        text: text.to_owned(),
        starts,
        options: prev.options,
        blocks: new_blocks,
        meta: new_meta,
        front_matter_end: if front_matter {
            prev.front_matter_end
        } else {
            None
        },
        local: true,
        spliced: true,
    })
}

#[cfg(test)]
mod tests;
