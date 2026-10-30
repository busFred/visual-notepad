//! Link destinations: locating them in the source (for broken-link checks) and
//! rewriting relative ones when text moves between folders.

use std::borrow::Cow;

use comrak::nodes::NodeValue;
use comrak::{Arena, parse_document};

use crate::ffi::LinkTarget;
use crate::md;

/// A destination in the source: byte range of the text as written.
pub(crate) struct Dest {
    pub start: usize,
    pub end: usize,
}

fn skip_space(src: &[u8], mut i: usize) -> usize {
    while i < src.len() && matches!(src[i], b' ' | b'\t' | b'\n' | b'\r') {
        i += 1;
    }
    i
}

/// Index of the first unescaped `]` at or after `from`, skipping nested
/// brackets.
fn label_end(src: &[u8], from: usize) -> Option<usize> {
    let mut depth = 0usize;
    let mut i = from;
    while i < src.len() {
        match src[i] {
            b'\\' => i += 1,
            b'[' => depth += 1,
            b']' => {
                if depth == 0 {
                    return Some(i);
                }
                depth -= 1;
            }
            _ => {}
        }
        i += 1;
    }
    None
}

/// The destination starting at `from` (after whitespace), either `<...>` or
/// bare with balanced parentheses.
fn destination(src: &[u8], from: usize) -> Option<Dest> {
    let start = skip_space(src, from);
    if start >= src.len() {
        return None;
    }
    if src[start] == b'<' {
        let mut i = start + 1;
        while i < src.len() {
            match src[i] {
                b'\\' => i += 1,
                b'>' => {
                    return Some(Dest {
                        start: start + 1,
                        end: i,
                    });
                }
                b'\n' | b'<' => return None,
                _ => {}
            }
            i += 1;
        }
        return None;
    }
    let mut depth = 0usize;
    let mut i = start;
    while i < src.len() {
        match src[i] {
            b'\\' => i += 1,
            b'(' => depth += 1,
            b')' => {
                if depth == 0 {
                    break;
                }
                depth -= 1;
            }
            b' ' | b'\t' | b'\n' | b'\r' => break,
            _ => {}
        }
        i += 1;
    }
    let end = i.min(src.len());
    (end > start).then_some(Dest { start, end })
}

/// Destination of an inline link `[..](dest)` or image `![..](dest)` whose
/// source begins at `start`.
fn inline_destination(src: &[u8], start: usize, image: bool, label_from: usize) -> Option<Dest> {
    let open = if image { start + 2 } else { start + 1 };
    if src.get(start) != Some(&if image { b'!' } else { b'[' }) {
        return None;
    }
    let close = label_end(src, label_from.max(open))?;
    if src.get(close + 1) != Some(&b'(') {
        return None;
    }
    destination(src, close + 2)
}

fn is_line_end(src: &[u8], i: usize) -> bool {
    i >= src.len() || matches!(src[i], b'\n' | b'\r')
}

fn skip_blanks(src: &[u8], mut i: usize) -> usize {
    while i < src.len() && matches!(src[i], b' ' | b'\t') {
        i += 1;
    }
    i
}

/// Start of the line after the one containing `i`.
fn next_line(src: &[u8], i: usize) -> usize {
    match src.get(i) {
        Some(b'\r') if src.get(i + 1) == Some(&b'\n') => i + 2,
        Some(_) => i + 1,
        None => i,
    }
}

/// A link title starting at `from` that is followed only by blanks on its
/// last line. Returns the end of that line.
fn title_end(src: &[u8], from: usize) -> Option<usize> {
    let closer = match src.get(from)? {
        b'"' => b'"',
        b'\'' => b'\'',
        b'(' => b')',
        _ => return None,
    };
    let mut i = from + 1;
    loop {
        match *src.get(i)? {
            b'\\' => i += 1,
            c if c == closer => break,
            b'(' if closer == b')' => return None,
            _ => {}
        }
        i += 1;
    }
    let end = skip_blanks(src, i + 1);
    is_line_end(src, end).then_some(end)
}

/// A link reference definition `[label]: dest "title"` starting at `start`
/// (the whole of `src` is the available text). Returns the destination and
/// the offset where the definition ends.
fn definition_at(src: &[u8], start: usize) -> Option<(Dest, usize)> {
    let mut i = start;
    while i < start + 3 && src.get(i) == Some(&b' ') {
        i += 1;
    }
    if src.get(i) != Some(&b'[') || src.get(i + 1) == Some(&b'^') {
        return None;
    }
    i += 1;
    let label_start = i;
    loop {
        match *src.get(i)? {
            b'\\' => i += 1,
            b'[' => return None,
            b']' => break,
            _ => {}
        }
        i += 1;
    }
    if src[label_start..i].iter().all(u8::is_ascii_whitespace) || src.get(i + 1) != Some(&b':') {
        return None;
    }
    i = skip_blanks(src, i + 2);
    if is_line_end(src, i) {
        i = skip_blanks(src, next_line(src, i));
    }
    let dest = destination(src, i)?;
    let after = if src[i] == b'<' {
        dest.end + 1
    } else {
        dest.end
    };
    let rest = skip_blanks(src, after);
    if !is_line_end(src, rest) {
        if rest == after {
            return None;
        }
        return title_end(src, rest).map(|end| (dest, end));
    }
    let next = skip_blanks(src, next_line(src, rest));
    let end = title_end(src, next).unwrap_or(rest);
    Some((dest, end))
}

/// A link reference definition found in the source.
pub(crate) struct Definition {
    pub first_line: usize,
    pub last_line: usize,
    pub dest: Dest,
}

/// Link reference definitions that sit on lines no top-level node covers
/// (comrak resolves definitions and drops them from the tree).
pub(crate) fn definitions(src: &str, starts: &[usize], covered: &[bool]) -> Vec<Definition> {
    let bytes = src.as_bytes();
    let line_of = |offset: usize| starts.partition_point(|s| *s <= offset) - 1;
    let blank = |line: usize| {
        let end = starts.get(line + 1).copied().unwrap_or(bytes.len());
        bytes[starts[line]..end].iter().all(u8::is_ascii_whitespace)
    };
    let mut out = Vec::new();
    let mut line = 0;
    while line < starts.len() {
        if covered[line] || blank(line) {
            line += 1;
            continue;
        }
        // A run of uncovered, non-blank lines.
        let mut run_end = line;
        while run_end + 1 < starts.len() && !covered[run_end + 1] && !blank(run_end + 1) {
            run_end += 1;
        }
        let bound = starts.get(run_end + 1).copied().unwrap_or(bytes.len());
        let text = &bytes[..bound];
        let mut l = line;
        while l <= run_end {
            match definition_at(text, starts[l]) {
                Some((dest, end)) => {
                    let last_line = line_of(end).min(run_end);
                    out.push(Definition {
                        first_line: l,
                        last_line,
                        dest,
                    });
                    l = last_line + 1;
                }
                None => l += 1,
            }
        }
        line = run_end + 1;
    }
    out
}

fn destinations(markdown: &str) -> Vec<Dest> {
    let arena = Arena::new();
    let root = parse_document(&arena, markdown, &md::options(true, false));
    let starts = md::line_starts(markdown);
    let bytes = markdown.as_bytes();
    let mut out = Vec::new();
    let mut covered = vec![false; starts.len()];
    for node in root.children() {
        let (first, last) = md::line_range(node);
        for flag in covered.iter_mut().take(last + 1).skip(first) {
            *flag = true;
        }
    }
    for node in root.descendants() {
        let data = node.data();
        let image = match data.value {
            NodeValue::Link(_) => false,
            NodeValue::Image(_) => true,
            _ => continue,
        };
        let Some(start) = md::byte_offset(&starts, data.sourcepos.start) else {
            continue;
        };
        // Children end inside the label, past any `]` they contain.
        let from = if image {
            0
        } else {
            node.last_child()
                .and_then(|c| md::byte_offset(&starts, c.data().sourcepos.end))
                .map_or(start + 1, |end| end + 1)
        };
        out.extend(inline_destination(bytes, start, image, from));
    }
    out.extend(
        definitions(markdown, &starts, &covered)
            .into_iter()
            .map(|d| d.dest),
    );
    out.retain(|d| {
        d.start < d.end && markdown.is_char_boundary(d.start) && markdown.is_char_boundary(d.end)
    });
    out.sort_by_key(|d| d.start);
    out
}

pub fn targets(markdown: &str) -> Vec<LinkTarget> {
    // Offsets are converted in one forward pass.
    let mut result = Vec::new();
    let mut byte = 0usize;
    let mut utf16 = 0usize;
    for dest in destinations(markdown) {
        if dest.start < byte {
            continue;
        }
        let start = utf16 + markdown[byte..dest.start].encode_utf16().count();
        let end = start + markdown[dest.start..dest.end].encode_utf16().count();
        byte = dest.end;
        utf16 = end;
        result.push(LinkTarget {
            start,
            end,
            dest: markdown[dest.start..dest.end].to_string(),
        });
    }
    result
}

fn has_scheme(dest: &str) -> bool {
    let Some(colon) = dest.find(':') else {
        return false;
    };
    let scheme = &dest[..colon];
    let mut chars = scheme.chars();
    chars.next().is_some_and(|c| c.is_ascii_alphabetic())
        && chars.all(|c| c.is_ascii_alphanumeric() || matches!(c, '+' | '.' | '-'))
}

fn percent_decode(text: &str) -> String {
    let bytes = text.as_bytes();
    let hex = |b: u8| (b as char).to_digit(16);
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%'
            && i + 2 < bytes.len()
            && let (Some(hi), Some(lo)) = (hex(bytes[i + 1]), hex(bytes[i + 2]))
        {
            out.push((hi * 16 + lo) as u8);
            i += 3;
            continue;
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn percent_encode(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for c in text.chars() {
        match c {
            ' ' => out.push_str("%20"),
            '%' => out.push_str("%25"),
            '(' => out.push_str("%28"),
            ')' => out.push_str("%29"),
            '<' => out.push_str("%3C"),
            '>' => out.push_str("%3E"),
            _ => out.push(c),
        }
    }
    out
}

/// Directory components; a leading root is kept as "/" and a drive as "C:".
fn components(dir: &str) -> Vec<String> {
    let dir = dir.replace('\\', "/");
    let mut parts = Vec::new();
    if dir.starts_with('/') {
        parts.push("/".to_string());
    }
    parts.extend(
        dir.split('/')
            .filter(|p| !p.is_empty() && *p != ".")
            .map(str::to_string),
    );
    parts
}

fn same_component(a: &str, b: &str) -> bool {
    if a.ends_with(':') {
        a.eq_ignore_ascii_case(b)
    } else {
        a == b
    }
}

/// The same file as seen from `to`, or None if no relative path exists.
/// `from` holds each component of the source folder with its percent-encoded
/// form, computed once per `rebase`.
fn rebase_path(path: &str, from: &[(String, String)], to: &[String]) -> Option<String> {
    // Borrowed where possible: only decoded segments of `path` allocate.
    let mut abs: Vec<(Cow<str>, Cow<str>)> = from
        .iter()
        .map(|(d, r)| (Cow::Borrowed(d.as_str()), Cow::Borrowed(r.as_str())))
        .collect();
    for seg in path.split('/') {
        match seg {
            "" | "." => {}
            ".." => {
                if abs.len() <= 1 {
                    return None;
                }
                abs.pop();
            }
            _ => abs.push((Cow::Owned(percent_decode(seg)), Cow::Borrowed(seg))),
        }
    }
    let common = abs
        .iter()
        .zip(to)
        .take_while(|((dec, _), t)| same_component(dec, t))
        .count();
    if common == 0 {
        return None;
    }
    let mut parts: Vec<&str> = vec![".."; to.len() - common];
    parts.extend(abs[common..].iter().map(|(_, raw)| raw.as_ref()));
    let mut result = parts.join("/");
    if result.is_empty() {
        result.push('.');
    }
    if path.ends_with('/') && !result.ends_with('/') {
        result.push('/');
    }
    Some(result)
}

pub fn rebase(text: &str, from_dir: &str, to_dir: &str) -> String {
    let from = components(from_dir);
    let to = components(to_dir);
    if from.is_empty()
        || to.is_empty()
        || from.len() == to.len() && from.iter().zip(&to).all(|(a, b)| same_component(a, b))
    {
        return text.to_string();
    }
    let encoded: Vec<(String, String)> = from
        .iter()
        .map(|c| (c.clone(), percent_encode(c)))
        .collect();
    let mut out = String::with_capacity(text.len());
    let mut last = 0usize;
    for dest in destinations(text) {
        let raw = &text[dest.start..dest.end];
        if dest.start < last
            || raw.starts_with(['#', '/', '\\'])
            || has_scheme(raw)
            || raw.contains(['<', '>'])
        {
            continue;
        }
        let split = raw.find(['#', '?']).unwrap_or(raw.len());
        let (path, suffix) = raw.split_at(split);
        if path.is_empty() {
            continue;
        }
        if let Some(new) = rebase_path(path, &encoded, &to) {
            out.push_str(&text[last..dest.start]);
            out.push_str(&new);
            out.push_str(suffix);
            last = dest.end;
        }
    }
    out.push_str(&text[last..]);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn targets_cover_links_images_and_definitions() {
        let doc = "[a](one.md) ![b](img/p.png \"t\") [c][ref] <https://x.y>\n\n[ref]: <two words.md#h> \"title\"\n";
        let got: Vec<String> = targets(doc).into_iter().map(|t| t.dest).collect();
        assert_eq!(got, vec!["one.md", "img/p.png", "two words.md#h"]);
    }

    #[test]
    fn target_offsets_are_utf16() {
        // The emoji is 4 bytes but 2 UTF-16 units; the accent is 2 bytes, 1 unit.
        let doc = "😀 é [a](x.md)";
        let t = &targets(doc)[0];
        let units: Vec<u16> = doc.encode_utf16().collect();
        assert_eq!(String::from_utf16(&units[t.start..t.end]).unwrap(), "x.md");
    }

    #[test]
    fn targets_ignore_code_and_bare_urls() {
        let doc = "`[a](x.md)`\n\n```\n[b](y.md)\n```\n\nhttps://example.com and www.example.com\n";
        assert!(targets(doc).is_empty());
    }

    #[test]
    fn label_with_brackets_and_markup() {
        let doc = "[*a* [b]](x.md) ![alt [x]](y.png)";
        let got: Vec<String> = targets(doc).into_iter().map(|t| t.dest).collect();
        assert_eq!(got, vec!["x.md", "y.png"]);
    }

    #[test]
    fn rebases_inline_image_and_definition() {
        let doc = "[t](a/b.md#sec) ![i](../img/p.png)\n\n[ref]: c.md \"T\"\n";
        let out = rebase(doc, "/docs/notes", "/docs/other/deep");
        assert_eq!(
            out,
            "[t](../../notes/a/b.md#sec) ![i](../../img/p.png)\n\n[ref]: ../../notes/c.md \"T\"\n"
        );
    }

    #[test]
    fn rebase_into_subfolder_and_parent() {
        assert_eq!(rebase("[a](x.md)", "/d", "/d/sub"), "[a](../x.md)");
        assert_eq!(rebase("[a](../x.md)", "/d/sub", "/d"), "[a](x.md)");
        assert_eq!(rebase("[a](sub/x.md)", "/d", "/d/sub"), "[a](x.md)");
    }

    #[test]
    fn rebase_leaves_absolute_anchor_and_scheme_alone() {
        let doc = "[a](https://e.com/x) [b](#top) [c](/abs/x.md) [d](mailto:a@b.c) [e](C:/x.md) <https://e.com>";
        assert_eq!(rebase(doc, "/a", "/b"), doc);
    }

    #[test]
    fn rebase_encodes_spaces_and_keeps_fragment_and_query() {
        assert_eq!(
            rebase("[a](x%20y.md#h) ![b](<z w.png>)", "/my dir", "/other"),
            "[a](../my%20dir/x%20y.md#h) ![b](<../my%20dir/z w.png>)"
        );
        assert_eq!(
            rebase("[a](x.md?v=1#h)", "/a", "/b"),
            "[a](../a/x.md?v=1#h)"
        );
    }

    #[test]
    fn rebase_skips_code_and_same_folder() {
        let doc = "`[a](x.md)`\n\n```\n[b](y.md)\n```\n";
        assert_eq!(rebase(doc, "/a", "/b"), doc);
        assert_eq!(rebase("[a](x.md)", "/a", "/a"), "[a](x.md)");
        assert_eq!(rebase("[a](x.md)", "/a/", "/a"), "[a](x.md)");
    }

    #[test]
    fn rebase_handles_windows_paths_and_other_drives() {
        assert_eq!(
            rebase("[a](x.md)", "C:\\docs\\a", "C:\\docs\\b"),
            "[a](../a/x.md)"
        );
        assert_eq!(rebase("[a](x.md)", "C:\\docs", "D:\\docs"), "[a](x.md)");
    }

    #[test]
    fn rebase_keeps_multibyte_text_intact() {
        assert_eq!(
            rebase("é 😀 [a](x.md) ü", "/a", "/b"),
            "é 😀 [a](../a/x.md) ü"
        );
    }

    #[test]
    fn multi_line_definitions_are_located() {
        let doc = "é\n\n[a]:\n  <x y.md>\n  \"t\"\n[b]: z.md\n";
        let units: Vec<u16> = doc.encode_utf16().collect();
        let got: Vec<String> = targets(doc)
            .iter()
            .map(|t| String::from_utf16(&units[t.start..t.end]).unwrap())
            .collect();
        assert_eq!(got, vec!["x y.md", "z.md"]);
    }
}
